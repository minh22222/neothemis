#pragma once

#include <QString>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

class QSqlDatabase;
class QSqlQuery;

namespace neothemis::server {

struct User {
    int id = 0;
    QString username;
    QString role;
    QString csrf;
};

struct ManagedUser {
    QString username;
    QString password;
    QString role;
    std::int64_t created_at = 0;
};

struct UserSyncResult {
    int created = 0;
    int existing = 0;
    int removed = 0;
    int skipped = 0;
};

enum class RemoveUserResult {
    Removed,
    NotFound,
    LastAdmin,
};

struct SubmissionSummary {
    int id = 0;
    int user_id = 0;
    QString username;
    QString problem;
    QString source_path;
    QString status;
    QString verdict;
    double score = 0.0;
    QString message;
    std::int64_t submitted_at = 0;
    std::int64_t judged_at = 0;
    bool ignored = false;
};

struct TestRow {
    QString test;
    QString verdict;
    std::uint64_t time_ms = 0;
    int exit_code = 0;
    double max_points = 0.0;
    double earned_points = 0.0;
    QString message;
};

struct SubmissionAdmissionLimits {
    int max_active_per_user = 3;
    int max_active_global = 64;
    int max_recent_per_user = 10;
    std::int64_t recent_window_seconds = 60;
};

enum class SubmissionAdmissionStatus {
    Accepted,
    UserActiveLimit,
    GlobalActiveLimit,
    RateLimit,
};

struct SubmissionAdmissionResult {
    SubmissionAdmissionStatus status = SubmissionAdmissionStatus::Accepted;
    int submission_id = 0;

    [[nodiscard]] bool accepted() const noexcept {
        return status == SubmissionAdmissionStatus::Accepted;
    }
};

inline constexpr int kRetainedTerminalSourceSnapshotsPerUser = 20;

struct SourceSnapshot {
    int submission_id = 0;
    int user_id = 0;
    QString source_path;
};

struct ResultPair {
    QString username;
    QString problem;
    std::uint64_t generation = 0;
};

struct AuthoritativeResultPair {
    QString username;
    QString problem;
    std::uint64_t generation = 0;
    std::vector<TestRow> rows;
};

class Database {
public:
    explicit Database(std::filesystem::path path, bool secure_password_storage = false);
    ~Database();

    void migrate_schema();
    void recover_interrupted_submissions();

    [[nodiscard]] bool secure_password_storage_enabled() const noexcept {
        return secure_password_storage_;
    }

    bool has_admin();
    void create_user(const QString& username, const QString& password, const QString& role);
    std::vector<ManagedUser> list_users();
    bool change_user_password(const QString& username, const QString& password);
    RemoveUserResult remove_user(const QString& username);
    UserSyncResult sync_contestant_users(const std::set<QString>& usernames,
                                         const QString& default_password);
    std::optional<User> authenticate(const QString& username, const QString& password);

    QString create_session(int user_id);
    void delete_session(const QString& token);
    std::optional<User> user_for_session(const QString& token);

    SubmissionAdmissionResult create_submission_draft(
        int user_id, const QString& username, const QString& problem,
        const SubmissionAdmissionLimits& limits = {});
    void queue_submission(int id, const QString& source_path);
    std::optional<SubmissionSummary> take_next_queued();
    void recover_running_submission(int id);
    void finish_submission(int id, const QString& status, const QString& verdict, double score,
                           const QString& message, const std::vector<TestRow>& rows);
    std::vector<SubmissionSummary> submissions_for_user(int user_id, bool admin);
    std::vector<TestRow> test_results(int submission_id, int user_id, bool admin);
    std::optional<int> submission_owner_id(int submission_id);
    std::vector<TestRow> test_results_for_submission(int submission_id);
    std::optional<SubmissionSummary> ignore_submission(int id);
    std::optional<SubmissionSummary> latest_effective_submission(const QString& username,
                                                                 const QString& problem);
    std::vector<AuthoritativeResultPair> authoritative_result_pairs();
    std::vector<ResultPair> pending_result_pairs();
    std::optional<std::uint64_t> pending_result_generation(const QString& username,
                                                           const QString& problem);
    bool acknowledge_result_pair(const QString& username, const QString& problem,
                                 std::uint64_t generation);
    std::vector<QString> contestant_usernames();
    std::vector<SourceSnapshot> terminal_source_snapshots_to_prune(
        int retain_per_user, std::optional<int> user_id = std::nullopt);
    bool clear_terminal_source_snapshot(int submission_id, const QString& expected_source_path);
    std::set<int> source_snapshot_submission_ids();

private:
    QSqlDatabase connection();

    static void exec(QSqlDatabase& database, const QString& sql);
    static void ensure_column(QSqlDatabase& database, const QString& table, const QString& column,
                              const QString& definition);
    static void check(bool ok, const QSqlQuery& query);
    static SubmissionSummary row_to_submission(const QSqlQuery& query);
    static void enqueue_result_pair(QSqlDatabase& database, const QString& username,
                                    const QString& problem);

    std::vector<TestRow> test_results_for_submission_locked(int submission_id);

    std::filesystem::path path_;
    std::uint64_t instance_id_ = 0;
    bool secure_password_storage_ = false;
    std::mutex mutex_;
};

} // namespace neothemis::server
