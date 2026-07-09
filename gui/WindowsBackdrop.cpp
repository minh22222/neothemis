#include "WindowsBackdrop.hpp"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QOperatingSystemVersion>
#include <QProcess>
#include <QWindow>
#include <QWidget>

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

bool native_background_blur_supported() {
#ifdef Q_OS_WIN
    static const bool supported = [] {
        const QOperatingSystemVersion version =
            QOperatingSystemVersion::current();
        return version.type() == QOperatingSystemVersion::Windows &&
               version.majorVersion() >= 10 &&
               version.microVersion() >= 22621;
    }();
    return supported;
#else
    return true;
#endif
}

void apply_windows_backdrop(QWidget* window,
                            bool transparency_enabled,
                            bool blur_enabled,
                            bool force_compositor_update) {
    if (!window) {
        return;
    }
    window->setAttribute(Qt::WA_TranslucentBackground, true);
    window->setAttribute(Qt::WA_NoSystemBackground, true);
    window->setAutoFillBackground(false);
    apply_compositor_blur(window, blur_enabled && transparency_enabled,
                          force_compositor_update);

#ifdef Q_OS_WIN
    HWND handle = reinterpret_cast<HWND>(window->winId());
    if (native_background_blur_supported()) {
        constexpr DWORD use_immersive_dark_mode = 20;
        constexpr DWORD system_backdrop_type = 38;
        constexpr int backdrop_none = 1;
        constexpr int backdrop_desktop_acrylic = 3;
        const BOOL dark_mode = TRUE;
        const bool acrylic_enabled =
            blur_enabled && transparency_enabled;
        const int backdrop = acrylic_enabled
                                 ? backdrop_desktop_acrylic
                                 : backdrop_none;
        DwmSetWindowAttribute(handle, use_immersive_dark_mode,
                              &dark_mode, sizeof(dark_mode));
        DwmSetWindowAttribute(handle, system_backdrop_type,
                              &backdrop, sizeof(backdrop));
        RedrawWindow(handle, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    }
#endif
    window->update();
}

} // namespace neothemis::gui
