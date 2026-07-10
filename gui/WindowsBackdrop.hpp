#pragma once

#include <QColor>

class QWidget;

namespace neothemis::gui {

bool native_background_blur_supported();
bool windows_backdrop_message_requires_refresh(void* native_message);

bool apply_windows_backdrop(QWidget* window,
                            bool transparency_enabled,
                            bool blur_enabled,
                            const QColor& tint,
                            int tint_opacity,
                            bool force_compositor_update = false);

void release_windows_backdrop(QWidget* window);

} // namespace neothemis::gui
