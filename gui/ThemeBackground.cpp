#include "ThemeBackground.hpp"

#include <QLinearGradient>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

namespace {

QColor with_opacity(QColor color, int opacity) {
    color.setAlpha(opacity);
    return color;
}

QColor layered_color(int red, int green, int blue, int alpha, int opacity) {
    return QColor(red, green, blue, alpha * opacity / 255);
}

} // namespace

namespace neothemis::gui {

ThemeBackground::ThemeBackground(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
}

void ThemeBackground::set_appearance(const std::string& theme, int opacity) {
    theme_ = theme;
    opacity_ = std::clamp(opacity, 0, 255);
    update();
}

void ThemeBackground::paintEvent(QPaintEvent* event) {
    (void)event;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QLinearGradient base(0, 0, width(), height());
    if (theme_ == "glassy-dark") {
        base.setColorAt(0.0, with_opacity(QColor(13, 43, 52), opacity_));
        base.setColorAt(0.30, with_opacity(QColor(11, 24, 33), opacity_));
        base.setColorAt(0.62, with_opacity(QColor(27, 20, 32), opacity_));
        base.setColorAt(0.84, with_opacity(QColor(43, 21, 35), opacity_));
        base.setColorAt(1.0, with_opacity(QColor(25, 13, 23), opacity_));
    } else {
        base.setColorAt(0.0, with_opacity(QColor(16, 35, 44), opacity_));
        base.setColorAt(0.46, with_opacity(QColor(11, 18, 26), opacity_));
        base.setColorAt(1.0, with_opacity(QColor(28, 18, 28), opacity_));
    }
    painter.fillRect(rect(), base);

    if (theme_ != "glassy-dark") {
        return;
    }

    QPainterPath teal_band;
    teal_band.moveTo(width() * 0.03, -height() * 0.08);
    teal_band.lineTo(width() * 0.27, -height() * 0.08);
    teal_band.lineTo(width() * 0.49, height() * 1.08);
    teal_band.lineTo(width() * 0.27, height() * 1.08);
    teal_band.closeSubpath();
    QLinearGradient teal_light(width() * 0.03, 0, width() * 0.49, 0);
    teal_light.setColorAt(0.0, QColor(83, 220, 203, 0));
    teal_light.setColorAt(0.5, layered_color(83, 220, 203, 13, opacity_));
    teal_light.setColorAt(1.0, QColor(83, 220, 203, 0));
    painter.fillPath(teal_band, teal_light);

    QPainterPath pink_band;
    pink_band.moveTo(width() * 0.70, -height() * 0.08);
    pink_band.lineTo(width() * 0.88, -height() * 0.08);
    pink_band.lineTo(width() * 0.69, height() * 1.08);
    pink_band.lineTo(width() * 0.51, height() * 1.08);
    pink_band.closeSubpath();
    QLinearGradient pink_light(width() * 0.51, 0, width() * 0.88, 0);
    pink_light.setColorAt(0.0, QColor(211, 76, 112, 0));
    pink_light.setColorAt(0.5, layered_color(211, 76, 112, 10, opacity_));
    pink_light.setColorAt(1.0, QColor(211, 76, 112, 0));
    painter.fillPath(pink_band, pink_light);

    QLinearGradient surface_light(0, 0, 0, height());
    surface_light.setColorAt(0.0, layered_color(255, 255, 255, 12, opacity_));
    surface_light.setColorAt(0.28, QColor(255, 255, 255, 0));
    surface_light.setColorAt(1.0, layered_color(0, 0, 0, 22, opacity_));
    painter.fillRect(rect(), surface_light);
}

} // namespace neothemis::gui
