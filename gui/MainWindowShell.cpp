#include "MainWindowPrivate.hpp"

namespace neothemis::gui {

MainWindow::MainWindow(fs::path initial_contest) {
    load_app_settings();
#ifndef Q_OS_WIN
    setAttribute(Qt::WA_TranslucentBackground, true);
#endif
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

    QObject::connect(judge_selected_button_, &QPushButton::clicked,
                     [this]() { start_judge(true); });
    QObject::connect(judge_all_button_, &QPushButton::clicked, [this]() { start_judge(false); });
    QObject::connect(stop_button_, &QPushButton::clicked, [this]() { request_stop_judge(); });
    QObject::connect(detail_view_button_, &QPushButton::clicked,
                     [this]() { show_judge_detail_view(); });
    QObject::connect(table_, &QTableWidget::cellDoubleClicked,
                     [this](int row, int col) { show_result_details(row, col); });
    QObject::connect(table_->horizontalHeader(), &QHeaderView::sectionClicked,
                     [this](int section) { sort_by_column(section); });
    QObject::connect(table_->horizontalHeader(), &QWidget::customContextMenuRequested,
                     [this](const QPoint& pos) { show_header_menu(pos); });
    QObject::connect(minimize, &QToolButton::clicked, this, &QWidget::showMinimized);
    QObject::connect(maximize, &QToolButton::clicked,
                     [this]() { neothemis::gui::toggle_window_maximized(this); });
    QObject::connect(close, &QToolButton::clicked, this, &QWidget::close);

    if (!initial_contest.empty()) {
        // Give the first window frame time to paint before multicore extraction starts.
        QTimer::singleShot(
            100, this, [this, path = std::move(initial_contest)]() { open_contest_file(path); });
    }
}

MainWindow::~MainWindow() {
    neothemis::gui::release_windows_backdrop(this);
    stop_local_server(false);
    stop_active_judge();
    join_archive_thread();
    cleanup_temporary_contests();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == title_bar_ || watched == contest_title_ ||
        (watched->isWidgetType() &&
         static_cast<QWidget*>(watched)->objectName() == "WindowAppName") ||
        (watched->isWidgetType() && static_cast<QWidget*>(watched)->objectName() == "AppLogo")) {
        if (event->type() == QEvent::MouseButtonDblClick) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                neothemis::gui::toggle_window_maximized(this);
                return true;
            }
        }
        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                if (QWindow* handle = windowHandle(); handle && handle->startSystemMove()) {
                    dragging_title_bar_ = false;
                    return true;
                }
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

void MainWindow::closeEvent(QCloseEvent* event) {
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

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    apply_selected_theme();
    schedule_backdrop_startup_passes();
}

bool MainWindow::nativeEvent(const QByteArray& event_type, void* message, qintptr* result) {
#ifdef Q_OS_WIN
    if (background_transparency_active() &&
        neothemis::gui::windows_backdrop_message_requires_refresh(message) &&
        !backdrop_refresh_queued_) {
        backdrop_refresh_queued_ = true;
        QTimer::singleShot(0, this, [this]() {
            backdrop_refresh_queued_ = false;
            if (background_transparency_active()) {
                apply_native_backdrop(true);
            }
        });
    }
#endif
    return QMainWindow::nativeEvent(event_type, message, result);
}

QString MainWindow::text(const char* key) const {
    return neothemis::gui::translated_text(key, language_ == "vi");
}

void MainWindow::load_app_settings() {
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
    const neothemis::gui::CyberThemeColors default_cyber_colors =
        neothemis::gui::default_cyber_theme_colors();
    cyber_background_color_ = color_setting_or_default(
        settings.value("cyber_background_color",
                       default_cyber_colors.background.name(QColor::HexRgb)),
        default_cyber_colors.background);
    cyber_primary_color_ = color_setting_or_default(
        settings.value("cyber_primary_color",
                       default_cyber_colors.primary.name(QColor::HexRgb)),
        default_cyber_colors.primary);
    cyber_secondary_color_ = color_setting_or_default(
        settings.value("cyber_secondary_color",
                       default_cyber_colors.secondary.name(QColor::HexRgb)),
        default_cyber_colors.secondary);
    cyber_text_color_ = color_setting_or_default(
        settings.value("cyber_text_color", default_cyber_colors.text.name(QColor::HexRgb)),
        default_cyber_colors.text);
    cyber_muted_text_color_ = color_setting_or_default(
        settings.value("cyber_muted_text_color",
                       default_cyber_colors.muted_text.name(QColor::HexRgb)),
        default_cyber_colors.muted_text);
    cyber_primary_text_color_ = color_setting_or_default(
        settings.value("cyber_primary_text_color",
                       default_cyber_colors.primary_text.name(QColor::HexRgb)),
        default_cyber_colors.primary_text);
    cyber_secondary_text_color_ = color_setting_or_default(
        settings.value("cyber_secondary_text_color",
                       default_cyber_colors.secondary_text.name(QColor::HexRgb)),
        default_cyber_colors.secondary_text);
    transparent_background_ = settings.value("transparent_background", false).toBool();
    background_transparency_ =
        std::clamp(settings.value("background_transparency", 20).toInt(), 0, 95);
    blur_background_ = settings.value("blur_background", false).toBool();
    if (settings.contains("sandbox_enabled")) {
        sandbox_enabled_ = settings.value("sandbox_enabled", true).toBool();
    } else {
        // Migrate the former inverse setting without silently changing an existing
        // user's local execution policy.
        sandbox_enabled_ = !settings.value("allow_unsafe_judging", false).toBool();
    }
    temporary_dir_ =
        settings.value("temporary_dir", QString::fromStdString(default_temporary_dir().string()))
            .toString()
            .toStdString();
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

void MainWindow::save_app_settings() const {
    QSettings settings("NeoThemis", "NeoThemis");
    settings.setValue("language", QString::fromStdString(language_));
    settings.setValue("theme", QString::fromStdString(theme_));
    settings.setValue("cyber_background_color",
                      cyber_background_color_.name(QColor::HexRgb));
    settings.setValue("cyber_primary_color", cyber_primary_color_.name(QColor::HexRgb));
    settings.setValue("cyber_secondary_color",
                      cyber_secondary_color_.name(QColor::HexRgb));
    settings.setValue("cyber_text_color", cyber_text_color_.name(QColor::HexRgb));
    settings.setValue("cyber_muted_text_color",
                      cyber_muted_text_color_.name(QColor::HexRgb));
    settings.setValue("cyber_primary_text_color",
                      cyber_primary_text_color_.name(QColor::HexRgb));
    settings.setValue("cyber_secondary_text_color",
                      cyber_secondary_text_color_.name(QColor::HexRgb));
    settings.setValue("transparent_background", transparent_background_);
    settings.setValue("background_transparency", background_transparency_);
    settings.setValue("blur_background", blur_background_);
    settings.setValue("sandbox_enabled", sandbox_enabled_);
    settings.remove("allow_unsafe_judging");
    settings.remove("background_blur_radius");
    settings.setValue("temporary_dir", QString::fromStdString(temporary_dir_.string()));
    settings.setValue("server_port", server_port_);
    settings.setValue("server_allow_lan", server_allow_lan_);
    settings.setValue("server_join_code", server_join_code_);
    settings.setValue("server_admin_password", server_admin_password_);
}

void MainWindow::build_toolbar() {
    auto* bar = menu_bar_ ? menu_bar_ : menuBar();
    bar->clear();

    auto* contest_menu = bar->addMenu(text("contest"));
    auto* open_folder_action =
        contest_menu->addAction(text("open_folder"), [this]() { open_contest(); });
    open_folder_action->setShortcut(QKeySequence("Ctrl+O"));
    auto* open_file_action =
        contest_menu->addAction(text("open_contest_file"), [this]() { open_contest_file(); });
    open_file_action->setShortcut(QKeySequence("Ctrl+Shift+O"));
    contest_menu->addSeparator();
    auto* save_action = contest_menu->addAction(text("save_contest_file"),
                                                [this]() { save_contest_container(false); });
    save_action->setShortcut(QKeySequence("Ctrl+S"));
    auto* save_as_action = contest_menu->addAction(text("save_contest_file_as"),
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
    judge_selected_action_ =
        judge_menu->addAction(text("judge_selected"), [this]() { start_judge(true); });
    judge_all_action_ = judge_menu->addAction(text("judge_all"), [this]() { start_judge(false); });
    stop_action_ = judge_menu->addAction(text("stop"), [this]() { request_stop_judge(); });
    stop_action_->setEnabled(false);

    auto* server_menu = bar->addMenu(text("server"));
    server_start_action_ = server_menu->addAction(text("start_local_server"),
                                                  [this]() { open_start_server_dialog(); });
    server_stop_action_ =
        server_menu->addAction(text("stop_local_server"), [this]() { stop_local_server(true); });
    update_server_actions();

    auto* export_menu = bar->addMenu(text("export"));
    export_menu->addAction(text("export_scoreboard"), [this]() { export_scoreboard_xlsx(); });
    export_menu->addAction(text("export_data"), [this]() { export_data_xlsx(); });

    auto* converter_menu = bar->addMenu(text("converter"));
    converter_menu->addAction(text("convert_old_contest_file"),
                              [this]() { convert_old_contest_to_ncontest(false); });
    converter_menu->addAction(text("convert_old_contest_folder"),
                              [this]() { convert_old_contest_to_ncontest(true); });

    auto* settings_menu = bar->addMenu(text("settings"));
    settings_menu->addAction(text("application_settings"), [this]() { open_settings_dialog(0); });
    settings_menu->addAction(text("contest_config"), [this]() { open_settings_dialog(1); });
    settings_menu->addAction(text("problem_config"), [this]() { open_settings_dialog(2); });
    settings_menu->addAction(text("server_settings"), [this]() { open_settings_dialog(3); });
    settings_menu->addAction(text("server_users"), [this]() { open_settings_dialog(4); });

    auto* help_menu = bar->addMenu(text("help"));
    help_menu->addAction(text("about"), [this]() { show_about_dialog(); });

    for (QMenu* menu : {contest_menu, contestant_menu, judge_menu, server_menu, export_menu,
                        converter_menu, settings_menu, help_menu}) {
        menu->setAttribute(Qt::WA_TranslucentBackground);
        menu->setWindowFlag(Qt::NoDropShadowWindowHint, true);
        menu->setWindowFlag(Qt::FramelessWindowHint, true);
    }
}

void MainWindow::open_settings_dialog(int initial_tab) {
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

void MainWindow::show_about_dialog() {
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

} // namespace neothemis::gui
