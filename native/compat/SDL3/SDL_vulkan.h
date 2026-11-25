#pragma once

#include "native/compat/SDL3/SDL.h"

#ifndef VK_USE_PLATFORM_ANDROID_KHR
#if defined(__ANDROID__)
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#endif

#include <iterator>
#include <vulkan/vulkan.h>

// Only the Android surface path is used by the ported backend; the window is
// supplied by native/android_jni.cpp through ct2::sdk_bridge.
inline const char* const* SDL_Vulkan_GetInstanceExtensions(uint32_t* count) {
    static const char* const kExtensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
    };
    if (count) *count = uint32_t(sizeof(kExtensions) / sizeof(kExtensions[0]));
    return kExtensions;
}

inline int SDL_Vulkan_CreateSurface(SDL_Window*, VkInstance instance,
                                    const VkAllocationCallbacks* allocator, VkSurfaceKHR* out) {
    if (!out) return 0;
    *out = VK_NULL_HANDLE;
#ifdef __ANDROID__
    ANativeWindow* window = ct2::sdk_bridge::GetAndroidWindow();
    if (!window) return 0;
    VkAndroidSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    info.window = window;
    return vkCreateAndroidSurfaceKHR(instance, &info, allocator, out) == VK_SUCCESS ? 1 : 0;
#else
    (void)instance;
    (void)allocator;
    return 0;
#endif
}
