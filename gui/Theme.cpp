#include "Theme.hpp"

#include <QApplication>

namespace neothemis::gui {

void apply_application_theme(const std::string& theme) {
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
            height: 18px;
            text-align: center;
        }
        QProgressBar::chunk {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                stop:0 #61d7c7, stop:0.68 #8ef7e3, stop:1 #e86a82);
            border-radius: 7px;
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

    if (theme == "glassy-dark") {
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
    }

    qApp->setStyleSheet(style_sheet);
}

} // namespace neothemis::gui
