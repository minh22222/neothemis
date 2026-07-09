#include "WindowsBackdrop.hpp"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QWindow>
#include <QWidget>

#include <algorithm>
#include <map>
#include <mutex>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

#ifdef NEOTHEMIS_HAS_KWINDOWSYSTEM
#include <KWindowEffects>
#endif

namespace {

#if defined(Q_OS_LINUX)
QString hyprland_window_address(QWidget* window) {
    if (!window || qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")) {
        return {};
    }

    QProcess process;
    process.start("hyprctl", QStringList{"-j", "clients"});
    if (!process.waitForFinished(700) || process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0) {
        return {};
    }

    QJsonParseError error{};
    QJsonDocument document = QJsonDocument::fromJson(process.readAllStandardOutput(), &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        return {};
    }

    const qint64 pid = QCoreApplication::applicationPid();
    const QString title = window->windowTitle();
    QString fallback;
    for (const QJsonValue& value : document.array()) {
        QJsonObject client = value.toObject();
        if (client.value("pid").toInteger() != pid) {
            continue;
        }
        const QString address = client.value("address").toString();
        if (address.isEmpty()) {
            continue;
        }
        if (fallback.isEmpty()) {
            fallback = address;
        }
        if (client.value("title").toString() == title) {
            return address;
        }
    }
    return fallback;
}

bool run_hyprctl_setprop(const QString& address,
                         const QString& property,
                         const QString& value) {
    auto run = [](const QStringList& arguments) {
        QProcess process;
        process.start("hyprctl", arguments);
        return process.waitForFinished(700) &&
               process.exitStatus() == QProcess::NormalExit &&
               process.exitCode() == 0;
    };

    const QString target = "address:" + address;
    const QStringList dispatch_args{"dispatch", "setprop", target, property, value};
    if (run(dispatch_args)) {
        return true;
    }

    const QStringList legacy_args{"setprop", target, property, value};
    return run(legacy_args);
}

void apply_hyprland_blur(QWidget* window, bool blur_enabled, bool force_update) {
    static std::mutex cache_mutex;
    static std::map<WId, bool> last_blur_state;

    WId id = window ? window->winId() : 0;
    if (id == 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto found = last_blur_state.find(id);
        if (!force_update && found != last_blur_state.end() &&
            found->second == blur_enabled) {
            return;
        }
    }

    const QString address = hyprland_window_address(window);
    if (address.isEmpty()) {
        return;
    }

    const QString no_blur = blur_enabled ? "0" : "1";
    bool ok = run_hyprctl_setprop(address, "no_blur", no_blur);
    if (!ok) {
        ok = run_hyprctl_setprop(address, "noblur", no_blur);
    }
    if (ok) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        last_blur_state[id] = blur_enabled;
    }
}
#endif

void apply_compositor_blur(QWidget* window, bool blur_enabled, bool force_update) {
#ifdef NEOTHEMIS_HAS_KWINDOWSYSTEM
    if (window) {
        window->winId();
        if (QWindow* handle = window->windowHandle()) {
            KWindowEffects::enableBlurBehind(handle, blur_enabled);
        }
    }
#else
    (void)window;
    (void)blur_enabled;
    (void)force_update;
#endif

#if defined(Q_OS_LINUX)
    apply_hyprland_blur(window, blur_enabled, force_update);
#endif
}

} // namespace

namespace neothemis::gui {

void apply_windows_backdrop(QWidget* window,
                            bool transparency_enabled,
                            int transparency_percent,
                            bool blur_enabled,
                            bool force_compositor_update) {
    if (!window) {
        return;
    }

    const bool enabled = transparency_enabled || blur_enabled;
    window->setAttribute(Qt::WA_TranslucentBackground, true);
    window->setAttribute(Qt::WA_NoSystemBackground, true);
    window->setAutoFillBackground(false);
    apply_compositor_blur(window, blur_enabled && transparency_enabled,
                          force_compositor_update);

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
    using DwmEnableBlurBehindWindowFn =
        HRESULT(WINAPI*)(HWND, const DWM_BLURBEHIND*);

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

            const int transparency = std::clamp(transparency_percent, 0, 95);
            int alpha = transparency_enabled
                            ? 255 * (100 - transparency) / 100
                            : 255;
            if (blur_enabled) {
                constexpr int acrylic_tint_alpha = 112;
                alpha = std::min(alpha, acrylic_tint_alpha);
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
    auto enable_dwm_blur = dwmapi
                               ? reinterpret_cast<DwmEnableBlurBehindWindowFn>(
                                     GetProcAddress(dwmapi, "DwmEnableBlurBehindWindow"))
                               : nullptr;
    HWND handle = reinterpret_cast<HWND>(window->winId());
    if (enable_dwm_blur) {
        DWM_BLURBEHIND blur_behind{};
        blur_behind.dwFlags = DWM_BB_ENABLE | DWM_BB_TRANSITIONONMAXIMIZED;
        blur_behind.fEnable = blur_enabled && transparency_enabled;
        blur_behind.fTransitionOnMaximized = TRUE;
        enable_dwm_blur(handle, &blur_behind);
    }
    if (set_dwm_attribute) {
        constexpr DWORD use_immersive_dark_mode = 20;
        constexpr DWORD system_backdrop_type = 38;
        constexpr int backdrop_none = 1;
        constexpr int backdrop_acrylic = 3;
        const BOOL dark_mode = TRUE;
        const int backdrop = blur_enabled ? backdrop_acrylic : backdrop_none;
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
    (void)enabled;
    (void)transparency_percent;
#endif
    window->update();
}

} // namespace neothemis::gui
