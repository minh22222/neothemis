#include "WindowsBackdrop.hpp"

#include <QWidget>

#include <algorithm>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace neothemis::gui {

void apply_windows_backdrop(QWidget* window,
                            bool transparency_enabled,
                            int transparency_percent,
                            bool blur_enabled,
                            int blur_radius) {
    if (!window) {
        return;
    }

    const bool enabled = transparency_enabled || blur_enabled;
    window->setAttribute(Qt::WA_TranslucentBackground, enabled);

#ifdef Q_OS_WIN
    enum AccentState {
        AccentDisabled = 0,
        AccentTransparentGradient = 2,
        AccentBlurBehind = 3,
        AccentAcrylicBlurBehind = 4
    };
    struct AccentPolicy {
        int state;
        int flags;
        DWORD gradient_color;
        int animation_id;
    };
    struct CompositionAttributeData {
        int attribute;
        void* data;
        SIZE_T data_size;
    };
    using SetWindowCompositionAttributeFn =
        BOOL(WINAPI*)(HWND, CompositionAttributeData*);

    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    auto set_composition = user32
                               ? reinterpret_cast<SetWindowCompositionAttributeFn>(
                                     GetProcAddress(user32, "SetWindowCompositionAttribute"))
                               : nullptr;
    if (set_composition) {
        AccentPolicy policy{};
        if (enabled) {
            policy.state = blur_enabled ? AccentAcrylicBlurBehind
                                        : AccentTransparentGradient;
            policy.flags = blur_enabled ? 2 : 0;

            const int transparency = std::clamp(transparency_percent, 0, 80);
            int alpha = transparency_enabled
                            ? 255 * (100 - transparency) / 100
                            : 255;
            if (blur_enabled) {
                const int blur = std::clamp(blur_radius, 0, 120);
                alpha = std::min(alpha, std::clamp(220 - blur, 72, 210));
            }
            // GradientColor is AABBGGRR. The dark tint keeps text contrast stable.
            policy.gradient_color = (static_cast<DWORD>(alpha) << 24U) |
                                    (0x17U << 16U) | (0x10U << 8U) | 0x09U;
        } else {
            policy.state = AccentDisabled;
        }

        CompositionAttributeData data{19, &policy, sizeof(policy)};
        HWND handle = reinterpret_cast<HWND>(window->winId());
        if (!set_composition(handle, &data) && blur_enabled) {
            policy.state = AccentBlurBehind;
            set_composition(handle, &data);
        }
    }

    // Windows 11 exposes acrylic as a documented system backdrop. Keep the
    // accent policy above for Windows 10 and as the tint/transparency control.
    HMODULE dwmapi = LoadLibraryW(L"dwmapi.dll");
    auto set_dwm_attribute = dwmapi
                                 ? reinterpret_cast<DwmSetWindowAttributeFn>(
                                       GetProcAddress(dwmapi, "DwmSetWindowAttribute"))
                                 : nullptr;
    if (set_dwm_attribute) {
        constexpr DWORD use_immersive_dark_mode = 20;
        constexpr DWORD system_backdrop_type = 38;
        constexpr int backdrop_none = 1;
        constexpr int backdrop_acrylic = 3;
        const BOOL dark_mode = TRUE;
        const int backdrop = blur_enabled ? backdrop_acrylic : backdrop_none;
        HWND handle = reinterpret_cast<HWND>(window->winId());
        set_dwm_attribute(handle, use_immersive_dark_mode,
                          &dark_mode, sizeof(dark_mode));
        set_dwm_attribute(handle, system_backdrop_type,
                          &backdrop, sizeof(backdrop));
        RedrawWindow(handle, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    }
    if (dwmapi) {
        FreeLibrary(dwmapi);
    }
#else
    (void)transparency_percent;
    (void)blur_radius;
#endif
    window->update();
}

} // namespace neothemis::gui
