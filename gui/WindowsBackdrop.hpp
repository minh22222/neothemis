#pragma once

class QWidget;

namespace neothemis::gui {

void apply_windows_backdrop(QWidget* window,
                            bool transparency_enabled,
                            int transparency_percent,
                            bool blur_enabled,
                            int blur_radius);

} // namespace neothemis::gui
