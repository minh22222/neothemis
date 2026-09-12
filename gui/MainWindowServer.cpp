#include "MainWindowPrivate.hpp"

namespace neothemis::gui {

QString MainWindow::generated_server_secret(int chars) const {
    QByteArray data;
    data.resize((chars + 1) / 2);
    for (qsizetype i = 0; i < data.size(); ++i) {
        data[i] = static_cast<char>(QRandomGenerator::system()->bounded(256));
    }
    return QString::fromLatin1(data.toHex()).left(chars);
}

QString MainWindow::local_server_executable() const {
    QString name =
#ifdef _WIN32
        "neothemis-server.exe";
#else
        "neothemis-server";
#endif
    fs::path alongside = path_from_qstring(QCoreApplication::applicationDirPath()) /
                         path_from_qstring(name);
    if (fs::exists(alongside)) {
        return qstring_from_path(alongside);
    }
    return name;
}

fs::path MainWindow::server_data_dir() const {
    if (contest_root_.empty()) {
        return {};
    }
    return validated_contest_path(".neothemis-server", "server data directory");
}

fs::path MainWindow::server_database_path() const {
    fs::path dir = server_data_dir();
    if (dir.empty()) {
        return {};
    }
    return validated_child_path(dir, "server.db", "server database");
}

QString MainWindow::server_database_display_path() const {
    try {
        const fs::path path = server_database_path();
        if (path.empty()) {
            return text("open_contest_first");
        }
        return qstring_from_path(path);
    } catch (const std::exception& ex) {
        return QString::fromUtf8(ex.what());
    }
}

bool MainWindow::valid_server_username(const QString& username) const {
    static const QRegularExpression pattern("^[A-Za-z0-9 _-]{1,64}$");
    static const QRegularExpression has_visible(".*[A-Za-z0-9].*");
    return pattern.match(username).hasMatch() && has_visible.match(username).hasMatch();
}

std::unique_ptr<neothemis::server::Database>
MainWindow::open_server_database(bool create_if_missing) const {
    if (contest_root_.empty()) {
        throw std::runtime_error(text("open_contest_first").toStdString());
    }
    fs::path database_path = server_database_path();
    (void)validated_child_path(database_path.parent_path(), "server.db-wal",
                               "server database WAL");
    (void)validated_child_path(database_path.parent_path(), "server.db-shm",
                               "server database shared memory");
    if (create_if_missing) {
        fs::create_directories(database_path.parent_path());
    } else if (!fs::exists(database_path)) {
        throw std::runtime_error(text("server_database_missing").toStdString());
    }

    auto database = std::make_unique<neothemis::server::Database>(
        database_path, server_secure_password_storage_);
    database->migrate_schema();
    return database;
}

void MainWindow::create_server_user(const QString& username, const QString& password,
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
    fs::path contestant_root;
    if (role == "contestant") {
        contestant_root = validated_child_path(contestants_root_path(),
                                               trimmed_username.toStdString(),
                                               "contestant directory");
    }

    auto database = open_server_database(true);
    try {
        database->create_user(trimmed_username, password, role);
    } catch (const std::exception& ex) {
        const QString error = QString::fromUtf8(ex.what());
        if (error.contains("UNIQUE", Qt::CaseInsensitive) ||
            error.contains("constraint", Qt::CaseInsensitive)) {
            throw std::runtime_error(text("server_user_exists").toStdString());
        }
        throw std::runtime_error(error.toStdString());
    }
    if (role == "contestant") {
        fs::create_directories(contestant_root);
    }
    mark_contest_dirty();
}

void MainWindow::change_server_user_password(const QString& username, const QString& password) {
    if (password.size() < 6 || password.size() > 128) {
        throw std::runtime_error(text("server_password_invalid").toStdString());
    }
    auto database = open_server_database(false);
    if (!database->change_user_password(username, password)) {
        throw std::runtime_error(text("server_user_not_found").toStdString());
    }
    mark_contest_dirty();
}

void MainWindow::remove_server_user(const QString& username) {
    auto database = open_server_database(false);
    const neothemis::server::RemoveUserResult result = database->remove_user(username);
    if (result == neothemis::server::RemoveUserResult::NotFound) {
        throw std::runtime_error(text("server_user_not_found").toStdString());
    }
    if (result == neothemis::server::RemoveUserResult::LastAdmin) {
        throw std::runtime_error(text("server_last_admin").toStdString());
    }
    mark_contest_dirty();
}

ServerUserSyncResult MainWindow::sync_server_users_from_contest() {
    if (contest_root_.empty()) {
        throw std::runtime_error(text("open_contest_first").toStdString());
    }
    const fs::path contestants_root = contestants_root_path();
    auto database = open_server_database(true);
    ServerUserSyncResult result;
    if (!fs::exists(contestants_root)) {
        return result;
    }
    std::set<QString> contest_usernames;
    for (const auto& entry : fs::directory_iterator(contestants_root)) {
        if (!entry.is_directory()) {
            continue;
        }
        const QString username = qstring_from_path(entry.path().filename()).trimmed();
        if (!valid_server_username(username)) {
            ++result.skipped;
            continue;
        }
        contest_usernames.insert(username);
    }

    const int skipped = result.skipped;
    result = database->sync_contestant_users(contest_usernames, "123456");
    result.skipped = skipped;

    if (result.created > 0 || result.removed > 0) {
        mark_contest_dirty();
    }
    return result;
}

std::pair<int, int> MainWindow::import_server_users_csv(const fs::path& path) {
    auto records = read_csv_records(path);
    int created = 0;
    int skipped = 0;
    bool first = true;
    for (const auto& row : records) {
        if (row.empty()) {
            continue;
        }
        QString first_cell = QString::fromStdString(row[0]).trimmed().toLower();
        if (first &&
            (first_cell == "username" || first_cell == "user" || first_cell == "contestant")) {
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
        QString role =
            row.size() >= 3 ? QString::fromStdString(row[2]).trimmed().toLower() : QString();
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

fs::file_time_type MainWindow::safe_last_write_time(const fs::path& path) const {
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

fs::path MainWindow::server_results_path() const {
    return contest_output_path(options_from_ui().output_csv);
}

fs::file_time_type MainWindow::server_database_activity_time() const {
    const fs::path database_path = server_database_path();
    const fs::path wal_path = validated_child_path(
        database_path.parent_path(), database_path.filename().string() + "-wal",
        "server database WAL");
    fs::file_time_type database_time = safe_last_write_time(database_path);
    fs::file_time_type wal_time = safe_last_write_time(wal_path);
    return std::max(database_time, wal_time);
}

void MainWindow::initialize_server_auto_refresh_state() {
    last_server_database_write_ = server_database_activity_time();
    last_server_results_write_ = safe_last_write_time(server_results_path());
    last_server_contestants_write_ = safe_last_write_time(contestants_root_path());
}

void MainWindow::start_server_auto_refresh() {
    initialize_server_auto_refresh_state();
    if (!server_refresh_timer_) {
        server_refresh_timer_ = new QTimer(this);
        server_refresh_timer_->setInterval(2000);
        QObject::connect(server_refresh_timer_, &QTimer::timeout,
                         [this]() { poll_server_auto_refresh(); });
    }
    server_refresh_timer_->start();
}

void MainWindow::stop_server_auto_refresh() {
    if (server_refresh_timer_) {
        server_refresh_timer_->stop();
    }
}

void MainWindow::poll_server_auto_refresh() {
    if (contest_root_.empty() || archive_running_.load() || judging_.load()) {
        return;
    }
    fs::file_time_type database_write;
    fs::file_time_type results_write;
    fs::file_time_type contestants_write;
    try {
        database_write = server_database_activity_time();
        results_write = safe_last_write_time(server_results_path());
        contestants_write = safe_last_write_time(contestants_root_path());
    } catch (const std::exception& ex) {
        stop_server_auto_refresh();
        log_->appendPlainText(text("server_process_error") + ": " +
                              QString::fromUtf8(ex.what()));
        return;
    }
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
            log_->appendPlainText(text("server_user_failed") + ": " + QString::fromUtf8(ex.what()));
        }
    }
}

void MainWindow::update_server_actions() {
    const bool running = server_process_ && server_process_->state() != QProcess::NotRunning;
    if (server_start_action_) {
        server_start_action_->setEnabled(!running);
    }
    if (server_stop_action_) {
        server_stop_action_->setEnabled(running);
    }
}

void MainWindow::append_server_output(const QByteArray& output) {
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

void MainWindow::open_start_server_dialog() {
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
    dialog->resize(560, 480);
    auto* layout = new QVBoxLayout(dialog);
    auto* form = new QFormLayout;

    auto* port = new QSpinBox(dialog);
    port->setRange(1024, 65535);
    port->setValue(server_port_);
    auto* allow_lan = new QCheckBox(dialog);
    allow_lan->setChecked(server_allow_lan_);
    auto* https_enabled = new QCheckBox(dialog);
    https_enabled->setChecked(server_https_enabled_);
    auto* join_code = new QLineEdit(server_join_code_, dialog);
    auto* admin_password = new QLineEdit(server_admin_password_, dialog);
    auto* tls_certificate = new QLineEdit(server_tls_certificate_, dialog);
    auto* tls_private_key = new QLineEdit(server_tls_private_key_, dialog);
    auto* data_path = new QLineEdit(server_database_display_path(), dialog);
    data_path->setReadOnly(true);
    auto* warning = new QLabel(text("server_security_warning"), dialog);
    warning->setWordWrap(true);
    warning->setObjectName("ServerWarning");

    form->addRow(text("server_port"), port);
    form->addRow(text("server_allow_lan"), allow_lan);
    form->addRow(text("server_enable_https"), https_enabled);
    form->addRow(text("server_join_code"), join_code);
    form->addRow(text("server_admin_password"), admin_password);
    form->addRow(text("server_tls_certificate"), tls_certificate);
    form->addRow(text("server_tls_private_key"), tls_private_key);
    form->addRow(text("server_database"), data_path);
    layout->addLayout(form);
    layout->addWidget(warning);

    auto update_tls_fields = [https_enabled, tls_certificate, tls_private_key]() {
        const bool enabled = https_enabled->isChecked();
        tls_certificate->setEnabled(enabled);
        tls_private_key->setEnabled(enabled);
    };
    QObject::connect(https_enabled, &QCheckBox::toggled, [this, update_tls_fields](bool enabled) {
        update_tls_fields();
        if (enabled) {
            QMessageBox::warning(this, text("server_https_warning_title"),
                                 text("server_https_warning"));
        }
    });
    update_tls_fields();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    auto* start_button = buttons->button(QDialogButtonBox::Ok);
    start_button->setText(text("start_local_server"));
    start_button->setIcon(QIcon());
    auto* close_button = buttons->button(QDialogButtonBox::Cancel);
    close_button->setText(text("close"));
    close_button->setIcon(QIcon());
    QObject::connect(buttons, &QDialogButtonBox::accepted,
                     [this, dialog, port, allow_lan, https_enabled, join_code, admin_password,
                      tls_certificate, tls_private_key]() {
                         server_port_ = port->value();
                         server_allow_lan_ = allow_lan->isChecked();
                         server_https_enabled_ = https_enabled->isChecked();
                         server_join_code_ = join_code->text().trimmed();
                         server_admin_password_ = admin_password->text();
                         server_tls_certificate_ = tls_certificate->text().trimmed();
                         server_tls_private_key_ = tls_private_key->text().trimmed();
                         save_app_settings();
                         if (start_local_server(server_port_, server_allow_lan_,
                                                 server_https_enabled_, server_join_code_,
                                                 server_admin_password_, server_tls_certificate_,
                                                 server_tls_private_key_)) {
                             dialog->accept();
                         }
                     });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

bool MainWindow::start_local_server(int port, bool allow_lan, bool https_enabled,
                                    const QString& join_code, const QString& admin_password,
                                    const QString& tls_certificate,
                                    const QString& tls_private_key) {
    if (join_code.isEmpty() || admin_password.isEmpty()) {
        QMessageBox::warning(this, text("server_start_failed"),
                             text("server_credentials_required"));
        return false;
    }
    if (https_enabled && (tls_certificate.isEmpty() || tls_private_key.isEmpty())) {
        QMessageBox::warning(this, text("server_start_failed"),
                             text("server_tls_files_required"));
        return false;
    }

    fs::path data_dir;
    try {
        data_dir = server_data_dir();
        fs::create_directories(data_dir);
        (void)server_database_path();
        (void)validated_child_path(data_dir, "server.db-wal", "server database WAL");
        (void)validated_child_path(data_dir, "server.db-shm",
                                   "server database shared memory");
    } catch (const std::exception& ex) {
        QMessageBox::critical(this, text("server_start_failed"), ex.what());
        return false;
    }

    auto* process = new QProcess(this);
    process->setProperty("stopRequested", false);
    process->setProgram(local_server_executable());
    QStringList args;
    args << "--contest" << qstring_from_path(contest_root_) << "--data"
         << qstring_from_path(data_dir) << "--port" << QString::number(port);
    if (allow_lan) {
        args << "--host" << "0.0.0.0" << "--allow-lan";
    }
    if (https_enabled) {
        args << "--tls-cert" << tls_certificate << "--tls-key" << tls_private_key;
    }
    process->setArguments(args);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("NEOTHEMIS_SERVER_JOIN_CODE", join_code);
    environment.insert("NEOTHEMIS_SERVER_ADMIN_PASSWORD", admin_password);
    environment.insert("NEOTHEMIS_SECURE_PASSWORD_STORAGE",
                       server_secure_password_storage_ ? "1" : "0");
    process->setProcessEnvironment(environment);
    process->setProcessChannelMode(QProcess::MergedChannels);

    QObject::connect(process, &QProcess::readyReadStandardOutput,
                     [this, process]() { append_server_output(process->readAllStandardOutput()); });
    QObject::connect(
        process, &QProcess::errorOccurred, [this, process](QProcess::ProcessError error) {
            if (process->property("stopRequested").toBool() || error == QProcess::Crashed) {
                return;
            }
            if (log_ && error == QProcess::FailedToStart) {
                log_->appendPlainText(text("server_start_failed") + ": " + process->errorString());
            } else if (log_) {
                log_->appendPlainText(text("server_process_error") + ": " + process->errorString());
            }
            update_server_actions();
        });
    QObject::connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
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
        return false;
    }

    const QString host = allow_lan ? "0.0.0.0" : "127.0.0.1";
    const QString scheme = https_enabled ? "https://" : "http://";
    const QString url = scheme + host + ":" + QString::number(port);
    log_->appendPlainText(text("server_started") + ": " + url);
    log_->appendPlainText(text("server_database") + ": " + server_database_display_path());
    log_->appendPlainText(text("server_join_code") + ": " + join_code);
    log_->appendPlainText(text("server_credentials_configured"));
    if (allow_lan) {
        log_->appendPlainText(text("server_lan_warning"));
        if (!https_enabled) {
            log_->appendPlainText(text("server_http_warning"));
        }
    }
    mark_contest_dirty();
    start_server_auto_refresh();
    update_server_actions();
    return true;
}

void MainWindow::stop_local_server(bool log_message) {
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

QWidget* MainWindow::build_server_settings_tab(QWidget* parent, SettingsDialog* settings_dialog) {
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
    auto* https_enabled = new QCheckBox(tab);
    https_enabled->setChecked(server_https_enabled_);
    auto* secure_password_storage = new QCheckBox(tab);
    secure_password_storage->setChecked(server_secure_password_storage_);
    auto* enable_ranking = new QCheckBox(tab);
    enable_ranking->setChecked(server_ranking_enabled_);
    auto* enable_details = new QCheckBox(tab);
    enable_details->setChecked(server_contestant_details_enabled_);
    auto* join_code = new QLineEdit(server_join_code_, tab);
    auto* admin_password = new QLineEdit(server_admin_password_, tab);
    auto* tls_certificate = new QLineEdit(server_tls_certificate_, tab);
    auto* tls_private_key = new QLineEdit(server_tls_private_key_, tab);
    auto* database = new QLineEdit(server_database_display_path(), tab);
    database->setReadOnly(true);
    auto* warning = new QLabel(text("server_settings_help"), tab);
    warning->setWordWrap(true);
    warning->setObjectName("ServerWarning");
    auto* save = new QPushButton(text("save_server_settings"), tab);

    form->addRow(text("server_port"), port);
    form->addRow(text("server_allow_lan"), allow_lan);
    form->addRow(text("server_enable_https"), https_enabled);
    form->addRow(text("server_secure_password_storage"), secure_password_storage);
    form->addRow(text("server_enable_ranking"), enable_ranking);
    form->addRow(text("server_enable_contestant_details"), enable_details);
    form->addRow(text("server_join_code"), join_code);
    form->addRow(text("server_admin_bootstrap_password"), admin_password);
    form->addRow(text("server_tls_certificate"), tls_certificate);
    form->addRow(text("server_tls_private_key"), tls_private_key);
    form->addRow(text("server_database"), database);
    form->addRow(warning);
    form->addRow(save);

    auto update_tls_fields = [https_enabled, tls_certificate, tls_private_key]() {
        const bool enabled = https_enabled->isChecked();
        tls_certificate->setEnabled(enabled);
        tls_private_key->setEnabled(enabled);
    };
    QObject::connect(https_enabled, &QCheckBox::toggled, [this, update_tls_fields](bool enabled) {
        update_tls_fields();
        if (enabled) {
            QMessageBox::warning(this, text("server_https_warning_title"),
                                 text("server_https_warning"));
        }
    });
    update_tls_fields();

    auto persist = [this, port, allow_lan, secure_password_storage, enable_ranking, enable_details,
                    https_enabled, join_code, admin_password, tls_certificate,
                    tls_private_key](bool notify) {
        const QString join = join_code->text().trimmed();
        const QString admin = admin_password->text();
        if (join.isEmpty() || admin.isEmpty()) {
            QMessageBox::warning(this, text("save_failed"), text("server_credentials_required"));
            return;
        }
        const bool changed = server_port_ != port->value() ||
                             server_allow_lan_ != allow_lan->isChecked() ||
                             server_https_enabled_ != https_enabled->isChecked() ||
                             server_secure_password_storage_ != secure_password_storage->isChecked() ||
                             server_ranking_enabled_ != enable_ranking->isChecked() ||
                             server_contestant_details_enabled_ != enable_details->isChecked() ||
                             server_join_code_ != join || server_admin_password_ != admin ||
                             server_tls_certificate_ != tls_certificate->text().trimmed() ||
                             server_tls_private_key_ != tls_private_key->text().trimmed();
        if (!changed) {
            if (notify) {
                log_->appendPlainText(text("saved_server_settings"));
            }
            return;
        }
        server_port_ = port->value();
        server_allow_lan_ = allow_lan->isChecked();
        server_https_enabled_ = https_enabled->isChecked();
        server_secure_password_storage_ = secure_password_storage->isChecked();
        server_ranking_enabled_ = enable_ranking->isChecked();
        server_contestant_details_enabled_ = enable_details->isChecked();
        server_join_code_ = join;
        server_admin_password_ = admin;
        server_tls_certificate_ = tls_certificate->text().trimmed();
        server_tls_private_key_ = tls_private_key->text().trimmed();
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
    QObject::connect(save, &QPushButton::clicked, [persist]() { persist(true); });
    settings_dialog->add_close_handler([persist]() { persist(false); });
    return tab;
}

void MainWindow::refresh_server_users_table(QTableWidget* users_table) {
    users_table->setRowCount(0);
    if (contest_root_.empty()) {
        return;
    }
    fs::path database_path = server_database_path();
    if (!fs::exists(database_path)) {
        return;
    }

    auto database = open_server_database(false);
    const bool secure_password_storage = database->secure_password_storage_enabled();
    for (const neothemis::server::ManagedUser& user : database->list_users()) {
        const int row = users_table->rowCount();
        users_table->insertRow(row);
        const QString password = user.password.isEmpty()
                                      ? (secure_password_storage
                                             ? text("server_password_hidden")
                                             : text("server_password_reset_required"))
                                      : user.password;
        const QString role_display =
            user.role == "admin" ? text("server_role_admin") : text("server_role_contestant");
        const QString created =
            QDateTime::fromSecsSinceEpoch(user.created_at).toString("yyyy-MM-dd HH:mm");

        auto add_readonly_item = [users_table, row](int column, const QString& value) {
            auto* item = new QTableWidgetItem(value);
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            users_table->setItem(row, column, item);
        };
        add_readonly_item(0, user.username);
        add_readonly_item(1, password);
        add_readonly_item(2, role_display);
        add_readonly_item(3, created);
    }
}

QWidget* MainWindow::build_server_users_tab(QWidget* parent) {
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
    users_table->setHorizontalHeaderLabels({text("server_user_username"),
                                            text("server_user_password"), text("server_user_role"),
                                            text("created_at")});
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
    QObject::connect(add, &QPushButton::clicked, [this, username, password, role, users_table]() {
        try {
            const QString new_username = username->text().trimmed();
            create_server_user(new_username, password->text(), role->currentData().toString());
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
            QMessageBox::information(this, text("server_users"), text("select_server_user"));
            return;
        }
        const QString selected_username = users_table->item(row, 0)->text();
        bool accepted = false;
        const QString new_password = QInputDialog::getText(
            this, text("change_server_password"), text("new_password_for").arg(selected_username),
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
            QMessageBox::information(this, text("server_users"), text("select_server_user"));
            return;
        }
        const QString selected_username = users_table->item(row, 0)->text();
        const QMessageBox::StandardButton answer =
            QMessageBox::warning(this, text("remove_server_user"),
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
        QString selected = QFileDialog::getOpenFileName(this, text("import_server_users_csv"),
                                                        QString(), text("csv_users_filter"));
        if (selected.isEmpty()) {
            return;
        }
        try {
            auto [created, skipped] = import_server_users_csv(path_from_qstring(selected));
            refresh_server_users_table(users_table);
            refresh_table(false);
            log_->appendPlainText(text("server_users_imported").arg(created).arg(skipped));
        } catch (const std::exception& ex) {
            QMessageBox::critical(this, text("server_users_import_failed"), ex.what());
        }
    });
    refresh_users();
    return tab;
}

} // namespace neothemis::gui
