#pragma once

#include <QColor>

#include <string>

namespace neothemis::gui {

struct CyberThemeColors {
    QColor background;
    QColor primary;
    QColor secondary;
};

CyberThemeColors default_cyber_theme_colors();

void apply_application_theme(const std::string& theme);
void apply_application_theme(const std::string& theme,
                             const CyberThemeColors& cyber_colors);

} // namespace neothemis::gui
