#include "ThemeBackground.hpp"

#include <QLinearGradient>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <algorithm>

namespace {

QColor with_opacity(QColor color, int opacity) {
    color.setAlpha(opacity);
    return color;
}

QColor layered_color(int red, int green, int blue, int alpha, int opacity) {
    return QColor(red, green, blue, alpha * opacity / 255);
}

QColor usable_color(const QColor& color, const QColor& fallback) {
    return color.isValid() ? color : fallback;
}

QColor mix_color(const QColor& first, const QColor& second, double second_weight) {
    const double weight = std::clamp(second_weight, 0.0, 1.0);
    const double first_weight = 1.0 - weight;
    return QColor(
        std::clamp(static_cast<int>(first.red() * first_weight +
                                    second.red() * weight),
                   0, 255),
        std::clamp(static_cast<int>(first.green() * first_weight +
                                    second.green() * weight),
                   0, 255),
        std::clamp(static_cast<int>(first.blue() * first_weight +
                                    second.blue() * weight),
                   0, 255));
}

QColor layered_color(QColor color, int alpha, int opacity) {
    color.setAlpha(alpha * opacity / 255);
    return color;
}

bool is_cyber_theme(const std::string& theme) {
    return theme == "cyber" || theme == "glassy-dark";
}

void paint_background(QPainter& painter,
                      int canvas_width,
                      int canvas_height,
                      const std::string& theme,
                      int opacity,
                      int blur_radius,
                      const neothemis::gui::CyberThemeColors& cyber_colors) {
    painter.setRenderHint(QPainter::Antialiasing, true);

    const bool cyber = is_cyber_theme(theme);
    const neothemis::gui::CyberThemeColors defaults =
        neothemis::gui::default_cyber_theme_colors();
    const QColor background =
        usable_color(cyber_colors.background, defaults.background);
    const QColor primary = usable_color(cyber_colors.primary, defaults.primary);
    const QColor secondary =
        usable_color(cyber_colors.secondary, defaults.secondary);
    const bool light_background = background.lightness() > 170;
    const QColor background_deep = light_background
                                       ? mix_color(background, primary, 0.04)
                                       : background.darker(230);
    const QColor background_ink = light_background
                                      ? mix_color(background, secondary, 0.08)
                                      : background.darker(360);
    const QColor secondary_deep = light_background
                                      ? mix_color(background, secondary, 0.22)
                                      : secondary.darker(520);
    const QColor primary_mid = light_background
                                   ? mix_color(background, primary, 0.10)
                                   : background.darker(180);
    const QColor secondary_mid = light_background
                                     ? mix_color(background, secondary, 0.14)
                                     : mix_color(background_ink,
                                                 secondary.darker(360), 0.35);
    QLinearGradient base(0, 0, canvas_width, canvas_height);
    if (cyber) {
        base.setColorAt(0.0, with_opacity(background_deep, opacity));
        base.setColorAt(0.30, with_opacity(primary_mid, opacity));
        base.setColorAt(0.62, with_opacity(secondary_mid, opacity));
        base.setColorAt(0.84, with_opacity(secondary_deep, opacity));
        base.setColorAt(1.0, with_opacity(mix_color(background_ink, secondary_deep, 0.72),
                                          opacity));
    } else {
        base.setColorAt(0.0, with_opacity(QColor(16, 35, 44), opacity));
        base.setColorAt(0.46, with_opacity(QColor(11, 18, 26), opacity));
        base.setColorAt(1.0, with_opacity(QColor(28, 18, 28), opacity));
    }
    painter.fillRect(QRect(0, 0, canvas_width, canvas_height), base);

    if (blur_radius > 0) {
        const int highlight_alpha = std::min(72, 24 + blur_radius / 3);
        QRadialGradient teal_glow(QPointF(canvas_width * 0.22, canvas_height * 0.18),
                                  canvas_width * 0.34);
        teal_glow.setColorAt(
            0.0, cyber ? layered_color(primary, highlight_alpha, opacity)
                       : layered_color(83, 220, 203, highlight_alpha, opacity));
        teal_glow.setColorAt(1.0, cyber ? QColor(primary.red(), primary.green(),
                                                 primary.blue(), 0)
                                        : QColor(83, 220, 203, 0));
        painter.fillRect(QRect(0, 0, canvas_width, canvas_height), teal_glow);

        QRadialGradient pink_glow(QPointF(canvas_width * 0.82, canvas_height * 0.82),
                                  canvas_width * 0.42);
        pink_glow.setColorAt(
            0.0, cyber ? layered_color(secondary, highlight_alpha - 6, opacity)
                       : layered_color(211, 76, 112, highlight_alpha - 6, opacity));
        pink_glow.setColorAt(1.0, cyber ? QColor(secondary.red(), secondary.green(),
                                                 secondary.blue(), 0)
                                        : QColor(211, 76, 112, 0));
        painter.fillRect(QRect(0, 0, canvas_width, canvas_height), pink_glow);
    }

    if (!cyber && blur_radius > 0) {
        const int line_alpha = std::min(18, 7 + blur_radius / 3);
        QPen dark_pen(layered_color(83, 220, 203, line_alpha, opacity));
        dark_pen.setWidthF(1.0);
        painter.setPen(dark_pen);
        for (int x = -canvas_height; x < canvas_width + canvas_height; x += 118) {
            painter.drawLine(QPointF(x, 0), QPointF(x + canvas_height * 0.24, canvas_height));
        }
    }
    if (!cyber) {
        return;
    }

    QPainterPath teal_band;
    teal_band.moveTo(canvas_width * 0.03, -canvas_height * 0.08);
    teal_band.lineTo(canvas_width * 0.27, -canvas_height * 0.08);
    teal_band.lineTo(canvas_width * 0.49, canvas_height * 1.08);
    teal_band.lineTo(canvas_width * 0.27, canvas_height * 1.08);
    teal_band.closeSubpath();
    QLinearGradient teal_light(canvas_width * 0.03, 0, canvas_width * 0.49, 0);
    teal_light.setColorAt(0.0, QColor(primary.red(), primary.green(), primary.blue(), 0));
    teal_light.setColorAt(0.5, layered_color(primary, 13, opacity));
    teal_light.setColorAt(1.0, QColor(primary.red(), primary.green(), primary.blue(), 0));
    painter.fillPath(teal_band, teal_light);

    QPainterPath pink_band;
    pink_band.moveTo(canvas_width * 0.70, -canvas_height * 0.08);
    pink_band.lineTo(canvas_width * 0.88, -canvas_height * 0.08);
    pink_band.lineTo(canvas_width * 0.69, canvas_height * 1.08);
    pink_band.lineTo(canvas_width * 0.51, canvas_height * 1.08);
    pink_band.closeSubpath();
    QLinearGradient pink_light(canvas_width * 0.51, 0, canvas_width * 0.88, 0);
    pink_light.setColorAt(0.0, QColor(secondary.red(), secondary.green(),
                                      secondary.blue(), 0));
    pink_light.setColorAt(0.5, layered_color(secondary, 10, opacity));
    pink_light.setColorAt(1.0, QColor(secondary.red(), secondary.green(),
                                      secondary.blue(), 0));
    painter.fillPath(pink_band, pink_light);

    const int line_alpha = std::min(34, 16 + blur_radius / 2);
    const int cyber_line_alpha =
        blur_radius > 0 ? line_alpha : std::max(12, line_alpha - 8);
    QPen teal_pen(layered_color(primary, cyber_line_alpha, opacity));
    teal_pen.setWidthF(1.0);
    painter.setPen(teal_pen);
    for (int x = -canvas_height; x < canvas_width + canvas_height; x += 86) {
        painter.drawLine(QPointF(x, 0), QPointF(x + canvas_height * 0.32, canvas_height));
    }

    QPen pink_pen(layered_color(secondary, std::max(7, cyber_line_alpha - 8), opacity));
    pink_pen.setWidthF(1.0);
    painter.setPen(pink_pen);
    for (int x = canvas_width / 2; x < canvas_width + canvas_height; x += 110) {
        painter.drawLine(QPointF(x, 0), QPointF(x - canvas_height * 0.20, canvas_height));
    }

    QLinearGradient surface_light(0, 0, 0, canvas_height);
    if (light_background) {
        surface_light.setColorAt(0.0, layered_color(primary, 10, opacity));
        surface_light.setColorAt(
            0.28, QColor(primary.red(), primary.green(), primary.blue(), 0));
        surface_light.setColorAt(1.0, layered_color(secondary, 12, opacity));
    } else {
        surface_light.setColorAt(0.0, layered_color(255, 255, 255, 12, opacity));
        surface_light.setColorAt(0.28, QColor(255, 255, 255, 0));
        surface_light.setColorAt(1.0, layered_color(0, 0, 0, 22, opacity));
    }
    painter.fillRect(QRect(0, 0, canvas_width, canvas_height), surface_light);
}

} // namespace

namespace neothemis::gui {

ThemeBackground::ThemeBackground(QWidget* parent) : QWidget(parent) {
    setObjectName("ThemeBackground");
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setAutoFillBackground(false);
}

void ThemeBackground::set_appearance(const std::string& theme,
                                     int opacity,
                                     int blur_radius,
                                     const CyberThemeColors& cyber_colors) {
    const int clamped_opacity = std::clamp(opacity, 0, 255);
    const int clamped_blur = std::clamp(blur_radius, 0, 240);
    const CyberThemeColors defaults = default_cyber_theme_colors();
    const CyberThemeColors resolved_colors{
        usable_color(cyber_colors.background, defaults.background),
        usable_color(cyber_colors.primary, defaults.primary),
        usable_color(cyber_colors.secondary, defaults.secondary),
        usable_color(cyber_colors.text, defaults.text),
        usable_color(cyber_colors.muted_text, defaults.muted_text),
        usable_color(cyber_colors.primary_text, defaults.primary_text),
        usable_color(cyber_colors.secondary_text, defaults.secondary_text)};
    if (theme_ == theme && opacity_ == clamped_opacity &&
        blur_radius_ == clamped_blur &&
        cyber_colors_.background == resolved_colors.background &&
        cyber_colors_.primary == resolved_colors.primary &&
        cyber_colors_.secondary == resolved_colors.secondary &&
        cyber_colors_.text == resolved_colors.text &&
        cyber_colors_.muted_text == resolved_colors.muted_text &&
        cyber_colors_.primary_text == resolved_colors.primary_text &&
        cyber_colors_.secondary_text == resolved_colors.secondary_text) {
        return;
    }
    theme_ = theme;
    opacity_ = clamped_opacity;
    blur_radius_ = clamped_blur;
    cyber_colors_ = resolved_colors;
    update();
}

void ThemeBackground::paintEvent(QPaintEvent* event) {
    (void)event;
    if (width() <= 0 || height() <= 0) {
        return;
    }
    QPainter painter(this);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    paint_background(painter, width(), height(), theme_, opacity_, blur_radius_,
                     cyber_colors_);
}

} // namespace neothemis::gui
