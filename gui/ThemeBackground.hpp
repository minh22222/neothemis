#pragma once

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
};

} // namespace neothemis::gui
