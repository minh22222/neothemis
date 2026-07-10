#include "Theme.hpp"

#include <QApplication>
#include <QPalette>

#include <algorithm>

namespace {

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

QString color_hex(const QColor& color) {
    return color.name(QColor::HexRgb);
}

QString rgba(QColor color, int alpha) {
    color.setAlpha(std::clamp(alpha, 0, 255));
    return QString("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

void replace_token(QString& text, const QString& token, const QColor& color) {
    text.replace(token, color_hex(color));
}

void replace_token(QString& text, const QString& token, const QString& color) {
    text.replace(token, color);
}

} // namespace

namespace neothemis::gui {

CyberThemeColors default_cyber_theme_colors() {
    return {QColor(13, 43, 52), QColor(83, 220, 203), QColor(211, 76, 112)};
}

void apply_application_theme(const std::string& theme) {
    apply_application_theme(theme, default_cyber_theme_colors());
}

void apply_application_theme(const std::string& theme,
                             const CyberThemeColors& cyber_colors) {
    static const QPalette system_palette = qApp->palette();
    qApp->setPalette(system_palette);
    const bool cyber = theme == "cyber" || theme == "glassy-dark";
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, cyber);
    const CyberThemeColors defaults = default_cyber_theme_colors();
    const QColor background =
        usable_color(cyber_colors.background, defaults.background);
    const QColor primary = usable_color(cyber_colors.primary, defaults.primary);
    const QColor secondary =
        usable_color(cyber_colors.secondary, defaults.secondary);
    const bool light_background = background.lightness() > 170;
    const QColor foreground =
        light_background ? QColor(18, 24, 32) : QColor(237, 243, 247);
    const QColor muted_foreground =
        mix_color(foreground, background, light_background ? 0.30 : 0.38);
    const QColor primary_light = light_background
                                     ? mix_color(background, primary, 0.72)
                                     : mix_color(primary, QColor(255, 255, 255), 0.38);
    const QColor secondary_light = light_background
                                       ? mix_color(background, secondary, 0.72)
                                       : mix_color(secondary, QColor(255, 255, 255), 0.34);
    const QColor background_dark =
        light_background ? mix_color(background, primary, 0.08)
                         : background.darker(210);
    const QColor background_deep =
        light_background ? mix_color(background, secondary, 0.14)
                         : background.darker(310);
    const QColor background_mid =
        light_background
            ? mix_color(background, mix_color(primary, secondary, 0.5), 0.05)
            : background.darker(155);
    const QColor primary_dark = light_background
                                    ? mix_color(background, primary, 0.30)
                                    : primary.darker(210);
    const QColor secondary_dark = light_background
                                      ? mix_color(background, secondary, 0.30)
                                      : secondary.darker(210);
    const QColor primary_deep = light_background
                                    ? mix_color(background, primary, 0.48)
                                    : primary.darker(330);
    const QColor secondary_deep = light_background
                                      ? mix_color(background, secondary, 0.48)
                                      : secondary.darker(330);
    const QColor light_text = light_background
                                  ? foreground
                                  : mix_color(primary, QColor(255, 255, 255), 0.72);
    const QColor secondary_text = light_background
                                      ? foreground
                                      : mix_color(secondary, QColor(255, 255, 255), 0.58);
    QString style_sheet = QString::fromUtf8(R"(
        QWidget {
            background: #090d12;
            color: #edf3f7;
            font-size: 13px;
            selection-background-color: #2a6f72;
            selection-color: #ffffff;
        }
        QMainWindow {
            background: transparent;
        }
        QDialog {
            background: rgba(10, 16, 23, 238);
        }
        QWidget#AppRoot {
            background: transparent;
            border: 1px solid rgba(255, 255, 255, 44);
        }
        QWidget#AppContent, QWidget#ThemeBackground {
            background: transparent;
            border: 0;
        }
        QWidget#InlineControl, QSlider {
            background: transparent;
        }
        QWidget#WindowTitleBar {
            background: rgba(18, 27, 34, 212);
            border-bottom: 1px solid rgba(255, 255, 255, 34);
            min-height: 38px;
        }
        QWidget#MenuRow {
            background: rgba(15, 22, 29, 178);
            border-bottom: 1px solid rgba(84, 211, 194, 50);
            min-height: 34px;
        }
        QLabel#WindowAppName {
            color: #f8fafc; font-size: 14px; font-weight: 800;
            padding-right: 10px;
        }
        QLabel#AppLogo {
            min-width: 28px; min-height: 28px;
        }
        QMenuBar {
            background: transparent; border: 0; padding: 0;
        }
        QMenuBar::item {
            background: transparent;
            padding: 7px 13px;
            border-radius: 8px;
            margin: 2px 1px;
        }
        QMenuBar::item:selected {
            background: rgba(84, 211, 194, 42);
            color: #8ef7e3;
            border: 1px solid rgba(142, 247, 227, 90);
        }
        QToolButton#WindowButton, QToolButton#WindowCloseButton {
            background: transparent;
            border: 0;
            color: #d9e6ec;
            min-width: 46px;
            min-height: 38px;
            border-radius: 0;
            font-weight: 700;
        }
        QToolButton#WindowButton:hover {
            background: rgba(84, 211, 194, 44);
            color: #ffffff;
        }
        QToolButton#WindowCloseButton {
            font-size: 13px;
        }
        QToolButton#WindowCloseButton:hover {
            background: rgba(204, 38, 57, 230);
            color: #ffffff;
        }
        QPushButton {
            background: rgba(24, 35, 43, 206);
            color: #ecfffb;
            border: 1px solid rgba(116, 224, 207, 120);
            border-radius: 8px;
            padding: 9px 12px;
            font-weight: 700;
        }
        QPushButton:hover {
            background: rgba(39, 65, 70, 230);
            border-color: #8ef7e3;
            color: #ffffff;
        }
        QPushButton:pressed { background: rgba(13, 20, 24, 235); border-color: #e86a82; }
        QPushButton:disabled {
            background: rgba(58, 65, 72, 160);
            color: #80909a;
            border-color: rgba(255, 255, 255, 30);
        }
        QPushButton#StopButton {
            background: rgba(76, 28, 42, 212);
            border-color: rgba(255, 112, 137, 145);
        }
        QPushButton#StopButton:hover { background: rgba(111, 34, 54, 230); border-color: #ff8ba1; }
        QPushButton#DangerButton {
            background: rgba(138, 35, 55, 220);
            border: 1px solid rgba(255, 121, 145, 180);
            color: #ffffff;
        }
        QPushButton#DangerButton:hover {
            background: rgba(180, 47, 72, 235);
            border-color: #ff9aae;
        }
        QMenu {
            background: rgba(18, 25, 32, 238);
            border: 1px solid rgba(255, 255, 255, 44);
            border-radius: 10px;
            padding: 6px;
        }
        QMenu::item { padding: 8px 24px; border-radius: 7px; }
        QMenu::item:selected {
            background: rgba(84, 211, 194, 38);
            color: #9dfdec;
        }
        QTableWidget, QPlainTextEdit, QLineEdit, QSpinBox, QComboBox {
            background: rgba(17, 24, 31, 204);
            border: 1px solid rgba(255, 255, 255, 36);
            border-radius: 8px;
            padding: 5px;
            color: #edf3f7;
        }
        QWidget#SidePanel {
            background: rgba(16, 23, 31, 174);
            border: 1px solid rgba(255, 255, 255, 45);
            border-radius: 14px;
        }
        QTableWidget#ScoreTable {
            background: rgba(10, 16, 22, 178);
            alternate-background-color: rgba(21, 31, 38, 192);
            gridline-color: rgba(99, 131, 141, 70);
            selection-background-color: rgba(56, 112, 119, 160);
            border-radius: 14px;
            outline: 0;
        }
        QTableWidget#CoreTasksTable {
            background: rgba(17, 24, 31, 235);
            alternate-background-color: rgba(17, 24, 31, 235);
            gridline-color: transparent;
            outline: 0;
        }
        QTableWidget#CoreTasksTable::item {
            background: rgba(17, 24, 31, 235);
            border: 0;
        }
        QTableWidget#CoreTasksTable::item:selected {
            background: rgba(62, 118, 124, 174);
            border: 0;
            outline: 0;
        }
        QTableWidget#CoreTasksTable::item:focus {
            border: 0;
            outline: 0;
        }
        QPlainTextEdit#LogPanel {
            background: rgba(7, 12, 17, 178);
        }
        QTableWidget::item {
            border-bottom: 1px solid rgba(255, 255, 255, 22);
            padding: 6px;
        }
        QTableWidget#ScoreTable::item {
            padding: 2px 6px;
        }
        QTableWidget::item:selected {
            background: rgba(62, 118, 124, 174);
            color: #ffffff;
            border: 0;
        }
        QTableWidget::item:focus {
            border: 0;
            outline: none;
        }
        QHeaderView::section {
            background: rgba(19, 31, 39, 220);
            color: #9dfdec;
            border: 0;
            border-right: 1px solid rgba(255, 255, 255, 28);
            border-bottom: 1px solid rgba(232, 106, 130, 120);
            padding: 8px;
            font-weight: 700;
        }
        QGroupBox {
            border: 1px solid rgba(255, 255, 255, 42);
            border-radius: 12px;
            margin-top: 10px;
            padding-top: 12px;
            background: rgba(22, 31, 39, 166);
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 12px;
            padding: 0 6px;
            color: #c4fff3;
        }
        QProgressBar {
            background: rgba(7, 12, 17, 190);
            border: 1px solid rgba(255, 255, 255, 42);
            border-radius: 8px;
            min-height: 28px;
            max-height: 28px;
            text-align: center;
        }
        QProgressBar::chunk {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                stop:0 #61d7c7, stop:0.68 #8ef7e3, stop:1 #e86a82);
            border-radius: 7px;
        }
        QLabel#JudgeElapsedLabel {
            color: #9fb3bd;
            padding: 0 4px 2px 4px;
        }
        QSlider::groove:horizontal {
            background: rgba(7, 12, 17, 190);
            border: 1px solid rgba(255, 255, 255, 42);
            border-radius: 3px;
            height: 6px;
        }
        QSlider::sub-page:horizontal {
            background: #61d7c7;
            border-radius: 3px;
        }
        QSlider::handle:horizontal {
            background: #e9fffb;
            border: 2px solid #61d7c7;
            border-radius: 7px;
            width: 14px;
            height: 14px;
            margin: -5px 0;
        }
        QSlider::handle:horizontal:hover {
            border-color: #e86a82;
        }
        QCheckBox {
            background: transparent;
            border: 0;
            padding: 3px 0;
            spacing: 10px;
            min-width: 38px;
            min-height: 18px;
        }
        QCheckBox:hover {
            background: transparent;
            border: 0;
        }
        QCheckBox:checked {
            background: transparent;
            border: 0;
        }
        QCheckBox::indicator {
            width: 16px;
            height: 16px;
            border-radius: 4px;
            background: rgba(5, 10, 14, 198);
            border: 1px solid rgba(255, 255, 255, 96);
        }
        QCheckBox::indicator:hover {
            border-color: #8ef7e3;
        }
        QCheckBox::indicator:checked {
            background: #61d7c7;
            border: 2px solid #eafffb;
        }
        QLabel { color: #d7e3e8; background: transparent; }
        QLabel#ContestTitle { color: #9db0b9; }
        QTabWidget::pane {
            border: 1px solid rgba(255, 255, 255, 42);
            border-radius: 10px;
            background: rgba(13, 19, 26, 164);
        }
        QTabBar::tab {
            background: rgba(24, 32, 39, 184);
            padding: 8px 12px;
            border-top-left-radius: 8px;
            border-top-right-radius: 8px;
            margin-right: 2px;
        }
        QTabBar::tab:selected {
            background: rgba(84, 211, 194, 38);
            color: #9dfdec;
            border: 1px solid rgba(142, 247, 227, 82);
        }
        QScrollBar:vertical, QScrollBar:horizontal {
            background: rgba(7, 12, 17, 120);
            border: 0;
            margin: 0;
        }
        QScrollBar:vertical { width: 10px; }
        QScrollBar:horizontal { height: 10px; }
        QScrollBar::handle { background: rgba(132, 156, 164, 122); border-radius: 5px; }
        QScrollBar::handle:hover { background: rgba(142, 247, 227, 155); }
        QScrollBar::handle:vertical { min-height: 26px; }
        QScrollBar::handle:horizontal { min-width: 26px; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
    )");

    if (cyber) {
        style_sheet += QString::fromUtf8(R"(
            QMainWindow {
                background: transparent;
            }
            QDialog {
                background: rgba(13, 15, 24, 224);
            }
            QWidget#AppRoot {
                background: transparent;
                border: 1px solid rgba(255, 151, 174, 68);
            }
            QWidget#WindowTitleBar {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                    stop:0 rgba(15, 35, 43, 210),
                    stop:0.58 rgba(19, 23, 34, 200),
                    stop:1 rgba(43, 22, 36, 210));
                border-bottom: 1px solid rgba(255, 137, 163, 58);
            }
            QWidget#MenuRow {
                background: rgba(12, 18, 27, 142);
                border-bottom: 1px solid rgba(112, 232, 216, 76);
            }
            QWidget#SidePanel {
                background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                    stop:0 rgba(25, 34, 44, 154),
                    stop:0.55 rgba(17, 22, 31, 132),
                    stop:1 rgba(39, 22, 33, 146));
                border: 1px solid rgba(255, 164, 185, 70);
                border-radius: 8px;
            }
            QTableWidget#ScoreTable {
                background: rgba(7, 15, 22, 138);
                alternate-background-color: rgba(32, 26, 37, 150);
                gridline-color: rgba(255, 147, 173, 36);
                selection-background-color: rgba(96, 58, 79, 184);
                border: 1px solid rgba(128, 238, 220, 66);
                border-radius: 8px;
            }
            QGroupBox {
                background: rgba(17, 25, 34, 128);
                border: 1px solid rgba(255, 159, 182, 60);
                border-radius: 8px;
            }
            QGroupBox::title {
                color: #b8fff2;
            }
            QTableWidget, QPlainTextEdit, QLineEdit, QSpinBox, QComboBox {
                background: rgba(10, 16, 24, 158);
                border: 1px solid rgba(255, 255, 255, 54);
                border-radius: 7px;
            }
            QPlainTextEdit#LogPanel {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                    stop:0 rgba(5, 12, 18, 148),
                    stop:1 rgba(30, 17, 27, 142));
                border-color: rgba(255, 145, 171, 50);
            }
            QPushButton {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                    stop:0 rgba(31, 50, 58, 190),
                    stop:0.72 rgba(32, 33, 45, 186),
                    stop:1 rgba(56, 31, 44, 190));
                border-color: rgba(142, 247, 227, 126);
            }
            QPushButton:hover {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                    stop:0 rgba(45, 81, 81, 222),
                    stop:1 rgba(82, 45, 62, 222));
                border-color: #efa2b4;
            }
            QPushButton:pressed {
                background: rgba(42, 18, 31, 232);
                border-color: #ff7899;
            }
            QMenu {
                background: rgba(18, 20, 30, 226);
                border-color: rgba(255, 143, 171, 88);
            }
            QMenuBar::item:selected, QMenu::item:selected {
                background: rgba(190, 81, 112, 46);
                color: #ffd7e1;
            }
            QHeaderView::section {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                    stop:0 rgba(20, 48, 53, 218),
                    stop:1 rgba(52, 29, 43, 218));
                color: #b8fff2;
                border-bottom-color: rgba(255, 126, 156, 140);
            }
            QProgressBar {
                background: rgba(6, 11, 18, 170);
                border-color: rgba(255, 146, 173, 82);
            }
            QProgressBar::chunk {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                    stop:0 #5dd8ca, stop:0.52 #a1f4e4, stop:0.8 #e78a9f,
                    stop:1 #c85d78);
            }
            QSlider::sub-page:horizontal {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                    stop:0 #61d7c7, stop:1 #d78198);
            }
            QSlider::handle:horizontal {
                background: #f4fffc;
                border-color: #d78198;
            }
            QCheckBox {
                background: transparent;
                border: 0;
            }
            QCheckBox:hover {
                background: transparent;
                border: 0;
            }
            QCheckBox:checked {
                background: transparent;
                border: 0;
            }
            QCheckBox::indicator {
                background: rgba(8, 14, 20, 178);
                border-color: rgba(255, 219, 228, 94);
            }
            QCheckBox::indicator:checked {
                background: #d78198;
                border: 2px solid #f4fffc;
            }
            QTabWidget::pane {
                background: rgba(12, 17, 26, 132);
                border-color: rgba(255, 151, 176, 70);
                border-radius: 8px;
            }
            QTabBar::tab:selected {
                background: rgba(178, 76, 106, 44);
                color: #ffdbe4;
                border-color: rgba(255, 147, 174, 112);
            }
            QScrollBar::handle {
                background: rgba(211, 132, 151, 104);
            }
            QScrollBar::handle:hover {
                background: rgba(139, 242, 224, 168);
            }
        )");
        QString cyber_accent = QString::fromUtf8(R"(
            QWidget {
                background: transparent;
                color: @foreground;
                selection-background-color: @selectionBackground;
                selection-color: @selectionText;
            }
            QLabel {
                background: transparent;
                color: @foreground;
            }
            QLabel#WindowAppName, QMenuBar, QMenuBar::item,
            QToolButton#WindowButton, QToolButton#WindowCloseButton {
                background: transparent;
                color: @foreground;
            }
            QLabel#ContestTitle, QLabel#JudgeElapsedLabel {
                color: @mutedForeground;
            }
            QDialog, QMessageBox, QFileDialog, QColorDialog, QInputDialog {
                background: @dialogBackground;
                color: @foreground;
            }
            QDialogButtonBox, QStackedWidget, QStatusBar, QToolBar {
                background: transparent;
            }
            QWidget#AppRoot {
                border: 1px solid @secondaryBorder;
            }
            QWidget#WindowTitleBar {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                    stop:0 @titlePrimary,
                    stop:0.58 @titleMiddle,
                    stop:1 @titleSecondary);
                border-bottom: 1px solid @secondaryBorder;
            }
            QWidget#MenuRow {
                background: @menuRowBackground;
                border-bottom: 1px solid @primaryMenuBorder;
            }
            QWidget#SidePanel {
                background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                    stop:0 @panelPrimary,
                    stop:0.55 @panelMiddle,
                    stop:1 @panelSecondary);
                border: 1px solid @secondaryPanelBorder;
            }
            QTableWidget#ScoreTable {
                background: @scoreTableBackground;
                alternate-background-color: @tableAlternate;
                gridline-color: @secondaryGrid;
                selection-background-color: @secondarySelection;
                border: 1px solid @primaryTableBorder;
            }
            QGroupBox {
                background: @groupBackground;
                border: 1px solid @secondaryPanelBorder;
            }
            QGroupBox::title {
                color: @primaryText;
            }
            QTableWidget, QPlainTextEdit, QLineEdit, QSpinBox, QComboBox {
                background: @inputBackground;
                color: @foreground;
            }
            QTextEdit, QListView, QTreeView, QDoubleSpinBox, QDateEdit,
            QTimeEdit, QDateTimeEdit, QAbstractItemView {
                background: @inputBackground;
                color: @foreground;
                selection-background-color: @selectionBackground;
                selection-color: @selectionText;
            }
            QComboBox QAbstractItemView {
                background: @popupListBackground;
                color: @foreground;
                border: 1px solid @secondaryMenuBorder;
                outline: 0;
            }
            QTableWidget#CoreTasksTable,
            QTableWidget#CoreTasksTable::item {
                background: @inputBackground;
                color: @foreground;
            }
            QTableWidget#CoreTasksTable::item:selected {
                background: @selectionBackground;
                color: @selectionText;
            }
            QTableWidget::item:selected {
                background: @selectionBackground;
                color: @selectionText;
            }
            QAbstractScrollArea::corner, QTableCornerButton::section {
                background: @inputBackground;
                border: 0;
            }
            QPlainTextEdit#LogPanel {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                    stop:0 @logBackgroundStart,
                    stop:1 @logBackgroundEnd);
            }
            QPushButton {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                    stop:0 @buttonPrimary,
                    stop:0.72 @buttonMiddle,
                    stop:1 @buttonSecondary);
                border-color: @primaryStrongBorder;
                color: @foreground;
            }
            QPushButton:hover, QToolButton#WindowButton:hover {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                    stop:0 @buttonHoverPrimary,
                    stop:1 @buttonHoverSecondary);
                border-color: @secondaryText;
            }
            QPushButton:pressed {
                background: @pressedSecondary;
                border-color: @secondaryLight;
            }
            QPushButton:disabled {
                background: @disabledBackground;
                color: @disabledText;
                border-color: @disabledBorder;
            }
            QPushButton#StopButton, QPushButton#DangerButton {
                background: @dangerBackground;
                border-color: @secondaryStrongBorder;
            }
            QPushButton#StopButton:hover, QPushButton#DangerButton:hover,
            QToolButton#WindowCloseButton:hover {
                background: @dangerHover;
                border-color: @secondaryLight;
                color: @foreground;
            }
            QMenu {
                background: @menuPopupBackground;
                border: 1px solid @secondaryMenuBorder;
                color: @foreground;
            }
            QMenu::item {
                background: transparent;
                color: @foreground;
            }
            QMenu::separator {
                background: @secondaryBorder;
                height: 1px;
                margin: 4px 10px;
            }
            QMenuBar::item:selected, QMenu::item:selected {
                background: @secondarySoft;
                color: @secondaryText;
                border-color: @secondaryBorder;
            }
            QHeaderView::section {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                    stop:0 @headerPrimary,
                    stop:1 @headerSecondary);
                color: @primaryText;
                border-bottom-color: @secondaryStrongBorder;
            }
            QProgressBar {
                background: @progressBackground;
                border-color: @secondaryBorder;
                color: @foreground;
            }
            QSlider::groove:horizontal {
                background: @sliderGroove;
                border-color: @secondaryBorder;
            }
            QProgressBar::chunk {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                    stop:0 @primary,
                    stop:0.52 @primaryLight,
                    stop:0.8 @secondaryLight,
                    stop:1 @secondary);
            }
            QSlider::sub-page:horizontal {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                    stop:0 @primary, stop:1 @secondary);
            }
            QSlider::handle:horizontal {
                background: @sliderHandle;
                border-color: @secondary;
            }
            QSlider::handle:horizontal:hover {
                border-color: @secondaryLight;
            }
            QCheckBox::indicator:hover {
                border-color: @primaryLight;
            }
            QCheckBox::indicator {
                background: @checkboxBackground;
            }
            QCheckBox::indicator:checked {
                background: @secondary;
                border: 2px solid @primaryLight;
            }
            QTabWidget::pane {
                background: @tabPaneBackground;
                border-color: @secondaryBorder;
            }
            QTabBar::tab {
                background: @tabBackground;
                color: @foreground;
            }
            QTabBar::tab:selected {
                background: @secondarySoft;
                color: @secondaryText;
                border-color: @secondaryStrongBorder;
            }
            QScrollBar::handle {
                background: @secondaryScroll;
            }
            QScrollBar::handle:hover {
                background: @primaryScrollHover;
            }
            QScrollBar:vertical, QScrollBar:horizontal {
                background: @scrollBackground;
            }
            QToolTip {
                background: @menuPopupBackground;
                color: @foreground;
                border: 1px solid @secondaryMenuBorder;
                padding: 5px;
            }
        )");
        replace_token(cyber_accent, "@mutedForeground", muted_foreground);
        replace_token(cyber_accent, "@foreground", foreground);
        replace_token(cyber_accent, "@selectionText", foreground);
        replace_token(cyber_accent, "@selectionBackground",
                      rgba(mix_color(background, secondary, 0.34), 224));
        replace_token(cyber_accent, "@dialogBackground", rgba(background_mid, 238));
        replace_token(cyber_accent, "@titleMiddle", rgba(background_mid, 206));
        replace_token(cyber_accent, "@menuRowBackground", rgba(background_mid, 168));
        replace_token(cyber_accent, "@panelMiddle", rgba(background_mid, 150));
        replace_token(cyber_accent, "@scoreTableBackground", rgba(background_mid, 154));
        replace_token(cyber_accent, "@groupBackground", rgba(background_mid, 142));
        replace_token(cyber_accent, "@inputBackground", rgba(background_mid, 172));
        replace_token(cyber_accent, "@logBackgroundStart", rgba(background_mid, 160));
        replace_token(
            cyber_accent, "@logBackgroundEnd",
            rgba(mix_color(background_deep, secondary_deep, 0.36), 156));
        replace_token(cyber_accent, "@buttonMiddle", rgba(background_mid, 194));
        replace_token(cyber_accent, "@menuPopupBackground", rgba(background_mid, 232));
        replace_token(cyber_accent, "@popupListBackground", rgba(background_mid, 248));
        replace_token(cyber_accent, "@progressBackground", rgba(background_mid, 184));
        replace_token(cyber_accent, "@sliderGroove", rgba(background_mid, 210));
        replace_token(cyber_accent, "@sliderHandle",
                      mix_color(background, primary, 0.72));
        replace_token(cyber_accent, "@scrollBackground", rgba(background_mid, 120));
        replace_token(cyber_accent, "@checkboxBackground", rgba(background_mid, 190));
        replace_token(cyber_accent, "@tabPaneBackground", rgba(background_mid, 152));
        replace_token(cyber_accent, "@tabBackground", rgba(background_mid, 186));
        replace_token(cyber_accent, "@primaryMenuBorder", rgba(primary_light, 76));
        replace_token(cyber_accent, "@primaryTableBorder", rgba(primary_light, 66));
        replace_token(cyber_accent, "@primaryStrongBorder", rgba(primary_light, 126));
        replace_token(cyber_accent, "@primaryScrollHover", rgba(primary_light, 168));
        replace_token(cyber_accent, "@primaryText", light_text);
        replace_token(cyber_accent, "@primaryLight", primary_light);
        replace_token(cyber_accent, "@secondaryPanelBorder", rgba(secondary_light, 70));
        replace_token(cyber_accent, "@secondaryStrongBorder", rgba(secondary_light, 140));
        replace_token(cyber_accent, "@secondaryMenuBorder", rgba(secondary_light, 88));
        replace_token(cyber_accent, "@secondarySelection", rgba(secondary_dark, 184));
        replace_token(cyber_accent, "@secondaryScroll", rgba(secondary_light, 104));
        replace_token(cyber_accent, "@secondaryBorder", rgba(secondary_light, 70));
        replace_token(cyber_accent, "@secondaryLight", secondary_light);
        replace_token(cyber_accent, "@secondaryText", secondary_text);
        replace_token(cyber_accent, "@disabledBackground",
                      rgba(mix_color(background, primary, 0.08), 176));
        replace_token(cyber_accent, "@disabledText",
                      mix_color(foreground, background, 0.48));
        replace_token(cyber_accent, "@disabledBorder",
                      rgba(mix_color(background, secondary, 0.22), 92));
        replace_token(cyber_accent, "@titlePrimary", rgba(background_dark, 218));
        replace_token(cyber_accent, "@titleSecondary",
                      rgba(mix_color(background_deep, secondary_deep, 0.58), 214));
        replace_token(cyber_accent, "@panelPrimary",
                      rgba(mix_color(background_dark, primary_deep, 0.18), 168));
        replace_token(cyber_accent, "@panelSecondary",
                      rgba(mix_color(background_deep, secondary_deep, 0.62), 158));
        replace_token(cyber_accent, "@tableAlternate",
                      rgba(mix_color(background_deep, secondary_deep, 0.35), 158));
        replace_token(cyber_accent, "@secondaryGrid", rgba(secondary_light, 36));
        replace_token(cyber_accent, "@buttonPrimary",
                      rgba(mix_color(background_dark, primary_dark, 0.42), 198));
        replace_token(cyber_accent, "@buttonSecondary",
                      rgba(mix_color(background_deep, secondary_dark, 0.44), 198));
        replace_token(cyber_accent, "@buttonHoverPrimary",
                      rgba(mix_color(background_dark, primary_dark, 0.62), 224));
        replace_token(cyber_accent, "@buttonHoverSecondary",
                      rgba(mix_color(background_deep, secondary_dark, 0.62), 224));
        replace_token(cyber_accent, "@pressedSecondary", rgba(secondary_deep, 232));
        replace_token(cyber_accent, "@dangerBackground", rgba(secondary_dark, 212));
        replace_token(cyber_accent, "@dangerHover", rgba(secondary, 230));
        replace_token(cyber_accent, "@secondarySoft", rgba(secondary, 46));
        replace_token(cyber_accent, "@headerPrimary", rgba(primary_dark, 218));
        replace_token(cyber_accent, "@headerSecondary", rgba(secondary_dark, 218));
        replace_token(cyber_accent, "@primary", primary);
        replace_token(cyber_accent, "@secondary", secondary);
        style_sheet += cyber_accent;

        QPalette palette = system_palette;
        const QColor surface = mix_color(background, mix_color(primary, secondary, 0.5),
                                         light_background ? 0.05 : 0.12);
        const QColor alternate_surface =
            mix_color(background, secondary, light_background ? 0.10 : 0.20);
        const QColor highlight =
            mix_color(background, secondary, light_background ? 0.34 : 0.54);
        palette.setColor(QPalette::Window, background);
        palette.setColor(QPalette::WindowText, foreground);
        palette.setColor(QPalette::Base, surface);
        palette.setColor(QPalette::AlternateBase, alternate_surface);
        palette.setColor(QPalette::ToolTipBase, surface);
        palette.setColor(QPalette::ToolTipText, foreground);
        palette.setColor(QPalette::Text, foreground);
        palette.setColor(QPalette::Button, surface);
        palette.setColor(QPalette::ButtonText, foreground);
        palette.setColor(QPalette::BrightText, foreground);
        palette.setColor(QPalette::Highlight, highlight);
        palette.setColor(QPalette::HighlightedText, foreground);
        palette.setColor(QPalette::PlaceholderText, muted_foreground);
        palette.setColor(QPalette::Light, primary_light);
        palette.setColor(QPalette::Midlight, surface);
        palette.setColor(QPalette::Mid, alternate_surface);
        palette.setColor(QPalette::Dark, alternate_surface);
        palette.setColor(QPalette::Shadow, background);
        qApp->setPalette(palette);
    }

    qApp->setStyleSheet(style_sheet);
}

} // namespace neothemis::gui
