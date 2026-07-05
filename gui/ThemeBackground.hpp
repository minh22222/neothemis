#pragma once

#include <QImage>
#include <QSize>
#include <QWidget>

#include <string>

namespace neothemis::gui {

class ThemeBackground final : public QWidget {
public:
    explicit ThemeBackground(QWidget* parent = nullptr);

    void set_appearance(const std::string& theme, int opacity, int blur_radius);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::string theme_ = "dark";
    int opacity_ = 255;
    int blur_radius_ = 0;
    mutable QImage cached_blurred_background_;
    mutable QSize cached_size_;
    mutable std::string cached_theme_;
    mutable int cached_opacity_ = -1;
    mutable int cached_blur_radius_ = -1;
};

} // namespace neothemis::gui
