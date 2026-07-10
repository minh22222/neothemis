#pragma once

#include <QColor>

#include <string>

namespace neothemis::gui {

struct CyberThemeColors {
    QColor background;
    QColor primary;
    QColor secondary;
    QColor text;
    QColor muted_text;
    QColor primary_text;
    QColor secondary_text;
};

CyberThemeColors default_cyber_theme_colors();
QColor ensure_theme_text_contrast(const QColor& preferred,
                                  const QColor& surface,
                                  double minimum_ratio = 4.5);

void apply_application_theme(const std::string& theme);
void apply_application_theme(const std::string& theme,
                             const CyberThemeColors& cyber_colors);

} // namespace neothemis::gui
