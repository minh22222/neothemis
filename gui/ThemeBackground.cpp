#include "ThemeBackground.hpp"

#include <QImage>
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

bool is_cyber_theme(const std::string& theme) {
    return theme == "cyber" || theme == "glassy-dark";
}

void paint_background(QPainter& painter,
                      int canvas_width,
                      int canvas_height,
                      const std::string& theme,
                      int opacity,
                      int blur_radius) {
    painter.setRenderHint(QPainter::Antialiasing, true);

    const bool cyber = is_cyber_theme(theme);
    QLinearGradient base(0, 0, canvas_width, canvas_height);
    if (cyber) {
        base.setColorAt(0.0, with_opacity(QColor(13, 43, 52), opacity));
        base.setColorAt(0.30, with_opacity(QColor(11, 24, 33), opacity));
        base.setColorAt(0.62, with_opacity(QColor(27, 20, 32), opacity));
        base.setColorAt(0.84, with_opacity(QColor(43, 21, 35), opacity));
        base.setColorAt(1.0, with_opacity(QColor(25, 13, 23), opacity));
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
        teal_glow.setColorAt(0.0, layered_color(83, 220, 203, highlight_alpha, opacity));
        teal_glow.setColorAt(1.0, QColor(83, 220, 203, 0));
        painter.fillRect(QRect(0, 0, canvas_width, canvas_height), teal_glow);

        QRadialGradient pink_glow(QPointF(canvas_width * 0.82, canvas_height * 0.82),
                                  canvas_width * 0.42);
        pink_glow.setColorAt(0.0, layered_color(211, 76, 112, highlight_alpha - 6, opacity));
        pink_glow.setColorAt(1.0, QColor(211, 76, 112, 0));
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
    teal_light.setColorAt(0.0, QColor(83, 220, 203, 0));
    teal_light.setColorAt(0.5, layered_color(83, 220, 203, 13, opacity));
    teal_light.setColorAt(1.0, QColor(83, 220, 203, 0));
    painter.fillPath(teal_band, teal_light);

    QPainterPath pink_band;
    pink_band.moveTo(canvas_width * 0.70, -canvas_height * 0.08);
    pink_band.lineTo(canvas_width * 0.88, -canvas_height * 0.08);
    pink_band.lineTo(canvas_width * 0.69, canvas_height * 1.08);
    pink_band.lineTo(canvas_width * 0.51, canvas_height * 1.08);
    pink_band.closeSubpath();
    QLinearGradient pink_light(canvas_width * 0.51, 0, canvas_width * 0.88, 0);
    pink_light.setColorAt(0.0, QColor(211, 76, 112, 0));
    pink_light.setColorAt(0.5, layered_color(211, 76, 112, 10, opacity));
    pink_light.setColorAt(1.0, QColor(211, 76, 112, 0));
    painter.fillPath(pink_band, pink_light);

    const int line_alpha = std::min(34, 16 + blur_radius / 2);
    QPen teal_pen(layered_color(83, 220, 203, line_alpha, opacity));
    teal_pen.setWidthF(1.0);
    painter.setPen(teal_pen);
    for (int x = -canvas_height; x < canvas_width + canvas_height; x += 86) {
        painter.drawLine(QPointF(x, 0), QPointF(x + canvas_height * 0.32, canvas_height));
    }

    QPen pink_pen(layered_color(211, 76, 112, std::max(8, line_alpha - 6), opacity));
    pink_pen.setWidthF(1.0);
    painter.setPen(pink_pen);
    for (int x = canvas_width / 2; x < canvas_width + canvas_height; x += 110) {
        painter.drawLine(QPointF(x, 0), QPointF(x - canvas_height * 0.20, canvas_height));
    }

    QLinearGradient surface_light(0, 0, 0, canvas_height);
    surface_light.setColorAt(0.0, layered_color(255, 255, 255, 12, opacity));
    surface_light.setColorAt(0.28, QColor(255, 255, 255, 0));
    surface_light.setColorAt(1.0, layered_color(0, 0, 0, 22, opacity));
    painter.fillRect(QRect(0, 0, canvas_width, canvas_height), surface_light);
}

} // namespace

namespace neothemis::gui {

ThemeBackground::ThemeBackground(QWidget* parent) : QWidget(parent) {
    setObjectName("ThemeBackground");
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
}

void ThemeBackground::set_appearance(const std::string& theme, int opacity, int blur_radius) {
    const int clamped_opacity = std::clamp(opacity, 0, 255);
    const int clamped_blur = std::clamp(blur_radius, 0, 120);
    if (theme_ == theme && opacity_ == clamped_opacity && blur_radius_ == clamped_blur) {
        return;
    }
    theme_ = theme;
    opacity_ = clamped_opacity;
    blur_radius_ = clamped_blur;
    cached_blurred_background_ = QImage();
    update();
}

void ThemeBackground::paintEvent(QPaintEvent* event) {
    (void)event;
    if (width() <= 0 || height() <= 0) {
        return;
    }
    QPainter painter(this);

    if (blur_radius_ <= 0) {
        paint_background(painter, width(), height(), theme_, opacity_, 0);
        return;
    }

    const bool cache_valid =
        !cached_blurred_background_.isNull() &&
        cached_size_ == size() &&
        cached_theme_ == theme_ &&
        cached_opacity_ == opacity_ &&
        cached_blur_radius_ == blur_radius_;
    if (!cache_valid) {
        QImage source(size(), QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::transparent);

        QPainter source_painter(&source);
        paint_background(source_painter, width(), height(), theme_, opacity_, blur_radius_);
        source_painter.end();

        const int divisor = std::clamp(2 + blur_radius_ / 3, 2, 42);
        const QSize blur_size(std::max(1, width() / divisor),
                              std::max(1, height() / divisor));
        QImage downsampled =
            source.scaled(blur_size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        cached_blurred_background_ =
            downsampled.scaled(size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        cached_size_ = size();
        cached_theme_ = theme_;
        cached_opacity_ = opacity_;
        cached_blur_radius_ = blur_radius_;
    }

    painter.drawImage(rect(), cached_blurred_background_);
}

} // namespace neothemis::gui
