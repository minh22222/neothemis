#include "MainWindow.hpp"
#include "FileAssociation.hpp"
#include "FileFormats.hpp"
#include "GuiSupport.hpp"
#include "Theme.hpp"
#include "ThemeBackground.hpp"
#include "Translations.hpp"
#include "WindowsBackdrop.hpp"
#include "WindowResizeHandles.hpp"

#include "neothemis/JudgeCore.hpp"
#include "neothemis/ContestArchive.hpp"

#include <QAction>
#include <QAbstractItemView>
#include <QAbstractButton>
#include <QCoreApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFormLayout>
#include <QGraphicsDropShadowEffect>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>
#include <QShowEvent>
#include <QSpinBox>
#include <QSlider>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStackedLayout>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kBackgroundBlurVisualStrength = 72;

struct CellScore {
    double earned = 0.0;
    double max = 0.0;
    int completed = 0;
};

struct ServerUserSyncResult {
    int created = 0;
    int existing = 0;
    int removed = 0;
    int skipped = 0;
};

using neothemis::gui::default_temporary_dir;
using neothemis::gui::format_points;
using neothemis::gui::load_logo_pixmap;
using neothemis::gui::path_from_qstring;
using neothemis::gui::read_config_file;
using neothemis::gui::test_names_for_problem;
using neothemis::gui::test_point_keys;
using neothemis::gui::trim;
using neothemis::gui::write_problem_config;

using neothemis::gui::ArchiveProgress;
using neothemis::gui::XlsxRow;
using neothemis::gui::core_archive_progress;
using neothemis::gui::extract_zip_file;
using neothemis::gui::read_csv_records;
using neothemis::gui::verdict_from_string;
using neothemis::gui::write_contest_archive;
using neothemis::gui::write_xlsx_file;
using neothemis::gui::xlsx_number;
using neothemis::gui::xlsx_text;

class SettingsDialog final : public QDialog {
public:
    explicit SettingsDialog(QWidget* parent) : QDialog(parent) {}

    void add_close_handler(std::function<void()> handler) {
        close_handlers_.push_back(std::move(handler));
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        auto handlers = std::move(close_handlers_);
        for (const auto& handler : handlers) {
            handler();
        }
        QDialog::closeEvent(event);
    }

private:
    std::vector<std::function<void()>> close_handlers_;
};

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(fs::path initial_contest) {
        load_app_settings();
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAutoFillBackground(false);
        setWindowTitle(text("window_title"));
        setWindowIcon(QIcon(":/materials/logo.png"));
        setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
        resize(1240, 780);

        auto* central = new QWidget(this);
        central->setObjectName("AppRoot");
        auto* stack = new QStackedLayout(central);
        stack->setContentsMargins(0, 0, 0, 0);
        stack->setStackingMode(QStackedLayout::StackAll);
        background_layer_ = new neothemis::gui::ThemeBackground(central);
        auto* content_surface = new QWidget(central);
        content_surface->setObjectName("AppContent");
        stack->addWidget(background_layer_);
        stack->addWidget(content_surface);
        stack->setCurrentWidget(content_surface);

        auto* root = new QVBoxLayout(content_surface);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);

        title_bar_ = new QWidget(content_surface);
        title_bar_->setObjectName("WindowTitleBar");
        title_bar_->installEventFilter(this);
        auto* title_layout = new QHBoxLayout(title_bar_);
        title_layout->setContentsMargins(10, 0, 6, 0);
        title_layout->setSpacing(8);
        auto* logo = new QLabel(title_bar_);
        logo->setObjectName("AppLogo");
        QPixmap pixmap = load_logo_pixmap();
        if (!pixmap.isNull()) {
            logo->setPixmap(pixmap.scaled(24, 24, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
        logo->installEventFilter(this);
        auto* app_name = new QLabel("NeoThemis", title_bar_);
        app_name->setObjectName("WindowAppName");
        app_name->installEventFilter(this);
        title_layout->addWidget(logo);
        title_layout->addWidget(app_name);
        title_layout->addStretch(1);
        contest_title_ = new QLabel(text("no_contest_open"), title_bar_);
        contest_title_->setObjectName("ContestTitle");
        contest_title_->installEventFilter(this);
        title_layout->addWidget(contest_title_);
        auto* minimize = new QToolButton(title_bar_);
        minimize->setObjectName("WindowButton");
        minimize->setText("-");
        auto* maximize = new QToolButton(title_bar_);
        maximize->setObjectName("WindowButton");
        maximize->setText("[]");
        auto* close = new QToolButton(title_bar_);
        close->setObjectName("WindowCloseButton");
        close->setText("X");
        close->setIcon(QIcon());
        close->setToolButtonStyle(Qt::ToolButtonTextOnly);
        close->setFocusPolicy(Qt::NoFocus);
        close->setToolTip(text("close"));
        close->setAccessibleName(text("close"));
        title_layout->addWidget(minimize);
        title_layout->addWidget(maximize);
        title_layout->addWidget(close);
        root->addWidget(title_bar_);

        auto* menu_row = new QWidget(content_surface);
        menu_row->setObjectName("MenuRow");
        auto* menu_layout = new QHBoxLayout(menu_row);
        menu_layout->setContentsMargins(10, 0, 10, 0);
        menu_layout->setSpacing(0);
        menu_bar_ = new QMenuBar(menu_row);
        menu_bar_->setNativeMenuBar(false);
        menu_layout->addWidget(menu_bar_, 0, Qt::AlignLeft);
        menu_layout->addStretch(1);
        root->addWidget(menu_row);

        build_toolbar();

        auto* content = new QHBoxLayout;
        content->setContentsMargins(12, 10, 12, 12);
        content->setSpacing(10);

        table_ = new QTableWidget(content_surface);
        table_->setObjectName("ScoreTable");
        table_->setAlternatingRowColors(true);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        table_->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
        table_->verticalHeader()->setVisible(false);
        table_->verticalHeader()->setDefaultSectionSize(42);
        content->addWidget(table_, 1);

        auto* side = new QWidget(content_surface);
        side->setObjectName("SidePanel");
        auto* side_layout = new QVBoxLayout(side);
        side_layout->setContentsMargins(14, 14, 14, 14);
        side_layout->setSpacing(10);
        side->setFixedWidth(380);

        judge_group_ = new QGroupBox(text("judge"), side);
        judge_group_->setObjectName("GlassGroup");
        auto* action_layout = new QVBoxLayout(judge_group_);
        judge_selected_button_ = new QPushButton(text("judge_selected"), judge_group_);
        judge_all_button_ = new QPushButton(text("judge_all"), judge_group_);
        stop_button_ = new QPushButton(text("stop"), judge_group_);
        detail_view_button_ = new QPushButton(text("detail_view"), judge_group_);
        stop_button_->setObjectName("StopButton");
        stop_button_->setEnabled(false);
        action_layout->addWidget(judge_selected_button_);
        action_layout->addWidget(judge_all_button_);
        action_layout->addWidget(stop_button_);
        action_layout->addWidget(detail_view_button_);
        side_layout->addWidget(judge_group_);

        progress_ = new QProgressBar(side);
        progress_->setRange(0, 100);
        progress_->setValue(0);
        progress_->setFixedHeight(30);
        side_layout->addWidget(progress_);

        judge_elapsed_label_ = new QLabel(side);
        judge_elapsed_label_->setObjectName("JudgeElapsedLabel");
        judge_elapsed_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        side_layout->addWidget(judge_elapsed_label_);
        judge_elapsed_update_timer_ = new QTimer(this);
        judge_elapsed_update_timer_->setInterval(100);
        QObject::connect(judge_elapsed_update_timer_, &QTimer::timeout,
                         [this]() { update_judge_elapsed_label(); });
        update_judge_elapsed_label();

        log_ = new QPlainTextEdit(side);
        log_->setObjectName("LogPanel");
        log_->setReadOnly(true);
        side_layout->addWidget(log_, 1);

        content->addWidget(side);
        root->addLayout(content, 1);
        setCentralWidget(central);
        neothemis::gui::install_windows_resize_handles(this);
        side_panel_ = side;
        apply_selected_theme();

        QObject::connect(judge_selected_button_, &QPushButton::clicked, [this]() { start_judge(true); });
        QObject::connect(judge_all_button_, &QPushButton::clicked, [this]() { start_judge(false); });
        QObject::connect(stop_button_, &QPushButton::clicked, [this]() { request_stop_judge(); });
        QObject::connect(detail_view_button_, &QPushButton::clicked,
                         [this]() { show_judge_detail_view(); });
        QObject::connect(table_, &QTableWidget::cellDoubleClicked, [this](int row, int col) {
            show_result_details(row, col);
        });
        QObject::connect(table_->horizontalHeader(), &QHeaderView::sectionClicked,
                         [this](int section) { sort_by_column(section); });
        QObject::connect(table_->horizontalHeader(), &QWidget::customContextMenuRequested,
                         [this](const QPoint& pos) { show_header_menu(pos); });
        QObject::connect(minimize, &QToolButton::clicked, this, &QWidget::showMinimized);
        QObject::connect(maximize, &QToolButton::clicked, [this]() {
            isMaximized() ? showNormal() : showMaximized();
        });
        QObject::connect(close, &QToolButton::clicked, this, &QWidget::close);

        if (!initial_contest.empty()) {
            // Give the first window frame time to paint before multicore extraction starts.
            QTimer::singleShot(100, this,
                               [this, path = std::move(initial_contest)]() {
                                   open_contest_file(path);
                               });
        }
    }

    ~MainWindow() override {
        stop_local_server(false);
        stop_active_judge();
        join_archive_thread();
        cleanup_temporary_contests();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == title_bar_ || watched == contest_title_ ||
            (watched->isWidgetType() &&
             static_cast<QWidget*>(watched)->objectName() == "WindowAppName") ||
            (watched->isWidgetType() &&
             static_cast<QWidget*>(watched)->objectName() == "AppLogo")) {
            if (event->type() == QEvent::MouseButtonDblClick) {
                auto* mouse = static_cast<QMouseEvent*>(event);
                if (mouse->button() == Qt::LeftButton) {
                    isMaximized() ? showNormal() : showMaximized();
                    return true;
                }
            }
            if (event->type() == QEvent::MouseButtonPress) {
                auto* mouse = static_cast<QMouseEvent*>(event);
                if (mouse->button() == Qt::LeftButton) {
                    dragging_title_bar_ = true;
                    drag_offset_ = mouse->globalPosition().toPoint() - frameGeometry().topLeft();
                    return true;
                }
            }
            if (event->type() == QEvent::MouseMove && dragging_title_bar_) {
                auto* mouse = static_cast<QMouseEvent*>(event);
                if (!isMaximized()) {
                    move(mouse->globalPosition().toPoint() - drag_offset_);
                }
                return true;
            }
            if (event->type() == QEvent::MouseButtonRelease) {
                dragging_title_bar_ = false;
            }
        }
        return QMainWindow::eventFilter(watched, event);
    }

    void closeEvent(QCloseEvent* event) override {
        if (archive_running_.load()) {
            QMessageBox::information(this, text("archive_operation_running"),
                                     text("wait_for_archive_operation"));
            event->ignore();
            return;
        }
        if (!confirm_discard_unsaved_file(true)) {
            event->ignore();
            return;
        }
        stop_active_judge();
        join_archive_thread();
        stop_local_server(false);
        cleanup_temporary_contests();
        event->accept();
    }

    void showEvent(QShowEvent* event) override {
        QMainWindow::showEvent(event);
        apply_selected_theme();
        schedule_backdrop_startup_passes();
    }

private:
    QString text(const char* key) const {
        return neothemis::gui::translated_text(key, language_ == "vi");
    }

    bool background_transparency_active() const {
        return transparent_background_ && background_transparency_ > 0;
    }

    bool background_blur_active() const {
        return blur_background_;
    }

    void load_app_settings() {
        QSettings settings("NeoThemis", "NeoThemis");
        language_ = settings.value("language", "en").toString().toStdString();
        if (language_ != "vi") {
            language_ = "en";
        }
        theme_ = settings.value("theme", "dark").toString().toStdString();
        if (theme_ == "glassy-dark") {
            theme_ = "cyber";
        }
        if (theme_ != "cyber") {
            theme_ = "dark";
        }
        transparent_background_ = settings.value("transparent_background", false).toBool();
        background_transparency_ =
            std::clamp(settings.value("background_transparency", 20).toInt(), 0, 95);
        blur_background_ = settings.value("blur_background", false).toBool();
        temporary_dir_ = settings.value(
            "temporary_dir",
            QString::fromStdString(default_temporary_dir().string())).toString().toStdString();
        if (temporary_dir_.empty()) {
            temporary_dir_ = default_temporary_dir();
        }
        server_port_ = std::clamp(settings.value("server_port", 8080).toInt(), 1024, 65535);
        server_allow_lan_ = settings.value("server_allow_lan", false).toBool();
        server_join_code_ = settings.value("server_join_code").toString().trimmed();
        if (server_join_code_.isEmpty()) {
            server_join_code_ = generated_server_secret(10);
        }
        server_admin_password_ = settings.value("server_admin_password").toString();
        if (server_admin_password_.isEmpty()) {
            server_admin_password_ = generated_server_secret(14);
        }
    }

    void save_app_settings() const {
        QSettings settings("NeoThemis", "NeoThemis");
        settings.setValue("language", QString::fromStdString(language_));
        settings.setValue("theme", QString::fromStdString(theme_));
        settings.setValue("transparent_background", transparent_background_);
        settings.setValue("background_transparency", background_transparency_);
        settings.setValue("blur_background", blur_background_);
        settings.remove("background_blur_radius");
        settings.setValue("temporary_dir", QString::fromStdString(temporary_dir_.string()));
        settings.setValue("server_port", server_port_);
        settings.setValue("server_allow_lan", server_allow_lan_);
        settings.setValue("server_join_code", server_join_code_);
        settings.setValue("server_admin_password", server_admin_password_);
    }

    void apply_language_to_main_window() {
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

    void build_toolbar() {
        auto* bar = menu_bar_ ? menu_bar_ : menuBar();
        bar->clear();

        auto* contest_menu = bar->addMenu(text("contest"));
        auto* open_folder_action =
            contest_menu->addAction(text("open_folder"), [this]() { open_contest(); });
        open_folder_action->setShortcut(QKeySequence("Ctrl+O"));
        auto* open_file_action =
            contest_menu->addAction(text("open_contest_file"),
                                    [this]() { open_contest_file(); });
        open_file_action->setShortcut(QKeySequence("Ctrl+Shift+O"));
        contest_menu->addSeparator();
        auto* save_action =
            contest_menu->addAction(text("save_contest_file"),
                                    [this]() { save_contest_container(false); });
        save_action->setShortcut(QKeySequence("Ctrl+S"));
        auto* save_as_action =
            contest_menu->addAction(text("save_contest_file_as"),
                                    [this]() { save_contest_container(true); });
        save_as_action->setShortcut(QKeySequence("Ctrl+Shift+S"));
        contest_menu->addSeparator();
        contest_menu->addAction(text("refresh"), [this]() { refresh_table(); });

        auto* contestant_menu = bar->addMenu(text("contestant_menu"));
        contestant_menu->addAction(text("add_contestants_from_folder"),
                                   [this]() { add_contestants_from_folder(); });
        contestant_menu->addAction(text("sync_server_users"), [this]() {
            try {
                const ServerUserSyncResult result = sync_server_users_from_contest();
                if (server_users_table_) {
                    refresh_server_users_table(server_users_table_);
                }
                const QString message = text("server_users_synced")
                                            .arg(result.created)
                                            .arg(result.existing)
                                            .arg(result.removed)
                                            .arg(result.skipped);
                log_->appendPlainText(message);
                QMessageBox::information(this, text("sync_server_users"), message);
            } catch (const std::exception& ex) {
                QMessageBox::critical(this, text("server_user_failed"), ex.what());
            }
        });

        auto* judge_menu = bar->addMenu(text("judge"));
        judge_selected_action_ = judge_menu->addAction(text("judge_selected"), [this]() { start_judge(true); });
        judge_all_action_ = judge_menu->addAction(text("judge_all"), [this]() { start_judge(false); });
        stop_action_ = judge_menu->addAction(text("stop"), [this]() { request_stop_judge(); });
        stop_action_->setEnabled(false);

        auto* server_menu = bar->addMenu(text("server"));
        server_start_action_ =
            server_menu->addAction(text("start_local_server"), [this]() { open_start_server_dialog(); });
        server_stop_action_ =
            server_menu->addAction(text("stop_local_server"), [this]() { stop_local_server(true); });
        update_server_actions();

        auto* export_menu = bar->addMenu(text("export"));
        export_menu->addAction(text("export_scoreboard"), [this]() { export_scoreboard_xlsx(); });
        export_menu->addAction(text("export_data"), [this]() { export_data_xlsx(); });

        auto* converter_menu = bar->addMenu(text("converter"));
        converter_menu->addAction(text("convert_old_contest_file"), [this]() {
            convert_old_contest_to_ncontest(false);
        });
        converter_menu->addAction(text("convert_old_contest_folder"), [this]() {
            convert_old_contest_to_ncontest(true);
        });

        auto* settings_menu = bar->addMenu(text("settings"));
        settings_menu->addAction(text("application_settings"), [this]() { open_settings_dialog(0); });
        settings_menu->addAction(text("contest_config"), [this]() { open_settings_dialog(1); });
        settings_menu->addAction(text("problem_config"), [this]() { open_settings_dialog(2); });
        settings_menu->addAction(text("server_settings"), [this]() { open_settings_dialog(3); });
        settings_menu->addAction(text("server_users"), [this]() { open_settings_dialog(4); });

        auto* help_menu = bar->addMenu(text("help"));
        help_menu->addAction(text("about"), [this]() { show_about_dialog(); });

        for (QMenu* menu : {contest_menu, contestant_menu, judge_menu, server_menu,
                            export_menu, converter_menu, settings_menu, help_menu}) {
            menu->setAttribute(Qt::WA_TranslucentBackground);
            menu->setWindowFlag(Qt::NoDropShadowWindowHint, true);
        }
    }

    void add_soft_shadow(QWidget* widget, qreal blur_radius, const QColor& color) {
        if (!widget) {
            return;
        }
        auto* shadow = new QGraphicsDropShadowEffect(widget);
        shadow->setBlurRadius(blur_radius);
        shadow->setOffset(0, 12);
        shadow->setColor(color);
        widget->setGraphicsEffect(shadow);
    }

    void apply_translucent_surface_attributes() {
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAutoFillBackground(false);

        if (QWidget* root = centralWidget()) {
            root->setAttribute(Qt::WA_NoSystemBackground, true);
            root->setAutoFillBackground(false);
        }
        if (background_layer_) {
            background_layer_->setAttribute(Qt::WA_NoSystemBackground, true);
            background_layer_->setAttribute(Qt::WA_TranslucentBackground, true);
            background_layer_->setAutoFillBackground(false);
        }
    }

    void apply_selected_theme() {
        const bool cyber = theme_ == "cyber";
        const bool transparent = background_transparency_active();
        const bool blurred = transparent && background_blur_active();
        apply_translucent_surface_attributes();
        neothemis::gui::apply_application_theme(theme_);
        apply_native_backdrop(false);
        int opacity = transparent ? 255 * (100 - background_transparency_) / 100 : 255;
        if (blurred) {
            opacity = opacity *
                      (100 - std::min(55, kBackgroundBlurVisualStrength / 4)) / 100;
        }
        if (background_layer_) {
            background_layer_->set_appearance(theme_, opacity,
                                              blurred ? kBackgroundBlurVisualStrength : 0);
        }
        add_soft_shadow(table_, cyber ? 56 : 34,
                        cyber ? QColor(55, 8, 32, 178) : QColor(0, 0, 0, 120));
        add_soft_shadow(side_panel_, cyber ? 60 : 36,
                        cyber ? QColor(72, 10, 39, 188) : QColor(0, 0, 0, 135));
    }

    void apply_native_backdrop(bool force_compositor_update) {
        const bool transparent = background_transparency_active();
        const bool blurred = transparent && background_blur_active();
        neothemis::gui::apply_windows_backdrop(
            this, transparent, background_transparency_, blurred,
            force_compositor_update);
    }

    void schedule_backdrop_startup_passes() {
        // Native compositors register a newly shown window asynchronously.
        // Reassert the current state after registration and window-rule handling.
        for (int delay_ms : {0, 75, 250, 700}) {
            QTimer::singleShot(delay_ms, this, [this]() {
                if (isVisible()) {
                    apply_native_backdrop(true);
                }
            });
        }
    }

    QString generated_server_secret(int chars) const {
        QByteArray data;
        data.resize((chars + 1) / 2);
        for (qsizetype i = 0; i < data.size(); ++i) {
            data[i] = static_cast<char>(QRandomGenerator::system()->bounded(256));
        }
        return QString::fromLatin1(data.toHex()).left(chars);
    }

    QString local_server_executable() const {
        QString name =
#ifdef _WIN32
            "neothemis-server.exe";
#else
            "neothemis-server";
#endif
        fs::path alongside = fs::path(QCoreApplication::applicationDirPath().toStdString()) /
                             name.toStdString();
        if (fs::exists(alongside)) {
            return QString::fromStdString(alongside.string());
        }
        return name;
    }

    fs::path server_data_dir() const {
        if (contest_root_.empty()) {
            return {};
        }
        return contest_root_ / ".neothemis-server";
    }

    fs::path server_database_path() const {
        fs::path dir = server_data_dir();
        if (dir.empty()) {
            return {};
        }
        return dir / "server.db";
    }

    QString server_database_display_path() const {
        fs::path path = server_database_path();
        if (path.empty()) {
            return text("open_contest_first");
        }
        return QString::fromStdString(path.string());
    }

    bool valid_server_username(const QString& username) const {
        static const QRegularExpression pattern("^[A-Za-z0-9 _-]{1,64}$");
        static const QRegularExpression has_visible(".*[A-Za-z0-9].*");
        return pattern.match(username).hasMatch() &&
               has_visible.match(username).hasMatch();
    }

    void exec_server_sql(QSqlDatabase& database, const QString& sql) const {
        QSqlQuery query(database);
        if (!query.exec(sql)) {
            throw std::runtime_error(query.lastError().text().toStdString());
        }
    }

    void initialize_server_database(QSqlDatabase& database) const {
        exec_server_sql(database, "PRAGMA journal_mode=WAL");
        exec_server_sql(database, "PRAGMA foreign_keys=ON");
        exec_server_sql(database, "PRAGMA busy_timeout=5000");
        exec_server_sql(database,
                        "CREATE TABLE IF NOT EXISTS users ("
                        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                        "username TEXT UNIQUE NOT NULL,"
                        "salt TEXT NOT NULL,"
                        "hash TEXT NOT NULL,"
                        "password TEXT NOT NULL DEFAULT '',"
                        "role TEXT NOT NULL,"
                        "created_at INTEGER NOT NULL)");
        exec_server_sql(database,
                        "CREATE TABLE IF NOT EXISTS sessions ("
                        "token TEXT PRIMARY KEY,"
                        "user_id INTEGER NOT NULL,"
                        "csrf TEXT NOT NULL,"
                        "expires_at INTEGER NOT NULL,"
                        "FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE)");
        exec_server_sql(database,
                        "CREATE TABLE IF NOT EXISTS submissions ("
                        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                        "user_id INTEGER NOT NULL,"
                        "username TEXT NOT NULL,"
                        "problem TEXT NOT NULL,"
                        "source_path TEXT NOT NULL,"
                        "status TEXT NOT NULL,"
                        "verdict TEXT NOT NULL,"
                        "score REAL NOT NULL,"
                        "message TEXT NOT NULL,"
                        "submitted_at INTEGER NOT NULL,"
                        "judged_at INTEGER NOT NULL,"
                        "FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE)");
        exec_server_sql(database,
                        "CREATE TABLE IF NOT EXISTS test_results ("
                        "submission_id INTEGER NOT NULL,"
                        "test TEXT NOT NULL,"
                        "verdict TEXT NOT NULL,"
                        "time_ms INTEGER NOT NULL,"
                        "exit_code INTEGER NOT NULL DEFAULT 0,"
                        "max_points REAL NOT NULL,"
                        "earned_points REAL NOT NULL,"
                        "message TEXT NOT NULL,"
                        "FOREIGN KEY(submission_id) REFERENCES submissions(id) ON DELETE CASCADE)");
        exec_server_sql(database,
                        "CREATE TABLE IF NOT EXISTS ignored_submissions ("
                        "submission_id INTEGER PRIMARY KEY,"
                        "ignored_at INTEGER NOT NULL,"
                        "FOREIGN KEY(submission_id) REFERENCES submissions(id) ON DELETE CASCADE)");
        ensure_server_column(database, "test_results", "exit_code",
                             "INTEGER NOT NULL DEFAULT 0");
        ensure_server_column(database, "users", "password",
                             "TEXT NOT NULL DEFAULT ''");
        exec_server_sql(database, "UPDATE submissions SET status='queued' WHERE status='running'");
    }

    void ensure_server_column(QSqlDatabase& database,
                              const QString& table,
                              const QString& column,
                              const QString& definition) const {
        QSqlQuery info(database);
        if (!info.exec("PRAGMA table_info(" + table + ")")) {
            throw std::runtime_error(info.lastError().text().toStdString());
        }
        while (info.next()) {
            if (info.value(1).toString() == column) {
                return;
            }
        }
        exec_server_sql(database, "ALTER TABLE " + table + " ADD COLUMN " +
                                  column + " " + definition);
    }

    QSqlDatabase open_server_database(bool create_if_missing) const {
        if (contest_root_.empty()) {
            throw std::runtime_error(text("open_contest_first").toStdString());
        }
        fs::path database_path = server_database_path();
        if (create_if_missing) {
            fs::create_directories(database_path.parent_path());
        } else if (!fs::exists(database_path)) {
            throw std::runtime_error(text("server_database_missing").toStdString());
        }

        const QString connection_name = "neothemis_gui_server_database";
        QSqlDatabase database = QSqlDatabase::contains(connection_name)
                                    ? QSqlDatabase::database(connection_name)
                                    : QSqlDatabase::addDatabase("QSQLITE", connection_name);
        if (database.isOpen()) {
            database.close();
        }
        database.setDatabaseName(QString::fromStdString(database_path.string()));
        if (!database.open()) {
            throw std::runtime_error(database.lastError().text().toStdString());
        }
        initialize_server_database(database);
        return database;
    }

    void create_server_user(const QString& username, const QString& password,
                            const QString& role) {
        const QString trimmed_username = username.trimmed();
        if (!valid_server_username(trimmed_username)) {
            throw std::runtime_error(text("server_username_invalid").toStdString());
        }
        if (password.size() < 6 || password.size() > 128) {
            throw std::runtime_error(text("server_password_invalid").toStdString());
        }
        if (role != "admin" && role != "contestant") {
            throw std::runtime_error(text("server_role_invalid").toStdString());
        }

        QSqlDatabase database = open_server_database(true);
        QSqlQuery query(database);
        query.prepare("INSERT INTO users(username, salt, hash, password, role, created_at) "
                      "VALUES(?, '', '', ?, ?, ?)");
        query.addBindValue(trimmed_username);
        query.addBindValue(password);
        query.addBindValue(role);
        query.addBindValue(QDateTime::currentSecsSinceEpoch());
        if (!query.exec()) {
            const QString error = query.lastError().text();
            if (error.contains("UNIQUE", Qt::CaseInsensitive) ||
                error.contains("constraint", Qt::CaseInsensitive)) {
                throw std::runtime_error(text("server_user_exists").toStdString());
            }
            throw std::runtime_error(error.toStdString());
        }
        if (role == "contestant") {
            fs::create_directories(contest_root_ / contestants_dir_ /
                                   trimmed_username.toStdString());
        }
        mark_contest_dirty();
    }

    void change_server_user_password(const QString& username, const QString& password) {
        if (password.size() < 6 || password.size() > 128) {
            throw std::runtime_error(text("server_password_invalid").toStdString());
        }
        QSqlDatabase database = open_server_database(false);
        QSqlQuery update(database);
        update.prepare("UPDATE users SET password=?, salt='', hash='' WHERE username=?");
        update.addBindValue(password);
        update.addBindValue(username);
        if (!update.exec()) {
            throw std::runtime_error(update.lastError().text().toStdString());
        }
        if (update.numRowsAffected() != 1) {
            throw std::runtime_error(text("server_user_not_found").toStdString());
        }
        QSqlQuery sessions(database);
        sessions.prepare("DELETE FROM sessions WHERE user_id=(SELECT id FROM users WHERE username=?)");
        sessions.addBindValue(username);
        if (!sessions.exec()) {
            throw std::runtime_error(sessions.lastError().text().toStdString());
        }
        mark_contest_dirty();
    }

    void remove_server_user(const QString& username) {
        QSqlDatabase database = open_server_database(false);
        QSqlQuery user(database);
        user.prepare("SELECT role FROM users WHERE username=?");
        user.addBindValue(username);
        if (!user.exec()) {
            throw std::runtime_error(user.lastError().text().toStdString());
        }
        if (!user.next()) {
            throw std::runtime_error(text("server_user_not_found").toStdString());
        }
        if (user.value(0).toString() == "admin") {
            QSqlQuery admins(database);
            if (!admins.exec("SELECT COUNT(*) FROM users WHERE role='admin'") ||
                !admins.next()) {
                throw std::runtime_error(admins.lastError().text().toStdString());
            }
            if (admins.value(0).toInt() <= 1) {
                throw std::runtime_error(text("server_last_admin").toStdString());
            }
        }
        user.finish();
        QSqlQuery remove(database);
        remove.prepare("DELETE FROM users WHERE username=?");
        remove.addBindValue(username);
        if (!remove.exec()) {
            throw std::runtime_error(remove.lastError().text().toStdString());
        }
        mark_contest_dirty();
    }

    ServerUserSyncResult sync_server_users_from_contest() {
        if (contest_root_.empty()) {
            throw std::runtime_error(text("open_contest_first").toStdString());
        }
        QSqlDatabase database = open_server_database(true);
        ServerUserSyncResult result;
        const fs::path contestants_root = contest_root_ / contestants_dir_;
        if (!fs::exists(contestants_root)) {
            return result;
        }
        std::set<QString> contest_usernames;
        for (const auto& entry : fs::directory_iterator(contestants_root)) {
            if (!entry.is_directory()) {
                continue;
            }
            const QString username =
                QString::fromStdString(entry.path().filename().string()).trimmed();
            if (!valid_server_username(username)) {
                ++result.skipped;
                continue;
            }
            contest_usernames.insert(username);
            QSqlQuery exists(database);
            exists.prepare("SELECT 1 FROM users WHERE username=?");
            exists.addBindValue(username);
            if (!exists.exec()) {
                throw std::runtime_error(exists.lastError().text().toStdString());
            }
            if (exists.next()) {
                ++result.existing;
                continue;
            }
            QSqlQuery insert(database);
            insert.prepare("INSERT INTO users(username, salt, hash, password, role, created_at) "
                           "VALUES(?, '', '', '123456', 'contestant', ?)");
            insert.addBindValue(username);
            insert.addBindValue(QDateTime::currentSecsSinceEpoch());
            if (!insert.exec()) {
                throw std::runtime_error(insert.lastError().text().toStdString());
            }
            ++result.created;
        }

        std::vector<QString> stale_users;
        QSqlQuery users(database);
        if (!users.exec("SELECT username FROM users WHERE role='contestant'")) {
            throw std::runtime_error(users.lastError().text().toStdString());
        }
        while (users.next()) {
            const QString username = users.value(0).toString();
            if (contest_usernames.count(username) == 0) {
                stale_users.push_back(username);
            }
        }
        users.finish();
        for (const QString& username : stale_users) {
            QSqlQuery remove(database);
            remove.prepare("DELETE FROM users WHERE username=? AND role='contestant'");
            remove.addBindValue(username);
            if (!remove.exec()) {
                throw std::runtime_error(remove.lastError().text().toStdString());
            }
            result.removed += remove.numRowsAffected();
        }

        if (result.created > 0 || result.removed > 0) {
            mark_contest_dirty();
        }
        return result;
    }

    std::pair<int, int> import_server_users_csv(const fs::path& path) {
        auto records = read_csv_records(path);
        int created = 0;
        int skipped = 0;
        bool first = true;
        for (const auto& row : records) {
            if (row.empty()) {
                continue;
            }
            QString first_cell = QString::fromStdString(row[0]).trimmed().toLower();
            if (first && (first_cell == "username" || first_cell == "user" ||
                          first_cell == "contestant")) {
                first = false;
                continue;
            }
            first = false;
            if (row.size() < 2) {
                ++skipped;
                continue;
            }
            QString username = QString::fromStdString(row[0]).trimmed();
            QString password = QString::fromStdString(row[1]);
            QString role = row.size() >= 3
                               ? QString::fromStdString(row[2]).trimmed().toLower()
                               : QString();
            if (role.isEmpty()) {
                role = "contestant";
            }
            try {
                create_server_user(username, password, role);
                ++created;
            } catch (const std::exception&) {
                ++skipped;
            }
        }
        return {created, skipped};
    }

    fs::file_time_type safe_last_write_time(const fs::path& path) const {
        std::error_code error;
        if (path.empty() || !fs::exists(path, error)) {
            return fs::file_time_type::min();
        }
        fs::file_time_type time = fs::last_write_time(path, error);
        if (error) {
            return fs::file_time_type::min();
        }
        return time;
    }

    fs::path server_results_path() const {
        return contest_output_path(options_from_ui().output_csv);
    }

    fs::file_time_type server_database_activity_time() const {
        fs::path database_path = server_database_path();
        fs::file_time_type database_time = safe_last_write_time(database_path);
        fs::file_time_type wal_time =
            safe_last_write_time(fs::path(database_path.string() + "-wal"));
        return std::max(database_time, wal_time);
    }

    void initialize_server_auto_refresh_state() {
        last_server_database_write_ = server_database_activity_time();
        last_server_results_write_ = safe_last_write_time(server_results_path());
        last_server_contestants_write_ =
            safe_last_write_time(contest_root_ / contestants_dir_);
    }

    void start_server_auto_refresh() {
        initialize_server_auto_refresh_state();
        if (!server_refresh_timer_) {
            server_refresh_timer_ = new QTimer(this);
            server_refresh_timer_->setInterval(2000);
            QObject::connect(server_refresh_timer_, &QTimer::timeout,
                             [this]() { poll_server_auto_refresh(); });
        }
        server_refresh_timer_->start();
    }

    void stop_server_auto_refresh() {
        if (server_refresh_timer_) {
            server_refresh_timer_->stop();
        }
    }

    void poll_server_auto_refresh() {
        if (contest_root_.empty() || archive_running_.load() || judging_.load()) {
            return;
        }
        fs::file_time_type database_write = server_database_activity_time();
        fs::file_time_type results_write = safe_last_write_time(server_results_path());
        fs::file_time_type contestants_write =
            safe_last_write_time(contest_root_ / contestants_dir_);
        const bool changed = database_write != last_server_database_write_ ||
                             results_write != last_server_results_write_ ||
                             contestants_write != last_server_contestants_write_;
        if (!changed) {
            return;
        }
        last_server_database_write_ = database_write;
        last_server_results_write_ = results_write;
        last_server_contestants_write_ = contestants_write;
        refresh_table(false);
        if (server_users_table_) {
            try {
                refresh_server_users_table(server_users_table_);
            } catch (const std::exception& ex) {
                log_->appendPlainText(text("server_user_failed") + ": " +
                                      QString::fromUtf8(ex.what()));
            }
        }
    }

    void update_server_actions() {
        const bool running =
            server_process_ && server_process_->state() != QProcess::NotRunning;
        if (server_start_action_) {
            server_start_action_->setEnabled(!running);
        }
        if (server_stop_action_) {
            server_stop_action_->setEnabled(running);
        }
    }

    void append_server_output(const QByteArray& output) {
        if (!log_ || output.isEmpty()) {
            return;
        }
        QString text_output = QString::fromLocal8Bit(output).trimmed();
        if (text_output.isEmpty()) {
            return;
        }
        for (const QString& line : text_output.split('\n')) {
            log_->appendPlainText("[server] " + line.trimmed());
        }
    }

    void open_start_server_dialog() {
        if (contest_root_.empty()) {
            QMessageBox::information(this, text("no_contest"), text("open_contest_first"));
            return;
        }
        if (server_process_ && server_process_->state() != QProcess::NotRunning) {
            QMessageBox::information(this, text("server_running"), text("server_already_running"));
            return;
        }

        auto* dialog = new QDialog(this);
        dialog->setWindowTitle(text("start_local_server"));
        dialog->resize(520, 320);
        auto* layout = new QVBoxLayout(dialog);
        auto* form = new QFormLayout;

        auto* port = new QSpinBox(dialog);
        port->setRange(1024, 65535);
        port->setValue(server_port_);
        auto* allow_lan = new QCheckBox(dialog);
        allow_lan->setChecked(server_allow_lan_);
        auto* join_code = new QLineEdit(server_join_code_, dialog);
        auto* admin_password = new QLineEdit(server_admin_password_, dialog);
        auto* data_path = new QLineEdit(server_database_display_path(), dialog);
        data_path->setReadOnly(true);
        auto* warning = new QLabel(text("server_security_warning"), dialog);
        warning->setWordWrap(true);
        warning->setObjectName("ServerWarning");

        form->addRow(text("server_port"), port);
        form->addRow(text("server_allow_lan"), allow_lan);
        form->addRow(text("server_join_code"), join_code);
        form->addRow(text("server_admin_password"), admin_password);
        form->addRow(text("server_database"), data_path);
        layout->addLayout(form);
        layout->addWidget(warning);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                             dialog);
        auto* start_button = buttons->button(QDialogButtonBox::Ok);
        start_button->setText(text("start_local_server"));
        start_button->setIcon(QIcon());
        auto* close_button = buttons->button(QDialogButtonBox::Cancel);
        close_button->setText(text("close"));
        close_button->setIcon(QIcon());
        QObject::connect(buttons, &QDialogButtonBox::accepted, [this, dialog, port, allow_lan,
                                                                join_code, admin_password]() {
            server_port_ = port->value();
            server_allow_lan_ = allow_lan->isChecked();
            server_join_code_ = join_code->text().trimmed();
            server_admin_password_ = admin_password->text();
            save_app_settings();
            start_local_server(server_port_, server_allow_lan_,
                               server_join_code_, server_admin_password_);
            dialog->accept();
        });
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        layout->addWidget(buttons);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    }

    void start_local_server(int port, bool allow_lan,
                            const QString& join_code,
                            const QString& admin_password) {
        if (join_code.isEmpty() || admin_password.isEmpty()) {
            QMessageBox::warning(this, text("server_start_failed"),
                                 text("server_credentials_required"));
            return;
        }

        auto* process = new QProcess(this);
        process->setProperty("stopRequested", false);
        process->setProgram(local_server_executable());
        fs::create_directories(server_data_dir());
        QStringList args;
        args << "--contest" << QString::fromStdString(contest_root_.string())
             << "--data" << QString::fromStdString(server_data_dir().string())
             << "--port" << QString::number(port);
        if (allow_lan) {
            args << "--host" << "0.0.0.0" << "--allow-lan";
        }
        process->setArguments(args);
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert("NEOTHEMIS_SERVER_JOIN_CODE", join_code);
        environment.insert("NEOTHEMIS_SERVER_ADMIN_PASSWORD", admin_password);
        process->setProcessEnvironment(environment);
        process->setProcessChannelMode(QProcess::MergedChannels);

        QObject::connect(process, &QProcess::readyReadStandardOutput, [this, process]() {
            append_server_output(process->readAllStandardOutput());
        });
        QObject::connect(process, &QProcess::errorOccurred,
                         [this, process](QProcess::ProcessError error) {
            if (process->property("stopRequested").toBool() ||
                error == QProcess::Crashed) {
                return;
            }
            if (log_ && error == QProcess::FailedToStart) {
                log_->appendPlainText(text("server_start_failed") + ": " +
                                      process->errorString());
            } else if (log_) {
                log_->appendPlainText(text("server_process_error") + ": " +
                                      process->errorString());
            }
            update_server_actions();
        });
        QObject::connect(process,
                         qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                         [this, process](int exit_code, QProcess::ExitStatus status) {
            append_server_output(process->readAllStandardOutput());
            if (process->property("stopRequested").toBool()) {
                log_->appendPlainText(text("server_stopped"));
            } else if (status == QProcess::CrashExit) {
                log_->appendPlainText(text("server_crashed"));
            } else {
                log_->appendPlainText(text("server_stopped") + " (" +
                                      QString::number(exit_code) + ")");
            }
            if (server_process_ == process) {
                server_process_ = nullptr;
            }
            stop_server_auto_refresh();
            process->deleteLater();
            update_server_actions();
        });

        server_process_ = process;
        process->start();
        if (!process->waitForStarted(3000)) {
            QMessageBox::critical(this, text("server_start_failed"), process->errorString());
            server_process_ = nullptr;
            process->deleteLater();
            update_server_actions();
            return;
        }

        const QString host = allow_lan ? "0.0.0.0" : "127.0.0.1";
        const QString url = "http://" + host + ":" + QString::number(port);
        log_->appendPlainText(text("server_started") + ": " + url);
        log_->appendPlainText(text("server_database") + ": " + server_database_display_path());
        log_->appendPlainText(text("server_join_code") + ": " + join_code);
        log_->appendPlainText(text("server_admin_password") + ": " + admin_password);
        if (allow_lan) {
            log_->appendPlainText(text("server_lan_warning"));
        }
        mark_contest_dirty();
        start_server_auto_refresh();
        update_server_actions();
    }

    void stop_local_server(bool log_message) {
        if (!server_process_ || server_process_->state() == QProcess::NotRunning) {
            stop_server_auto_refresh();
            update_server_actions();
            return;
        }
        if (log_message && log_) {
            log_->appendPlainText(text("stopping_server"));
        }
        QProcess* process = server_process_;
        process->setProperty("stopRequested", true);
        process->terminate();
        if (!process->waitForFinished(3000)) {
            process->kill();
            process->waitForFinished(2000);
        }
        stop_server_auto_refresh();
        update_server_actions();
    }


    neothemis::JudgeOptions options_from_ui() const {
        neothemis::JudgeOptions options;
        options.contest_root = contest_root_;
        options.compiler = compiler_;
        options.compile_flags = compile_flags_;
        options.contestants_dir = contestants_dir_;
        options.tests_dir = tests_dir_;
        options.parallel_jobs = parallel_jobs_;
        options.stack_limit_mb = stack_limit_mb_;
        options.keep_workdir = keep_workdir_;
        options.forbidden_patterns = {
            "system(", "popen(", "fork(", "exec(", "#include <unistd.h>",
            "#include <sys/", "#include <windows.h>"
        };
        return options;
    }

    bool archive_operation_available() {
        if (archive_running_.load()) {
            QMessageBox::information(this, text("archive_operation_running"),
                                     text("wait_for_archive_operation"));
            return false;
        }
        if (judging_.load()) {
            QMessageBox::information(this, text("judge"),
                                     text("wait_for_judge_operation"));
            return false;
        }
        return true;
    }

    void join_archive_thread() {
        if (archive_thread_.joinable()) {
            archive_thread_.join();
        }
    }

    void set_archive_controls_enabled(bool enabled) {
        if (judge_selected_button_) {
            judge_selected_button_->setEnabled(enabled);
        }
        if (judge_all_button_) {
            judge_all_button_->setEnabled(enabled);
        }
        if (judge_selected_action_) {
            judge_selected_action_->setEnabled(enabled);
        }
        if (judge_all_action_) {
            judge_all_action_->setEnabled(enabled);
        }
        if (stop_button_) {
            stop_button_->setEnabled(false);
        }
        if (stop_action_) {
            stop_action_->setEnabled(false);
        }
    }

    void begin_archive_operation(const QString& label) {
        join_archive_thread();
        archive_running_.store(true);
        set_archive_controls_enabled(false);
        progress_->setRange(0, 0);
        progress_->setValue(0);
        current_archive_progress_key_.clear();
        log_->appendPlainText(label);
    }

    void update_archive_progress(std::uint64_t done,
                                 std::uint64_t total,
                                 const std::string& label_key) {
        QString label = text(label_key.c_str());
        if (label_key != current_archive_progress_key_) {
            current_archive_progress_key_ = label_key;
            log_->appendPlainText(label);
        }
        if (total == 0) {
            progress_->setRange(0, 0);
            return;
        }

        int max = total > static_cast<std::uint64_t>(std::numeric_limits<int>::max())
                      ? std::numeric_limits<int>::max()
                      : static_cast<int>(total);
        int value = done > total ? max
                    : static_cast<int>((done * static_cast<std::uint64_t>(max)) / total);
        progress_->setRange(0, max);
        progress_->setValue(value);
    }

    ArchiveProgress archive_progress_callback() {
        return [this](std::uint64_t done, std::uint64_t total, const char* label_key) {
            bool queue_update = false;
            {
                std::lock_guard<std::mutex> lock(archive_progress_mutex_);
                pending_archive_progress_done_ = done;
                pending_archive_progress_total_ = total;
                pending_archive_progress_key_ = label_key ? label_key : "";
                if (!archive_progress_update_queued_) {
                    archive_progress_update_queued_ = true;
                    queue_update = true;
                }
            }
            if (!queue_update) {
                return;
            }

            QMetaObject::invokeMethod(this, [this]() {
                std::uint64_t latest_done = 0;
                std::uint64_t latest_total = 0;
                std::string latest_key;
                {
                    std::lock_guard<std::mutex> lock(archive_progress_mutex_);
                    latest_done = pending_archive_progress_done_;
                    latest_total = pending_archive_progress_total_;
                    latest_key = pending_archive_progress_key_;
                    archive_progress_update_queued_ = false;
                }
                if (archive_running_.load()) {
                    update_archive_progress(latest_done, latest_total, latest_key);
                }
            }, Qt::QueuedConnection);
        };
    }

    void finish_archive_operation(const QString& label) {
        join_archive_thread();
        archive_running_.store(false);
        set_archive_controls_enabled(true);
        progress_->setRange(0, 100);
        progress_->setValue(100);
        log_->appendPlainText(label);
    }

    void fail_archive_operation(const QString& label) {
        join_archive_thread();
        archive_running_.store(false);
        set_archive_controls_enabled(true);
        progress_->setRange(0, 100);
        progress_->setValue(0);
        log_->appendPlainText(label);
    }

    void reset_contest_config_defaults() {
        compiler_ = "g++";
        compile_flags_ = "-std=c++14 -O2 -pipe";
        contestants_dir_ = "contestants";
        tests_dir_ = "tests";
        stack_limit_mb_ = 64;
        parallel_jobs_ = 0;
        keep_workdir_ = false;
        server_ranking_enabled_ = false;
        server_contestant_details_enabled_ = false;
    }

    fs::path ensure_ncontest_extension(fs::path path) const {
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (extension != ".ncontest") {
            path += ".ncontest";
        }
        return path;
    }

    bool confirm_discard_unsaved_file(bool close_after_save = false) {
        if (!contest_from_file_ || !contest_dirty_) {
            return true;
        }
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(text("unsaved_title"));
        box.setText(text("unsaved_message"));
        QPushButton* save_button = box.addButton(text("save"), QMessageBox::AcceptRole);
        QPushButton* cancel_button = box.addButton(text("cancel"), QMessageBox::RejectRole);
        QPushButton* close_anyway_button =
            box.addButton(text("close_anyway"), QMessageBox::DestructiveRole);
        close_anyway_button->setObjectName("DangerButton");
        close_anyway_button->setStyleSheet(
            "QPushButton#DangerButton {"
            "background: rgba(138, 35, 55, 220);"
            "border: 1px solid rgba(255, 121, 145, 180);"
            "color: white; border-radius: 8px; padding: 8px 12px; font-weight: 700;"
            "}"
            "QPushButton#DangerButton:hover {"
            "background: rgba(180, 47, 72, 235); border-color: #ff9aae;"
            "}");
        box.setDefaultButton(save_button);
        box.exec();

        QAbstractButton* clicked = box.clickedButton();
        if (clicked == close_anyway_button) {
            pending_close_after_save_ = false;
            return true;
        }
        if (clicked == save_button) {
            pending_close_after_save_ = close_after_save;
            if (!save_contest_container(false)) {
                pending_close_after_save_ = false;
            }
            return false;
        }
        if (clicked == cancel_button) {
            pending_close_after_save_ = false;
        }
        return false;
    }

    void mark_contest_dirty() {
        if (contest_from_file_) {
            contest_dirty_ = true;
        }
    }

    void cleanup_temp_root(const fs::path& root) {
        if (root.empty()) {
            return;
        }
        std::error_code ignored;
        fs::remove_all(root, ignored);
        temporary_roots_.erase(std::remove(temporary_roots_.begin(), temporary_roots_.end(), root),
                               temporary_roots_.end());
    }

    void cleanup_temporary_contests() {
        for (const auto& root : temporary_roots_) {
            std::error_code ignored;
            fs::remove_all(root, ignored);
        }
        temporary_roots_.clear();
        active_temp_root_.clear();
    }

    bool prepare_to_replace_contest() {
        if (!confirm_discard_unsaved_file()) {
            return false;
        }
        cleanup_temp_root(active_temp_root_);
        active_temp_root_.clear();
        contest_from_file_ = false;
        contest_dirty_ = false;
        contest_file_path_.clear();
        return true;
    }

    fs::path make_temporary_contest_root() {
        fs::create_directories(temporary_dir_);
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 1000; ++attempt) {
            fs::path root = temporary_dir_ /
                ("ncontest-" + std::to_string(now) + "-" + std::to_string(attempt));
            std::error_code ec;
            if (fs::create_directory(root, ec)) {
                temporary_roots_.push_back(root);
                return root;
            }
        }
        throw std::runtime_error("failed to create temporary contest folder");
    }

    fs::path detect_extracted_contest_root(const fs::path& root) const {
        if (fs::exists(root / "neothemis.conf")) {
            return root;
        }
        std::vector<fs::path> candidates;
        for (const auto& entry : fs::directory_iterator(root)) {
            if (entry.is_directory() && fs::exists(entry.path() / "neothemis.conf")) {
                candidates.push_back(entry.path());
            }
        }
        return candidates.size() == 1 ? candidates.front() : root;
    }

    void open_contest() {
        if (!archive_operation_available()) {
            return;
        }
        QString dir = QFileDialog::getExistingDirectory(this, text("open_folder"));
        if (dir.isEmpty()) {
            return;
        }
        if (!prepare_to_replace_contest()) {
            return;
        }
        contest_root_ = dir.toStdString();
        contest_from_file_ = false;
        contest_dirty_ = false;
        contest_file_path_.clear();
        contest_title_->setText(QString::fromStdString(contest_root_.filename().string()));
        load_contest_config();
        refresh_table();
    }

    void open_contest_file() {
        QString selected = QFileDialog::getOpenFileName(
            this, text("open_contest_file"), QString(),
            text("ncontest_open_filter"));
        if (selected.isEmpty()) {
            return;
        }
        open_contest_file(path_from_qstring(selected));
    }

    void open_contest_file(const fs::path& archive_path) {
        if (!archive_operation_available()) {
            return;
        }
        if (!prepare_to_replace_contest()) {
            return;
        }

        fs::path temp_root;
        try {
            temp_root = make_temporary_contest_root();
            begin_archive_operation(text("opening_contest_file"));
            ArchiveProgress progress = archive_progress_callback();
            archive_thread_ = std::thread([this, archive_path, temp_root, progress]() {
                try {
                    extract_zip_file(archive_path, temp_root, progress);
                    QMetaObject::invokeMethod(this, [this, archive_path, temp_root]() {
                        try {
                            contest_root_ = detect_extracted_contest_root(temp_root);
                            contest_file_path_ = archive_path;
                            active_temp_root_ = temp_root;
                            contest_from_file_ = true;
                            contest_dirty_ = false;
                            contest_title_->setText(QString::fromStdString(contest_file_path_.filename().string()));
                            load_contest_config();
                            refresh_table();
                            finish_archive_operation(text("file_operation_complete"));
                        } catch (const std::exception& ex) {
                            cleanup_temp_root(temp_root);
                            fail_archive_operation(text("file_operation_failed"));
                            QMessageBox::critical(this, text("open_failed"), ex.what());
                        }
                    }, Qt::QueuedConnection);
                } catch (const std::exception& ex) {
                    QMetaObject::invokeMethod(this, [this, temp_root, message = QString::fromUtf8(ex.what())]() {
                        cleanup_temp_root(temp_root);
                        fail_archive_operation(text("file_operation_failed"));
                        QMessageBox::critical(this, text("open_failed"), message);
                    }, Qt::QueuedConnection);
                }
            });
        } catch (const std::exception& ex) {
            cleanup_temp_root(temp_root);
            QMessageBox::critical(this, text("open_failed"), ex.what());
        }
    }

    void load_contest_config() {
        reset_contest_config_defaults();
        auto values = read_config_file(contest_root_ / "neothemis.conf");
        if (values.count("compiler")) compiler_ = values["compiler"];
        if (values.count("compile_flags")) compile_flags_ = values["compile_flags"];
        if (values.count("contestants_dir")) contestants_dir_ = values["contestants_dir"];
        if (values.count("tests_dir")) tests_dir_ = values["tests_dir"];
        if (values.count("stack_limit_mb")) stack_limit_mb_ = std::stoull(values["stack_limit_mb"]);
        if (values.count("parallel_jobs")) parallel_jobs_ = std::stoul(values["parallel_jobs"]);
        if (values.count("keep_workdir")) {
            std::string value = values["keep_workdir"];
            keep_workdir_ = value == "true" || value == "1" || value == "yes" || value == "on";
        }
        if (values.count("server_ranking_enabled")) {
            std::string value = values["server_ranking_enabled"];
            server_ranking_enabled_ =
                value == "true" || value == "1" || value == "yes" || value == "on";
        }
        if (values.count("server_contestant_details_enabled")) {
            std::string value = values["server_contestant_details_enabled"];
            server_contestant_details_enabled_ =
                value == "true" || value == "1" || value == "yes" || value == "on";
        }
    }

    void save_contest_config() const {
        if (contest_root_.empty()) {
            return;
        }
        std::ofstream out(contest_root_ / "neothemis.conf");
        if (!out) {
            throw std::runtime_error("failed to write neothemis.conf");
        }
        out << "core=builtin\n"
            << "contestants_dir=" << contestants_dir_ << '\n'
            << "tests_dir=" << tests_dir_ << '\n'
            << "output_csv=results.csv\n"
            << "scoreboard_csv=scoreboard.csv\n"
            << "keep_workdir=" << (keep_workdir_ ? "true" : "false") << '\n'
            << "server_ranking_enabled=" << (server_ranking_enabled_ ? "true" : "false") << '\n'
            << "server_contestant_details_enabled="
            << (server_contestant_details_enabled_ ? "true" : "false") << '\n'
            << "compiler=" << compiler_ << '\n'
            << "compile_flags=" << compile_flags_ << '\n'
            << "stack_limit_mb=" << stack_limit_mb_ << '\n'
            << "parallel_jobs=" << parallel_jobs_ << '\n'
            << "forbidden_pattern=system(\n"
            << "forbidden_pattern=popen(\n"
            << "forbidden_pattern=fork(\n"
            << "forbidden_pattern=exec(\n"
            << "forbidden_pattern=#include <unistd.h>\n"
            << "forbidden_pattern=#include <sys/\n"
            << "forbidden_pattern=#include <windows.h>\n";
    }

    bool save_contest_container(bool save_as) {
        if (!archive_operation_available()) {
            return false;
        }
        if (contest_root_.empty()) {
            QMessageBox::information(this, text("contest"), text("no_contest_open"));
            return false;
        }

        try {
            save_contest_config();
            if (contest_from_file_) {
                fs::path target = contest_file_path_;
                if (save_as || target.empty()) {
                    QString selected = QFileDialog::getSaveFileName(
                        this, text("save_contest_file_as"),
                        QString::fromStdString((target.empty()
                            ? fs::path("contest.ncontest")
                            : target).string()),
                        text("ncontest_save_filter"));
                    if (selected.isEmpty()) {
                        return false;
                    }
                    target = ensure_ncontest_extension(selected.toStdString());
                }
                fs::path source_root = contest_root_;
                bool was_save_as = save_as;
                begin_archive_operation(text("saving_contest_file"));
                ArchiveProgress progress = archive_progress_callback();
                archive_thread_ = std::thread([this, source_root, target, was_save_as, progress]() {
                    try {
                        write_contest_archive(target, source_root, progress);
                        QMetaObject::invokeMethod(this, [this, target, was_save_as]() {
                            contest_file_path_ = target;
                            contest_dirty_ = false;
                            contest_title_->setText(QString::fromStdString(contest_file_path_.filename().string()));
                            finish_archive_operation(text("file_operation_complete"));
                            log_->appendPlainText(text(was_save_as ? "save_as_complete" : "save_complete") +
                                                  ": " + QString::fromStdString(target.string()));
                            if (pending_close_after_save_) {
                                pending_close_after_save_ = false;
                                QTimer::singleShot(0, this, &QWidget::close);
                            }
                        }, Qt::QueuedConnection);
                    } catch (const std::exception& ex) {
                        QMetaObject::invokeMethod(this, [this, message = QString::fromUtf8(ex.what())]() {
                            pending_close_after_save_ = false;
                            mark_contest_dirty();
                            fail_archive_operation(text("file_operation_failed"));
                            QMessageBox::critical(this, text("save_failed"), message);
                        }, Qt::QueuedConnection);
                    }
                });
            } else {
                fs::path default_target = contest_file_path_.empty()
                                              ? contest_root_.parent_path() /
                                                    (contest_root_.filename().string() + ".ncontest")
                                              : contest_file_path_;
                QString selected = QFileDialog::getSaveFileName(
                    this, text(save_as ? "save_contest_file_as" : "save_contest_file"),
                    QString::fromStdString(default_target.string()),
                    text("ncontest_save_filter"));
                if (selected.isEmpty()) {
                    return false;
                }
                fs::path source_root = contest_root_;
                fs::path target = ensure_ncontest_extension(selected.toStdString());
                bool was_save_as = save_as;
                begin_archive_operation(text("saving_contest_file"));
                ArchiveProgress progress = archive_progress_callback();
                archive_thread_ = std::thread([this, source_root, target, was_save_as, progress]() {
                    try {
                        write_contest_archive(target, source_root, progress);
                        QMetaObject::invokeMethod(this, [this, target, was_save_as]() {
                            contest_file_path_ = target;
                            contest_from_file_ = true;
                            contest_dirty_ = false;
                            contest_title_->setText(QString::fromStdString(contest_file_path_.filename().string()));
                            finish_archive_operation(text("file_operation_complete"));
                            log_->appendPlainText(text(was_save_as ? "save_as_complete" : "save_complete") +
                                                  ": " + QString::fromStdString(target.string()));
                            if (pending_close_after_save_) {
                                pending_close_after_save_ = false;
                                QTimer::singleShot(0, this, &QWidget::close);
                            }
                        }, Qt::QueuedConnection);
                    } catch (const std::exception& ex) {
                        QMetaObject::invokeMethod(this, [this, message = QString::fromUtf8(ex.what())]() {
                            pending_close_after_save_ = false;
                            fail_archive_operation(text("file_operation_failed"));
                            QMessageBox::critical(this, text("save_failed"), message);
                        }, Qt::QueuedConnection);
                    }
                });
            }
        } catch (const std::exception& ex) {
            pending_close_after_save_ = false;
            mark_contest_dirty();
            QMessageBox::critical(this, text("save_failed"), ex.what());
            return false;
        }
        return true;
    }

    void convert_old_contest_to_ncontest(bool source_is_folder) {
        if (!archive_operation_available()) {
            return;
        }

        QString source;
        if (source_is_folder) {
            source = QFileDialog::getExistingDirectory(
                this, text("convert_old_contest_folder"));
        } else {
            source = QFileDialog::getOpenFileName(
                this, text("convert_old_contest_file"), QString(),
                text("themis_contest_filter"));
        }
        if (source.isEmpty()) {
            return;
        }

        fs::path source_path = source.toStdString();
        fs::path default_output = source_path;
        if (source_is_folder) {
            default_output = source_path.parent_path() /
                             (source_path.filename().string() + ".ncontest");
        } else {
            default_output.replace_extension(".ncontest");
        }

        QString selected_output = QFileDialog::getSaveFileName(
            this, text("convert_old_contest_file"),
            QString::fromStdString(default_output.string()),
            text("ncontest_save_filter"));
        if (selected_output.isEmpty()) {
            return;
        }

        fs::path output_path = ensure_ncontest_extension(selected_output.toStdString());
        begin_archive_operation(text("converting_contest_file"));
        ArchiveProgress ui_progress = archive_progress_callback();
        neothemis::ArchiveProgress core_progress =
            [ui_progress](std::uint64_t done,
                          std::uint64_t total,
                          const std::string& label) {
                ui_progress(done, total, label.c_str());
            };

        archive_thread_ = std::thread([this, source_path, output_path, core_progress]() {
            try {
                neothemis::convert_old_themis_contest(source_path, output_path, core_progress);
                QMetaObject::invokeMethod(this, [this, output_path]() {
                    finish_archive_operation(text("convert_complete"));
                    log_->appendPlainText(text("convert_complete") + ": " +
                                          QString::fromStdString(output_path.string()));
                    QMessageBox::information(this, text("converter"),
                                             text("convert_complete") + "\n" +
                                             QString::fromStdString(output_path.string()));
                }, Qt::QueuedConnection);
            } catch (const std::exception& ex) {
                QMetaObject::invokeMethod(this, [this, message = QString::fromUtf8(ex.what())]() {
                    fail_archive_operation(text("file_operation_failed"));
                    QMessageBox::critical(this, text("file_operation_failed"), message);
                }, Qt::QueuedConnection);
            }
        });
    }

    void add_contestants_from_folder() {
        if (!archive_operation_available()) {
            return;
        }
        if (contest_root_.empty()) {
            QMessageBox::information(this, text("no_contest"), text("open_contest_first"));
            return;
        }

        QString selected = QFileDialog::getExistingDirectory(
            this, text("add_contestants_from_folder"));
        if (selected.isEmpty()) {
            return;
        }

        try {
            fs::path source_root = path_from_qstring(selected);
            std::vector<fs::path> sources;
            for (const auto& entry : fs::directory_iterator(source_root)) {
                if (entry.is_directory()) {
                    sources.push_back(entry.path());
                }
            }
            if (sources.empty()) {
                sources.push_back(source_root);
            }
            std::sort(sources.begin(), sources.end());

            fs::path destination_root = contest_root_ / contestants_dir_;
            int imported = 0;
            int skipped = 0;
            fs::create_directories(destination_root);
            for (const fs::path& source : sources) {
                std::string name = source.filename().string();
                if (name.empty() || name == "." || name == "..") {
                    ++skipped;
                    continue;
                }
                fs::path destination = destination_root / name;
                std::error_code equivalent_error;
                if (fs::exists(destination) &&
                    fs::equivalent(source, destination, equivalent_error) &&
                    !equivalent_error) {
                    ++skipped;
                    continue;
                }
                fs::create_directories(destination);
                fs::copy(source, destination,
                         fs::copy_options::recursive |
                             fs::copy_options::overwrite_existing |
                             fs::copy_options::skip_symlinks);
                ++imported;
            }
            mark_contest_dirty();
            refresh_table(false);
            log_->appendPlainText(text("contestants_imported")
                                  .arg(imported).arg(skipped));
        } catch (const std::exception& ex) {
            QMessageBox::critical(this, text("contestant_import_failed"), ex.what());
        }
    }

    void refresh_table(bool log_opened = true) {
        if (contest_root_.empty()) {
            return;
        }
        try {
            auto overview = neothemis::inspect_contest(options_from_ui());
            contestants_ = overview.contestants;
            problems_ = overview.problems;
            source_ready_.clear();
            cell_texts_.clear();
            for (std::size_t row = 0; row < overview.contestants.size(); ++row) {
                for (std::size_t col = 0; col < overview.problems.size(); ++col) {
                    source_ready_[cell_key(overview.contestants[row], overview.problems[col])] =
                        overview.has_source[row][col];
                }
            }
            load_problem_test_counts();
            load_existing_results();
            populate_table();
            if (log_opened) {
                log_->appendPlainText(text("opened") + " " +
                                      QString::fromStdString(contest_root_.string()));
            }
        } catch (const std::exception& ex) {
            QMessageBox::critical(this, text("open_failed"), ex.what());
        }
    }

    fs::path contest_output_path(const fs::path& path) const {
        return path.is_relative() ? contest_root_ / path : path;
    }

    void record_result(const neothemis::TestResult& result) {
        std::string key = cell_key(result.contestant, result.problem);
        result_details_[key].push_back(result);
        CellScore& score = score_cells_[key];
        score.earned += result.earned_points;
        score.max += result.max_points;
        ++score.completed;
    }

    std::vector<neothemis::TestResult> all_recorded_results() const {
        std::vector<neothemis::TestResult> rows;
        for (const auto& entry : result_details_) {
            rows.insert(rows.end(), entry.second.begin(), entry.second.end());
        }
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            return std::tie(a.contestant, a.problem, a.test) <
                   std::tie(b.contestant, b.problem, b.test);
        });
        return rows;
    }

    void load_existing_results() {
        score_cells_.clear();
        result_details_.clear();

        neothemis::JudgeOptions options = options_from_ui();
        fs::path details_path = contest_output_path(options.output_csv);
        if (!fs::exists(details_path)) {
            return;
        }

        std::set<std::string> known_contestants(contestants_.begin(), contestants_.end());
        std::set<std::string> known_problems(problems_.begin(), problems_.end());
        auto records = read_csv_records(details_path);
        if (records.size() <= 1) {
            return;
        }
        for (std::size_t i = 1; i < records.size(); ++i) {
            const auto& fields = records[i];
            if (fields.size() < 9 ||
                known_contestants.count(fields[0]) == 0 ||
                known_problems.count(fields[1]) == 0) {
                continue;
            }
            try {
                neothemis::TestResult result;
                result.contestant = fields[0];
                result.problem = fields[1];
                result.test = fields[2];
                result.verdict = verdict_from_string(fields[3]);
                result.time_ms = static_cast<std::uint64_t>(std::stoull(fields[4]));
                result.exit_code = std::stoi(fields[5]);
                result.max_points = std::stod(fields[6]);
                result.earned_points = std::stod(fields[7]);
                result.message = fields[8];
                record_result(result);
            } catch (...) {
                log_->appendPlainText(text("malformed_row_skipped"));
            }
        }
    }

    void write_current_csv_outputs() {
        neothemis::JudgeOptions options = options_from_ui();
        std::vector<neothemis::TestResult> rows = all_recorded_results();

        fs::path details_path = contest_output_path(options.output_csv);
        fs::create_directories(details_path.parent_path());
        std::ofstream details(details_path);
        if (!details) {
            throw std::runtime_error("failed to open CSV output: " + details_path.string());
        }
        neothemis::write_csv(details, rows);

        fs::path scoreboard_path = contest_output_path(options.scoreboard_csv);
        fs::create_directories(scoreboard_path.parent_path());
        std::ofstream scoreboard(scoreboard_path);
        if (!scoreboard) {
            throw std::runtime_error("failed to open scoreboard CSV output: " +
                                     scoreboard_path.string());
        }
        neothemis::write_scoreboard_csv(scoreboard, rows);
        mark_contest_dirty();
    }

    std::string status_for_scoreboard_cell(const std::string& contestant,
                                           const std::string& problem) const {
        auto found = result_details_.find(cell_key(contestant, problem));
        if (found == result_details_.end()) {
            return {};
        }
        std::string status;
        int priority = 0;
        for (const auto& result : found->second) {
            int candidate_priority = 0;
            std::string candidate;
            if (result.verdict == neothemis::Verdict::CompileError) {
                candidate = "CE";
                candidate_priority = 2;
            } else if (result.verdict == neothemis::Verdict::MissingSource) {
                candidate = "MS";
                candidate_priority = 1;
            }
            if (candidate_priority > priority) {
                status = candidate;
                priority = candidate_priority;
            }
        }
        return status;
    }

    QString terminal_status_for_cell(const std::string& contestant,
                                     const std::string& problem) const {
        std::string status = status_for_scoreboard_cell(contestant, problem);
        if (status == "CE") {
            return "CE";
        }
        if (status == "MS") {
            return text("missing");
        }
        return {};
    }

    fs::path choose_export_path(const std::string& filename) {
        QString default_path = contest_root_.empty()
                                   ? QString::fromStdString(filename)
                                   : QString::fromStdString((contest_root_ / filename).string());
        QString selected = QFileDialog::getSaveFileName(
            this, text("export_workbook"), default_path, text("xlsx_filter"));
        if (selected.isEmpty()) {
            return {};
        }
        fs::path path = selected.toStdString();
        if (path.extension().string() != ".xlsx") {
            path += ".xlsx";
        }
        return path;
    }

    bool export_is_available() {
        if (archive_running_.load()) {
            QMessageBox::information(this, text("archive_operation_running"),
                                     text("wait_for_archive_operation"));
            return false;
        }
        if (judging_.load()) {
            QMessageBox::information(this, text("judge_running"),
                                     text("wait_for_export_judge"));
            return false;
        }
        if (contest_root_.empty()) {
            QMessageBox::information(this, text("no_contest"),
                                     text("open_contest_first"));
            return false;
        }
        if (all_recorded_results().empty()) {
            QMessageBox::information(this, text("no_results"),
                                     text("no_results_detail"));
            return false;
        }
        return true;
    }

    void export_scoreboard_xlsx() {
        if (!export_is_available()) {
            return;
        }
        fs::path path = choose_export_path("scoreboard.xlsx");
        if (path.empty()) {
            return;
        }

        std::vector<XlsxRow> rows;
        XlsxRow header{xlsx_text("Contestant")};
        for (const auto& problem : problems_) {
            header.push_back(xlsx_text(problem));
        }
        header.push_back(xlsx_text("Total"));
        rows.push_back(std::move(header));

        for (const auto& contestant : contestants_) {
            XlsxRow row{xlsx_text(contestant)};
            for (const auto& problem : problems_) {
                double score = earned_for_problem(contestant, problem);
                std::string status = status_for_scoreboard_cell(contestant, problem);
                if (score == 0.0 && !status.empty()) {
                    row.push_back(xlsx_text(status + "(0)"));
                } else {
                    row.push_back(xlsx_number(score));
                }
            }
            row.push_back(xlsx_number(total_earned_for(contestant)));
            rows.push_back(std::move(row));
        }

        std::vector<double> widths(rows.front().size(), 14.0);
        widths[0] = 28.0;
        try {
            write_xlsx_file(path, "Scoreboard", rows, widths);
            log_->appendPlainText(text("exported") + " " + QString::fromStdString(path.string()));
            QMessageBox::information(this, text("export_complete"),
                                     text("exported") + " " + QString::fromStdString(path.string()));
        } catch (const std::exception& ex) {
            QMessageBox::critical(this, text("export_failed"), ex.what());
        }
    }

    void export_data_xlsx() {
        if (!export_is_available()) {
            return;
        }
        fs::path path = choose_export_path("results-data.xlsx");
        if (path.empty()) {
            return;
        }

        std::vector<XlsxRow> rows;
        rows.push_back({
            xlsx_text("contestant"),
            xlsx_text("problem"),
            xlsx_text("test"),
            xlsx_text("verdict"),
            xlsx_text("time_ms"),
            xlsx_text("exit_code"),
            xlsx_text("max_points"),
            xlsx_text("earned_points"),
            xlsx_text("message")
        });
        for (const auto& result : all_recorded_results()) {
            rows.push_back({
                xlsx_text(result.contestant),
                xlsx_text(result.problem),
                xlsx_text(result.test),
                xlsx_text(neothemis::to_string(result.verdict)),
                xlsx_number(static_cast<double>(result.time_ms)),
                xlsx_number(static_cast<double>(result.exit_code)),
                xlsx_number(result.max_points),
                xlsx_number(result.earned_points),
                xlsx_text(result.message)
            });
        }

        std::vector<double> widths{28.0, 14.0, 12.0, 10.0, 12.0, 12.0, 12.0, 14.0, 48.0};
        try {
            write_xlsx_file(path, "Data", rows, widths);
            log_->appendPlainText(text("exported") + " " + QString::fromStdString(path.string()));
            QMessageBox::information(this, text("export_complete"),
                                     text("exported") + " " + QString::fromStdString(path.string()));
        } catch (const std::exception& ex) {
            QMessageBox::critical(this, text("export_failed"), ex.what());
        }
    }

    void rebuild_maps() {
        contestant_rows_.clear();
        problem_columns_.clear();
        for (std::size_t i = 0; i < contestants_.size(); ++i) {
            contestant_rows_[contestants_[i]] = static_cast<int>(i);
        }
        for (std::size_t i = 0; i < problems_.size(); ++i) {
            problem_columns_[problems_[i]] = static_cast<int>(i + 1);
        }
    }

    int total_column() const {
        return static_cast<int>(problems_.size() + 1);
    }

    std::string cell_key(const std::string& contestant, const std::string& problem) const {
        return contestant + "\n" + problem;
    }

    double earned_for_problem(const std::string& contestant, const std::string& problem) const {
        auto it = score_cells_.find(cell_key(contestant, problem));
        return it == score_cells_.end() ? 0.0 : it->second.earned;
    }

    double max_for_problem(const std::string& contestant, const std::string& problem) const {
        auto it = score_cells_.find(cell_key(contestant, problem));
        return it == score_cells_.end() ? 0.0 : it->second.max;
    }

    double total_earned_for(const std::string& contestant) const {
        double total = 0.0;
        for (const auto& problem : problems_) {
            total += earned_for_problem(contestant, problem);
        }
        return total;
    }

    double total_max_for(const std::string& contestant) const {
        double total = 0.0;
        for (const auto& problem : problems_) {
            total += max_for_problem(contestant, problem);
        }
        return total;
    }

    QString score_cell_text(const CellScore& score, int expected) const {
        QString score_text = format_points(score.earned) + "/" + format_points(score.max);
        if (score.completed >= expected) {
            return format_points(score.earned);
        }
        return score_text + "\n" + text("running") + " " +
               QString::number(score.completed) + "/" + QString::number(expected);
    }

    QString problem_cell_text(const std::string& contestant, const std::string& problem) const {
        std::string key = cell_key(contestant, problem);
        auto text_it = cell_texts_.find(key);
        if (text_it != cell_texts_.end()) {
            return text_it->second;
        }

        auto score_it = score_cells_.find(key);
        if (score_it == score_cells_.end()) {
            auto source_it = source_ready_.find(key);
            return source_it != source_ready_.end() && source_it->second
                       ? text("ready")
                       : text("missing");
        }
        QString terminal_status = terminal_status_for_cell(contestant, problem);
        if (!terminal_status.isEmpty()) {
            return terminal_status;
        }
        const CellScore& score = score_it->second;
        int expected = 1;
        auto expected_it = problem_test_counts_.find(problem);
        if (expected_it != problem_test_counts_.end()) {
            expected = expected_it->second;
        }
        return score_cell_text(score, expected);
    }

    void populate_table() {
        rebuild_maps();
        table_->clear();
        table_->setRowCount(static_cast<int>(contestants_.size()));
        table_->setColumnCount(total_column() + 1);
        table_->setHorizontalHeaderItem(0, new QTableWidgetItem(text("contestant")));
        for (std::size_t col = 0; col < problems_.size(); ++col) {
            table_->setHorizontalHeaderItem(static_cast<int>(col + 1),
                                            new QTableWidgetItem(QString::fromStdString(problems_[col])));
        }
        table_->setHorizontalHeaderItem(total_column(), new QTableWidgetItem(text("total")));

        for (std::size_t row = 0; row < contestants_.size(); ++row) {
            const std::string& contestant = contestants_[row];
            auto* name_item = new QTableWidgetItem(QString::fromStdString(contestant));
            style_name_item(name_item);
            table_->setItem(static_cast<int>(row), 0, name_item);
            for (std::size_t col = 0; col < problems_.size(); ++col) {
                auto* item = new QTableWidgetItem(problem_cell_text(contestant, problems_[col]));
                item->setTextAlignment(Qt::AlignCenter);
                style_problem_item(contestant, problems_[col], item);
                table_->setItem(static_cast<int>(row), static_cast<int>(col + 1), item);
            }
            update_total_cell(contestant);
        }
    }

    void load_problem_test_counts() {
        problem_test_counts_.clear();
        fs::path tests_root = contest_root_ / tests_dir_;
        for (const auto& problem : problems_) {
            int count = 0;
            fs::path problem_root = tests_root / problem;
            if (fs::exists(problem_root)) {
                for (const auto& entry : fs::directory_iterator(problem_root)) {
                    if (entry.is_directory()) {
                        ++count;
                    }
                }
            }
            problem_test_counts_[problem] = std::max(1, count);
        }
    }

    std::vector<std::string> selected_contestants() const {
        std::set<int> rows;
        for (const QModelIndex& index : table_->selectionModel()->selectedRows()) {
            rows.insert(index.row());
        }
        std::vector<std::string> selected;
        for (int row : rows) {
            if (row >= 0 && static_cast<std::size_t>(row) < contestants_.size()) {
                selected.push_back(contestants_[static_cast<std::size_t>(row)]);
            }
        }
        return selected;
    }

    void reset_run_cells(const std::vector<std::string>& selected, const std::string& selected_problem) {
        std::set<std::string> selected_set(selected.begin(), selected.end());
        for (const auto& contestant : contestants_) {
            if (!selected_set.empty() && selected_set.count(contestant) == 0) {
                continue;
            }
            for (const auto& problem : problems_) {
                if (!selected_problem.empty() && problem != selected_problem) {
                    continue;
                }
                std::string key = cell_key(contestant, problem);
                score_cells_.erase(key);
                result_details_.erase(key);
                cell_texts_.erase(key);
                set_table_cell(contestant, problem, text("queued"));
            }
            update_total_cell(contestant);
        }
    }

    void set_table_cell(const std::string& contestant,
                        const std::string& problem,
                        const QString& text) {
        auto row_it = contestant_rows_.find(contestant);
        auto col_it = problem_columns_.find(problem);
        if (row_it == contestant_rows_.end() || col_it == problem_columns_.end()) {
            return;
        }
        auto* item = table_->item(row_it->second, col_it->second);
        if (!item) {
            item = new QTableWidgetItem;
            table_->setItem(row_it->second, col_it->second, item);
        }
        cell_texts_[cell_key(contestant, problem)] = text;
        item->setText(text);
        item->setTextAlignment(Qt::AlignCenter);
        style_problem_item(contestant, problem, item);
    }

    void update_total_cell(const std::string& contestant) {
        auto row_it = contestant_rows_.find(contestant);
        if (row_it == contestant_rows_.end()) {
            return;
        }
        auto* item = table_->item(row_it->second, total_column());
        if (!item) {
            item = new QTableWidgetItem;
            table_->setItem(row_it->second, total_column(), item);
        }
        item->setText(format_points(total_earned_for(contestant)) + "/" +
                      format_points(total_max_for(contestant)));
        item->setTextAlignment(Qt::AlignCenter);
        style_total_item(contestant, item);
    }

    void style_name_item(QTableWidgetItem* item) const {
        if (!item) {
            return;
        }
        item->setForeground(QColor("#e7fbff"));
        item->setBackground(QColor("#151b23"));
    }

    void style_problem_item(const std::string& contestant,
                            const std::string& problem,
                            QTableWidgetItem* item) const {
        if (!item) {
            return;
        }
        std::string key = cell_key(contestant, problem);
        QString terminal_status = terminal_status_for_cell(contestant, problem);
        if (!terminal_status.isEmpty()) {
            item->setForeground(QColor("#ff8fab"));
            item->setBackground(QColor("#30151f"));
            return;
        }
        auto score_it = score_cells_.find(key);
        if (score_it != score_cells_.end()) {
            const CellScore& score = score_it->second;
            int expected = 1;
            auto expected_it = problem_test_counts_.find(problem);
            if (expected_it != problem_test_counts_.end()) {
                expected = expected_it->second;
            }
            if (score.completed < expected) {
                item->setForeground(QColor("#7df9ff"));
                item->setBackground(QColor("#122631"));
            } else if (score.max > 0.0 && score.earned + 1e-9 >= score.max) {
                item->setForeground(QColor("#99ffcc"));
                item->setBackground(QColor("#123028"));
            } else if (score.earned > 0.0) {
                item->setForeground(QColor("#ffe680"));
                item->setBackground(QColor("#302512"));
            } else {
                item->setForeground(QColor("#ff8fab"));
                item->setBackground(QColor("#30151f"));
            }
            return;
        }

        QString text = item->text().toLower();
        if (text.contains(this->text("queued").toLower()) || text.contains("queued")) {
            item->setForeground(QColor("#ffe680"));
            item->setBackground(QColor("#2a2412"));
            return;
        }
        auto source_it = source_ready_.find(key);
        if (source_it != source_ready_.end() && source_it->second) {
            item->setForeground(QColor("#7df9ff"));
            item->setBackground(QColor("#122631"));
        } else {
            item->setForeground(QColor("#ff8fab"));
            item->setBackground(QColor("#281821"));
        }
    }

    void style_total_item(const std::string& contestant, QTableWidgetItem* item) const {
        if (!item) {
            return;
        }
        double earned = total_earned_for(contestant);
        double max = total_max_for(contestant);
        if (max > 0.0 && earned + 1e-9 >= max) {
            item->setForeground(QColor("#99ffcc"));
            item->setBackground(QColor("#102a24"));
        } else if (earned > 0.0) {
            item->setForeground(QColor("#ffe680"));
            item->setBackground(QColor("#2c2312"));
        } else {
            item->setForeground(QColor("#7df9ff"));
            item->setBackground(QColor("#121f2a"));
        }
    }

    void handle_result(const neothemis::TestResult& result) {
        record_result(result);
        const CellScore& score = score_cells_[cell_key(result.contestant, result.problem)];

        QString terminal_status = terminal_status_for_cell(result.contestant, result.problem);
        if (!terminal_status.isEmpty()) {
            set_table_cell(result.contestant, result.problem, terminal_status);
            update_total_cell(result.contestant);
            return;
        }

        int expected = problem_test_counts_[result.problem];
        set_table_cell(result.contestant, result.problem, score_cell_text(score, expected));
        update_total_cell(result.contestant);
    }

    void show_result_details(int row, int col) {
        if (row < 0 || col <= 0 ||
            static_cast<std::size_t>(row) >= contestants_.size() ||
            static_cast<std::size_t>(col - 1) >= problems_.size()) {
            return;
        }

        std::string contestant = contestants_[static_cast<std::size_t>(row)];
        std::string problem = problems_[static_cast<std::size_t>(col - 1)];
        std::string key = contestant + "\n" + problem;
        std::vector<neothemis::TestResult> rows = result_details_[key];
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            return a.test < b.test;
        });

        std::uint64_t maximum_time_ms = 0;
        for (const auto& result : rows) {
            maximum_time_ms = std::max(maximum_time_ms, result.time_ms);
        }

        auto* dialog = new QDialog(this);
        dialog->setWindowTitle(QString::fromStdString(contestant + " - " + problem));
        dialog->resize(640, 520);
        auto* layout = new QVBoxLayout(dialog);
        auto* title = new QLabel(QString::fromStdString(contestant + " / " + problem), dialog);
        title->setObjectName("AppTitle");
        auto* maximum_time = new QLabel(
            this->text("maximum_test_time") + ": " + QString::number(maximum_time_ms) + " ms",
            dialog);
        auto* details = new QTableWidget(dialog);
        details->setColumnCount(4);
        details->setHorizontalHeaderLabels({this->text("test"), this->text("point"),
                                             this->text("run_time"),
                                             this->text("description")});
        details->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        details->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        details->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        details->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
        details->verticalHeader()->setVisible(false);
        details->setEditTriggers(QAbstractItemView::NoEditTriggers);
        details->setSelectionMode(QAbstractItemView::NoSelection);
        details->setTextElideMode(Qt::ElideRight);
        details->setMouseTracking(true);
        details->setRowCount(static_cast<int>(rows.size()));
        for (std::size_t row = 0; row < rows.size(); ++row) {
            const auto& result = rows[row];
            QString description = QString::fromStdString(neothemis::to_string(result.verdict));
            if (!result.message.empty()) {
                description += ": " + QString::fromStdString(result.message);
            }
            details->setItem(static_cast<int>(row), 0,
                             new QTableWidgetItem(QString::fromStdString(result.test)));
            details->setItem(static_cast<int>(row), 1,
                             new QTableWidgetItem(format_points(result.earned_points) + "/" +
                                                  format_points(result.max_points)));
            details->setItem(static_cast<int>(row), 2,
                             new QTableWidgetItem(QString::number(result.time_ms) + " ms"));
            auto* description_item = new QTableWidgetItem(description);
            description_item->setData(Qt::UserRole, description);
            description_item->setToolTip(this->text("open_full_description"));
            details->setItem(static_cast<int>(row), 3, description_item);
        }
        if (rows.empty()) {
            details->setRowCount(1);
            details->setItem(0, 0, new QTableWidgetItem(this->text("no_judged_tests")));
            details->setSpan(0, 0, 1, 4);
        }
        QObject::connect(details, &QTableWidget::cellEntered,
                         [details](int, int column) {
            details->viewport()->setCursor(column == 3 ? Qt::PointingHandCursor
                                                        : Qt::ArrowCursor);
        });
        QObject::connect(details, &QTableWidget::cellClicked,
                         [this, dialog, details](int row, int column) {
            if (column != 3) {
                return;
            }
            QTableWidgetItem* item = details->item(row, column);
            if (!item) {
                return;
            }
            auto* description_dialog = new QDialog(dialog);
            description_dialog->setWindowTitle(this->text("full_description"));
            description_dialog->resize(760, 500);
            auto* description_layout = new QVBoxLayout(description_dialog);
            auto* full_description = new QPlainTextEdit(description_dialog);
            full_description->setReadOnly(true);
            full_description->setPlainText(item->data(Qt::UserRole).toString());
            auto* close_buttons = new QDialogButtonBox(QDialogButtonBox::Close,
                                                       description_dialog);
            QObject::connect(close_buttons, &QDialogButtonBox::rejected,
                             description_dialog, &QDialog::close);
            description_layout->addWidget(full_description, 1);
            description_layout->addWidget(close_buttons);
            description_dialog->setAttribute(Qt::WA_DeleteOnClose);
            description_dialog->show();
        });
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
        layout->addWidget(title);
        layout->addWidget(maximum_time);
        layout->addWidget(details, 1);
        layout->addWidget(buttons);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    }

    void sort_by_column(int section) {
        if (section < 0 || section > total_column()) {
            return;
        }
        if (sort_column_ == section) {
            sort_ascending_ = !sort_ascending_;
        } else {
            sort_column_ = section;
            sort_ascending_ = true;
        }

        auto name_less = [](const std::string& a, const std::string& b) {
            return QString::fromStdString(a).toCaseFolded() <
                   QString::fromStdString(b).toCaseFolded();
        };

        std::stable_sort(contestants_.begin(), contestants_.end(),
                         [&](const std::string& a, const std::string& b) {
            int cmp = 0;
            if (section == 0) {
                cmp = name_less(a, b) ? -1 : (name_less(b, a) ? 1 : 0);
            } else if (section == total_column()) {
                double av = total_earned_for(a);
                double bv = total_earned_for(b);
                cmp = av < bv ? -1 : (av > bv ? 1 : 0);
            } else {
                std::string problem = problems_[static_cast<std::size_t>(section - 1)];
                double av = earned_for_problem(a, problem);
                double bv = earned_for_problem(b, problem);
                cmp = av < bv ? -1 : (av > bv ? 1 : 0);
            }
            if (cmp == 0) {
                cmp = name_less(a, b) ? -1 : (name_less(b, a) ? 1 : 0);
            }
            return sort_ascending_ ? cmp < 0 : cmp > 0;
        });
        populate_table();
    }

    void show_header_menu(const QPoint& pos) {
        int section = table_->horizontalHeader()->logicalIndexAt(pos);
        if (section <= 0 || section > static_cast<int>(problems_.size())) {
            return;
        }
        std::string problem = problems_[static_cast<std::size_t>(section - 1)];
        QMenu menu(this);
        menu.addAction("Judge this problem for selected contestants",
                       [this, problem]() { start_judge(true, problem); });
        menu.addAction("Judge this problem for all contestants",
                       [this, problem]() { start_judge(false, problem); });
        menu.exec(table_->horizontalHeader()->mapToGlobal(pos));
    }

    void refresh_judge_detail_view() {
        if (detail_progress_label_) {
            detail_progress_label_->setText(
                detail_progress_text_.isEmpty() ? text("idle") : detail_progress_text_);
        }
        if (!core_tasks_table_) {
            return;
        }

        core_tasks_table_->setRowCount(static_cast<int>(core_tasks_.size()));
        for (std::size_t row = 0; row < core_tasks_.size(); ++row) {
            auto* core_item = new QTableWidgetItem(core_tasks_[row].first);
            auto* task_item = new QTableWidgetItem(core_tasks_[row].second);
            core_item->setTextAlignment(Qt::AlignCenter);
            core_tasks_table_->setItem(static_cast<int>(row), 0, core_item);
            core_tasks_table_->setItem(static_cast<int>(row), 1, task_item);
        }
    }

    void show_judge_detail_view() {
        if (detail_dialog_) {
            detail_dialog_->show();
            detail_dialog_->raise();
            detail_dialog_->activateWindow();
            return;
        }

        auto* dialog = new QDialog(this);
        detail_dialog_ = dialog;
        dialog->setWindowTitle(text("judge_details"));
        dialog->resize(680, 420);
        auto* layout = new QVBoxLayout(dialog);
        detail_progress_label_ = new QLabel(dialog);
        detail_progress_label_->setWordWrap(true);

        core_tasks_table_ = new QTableWidget(dialog);
        core_tasks_table_->setObjectName("CoreTasksTable");
        core_tasks_table_->setColumnCount(2);
        core_tasks_table_->setHorizontalHeaderLabels({text("core"), text("task")});
        core_tasks_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        core_tasks_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        core_tasks_table_->verticalHeader()->setVisible(false);
        core_tasks_table_->setAlternatingRowColors(false);
        core_tasks_table_->setShowGrid(false);
        core_tasks_table_->setFocusPolicy(Qt::NoFocus);
        core_tasks_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        core_tasks_table_->setSelectionBehavior(QAbstractItemView::SelectRows);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
        QObject::connect(dialog, &QObject::destroyed, this, [this]() {
            detail_dialog_ = nullptr;
            detail_progress_label_ = nullptr;
            core_tasks_table_ = nullptr;
        });

        layout->addWidget(detail_progress_label_);
        layout->addWidget(core_tasks_table_, 1);
        layout->addWidget(buttons);
        refresh_judge_detail_view();
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    }

    void handle_progress_line(const std::string& raw) {
        QString line = QString::fromStdString(raw);
        QStringList parts = line.split(' ', Qt::SkipEmptyParts);
        if (parts.size() < 4 || parts.value(0) != "progress") {
            log_->appendPlainText(line);
            return;
        }
        QString counts;
        for (const QString& part : parts) {
            if (part.contains('/')) {
                counts = part;
                break;
            }
        }
        QStringList split = counts.split('/');
        if (split.size() != 2) {
            return;
        }
        bool done_ok = false;
        bool total_ok = false;
        int done = split[0].toInt(&done_ok);
        int total = split[1].toInt(&total_ok);
        if (!done_ok || !total_ok || total <= 0) {
            return;
        }
        progress_->setRange(0, total);
        progress_->setValue(done);
        QStringList sections = line.split('|');
        detail_progress_text_ = sections.value(0).trimmed();
        core_tasks_.clear();
        for (int i = 1; i < sections.size(); ++i) {
            QString worker = sections[i].trimmed();
            int colon = worker.indexOf(':');
            if (colon < 0) {
                continue;
            }
            QString core_name = worker.left(colon).trimmed();
            if (core_name.startsWith("core ")) {
                core_name = text("core") + " " + core_name.mid(5);
            }
            core_tasks_.push_back({core_name, worker.mid(colon + 1).trimmed()});
        }
        refresh_judge_detail_view();
    }

    void start_judge(bool selected_only, const std::string& selected_problem = {}) {
        if (archive_running_.load()) {
            QMessageBox::information(this, text("archive_operation_running"),
                                     text("wait_for_archive_operation"));
            return;
        }
        if (judging_.load()) {
            log_->appendPlainText(text("judge_already_active"));
            return;
        }
        if (contest_root_.empty()) {
            QMessageBox::information(this, text("no_contest"), text("open_contest_first"));
            return;
        }
        neothemis::JudgeOptions options = options_from_ui();
        if (!selected_problem.empty()) {
            options.selected_problems.push_back(selected_problem);
        }
        std::vector<std::string> selected;
        if (selected_only) {
            selected = selected_contestants();
            if (selected.empty()) {
                QMessageBox::information(this, text("no_selection"), text("select_contestant_rows"));
                return;
            }
            options.selected_contestants = selected;
        }
        if (judge_thread_.joinable()) {
            judge_thread_.join();
        }
        judging_.store(true);
        cancel_requested_.store(false);
        start_judge_elapsed();
        set_judge_controls_enabled(false);
        reset_run_cells(selected, selected_problem);
        progress_->setRange(0, 0);
        detail_progress_text_ = text("starting");
        core_tasks_.clear();
        refresh_judge_detail_view();
        QString scope = selected_only ? text("selected_contestants") : text("all_contestants");
        log_->appendPlainText(selected_problem.empty()
                                  ? text("judging") + " " + scope
                                  : text("judging") + " " +
                                        QString::fromStdString(selected_problem) + " - " + scope);

        judge_thread_ = std::thread([this, options]() mutable {
            try {
                options.should_cancel = [this]() {
                    return cancel_requested_.load();
                };
                options.progress = [this](const std::string& line) {
                    QMetaObject::invokeMethod(this, [this, line]() {
                        handle_progress_line(line);
                    }, Qt::QueuedConnection);
                };
                options.result = [this](const neothemis::TestResult& result) {
                    QMetaObject::invokeMethod(this, [this, result]() {
                        handle_result(result);
                    }, Qt::QueuedConnection);
                };

                auto core = neothemis::make_judge_core(options.core_name);
                core->judge(options);
                QMetaObject::invokeMethod(this, [this]() {
                    try {
                        write_current_csv_outputs();
                        progress_->setRange(0, 100);
                        progress_->setValue(100);
                        log_->appendPlainText(text("judge_run_complete"));
                    } catch (const std::exception& ex) {
                        progress_->setRange(0, 100);
                        progress_->setValue(0);
                        log_->appendPlainText(text("csv_write_failed") + ": " +
                                              QString::fromUtf8(ex.what()));
                        QMessageBox::critical(this, text("csv_write_failed"), ex.what());
                    }
                    finish_judge_elapsed();
                    judging_.store(false);
                    set_judge_controls_enabled(true);
                }, Qt::QueuedConnection);
            } catch (const std::exception& ex) {
                bool cancelled = cancel_requested_.load();
                QMetaObject::invokeMethod(this, [this, message = QString::fromUtf8(ex.what()), cancelled]() {
                    progress_->setRange(0, 100);
                    progress_->setValue(0);
                    log_->appendPlainText(cancelled ? text("cancelled") + "."
                                                    : text("failed") + ": " + message);
                    finish_judge_elapsed();
                    judging_.store(false);
                    set_judge_controls_enabled(true);
                    if (!cancelled) {
                        QMessageBox::critical(this, text("judge_failed"), message);
                    }
                }, Qt::QueuedConnection);
            }
        });
    }

    void set_judge_controls_enabled(bool enabled) {
        if (judge_selected_button_) {
            judge_selected_button_->setEnabled(enabled);
        }
        if (judge_all_button_) {
            judge_all_button_->setEnabled(enabled);
        }
        if (judge_selected_action_) {
            judge_selected_action_->setEnabled(enabled);
        }
        if (judge_all_action_) {
            judge_all_action_->setEnabled(enabled);
        }
        if (stop_button_) {
            stop_button_->setEnabled(!enabled);
        }
        if (stop_action_) {
            stop_action_->setEnabled(!enabled);
        }
    }

    void request_stop_judge() {
        if (!judging_.load()) {
            return;
        }
        cancel_requested_.store(true);
        log_->appendPlainText(text("stopping_active_run"));
        if (stop_button_) {
            stop_button_->setEnabled(false);
        }
        if (stop_action_) {
            stop_action_->setEnabled(false);
        }
    }

    void stop_active_judge() {
        cancel_requested_.store(true);
        if (judge_thread_.joinable()) {
            judge_thread_.join();
        }
        finish_judge_elapsed();
        judging_.store(false);
    }

    static QString format_judge_elapsed(qint64 elapsed_ms) {
        const qint64 total_tenths = std::max<qint64>(0, elapsed_ms) / 100;
        const qint64 tenths = total_tenths % 10;
        const qint64 total_seconds = total_tenths / 10;
        const qint64 seconds = total_seconds % 60;
        const qint64 total_minutes = total_seconds / 60;
        const qint64 minutes = total_minutes % 60;
        const qint64 hours = total_minutes / 60;
        if (hours > 0) {
            return QString("%1:%2:%3.%4")
                .arg(hours)
                .arg(minutes, 2, 10, QLatin1Char('0'))
                .arg(seconds, 2, 10, QLatin1Char('0'))
                .arg(tenths);
        }
        return QString("%1:%2.%3")
            .arg(minutes)
            .arg(seconds, 2, 10, QLatin1Char('0'))
            .arg(tenths);
    }

    void update_judge_elapsed_label() {
        if (!judge_elapsed_label_) {
            return;
        }
        const qint64 elapsed = judge_elapsed_update_timer_ &&
                                      judge_elapsed_update_timer_->isActive() &&
                                      judge_elapsed_clock_.isValid()
                                  ? judge_elapsed_clock_.elapsed()
                                  : last_judge_elapsed_ms_;
        judge_elapsed_label_->setText(text("elapsed_time") + ": " +
                                      format_judge_elapsed(elapsed));
    }

    void start_judge_elapsed() {
        last_judge_elapsed_ms_ = 0;
        judge_elapsed_clock_.restart();
        if (judge_elapsed_update_timer_) {
            judge_elapsed_update_timer_->start();
        }
        update_judge_elapsed_label();
    }

    void finish_judge_elapsed() {
        if (judge_elapsed_update_timer_ && judge_elapsed_update_timer_->isActive()) {
            if (judge_elapsed_clock_.isValid()) {
                last_judge_elapsed_ms_ = judge_elapsed_clock_.elapsed();
            }
            judge_elapsed_update_timer_->stop();
            update_judge_elapsed_label();
        }
    }

    QWidget* build_visual_tab(QWidget* parent, SettingsDialog* settings_dialog) {
        auto* tab = new QWidget(parent);
        auto* form = new QFormLayout(tab);
        form->setContentsMargins(18, 18, 18, 18);
        form->setHorizontalSpacing(24);
        form->setVerticalSpacing(14);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setFormAlignment(Qt::AlignTop);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto* theme = new QComboBox(tab);
        theme->addItem(text("dark"), "dark");
        theme->addItem(text("cyber"), "cyber");
        int theme_index = theme->findData(QString::fromStdString(theme_));
        if (theme_index >= 0) {
            theme->setCurrentIndex(theme_index);
        }
        auto* transparent_background = new QCheckBox(tab);
        transparent_background->setChecked(transparent_background_);
        auto* transparency_widget = new QWidget(tab);
        transparency_widget->setObjectName("InlineControl");
        auto* transparency_layout = new QHBoxLayout(transparency_widget);
        transparency_layout->setContentsMargins(0, 0, 0, 0);
        transparency_layout->setSpacing(10);
        auto* transparency = new QSlider(Qt::Horizontal, transparency_widget);
        transparency->setRange(0, 95);
        transparency->setValue(background_transparency_);
        transparency->setEnabled(transparent_background_);
        auto* transparency_value = new QLabel(
            QString::number(background_transparency_) + "%", transparency_widget);
        transparency_value->setFixedWidth(42);
        transparency_value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        transparency_layout->addWidget(transparency, 1);
        transparency_layout->addWidget(transparency_value);

        auto* blur_background = new QCheckBox(tab);
        blur_background->setChecked(blur_background_);

        auto* language = new QComboBox(tab);
        language->addItem(text("english"), "en");
        language->addItem(text("vietnamese"), "vi");
        int language_index = language->findData(QString::fromStdString(language_));
        if (language_index >= 0) {
            language->setCurrentIndex(language_index);
        }
        auto* temp_widget = new QWidget(tab);
        auto* temp_layout = new QHBoxLayout(temp_widget);
        temp_layout->setContentsMargins(0, 0, 0, 0);
        temp_layout->setSpacing(10);
        auto* temp_dir = new QLineEdit(QString::fromStdString(temporary_dir_.string()), temp_widget);
        auto* browse_temp = new QPushButton(text("browse"), temp_widget);
        temp_layout->addWidget(temp_dir, 1);
        temp_layout->addWidget(browse_temp);
        auto* save = new QPushButton(text("save_application_settings"), tab);
        auto* association = new QPushButton(tab);
        const bool association_registered =
            neothemis::gui::contest_file_association_is_registered();
        association->setText(text(association_registered
                                      ? "file_association_registered"
                                      : "register_file_association"));
        association->setEnabled(!association_registered);

        form->addRow(text("theme"), theme);
        form->addRow(text("transparent_background"), transparent_background);
        form->addRow(text("background_transparency"), transparency_widget);
        form->addRow(text("blur_background"), blur_background);
        form->addRow(text("language"), language);
        form->addRow(text("temporary_dir"), temp_widget);
        form->addRow(text("ncontest_file_association"), association);
        form->addRow(save);

        QObject::connect(browse_temp, &QPushButton::clicked, [this, temp_dir]() {
            QString dir = QFileDialog::getExistingDirectory(
                this, text("temporary_dir"), temp_dir->text());
            if (!dir.isEmpty()) {
                temp_dir->setText(dir);
            }
        });

        auto apply_visual_preview = [this, theme, transparent_background,
                                     transparency, blur_background]() {
            theme_ = theme->currentData().toString().toStdString();
            transparent_background_ = transparent_background->isChecked();
            background_transparency_ = transparency->value();
            blur_background_ = blur_background->isChecked();
            apply_selected_theme();
        };

        QObject::connect(theme, &QComboBox::currentTextChanged,
                         [apply_visual_preview](const QString&) { apply_visual_preview(); });
        QObject::connect(transparent_background, &QCheckBox::toggled,
                         [transparency, apply_visual_preview](bool enabled) {
                             transparency->setEnabled(enabled);
                             apply_visual_preview();
                         });
        QObject::connect(transparency, &QSlider::valueChanged,
                         [transparency_value, apply_visual_preview](int value) {
                             transparency_value->setText(QString::number(value) + "%");
                             apply_visual_preview();
                         });
        QObject::connect(blur_background, &QCheckBox::toggled,
                         [apply_visual_preview](bool) {
                             apply_visual_preview();
                         });

        auto persist = [this, theme, transparent_background, transparency,
                        blur_background, language, temp_dir](bool notify) {
            try {
                theme_ = theme->currentData().toString().toStdString();
                transparent_background_ = transparent_background->isChecked();
                background_transparency_ = transparency->value();
                blur_background_ = blur_background->isChecked();
                language_ = language->currentData().toString().toStdString();
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
            } catch (const std::exception& ex) {
                QMessageBox::critical(this, text("save_failed"), ex.what());
            }
        };
        QObject::connect(save, &QPushButton::clicked,
                         [persist]() { persist(true); });
        settings_dialog->add_close_handler([persist]() { persist(false); });
        QObject::connect(association, &QPushButton::clicked, [this, association]() {
            try {
                neothemis::gui::register_contest_file_association();
                association->setText(text("file_association_registered"));
                association->setEnabled(false);
                log_->appendPlainText(text("file_association_registered"));
            } catch (const std::exception& ex) {
                QMessageBox::critical(this, text("file_association_failed"), ex.what());
            }
        });
        return tab;
    }

    QWidget* build_contest_tab(QWidget* parent, SettingsDialog* settings_dialog) {
        auto* tab = new QWidget(parent);
        auto* form = new QFormLayout(tab);
        auto* compiler_widget = new QWidget(tab);
        auto* compiler_layout = new QHBoxLayout(compiler_widget);
        compiler_layout->setContentsMargins(0, 0, 0, 0);
        compiler_layout->setSpacing(10);
        auto* compiler = new QLineEdit(QString::fromStdString(compiler_), compiler_widget);
        auto* browse_compiler = new QPushButton(text("browse"), compiler_widget);
        compiler_layout->addWidget(compiler, 1);
        compiler_layout->addWidget(browse_compiler);
        auto* flags = new QLineEdit(QString::fromStdString(compile_flags_), tab);
        auto* contestants = new QLineEdit(QString::fromStdString(contestants_dir_), tab);
        auto* tests = new QLineEdit(QString::fromStdString(tests_dir_), tab);
        auto* stack = new QSpinBox(tab);
        stack->setRange(0, 1024 * 1024);
        stack->setValue(static_cast<int>(stack_limit_mb_));
        auto* parallel = new QSpinBox(tab);
        parallel->setRange(0, 256);
        parallel->setValue(static_cast<int>(parallel_jobs_));
        auto* keep = new QCheckBox(tab);
        keep->setChecked(keep_workdir_);
        auto* save = new QPushButton(text("save_contest_config"), tab);

        form->addRow(text("compiler"), compiler_widget);
        form->addRow(text("compile_flags"), flags);
        form->addRow(text("contestants_dir"), contestants);
        form->addRow(text("tests_dir"), tests);
        form->addRow(text("stack_mb"), stack);
        form->addRow(text("parallel_jobs"), parallel);
        form->addRow(text("keep_workdir"), keep);
        form->addRow(save);

        QObject::connect(browse_compiler, &QPushButton::clicked, [this, compiler]() {
#ifdef Q_OS_WIN
            const QString filter = text("compiler_executable_filter_windows");
#else
            const QString filter = text("compiler_executable_filter");
#endif
            const QString selected = QFileDialog::getOpenFileName(
                this, text("select_compiler"), compiler->text(), filter);
            if (!selected.isEmpty()) {
                compiler->setText(selected);
            }
        });

        auto persist = [this, compiler, flags, contestants, tests, stack,
                        parallel, keep](bool notify) {
            if (contest_root_.empty()) {
                return;
            }
            const std::string new_compiler = compiler->text().toStdString();
            const std::string new_flags = flags->text().toStdString();
            const std::string new_contestants = contestants->text().toStdString();
            const std::string new_tests = tests->text().toStdString();
            const auto new_stack = static_cast<std::uint64_t>(stack->value());
            const auto new_parallel = static_cast<unsigned int>(parallel->value());
            const bool new_keep = keep->isChecked();
            const bool changed = compiler_ != new_compiler ||
                                 compile_flags_ != new_flags ||
                                 contestants_dir_ != new_contestants ||
                                 tests_dir_ != new_tests ||
                                 stack_limit_mb_ != new_stack ||
                                 parallel_jobs_ != new_parallel ||
                                 keep_workdir_ != new_keep;
            if (!changed) {
                if (notify) {
                    log_->appendPlainText(text("saved_contest_config"));
                }
                return;
            }
            compiler_ = new_compiler;
            compile_flags_ = new_flags;
            contestants_dir_ = new_contestants;
            tests_dir_ = new_tests;
            stack_limit_mb_ = new_stack;
            parallel_jobs_ = new_parallel;
            keep_workdir_ = new_keep;
            try {
                save_contest_config();
                mark_contest_dirty();
                refresh_table();
                if (notify) {
                    log_->appendPlainText(text("saved_contest_config"));
                }
            } catch (const std::exception& ex) {
                QMessageBox::critical(this, text("save_failed"), ex.what());
            }
        };
        QObject::connect(save, &QPushButton::clicked,
                         [persist]() { persist(true); });
        settings_dialog->add_close_handler([persist]() { persist(false); });
        return tab;
    }

    QWidget* build_problem_tab(QWidget* parent, SettingsDialog* settings_dialog) {
        auto* tab = new QWidget(parent);
        auto* form = new QFormLayout(tab);
        auto* problem = new QComboBox(tab);
        for (const auto& name : problems_) {
            problem->addItem(QString::fromStdString(name));
        }
        auto* time = new QSpinBox(tab);
        time->setRange(1, 60 * 60 * 1000);
        auto* memory = new QSpinBox(tab);
        memory->setRange(0, 1024 * 1024);
        auto* points = new QLineEdit(tab);
        auto* checker = new QComboBox(tab);
        checker->addItem("custom");
        checker->addItem("token");
        auto* test_table = new QTableWidget(tab);
        test_table->setColumnCount(2);
        test_table->setHorizontalHeaderItem(0, new QTableWidgetItem(text("test")));
        test_table->setHorizontalHeaderItem(1, new QTableWidgetItem(text("point_override")));
        test_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        test_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        test_table->verticalHeader()->setVisible(false);
        test_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        test_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
        test_table->setMinimumHeight(220);

        auto* batch_widget = new QWidget(tab);
        auto* batch_layout = new QHBoxLayout(batch_widget);
        batch_layout->setContentsMargins(0, 0, 0, 0);
        auto* batch_points = new QLineEdit(batch_widget);
        auto* apply_batch = new QPushButton(text("apply_to_selected"), batch_widget);
        batch_layout->addWidget(batch_points, 1);
        batch_layout->addWidget(apply_batch);

        auto* save = new QPushButton(text("save_problem_config"), tab);
        auto problem_loading = std::make_shared<bool>(false);
        auto problem_dirty = std::make_shared<bool>(false);

        auto load_problem = [this, problem, time, memory, points, checker, test_table,
                             problem_loading, problem_dirty]() {
            *problem_loading = true;
            std::string name = problem->currentText().toStdString();
            if (name.empty()) {
                test_table->setRowCount(0);
                *problem_loading = false;
                *problem_dirty = false;
                return;
            }
            auto values = read_config_file(contest_root_ / tests_dir_ / name / "problem.conf");
            time->setValue(values.count("time_limit_ms") ? std::stoi(values["time_limit_ms"]) : 1000);
            memory->setValue(values.count("memory_limit_mb") ? std::stoi(values["memory_limit_mb"]) : 256);
            points->setText(QString::fromStdString(values.count("default_points") ? values["default_points"] : "1"));
            std::string checker_value = values.count("checker") ? values["checker"] : "token";
            checker->setCurrentText(checker_value.rfind("custom", 0) == 0 ? "custom" : "token");

            std::vector<std::string> tests = test_names_for_problem(contest_root_ / tests_dir_ / name);
            test_table->setRowCount(static_cast<int>(tests.size()));
            for (std::size_t row = 0; row < tests.size(); ++row) {
                const std::string& test_name = tests[row];
                auto* test_item = new QTableWidgetItem(QString::fromStdString(test_name));
                test_item->setFlags(test_item->flags() & ~Qt::ItemIsEditable);
                test_table->setItem(static_cast<int>(row), 0, test_item);

                std::string override_points;
                for (const auto& key : test_point_keys(test_name)) {
                    auto found = values.find("test_points." + key);
                    if (found != values.end()) {
                        override_points = found->second;
                        break;
                    }
                }
                test_table->setItem(static_cast<int>(row), 1,
                                    new QTableWidgetItem(QString::fromStdString(override_points)));
            }
            *problem_loading = false;
            *problem_dirty = false;
        };
        QObject::connect(problem, &QComboBox::currentTextChanged, [load_problem]() { load_problem(); });

        QObject::connect(apply_batch, &QPushButton::clicked, [test_table, batch_points]() {
            std::set<int> rows;
            for (const QModelIndex& index : test_table->selectionModel()->selectedRows()) {
                rows.insert(index.row());
            }
            for (int row : rows) {
                auto* item = test_table->item(row, 1);
                if (!item) {
                    item = new QTableWidgetItem;
                    test_table->setItem(row, 1, item);
                }
                item->setText(batch_points->text());
            }
        });

        auto persist = [this, problem, time, memory, points, checker,
                        test_table, problem_dirty](bool notify) {
            try {
                std::string name = problem->currentText().toStdString();
                if (name.empty()) {
                    return;
                }
                if (!*problem_dirty) {
                    if (notify) {
                        log_->appendPlainText(text("saved_problem_config") +
                                              problem->currentText());
                    }
                    return;
                }
                std::string default_points = trim(points->text().toStdString());
                auto ensure_points = [&](const std::string& value) {
                    if (value.empty()) {
                        return;
                    }
                    std::size_t parsed = 0;
                    (void)std::stod(value, &parsed);
                    if (parsed != value.size()) {
                        throw std::runtime_error(text("invalid_points_detail").toStdString());
                    }
                };
                ensure_points(default_points);

                std::vector<std::pair<std::string, std::string>> overrides;
                for (int row = 0; row < test_table->rowCount(); ++row) {
                    auto* test_item = test_table->item(row, 0);
                    auto* point_item = test_table->item(row, 1);
                    if (!test_item) {
                        continue;
                    }
                    std::string value = point_item ? trim(point_item->text().toStdString()) : std::string();
                    ensure_points(value);
                    if (!value.empty()) {
                        overrides.push_back({test_item->text().toStdString(), value});
                    }
                }

                fs::create_directories(contest_root_ / tests_dir_ / name);
                write_problem_config(contest_root_ / tests_dir_ / name / "problem.conf",
                                     time->value(), memory->value(),
                                     default_points.empty() ? std::string("1") : default_points,
                                     checker->currentText().toStdString(), overrides);
                mark_contest_dirty();
                *problem_dirty = false;
                if (notify) {
                    log_->appendPlainText(text("saved_problem_config") + problem->currentText());
                }
            } catch (const std::exception& ex) {
                QMessageBox::critical(this, text("save_failed"), ex.what());
            }
        };
        QObject::connect(save, &QPushButton::clicked,
                         [persist]() { persist(true); });
        settings_dialog->add_close_handler([persist]() { persist(false); });

        form->addRow(text("problem"), problem);
        form->addRow(text("time_limit_ms"), time);
        form->addRow(text("memory_mb"), memory);
        form->addRow(text("default_points"), points);
        form->addRow(text("checker"), checker);
        form->addRow(text("test_points"), test_table);
        form->addRow(text("selected_point"), batch_widget);
        form->addRow(save);
        if (problem->count() > 0) {
            load_problem();
        }
        auto mark_problem_dirty = [problem_loading, problem_dirty]() {
            if (!*problem_loading) {
                *problem_dirty = true;
            }
        };
        QObject::connect(time, &QSpinBox::valueChanged,
                         [mark_problem_dirty](int) { mark_problem_dirty(); });
        QObject::connect(memory, &QSpinBox::valueChanged,
                         [mark_problem_dirty](int) { mark_problem_dirty(); });
        QObject::connect(points, &QLineEdit::textChanged,
                         [mark_problem_dirty](const QString&) { mark_problem_dirty(); });
        QObject::connect(checker, &QComboBox::currentTextChanged,
                         [mark_problem_dirty](const QString&) { mark_problem_dirty(); });
        QObject::connect(test_table, &QTableWidget::itemChanged,
                         [mark_problem_dirty](QTableWidgetItem*) { mark_problem_dirty(); });
        return tab;
    }

    QWidget* build_server_settings_tab(QWidget* parent, SettingsDialog* settings_dialog) {
        auto* tab = new QWidget(parent);
        auto* form = new QFormLayout(tab);
        form->setContentsMargins(18, 18, 18, 18);
        form->setHorizontalSpacing(24);
        form->setVerticalSpacing(14);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setFormAlignment(Qt::AlignTop);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

        auto* port = new QSpinBox(tab);
        port->setRange(1024, 65535);
        port->setValue(server_port_);
        auto* allow_lan = new QCheckBox(tab);
        allow_lan->setChecked(server_allow_lan_);
        auto* enable_ranking = new QCheckBox(tab);
        enable_ranking->setChecked(server_ranking_enabled_);
        auto* enable_details = new QCheckBox(tab);
        enable_details->setChecked(server_contestant_details_enabled_);
        auto* join_code = new QLineEdit(server_join_code_, tab);
        auto* admin_password = new QLineEdit(server_admin_password_, tab);
        auto* database = new QLineEdit(server_database_display_path(), tab);
        database->setReadOnly(true);
        auto* warning = new QLabel(text("server_settings_help"), tab);
        warning->setWordWrap(true);
        warning->setObjectName("ServerWarning");
        auto* save = new QPushButton(text("save_server_settings"), tab);

        form->addRow(text("server_port"), port);
        form->addRow(text("server_allow_lan"), allow_lan);
        form->addRow(text("server_enable_ranking"), enable_ranking);
        form->addRow(text("server_enable_contestant_details"), enable_details);
        form->addRow(text("server_join_code"), join_code);
        form->addRow(text("server_admin_bootstrap_password"), admin_password);
        form->addRow(text("server_database"), database);
        form->addRow(warning);
        form->addRow(save);

        auto persist = [this, port, allow_lan, enable_ranking, enable_details,
                        join_code, admin_password](bool notify) {
            const QString join = join_code->text().trimmed();
            const QString admin = admin_password->text();
            if (join.isEmpty() || admin.isEmpty()) {
                QMessageBox::warning(this, text("save_failed"),
                                     text("server_credentials_required"));
                return;
            }
            const bool changed = server_port_ != port->value() ||
                                 server_allow_lan_ != allow_lan->isChecked() ||
                                 server_ranking_enabled_ != enable_ranking->isChecked() ||
                                 server_contestant_details_enabled_ != enable_details->isChecked() ||
                                 server_join_code_ != join ||
                                 server_admin_password_ != admin;
            if (!changed) {
                if (notify) {
                    log_->appendPlainText(text("saved_server_settings"));
                }
                return;
            }
            server_port_ = port->value();
            server_allow_lan_ = allow_lan->isChecked();
            server_ranking_enabled_ = enable_ranking->isChecked();
            server_contestant_details_enabled_ = enable_details->isChecked();
            server_join_code_ = join;
            server_admin_password_ = admin;
            try {
                save_app_settings();
                if (!contest_root_.empty()) {
                    save_contest_config();
                    mark_contest_dirty();
                }
                if (notify) {
                    log_->appendPlainText(text("saved_server_settings"));
                }
            } catch (const std::exception& ex) {
                QMessageBox::critical(this, text("save_failed"), ex.what());
            }
        };
        QObject::connect(save, &QPushButton::clicked,
                         [persist]() { persist(true); });
        settings_dialog->add_close_handler([persist]() { persist(false); });
        return tab;
    }

    void refresh_server_users_table(QTableWidget* users_table) {
        users_table->setRowCount(0);
        if (contest_root_.empty()) {
            return;
        }
        fs::path database_path = server_database_path();
        if (!fs::exists(database_path)) {
            return;
        }

        QSqlDatabase database = open_server_database(false);
        QSqlQuery query(database);
        if (!query.exec("SELECT username, password, role, created_at FROM users "
                        "ORDER BY CASE role WHEN 'admin' THEN 0 ELSE 1 END, username")) {
            throw std::runtime_error(query.lastError().text().toStdString());
        }

        while (query.next()) {
            const int row = users_table->rowCount();
            users_table->insertRow(row);
            const QString username = query.value(0).toString();
            const QString stored_password = query.value(1).toString();
            const QString password = stored_password.isEmpty()
                                         ? text("server_password_reset_required")
                                         : stored_password;
            const QString role = query.value(2).toString();
            const QString role_display = role == "admin"
                                             ? text("server_role_admin")
                                             : text("server_role_contestant");
            const QString created = QDateTime::fromSecsSinceEpoch(query.value(3).toLongLong())
                                        .toString("yyyy-MM-dd HH:mm");

            auto add_readonly_item = [users_table, row](int column, const QString& value) {
                auto* item = new QTableWidgetItem(value);
                item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                users_table->setItem(row, column, item);
            };
            add_readonly_item(0, username);
            add_readonly_item(1, password);
            add_readonly_item(2, role_display);
            add_readonly_item(3, created);
        }
    }

    QWidget* build_server_users_tab(QWidget* parent) {
        auto* tab = new QWidget(parent);
        auto* layout = new QVBoxLayout(tab);
        layout->setContentsMargins(18, 18, 18, 18);
        layout->setSpacing(14);

        auto* form = new QFormLayout;
        form->setHorizontalSpacing(24);
        form->setVerticalSpacing(14);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

        auto* database = new QLineEdit(server_database_display_path(), tab);
        database->setReadOnly(true);
        auto* username = new QLineEdit(tab);
        username->setPlaceholderText(text("server_username_hint"));
        auto* password = new QLineEdit(tab);
        password->setEchoMode(QLineEdit::Normal);
        auto* role = new QComboBox(tab);
        role->addItem(text("server_role_contestant"), "contestant");
        role->addItem(text("server_role_admin"), "admin");
        auto* add = new QPushButton(text("add_server_user"), tab);
        auto* change_password = new QPushButton(text("change_server_password"), tab);
        auto* remove = new QPushButton(text("remove_server_user"), tab);
        remove->setObjectName("DangerButton");
        auto* import_csv = new QPushButton(text("import_server_users_csv"), tab);
        auto* refresh = new QPushButton(text("refresh"), tab);
        auto* actions = new QWidget(tab);
        auto* actions_layout = new QHBoxLayout(actions);
        actions_layout->setContentsMargins(0, 0, 0, 0);
        actions_layout->setSpacing(10);
        actions_layout->addWidget(add);
        actions_layout->addWidget(change_password);
        actions_layout->addWidget(remove);
        actions_layout->addWidget(import_csv);
        actions_layout->addWidget(refresh);
        actions_layout->addStretch(1);
        auto* csv_help = new QLabel(text("server_users_csv_help"), tab);
        csv_help->setWordWrap(true);
        csv_help->setObjectName("ServerWarning");

        form->addRow(text("server_database"), database);
        form->addRow(text("server_user_username"), username);
        form->addRow(text("server_user_password"), password);
        form->addRow(text("server_user_role"), role);
        form->addRow(actions);
        form->addRow(csv_help);
        layout->addLayout(form);

        auto* users_table = new QTableWidget(tab);
        server_users_table_ = users_table;
        QObject::connect(users_table, &QObject::destroyed, [this, users_table]() {
            if (server_users_table_ == users_table) {
                server_users_table_ = nullptr;
            }
        });
        users_table->setObjectName("ServerUsersTable");
        users_table->setColumnCount(4);
        users_table->setHorizontalHeaderLabels({
            text("server_user_username"), text("server_user_password"),
            text("server_user_role"), text("created_at")
        });
        users_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        users_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        users_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        users_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        users_table->verticalHeader()->setVisible(false);
        users_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        users_table->setSelectionMode(QAbstractItemView::SingleSelection);
        users_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        layout->addWidget(users_table, 1);

        const bool contest_open = !contest_root_.empty();
        username->setEnabled(contest_open);
        password->setEnabled(contest_open);
        role->setEnabled(contest_open);
        add->setEnabled(contest_open);
        change_password->setEnabled(contest_open);
        remove->setEnabled(contest_open);
        import_csv->setEnabled(contest_open);
        refresh->setEnabled(contest_open);

        auto refresh_users = [this, users_table]() {
            try {
                refresh_server_users_table(users_table);
            } catch (const std::exception& ex) {
                QMessageBox::critical(this, text("server_user_failed"), ex.what());
            }
        };
        QObject::connect(refresh, &QPushButton::clicked, refresh_users);
        QObject::connect(add, &QPushButton::clicked, [this, username, password, role,
                                                       users_table]() {
            try {
                const QString new_username = username->text().trimmed();
                create_server_user(new_username, password->text(),
                                   role->currentData().toString());
                password->clear();
                refresh_server_users_table(users_table);
                refresh_table(false);
                log_->appendPlainText(text("server_user_created").arg(new_username));
            } catch (const std::exception& ex) {
                QMessageBox::critical(this, text("server_user_failed"), ex.what());
            }
        });
        QObject::connect(change_password, &QPushButton::clicked, [this, users_table]() {
            const int row = users_table->currentRow();
            if (row < 0 || !users_table->item(row, 0)) {
                QMessageBox::information(this, text("server_users"),
                                         text("select_server_user"));
                return;
            }
            const QString selected_username = users_table->item(row, 0)->text();
            bool accepted = false;
            const QString new_password = QInputDialog::getText(
                this, text("change_server_password"),
                text("new_password_for").arg(selected_username),
                QLineEdit::Normal, QString(), &accepted);
            if (!accepted) {
                return;
            }
            try {
                change_server_user_password(selected_username, new_password);
                refresh_server_users_table(users_table);
                log_->appendPlainText(text("server_password_changed").arg(selected_username));
            } catch (const std::exception& ex) {
                QMessageBox::critical(this, text("server_user_failed"), ex.what());
            }
        });
        QObject::connect(remove, &QPushButton::clicked, [this, users_table]() {
            const int row = users_table->currentRow();
            if (row < 0 || !users_table->item(row, 0)) {
                QMessageBox::information(this, text("server_users"),
                                         text("select_server_user"));
                return;
            }
            const QString selected_username = users_table->item(row, 0)->text();
            const QMessageBox::StandardButton answer = QMessageBox::warning(
                this, text("remove_server_user"),
                text("remove_server_user_confirm").arg(selected_username),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
            if (answer != QMessageBox::Yes) {
                return;
            }
            try {
                remove_server_user(selected_username);
                refresh_server_users_table(users_table);
                refresh_table(false);
                log_->appendPlainText(text("server_user_removed").arg(selected_username));
            } catch (const std::exception& ex) {
                QMessageBox::critical(this, text("server_user_failed"), ex.what());
            }
        });
        QObject::connect(import_csv, &QPushButton::clicked, [this, users_table]() {
            QString selected = QFileDialog::getOpenFileName(
                this, text("import_server_users_csv"), QString(),
                text("csv_users_filter"));
            if (selected.isEmpty()) {
                return;
            }
            try {
                auto [created, skipped] = import_server_users_csv(path_from_qstring(selected));
                refresh_server_users_table(users_table);
                refresh_table(false);
                log_->appendPlainText(text("server_users_imported")
                                      .arg(created).arg(skipped));
            } catch (const std::exception& ex) {
                QMessageBox::critical(this, text("server_users_import_failed"), ex.what());
            }
        });
        refresh_users();
        return tab;
    }

    void open_settings_dialog(int initial_tab) {
        auto* dialog = new SettingsDialog(this);
        dialog->setWindowTitle(text("settings_title"));
        dialog->setWindowModality(Qt::WindowModal);
        dialog->resize(820, 680);
        auto* layout = new QVBoxLayout(dialog);
        auto* tabs = new QTabWidget(dialog);
        tabs->addTab(build_visual_tab(tabs, dialog), text("application"));
        tabs->addTab(build_contest_tab(tabs, dialog), text("contest"));
        tabs->addTab(build_problem_tab(tabs, dialog), text("problems"));
        tabs->addTab(build_server_settings_tab(tabs, dialog), text("server_settings"));
        tabs->addTab(build_server_users_tab(tabs), text("server_users"));
        tabs->setCurrentIndex(initial_tab);
        layout->addWidget(tabs);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
        layout->addWidget(buttons);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    }

    void show_about_dialog() {
        auto* dialog = new QDialog(this);
        dialog->setWindowTitle(text("about_title"));
        dialog->resize(520, 360);
        auto* layout = new QVBoxLayout(dialog);

        auto* title = new QLabel("NeoThemis", dialog);
        title->setObjectName("WindowAppName");
        auto* details = new QPlainTextEdit(dialog);
        details->setReadOnly(true);
        details->setPlainText(text("about_details"));
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);

        layout->addWidget(title);
        layout->addWidget(details, 1);
        layout->addWidget(buttons);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    }

    fs::path contest_root_;
    std::vector<std::string> contestants_;
    std::vector<std::string> problems_;
    std::map<std::string, int> contestant_rows_;
    std::map<std::string, int> problem_columns_;
    std::map<std::string, int> problem_test_counts_;
    std::map<std::string, bool> source_ready_;
    std::map<std::string, CellScore> score_cells_;
    std::map<std::string, QString> cell_texts_;
    std::map<std::string, std::vector<neothemis::TestResult>> result_details_;
    int sort_column_ = 0;
    bool sort_ascending_ = true;

    std::string compiler_ = "g++";
    std::string compile_flags_ = "-std=c++14 -O2 -pipe";
    std::string contestants_dir_ = "contestants";
    std::string tests_dir_ = "tests";
    std::string language_ = "en";
    std::string theme_ = "dark";
    bool transparent_background_ = false;
    int background_transparency_ = 20;
    bool blur_background_ = false;
    fs::path temporary_dir_;
    int server_port_ = 8080;
    bool server_allow_lan_ = false;
    bool server_ranking_enabled_ = false;
    bool server_contestant_details_enabled_ = false;
    QString server_join_code_;
    QString server_admin_password_;
    fs::path contest_file_path_;
    fs::path active_temp_root_;
    std::vector<fs::path> temporary_roots_;
    std::uint64_t stack_limit_mb_ = 64;
    unsigned int parallel_jobs_ = 0;
    bool keep_workdir_ = false;
    bool contest_from_file_ = false;
    bool contest_dirty_ = false;
    bool pending_close_after_save_ = false;
    std::atomic_bool judging_{false};
    std::atomic_bool archive_running_{false};
    std::atomic_bool cancel_requested_{false};
    std::mutex archive_progress_mutex_;
    std::uint64_t pending_archive_progress_done_ = 0;
    std::uint64_t pending_archive_progress_total_ = 0;
    std::string pending_archive_progress_key_;
    bool archive_progress_update_queued_ = false;
    std::thread judge_thread_;
    std::thread archive_thread_;

    QTableWidget* table_ = nullptr;
    neothemis::gui::ThemeBackground* background_layer_ = nullptr;
    QWidget* side_panel_ = nullptr;
    QWidget* title_bar_ = nullptr;
    QGroupBox* judge_group_ = nullptr;
    QMenuBar* menu_bar_ = nullptr;
    QLabel* contest_title_ = nullptr;
    QPushButton* judge_selected_button_ = nullptr;
    QPushButton* judge_all_button_ = nullptr;
    QPushButton* stop_button_ = nullptr;
    QPushButton* detail_view_button_ = nullptr;
    QAction* judge_selected_action_ = nullptr;
    QAction* judge_all_action_ = nullptr;
    QAction* stop_action_ = nullptr;
    QAction* server_start_action_ = nullptr;
    QAction* server_stop_action_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* judge_elapsed_label_ = nullptr;
    QTimer* judge_elapsed_update_timer_ = nullptr;
    QElapsedTimer judge_elapsed_clock_;
    qint64 last_judge_elapsed_ms_ = 0;
    QPlainTextEdit* log_ = nullptr;
    QProcess* server_process_ = nullptr;
    QTimer* server_refresh_timer_ = nullptr;
    QDialog* detail_dialog_ = nullptr;
    QLabel* detail_progress_label_ = nullptr;
    QTableWidget* core_tasks_table_ = nullptr;
    QTableWidget* server_users_table_ = nullptr;
    QString detail_progress_text_;
    std::string current_archive_progress_key_;
    std::vector<std::pair<QString, QString>> core_tasks_;
    fs::file_time_type last_server_database_write_ = fs::file_time_type::min();
    fs::file_time_type last_server_results_write_ = fs::file_time_type::min();
    fs::file_time_type last_server_contestants_write_ = fs::file_time_type::min();
    QPoint drag_offset_;
    bool dragging_title_bar_ = false;
};

} // namespace

namespace neothemis::gui {

std::unique_ptr<QMainWindow> create_main_window(fs::path initial_contest) {
    return std::make_unique<MainWindow>(std::move(initial_contest));
}

} // namespace neothemis::gui
