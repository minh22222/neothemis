#pragma once

class QWidget;

namespace neothemis::gui {

void apply_windows_backdrop(QWidget* window,
                            bool transparency_enabled,
                            int transparency_percent,
                            bool blur_enabled,
                            bool force_compositor_update = false);

} // namespace neothemis::gui
