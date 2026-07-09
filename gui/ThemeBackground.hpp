#pragma once

#include <QPixmap>
#include <QWidget>

#include <string>

namespace neothemis::gui {

class ThemeBackground final : public QWidget {
public:
    explicit ThemeBackground(QWidget* parent = nullptr);

    void set_appearance(const std::string& theme, int opacity, int blur_radius);
    void set_desktop_backdrop(const QPixmap& backdrop,
                              const QPoint& virtual_desktop_origin,
                              int scale);
    void freeze_desktop_backdrop_alignment();
    void unfreeze_desktop_backdrop_alignment();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::string theme_ = "dark";
    int opacity_ = 255;
    int blur_radius_ = 0;
    QPixmap desktop_backdrop_;
    QPoint virtual_desktop_origin_;
    int desktop_backdrop_scale_ = 1;
    bool desktop_backdrop_alignment_frozen_ = false;
    QPoint frozen_desktop_backdrop_source_;
};

} // namespace neothemis::gui
