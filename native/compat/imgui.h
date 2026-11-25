#pragma once

// The Android port has no ImGui host UI: the touch overlay is drawn by the
// Kotlin side and the recompiled title draws its own HUD through Vulkan.
// These stubs only satisfy the optional UI hooks in the portable backend;
// ImGui::GetCurrentContext() stays null so VulkanBackend::InitUi() disables
// the ImGui render pass entirely.

#include <vulkan/vulkan.h>

namespace ImGui {

struct ImGuiIO {
    const char* BackendRendererName = nullptr;
};

struct ImDrawData;

inline bool GetCurrentContext() { return false; }
inline ImGuiIO& GetIO() {
    static ImGuiIO io;
    return io;
}
inline ImDrawData* GetDrawData() { return nullptr; }
inline void NewFrame() {}
inline void Render() {}

}  // namespace ImGui

struct ImGui_ImplVulkan_InitInfo {
    VkInstance Instance = VK_NULL_HANDLE;
    VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
    VkDevice Device = VK_NULL_HANDLE;
    uint32_t QueueFamily = 0;
    VkQueue Queue = VK_NULL_HANDLE;
    VkDescriptorPool DescriptorPool = VK_NULL_HANDLE;
    uint32_t MinImageCount = 2;
    uint32_t ImageCount = 2;
    VkSampleCountFlagBits MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    bool UseDynamicRendering = false;
    VkRenderPass RenderPass = VK_NULL_HANDLE;
    VkPipelineRenderingCreateInfo PipelineRenderingCreateInfo{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
};

inline bool ImGui_ImplVulkan_Init(const ImGui_ImplVulkan_InitInfo*) { return false; }
inline void ImGui_ImplVulkan_CreateFontsTexture() {}
inline void ImGui_ImplVulkan_NewFrame() {}
inline void ImGui_ImplVulkan_RenderDrawData(ImGui::ImDrawData*, VkCommandBuffer) {}
