#pragma once

// Tiny SDL3-compatible surface shim: native/mw_vulkan.cpp was written against
// SDL's Vulkan helpers, but the Android APK owns its ANativeWindow directly.
#include <cstdint>

#include "native/compat/mw_host.h"

struct SDL_Window;
using SDL_PropertiesID = uint32_t;

inline constexpr const char* SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER = "ct3.android.window";

inline SDL_PropertiesID SDL_GetWindowProperties(SDL_Window*) { return 1; }

inline void* SDL_GetPointerProperty(SDL_PropertiesID, const char*, void*) {
    return ct2::sdk_bridge::GetAndroidWindow();
}

inline const char* SDL_GetError() { return "Android Vulkan surface unavailable"; }
