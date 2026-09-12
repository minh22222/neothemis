#pragma once

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
#include "neothemis/Config.hpp"
#include "neothemis/Csv.hpp"
#include "neothemis/server/Database.hpp"

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
#include <QGraphicsOpacityEffect>
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
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>
#include <QShowEvent>
#include <QSpinBox>
#include <QSlider>
#include <QStackedLayout>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVariant>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWindow>

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

namespace neothemis::gui {

namespace fs = std::filesystem;

inline constexpr int kBackgroundBlurVisualStrength = 72;

struct CellScore {
    double earned = 0.0;
    double max = 0.0;
    int completed = 0;
};

using ServerUserSyncResult = neothemis::server::UserSyncResult;

class AnimatedTabBar final : public QTabBar {
public:
    explicit AnimatedTabBar(QWidget* parent = nullptr);

    void setAnimationsEnabled(bool enabled);

    void slideToIndex(int index);

    void setCenteredIcon(int index, const QIcon& icon);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void stopIndicatorAnimation();

    QRectF indicator_rect_;
    std::vector<QIcon> centered_icons_;
    QVariantAnimation* indicator_animation_ = nullptr;
    bool animations_enabled_ = false;
};

class SettingsTabWidget final : public QTabWidget {
public:
    explicit SettingsTabWidget(QWidget* parent = nullptr);
};

class SettingsDialog final : public QDialog {
public:
    explicit SettingsDialog(QWidget* parent) : QDialog(parent) {}

    ~SettingsDialog() override;

    void add_close_handler(std::function<void()> handler) {
        close_handlers_.push_back(std::move(handler));
    }

    void set_animated_tabs(QTabWidget* tabs);

    void set_tab_animations_enabled(bool enabled);

protected:
    void closeEvent(QCloseEvent* event) override {
        auto handlers = std::move(close_handlers_);
        for (const auto& handler : handlers) {
            handler();
        }
        QDialog::closeEvent(event);
    }

private:
    void stop_tab_animation();

    void animate_tab(int index);

    std::vector<std::function<void()>> close_handlers_;
    QTabWidget* animated_tabs_ = nullptr;
    QMetaObject::Connection tab_change_connection_;
    QPropertyAnimation* tab_animation_ = nullptr;
    QGraphicsOpacityEffect* tab_opacity_effect_ = nullptr;
    QWidget* animated_page_ = nullptr;
    bool tab_animations_enabled_ = false;
};

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(fs::path initial_contest);

    ~MainWindow() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

    void closeEvent(QCloseEvent* event) override;

    void showEvent(QShowEvent* event) override;

    bool nativeEvent(const QByteArray& event_type, void* message, qintptr* result) override;

private:
    QString text(const char* key) const;

    bool background_transparency_active() const;

    bool background_blur_active() const;

    neothemis::gui::CyberThemeColors cyber_theme_colors() const;

    static QColor color_setting_or_default(const QVariant& value, const QColor& fallback);

    void load_app_settings();

    void save_app_settings() const;

    void apply_language_to_main_window();

    void build_toolbar();

    void add_soft_shadow(QWidget* widget, qreal blur_radius, const QColor& color);

    void apply_translucent_surface_attributes();

    void apply_selected_theme();

    bool apply_native_backdrop(bool force_compositor_update);

    void schedule_backdrop_startup_passes();

    QString generated_server_secret(int chars) const;

    QString local_server_executable() const;

    fs::path server_data_dir() const;

    fs::path server_database_path() const;

    QString server_database_display_path() const;

    bool valid_server_username(const QString& username) const;

    std::unique_ptr<neothemis::server::Database> open_server_database(bool create_if_missing) const;

    void create_server_user(const QString& username, const QString& password, const QString& role);

    void change_server_user_password(const QString& username, const QString& password);

    void remove_server_user(const QString& username);

    ServerUserSyncResult sync_server_users_from_contest();

    std::pair<int, int> import_server_users_csv(const fs::path& path);

    fs::file_time_type safe_last_write_time(const fs::path& path) const;

    fs::path server_results_path() const;

    fs::file_time_type server_database_activity_time() const;

    void initialize_server_auto_refresh_state();

    void start_server_auto_refresh();

    void stop_server_auto_refresh();

    void poll_server_auto_refresh();

    void update_server_actions();

    void append_server_output(const QByteArray& output);

    void open_start_server_dialog();

    void start_local_server(int port, bool allow_lan, const QString& join_code,
                            const QString& admin_password, const QString& tls_certificate,
                            const QString& tls_private_key);

    void stop_local_server(bool log_message);

    neothemis::JudgeOptions options_from_ui() const;

    fs::path validated_contest_path(const fs::path& path, const char* name) const;

    fs::path validated_child_path(const fs::path& root, const fs::path& path,
                                  const char* name) const;

    fs::path contestants_root_path() const;

    fs::path problem_root_path(const std::string& problem) const;

    bool archive_operation_available();

    void join_archive_thread();

    void set_archive_controls_enabled(bool enabled);

    void begin_archive_operation(const QString& label);

    void update_archive_progress(std::uint64_t done, std::uint64_t total,
                                 const std::string& label_key);

    ArchiveProgress archive_progress_callback();

    void finish_archive_operation(const QString& label);

    void fail_archive_operation(const QString& label);

    neothemis::ContestConfig current_contest_config() const;

    void apply_contest_config(const neothemis::ContestConfig& config);

    void reset_contest_config_defaults();

    fs::path ensure_ncontest_extension(fs::path path) const;

    bool confirm_discard_unsaved_file(bool close_after_save = false);

    void mark_contest_dirty();

    void cleanup_temp_root(const fs::path& root);

    void cleanup_temporary_contests();

    bool prepare_to_replace_contest();

    fs::path make_temporary_contest_root();

    fs::path detect_extracted_contest_root(const fs::path& root) const;

    void open_contest();

    void open_contest_file();

    void open_contest_file(const fs::path& archive_path);

    void load_contest_config();

    void save_contest_config() const;

    bool save_contest_container(bool save_as);

    void convert_old_contest_to_ncontest(bool source_is_folder);

    void add_contestants_from_folder();

    void refresh_table(bool log_opened = true);

    fs::path contest_output_path(const fs::path& path) const;

    void record_result(const neothemis::TestResult& result);

    std::vector<neothemis::TestResult> all_recorded_results() const;

    void load_existing_results();

    void write_current_csv_outputs();

    std::string status_for_scoreboard_cell(const std::string& contestant,
                                           const std::string& problem) const;

    QString terminal_status_for_cell(const std::string& contestant,
                                     const std::string& problem) const;

    fs::path choose_export_path(const std::string& filename);

    bool export_is_available();

    void export_scoreboard_xlsx();

    void export_data_xlsx();

    void rebuild_maps();

    int total_column() const;

    std::string cell_key(const std::string& contestant, const std::string& problem) const;

    double earned_for_problem(const std::string& contestant, const std::string& problem) const;

    double max_for_problem(const std::string& contestant, const std::string& problem) const;

    double total_earned_for(const std::string& contestant) const;

    double total_max_for(const std::string& contestant) const;

    QString score_cell_text(const CellScore& score, int expected) const;

    QString problem_cell_text(const std::string& contestant, const std::string& problem) const;

    void populate_table();

    void load_problem_test_counts();

    std::vector<std::string> selected_contestants() const;

    void reset_run_cells(const std::vector<std::string>& selected,
                         const std::string& selected_problem);

    void set_table_cell(const std::string& contestant, const std::string& problem,
                        const QString& text);

    void update_total_cell(const std::string& contestant);

    void style_name_item(QTableWidgetItem* item) const;

    void style_problem_item(const std::string& contestant, const std::string& problem,
                            QTableWidgetItem* item) const;

    void style_total_item(const std::string& contestant, QTableWidgetItem* item) const;

    void handle_result(const neothemis::TestResult& result);

    void show_result_details(int row, int col);

    void sort_by_column(int section);

    void show_header_menu(const QPoint& pos);

    void refresh_judge_detail_view();

    void show_judge_detail_view();

    void handle_progress_line(const std::string& raw);

    void start_judge(bool selected_only, const std::string& selected_problem = {});

    void set_judge_controls_enabled(bool enabled);

    void request_stop_judge();

    void stop_active_judge();

    static QString format_judge_elapsed(qint64 elapsed_ms);

    void update_judge_elapsed_label();

    void start_judge_elapsed();

    void finish_judge_elapsed();

    QWidget* build_visual_tab(QWidget* parent, SettingsDialog* settings_dialog);

    QWidget* build_contest_tab(QWidget* parent, SettingsDialog* settings_dialog);

    QWidget* build_problem_tab(QWidget* parent, SettingsDialog* settings_dialog);

    QWidget* build_server_settings_tab(QWidget* parent, SettingsDialog* settings_dialog);

    void refresh_server_users_table(QTableWidget* users_table);

    QWidget* build_server_users_tab(QWidget* parent);

    void open_settings_dialog(int initial_tab);

    void show_about_dialog();

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
    std::set<std::pair<std::string, std::string>> active_judge_pairs_;
    int sort_column_ = 0;
    bool sort_ascending_ = true;

    std::string compiler_ = "g++";
    std::string compile_flags_ = "-std=c++14 -O2 -pipe";
    std::string contestants_dir_ = "contestants";
    std::string tests_dir_ = "tests";
    std::string core_name_ = "builtin";
    fs::path output_csv_ = "results.csv";
    fs::path scoreboard_csv_ = "scoreboard.csv";
    std::vector<std::string> forbidden_patterns_ = neothemis::default_forbidden_patterns();
    std::string language_ = "en";
    std::string theme_ = "dark";
    QColor cyber_background_color_ =
        neothemis::gui::default_cyber_theme_colors().background;
    QColor cyber_primary_color_ = neothemis::gui::default_cyber_theme_colors().primary;
    QColor cyber_secondary_color_ = neothemis::gui::default_cyber_theme_colors().secondary;
    QColor cyber_text_color_ = neothemis::gui::default_cyber_theme_colors().text;
    QColor cyber_muted_text_color_ =
        neothemis::gui::default_cyber_theme_colors().muted_text;
    QColor cyber_primary_text_color_ =
        neothemis::gui::default_cyber_theme_colors().primary_text;
    QColor cyber_secondary_text_color_ =
        neothemis::gui::default_cyber_theme_colors().secondary_text;
    bool transparent_background_ = false;
    int background_transparency_ = 20;
    bool blur_background_ = false;
    bool animations_enabled_ = true;
    bool backdrop_refresh_queued_ = false;
    bool sandbox_enabled_ = true;
    fs::path temporary_dir_;
    int server_port_ = 8080;
    bool server_allow_lan_ = false;
    bool server_secure_password_storage_ = false;
    bool server_ranking_enabled_ = false;
    bool server_contestant_details_enabled_ = false;
    neothemis::ContestConfig loaded_contest_config_;
    QString server_join_code_;
    QString server_admin_password_;
    QString server_tls_certificate_;
    QString server_tls_private_key_;
    fs::path contest_file_path_;
    fs::path active_temp_root_;
    std::vector<fs::path> temporary_roots_;
    std::uint64_t stack_limit_mb_ = 64;
    unsigned int parallel_jobs_ = 0;
    unsigned int compile_jobs_ = 0;
    unsigned int test_jobs_ = 0;
    bool timing_focused_ = false;
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

} // namespace neothemis::gui
