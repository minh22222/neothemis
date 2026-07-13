#include "MainWindowPrivate.hpp"

namespace neothemis::gui {

namespace {

QColor blend_theme_colors(const QColor& background,
                          const QColor& primary,
                          const QColor& secondary,
                          double primary_weight,
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

bool decode_result_row(const neothemis::CsvRow& fields, neothemis::TestResult& result) {
    if (fields.size() < 9) {
        return false;
    }
    try {
        result.contestant = fields[0];
        result.problem = fields[1];
        result.test = fields[2];
        result.verdict = neothemis::verdict_from_string(fields[3]);
        result.time_ms = static_cast<std::uint64_t>(std::stoull(fields[4]));
        result.exit_code = std::stoi(fields[5]);
        result.max_points = std::stod(fields[6]);
        result.earned_points = std::stod(fields[7]);
        result.message = fields[8];
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace

neothemis::JudgeOptions MainWindow::options_from_ui() const {
    neothemis::JudgeOptions options;
    options.contest_root = contest_root_;
    neothemis::apply_contest_config(current_contest_config(), options);
    options.execution_security = sandbox_enabled_
                                     ? neothemis::ExecutionSecurity::Required
                                     : neothemis::ExecutionSecurity::ExplicitlyUnsafe;
    neothemis::validate_judge_paths(options);
    return options;
}

fs::path MainWindow::validated_contest_path(const fs::path& path, const char* name) const {
    if (contest_root_.empty()) {
        throw std::runtime_error("contest root is empty");
    }
    return validated_child_path(contest_root_, path, name);
}

fs::path MainWindow::validated_child_path(const fs::path& root, const fs::path& path,
                                          const char* name) const {
    if (root.empty()) {
        throw std::runtime_error(std::string(name) + " root is empty");
    }
    if (path.empty()) {
        throw std::runtime_error(std::string(name) + " path is empty");
    }

    std::error_code root_error;
    const fs::path canonical_root = fs::weakly_canonical(root, root_error);
    if (root_error || !canonical_root.is_absolute()) {
        throw std::runtime_error(std::string("failed to resolve ") + name + " root: " +
                                 root.string());
    }

    const fs::path candidate = path.is_absolute() ? path : canonical_root / path;
    std::error_code candidate_error;
    const fs::path canonical_candidate = fs::weakly_canonical(candidate, candidate_error);
    if (candidate_error || !canonical_candidate.is_absolute()) {
        throw std::runtime_error(std::string("failed to resolve ") + name + " path: " +
                                 path.string());
    }

    auto root_part = canonical_root.begin();
    auto candidate_part = canonical_candidate.begin();
    for (; root_part != canonical_root.end(); ++root_part, ++candidate_part) {
        if (candidate_part == canonical_candidate.end() || *root_part != *candidate_part) {
            throw std::runtime_error(std::string(name) + " path escapes its configured root: " +
                                     path.string());
        }
    }
    if (candidate_part == canonical_candidate.end()) {
        throw std::runtime_error(std::string(name) + " path must be below its configured root");
    }
    return canonical_candidate;
}

fs::path MainWindow::contestants_root_path() const {
    const neothemis::JudgeOptions options = options_from_ui();
    return validated_contest_path(options.contestants_dir, "contestants directory");
}

fs::path MainWindow::problem_root_path(const std::string& problem) const {
    validate_path_component(problem, "problem name");
    const neothemis::JudgeOptions options = options_from_ui();
    const fs::path tests_root = validated_contest_path(options.tests_dir, "tests directory");
    return validated_child_path(tests_root, problem, "problem directory");
}

fs::path MainWindow::contest_output_path(const fs::path& path) const {
    return validated_contest_path(path, "contest output");
}

void MainWindow::record_result(const neothemis::TestResult& result) {
    std::string key = cell_key(result.contestant, result.problem);
    result_details_[key].push_back(result);
    CellScore& score = score_cells_[key];
    score.earned += result.earned_points;
    score.max += result.max_points;
    ++score.completed;
}

std::vector<neothemis::TestResult> MainWindow::all_recorded_results() const {
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

void MainWindow::load_existing_results() {
    score_cells_.clear();
    result_details_.clear();

    neothemis::JudgeOptions options = options_from_ui();
    neothemis::validate_judge_paths(options);
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
        if (fields.size() < 2 || known_contestants.count(fields[0]) == 0 ||
            known_problems.count(fields[1]) == 0) {
            continue;
        }
        neothemis::TestResult result;
        if (decode_result_row(fields, result)) {
            record_result(result);
        } else {
            log_->appendPlainText(text("malformed_row_skipped"));
        }
    }
}

void MainWindow::write_current_csv_outputs() {
    neothemis::JudgeOptions options = options_from_ui();
    neothemis::validate_judge_paths(options);
    std::vector<neothemis::TestResult> rows = all_recorded_results();

    fs::path details_path = contest_output_path(options.output_csv);
    std::ostringstream details;
    neothemis::write_csv(details, rows);
    neothemis::CsvTable encoded_rows = neothemis::parse_csv(details.str());
    if (encoded_rows.empty()) {
        throw std::runtime_error("judge produced an invalid empty CSV document");
    }
    const neothemis::CsvRow header = encoded_rows.front();
    neothemis::CsvTable replacements;
    for (std::size_t index = 1; index < encoded_rows.size(); ++index) {
        const neothemis::CsvRow& row = encoded_rows[index];
        if (row.size() >= 2 && active_judge_pairs_.count({row[0], row[1]}) != 0) {
            replacements.push_back(row);
        }
    }
    fs::path scoreboard_path = contest_output_path(options.scoreboard_csv);
    neothemis::replace_csv_rows_by_key_atomic(
        details_path, header, replacements, active_judge_pairs_,
        [&](const neothemis::CsvTable& merged_rows) {
            std::ostringstream scoreboard;
            neothemis::write_scoreboard_csv_from_results(scoreboard, merged_rows);
            neothemis::write_text_file_atomic(scoreboard_path, scoreboard.str());
        });
    mark_contest_dirty();
}

std::string MainWindow::status_for_scoreboard_cell(const std::string& contestant,
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

QString MainWindow::terminal_status_for_cell(const std::string& contestant,
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

void MainWindow::rebuild_maps() {
    contestant_rows_.clear();
    problem_columns_.clear();
    for (std::size_t i = 0; i < contestants_.size(); ++i) {
        contestant_rows_[contestants_[i]] = static_cast<int>(i);
    }
    for (std::size_t i = 0; i < problems_.size(); ++i) {
        problem_columns_[problems_[i]] = static_cast<int>(i + 1);
    }
}

int MainWindow::total_column() const { return static_cast<int>(problems_.size() + 1); }

std::string MainWindow::cell_key(const std::string& contestant, const std::string& problem) const {
    return contestant + "\n" + problem;
}

double MainWindow::earned_for_problem(const std::string& contestant,
                                      const std::string& problem) const {
    auto it = score_cells_.find(cell_key(contestant, problem));
    return it == score_cells_.end() ? 0.0 : it->second.earned;
}

double MainWindow::max_for_problem(const std::string& contestant,
                                   const std::string& problem) const {
    auto it = score_cells_.find(cell_key(contestant, problem));
    return it == score_cells_.end() ? 0.0 : it->second.max;
}

double MainWindow::total_earned_for(const std::string& contestant) const {
    double total = 0.0;
    for (const auto& problem : problems_) {
        total += earned_for_problem(contestant, problem);
    }
    return total;
}

double MainWindow::total_max_for(const std::string& contestant) const {
    double total = 0.0;
    for (const auto& problem : problems_) {
        total += max_for_problem(contestant, problem);
    }
    return total;
}

QString MainWindow::score_cell_text(const CellScore& score, int expected) const {
    QString score_text = format_points(score.earned) + "/" + format_points(score.max);
    if (score.completed >= expected) {
        return format_points(score.earned);
    }
    return score_text + "\n" + text("running") + " " + QString::number(score.completed) + "/" +
           QString::number(expected);
}

QString MainWindow::problem_cell_text(const std::string& contestant,
                                      const std::string& problem) const {
    std::string key = cell_key(contestant, problem);
    auto text_it = cell_texts_.find(key);
    if (text_it != cell_texts_.end()) {
        return text_it->second;
    }

    auto score_it = score_cells_.find(key);
    if (score_it == score_cells_.end()) {
        auto source_it = source_ready_.find(key);
        return source_it != source_ready_.end() && source_it->second ? text("ready")
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

void MainWindow::populate_table() {
    rebuild_maps();
    table_->clear();
    table_->setRowCount(static_cast<int>(contestants_.size()));
    table_->setColumnCount(total_column() + 1);
    table_->setHorizontalHeaderItem(0, new QTableWidgetItem(text("contestant")));
    for (std::size_t col = 0; col < problems_.size(); ++col) {
        table_->setHorizontalHeaderItem(
            static_cast<int>(col + 1),
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

void MainWindow::load_problem_test_counts() {
    problem_test_counts_.clear();
    for (const auto& problem : problems_) {
        int count = 0;
        const fs::path problem_root = problem_root_path(problem);
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

std::vector<std::string> MainWindow::selected_contestants() const {
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

void MainWindow::reset_run_cells(const std::vector<std::string>& selected,
                                 const std::string& selected_problem) {
    active_judge_pairs_.clear();
    std::set<std::string> selected_set(selected.begin(), selected.end());
    for (const auto& contestant : contestants_) {
        if (!selected_set.empty() && selected_set.count(contestant) == 0) {
            continue;
        }
        for (const auto& problem : problems_) {
            if (!selected_problem.empty() && problem != selected_problem) {
                continue;
            }
            active_judge_pairs_.insert({contestant, problem});
            std::string key = cell_key(contestant, problem);
            score_cells_.erase(key);
            result_details_.erase(key);
            cell_texts_.erase(key);
            set_table_cell(contestant, problem, text("queued"));
        }
        update_total_cell(contestant);
    }
}

void MainWindow::set_table_cell(const std::string& contestant, const std::string& problem,
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

void MainWindow::update_total_cell(const std::string& contestant) {
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

void MainWindow::style_name_item(QTableWidgetItem* item) const {
    if (!item) {
        return;
    }
    if (theme_ == "cyber") {
        const QColor surface = blend_theme_colors(
            cyber_background_color_, cyber_primary_color_,
            cyber_secondary_color_, 0.08, 0.0);
        item->setForeground(neothemis::gui::ensure_theme_text_contrast(
            cyber_text_color_, surface));
        item->setBackground(surface);
        return;
    }
    item->setForeground(QColor("#e7fbff"));
    item->setBackground(QColor("#151b23"));
}

void MainWindow::style_problem_item(const std::string& contestant, const std::string& problem,
                                    QTableWidgetItem* item) const {
    if (!item) {
        return;
    }
    std::string key = cell_key(contestant, problem);
    QString terminal_status = terminal_status_for_cell(contestant, problem);
    auto apply_cyber_surface = [this, item](double primary_weight,
                                           double secondary_weight) {
        const QColor surface = blend_theme_colors(
            cyber_background_color_, cyber_primary_color_,
            cyber_secondary_color_, primary_weight, secondary_weight);
        item->setForeground(neothemis::gui::ensure_theme_text_contrast(
            cyber_text_color_, surface));
        item->setBackground(surface);
    };
    if (!terminal_status.isEmpty()) {
        if (theme_ == "cyber") {
            apply_cyber_surface(0.0, 0.22);
            return;
        }
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
            if (theme_ == "cyber") {
                apply_cyber_surface(0.18, 0.0);
                return;
            }
            item->setForeground(QColor("#7df9ff"));
            item->setBackground(QColor("#122631"));
        } else if (score.max > 0.0 && score.earned + 1e-9 >= score.max) {
            if (theme_ == "cyber") {
                apply_cyber_surface(0.24, 0.0);
                return;
            }
            item->setForeground(QColor("#99ffcc"));
            item->setBackground(QColor("#123028"));
        } else if (score.earned > 0.0) {
            if (theme_ == "cyber") {
                apply_cyber_surface(0.10, 0.10);
                return;
            }
            item->setForeground(QColor("#ffe680"));
            item->setBackground(QColor("#302512"));
        } else {
            if (theme_ == "cyber") {
                apply_cyber_surface(0.0, 0.18);
                return;
            }
            item->setForeground(QColor("#ff8fab"));
            item->setBackground(QColor("#30151f"));
        }
        return;
    }

    QString text = item->text().toLower();
    if (text.contains(this->text("queued").toLower()) || text.contains("queued")) {
        if (theme_ == "cyber") {
            apply_cyber_surface(0.10, 0.10);
            return;
        }
        item->setForeground(QColor("#ffe680"));
        item->setBackground(QColor("#2a2412"));
        return;
    }
    auto source_it = source_ready_.find(key);
    if (source_it != source_ready_.end() && source_it->second) {
        if (theme_ == "cyber") {
            apply_cyber_surface(0.16, 0.0);
            return;
        }
        item->setForeground(QColor("#7df9ff"));
        item->setBackground(QColor("#122631"));
    } else {
        if (theme_ == "cyber") {
            apply_cyber_surface(0.0, 0.16);
            return;
        }
        item->setForeground(QColor("#ff8fab"));
        item->setBackground(QColor("#281821"));
    }
}

void MainWindow::style_total_item(const std::string& contestant, QTableWidgetItem* item) const {
    if (!item) {
        return;
    }
    double earned = total_earned_for(contestant);
    double max = total_max_for(contestant);
    auto apply_cyber_surface = [this, item](double primary_weight,
                                           double secondary_weight) {
        const QColor surface = blend_theme_colors(
            cyber_background_color_, cyber_primary_color_,
            cyber_secondary_color_, primary_weight, secondary_weight);
        item->setForeground(neothemis::gui::ensure_theme_text_contrast(
            cyber_text_color_, surface));
        item->setBackground(surface);
    };
    if (max > 0.0 && earned + 1e-9 >= max) {
        if (theme_ == "cyber") {
            apply_cyber_surface(0.24, 0.0);
            return;
        }
        item->setForeground(QColor("#99ffcc"));
        item->setBackground(QColor("#102a24"));
    } else if (earned > 0.0) {
        if (theme_ == "cyber") {
            apply_cyber_surface(0.10, 0.10);
            return;
        }
        item->setForeground(QColor("#ffe680"));
        item->setBackground(QColor("#2c2312"));
    } else {
        if (theme_ == "cyber") {
            apply_cyber_surface(0.14, 0.0);
            return;
        }
        item->setForeground(QColor("#7df9ff"));
        item->setBackground(QColor("#121f2a"));
    }
}

void MainWindow::handle_result(const neothemis::TestResult& result) {
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

void MainWindow::show_result_details(int row, int col) {
    if (row < 0 || col <= 0 || static_cast<std::size_t>(row) >= contestants_.size() ||
        static_cast<std::size_t>(col - 1) >= problems_.size()) {
        return;
    }

    std::string contestant = contestants_[static_cast<std::size_t>(row)];
    std::string problem = problems_[static_cast<std::size_t>(col - 1)];
    std::string key = contestant + "\n" + problem;
    std::vector<neothemis::TestResult> rows = result_details_[key];
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.test < b.test; });

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
        this->text("maximum_test_time") + ": " + QString::number(maximum_time_ms) + " ms", dialog);
    auto* details = new QTableWidget(dialog);
    details->setColumnCount(4);
    details->setHorizontalHeaderLabels({this->text("test"), this->text("point"),
                                        this->text("run_time"), this->text("description")});
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
    QObject::connect(details, &QTableWidget::cellEntered, [details](int, int column) {
        details->viewport()->setCursor(column == 3 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    });
    QObject::connect(
        details, &QTableWidget::cellClicked, [this, dialog, details](int row, int column) {
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
            auto* close_buttons = new QDialogButtonBox(QDialogButtonBox::Close, description_dialog);
            QObject::connect(close_buttons, &QDialogButtonBox::rejected, description_dialog,
                             &QDialog::close);
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

void MainWindow::sort_by_column(int section) {
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
        return QString::fromStdString(a).toCaseFolded() < QString::fromStdString(b).toCaseFolded();
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

void MainWindow::show_header_menu(const QPoint& pos) {
    int section = table_->horizontalHeader()->logicalIndexAt(pos);
    if (section <= 0 || section > static_cast<int>(problems_.size())) {
        return;
    }
    std::string problem = problems_[static_cast<std::size_t>(section - 1)];
    QMenu menu(this);
    menu.setAttribute(Qt::WA_TranslucentBackground);
    menu.setWindowFlag(Qt::NoDropShadowWindowHint, true);
    menu.setWindowFlag(Qt::FramelessWindowHint, true);
    menu.addAction("Judge this problem for selected contestants",
                   [this, problem]() { start_judge(true, problem); });
    menu.addAction("Judge this problem for all contestants",
                   [this, problem]() { start_judge(false, problem); });
    menu.exec(table_->horizontalHeader()->mapToGlobal(pos));
}

void MainWindow::refresh_judge_detail_view() {
    if (detail_progress_label_) {
        detail_progress_label_->setText(detail_progress_text_.isEmpty() ? text("idle")
                                                                        : detail_progress_text_);
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

void MainWindow::show_judge_detail_view() {
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

void MainWindow::handle_progress_line(const std::string& raw) {
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

void MainWindow::start_judge(bool selected_only, const std::string& selected_problem) {
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
    neothemis::JudgeOptions options;
    try {
        options = options_from_ui();
        if (!selected_problem.empty()) {
            validate_path_component(selected_problem, "problem name");
            options.selected_problems.push_back(selected_problem);
        }
    } catch (const std::exception& ex) {
        QMessageBox::critical(this, text("open_failed"), ex.what());
        return;
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
                              : text("judging") + " " + QString::fromStdString(selected_problem) +
                                    " - " + scope);

    judge_thread_ = std::thread([this, options]() mutable {
        try {
            options.should_cancel = [this]() { return cancel_requested_.load(); };
            options.progress = [this](const std::string& line) {
                QMetaObject::invokeMethod(
                    this, [this, line]() { handle_progress_line(line); }, Qt::QueuedConnection);
            };
            options.result = [this](const neothemis::TestResult& result) {
                QMetaObject::invokeMethod(
                    this, [this, result]() { handle_result(result); }, Qt::QueuedConnection);
            };

            auto core = neothemis::make_judge_core(options.core_name);
            core->judge(options);
            QMetaObject::invokeMethod(
                this,
                [this]() {
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
                },
                Qt::QueuedConnection);
        } catch (const std::exception& ex) {
            bool cancelled = cancel_requested_.load();
            QMetaObject::invokeMethod(
                this,
                [this, message = QString::fromUtf8(ex.what()), cancelled]() {
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
                },
                Qt::QueuedConnection);
        }
    });
}

void MainWindow::set_judge_controls_enabled(bool enabled) {
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

void MainWindow::request_stop_judge() {
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

void MainWindow::stop_active_judge() {
    cancel_requested_.store(true);
    if (judge_thread_.joinable()) {
        judge_thread_.join();
    }
    finish_judge_elapsed();
    judging_.store(false);
}

QString MainWindow::format_judge_elapsed(qint64 elapsed_ms) {
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
    return QString("%1:%2.%3").arg(minutes).arg(seconds, 2, 10, QLatin1Char('0')).arg(tenths);
}

void MainWindow::update_judge_elapsed_label() {
    if (!judge_elapsed_label_) {
        return;
    }
    const qint64 elapsed = judge_elapsed_update_timer_ && judge_elapsed_update_timer_->isActive() &&
                                   judge_elapsed_clock_.isValid()
                               ? judge_elapsed_clock_.elapsed()
                               : last_judge_elapsed_ms_;
    judge_elapsed_label_->setText(text("elapsed_time") + ": " + format_judge_elapsed(elapsed));
}

void MainWindow::start_judge_elapsed() {
    last_judge_elapsed_ms_ = 0;
    judge_elapsed_clock_.restart();
    if (judge_elapsed_update_timer_) {
        judge_elapsed_update_timer_->start();
    }
    update_judge_elapsed_label();
}

void MainWindow::finish_judge_elapsed() {
    if (judge_elapsed_update_timer_ && judge_elapsed_update_timer_->isActive()) {
        if (judge_elapsed_clock_.isValid()) {
            last_judge_elapsed_ms_ = judge_elapsed_clock_.elapsed();
        }
        judge_elapsed_update_timer_->stop();
        update_judge_elapsed_label();
    }
}

} // namespace neothemis::gui
