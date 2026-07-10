#include "WindowsBackdrop.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QOperatingSystemVersion>
#include <QProcess>
#include <QWindow>
#include <QWidget>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <map>
#include <mutex>
#include <string>

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

#if defined(Q_OS_WIN)

constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmSystemBackdropType = 38;
constexpr int kDwmBackdropNone = 1;
constexpr int kDwmBackdropDesktopAcrylic = 3;
constexpr int kMinimumPrivateAcrylicBuild = 17763;
constexpr int kMinimumSystemBackdropBuild = 22621;

enum class WindowsBackdropBackend {
    none,
    dwm_desktop_acrylic,
    accent_policy,
    accent_host_backdrop,
};

struct WindowsBackdropState {
    HWND window = nullptr;
    bool enabled = false;
    WindowsBackdropBackend backend = WindowsBackdropBackend::none;
    QColor tint;
    int tint_opacity = 0;
};

enum class WindowCompositionAttribute : int {
    accent_policy = 19,
};

enum class AccentState : int {
    disabled = 0,
    acrylic_blur_behind = 4,
    host_backdrop = 5,
};

struct AccentPolicy {
    AccentState state = AccentState::disabled;
    int flags = 0;
    DWORD gradient_color = 0;
    int animation_id = 0;
};

struct WindowCompositionAttributeData {
    WindowCompositionAttribute attribute =
        WindowCompositionAttribute::accent_policy;
    PVOID data = nullptr;
    SIZE_T size = 0;
};

using SetWindowCompositionAttributeFunction =
    BOOL(WINAPI*)(HWND, WindowCompositionAttributeData*);

std::mutex windows_backdrop_mutex;
std::map<QWidget*, WindowsBackdropState> windows_backdrop_states;

int windows_build_number() {
    const QOperatingSystemVersion version = QOperatingSystemVersion::current();
    if (version.type() != QOperatingSystemVersion::Windows ||
        version.majorVersion() < 10) {
        return 0;
    }
    return version.microVersion();
}

bool dwm_composition_enabled() {
    BOOL enabled = FALSE;
    return SUCCEEDED(DwmIsCompositionEnabled(&enabled)) && enabled == TRUE;
}

bool software_or_virtual_display_driver() {
    static const bool software_driver = [] {
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        for (DWORD index = 0;
             EnumDisplayDevicesW(nullptr, index, &device, 0) == TRUE;
             ++index) {
            if ((device.StateFlags & DISPLAY_DEVICE_ACTIVE) == 0) {
                device = {};
                device.cb = sizeof(device);
                continue;
            }
            std::wstring name(device.DeviceString);
            std::transform(name.begin(), name.end(), name.begin(),
                           [](wchar_t value) {
                               return static_cast<wchar_t>(std::towlower(value));
                           });
            if (name.find(L"qxl") != std::wstring::npos ||
                name.find(L"basic display") != std::wstring::npos ||
                name.find(L"remote display") != std::wstring::npos) {
                return true;
            }
            device = {};
            device.cb = sizeof(device);
        }
        return false;
    }();
    return software_driver;
}

SetWindowCompositionAttributeFunction set_window_composition_attribute() {
    static const auto function = [] {
        const FARPROC address =
            GetProcAddress(GetModuleHandleW(L"user32.dll"),
                           "SetWindowCompositionAttribute");
        SetWindowCompositionAttributeFunction typed = nullptr;
        static_assert(sizeof(typed) == sizeof(address));
        std::memcpy(&typed, &address, sizeof(typed));
        return typed;
    }();
    return function;
}

DWORD accent_gradient_color(QColor tint, int opacity) {
    if (!tint.isValid()) {
        tint = QColor(22, 28, 34);
    }
    const DWORD alpha = static_cast<DWORD>(std::clamp(opacity, 1, 255));
    return (alpha << 24) |
           (static_cast<DWORD>(tint.blue()) << 16) |
           (static_cast<DWORD>(tint.green()) << 8) |
           static_cast<DWORD>(tint.red());
}

bool set_accent_policy(HWND window,
                       AccentState state,
                       const QColor& tint = {},
                       int tint_opacity = 0) {
    const auto function = set_window_composition_attribute();
    if (!function) {
        return false;
    }
    AccentPolicy policy;
    policy.state = state;
    if (state == AccentState::acrylic_blur_behind ||
        state == AccentState::host_backdrop) {
        policy.gradient_color = accent_gradient_color(tint, tint_opacity);
    }
    WindowCompositionAttributeData data;
    data.data = &policy;
    data.size = sizeof(policy);
    return function(window, &data) == TRUE;
}

void request_frame_refresh(HWND window) {
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    RedrawWindow(window, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
}

void disable_windows_backdrop(HWND window) {
    set_accent_policy(window, AccentState::disabled);
    const int backdrop = kDwmBackdropNone;
    DwmSetWindowAttribute(window, kDwmSystemBackdropType,
                          &backdrop, sizeof(backdrop));
    const MARGINS client_area{0, 0, 0, 0};
    DwmExtendFrameIntoClientArea(window, &client_area);
}

WindowsBackdropBackend enable_windows_backdrop(HWND window,
                                                const QColor& tint,
                                                int tint_opacity) {
    const BOOL dark_mode = tint.isValid() && tint.lightness() < 150 ? TRUE : FALSE;
    DwmSetWindowAttribute(window, kDwmUseImmersiveDarkMode,
                          &dark_mode, sizeof(dark_mode));

    if (software_or_virtual_display_driver()) {
        const int none = kDwmBackdropNone;
        DwmSetWindowAttribute(window, kDwmSystemBackdropType,
                              &none, sizeof(none));
        const MARGINS client_area{0, 0, 0, 0};
        DwmExtendFrameIntoClientArea(window, &client_area);
        if (set_accent_policy(window, AccentState::host_backdrop,
                              tint, tint_opacity)) {
            return WindowsBackdropBackend::accent_host_backdrop;
        }
    }

    if (windows_build_number() >= kMinimumSystemBackdropBuild) {
        set_accent_policy(window, AccentState::disabled);
        const MARGINS client_area{-1, -1, -1, -1};
        const HRESULT frame_result =
            DwmExtendFrameIntoClientArea(window, &client_area);
        const int backdrop = kDwmBackdropDesktopAcrylic;
        const HRESULT backdrop_result =
            DwmSetWindowAttribute(window, kDwmSystemBackdropType,
                                  &backdrop, sizeof(backdrop));
        if (SUCCEEDED(frame_result) && SUCCEEDED(backdrop_result)) {
            return WindowsBackdropBackend::dwm_desktop_acrylic;
        }
        const int none = kDwmBackdropNone;
        DwmSetWindowAttribute(window, kDwmSystemBackdropType,
                              &none, sizeof(none));
    }

    if (windows_build_number() >= kMinimumPrivateAcrylicBuild &&
        set_accent_policy(window, AccentState::acrylic_blur_behind,
                          tint, tint_opacity)) {
        return WindowsBackdropBackend::accent_policy;
    }
    disable_windows_backdrop(window);
    return WindowsBackdropBackend::none;
}

bool windows_blur_capable() {
    const int build = windows_build_number();
    if (build < kMinimumPrivateAcrylicBuild || !dwm_composition_enabled()) {
        return false;
    }
    return build >= kMinimumSystemBackdropBuild ||
           set_window_composition_attribute() != nullptr;
}

#endif

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

#if !defined(Q_OS_WIN)
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
#endif

} // namespace

namespace neothemis::gui {

bool native_background_blur_supported() {
#ifdef Q_OS_WIN
    static const bool supported = windows_blur_capable();
    return supported;
#else
    return true;
#endif
}

bool windows_backdrop_message_requires_refresh(void* native_message) {
#ifdef Q_OS_WIN
    const auto* message = static_cast<const MSG*>(native_message);
    if (!message) {
        return false;
    }
    return message->message == WM_DWMCOMPOSITIONCHANGED ||
           message->message == WM_THEMECHANGED ||
           message->message == WM_SETTINGCHANGE;
#else
    (void)native_message;
    return false;
#endif
}

bool apply_windows_backdrop(QWidget* window,
                            bool transparency_enabled,
                            bool blur_enabled,
                            const QColor& tint,
                            int tint_opacity,
                            bool force_compositor_update) {
    if (!window) {
        return false;
    }
    const bool backdrop_enabled = blur_enabled && transparency_enabled;

#ifdef Q_OS_WIN
    HWND handle = reinterpret_cast<HWND>(window->winId());
    if (!handle) {
        return false;
    }
    const int clamped_tint_opacity = std::clamp(tint_opacity, 1, 255);
    {
        std::lock_guard<std::mutex> lock(windows_backdrop_mutex);
        auto found = windows_backdrop_states.find(window);
        if (found != windows_backdrop_states.end() &&
            found->second.window != handle) {
            if (found->second.enabled && IsWindow(found->second.window)) {
                disable_windows_backdrop(found->second.window);
            }
            windows_backdrop_states.erase(found);
            found = windows_backdrop_states.end();
        }
        if (!force_compositor_update && found != windows_backdrop_states.end() &&
            found->second.enabled == backdrop_enabled &&
            (!backdrop_enabled ||
             (found->second.tint == tint &&
              found->second.tint_opacity == clamped_tint_opacity))) {
            return found->second.enabled;
        }

        if (!backdrop_enabled) {
            if (found != windows_backdrop_states.end() && !found->second.enabled) {
                return false;
            }
            const bool was_enabled =
                found != windows_backdrop_states.end() && found->second.enabled;
            disable_windows_backdrop(handle);
            windows_backdrop_states[window] =
                {handle, false, WindowsBackdropBackend::none, {}, 0};
            if (was_enabled) {
                request_frame_refresh(handle);
                window->update();
            }
            return false;
        }

        const WindowsBackdropBackend backend =
            enable_windows_backdrop(handle, tint, clamped_tint_opacity);
        const bool enabled = backend != WindowsBackdropBackend::none;
        windows_backdrop_states[window] =
            {handle, enabled, backend, tint, clamped_tint_opacity};
        request_frame_refresh(handle);
        window->update();
        return enabled;
    }
#else
    window->setAttribute(Qt::WA_NoSystemBackground, true);
    window->setAutoFillBackground(false);
    apply_compositor_blur(window, backdrop_enabled,
                          force_compositor_update);
    window->update();
    (void)tint;
    (void)tint_opacity;
    return backdrop_enabled;
#endif
}

void release_windows_backdrop(QWidget* window) {
    if (!window) {
        return;
    }
#ifdef Q_OS_WIN
    std::lock_guard<std::mutex> lock(windows_backdrop_mutex);
    const auto found = windows_backdrop_states.find(window);
    if (found == windows_backdrop_states.end()) {
        return;
    }
    if (found->second.enabled && IsWindow(found->second.window)) {
        disable_windows_backdrop(found->second.window);
    }
    windows_backdrop_states.erase(found);
#else
    apply_compositor_blur(window, false, true);
#endif
}

} // namespace neothemis::gui
