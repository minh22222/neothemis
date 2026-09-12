#include "MainWindowPrivate.hpp"

#include "neothemis/Spreadsheet.hpp"

#include <cmath>

namespace neothemis::gui {

namespace {

void validate_contest_config_paths(const fs::path& contest_root,
                                   const neothemis::ContestConfig& config) {
    neothemis::JudgeOptions options;
    options.contest_root = contest_root;
    neothemis::apply_contest_config(config, options);
    neothemis::validate_judge_paths(options);
}

} // namespace

bool MainWindow::archive_operation_available() {
    if (archive_running_.load()) {
        QMessageBox::information(this, text("archive_operation_running"),
                                 text("wait_for_archive_operation"));
        return false;
    }
    if (judging_.load()) {
        QMessageBox::information(this, text("judge"), text("wait_for_judge_operation"));
        return false;
    }
    return true;
}

void MainWindow::join_archive_thread() {
    if (archive_thread_.joinable()) {
        archive_thread_.join();
    }
}

void MainWindow::set_archive_controls_enabled(bool enabled) {
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

void MainWindow::begin_archive_operation(const QString& label) {
    join_archive_thread();
    archive_running_.store(true);
    set_archive_controls_enabled(false);
    progress_->setRange(0, 0);
    progress_->setValue(0);
    current_archive_progress_key_.clear();
    log_->appendPlainText(label);
}

void MainWindow::update_archive_progress(std::uint64_t done, std::uint64_t total,
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
    int value =
        done > total ? max : static_cast<int>((done * static_cast<std::uint64_t>(max)) / total);
    progress_->setRange(0, max);
    progress_->setValue(value);
}

ArchiveProgress MainWindow::archive_progress_callback() {
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

        QMetaObject::invokeMethod(
            this,
            [this]() {
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
            },
            Qt::QueuedConnection);
    };
}

void MainWindow::finish_archive_operation(const QString& label) {
    join_archive_thread();
    archive_running_.store(false);
    set_archive_controls_enabled(true);
    progress_->setRange(0, 100);
    progress_->setValue(100);
    log_->appendPlainText(label);
}

void MainWindow::fail_archive_operation(const QString& label) {
    join_archive_thread();
    archive_running_.store(false);
    set_archive_controls_enabled(true);
    progress_->setRange(0, 100);
    progress_->setValue(0);
    log_->appendPlainText(label);
}

neothemis::ContestConfig MainWindow::current_contest_config() const {
    neothemis::ContestConfig config = loaded_contest_config_;
    config.core_name = core_name_;
    config.contestants_dir = contestants_dir_;
    config.tests_dir = tests_dir_;
    config.output_csv = output_csv_;
    config.scoreboard_csv = scoreboard_csv_;
    config.compiler = compiler_;
    config.compile_flags = compile_flags_;
    config.stack_limit_mb = stack_limit_mb_;
    config.parallel_jobs = parallel_jobs_;
    config.compile_jobs = compile_jobs_;
    config.test_jobs = test_jobs_;
    config.timing_focused = timing_focused_;
    config.forbidden_patterns = forbidden_patterns_;
    config.keep_workdir = keep_workdir_;
    config.server_ranking_enabled = server_ranking_enabled_;
    config.server_contestant_details_enabled = server_contestant_details_enabled_;
    return config;
}

void MainWindow::apply_contest_config(const neothemis::ContestConfig& config) {
    loaded_contest_config_ = config;
    core_name_ = config.core_name;
    contestants_dir_ = config.contestants_dir.string();
    tests_dir_ = config.tests_dir.string();
    output_csv_ = config.output_csv;
    scoreboard_csv_ = config.scoreboard_csv;
    compiler_ = config.compiler;
    compile_flags_ = config.compile_flags;
    stack_limit_mb_ = config.stack_limit_mb;
    parallel_jobs_ = config.parallel_jobs;
    compile_jobs_ = config.compile_jobs;
    test_jobs_ = config.test_jobs;
    timing_focused_ = config.timing_focused;
    forbidden_patterns_ = config.forbidden_patterns;
    keep_workdir_ = config.keep_workdir;
    server_ranking_enabled_ = config.server_ranking_enabled;
    server_contestant_details_enabled_ = config.server_contestant_details_enabled;
}

void MainWindow::reset_contest_config_defaults() {
    neothemis::ContestConfig config;
    config.forbidden_patterns = neothemis::default_forbidden_patterns();
    apply_contest_config(config);
}

fs::path MainWindow::ensure_ncontest_extension(fs::path path) const {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (extension != ".ncontest") {
        path += ".ncontest";
    }
    return path;
}

bool MainWindow::confirm_discard_unsaved_file(bool close_after_save) {
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

void MainWindow::mark_contest_dirty() {
    if (contest_from_file_) {
        contest_dirty_ = true;
    }
}

void MainWindow::cleanup_temp_root(const fs::path& root) {
    if (root.empty()) {
        return;
    }
    std::error_code ignored;
    fs::remove_all(root, ignored);
    temporary_roots_.erase(std::remove(temporary_roots_.begin(), temporary_roots_.end(), root),
                           temporary_roots_.end());
}

void MainWindow::cleanup_temporary_contests() {
    for (const auto& root : temporary_roots_) {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    temporary_roots_.clear();
    active_temp_root_.clear();
}

bool MainWindow::prepare_to_replace_contest() {
    if (!confirm_discard_unsaved_file()) {
        return false;
    }
    cleanup_temp_root(active_temp_root_);
    active_temp_root_.clear();
    contest_root_.clear();
    contest_from_file_ = false;
    contest_dirty_ = false;
    contest_file_path_.clear();
    reset_contest_config_defaults();
    contest_title_->setText(text("no_contest_open"));
    return true;
}

fs::path MainWindow::make_temporary_contest_root() {
    fs::create_directories(temporary_dir_);
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 1000; ++attempt) {
        fs::path root =
            temporary_dir_ / ("ncontest-" + std::to_string(now) + "-" + std::to_string(attempt));
        std::error_code ec;
        if (fs::create_directory(root, ec)) {
            temporary_roots_.push_back(root);
            return root;
        }
    }
    throw std::runtime_error("failed to create temporary contest folder");
}

fs::path MainWindow::detect_extracted_contest_root(const fs::path& root) const {
    if (fs::exists(root / neothemis::kContestConfigFilename)) {
        return root;
    }
    std::vector<fs::path> candidates;
    for (const auto& entry : fs::directory_iterator(root)) {
        if (entry.is_directory() && fs::exists(entry.path() / neothemis::kContestConfigFilename)) {
            candidates.push_back(entry.path());
        }
    }
    return candidates.size() == 1 ? candidates.front() : root;
}

void MainWindow::open_contest() {
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
    try {
        contest_root_ = path_from_qstring(dir);
        contest_from_file_ = false;
        contest_dirty_ = false;
        contest_file_path_.clear();
        load_contest_config();
        contest_title_->setText(qstring_from_path(contest_root_.filename()));
        refresh_table();
    } catch (const std::exception& ex) {
        contest_root_.clear();
        reset_contest_config_defaults();
        contest_title_->setText(text("no_contest_open"));
        QMessageBox::critical(this, text("open_failed"), ex.what());
    }
}

void MainWindow::open_contest_file() {
    QString selected = QFileDialog::getOpenFileName(this, text("open_contest_file"), QString(),
                                                    text("ncontest_open_filter"));
    if (selected.isEmpty()) {
        return;
    }
    open_contest_file(path_from_qstring(selected));
}

void MainWindow::open_contest_file(const fs::path& archive_path) {
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
                QMetaObject::invokeMethod(
                    this,
                    [this, archive_path, temp_root]() {
                        try {
                            contest_root_ = detect_extracted_contest_root(temp_root);
                            load_contest_config();
                            contest_file_path_ = archive_path;
                            active_temp_root_ = temp_root;
                            contest_from_file_ = true;
                            contest_dirty_ = false;
                            contest_title_->setText(
                                qstring_from_path(contest_file_path_.filename()));
                            refresh_table();
                            finish_archive_operation(text("file_operation_complete"));
                        } catch (const std::exception& ex) {
                            contest_root_.clear();
                            contest_file_path_.clear();
                            active_temp_root_.clear();
                            contest_from_file_ = false;
                            contest_dirty_ = false;
                            reset_contest_config_defaults();
                            contest_title_->setText(text("no_contest_open"));
                            cleanup_temp_root(temp_root);
                            fail_archive_operation(text("file_operation_failed"));
                            QMessageBox::critical(this, text("open_failed"), ex.what());
                        }
                    },
                    Qt::QueuedConnection);
            } catch (const std::exception& ex) {
                QMetaObject::invokeMethod(
                    this,
                    [this, temp_root, message = QString::fromUtf8(ex.what())]() {
                        cleanup_temp_root(temp_root);
                        fail_archive_operation(text("file_operation_failed"));
                        QMessageBox::critical(this, text("open_failed"), message);
                    },
                    Qt::QueuedConnection);
            }
        });
    } catch (const std::exception& ex) {
        cleanup_temp_root(temp_root);
        QMessageBox::critical(this, text("open_failed"), ex.what());
    }
}

void MainWindow::load_contest_config() {
    neothemis::ContestConfig config;
    config.forbidden_patterns = neothemis::default_forbidden_patterns();
    const fs::path path =
        validated_contest_path(neothemis::kContestConfigFilename, "contest config");
    if (fs::exists(path)) {
        config = neothemis::load_contest_config(path, neothemis::UnknownConfigKeyPolicy::Ignore);
        if (config.forbidden_patterns.empty()) {
            config.forbidden_patterns = neothemis::default_forbidden_patterns();
        }
    }
    validate_contest_config_paths(contest_root_, config);
    apply_contest_config(config);
}

void MainWindow::save_contest_config() const {
    if (contest_root_.empty()) {
        return;
    }
    const neothemis::ContestConfig config = current_contest_config();
    validate_contest_config_paths(contest_root_, config);
    neothemis::write_contest_config(
        validated_contest_path(neothemis::kContestConfigFilename, "contest config"), config);
}

bool MainWindow::save_contest_container(bool save_as) {
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
                    qstring_from_path(target.empty() ? fs::path("contest.ncontest") : target),
                    text("ncontest_save_filter"));
                if (selected.isEmpty()) {
                    return false;
                }
                target = ensure_ncontest_extension(path_from_qstring(selected));
            }
            fs::path source_root = contest_root_;
            bool was_save_as = save_as;
            begin_archive_operation(text("saving_contest_file"));
            ArchiveProgress progress = archive_progress_callback();
            archive_thread_ = std::thread([this, source_root, target, was_save_as, progress]() {
                try {
                    write_contest_archive(target, source_root, progress);
                    QMetaObject::invokeMethod(
                        this,
                        [this, target, was_save_as]() {
                            contest_file_path_ = target;
                            contest_dirty_ = false;
                            contest_title_->setText(
                                qstring_from_path(contest_file_path_.filename()));
                            finish_archive_operation(text("file_operation_complete"));
                            log_->appendPlainText(
                                text(was_save_as ? "save_as_complete" : "save_complete") + ": " +
                                qstring_from_path(target));
                            if (pending_close_after_save_) {
                                pending_close_after_save_ = false;
                                QTimer::singleShot(0, this, &QWidget::close);
                            }
                        },
                        Qt::QueuedConnection);
                } catch (const std::exception& ex) {
                    QMetaObject::invokeMethod(
                        this,
                        [this, message = QString::fromUtf8(ex.what())]() {
                            pending_close_after_save_ = false;
                            mark_contest_dirty();
                            fail_archive_operation(text("file_operation_failed"));
                            QMessageBox::critical(this, text("save_failed"), message);
                        },
                        Qt::QueuedConnection);
                }
            });
        } else {
            fs::path default_target = contest_file_path_;
            if (default_target.empty()) {
                default_target = contest_root_.parent_path() / contest_root_.filename();
                default_target += ".ncontest";
            }
            QString selected = QFileDialog::getSaveFileName(
                this, text(save_as ? "save_contest_file_as" : "save_contest_file"),
                qstring_from_path(default_target), text("ncontest_save_filter"));
            if (selected.isEmpty()) {
                return false;
            }
            fs::path source_root = contest_root_;
            fs::path target = ensure_ncontest_extension(path_from_qstring(selected));
            bool was_save_as = save_as;
            begin_archive_operation(text("saving_contest_file"));
            ArchiveProgress progress = archive_progress_callback();
            archive_thread_ = std::thread([this, source_root, target, was_save_as, progress]() {
                try {
                    write_contest_archive(target, source_root, progress);
                    QMetaObject::invokeMethod(
                        this,
                        [this, target, was_save_as]() {
                            contest_file_path_ = target;
                            contest_from_file_ = true;
                            contest_dirty_ = false;
                            contest_title_->setText(
                                qstring_from_path(contest_file_path_.filename()));
                            finish_archive_operation(text("file_operation_complete"));
                            log_->appendPlainText(
                                text(was_save_as ? "save_as_complete" : "save_complete") + ": " +
                                qstring_from_path(target));
                            if (pending_close_after_save_) {
                                pending_close_after_save_ = false;
                                QTimer::singleShot(0, this, &QWidget::close);
                            }
                        },
                        Qt::QueuedConnection);
                } catch (const std::exception& ex) {
                    QMetaObject::invokeMethod(
                        this,
                        [this, message = QString::fromUtf8(ex.what())]() {
                            pending_close_after_save_ = false;
                            fail_archive_operation(text("file_operation_failed"));
                            QMessageBox::critical(this, text("save_failed"), message);
                        },
                        Qt::QueuedConnection);
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

void MainWindow::convert_old_contest_to_ncontest(bool source_is_folder) {
    if (!archive_operation_available()) {
        return;
    }

    QString source;
    if (source_is_folder) {
        source = QFileDialog::getExistingDirectory(this, text("convert_old_contest_folder"));
    } else {
        source = QFileDialog::getOpenFileName(this, text("convert_old_contest_file"), QString(),
                                              text("themis_contest_filter"));
    }
    if (source.isEmpty()) {
        return;
    }

    fs::path source_path = path_from_qstring(source);
    fs::path default_output = source_path;
    if (source_is_folder) {
        default_output = source_path.parent_path() / source_path.filename();
        default_output += ".ncontest";
    } else {
        default_output.replace_extension(".ncontest");
    }

    QString selected_output = QFileDialog::getSaveFileName(
        this, text("convert_old_contest_file"), qstring_from_path(default_output),
        text("ncontest_save_filter"));
    if (selected_output.isEmpty()) {
        return;
    }

    fs::path output_path = ensure_ncontest_extension(path_from_qstring(selected_output));
    begin_archive_operation(text("converting_contest_file"));
    ArchiveProgress ui_progress = archive_progress_callback();
    neothemis::ArchiveProgress core_progress =
        [ui_progress](std::uint64_t done, std::uint64_t total, const std::string& label) {
            ui_progress(done, total, label.c_str());
        };

    archive_thread_ = std::thread([this, source_path, output_path, core_progress]() {
        try {
            neothemis::convert_old_themis_contest(source_path, output_path, core_progress);
            QMetaObject::invokeMethod(
                this,
                [this, output_path]() {
                    finish_archive_operation(text("convert_complete"));
                    log_->appendPlainText(text("convert_complete") + ": " +
                                          qstring_from_path(output_path));
                    QMessageBox::information(this, text("converter"),
                                             text("convert_complete") + "\n" +
                                                 qstring_from_path(output_path));
                },
                Qt::QueuedConnection);
        } catch (const std::exception& ex) {
            QMetaObject::invokeMethod(
                this,
                [this, message = QString::fromUtf8(ex.what())]() {
                    fail_archive_operation(text("file_operation_failed"));
                    QMessageBox::critical(this, text("file_operation_failed"), message);
                },
                Qt::QueuedConnection);
        }
    });
}

void MainWindow::add_contestants_from_folder() {
    if (!archive_operation_available()) {
        return;
    }
    if (contest_root_.empty()) {
        QMessageBox::information(this, text("no_contest"), text("open_contest_first"));
        return;
    }

    QString selected = QFileDialog::getExistingDirectory(this, text("add_contestants_from_folder"));
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

        const fs::path destination_root = contestants_root_path();
        int imported = 0;
        int skipped = 0;
        fs::create_directories(destination_root);
        for (const fs::path& source : sources) {
            const fs::path name = source.filename();
            if (name.empty() || name == "." || name == "..") {
                ++skipped;
                continue;
            }
            const fs::path destination =
                validated_child_path(destination_root, name, "contestant directory");
            std::error_code equivalent_error;
            if (fs::exists(destination) && fs::equivalent(source, destination, equivalent_error) &&
                !equivalent_error) {
                ++skipped;
                continue;
            }
            fs::create_directories(destination);
            fs::copy(source, destination,
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing |
                         fs::copy_options::skip_symlinks);
            ++imported;
        }
        mark_contest_dirty();
        refresh_table(false);
        log_->appendPlainText(text("contestants_imported").arg(imported).arg(skipped));
    } catch (const std::exception& ex) {
        QMessageBox::critical(this, text("contestant_import_failed"), ex.what());
    }
}

void MainWindow::refresh_table(bool log_opened) {
    if (contest_root_.empty()) {
        return;
    }
    try {
        auto overview = neothemis::inspect_contest(options_from_ui());
        for (const std::string& problem : overview.problems) {
            (void)problem_root_path(problem);
        }
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
                                  qstring_from_path(contest_root_));
        }
    } catch (const std::exception& ex) {
        contestants_.clear();
        problems_.clear();
        problem_test_counts_.clear();
        source_ready_.clear();
        score_cells_.clear();
        cell_texts_.clear();
        result_details_.clear();
        active_judge_pairs_.clear();
        populate_table();
        QMessageBox::critical(this, text("open_failed"), ex.what());
    }
}

fs::path MainWindow::choose_export_path(const std::string& filename) {
    QString default_path = contest_root_.empty()
                               ? QString::fromStdString(filename)
                               : qstring_from_path(contest_root_ / filename);
    QString selected = QFileDialog::getSaveFileName(this, text("export_workbook"), default_path,
                                                    text("xlsx_filter"));
    if (selected.isEmpty()) {
        return {};
    }
    fs::path path = path_from_qstring(selected);
    if (path.extension().string() != ".xlsx") {
        path += ".xlsx";
    }
    return path;
}

bool MainWindow::export_is_available() {
    if (archive_running_.load()) {
        QMessageBox::information(this, text("archive_operation_running"),
                                 text("wait_for_archive_operation"));
        return false;
    }
    if (judging_.load()) {
        QMessageBox::information(this, text("judge_running"), text("wait_for_export_judge"));
        return false;
    }
    if (contest_root_.empty()) {
        QMessageBox::information(this, text("no_contest"), text("open_contest_first"));
        return false;
    }
    if (all_recorded_results().empty()) {
        QMessageBox::information(this, text("no_results"), text("no_results_detail"));
        return false;
    }
    return true;
}

void MainWindow::export_scoreboard_xlsx() {
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
        neothemis::write_xlsx_file(path, "Scoreboard", rows, widths);
        log_->appendPlainText(text("exported") + " " + qstring_from_path(path));
        QMessageBox::information(this, text("export_complete"),
                                 text("exported") + " " + qstring_from_path(path));
    } catch (const std::exception& ex) {
        QMessageBox::critical(this, text("export_failed"), ex.what());
    }
}

void MainWindow::export_data_xlsx() {
    if (!export_is_available()) {
        return;
    }
    fs::path path = choose_export_path("results-data.xlsx");
    if (path.empty()) {
        return;
    }

    std::vector<XlsxRow> rows;
    rows.push_back({xlsx_text("contestant"), xlsx_text("problem"), xlsx_text("test"),
                    xlsx_text("verdict"), xlsx_text("time_ms"), xlsx_text("exit_code"),
                    xlsx_text("max_points"), xlsx_text("earned_points"), xlsx_text("message")});
    for (const auto& result : all_recorded_results()) {
        rows.push_back({xlsx_text(result.contestant), xlsx_text(result.problem),
                        xlsx_text(result.test), xlsx_text(neothemis::to_string(result.verdict)),
                        xlsx_number(static_cast<double>(result.time_ms)),
                        xlsx_number(static_cast<double>(result.exit_code)),
                        xlsx_number(result.max_points), xlsx_number(result.earned_points),
                        xlsx_text(result.message)});
    }

    std::vector<double> widths{28.0, 14.0, 12.0, 10.0, 12.0, 12.0, 12.0, 14.0, 48.0};
    try {
        neothemis::write_xlsx_file(path, "Data", rows, widths);
        log_->appendPlainText(text("exported") + " " + qstring_from_path(path));
        QMessageBox::information(this, text("export_complete"),
                                 text("exported") + " " + qstring_from_path(path));
    } catch (const std::exception& ex) {
        QMessageBox::critical(this, text("export_failed"), ex.what());
    }
}

QWidget* MainWindow::build_contest_tab(QWidget* parent, SettingsDialog* settings_dialog) {
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
    auto worker_setting = [tab](unsigned int count) {
        auto* setting = new QSpinBox(tab);
        setting->setRange(0, std::numeric_limits<int>::max());
        setting->setValue(static_cast<int>(std::min(
            count, static_cast<unsigned int>(std::numeric_limits<int>::max()))));
        return setting;
    };
    auto* parallel = worker_setting(parallel_jobs_);
    parallel->setSpecialValueText(text("automatic"));
    auto* compile_jobs = worker_setting(compile_jobs_);
    compile_jobs->setSpecialValueText(text("inherit_worker_limit"));
    auto* test_jobs = worker_setting(test_jobs_);
    test_jobs->setSpecialValueText(text("inherit_worker_limit"));
    auto* timing_focused = new QCheckBox(tab);
    timing_focused->setChecked(timing_focused_);
    timing_focused->setToolTip(text("timing_focused_hint"));
    test_jobs->setEnabled(!timing_focused_);
    QObject::connect(timing_focused, &QCheckBox::toggled, test_jobs,
                     [test_jobs](bool checked) { test_jobs->setEnabled(!checked); });
    auto* keep = new QCheckBox(tab);
    keep->setChecked(keep_workdir_);
    auto* save = new QPushButton(text("save_contest_config"), tab);

    form->addRow(text("compiler"), compiler_widget);
    form->addRow(text("compile_flags"), flags);
    form->addRow(text("contestants_dir"), contestants);
    form->addRow(text("tests_dir"), tests);
    form->addRow(text("stack_mb"), stack);
    form->addRow(text("parallel_jobs"), parallel);
    form->addRow(text("compile_jobs"), compile_jobs);
    form->addRow(text("test_jobs"), test_jobs);
    form->addRow(text("timing_focused"), timing_focused);
    form->addRow(text("keep_workdir"), keep);
    form->addRow(save);

    QObject::connect(browse_compiler, &QPushButton::clicked, [this, compiler]() {
#ifdef Q_OS_WIN
        const QString filter = text("compiler_executable_filter_windows");
#else
        const QString filter = text("compiler_executable_filter");
#endif
        const QString selected =
            QFileDialog::getOpenFileName(this, text("select_compiler"), compiler->text(), filter);
        if (!selected.isEmpty()) {
            compiler->setText(selected);
        }
    });

    auto persist = [this, compiler, flags, contestants, tests, stack, parallel,
                    compile_jobs, test_jobs, timing_focused, keep](bool notify) {
        if (contest_root_.empty()) {
            return;
        }
        const std::string new_compiler = compiler->text().toStdString();
        const std::string new_flags = flags->text().toStdString();
        const std::string new_contestants = contestants->text().toStdString();
        const std::string new_tests = tests->text().toStdString();
        const auto new_stack = static_cast<std::uint64_t>(stack->value());
        auto worker_value = [](const QSpinBox* setting, unsigned int original) {
            // QSpinBox is signed; leave larger valid file values intact unless edited.
            if (original > static_cast<unsigned int>(setting->maximum()) &&
                setting->value() == setting->maximum()) {
                return original;
            }
            return static_cast<unsigned int>(setting->value());
        };
        const auto new_parallel = worker_value(parallel, parallel_jobs_);
        const auto new_compile_jobs = worker_value(compile_jobs, compile_jobs_);
        const auto new_test_jobs = worker_value(test_jobs, test_jobs_);
        const bool new_timing_focused = timing_focused->isChecked();
        const bool new_keep = keep->isChecked();
        const bool changed = compiler_ != new_compiler || compile_flags_ != new_flags ||
                             contestants_dir_ != new_contestants || tests_dir_ != new_tests ||
                             stack_limit_mb_ != new_stack || parallel_jobs_ != new_parallel ||
                             compile_jobs_ != new_compile_jobs || test_jobs_ != new_test_jobs ||
                             timing_focused_ != new_timing_focused ||
                             keep_workdir_ != new_keep;
        if (!changed) {
            if (notify) {
                log_->appendPlainText(text("saved_contest_config"));
            }
            return;
        }
        try {
            neothemis::ContestConfig proposed = current_contest_config();
            proposed.compiler = new_compiler;
            proposed.compile_flags = new_flags;
            proposed.contestants_dir = new_contestants;
            proposed.tests_dir = new_tests;
            proposed.stack_limit_mb = new_stack;
            proposed.parallel_jobs = new_parallel;
            proposed.compile_jobs = new_compile_jobs;
            proposed.test_jobs = new_test_jobs;
            proposed.timing_focused = new_timing_focused;
            proposed.keep_workdir = new_keep;
            validate_contest_config_paths(contest_root_, proposed);
            neothemis::write_contest_config(
                validated_contest_path(neothemis::kContestConfigFilename, "contest config"),
                proposed);
            apply_contest_config(proposed);
            mark_contest_dirty();
            refresh_table();
            if (notify) {
                log_->appendPlainText(text("saved_contest_config"));
            }
        } catch (const std::exception& ex) {
            QMessageBox::critical(this, text("save_failed"), ex.what());
        }
    };
    QObject::connect(save, &QPushButton::clicked, [persist]() { persist(true); });
    settings_dialog->add_close_handler([persist]() { persist(false); });
    return tab;
}

QWidget* MainWindow::build_problem_tab(QWidget* parent, SettingsDialog* settings_dialog) {
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
    checker->addItem("testlib");
    checker->addItem("token");
    checker->setEditable(true);
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
    auto loaded_problem_config = std::make_shared<neothemis::ProblemConfig>();

    auto load_problem = [this, problem, time, memory, points, checker, test_table, problem_loading,
                         problem_dirty, loaded_problem_config]() {
        *problem_loading = true;
        std::string name = problem->currentText().toStdString();
        if (name.empty()) {
            test_table->setRowCount(0);
            *problem_loading = false;
            *problem_dirty = false;
            return;
        }
        fs::path problem_root;
        try {
            problem_root = problem_root_path(name);
        } catch (const std::exception& ex) {
            test_table->setRowCount(0);
            *problem_loading = false;
            *problem_dirty = false;
            QMessageBox::critical(this, text("open_failed"), ex.what());
            return;
        }
        const fs::path config_path = validated_child_path(
            problem_root, neothemis::kProblemConfigFilename, "problem config");
        neothemis::ProblemConfig config;
        if (fs::exists(config_path)) {
            config = neothemis::load_problem_config(config_path,
                                                    neothemis::UnknownConfigKeyPolicy::Ignore);
        }
        *loaded_problem_config = config;
        time->setValue(static_cast<int>(config.time_limit_ms));
        memory->setValue(static_cast<int>(config.memory_limit_mb));
        points->setText(QString::fromStdString(
            neothemis::format_config_number(config.default_points)));
        checker->setCurrentText(QString::fromStdString(config.checker));

        std::vector<std::string> tests = test_names_for_problem(problem_root);
        test_table->setRowCount(static_cast<int>(tests.size()));
        for (std::size_t row = 0; row < tests.size(); ++row) {
            const std::string& test_name = tests[row];
            auto* test_item = new QTableWidgetItem(QString::fromStdString(test_name));
            test_item->setFlags(test_item->flags() & ~Qt::ItemIsEditable);
            test_table->setItem(static_cast<int>(row), 0, test_item);

            std::string override_points;
            for (const auto& key : neothemis::test_point_keys(test_name)) {
                auto found = config.test_points.find(key);
                if (found != config.test_points.end()) {
                    override_points = neothemis::format_config_number(found->second);
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

    auto persist = [this, problem, time, memory, points, checker, test_table,
                    problem_dirty, loaded_problem_config](bool notify) {
        try {
            std::string name = problem->currentText().toStdString();
            if (name.empty()) {
                return;
            }
            const fs::path problem_root = problem_root_path(name);
            if (!*problem_dirty) {
                if (notify) {
                    log_->appendPlainText(text("saved_problem_config") + problem->currentText());
                }
                return;
            }
            std::string default_points = trim(points->text().toStdString());
            auto ensure_points = [&](const std::string& value) {
                if (value.empty()) {
                    return;
                }
                std::size_t parsed = 0;
                double number = 0.0;
                try {
                    number = std::stod(value, &parsed);
                } catch (const std::exception&) {
                    throw std::runtime_error(text("invalid_points_detail").toStdString());
                }
                if (parsed != value.size() || !std::isfinite(number) || number < 0.0) {
                    throw std::runtime_error(text("invalid_points_detail").toStdString());
                }
            };
            ensure_points(default_points);

            neothemis::ProblemConfig config = *loaded_problem_config;
            config.time_limit_ms = static_cast<std::uint64_t>(time->value());
            config.memory_limit_mb = static_cast<std::uint64_t>(memory->value());
            config.default_points = std::stod(default_points.empty() ? "1" : default_points);
            config.checker = checker->currentText().toStdString();
            for (int row = 0; row < test_table->rowCount(); ++row) {
                auto* test_item = test_table->item(row, 0);
                auto* point_item = test_table->item(row, 1);
                if (!test_item) {
                    continue;
                }
                std::string value =
                    point_item ? trim(point_item->text().toStdString()) : std::string();
                ensure_points(value);
                const std::string test_name = test_item->text().toStdString();
                bool had_override = false;
                double old_value = 0.0;
                for (const auto& key : neothemis::test_point_keys(test_name)) {
                    const auto found = loaded_problem_config->test_points.find(key);
                    if (found != loaded_problem_config->test_points.end()) {
                        had_override = true;
                        old_value = found->second;
                        break;
                    }
                }
                const bool has_override = !value.empty();
                const double new_value = has_override ? std::stod(value) : 0.0;
                if (had_override == has_override &&
                    (!has_override || old_value == new_value)) {
                    continue;
                }

                // An exact entry shadows numeric aliases while leaving those
                // aliases available to any other matching test. Clearing an
                // inherited alias is represented by the new default value.
                if (has_override) {
                    config.test_points[test_name] = new_value;
                } else if (had_override) {
                    config.test_points[test_name] = config.default_points;
                } else {
                    config.test_points.erase(test_name);
                }
            }

            fs::create_directories(problem_root);
            neothemis::write_problem_config(
                validated_child_path(problem_root, neothemis::kProblemConfigFilename,
                                     "problem config"),
                config);
            *loaded_problem_config = config;
            mark_contest_dirty();
            *problem_dirty = false;
            if (notify) {
                log_->appendPlainText(text("saved_problem_config") + problem->currentText());
            }
        } catch (const std::exception& ex) {
            QMessageBox::critical(this, text("save_failed"), ex.what());
        }
    };
    QObject::connect(save, &QPushButton::clicked, [persist]() { persist(true); });
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

} // namespace neothemis::gui
