#include "MainWindowPrivate.hpp"

#include <QColorDialog>
#include <QGridLayout>

namespace {

QColor blend_theme_colors(const QColor &background, const QColor &primary,
                          const QColor &secondary, double primary_weight,
                          double secondary_weight) {
  const double primary_part = std::clamp(primary_weight, 0.0, 1.0);
  const double secondary_part =
      std::clamp(secondary_weight, 0.0, 1.0 - primary_part);
  const double background_part = 1.0 - primary_part - secondary_part;
  return QColor(
      std::clamp(static_cast<int>(background.red() * background_part +
                                  primary.red() * primary_part +
                                  secondary.red() * secondary_part),
                 0, 255),
      std::clamp(static_cast<int>(background.green() * background_part +
                                  primary.green() * primary_part +
                                  secondary.green() * secondary_part),
                 0, 255),
      std::clamp(static_cast<int>(background.blue() * background_part +
                                  primary.blue() * primary_part +
                                  secondary.blue() * secondary_part),
                 0, 255));
}

} // namespace

namespace neothemis::gui {

bool MainWindow::background_transparency_active() const {
  return transparent_background_ && background_transparency_ > 0;
}

bool MainWindow::background_blur_active() const {
  return background_transparency_active() && blur_background_ &&
         neothemis::gui::native_background_blur_supported();
}

neothemis::gui::CyberThemeColors MainWindow::cyber_theme_colors() const {
  return {cyber_background_color_,    cyber_primary_color_,
          cyber_secondary_color_,     cyber_text_color_,
          cyber_muted_text_color_,    cyber_primary_text_color_,
          cyber_secondary_text_color_};
}

QColor MainWindow::color_setting_or_default(const QVariant &value,
                                            const QColor &fallback) {
  const QColor color(value.toString());
  return color.isValid() ? color : fallback;
}

void MainWindow::apply_language_to_main_window() {
  setWindowTitle(text("window_title"));
  if (contest_root_.empty() && contest_title_) {
    contest_title_->setText(text("no_contest_open"));
  }
  if (judge_group_) {
    judge_group_->setTitle(text("judge"));
  }
  if (judge_selected_button_) {
    judge_selected_button_->setText(text("judge_selected"));
  }
  if (judge_all_button_) {
    judge_all_button_->setText(text("judge_all"));
  }
  if (stop_button_) {
    stop_button_->setText(text("stop"));
  }
  if (detail_view_button_) {
    detail_view_button_->setText(text("detail_view"));
  }
  update_judge_elapsed_label();
  if (server_start_action_) {
    server_start_action_->setText(text("start_local_server"));
  }
  if (server_stop_action_) {
    server_stop_action_->setText(text("stop_local_server"));
  }
  if (detail_dialog_) {
    detail_dialog_->setWindowTitle(text("judge_details"));
  }
  if (core_tasks_table_) {
    core_tasks_table_->setHorizontalHeaderLabels({text("core"), text("task")});
  }
  build_toolbar();
  if (!contestants_.empty() || !problems_.empty()) {
    populate_table();
  }
}

void MainWindow::add_soft_shadow(QWidget *widget, qreal blur_radius,
                                 const QColor &color) {
  if (!widget) {
    return;
  }
  auto *shadow = new QGraphicsDropShadowEffect(widget);
  shadow->setBlurRadius(blur_radius);
  shadow->setOffset(0, 12);
  shadow->setColor(color);
  widget->setGraphicsEffect(shadow);
}

void MainWindow::apply_translucent_surface_attributes() {
#ifndef Q_OS_WIN
  setAttribute(Qt::WA_TranslucentBackground, true);
#endif
  setAttribute(Qt::WA_NoSystemBackground, true);
  setAutoFillBackground(false);

  if (QWidget *root = centralWidget()) {
    root->setAttribute(Qt::WA_NoSystemBackground, true);
#ifndef Q_OS_WIN
    root->setAttribute(Qt::WA_TranslucentBackground, true);
#endif
    root->setAutoFillBackground(false);
  }
  if (background_layer_) {
    background_layer_->setAttribute(Qt::WA_NoSystemBackground, true);
#ifndef Q_OS_WIN
    background_layer_->setAttribute(Qt::WA_TranslucentBackground, true);
#endif
    background_layer_->setAutoFillBackground(false);
  }
}

void MainWindow::apply_selected_theme() {
  const bool cyber = theme_ == "cyber";
  const bool transparent = background_transparency_active();
  const neothemis::gui::CyberThemeColors cyber_colors = cyber_theme_colors();
  apply_translucent_surface_attributes();
  neothemis::gui::apply_application_theme(theme_, cyber_colors);
  const bool native_blur_active = apply_native_backdrop(false);
  int opacity =
      transparent ? 255 * (100 - background_transparency_) / 100 : 255;
  if (native_blur_active) {
    opacity =
        opacity * (100 - std::min(55, kBackgroundBlurVisualStrength / 4)) / 100;
  }
  if (background_layer_) {
    background_layer_->set_appearance(
        theme_, opacity, native_blur_active ? kBackgroundBlurVisualStrength : 0,
        cyber_colors);
  }
  const bool light_cyber = cyber_colors.background.lightness() > 170;
  QColor cyber_table_shadow =
      light_cyber
          ? blend_theme_colors(cyber_colors.background, cyber_colors.primary,
                               cyber_colors.secondary, 0.08, 0.18)
          : cyber_colors.secondary.darker(260);
  cyber_table_shadow.setAlpha(light_cyber ? 72 : 178);
  QColor cyber_side_shadow =
      light_cyber
          ? blend_theme_colors(cyber_colors.background, cyber_colors.primary,
                               cyber_colors.secondary, 0.16, 0.10)
          : cyber_colors.secondary.darker(230);
  cyber_side_shadow.setAlpha(light_cyber ? 78 : 188);
  add_soft_shadow(table_, cyber ? 56 : 34,
                  cyber ? cyber_table_shadow : QColor(0, 0, 0, 120));
  add_soft_shadow(side_panel_, cyber ? 60 : 36,
                  cyber ? cyber_side_shadow : QColor(0, 0, 0, 135));
  for (std::size_t row = 0; row < contestants_.size(); ++row) {
    if (auto *name = table_->item(static_cast<int>(row), 0)) {
      style_name_item(name);
    }
    for (std::size_t column = 0; column < problems_.size(); ++column) {
      if (auto *item = table_->item(static_cast<int>(row),
                                    static_cast<int>(column + 1))) {
        style_problem_item(contestants_[row], problems_[column], item);
      }
    }
    if (auto *total = table_->item(static_cast<int>(row), total_column())) {
      style_total_item(contestants_[row], total);
    }
  }
}

bool MainWindow::apply_native_backdrop(bool force_compositor_update) {
  const bool transparent = background_transparency_active();
  const bool blurred = transparent && background_blur_active();
  const int tint_opacity = std::clamp(96 - background_transparency_, 20, 96);
  return neothemis::gui::apply_windows_backdrop(
      this, transparent, blurred, cyber_background_color_, tint_opacity,
      force_compositor_update);
}

void MainWindow::schedule_backdrop_startup_passes() {
#ifdef Q_OS_WIN
  // DWM owns the Windows backdrop and tracks movement without app-side
  // polling or repaint passes. showEvent already applied it once.
  return;
#else
  // Native compositors register a newly shown window asynchronously.
  // Reassert the current state after registration and window-rule handling.
  for (int delay_ms : {0, 75, 250, 700}) {
    QTimer::singleShot(delay_ms, this, [this]() {
      if (isVisible()) {
        apply_native_backdrop(true);
        if (background_layer_) {
          background_layer_->repaint();
        }
        repaint();
      }
    });
  }
#endif
}

QWidget *MainWindow::build_visual_tab(QWidget *parent,
                                      SettingsDialog *settings_dialog) {
  auto *tab = new QWidget(parent);
  auto *form = new QFormLayout(tab);
  form->setContentsMargins(18, 18, 18, 18);
  form->setHorizontalSpacing(24);
  form->setVerticalSpacing(14);
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->setFormAlignment(Qt::AlignTop);
  form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
  auto *theme = new QComboBox(tab);
  theme->addItem(text("dark"), "dark");
  theme->addItem(text("cyber"), "cyber");
  int theme_index = theme->findData(QString::fromStdString(theme_));
  if (theme_index >= 0) {
    theme->setCurrentIndex(theme_index);
  }
  auto *cyber_colors_label = new QLabel(text("cyber_colors"), tab);
  auto *cyber_colors_widget = new QWidget(tab);
  cyber_colors_widget->setObjectName("InlineControl");
  auto *cyber_colors_layout = new QGridLayout(cyber_colors_widget);
  cyber_colors_layout->setContentsMargins(0, 0, 0, 0);
  cyber_colors_layout->setSpacing(10);
  auto *cyber_background = new QPushButton(cyber_colors_widget);
  auto *cyber_primary = new QPushButton(cyber_colors_widget);
  auto *cyber_secondary = new QPushButton(cyber_colors_widget);
  auto *reset_cyber_colors =
      new QPushButton(text("reset_cyber_colors"), cyber_colors_widget);
  cyber_colors_layout->addWidget(cyber_background, 0, 0);
  cyber_colors_layout->addWidget(cyber_primary, 0, 1);
  cyber_colors_layout->addWidget(cyber_secondary, 1, 0);
  cyber_colors_layout->addWidget(reset_cyber_colors, 1, 1);
  auto *cyber_text_colors_label = new QLabel(text("cyber_text_colors"), tab);
  auto *cyber_text_colors_widget = new QWidget(tab);
  cyber_text_colors_widget->setObjectName("InlineControl");
  auto *cyber_text_colors_layout = new QGridLayout(cyber_text_colors_widget);
  cyber_text_colors_layout->setContentsMargins(0, 0, 0, 0);
  cyber_text_colors_layout->setSpacing(10);
  auto *cyber_text = new QPushButton(cyber_text_colors_widget);
  auto *cyber_muted_text = new QPushButton(cyber_text_colors_widget);
  auto *cyber_primary_text = new QPushButton(cyber_text_colors_widget);
  auto *cyber_secondary_text = new QPushButton(cyber_text_colors_widget);
  cyber_text_colors_layout->addWidget(cyber_text, 0, 0);
  cyber_text_colors_layout->addWidget(cyber_muted_text, 0, 1);
  cyber_text_colors_layout->addWidget(cyber_primary_text, 1, 0);
  cyber_text_colors_layout->addWidget(cyber_secondary_text, 1, 1);
  auto color_text = [](const QColor &background) {
    const int luminance = (background.red() * 299 + background.green() * 587 +
                           background.blue() * 114) /
                          1000;
    return luminance > 150 ? QColor(12, 17, 26) : QColor(244, 255, 252);
  };
  auto set_color_button = [color_text](QPushButton *button,
                                       const QString &label,
                                       const QColor &color) {
    button->setProperty("selectedColor", color);
    button->setText(label + ": " + color.name(QColor::HexRgb).toUpper());
    button->setMinimumWidth(150);
    button->setStyleSheet(QString("QPushButton {"
                                  "background: %1;"
                                  "color: %2;"
                                  "border: 1px solid rgba(255, 255, 255, 96);"
                                  "border-radius: 8px;"
                                  "padding: 8px 12px;"
                                  "font-weight: 800;"
                                  "}"
                                  "QPushButton:hover {"
                                  "border: 1px solid rgba(255, 255, 255, 190);"
                                  "}")
                              .arg(color.name(QColor::HexRgb),
                                   color_text(color).name(QColor::HexRgb)));
  };
  auto selected_color = [](const QPushButton *button) {
    const QVariant value = button->property("selectedColor");
    const QColor color = value.value<QColor>();
    return color.isValid() ? color : QColor();
  };
  set_color_button(cyber_background, text("cyber_background_color"),
                   cyber_background_color_);
  set_color_button(cyber_primary, text("cyber_primary_color"),
                   cyber_primary_color_);
  set_color_button(cyber_secondary, text("cyber_secondary_color"),
                   cyber_secondary_color_);
  set_color_button(cyber_text, text("cyber_text_color"), cyber_text_color_);
  set_color_button(cyber_muted_text, text("cyber_muted_text_color"),
                   cyber_muted_text_color_);
  set_color_button(cyber_primary_text, text("cyber_primary_text_color"),
                   cyber_primary_text_color_);
  set_color_button(cyber_secondary_text, text("cyber_secondary_text_color"),
                   cyber_secondary_text_color_);
  auto *transparent_background = new QCheckBox(tab);
  transparent_background->setChecked(transparent_background_);
  auto *transparency_widget = new QWidget(tab);
  transparency_widget->setObjectName("InlineControl");
  auto *transparency_layout = new QHBoxLayout(transparency_widget);
  transparency_layout->setContentsMargins(0, 0, 0, 0);
  transparency_layout->setSpacing(10);
  auto *transparency = new QSlider(Qt::Horizontal, transparency_widget);
  transparency->setRange(0, 95);
  transparency->setValue(background_transparency_);
  transparency->setEnabled(transparent_background_);
  auto *transparency_value = new QLabel(
      QString::number(background_transparency_) + "%", transparency_widget);
  transparency_value->setFixedWidth(42);
  transparency_value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  transparency_layout->addWidget(transparency, 1);
  transparency_layout->addWidget(transparency_value);

  auto *blur_background = new QCheckBox(tab);
  const bool blur_supported =
      neothemis::gui::native_background_blur_supported();
  const bool transparency_active =
      transparent_background_ && background_transparency_ > 0;
  blur_background->setChecked(blur_supported && transparency_active &&
                              blur_background_);
  blur_background->setEnabled(blur_supported && transparency_active);
  blur_background->setVisible(blur_supported);

  auto *language = new QComboBox(tab);
  language->addItem(text("english"), "en");
  language->addItem(text("vietnamese"), "vi");
  int language_index = language->findData(QString::fromStdString(language_));
  if (language_index >= 0) {
    language->setCurrentIndex(language_index);
  }
  auto *temp_widget = new QWidget(tab);
  auto *temp_layout = new QHBoxLayout(temp_widget);
  temp_layout->setContentsMargins(0, 0, 0, 0);
  temp_layout->setSpacing(10);
  auto *temp_dir = new QLineEdit(
      QString::fromStdString(temporary_dir_.string()), temp_widget);
  auto *browse_temp = new QPushButton(text("browse"), temp_widget);
  temp_layout->addWidget(temp_dir, 1);
  temp_layout->addWidget(browse_temp);
  auto *sandbox_enabled = new QCheckBox(tab);
  sandbox_enabled->setChecked(sandbox_enabled_);
  auto *save = new QPushButton(text("save_application_settings"), tab);
  auto *association = new QPushButton(tab);
  const bool association_registered =
      neothemis::gui::contest_file_association_is_registered();
  association->setText(text(association_registered
                                ? "file_association_registered"
                                : "register_file_association"));
  association->setEnabled(!association_registered);

  form->addRow(text("theme"), theme);
  form->addRow(cyber_colors_label, cyber_colors_widget);
  form->addRow(cyber_text_colors_label, cyber_text_colors_widget);
  form->addRow(text("transparent_background"), transparent_background);
  form->addRow(text("background_transparency"), transparency_widget);
  if (blur_supported) {
    form->addRow(text("blur_background"), blur_background);
  }
  form->addRow(text("language"), language);
  form->addRow(text("temporary_dir"), temp_widget);
  form->addRow(text("sandbox_enabled"), sandbox_enabled);
  form->addRow(text("ncontest_file_association"), association);
  form->addRow(save);

  QObject::connect(browse_temp, &QPushButton::clicked, [this, temp_dir]() {
    QString dir = QFileDialog::getExistingDirectory(this, text("temporary_dir"),
                                                    temp_dir->text());
    if (!dir.isEmpty()) {
      temp_dir->setText(dir);
    }
  });

  auto apply_visual_preview = [this, theme, transparent_background,
                               transparency, blur_background, cyber_background,
                               cyber_primary, cyber_secondary, cyber_text,
                               cyber_muted_text, cyber_primary_text,
                               cyber_secondary_text, selected_color]() {
    theme_ = theme->currentData().toString().toStdString();
    const QColor background = selected_color(cyber_background);
    const QColor primary = selected_color(cyber_primary);
    const QColor secondary = selected_color(cyber_secondary);
    const QColor body_text = selected_color(cyber_text);
    const QColor muted_text = selected_color(cyber_muted_text);
    const QColor primary_text = selected_color(cyber_primary_text);
    const QColor secondary_text = selected_color(cyber_secondary_text);
    if (background.isValid()) {
      cyber_background_color_ = background;
    }
    if (primary.isValid()) {
      cyber_primary_color_ = primary;
    }
    if (secondary.isValid()) {
      cyber_secondary_color_ = secondary;
    }
    if (body_text.isValid()) {
      cyber_text_color_ = body_text;
    }
    if (muted_text.isValid()) {
      cyber_muted_text_color_ = muted_text;
    }
    if (primary_text.isValid()) {
      cyber_primary_text_color_ = primary_text;
    }
    if (secondary_text.isValid()) {
      cyber_secondary_text_color_ = secondary_text;
    }
    transparent_background_ = transparent_background->isChecked();
    background_transparency_ = transparency->value();
    blur_background_ = blur_background->isChecked();
    apply_selected_theme();
  };
  auto update_cyber_color_visibility =
      [theme, cyber_colors_label, cyber_colors_widget, cyber_text_colors_label,
       cyber_text_colors_widget]() {
        const bool visible =
            theme->currentData().toString().toStdString() == "cyber";
        cyber_colors_label->setVisible(visible);
        cyber_colors_widget->setVisible(visible);
        cyber_text_colors_label->setVisible(visible);
        cyber_text_colors_widget->setVisible(visible);
      };
  update_cyber_color_visibility();

  QObject::connect(
      theme, &QComboBox::currentTextChanged,
      [apply_visual_preview, update_cyber_color_visibility](const QString &) {
        update_cyber_color_visibility();
        apply_visual_preview();
      });
  auto pick_cyber_color = [this, set_color_button, apply_visual_preview](
                              QPushButton *button, const QString &label,
                              const char *title_key) {
    const QColor current = button->property("selectedColor").value<QColor>();
    const QColor chosen = QColorDialog::getColor(
        current.isValid() ? current : QColor(255, 255, 255), this,
        text(title_key), QColorDialog::DontUseNativeDialog);
    if (!chosen.isValid()) {
      return;
    }
    set_color_button(button, label, chosen);
    apply_visual_preview();
  };
  QObject::connect(cyber_background, &QPushButton::clicked,
                   [pick_cyber_color, cyber_background, this]() {
                     pick_cyber_color(cyber_background,
                                      text("cyber_background_color"),
                                      "cyber_background_color");
                   });
  QObject::connect(cyber_primary, &QPushButton::clicked,
                   [pick_cyber_color, cyber_primary, this]() {
                     pick_cyber_color(cyber_primary,
                                      text("cyber_primary_color"),
                                      "cyber_primary_color");
                   });
  QObject::connect(cyber_secondary, &QPushButton::clicked,
                   [pick_cyber_color, cyber_secondary, this]() {
                     pick_cyber_color(cyber_secondary,
                                      text("cyber_secondary_color"),
                                      "cyber_secondary_color");
                   });
  QObject::connect(cyber_text, &QPushButton::clicked,
                   [pick_cyber_color, cyber_text, this]() {
                     pick_cyber_color(cyber_text, text("cyber_text_color"),
                                      "cyber_text_color");
                   });
  QObject::connect(cyber_muted_text, &QPushButton::clicked,
                   [pick_cyber_color, cyber_muted_text, this]() {
                     pick_cyber_color(cyber_muted_text,
                                      text("cyber_muted_text_color"),
                                      "cyber_muted_text_color");
                   });
  QObject::connect(cyber_primary_text, &QPushButton::clicked,
                   [pick_cyber_color, cyber_primary_text, this]() {
                     pick_cyber_color(cyber_primary_text,
                                      text("cyber_primary_text_color"),
                                      "cyber_primary_text_color");
                   });
  QObject::connect(cyber_secondary_text, &QPushButton::clicked,
                   [pick_cyber_color, cyber_secondary_text, this]() {
                     pick_cyber_color(cyber_secondary_text,
                                      text("cyber_secondary_text_color"),
                                      "cyber_secondary_text_color");
                   });
  QObject::connect(
      reset_cyber_colors, &QPushButton::clicked,
      [this, set_color_button, cyber_background, cyber_primary, cyber_secondary,
       cyber_text, cyber_muted_text, cyber_primary_text, cyber_secondary_text,
       apply_visual_preview]() {
        const neothemis::gui::CyberThemeColors defaults =
            neothemis::gui::default_cyber_theme_colors();
        set_color_button(cyber_background, text("cyber_background_color"),
                         defaults.background);
        set_color_button(cyber_primary, text("cyber_primary_color"),
                         defaults.primary);
        set_color_button(cyber_secondary, text("cyber_secondary_color"),
                         defaults.secondary);
        set_color_button(cyber_text, text("cyber_text_color"), defaults.text);
        set_color_button(cyber_muted_text, text("cyber_muted_text_color"),
                         defaults.muted_text);
        set_color_button(cyber_primary_text, text("cyber_primary_text_color"),
                         defaults.primary_text);
        set_color_button(cyber_secondary_text,
                         text("cyber_secondary_text_color"),
                         defaults.secondary_text);
        apply_visual_preview();
      });
  QObject::connect(transparent_background, &QCheckBox::toggled,
                   [transparency, blur_background, blur_supported,
                    apply_visual_preview](bool enabled) {
                     transparency->setEnabled(enabled);
                     blur_background->setEnabled(blur_supported && enabled &&
                                                 transparency->value() > 0);
                     if (!enabled) {
                       blur_background->setChecked(false);
                     }
                     apply_visual_preview();
                   });
  QObject::connect(transparency, &QSlider::valueChanged,
                   [transparent_background, transparency_value, blur_background,
                    blur_supported, apply_visual_preview](int value) {
                     transparency_value->setText(QString::number(value) + "%");
                     const bool can_blur =
                         blur_supported &&
                         transparent_background->isChecked() && value > 0;
                     blur_background->setEnabled(can_blur);
                     if (!can_blur) {
                       blur_background->setChecked(false);
                     }
                     apply_visual_preview();
                   });
  QObject::connect(blur_background, &QCheckBox::toggled,
                   [apply_visual_preview](bool) { apply_visual_preview(); });

  QObject::connect(
      sandbox_enabled, &QCheckBox::toggled,
      [this, sandbox_enabled](bool enabled) {
        if (enabled) {
          return;
        }
        QMessageBox confirmation(this);
        confirmation.setIcon(QMessageBox::Critical);
        confirmation.setWindowTitle(text("disable_sandbox_confirmation_title"));
        confirmation.setText(text("disable_sandbox_confirmation"));
        QPushButton *disable = confirmation.addButton(
            text("disable_sandbox"), QMessageBox::DestructiveRole);
        disable->setObjectName("DangerButton");
        QPushButton *cancel =
            confirmation.addButton(text("cancel"), QMessageBox::RejectRole);
        confirmation.setDefaultButton(cancel);
        confirmation.exec();
        if (confirmation.clickedButton() != disable) {
          sandbox_enabled->setChecked(true);
        }
      });

  auto persist = [this, theme, transparent_background, transparency,
                  blur_background, language, temp_dir, sandbox_enabled,
                  cyber_background, cyber_primary, cyber_secondary, cyber_text,
                  cyber_muted_text, cyber_primary_text, cyber_secondary_text,
                  selected_color](bool notify) {
    try {
      theme_ = theme->currentData().toString().toStdString();
      const QColor background = selected_color(cyber_background);
      const QColor primary = selected_color(cyber_primary);
      const QColor secondary = selected_color(cyber_secondary);
      const QColor body_text = selected_color(cyber_text);
      const QColor muted_text = selected_color(cyber_muted_text);
      const QColor primary_text = selected_color(cyber_primary_text);
      const QColor secondary_text = selected_color(cyber_secondary_text);
      if (background.isValid()) {
        cyber_background_color_ = background;
      }
      if (primary.isValid()) {
        cyber_primary_color_ = primary;
      }
      if (secondary.isValid()) {
        cyber_secondary_color_ = secondary;
      }
      if (body_text.isValid()) {
        cyber_text_color_ = body_text;
      }
      if (muted_text.isValid()) {
        cyber_muted_text_color_ = muted_text;
      }
      if (primary_text.isValid()) {
        cyber_primary_text_color_ = primary_text;
      }
      if (secondary_text.isValid()) {
        cyber_secondary_text_color_ = secondary_text;
      }
      transparent_background_ = transparent_background->isChecked();
      background_transparency_ = transparency->value();
      blur_background_ = blur_background->isChecked();
      language_ = language->currentData().toString().toStdString();
      sandbox_enabled_ = sandbox_enabled->isChecked();
      temporary_dir_ = temp_dir->text().toStdString();
      if (temporary_dir_.empty()) {
        temporary_dir_ = default_temporary_dir();
      }
      fs::create_directories(temporary_dir_);
      save_app_settings();
      apply_selected_theme();
      apply_language_to_main_window();
      if (notify) {
        log_->appendPlainText(text("saved_app_settings"));
      }
    } catch (const std::exception &ex) {
      QMessageBox::critical(this, text("save_failed"), ex.what());
    }
  };
  QObject::connect(save, &QPushButton::clicked, [persist]() { persist(true); });
  settings_dialog->add_close_handler([persist]() { persist(false); });
  QObject::connect(association, &QPushButton::clicked, [this, association]() {
    try {
      neothemis::gui::register_contest_file_association();
      association->setText(text("file_association_registered"));
      association->setEnabled(false);
      log_->appendPlainText(text("file_association_registered"));
    } catch (const std::exception &ex) {
      QMessageBox::critical(this, text("file_association_failed"), ex.what());
    }
  });
  return tab;
}

} // namespace neothemis::gui
