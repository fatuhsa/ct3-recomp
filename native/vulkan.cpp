#include "native/vulkan.h"

#include "host/log.h"
#include "host/memory.h"
#include "native/stats.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace ct2::vk {

namespace {

uint64_t Fnv1a64(const void* data, size_t len, uint64_t seed = 14695981039346656037ull) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

uint64_t AlignUp(uint64_t v, uint64_t align) {
    return (v + align - 1) & ~(align - 1);
}

#if CT2_HAS_VULKAN_HEADERS
bool HasExtension(const std::vector<VkExtensionProperties>& exts, const char* name) {
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}
#endif

} // namespace

VulkanRenderer::~VulkanRenderer() {
    Shutdown();
}

uint64_t VulkanRenderer::MemSig(uint32_t guest_va, uint32_t total_bytes) {
    diag::PerfScope scope(diag::ScopeId::Sig);
    const auto* p = static_cast<const uint8_t*>(kernel::HostPtrConst(guest_va));
    if (!p || total_bytes == 0) return 0;

    uint64_t sig = Fnv1a64(&total_bytes, sizeof(total_bytes));
    uint64_t hashed_bytes = 0;
    if (total_bytes <= kSigWholeMax) {
        sig = Fnv1a64(p, total_bytes, sig);
        hashed_bytes = total_bytes;
    } else {
        for (uint32_t i = 0; i < kSigSamples; ++i) {
            const uint64_t offset =
                static_cast<uint64_t>(total_bytes - kSigSampleBytes) * i / (kSigSamples - 1);
            sig = Fnv1a64(p + offset, kSigSampleBytes, sig);
        }
        hashed_bytes = static_cast<uint64_t>(kSigSamples) * kSigSampleBytes;
    }

    diag::FrameCounters fc{};
    fc.sigs = 1;
    fc.sig_bytes = hashed_bytes;
    diag::AddCounters(fc);
    return sig;
}

void VulkanRenderer::SelectMobileQuirks(uint32_t vendor_id, uint32_t api_version,
                                        bool has_dyn_render_ext, bool has_ext_dyn_state) {
    caps_.vendor_id = vendor_id;
    caps_.api_version = api_version;
    caps_.is_adreno = (vendor_id == kQualcommVendorId);
    caps_.is_mali = (vendor_id == kArmVendorId);

#if CT2_HAS_VULKAN_HEADERS
    const bool is_vk12 = api_version >= VK_API_VERSION_1_2;
    const bool is_vk13 = api_version >= VK_API_VERSION_1_3;
#else
    const bool is_vk12 = api_version >= ((1u << 22) | (2u << 12));
    const bool is_vk13 = api_version >= ((1u << 22) | (3u << 12));
#endif

    // Adreno (0x5143): bypass vkCmdBeginRendering in favor of cached VkRenderPass + deferred LOAD_OP_CLEAR.
    bool dynamic = is_vk13 || has_dyn_render_ext;
    if (dynamic && caps_.is_adreno) {
        dynamic = false;
        log::Info("CT2Vk", "Adreno (0x5143) detected: disabling dynamic rendering -> cached VkRenderPass");
    }
    caps_.use_dynamic_rendering = dynamic;

    // Mali-G76 VK 1.1 VK_KHR_timeline_semaphore causes VK_ERROR_DEVICE_LOST:
    // ONLY enable timeline semaphores on Vulkan 1.2+ core; fall back to VkFence pool on VK 1.1.
    caps_.use_timeline_semaphores = is_vk12;
    if (!caps_.use_timeline_semaphores) {
        log::Info("CT2Vk", "Vulkan < 1.2: timeline semaphores disabled -> VkFence pool fallback");
    }

    // Dynamic primitive topology & vertex strides cut pipeline permutations dramatically.
    caps_.use_extended_dynamic_state = is_vk13 || has_ext_dyn_state;

    log::Info("CT2Vk",
              "GPU vendor=0x%04x name='%s' dynRender=%d timeline=%d extDynState=%d bc=%d",
              caps_.vendor_id, caps_.device_name.c_str(),
              caps_.use_dynamic_rendering ? 1 : 0,
              caps_.use_timeline_semaphores ? 1 : 0,
              caps_.use_extended_dynamic_state ? 1 : 0,
              caps_.supports_bc ? 1 : 0);
}

bool VulkanRenderer::InitVulkanDevice() {
#if CT2_HAS_VULKAN_HEADERS
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "ct2-recomp";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "ct2-mobile-vk";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_2;

    std::vector<const char*> inst_exts;
#ifdef VK_KHR_SURFACE_EXTENSION_NAME
    inst_exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#endif
#if defined(__ANDROID__) && defined(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)
    inst_exts.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#endif

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app_info;
    ici.enabledExtensionCount = static_cast<uint32_t>(inst_exts.size());
    ici.ppEnabledExtensionNames = inst_exts.data();

    if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) {
        log::Warn("CT2Vk", "vkCreateInstance failed; running headless fallback mode");
        return false;
    }

    uint32_t gpu_count = 0;
    vkEnumeratePhysicalDevices(instance_, &gpu_count, nullptr);
    if (gpu_count == 0) {
        log::Warn("CT2Vk", "No Vulkan physical devices enumerated; headless fallback");
        return false;
    }

    std::vector<VkPhysicalDevice> gpus(gpu_count);
    vkEnumeratePhysicalDevices(instance_, &gpu_count, gpus.data());
    phys_dev_ = gpus[0];

    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceFeatures feats{};
    vkGetPhysicalDeviceProperties(phys_dev_, &props);
    vkGetPhysicalDeviceFeatures(phys_dev_, &feats);
    caps_.device_name = props.deviceName;
    caps_.supports_bc = (feats.textureCompressionBC == VK_TRUE);
    caps_.timestamp_period_ns = props.limits.timestampPeriod;

    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(phys_dev_, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> exts(ext_count);
    vkEnumerateDeviceExtensionProperties(phys_dev_, nullptr, &ext_count, exts.data());

    const bool has_dyn_render =
#ifdef VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME
        HasExtension(exts, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
#else
        false;
#endif
    const bool has_ext_dyn_state =
#ifdef VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME
        HasExtension(exts, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
#else
        false;
#endif

    SelectMobileQuirks(props.vendorID, props.apiVersion, has_dyn_render, has_ext_dyn_state);

    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys_dev_, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qf_count);
    vkGetPhysicalDeviceQueueFamilyProperties(phys_dev_, &qf_count, qfs.data());
    queue_family_ = 0;
    for (uint32_t i = 0; i < qf_count; ++i) {
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queue_family_ = i;
            break;
        }
    }

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    // Keep VkDeviceCreateInfo.pNext strictly minimal (VK 1.1 drivers crash on unused feature structs).
    std::vector<const char*> enabled_exts;
#ifdef VK_KHR_SWAPCHAIN_EXTENSION_NAME
    if (HasExtension(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        enabled_exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
#endif
    void* pnext_head = nullptr;

    VkPhysicalDeviceVulkan12Features vk12_feats{};
    if (caps_.use_timeline_semaphores) {
        vk12_feats.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        vk12_feats.timelineSemaphore = VK_TRUE;
        vk12_feats.pNext = pnext_head;
        pnext_head = &vk12_feats;
    }

#ifdef VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT dyn_state_feats{};
    if (caps_.use_extended_dynamic_state && props.apiVersion < VK_API_VERSION_1_3 &&
        has_ext_dyn_state) {
        enabled_exts.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
        dyn_state_feats.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT;
        dyn_state_feats.extendedDynamicState = VK_TRUE;
        dyn_state_feats.pNext = pnext_head;
        pnext_head = &dyn_state_feats;
    }
#endif

    VkPhysicalDeviceFeatures enabled_feats{};
    enabled_feats.textureCompressionBC = caps_.supports_bc ? VK_TRUE : VK_FALSE;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = pnext_head;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(enabled_exts.size());
    dci.ppEnabledExtensionNames = enabled_exts.data();
    dci.pEnabledFeatures = &enabled_feats;

    if (vkCreateDevice(phys_dev_, &dci, nullptr, &device_) != VK_SUCCESS) {
        log::Error("CT2Vk", "vkCreateDevice failed");
        return false;
    }
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    if (caps_.use_timeline_semaphores) {
        pfn_vkWaitSemaphores_ = reinterpret_cast<PFN_vkWaitSemaphores>(
            vkGetDeviceProcAddr(device_, "vkWaitSemaphores"));
        if (!pfn_vkWaitSemaphores_) {
            log::Warn("CT2Vk", "vkWaitSemaphores unavailable at runtime; falling back to VkFence pool");
            caps_.use_timeline_semaphores = false;
        }
    }

    if (caps_.use_timeline_semaphores) {
        VkSemaphoreTypeCreateInfo stci{};
        stci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        stci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        stci.initialValue = 0;
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        sci.pNext = &stci;
        vkCreateSemaphore(device_, &sci, nullptr, &timeline_sem_);
    }

    for (auto& f : frames_) {
        VkCommandPoolCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci.queueFamilyIndex = queue_family_;
        cpci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        vkCreateCommandPool(device_, &cpci, nullptr, &f.pool);

        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = f.pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        vkAllocateCommandBuffers(device_, &cbai, &f.cmd);

        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(device_, &fci, nullptr, &f.fence);

        VkSemaphoreCreateInfo bsci{};
        bsci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(device_, &bsci, nullptr, &f.img_available);
        vkCreateSemaphore(device_, &bsci, nullptr, &f.render_finished);

        VkQueryPoolCreateInfo qpci{};
        qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpci.queryCount = 64; // Frame start/end + per-pass timestamps
        vkCreateQueryPool(device_, &qpci, nullptr, &f.query_pool);
    }

    LoadPipelineCacheFromDisk();
    return true;
#else
    caps_.device_name = "HeadlessStub";
    SelectMobileQuirks(/*vendor_id=*/0, /*api_version=*/(1u << 22) | (2u << 12), false, false);
    return false;
#endif
}

bool VulkanRenderer::Initialize(const RendererConfig& cfg) {
    cfg_ = cfg;
    shader_cache_ = std::make_unique<ShaderCache>(cfg_.cache_dir);
    InitVulkanDevice();

    if (caps_.use_timeline_semaphores) {
        StartPoolResetWorker();
    }
    StartTexPackWorker();
    if (cfg_.enable_preload) {
        StartPreloadWorker();
    }

    initialized_ = true;
    return true;
}

void VulkanRenderer::Shutdown() {
    if (!initialized_) return;
    initialized_ = false;

#if CT2_HAS_VULKAN_HEADERS
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
#endif
    if (preload_worker_.joinable()) preload_worker_.join();
    StopTexPackWorker();
    StopPoolResetWorker();
    SavePipelineCacheAtomic();

#if CT2_HAS_VULKAN_HEADERS
    {
        std::lock_guard<std::mutex> slock(swap_mu_);
        DestroySwapchainLocked();
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        for (auto& f : frames_) {
            if (f.query_pool) vkDestroyQueryPool(device_, f.query_pool, nullptr);
            if (f.render_finished) vkDestroySemaphore(device_, f.render_finished, nullptr);
            if (f.img_available) vkDestroySemaphore(device_, f.img_available, nullptr);
            if (f.fence) vkDestroyFence(device_, f.fence, nullptr);
            if (f.pool) vkDestroyCommandPool(device_, f.pool, nullptr);
            f = FrameSlot{};
        }
        if (timeline_sem_) vkDestroySemaphore(device_, timeline_sem_, nullptr);
        if (pipeline_cache_) vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
#endif
    textures_.clear();
    total_texture_bytes_ = 0;
}

void VulkanRenderer::DestroySwapchainLocked() {
#if CT2_HAS_VULKAN_HEADERS
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        for (auto fb : swap_framebuffers_) {
            if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
        }
        swap_framebuffers_.clear();
        for (auto iv : swap_views_) {
            if (iv) vkDestroyImageView(device_, iv, nullptr);
        }
        swap_views_.clear();
        swap_images_.clear();
        if (present_render_pass_) {
            vkDestroyRenderPass(device_, present_render_pass_, nullptr);
            present_render_pass_ = VK_NULL_HANDLE;
        }
        if (swapchain_) {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }
    }
    if (instance_ != VK_NULL_HANDLE && surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
#endif
    swapchain_ready_ = false;
    swap_width_ = 0;
    swap_height_ = 0;
    swap_image_count_ = 0;
}

bool VulkanRenderer::CreateSwapchainLocked(void* native_window, uint32_t width, uint32_t height) {
#if CT2_HAS_VULKAN_HEADERS && defined(__ANDROID__)
    DestroySwapchainLocked();
    if (!native_window || instance_ == VK_NULL_HANDLE || device_ == VK_NULL_HANDLE) {
        return false;
    }

    VkAndroidSurfaceCreateInfoKHR asci{};
    asci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    asci.window = static_cast<ANativeWindow*>(native_window);
    if (vkCreateAndroidSurfaceKHR(instance_, &asci, nullptr, &surface_) != VK_SUCCESS) {
        log::Error("CT2Vk", "vkCreateAndroidSurfaceKHR failed");
        return false;
    }

    VkSurfaceCapabilitiesKHR surf_caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_dev_, surface_, &surf_caps) != VK_SUCCESS) {
        DestroySwapchainLocked();
        return false;
    }

    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_dev_, surface_, &fmt_count, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmt_count);
    if (fmt_count > 0) {
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys_dev_, surface_, &fmt_count, fmts.data());
    }
    VkSurfaceFormatKHR chosen_fmt{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (const auto& sf : fmts) {
        if (sf.format == VK_FORMAT_R8G8B8A8_UNORM || sf.format == VK_FORMAT_B8G8R8A8_UNORM) {
            chosen_fmt = sf;
            break;
        }
    }
    swap_format_ = chosen_fmt.format;

    VkExtent2D extent{};
    if (surf_caps.currentExtent.width != UINT32_MAX && surf_caps.currentExtent.width > 0) {
        extent = surf_caps.currentExtent;
    } else {
        extent.width = std::clamp(width ? width : 1280u,
                                  surf_caps.minImageExtent.width, surf_caps.maxImageExtent.width);
        extent.height = std::clamp(height ? height : 720u,
                                   surf_caps.minImageExtent.height, surf_caps.maxImageExtent.height);
    }

    uint32_t img_count = std::max(surf_caps.minImageCount + 1, 3u);
    if (surf_caps.maxImageCount > 0 && img_count > surf_caps.maxImageCount) {
        img_count = surf_caps.maxImageCount;
    }

    VkCompositeAlphaFlagBitsKHR comp_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(surf_caps.supportedCompositeAlpha & comp_alpha)) {
        comp_alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    }

    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = surface_;
    sci.minImageCount = img_count;
    sci.imageFormat = chosen_fmt.format;
    sci.imageColorSpace = chosen_fmt.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = surf_caps.currentTransform;
    sci.compositeAlpha = comp_alpha;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain_) != VK_SUCCESS) {
        log::Error("CT2Vk", "vkCreateSwapchainKHR failed");
        DestroySwapchainLocked();
        return false;
    }

    uint32_t actual_imgs = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &actual_imgs, nullptr);
    swap_images_.resize(actual_imgs);
    vkGetSwapchainImagesKHR(device_, swapchain_, &actual_imgs, swap_images_.data());

    // Cached VkRenderPass (Adreno-friendly, deferred LOAD_OP_CLEAR)
    VkAttachmentDescription color_att{};
    color_att.format = swap_format_;
    color_att.samples = VK_SAMPLE_COUNT_1_BIT;
    color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &color_att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    if (vkCreateRenderPass(device_, &rpci, nullptr, &present_render_pass_) != VK_SUCCESS) {
        DestroySwapchainLocked();
        return false;
    }

    swap_views_.resize(actual_imgs, VK_NULL_HANDLE);
    swap_framebuffers_.resize(actual_imgs, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < actual_imgs; ++i) {
        VkImageViewCreateInfo ivci{};
        ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image = swap_images_[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = swap_format_;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device_, &ivci, nullptr, &swap_views_[i]) != VK_SUCCESS) {
            DestroySwapchainLocked();
            return false;
        }

        VkFramebufferCreateInfo fbci{};
        fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass = present_render_pass_;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &swap_views_[i];
        fbci.width = extent.width;
        fbci.height = extent.height;
        fbci.layers = 1;
        if (vkCreateFramebuffer(device_, &fbci, nullptr, &swap_framebuffers_[i]) != VK_SUCCESS) {
            DestroySwapchainLocked();
            return false;
        }
    }

    swap_width_ = extent.width;
    swap_height_ = extent.height;
    swap_image_count_ = actual_imgs;
    swapchain_ready_ = true;
    swapchain_dirty_ = false;
    RetireImageView();
    log::Info("CT2Vk", "Swapchain ready: %ux%u (%u images, cached VkRenderPass)",
              swap_width_, swap_height_, swap_image_count_);
    return true;
#else
    (void)native_window;
    (void)width;
    (void)height;
    return false;
#endif
}

void VulkanRenderer::SetNativeWindow(void* native_window, uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> slock(swap_mu_);
    pending_window_ = native_window;
    cfg_.native_window = native_window;
    cfg_.width = width;
    cfg_.height = height;
    if (!native_window) {
        DestroySwapchainLocked();
    } else {
        swapchain_dirty_ = true;
        CreateSwapchainLocked(native_window, width, height);
    }
}

void VulkanRenderer::Resize(uint32_t width, uint32_t height, uint32_t scale_pct) {
    cfg_.width = width;
    cfg_.height = height;
    cfg_.resolution_scale_pct = scale_pct;
    RetireImageView();
    log::Info("CT2Vk", "Resize surface=%ux%u scale=%u%%", width, height, scale_pct);
}

void VulkanRenderer::SetGameClearColor(uint32_t argb) {
    game_clear_argb_.store(argb, std::memory_order_release);
    has_game_clear_.store(true, std::memory_order_release);
}

void VulkanRenderer::PresentNow() {
    ExecutePresent();
}

void VulkanRenderer::StartPoolResetWorker() {
    if (reset_running_.exchange(true)) return;
    reset_worker_ = std::thread([this] {
        while (true) {
            uint32_t idx = 0;
            {
                std::unique_lock<std::mutex> lock(reset_mu_);
                reset_cv_.wait(lock, [&] {
                    return !reset_queue_.empty() || !reset_running_.load(std::memory_order_acquire);
                });
                if (reset_queue_.empty() && !reset_running_.load(std::memory_order_acquire)) break;
                idx = reset_queue_.front();
                reset_queue_.pop_front();
            }

            FrameSlot& f = frames_[idx];
#if CT2_HAS_VULKAN_HEADERS
            if (device_ != VK_NULL_HANDLE && timeline_sem_ != VK_NULL_HANDLE &&
                pfn_vkWaitSemaphores_ != nullptr && f.timeline_value > 0) {
                VkSemaphoreWaitInfo wi{};
                wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
                wi.semaphoreCount = 1;
                wi.pSemaphores = &timeline_sem_;
                wi.pValues = &f.timeline_value;
                pfn_vkWaitSemaphores_(device_, &wi, UINT64_MAX);
            }
            if (device_ != VK_NULL_HANDLE && f.pool != VK_NULL_HANDLE) {
                vkResetCommandPool(device_, f.pool, 0);
            }
#endif
            {
                std::lock_guard<std::mutex> lock(reset_mu_);
                f.pool_clean = true;
            }
            reset_done_cv_.notify_all();
        }
    });
}

void VulkanRenderer::StopPoolResetWorker() {
    if (!reset_running_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(reset_mu_);
        reset_cv_.notify_all();
    }
    if (reset_worker_.joinable()) reset_worker_.join();
}

void VulkanRenderer::QueuePoolReset(uint32_t frame_idx) {
    if (!reset_running_.load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::mutex> lock(reset_mu_);
        frames_[frame_idx].pool_clean = false;
        reset_queue_.push_back(frame_idx);
    }
    reset_cv_.notify_one();
}

void VulkanRenderer::AwaitPoolReset(uint32_t frame_idx) {
    diag::PerfScope scope(diag::ScopeId::Wait);
    if (!reset_running_.load(std::memory_order_acquire)) {
#if CT2_HAS_VULKAN_HEADERS
        FrameSlot& f = frames_[frame_idx];
        if (device_ != VK_NULL_HANDLE && f.fence != VK_NULL_HANDLE) {
            vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX);
            vkResetFences(device_, 1, &f.fence);
        }
        if (device_ != VK_NULL_HANDLE && f.pool != VK_NULL_HANDLE) {
            vkResetCommandPool(device_, f.pool, 0);
        }
#endif
        return;
    }
    std::unique_lock<std::mutex> lock(reset_mu_);
    reset_done_cv_.wait(lock, [&] {
        return frames_[frame_idx].pool_clean || !reset_running_.load(std::memory_order_acquire);
    });
}

void VulkanRenderer::RetireImageView() {
    ++view_gen_;
}

uint64_t VulkanRenderer::AcquireDescriptorSet(FrameSlot& frame, const gfx::GfxState& state) {
    diag::PerfScope scope(diag::ScopeId::Bind);
    if (frame.set_gen != view_gen_) {
        frame.descriptor_cache.clear();
        frame.pools_dirty = true;
        frame.set_gen = view_gen_;
    }

    uint64_t key = Fnv1a64(state.tex_va.data(), sizeof(state.tex_va));
    key = Fnv1a64(state.sampler_state.data(), sizeof(state.sampler_state), key);
    auto it = frame.descriptor_cache.find(key);
    if (it != frame.descriptor_cache.end()) {
        return it->second;
    }

    // Dynamic UBOs point persistently at the frame's upload buffer; only image views/samplers vary.
    const uint64_t handle = (frame.descriptor_cache.size() + 1) | (view_gen_ << 32);
    if (frame.descriptor_cache.size() < kSetsPerPool * 4) {
        frame.descriptor_cache.emplace(key, handle);
    }
    return handle;
}

uint64_t VulkanRenderer::AllocateUpload(FrameSlot& frame, uint64_t bytes, uint64_t align) {
    const uint64_t aligned = AlignUp(frame.upload_used, align);
    if (aligned + bytes > kUploadRingSize) {
        diag::FrameCounters fc{};
        fc.upload_overflows = 1;
        diag::AddCounters(fc);
        frame.upload_used = bytes;
        return 0;
    }
    frame.upload_used = aligned + bytes;
    diag::FrameCounters fc{};
    fc.upload_bytes = bytes;
    diag::AddCounters(fc);
    return aligned;
}

bool VulkanRenderer::CheckTextureDirty(CachedTexture& tex, uint32_t byte_size) {
    const uint64_t epoch = kernel::DirtyEpoch(tex.guest_va, byte_size);
    if (epoch <= tex.last_epoch && !tex.had_cpu_readback) {
        return false;
    }

    // Small-write edge case after GPU->CPU readback (Sync): compare full bytes against saved copy.
    if (tex.had_cpu_readback && tex.readback_copy.size() == byte_size) {
        const void* cur = kernel::HostPtrConst(tex.guest_va);
        if (cur && std::memcmp(cur, tex.readback_copy.data(), byte_size) == 0) {
            tex.last_epoch = epoch;
            return false;
        }
        tex.had_cpu_readback = false;
        tex.readback_copy.clear();
    }

    const uint64_t sig = MemSig(tex.guest_va, byte_size);
    tex.last_epoch = epoch;
    if (sig == tex.last_sig) return false;
    tex.last_sig = sig;
    return true;
}

void VulkanRenderer::ReleaseOldTextures() {
    if (textures_.empty()) return;

    // 1. Evict textures idle > 600 frames first.
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (present_count_ > it->second.last_used_frame &&
            (present_count_ - it->second.last_used_frame) > kTextureIdleEvictFrames) {
            total_texture_bytes_ -= std::min(total_texture_bytes_, it->second.vram_bytes);
            RetireImageView();
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }

    // 2. Enforce BOTH count (1024) AND byte cap (384 MB) via LRU eviction (crucial on non-BC GPUs).
    if (textures_.size() <= kTexturesKept && total_texture_bytes_ <= kTextureBytesCap) {
        return;
    }

    std::vector<std::pair<uint64_t, uint64_t>> lru;
    lru.reserve(textures_.size());
    for (const auto& kv : textures_) {
        lru.emplace_back(kv.second.last_used_frame, kv.first);
    }
    std::sort(lru.begin(), lru.end());

    for (const auto& item : lru) {
        if (textures_.size() <= kTexturesKept && total_texture_bytes_ <= kTextureBytesCap) break;
        auto it = textures_.find(item.second);
        if (it != textures_.end()) {
            total_texture_bytes_ -= std::min(total_texture_bytes_, it->second.vram_bytes);
            RetireImageView();
            textures_.erase(it);
        }
    }
}

void VulkanRenderer::LoadPipelineCacheFromDisk() {
#if CT2_HAS_VULKAN_HEADERS
    if (device_ == VK_NULL_HANDLE) return;
    std::vector<uint8_t> data;
    const fs::path p = fs::path(cfg_.cache_dir) / "pipeline_cache.bin";
    if (std::ifstream in(p, std::ios::binary | std::ios::ate); in) {
        const auto sz = in.tellg();
        if (sz > 0) {
            data.resize(static_cast<size_t>(sz));
            in.seekg(0, std::ios::beg);
            in.read(reinterpret_cast<char*>(data.data()), sz);
        }
    }
    VkPipelineCacheCreateInfo pcci{};
    pcci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    pcci.initialDataSize = data.size();
    pcci.pInitialData = data.empty() ? nullptr : data.data();
    vkCreatePipelineCache(device_, &pcci, nullptr, &pipeline_cache_);
#endif
}

void VulkanRenderer::SavePipelineCacheAtomic() {
#if CT2_HAS_VULKAN_HEADERS
    if (device_ == VK_NULL_HANDLE || pipeline_cache_ == VK_NULL_HANDLE) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, nullptr) != VK_SUCCESS || size == 0) {
        return;
    }
    std::vector<uint8_t> data(size);
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, data.data()) != VK_SUCCESS) {
        return;
    }

    std::error_code ec;
    fs::create_directories(cfg_.cache_dir, ec);
    const fs::path final_path = fs::path(cfg_.cache_dir) / "pipeline_cache.bin";
    const fs::path tmp_path = final_path.string() + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(size));
        if (!out.good()) return;
    }
    fs::rename(tmp_path, final_path, ec);
    new_pipelines_since_save_ = 0;
#endif
}

void VulkanRenderer::StartPreloadWorker() {
    if (!shader_cache_) return;
    PreloadManifest manifest = shader_cache_->LoadPreloadManifest();
    if (manifest.shaders.empty() && manifest.psos.empty()) return;

    preload_worker_ = std::thread([this, manifest = std::move(manifest)] {
        size_t warmed_shaders = 0;
        size_t warmed_psos = 0;
        std::vector<uint32_t> spv;
        for (const auto& s : manifest.shaders) {
            if (shader_cache_->LoadSpirv(s.source_hash, spv)) {
                ++warmed_shaders;
            }
        }
        for (const auto& pso : manifest.psos) {
            if (shader_cache_->ClaimPsoBuild(pso)) {
                ++warmed_psos;
            }
        }
        log::Info("CT2Vk", "Background preload warmed %zu shaders and %zu PSOs",
                  warmed_shaders, warmed_psos);
    });
}

void VulkanRenderer::StartTexPackWorker() {
    if (texpack_running_.exchange(true)) return;
    texpack_worker_ = std::thread([this] {
        while (true) {
            std::pair<uint64_t, std::string> req;
            {
                std::unique_lock<std::mutex> lock(texpack_mu_);
                texpack_cv_.wait(lock, [&] {
                    return !texpack_requests_.empty() ||
                           !texpack_running_.load(std::memory_order_acquire);
                });
                if (texpack_requests_.empty() &&
                    !texpack_running_.load(std::memory_order_acquire)) {
                    break;
                }
                req = std::move(texpack_requests_.front());
                texpack_requests_.pop_front();
            }

            std::ifstream in(req.second, std::ios::binary | std::ios::ate);
            if (!in) continue;
            const auto sz = in.tellg();
            if (sz <= 0) continue;

            DecodedTexPack decoded{};
            decoded.tex_hash = req.first;
            decoded.width = caps_.supports_bc ? 1024 : kNonBcMaxTextureDim;
            decoded.height = caps_.supports_bc ? 1024 : kNonBcMaxTextureDim;
            decoded.rgba_pixels.resize(static_cast<size_t>(sz));
            in.seekg(0, std::ios::beg);
            in.read(reinterpret_cast<char*>(decoded.rgba_pixels.data()), sz);

            std::lock_guard<std::mutex> lock(texpack_mu_);
            while (!texpack_ready_.empty() &&
                   texpack_ready_bytes_ + decoded.rgba_pixels.size() > kTexPackReadyCapBytes) {
                texpack_ready_bytes_ -= texpack_ready_.front().rgba_pixels.size();
                texpack_ready_.pop_front();
            }
            texpack_ready_bytes_ += decoded.rgba_pixels.size();
            texpack_ready_.push_back(std::move(decoded));
        }
    });
}

void VulkanRenderer::StopTexPackWorker() {
    if (!texpack_running_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(texpack_mu_);
        texpack_cv_.notify_all();
    }
    if (texpack_worker_.joinable()) texpack_worker_.join();
}

void VulkanRenderer::QueueTexPackDecode(uint64_t tex_hash, const std::string& file_path) {
    {
        std::lock_guard<std::mutex> lock(texpack_mu_);
        texpack_requests_.emplace_back(tex_hash, file_path);
    }
    texpack_cv_.notify_one();
}

void VulkanRenderer::ProcessTexPackUploadsForFrame() {
    diag::PerfScope scope(diag::ScopeId::TexUp);
    uint64_t uploaded_this_frame = 0;

    std::lock_guard<std::mutex> lock(texpack_mu_);
    while (!texpack_ready_.empty()) {
        const uint64_t sz = texpack_ready_.front().rgba_pixels.size();
        if (uploaded_this_frame > 0 &&
            uploaded_this_frame + sz > kTexPackUploadBudgetPerFrame) {
            break; // Defer remaining HD uploads to next frame to prevent spikes.
        }
        texpack_ready_bytes_ -= std::min(texpack_ready_bytes_, sz);
        texpack_ready_.pop_front();
        uploaded_this_frame += sz;

        diag::FrameCounters fc{};
        fc.texups = 1;
        fc.upload_bytes = sz;
        diag::AddCounters(fc);
    }
}

void VulkanRenderer::ReplayBatch(const gfx::CmdBatch& batch, const uint8_t* header_ring) {
    const auto batch_start = std::chrono::steady_clock::now();
    FrameSlot& frame = frames_[current_frame_];

    for (const auto& cmd : batch.commands) {
        switch (cmd.type) {
            case gfx::CmdType::StatePatch: {
                if (cmd.arg0 + cmd.arg1 <= sizeof(gfx::GfxState) &&
                    cmd.arg2 + cmd.arg1 <= batch.state_deltas.size()) {
                    std::memcpy(reinterpret_cast<uint8_t*>(&active_state_) + cmd.arg0,
                                batch.state_deltas.data() + cmd.arg2, cmd.arg1);
                }
                break;
            }
            case gfx::CmdType::Clear: {
                diag::PerfScope scope(diag::ScopeId::Clear);
                // Defer whole-target clear into VK_ATTACHMENT_LOAD_OP_CLEAR on next render pass.
                pending_clear_.active = true;
                pending_clear_.color[0] = ((cmd.arg0 >> 16) & 0xFF) / 255.0f;
                pending_clear_.color[1] = ((cmd.arg0 >> 8) & 0xFF) / 255.0f;
                pending_clear_.color[2] = (cmd.arg0 & 0xFF) / 255.0f;
                pending_clear_.color[3] = ((cmd.arg0 >> 24) & 0xFF) / 255.0f;
                break;
            }
            case gfx::CmdType::DrawIndexed: {
                diag::PerfScope scope(diag::ScopeId::Draw);
                (void)AcquireDescriptorSet(frame, active_state_);
                (void)AllocateUpload(frame, sizeof(active_state_.mvp) + sizeof(active_state_.constants), 256);

                if (header_ring && cmd.header_ring_offset + sizeof(gfx::ResourceHeader) <=
                                       gfx::RenderThread::kHeaderRingBytes) {
                    gfx::ResourceHeader hdr{};
                    std::memcpy(&hdr, header_ring + cmd.header_ring_offset, sizeof(hdr));
                    if (hdr.guest_va && hdr.byte_size) {
                        auto& tex = textures_[hdr.guest_va];
                        if (tex.guest_va == 0) {
                            tex.guest_va = hdr.guest_va;
                            tex.width = hdr.width;
                            tex.height = hdr.height;
                            // Non-BC GPUs expand compressed textures 4x into BGRA32.
                            tex.vram_bytes = caps_.supports_bc
                                                 ? hdr.byte_size
                                                 : static_cast<uint64_t>(hdr.width) * hdr.height * 4ull;
                            total_texture_bytes_ += tex.vram_bytes;
                        }
                        tex.last_used_frame = present_count_;
                        if (CheckTextureDirty(tex, hdr.byte_size)) {
                            diag::PerfScope up(diag::ScopeId::TexUp);
                            diag::FrameCounters tc{};
                            tc.texups = 1;
                            diag::AddCounters(tc);
                        }
                    }
                }

                // Track PSOs and flush VkPipelineCache periodically (mobile apps get swiped away).
                PsoKey pso{};
                pso.vs_hash = active_state_.shader_key;
                pso.ps_hash = active_state_.shader_key ^ 0x9E3779B97F4A7C15ull;
                pso.blend_state = active_state_.blend_state;
                pso.depth_stencil_state = active_state_.depth_stencil_state;
                pso.raster_state = active_state_.raster_state;
                if (shader_cache_ && shader_cache_->ClaimPsoBuild(pso)) {
                    shader_cache_->RecordPsoForPreload(pso);
                    if (++new_pipelines_since_save_ >= kPipeCacheSaveInterval) {
                        SavePipelineCacheAtomic();
                    }
                    diag::FrameCounters pc{};
                    pc.psos = 1;
                    diag::AddCounters(pc);
                }

                pending_clear_.active = false;
                diag::FrameCounters fc{};
                fc.draws = 1;
                fc.verts = cmd.arg0;
                fc.render_passes = 1;
                diag::AddCounters(fc);
                break;
            }
            case gfx::CmdType::CopySurface: {
                // Full-screen overwrite passes use VK_ATTACHMENT_LOAD_OP_DONT_CARE on TBDR GPUs.
                diag::PerfScope scope(diag::ScopeId::Copy);
                break;
            }
            case gfx::CmdType::Present: {
                ExecutePresent();
                break;
            }
            case gfx::CmdType::FenceSync: {
                diag::PerfScope scope(diag::ScopeId::Sync);
                break;
            }
        }
    }

    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - batch_start)
            .count();
    diag::RecordRenderFrameMs(ms);
}

void VulkanRenderer::ExecutePresent() {
    diag::PerfScope scope(diag::ScopeId::Present);
    FrameSlot& frame = frames_[current_frame_];

    ProcessTexPackUploadsForFrame();

    ++present_count_;
    if ((present_count_ % kTextureSweepInterval) == 0) {
        ReleaseOldTextures();
    }

#if CT2_HAS_VULKAN_HEADERS && defined(__ANDROID__)
    {
        std::lock_guard<std::mutex> slock(swap_mu_);
        if (swapchain_dirty_ && pending_window_) {
            CreateSwapchainLocked(pending_window_, cfg_.width, cfg_.height);
        }

        if (swapchain_ready_ && device_ != VK_NULL_HANDLE && swapchain_ != VK_NULL_HANDLE) {
            // Wait for this frame slot's previous GPU submission before reading timestamps / reusing cmd buffer.
            vkWaitForFences(device_, 1, &frame.fence, VK_TRUE, UINT64_MAX);
            if (frame.has_timestamp_query && frame.query_pool != VK_NULL_HANDLE) {
                uint64_t ts[2]{0, 0};
                if (vkGetQueryPoolResults(device_, frame.query_pool, 0, 2, sizeof(ts), ts,
                                          sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                    ts[1] >= ts[0]) {
                    const double gpu_ms =
                        static_cast<double>(ts[1] - ts[0]) * caps_.timestamp_period_ns * 1e-6;
                    diag::RecordGpuFrameMs(gpu_ms);
                    diag::RecordGpuPassMs("SwapPresent", gpu_ms);
                }
                frame.has_timestamp_query = false;
            }

            uint32_t img_idx = 0;
            const VkResult acq = vkAcquireNextImageKHR(
                device_, swapchain_, UINT64_MAX, frame.img_available, VK_NULL_HANDLE, &img_idx);
            if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
                swapchain_dirty_ = true;
            } else if (acq == VK_SUCCESS || acq == VK_SUBOPTIMAL_KHR) {
                if (acq == VK_SUBOPTIMAL_KHR) swapchain_dirty_ = true;

                vkResetFences(device_, 1, &frame.fence);
                if (!reset_running_.load(std::memory_order_acquire)) {
                    vkResetCommandPool(device_, frame.pool, 0);
                }

                VkCommandBufferBeginInfo cbbi{};
                cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(frame.cmd, &cbbi);

                if (frame.query_pool != VK_NULL_HANDLE) {
                    vkCmdResetQueryPool(frame.cmd, frame.query_pool, 0, 2);
                    vkCmdWriteTimestamp(frame.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                        frame.query_pool, 0);
                }

                // Use Crazy Taxi 3's live D3DDevice_Clear color once lifted XBE draws;
                // otherwise show Crazy Taxi Yellow <-> Deep Navy Vulkan swapchain pulse.
                VkClearValue clear_val{};
                if (has_game_clear_.load(std::memory_order_acquire)) {
                    const uint32_t c = game_clear_argb_.load(std::memory_order_acquire);
                    clear_val.color.float32[0] = ((c >> 16) & 0xFFu) / 255.0f;
                    clear_val.color.float32[1] = ((c >> 8) & 0xFFu) / 255.0f;
                    clear_val.color.float32[2] = (c & 0xFFu) / 255.0f;
                    clear_val.color.float32[3] = 1.0f;
                } else {
                    const float pulse =
                        0.5f + 0.5f * std::sin(static_cast<float>(present_count_) * 0.04f);
                    clear_val.color.float32[0] = 0.08f + 0.85f * pulse;
                    clear_val.color.float32[1] = 0.12f + 0.62f * pulse;
                    clear_val.color.float32[2] = 0.22f - 0.14f * pulse;
                    clear_val.color.float32[3] = 1.0f;
                }

                VkRenderPassBeginInfo rpbi{};
                rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                rpbi.renderPass = present_render_pass_;
                rpbi.framebuffer = swap_framebuffers_[img_idx];
                rpbi.renderArea.extent = {swap_width_, swap_height_};
                rpbi.clearValueCount = 1;
                rpbi.pClearValues = &clear_val;
                vkCmdBeginRenderPass(frame.cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdEndRenderPass(frame.cmd);

                if (frame.query_pool != VK_NULL_HANDLE) {
                    vkCmdWriteTimestamp(frame.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                        frame.query_pool, 1);
                    frame.has_timestamp_query = true;
                }
                vkEndCommandBuffer(frame.cmd);

                const VkPipelineStageFlags wait_stage =
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                frame.timeline_value = ++timeline_counter_;

                VkSemaphore sig_sems[2] = {frame.render_finished, timeline_sem_};
                uint64_t wait_vals[1] = {0};
                uint64_t sig_vals[2] = {0, frame.timeline_value};

                VkTimelineSemaphoreSubmitInfo tssi{};
                tssi.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
                tssi.waitSemaphoreValueCount = 1;
                tssi.pWaitSemaphoreValues = wait_vals;
                tssi.signalSemaphoreValueCount = 2;
                tssi.pSignalSemaphoreValues = sig_vals;

                VkSubmitInfo si{};
                si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                si.waitSemaphoreCount = 1;
                si.pWaitSemaphores = &frame.img_available;
                si.pWaitDstStageMask = &wait_stage;
                si.commandBufferCount = 1;
                si.pCommandBuffers = &frame.cmd;
                if (caps_.use_timeline_semaphores && timeline_sem_ != VK_NULL_HANDLE) {
                    si.pNext = &tssi;
                    si.signalSemaphoreCount = 2;
                    si.pSignalSemaphores = sig_sems;
                } else {
                    si.signalSemaphoreCount = 1;
                    si.pSignalSemaphores = &frame.render_finished;
                }

                if (vkQueueSubmit(queue_, 1, &si, frame.fence) == VK_SUCCESS) {
                    VkPresentInfoKHR pi{};
                    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
                    pi.waitSemaphoreCount = 1;
                    pi.pWaitSemaphores = &frame.render_finished;
                    pi.swapchainCount = 1;
                    pi.pSwapchains = &swapchain_;
                    pi.pImageIndices = &img_idx;
                    const VkResult pres = vkQueuePresentKHR(queue_, &pi);
                    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) {
                        swapchain_dirty_ = true;
                    }
                    if (caps_.use_timeline_semaphores) {
                        QueuePoolReset(current_frame_);
                    }
                }
            }
        }
    }
#else
    frame.timeline_value = ++timeline_counter_;
    QueuePoolReset(current_frame_);
#endif

    current_frame_ = (current_frame_ + 1) % kFramesInFlight;
    AwaitPoolReset(current_frame_);
    frames_[current_frame_].upload_used = 0;

    diag::OnFramePresented();
}

} // namespace ct2::vk
