#pragma once

class QWidget;

namespace neothemis::gui {

bool native_background_blur_supported();

void apply_windows_backdrop(QWidget* window,
                            bool transparency_enabled,
                            bool blur_enabled,
                            bool force_compositor_update = false);

} // namespace neothemis::gui
