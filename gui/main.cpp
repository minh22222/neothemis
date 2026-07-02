#include "neothemis/JudgeCore.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

namespace fs = std::filesystem;

namespace {

class MainWindow : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle("NeoThemis");
        resize(1180, 760);

        auto* central = new QWidget(this);
        auto* root = new QHBoxLayout(central);

        table_ = new QTableWidget(central);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        table_->verticalHeader()->setVisible(false);
        root->addWidget(table_, 1);

        auto* side = new QWidget(central);
        auto* side_layout = new QVBoxLayout(side);
        side->setFixedWidth(360);

        auto* open = new QPushButton("Open Contest", side);
        auto* judge = new QPushButton("Judge Selected", side);
        auto* judge_all = new QPushButton("Judge All", side);
        side_layout->addWidget(open);
        side_layout->addWidget(judge);
        side_layout->addWidget(judge_all);

        auto* settings = new QGroupBox("Judge Settings", side);
        auto* form = new QFormLayout(settings);
        compiler_ = new QLineEdit("g++", settings);
        flags_ = new QLineEdit("-std=c++17 -O2 -pipe", settings);
        testlib_ = new QLineEdit("testlib", settings);
        contestants_dir_ = new QLineEdit("contestants", settings);
        tests_dir_ = new QLineEdit("tests", settings);
        parallel_ = new QSpinBox(settings);
        parallel_->setRange(0, 256);
        parallel_->setValue(0);
        keep_workdir_ = new QCheckBox(settings);
        form->addRow("Compiler", compiler_);
        form->addRow("Compile flags", flags_);
        form->addRow("testlib dir", testlib_);
        form->addRow("Contestants dir", contestants_dir_);
        form->addRow("Tests dir", tests_dir_);
        form->addRow("Parallel jobs", parallel_);
        form->addRow("Keep workdir", keep_workdir_);
        side_layout->addWidget(settings);

        auto* problem_settings = new QGroupBox("Problem Defaults", side);
        auto* problem_form = new QFormLayout(problem_settings);
        time_ms_ = new QSpinBox(problem_settings);
        time_ms_->setRange(1, 60 * 60 * 1000);
        time_ms_->setValue(1000);
        memory_mb_ = new QSpinBox(problem_settings);
        memory_mb_->setRange(0, 1024 * 1024);
        memory_mb_->setValue(256);
        stack_mb_ = new QSpinBox(problem_settings);
        stack_mb_->setRange(0, 1024 * 1024);
        stack_mb_->setValue(64);
        default_points_ = new QLineEdit("1", problem_settings);
        checker_ = new QLineEdit("token", problem_settings);
        auto* apply_problem = new QPushButton("Apply To All Problems", problem_settings);
        problem_form->addRow("Time limit ms", time_ms_);
        problem_form->addRow("Memory MB", memory_mb_);
        problem_form->addRow("Stack MB", stack_mb_);
        problem_form->addRow("Default points", default_points_);
        problem_form->addRow("Checker", checker_);
        problem_form->addRow(apply_problem);
        side_layout->addWidget(problem_settings);

        progress_ = new QProgressBar(side);
        progress_->setRange(0, 0);
        progress_->setVisible(false);
        log_ = new QPlainTextEdit(side);
        log_->setReadOnly(true);
        side_layout->addWidget(progress_);
        side_layout->addWidget(log_, 1);

        setCentralWidget(central);
        apply_theme();

        QObject::connect(open, &QPushButton::clicked, [this]() { open_contest(); });
        QObject::connect(judge, &QPushButton::clicked, [this]() { start_judge(true); });
        QObject::connect(judge_all, &QPushButton::clicked, [this]() { start_judge(false); });
        QObject::connect(apply_problem, &QPushButton::clicked, [this]() { apply_problem_defaults(); });
    }

private:
    void apply_theme() {
        qApp->setStyleSheet(R"(
            QWidget { background: #f7f8fb; color: #1f2937; font-size: 13px; }
            QTableWidget, QPlainTextEdit, QLineEdit, QSpinBox {
                background: #ffffff; border: 1px solid #d7dce5; border-radius: 6px; padding: 4px;
            }
            QHeaderView::section { background: #eef2f7; border: 0; padding: 7px; font-weight: 600; }
            QPushButton {
                background: #2563eb; color: white; border: 0; border-radius: 6px; padding: 8px 10px;
                font-weight: 600;
            }
            QPushButton:hover { background: #1d4ed8; }
            QGroupBox {
                border: 1px solid #d7dce5; border-radius: 8px; margin-top: 10px; padding-top: 12px;
                background: #ffffff;
            }
            QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }
        )");
    }

    neothemis::JudgeOptions options_from_ui() const {
        neothemis::JudgeOptions options;
        options.contest_root = contest_root_;
        options.compiler = compiler_->text().toStdString();
        options.compile_flags = flags_->text().toStdString();
        options.testlib_dir = testlib_->text().toStdString();
        options.contestants_dir = contestants_dir_->text().toStdString();
        options.tests_dir = tests_dir_->text().toStdString();
        options.parallel_jobs = static_cast<unsigned int>(parallel_->value());
        options.keep_workdir = keep_workdir_->isChecked();
        options.forbidden_patterns = {
            "system(", "popen(", "fork(", "exec(", "#include <unistd.h>",
            "#include <sys/", "#include <windows.h>"
        };
        return options;
    }

    void open_contest() {
        QString dir = QFileDialog::getExistingDirectory(this, "Open contest");
        if (dir.isEmpty()) {
            return;
        }
        contest_root_ = dir.toStdString();
        refresh_table();
    }

    void refresh_table() {
        try {
            auto overview = neothemis::inspect_contest(options_from_ui());
            contestants_ = overview.contestants;
            table_->clear();
            table_->setRowCount(static_cast<int>(overview.contestants.size()));
            table_->setColumnCount(static_cast<int>(overview.problems.size() + 1));
            table_->setHorizontalHeaderItem(0, new QTableWidgetItem("Contestant"));
            for (std::size_t col = 0; col < overview.problems.size(); ++col) {
                table_->setHorizontalHeaderItem(static_cast<int>(col + 1),
                                                new QTableWidgetItem(QString::fromStdString(overview.problems[col])));
            }
            for (std::size_t row = 0; row < overview.contestants.size(); ++row) {
                table_->setItem(static_cast<int>(row), 0,
                                new QTableWidgetItem(QString::fromStdString(overview.contestants[row])));
                for (std::size_t col = 0; col < overview.problems.size(); ++col) {
                    bool has = overview.has_source[row][col];
                    auto* item = new QTableWidgetItem(has ? "Ready" : "Missing");
                    item->setTextAlignment(Qt::AlignCenter);
                    table_->setItem(static_cast<int>(row), static_cast<int>(col + 1), item);
                }
            }
            log_->appendPlainText("Opened " + QString::fromStdString(contest_root_.string()));
        } catch (const std::exception& ex) {
            QMessageBox::critical(this, "Open failed", ex.what());
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

    void apply_problem_defaults() {
        if (contest_root_.empty()) {
            return;
        }
        fs::path tests_root = contest_root_ / tests_dir_->text().toStdString();
        for (const auto& entry : fs::directory_iterator(tests_root)) {
            if (!entry.is_directory()) {
                continue;
            }
            fs::path conf = entry.path() / "problem.conf";
            std::ofstream out(conf);
            out << "time_limit_ms=" << time_ms_->value() << '\n'
                << "memory_limit_mb=" << memory_mb_->value() << '\n'
                << "stack_limit_mb=" << stack_mb_->value() << '\n'
                << "default_points=" << default_points_->text().toStdString() << '\n'
                << "checker=" << checker_->text().toStdString() << '\n';
        }
        log_->appendPlainText("Updated problem defaults.");
    }

    void start_judge(bool selected_only) {
        if (contest_root_.empty()) {
            return;
        }
        neothemis::JudgeOptions options = options_from_ui();
        if (selected_only) {
            options.selected_contestants = selected_contestants();
            if (options.selected_contestants.empty()) {
                QMessageBox::information(this, "No selection", "Select one or more contestant rows.");
                return;
            }
        }
        progress_->setVisible(true);
        log_->appendPlainText("Judging...");

        std::thread([this, options]() mutable {
            try {
                options.progress = [this](const std::string& line) {
                    QMetaObject::invokeMethod(log_, [this, line]() {
                        log_->appendPlainText(QString::fromStdString(line));
                    }, Qt::QueuedConnection);
                };
                auto core = neothemis::make_judge_core(options.core_name);
                std::vector<neothemis::TestResult> results = core->judge(options);
                std::ofstream details(options.contest_root / options.output_csv);
                neothemis::write_csv(details, results);
                std::ofstream scoreboard(options.contest_root / options.scoreboard_csv);
                neothemis::write_scoreboard_csv(scoreboard, results);
                QMetaObject::invokeMethod(this, [this]() {
                    progress_->setVisible(false);
                    log_->appendPlainText("Done.");
                }, Qt::QueuedConnection);
            } catch (const std::exception& ex) {
                QMetaObject::invokeMethod(this, [this, message = QString::fromUtf8(ex.what())]() {
                    progress_->setVisible(false);
                    QMessageBox::critical(this, "Judge failed", message);
                }, Qt::QueuedConnection);
            }
        }).detach();
    }

    fs::path contest_root_;
    std::vector<std::string> contestants_;
    QTableWidget* table_ = nullptr;
    QLineEdit* compiler_ = nullptr;
    QLineEdit* flags_ = nullptr;
    QLineEdit* testlib_ = nullptr;
    QLineEdit* contestants_dir_ = nullptr;
    QLineEdit* tests_dir_ = nullptr;
    QSpinBox* parallel_ = nullptr;
    QCheckBox* keep_workdir_ = nullptr;
    QSpinBox* time_ms_ = nullptr;
    QSpinBox* memory_mb_ = nullptr;
    QSpinBox* stack_mb_ = nullptr;
    QLineEdit* default_points_ = nullptr;
    QLineEdit* checker_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
};

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    MainWindow window;
    window.show();
    return app.exec();
}
