#include "neothemis/server/Database.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QRandomGenerator>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <atomic>
#include <set>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;

namespace neothemis::server {
namespace {

constexpr std::int64_t kSessionLifetimeSeconds = 12 * 60 * 60;
constexpr int kDatabaseBusyTimeoutMs = 5000;
std::atomic<std::uint64_t> next_database_instance{1};
std::atomic<std::uint64_t> next_database_thread{1};
std::mutex sql_connection_registry_mutex;

void remove_sql_connection(const QString& name) {
    std::lock_guard<std::mutex> registry_lock(sql_connection_registry_mutex);
    if (!QSqlDatabase::contains(name)) {
        return;
    }
    {
        QSqlDatabase database = QSqlDatabase::database(name, false);
        if (database.isValid()) {
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(name);
}

struct ThreadConnectionRegistry {
    std::uint64_t token = next_database_thread.fetch_add(1, std::memory_order_relaxed);
    std::set<QString> names;

    ~ThreadConnectionRegistry() {
        for (const QString& name : names) {
            remove_sql_connection(name);
        }
    }
};

thread_local ThreadConnectionRegistry thread_connections;

QString database_path_string(const fs::path& path) {
#ifdef _WIN32
    return QString::fromStdWString(path.wstring());
#else
    const std::string native = path.string();
    return QString::fromLocal8Bit(native.data(), static_cast<qsizetype>(native.size()));
#endif
}

QString random_token(std::size_t bytes) {
    QByteArray data;
    data.resize(static_cast<qsizetype>(bytes));
    for (std::size_t i = 0; i < bytes; ++i) {
        data[static_cast<qsizetype>(i)] =
            static_cast<char>(QRandomGenerator::system()->bounded(256));
    }
    return data.toHex();
}

QString legacy_password_hash(const QString& salt, const QString& password) {
    QByteArray material = salt.toUtf8();
    material += ':';
    material += password.toUtf8();
    QByteArray digest = material;
    for (int i = 0; i < 60000; ++i) {
        digest += salt.toUtf8();
        digest = QCryptographicHash::hash(digest, QCryptographicHash::Sha256);
    }
    return digest.toHex();
}

} // namespace

Database::Database(fs::path path)
    : path_(std::move(path)),
      instance_id_(next_database_instance.fetch_add(1, std::memory_order_relaxed)) {}

Database::~Database() {
    std::lock_guard<std::mutex> lock(mutex_);
    const QString name = QString("neothemis_server_%1_%2")
                             .arg(instance_id_)
                             .arg(thread_connections.token);
    remove_sql_connection(name);
    thread_connections.names.erase(name);
}

void Database::migrate_schema() {
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlDatabase database = connection();
    exec(database, "PRAGMA journal_mode=WAL");
    exec(database, "CREATE TABLE IF NOT EXISTS users ("
                   "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                   "username TEXT UNIQUE NOT NULL,"
                   "salt TEXT NOT NULL,"
                   "hash TEXT NOT NULL,"
                   "password TEXT NOT NULL DEFAULT '',"
                   "role TEXT NOT NULL,"
                   "created_at INTEGER NOT NULL)");
    exec(database, "CREATE TABLE IF NOT EXISTS sessions ("
                   "token TEXT PRIMARY KEY,"
                   "user_id INTEGER NOT NULL,"
                   "csrf TEXT NOT NULL,"
                   "expires_at INTEGER NOT NULL,"
                   "FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE)");
    exec(database, "CREATE TABLE IF NOT EXISTS submissions ("
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
    exec(database, "CREATE TABLE IF NOT EXISTS test_results ("
                   "submission_id INTEGER NOT NULL,"
                   "test TEXT NOT NULL,"
                   "verdict TEXT NOT NULL,"
                   "time_ms INTEGER NOT NULL,"
                   "exit_code INTEGER NOT NULL DEFAULT 0,"
                   "max_points REAL NOT NULL,"
                   "earned_points REAL NOT NULL,"
                   "message TEXT NOT NULL,"
                   "FOREIGN KEY(submission_id) REFERENCES submissions(id) ON DELETE CASCADE)");
    exec(database, "CREATE TABLE IF NOT EXISTS ignored_submissions ("
                   "submission_id INTEGER PRIMARY KEY,"
                   "ignored_at INTEGER NOT NULL,"
                   "FOREIGN KEY(submission_id) REFERENCES submissions(id) ON DELETE CASCADE)");
    // This transactional outbox has no user foreign key deliberately. Account
    // deletion can cascade every submission while the surviving pair still
    // requests removal of its stale CSV result.
    exec(database, "CREATE TABLE IF NOT EXISTS server_result_pairs ("
                   "username TEXT NOT NULL,"
                   "problem TEXT NOT NULL,"
                   "generation INTEGER NOT NULL DEFAULT 1,"
                   "pending INTEGER NOT NULL DEFAULT 1,"
                   "PRIMARY KEY(username, problem))");
    ensure_column(database, "server_result_pairs", "generation", "INTEGER NOT NULL DEFAULT 1");
    ensure_column(database, "server_result_pairs", "pending", "INTEGER NOT NULL DEFAULT 1");
    exec(database, "INSERT OR IGNORE INTO server_result_pairs(username, problem, generation) "
                   "SELECT DISTINCT username, problem, 1 FROM submissions");
    exec(database, "CREATE INDEX IF NOT EXISTS submissions_user_status_idx "
                   "ON submissions(user_id, status)");
    exec(database, "CREATE INDEX IF NOT EXISTS submissions_status_idx "
                   "ON submissions(status)");
    exec(database, "CREATE INDEX IF NOT EXISTS submissions_user_submitted_idx "
                   "ON submissions(user_id, submitted_at)");
    exec(database, "CREATE INDEX IF NOT EXISTS submissions_user_id_idx "
                   "ON submissions(user_id, id)");
    exec(database, "CREATE INDEX IF NOT EXISTS submissions_result_pair_idx "
                   "ON submissions(status, username, problem, id DESC)");
    exec(database, "CREATE INDEX IF NOT EXISTS test_results_submission_idx "
                   "ON test_results(submission_id)");
    ensure_column(database, "test_results", "exit_code", "INTEGER NOT NULL DEFAULT 0");
    ensure_column(database, "users", "password", "TEXT NOT NULL DEFAULT ''");
}

void Database::recover_interrupted_submissions() {
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlDatabase database = connection();
    exec(database, "BEGIN IMMEDIATE");
    try {
        QSqlQuery affected(database);
        check(affected.exec(
                  "SELECT DISTINCT username, problem FROM submissions "
                  "WHERE status IN ('staging','running') OR ("
                  "status='queued' AND EXISTS (SELECT 1 FROM ignored_submissions "
                  "WHERE ignored_submissions.submission_id=submissions.id))"),
              affected);
        std::vector<std::pair<QString, QString>> affected_pairs;
        while (affected.next()) {
            affected_pairs.emplace_back(affected.value(0).toString(),
                                        affected.value(1).toString());
        }
        affected.finish();
        for (const auto& [username, problem] : affected_pairs) {
            enqueue_result_pair(database, username, problem);
        }

        exec(database,
             "UPDATE submissions SET status='failed', verdict='IGN', score=0, "
             "message='Ignored by administrator.', judged_at=strftime('%s','now') "
             "WHERE status IN ('staging','queued','running') AND EXISTS ("
             "SELECT 1 FROM ignored_submissions "
             "WHERE ignored_submissions.submission_id=submissions.id)");
        exec(database,
             "UPDATE submissions SET status='queued' WHERE status='running' AND NOT EXISTS ("
             "SELECT 1 FROM ignored_submissions "
             "WHERE ignored_submissions.submission_id=submissions.id)");
        exec(database,
             "UPDATE submissions SET status='failed', verdict='IE', "
             "message='Submission source was not completely stored.', "
             "judged_at=strftime('%s','now') "
             "WHERE status='staging' AND NOT EXISTS ("
             "SELECT 1 FROM ignored_submissions "
             "WHERE ignored_submissions.submission_id=submissions.id)");
        exec(database, "COMMIT");
    } catch (...) {
        QSqlQuery rollback(database);
        rollback.exec("ROLLBACK");
        throw;
    }
}

bool Database::has_admin() {
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlQuery query(connection());
    check(query.exec("SELECT 1 FROM users WHERE role='admin' LIMIT 1"), query);
    return query.next();
}

void Database::create_user(const QString& username, const QString& password, const QString& role) {
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlQuery query(connection());
    query.prepare("INSERT INTO users(username, salt, hash, password, role, created_at) "
                  "VALUES(?, '', '', ?, ?, ?)");
    query.addBindValue(username);
    query.addBindValue(password);
    query.addBindValue(role);
    query.addBindValue(QDateTime::currentSecsSinceEpoch());
    check(query.exec(), query);
}

std::vector<ManagedUser> Database::list_users() {
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlQuery query(connection());
    check(query.exec("SELECT username, password, role, created_at FROM users "
                     "ORDER BY CASE role WHEN 'admin' THEN 0 ELSE 1 END, username"),
          query);
    std::vector<ManagedUser> users;
    while (query.next()) {
        users.push_back(ManagedUser{query.value(0).toString(), query.value(1).toString(),
                                    query.value(2).toString(), query.value(3).toLongLong()});
    }
    return users;
}

bool Database::change_user_password(const QString& username, const QString& password) {
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlDatabase database = connection();
    exec(database, "BEGIN IMMEDIATE");
    try {
        QSqlQuery update(database);
        update.prepare("UPDATE users SET password=?, salt='', hash='' WHERE username=?");
        update.addBindValue(password);
        update.addBindValue(username);
        check(update.exec(), update);
        if (update.numRowsAffected() != 1) {
            exec(database, "COMMIT");
            return false;
        }

        QSqlQuery sessions(database);
        sessions.prepare(
            "DELETE FROM sessions WHERE user_id=(SELECT id FROM users WHERE username=?)");
        sessions.addBindValue(username);
        check(sessions.exec(), sessions);
        exec(database, "COMMIT");
        return true;
    } catch (...) {
        QSqlQuery rollback(database);
        rollback.exec("ROLLBACK");
        throw;
    }
}

RemoveUserResult Database::remove_user(const QString& username) {
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlDatabase database = connection();
    exec(database, "BEGIN IMMEDIATE");
    try {
        QSqlQuery user(database);
        user.prepare("SELECT role FROM users WHERE username=?");
        user.addBindValue(username);
        check(user.exec(), user);
        if (!user.next()) {
            exec(database, "COMMIT");
            return RemoveUserResult::NotFound;
        }
        const bool admin = user.value(0).toString() == "admin";
        user.finish();

        if (admin) {
            QSqlQuery admins(database);
            check(admins.exec("SELECT COUNT(*) FROM users WHERE role='admin'"), admins);
            if (!admins.next()) {
                throw std::runtime_error("admin count query returned no row");
            }
            if (admins.value(0).toInt() <= 1) {
                exec(database, "COMMIT");
                return RemoveUserResult::LastAdmin;
            }
        }

        QSqlQuery result_pairs(database);
        result_pairs.prepare(
            "SELECT DISTINCT username, problem FROM submissions WHERE user_id=("
            "SELECT id FROM users WHERE username=?)");
        result_pairs.addBindValue(username);
        check(result_pairs.exec(), result_pairs);
        std::vector<std::pair<QString, QString>> pairs_to_remove;
        while (result_pairs.next()) {
            pairs_to_remove.emplace_back(result_pairs.value(0).toString(),
                                         result_pairs.value(1).toString());
        }
        result_pairs.finish();
        for (const auto& [pair_username, problem] : pairs_to_remove) {
            enqueue_result_pair(database, pair_username, problem);
        }

        QSqlQuery remove(database);
        remove.prepare("DELETE FROM users WHERE username=?");
        remove.addBindValue(username);
        check(remove.exec(), remove);
        if (remove.numRowsAffected() != 1) {
            throw std::runtime_error("user disappeared while being removed");
        }
        exec(database, "COMMIT");
        return RemoveUserResult::Removed;
    } catch (...) {
        QSqlQuery rollback(database);
        rollback.exec("ROLLBACK");
        throw;
    }
}

UserSyncResult Database::sync_contestant_users(const std::set<QString>& usernames,
                                               const QString& default_password) {
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlDatabase database = connection();
    exec(database, "BEGIN IMMEDIATE");
    try {
        UserSyncResult result;
        for (const QString& username : usernames) {
            QSqlQuery exists(database);
            exists.prepare("SELECT 1 FROM users WHERE username=?");
            exists.addBindValue(username);
            check(exists.exec(), exists);
            if (exists.next()) {
                ++result.existing;
                continue;
            }

            QSqlQuery insert(database);
            insert.prepare("INSERT INTO users(username, salt, hash, password, role, created_at) "
                           "VALUES(?, '', '', ?, 'contestant', ?)");
            insert.addBindValue(username);
            insert.addBindValue(default_password);
            insert.addBindValue(QDateTime::currentSecsSinceEpoch());
            check(insert.exec(), insert);
            ++result.created;
        }

        std::vector<QString> stale_users;
        QSqlQuery users(database);
        check(users.exec("SELECT username FROM users WHERE role='contestant'"), users);
        while (users.next()) {
            const QString username = users.value(0).toString();
            if (usernames.count(username) == 0) {
                stale_users.push_back(username);
            }
        }
        users.finish();

        for (const QString& username : stale_users) {
            QSqlQuery result_pairs(database);
            result_pairs.prepare(
                "SELECT DISTINCT username, problem FROM submissions WHERE user_id=("
                "SELECT id FROM users WHERE username=?)");
            result_pairs.addBindValue(username);
            check(result_pairs.exec(), result_pairs);
            std::vector<std::pair<QString, QString>> pairs_to_remove;
            while (result_pairs.next()) {
                pairs_to_remove.emplace_back(result_pairs.value(0).toString(),
                                             result_pairs.value(1).toString());
            }
            result_pairs.finish();
            for (const auto& [pair_username, problem] : pairs_to_remove) {
                enqueue_result_pair(database, pair_username, problem);
            }

            QSqlQuery remove(database);
            remove.prepare("DELETE FROM users WHERE username=? AND role='contestant'");
            remove.addBindValue(username);
            check(remove.exec(), remove);
            result.removed += remove.numRowsAffected();
        }

        exec(database, "COMMIT");
        return result;
    } catch (...) {
        QSqlQuery rollback(database);
        rollback.exec("ROLLBACK");
        throw;
    }
}

std::optional<User> Database::authenticate(const QString& username, const QString& password) {
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlQuery query(connection());
    query.prepare("SELECT id, username, salt, hash, password, role "
                  "FROM users WHERE username=?");
    query.addBindValue(username);
    check(query.exec(), query);
    if (!query.next()) {
        return std::nullopt;
    }
    const QString stored_password = query.value(4).toString();
    const bool password_matches = !stored_password.isEmpty()
                                      ? stored_password == password
                                      : legacy_password_hash(query.value(2).toString(), password) ==
                                            query.value(3).toString();
    if (!password_matches) {
        return std::nullopt;
    }
    return User{query.value(0).toInt(), query.value(1).toString(), query.value(5).toString(), {}};
}

QString Database::create_session(int user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    QString token = random_token(32);
    QString csrf = random_token(24);
    QSqlQuery query(connection());
    query.prepare("INSERT INTO sessions(token, user_id, csrf, expires_at) VALUES(?, ?, ?, ?)");
    query.addBindValue(token);
    query.addBindValue(user_id);
    query.addBindValue(csrf);
    query.addBindValue(QDateTime::currentSecsSinceEpoch() + kSessionLifetimeSeconds);
    check(query.exec(), query);
    return token;
}

void Database::delete_session(const QString& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlQuery query(connection());
    query.prepare("DELETE FROM sessions WHERE token=?");
    query.addBindValue(token);
    check(query.exec(), query);
}

std::optional<User> Database::user_for_session(const QString& token) {
    if (token.isEmpty()) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlQuery cleanup(connection());
    cleanup.prepare("DELETE FROM sessions WHERE expires_at < ?");
    cleanup.addBindValue(QDateTime::currentSecsSinceEpoch());
    check(cleanup.exec(), cleanup);

    QSqlQuery query(connection());
    query.prepare("SELECT users.id, users.username, users.role, sessions.csrf "
                  "FROM sessions JOIN users ON users.id=sessions.user_id "
                  "WHERE sessions.token=? AND sessions.expires_at >= ?");
    query.addBindValue(token);
    query.addBindValue(QDateTime::currentSecsSinceEpoch());
    check(query.exec(), query);
    if (!query.next()) {
        return std::nullopt;
    }
    return User{query.value(0).toInt(), query.value(1).toString(), query.value(2).toString(),
                query.value(3).toString()};
}

SubmissionAdmissionResult Database::create_submission_draft(
    int user_id, const QString& username, const QString& problem,
    const SubmissionAdmissionLimits& limits) {
    if (limits.max_active_per_user <= 0 || limits.max_active_global <= 0 ||
        limits.max_recent_per_user <= 0 || limits.recent_window_seconds <= 0) {
        throw std::invalid_argument("submission admission limits must be positive");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    QSqlDatabase database = connection();
    exec(database, "BEGIN IMMEDIATE");
    try {
        QSqlQuery user_active(database);
        user_active.prepare(
            "SELECT COUNT(*) FROM submissions AS submission "
            "WHERE submission.user_id=? AND (submission.status='running' OR ("
            "submission.status IN ('staging','queued') AND NOT EXISTS ("
            "SELECT 1 FROM ignored_submissions AS ignored "
            "WHERE ignored.submission_id=submission.id)))");
        user_active.addBindValue(user_id);
        check(user_active.exec(), user_active);
        if (!user_active.next()) {
            throw std::runtime_error("active submission count returned no row");
        }
        const bool user_limit_reached =
            user_active.value(0).toInt() >= limits.max_active_per_user;
        user_active.finish();
        if (user_limit_reached) {
            exec(database, "COMMIT");
            return {SubmissionAdmissionStatus::UserActiveLimit, 0};
        }

        QSqlQuery global_active(database);
        check(global_active.exec(
                  "SELECT COUNT(*) FROM submissions AS submission "
                  "WHERE submission.status='running' OR ("
                  "submission.status IN ('staging','queued') AND NOT EXISTS ("
                  "SELECT 1 FROM ignored_submissions AS ignored "
                  "WHERE ignored.submission_id=submission.id))"),
              global_active);
        if (!global_active.next()) {
            throw std::runtime_error("global active submission count returned no row");
        }
        const bool global_limit_reached =
            global_active.value(0).toInt() >= limits.max_active_global;
        global_active.finish();
        if (global_limit_reached) {
            exec(database, "COMMIT");
            return {SubmissionAdmissionStatus::GlobalActiveLimit, 0};
        }

        const std::int64_t now = QDateTime::currentSecsSinceEpoch();
        QSqlQuery recent(database);
        recent.prepare("SELECT COUNT(*) FROM submissions WHERE user_id=? AND submitted_at>?");
        recent.addBindValue(user_id);
        recent.addBindValue(
            QVariant::fromValue<qlonglong>(now - limits.recent_window_seconds));
        check(recent.exec(), recent);
        if (!recent.next()) {
            throw std::runtime_error("recent submission count returned no row");
        }
        const bool rate_limit_reached =
            recent.value(0).toInt() >= limits.max_recent_per_user;
        recent.finish();
        if (rate_limit_reached) {
            exec(database, "COMMIT");
            return {SubmissionAdmissionStatus::RateLimit, 0};
        }

        QSqlQuery insert(database);
        insert.prepare("INSERT INTO submissions(user_id, username, problem, source_path, status, "
                       "verdict, score, message, submitted_at, judged_at) "
                       "VALUES(?, ?, ?, '', 'staging', '', 0, 'Saving source...', ?, 0)");
        insert.addBindValue(user_id);
        insert.addBindValue(username);
        insert.addBindValue(problem);
        insert.addBindValue(QVariant::fromValue<qlonglong>(now));
        check(insert.exec(), insert);
        const int submission_id = insert.lastInsertId().toInt();

        enqueue_result_pair(database, username, problem);
        exec(database, "COMMIT");
        return {SubmissionAdmissionStatus::Accepted, submission_id};
    } catch (...) {
        QSqlQuery rollback(database);
        rollback.exec("ROLLBACK");
        throw;
    }
}

void Database::queue_submission(int id, const QString& source_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlQuery query(connection());
    query.prepare("UPDATE submissions SET source_path=?, status='queued', message='' "
                  "WHERE id=? AND status='staging'");
    query.addBindValue(source_path);
    query.addBindValue(id);
    check(query.exec(), query);
    if (query.numRowsAffected() != 1) {
        throw std::runtime_error("submission draft is no longer available");
    }
}

std::optional<SubmissionSummary> Database::take_next_queued() {
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlDatabase database = connection();
    exec(database, "BEGIN IMMEDIATE");
    try {
        QSqlQuery select(database);
        check(select.exec("SELECT id, user_id, username, problem, source_path, status, verdict, "
                          "score, message, submitted_at, judged_at, 0 "
                          "FROM submissions WHERE status='queued' "
                          "AND NOT EXISTS (SELECT 1 FROM ignored_submissions "
                          "WHERE ignored_submissions.submission_id=submissions.id) "
                          "ORDER BY id LIMIT 1"),
              select);
        if (!select.next()) {
            exec(database, "COMMIT");
            return std::nullopt;
        }
        SubmissionSummary submission = row_to_submission(select);
        select.finish();

        QSqlQuery update(database);
        update.prepare("UPDATE submissions SET status='running', message=? "
                       "WHERE id=? AND status='queued' "
                       "AND NOT EXISTS (SELECT 1 FROM ignored_submissions "
                       "WHERE ignored_submissions.submission_id=submissions.id)");
        update.addBindValue("Judging...");
        update.addBindValue(submission.id);
        check(update.exec(), update);
        if (update.numRowsAffected() != 1) {
            throw std::runtime_error("queued submission could not be claimed");
        }
        exec(database, "COMMIT");
        submission.status = "running";
        submission.message = "Judging...";
        return submission;
    } catch (...) {
        QSqlQuery rollback(database);
        rollback.exec("ROLLBACK");
        throw;
    }
}

void Database::recover_running_submission(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlDatabase database = connection();
    exec(database, "BEGIN IMMEDIATE");
    try {
        QSqlQuery select(database);
        select.prepare(
            "SELECT username, problem, EXISTS(SELECT 1 FROM ignored_submissions "
            "WHERE ignored_submissions.submission_id=submissions.id) "
            "FROM submissions WHERE id=? AND status='running'");
        select.addBindValue(id);
        check(select.exec(), select);
        if (!select.next()) {
            select.finish();
            exec(database, "COMMIT");
            return;
        }
        const QString username = select.value(0).toString();
        const QString problem = select.value(1).toString();
        const bool ignored = select.value(2).toBool();
        select.finish();

        QSqlQuery update(database);
        if (ignored) {
            update.prepare(
                "UPDATE submissions SET status='failed', verdict='IGN', score=0, "
                "message='Ignored by administrator.', judged_at=? "
                "WHERE id=? AND status='running'");
            update.addBindValue(QDateTime::currentSecsSinceEpoch());
        } else {
            update.prepare(
                "UPDATE submissions SET status='queued', message='Retrying after worker error.' "
                "WHERE id=? AND status='running'");
        }
        update.addBindValue(id);
        check(update.exec(), update);
        if (update.numRowsAffected() == 1 && ignored) {
            enqueue_result_pair(database, username, problem);
        }
        exec(database, "COMMIT");
    } catch (...) {
        QSqlQuery rollback(database);
        rollback.exec("ROLLBACK");
        throw;
    }
}

void Database::finish_submission(int id, const QString& status, const QString& verdict,
                                 double score, const QString& message,
                                 const std::vector<TestRow>& rows) {
    QString safe_message = message;
    if (safe_message.isNull()) {
        safe_message = "";
    }
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlDatabase database = connection();
    exec(database, "BEGIN IMMEDIATE");
    try {
        QSqlQuery submission(database);
        submission.prepare("SELECT username, problem FROM submissions WHERE id=?");
        submission.addBindValue(id);
        check(submission.exec(), submission);
        if (!submission.next()) {
            submission.finish();
            exec(database, "COMMIT");
            return;
        }
        const QString username = submission.value(0).toString();
        const QString problem = submission.value(1).toString();
        submission.finish();

        QSqlQuery del(database);
        del.prepare("DELETE FROM test_results WHERE submission_id=?");
        del.addBindValue(id);
        check(del.exec(), del);

        QSqlQuery update(database);
        update.prepare(
            "UPDATE submissions SET status=?, verdict=?, score=?, message=?, judged_at=? "
            "WHERE id=?");
        update.addBindValue(status);
        update.addBindValue(verdict);
        update.addBindValue(score);
        update.addBindValue(safe_message);
        update.addBindValue(QDateTime::currentSecsSinceEpoch());
        update.addBindValue(id);
        check(update.exec(), update);
        if (update.numRowsAffected() != 1) {
            throw std::runtime_error("submission disappeared while being finished");
        }

        for (const auto& row : rows) {
            QSqlQuery insert(database);
            insert.prepare("INSERT INTO test_results(submission_id, test, verdict, time_ms, "
                           "exit_code, max_points, earned_points, message) "
                           "VALUES(?, ?, ?, ?, ?, ?, ?, ?)");
            insert.addBindValue(id);
            insert.addBindValue(row.test);
            insert.addBindValue(row.verdict);
            insert.addBindValue(QVariant::fromValue<qulonglong>(row.time_ms));
            insert.addBindValue(row.exit_code);
            insert.addBindValue(row.max_points);
            insert.addBindValue(row.earned_points);
            QString row_message = row.message;
            if (row_message.isNull()) {
                row_message = "";
            }
            insert.addBindValue(row_message);
            check(insert.exec(), insert);
        }
        enqueue_result_pair(database, username, problem);
        exec(database, "COMMIT");
    } catch (...) {
        QSqlQuery rollback(database);
        rollback.exec("ROLLBACK");
        throw;
    }
}

std::vector<SubmissionSummary> Database::submissions_for_user(int user_id, bool admin) {
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlQuery query(connection());
    if (admin) {
        check(query.exec("SELECT id, user_id, username, problem, source_path, status, verdict, "
                         "score, message, submitted_at, judged_at, "
                         "EXISTS(SELECT 1 FROM ignored_submissions "
                         "WHERE ignored_submissions.submission_id=submissions.id) "
                         "FROM submissions ORDER BY id DESC LIMIT 200"),
              query);
    } else {
        query.prepare("SELECT id, user_id, username, problem, source_path, status, verdict, "
                      "score, message, submitted_at, judged_at, "
                      "EXISTS(SELECT 1 FROM ignored_submissions "
                      "WHERE ignored_submissions.submission_id=submissions.id) "
                      "FROM submissions WHERE user_id=? ORDER BY id DESC LIMIT 100");
        query.addBindValue(user_id);
        check(query.exec(), query);
    }
    std::vector<SubmissionSummary> rows;
    while (query.next()) {
        rows.push_back(row_to_submission(query));
    }
    return rows;
}

std::vector<TestRow> Database::test_results(int submission_id, int user_id, bool admin) {
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlQuery owner(connection());
    owner.prepare("SELECT user_id FROM submissions WHERE id=?");
    owner.addBindValue(submission_id);
    check(owner.exec(), owner);
    if (!owner.next() || (!admin && owner.value(0).toInt() != user_id)) {
        return {};
    }
    return test_results_for_submission_locked(submission_id);
}

std::optional<int> Database::submission_owner_id(int submission_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlQuery query(connection());
    query.prepare("SELECT user_id FROM submissions WHERE id=?");
    query.addBindValue(submission_id);
    check(query.exec(), query);
    if (!query.next()) {
        return std::nullopt;
    }
    return query.value(0).toInt();
}

std::vector<TestRow> Database::test_results_for_submission(int submission_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return test_results_for_submission_locked(submission_id);
}

std::optional<SubmissionSummary> Database::ignore_submission(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlDatabase database = connection();
    exec(database, "BEGIN IMMEDIATE");
    try {
        QSqlQuery select(database);
        select.prepare("SELECT id, user_id, username, problem, source_path, status, verdict, "
                       "score, message, submitted_at, judged_at, "
                       "EXISTS(SELECT 1 FROM ignored_submissions "
                       "WHERE ignored_submissions.submission_id=submissions.id) "
                       "FROM submissions WHERE id=?");
        select.addBindValue(id);
        check(select.exec(), select);
        if (!select.next()) {
            select.finish();
            exec(database, "COMMIT");
            return std::nullopt;
        }
        SubmissionSummary submission = row_to_submission(select);
        select.finish();

        const std::int64_t now = QDateTime::currentSecsSinceEpoch();
        QSqlQuery insert(database);
        insert.prepare("INSERT OR IGNORE INTO ignored_submissions(submission_id, ignored_at) "
                       "VALUES(?, ?)");
        insert.addBindValue(id);
        insert.addBindValue(QVariant::fromValue<qlonglong>(now));
        check(insert.exec(), insert);

        if (submission.status == "staging" || submission.status == "queued") {
            QSqlQuery terminalize(database);
            terminalize.prepare(
                "UPDATE submissions SET status='failed', verdict='IGN', score=0, "
                "message='Ignored by administrator.', judged_at=? "
                "WHERE id=? AND status IN ('staging','queued')");
            terminalize.addBindValue(QVariant::fromValue<qlonglong>(now));
            terminalize.addBindValue(id);
            check(terminalize.exec(), terminalize);
            if (terminalize.numRowsAffected() == 1) {
                submission.status = "failed";
                submission.verdict = "IGN";
                submission.score = 0.0;
                submission.message = "Ignored by administrator.";
                submission.judged_at = now;
            }
        }
        enqueue_result_pair(database, submission.username, submission.problem);
        exec(database, "COMMIT");
        submission.ignored = true;
        return submission;
    } catch (...) {
        QSqlQuery rollback(database);
        rollback.exec("ROLLBACK");
        throw;
    }
}

std::optional<SubmissionSummary> Database::latest_effective_submission(const QString& username,
                                                                       const QString& problem) {
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlQuery query(connection());
    query.prepare("SELECT id, user_id, username, problem, source_path, status, verdict, "
                  "score, message, submitted_at, judged_at, 0 "
                  "FROM submissions WHERE username=? AND problem=? AND status='done' "
                  "AND NOT EXISTS (SELECT 1 FROM ignored_submissions "
                  "WHERE ignored_submissions.submission_id=submissions.id) "
                  "ORDER BY id DESC LIMIT 1");
    query.addBindValue(username);
    query.addBindValue(problem);
    check(query.exec(), query);
    if (!query.next()) {
        return std::nullopt;
    }
    return row_to_submission(query);
}

std::vector<AuthoritativeResultPair> Database::authoritative_result_pairs() {
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlQuery query(connection());
    check(query.exec(
              "WITH latest_effective AS ("
              "SELECT submission.username, submission.problem, "
              "MAX(submission.id) AS submission_id "
              "FROM submissions AS submission "
              "WHERE submission.status='done' AND NOT EXISTS ("
              "SELECT 1 FROM ignored_submissions AS ignored "
              "WHERE ignored.submission_id=submission.id) "
              "GROUP BY submission.username, submission.problem) "
              "SELECT pair.username, pair.problem, pair.generation, "
              "latest.submission_id, result.test, result.verdict, result.time_ms, "
              "result.exit_code, result.max_points, result.earned_points, result.message "
              "FROM server_result_pairs AS pair "
              "LEFT JOIN latest_effective AS latest "
              "ON latest.username=pair.username AND latest.problem=pair.problem "
              "LEFT JOIN test_results AS result "
              "ON result.submission_id=latest.submission_id "
              "ORDER BY pair.username, pair.problem, result.test"),
          query);

    std::vector<AuthoritativeResultPair> pairs;
    while (query.next()) {
        const QString username = query.value(0).toString();
        const QString problem = query.value(1).toString();
        if (pairs.empty() || pairs.back().username != username ||
            pairs.back().problem != problem) {
            pairs.push_back(AuthoritativeResultPair{
                username, problem, query.value(2).toULongLong(), {}});
        }
        if (!query.value(4).isNull()) {
            pairs.back().rows.push_back(TestRow{
                query.value(4).toString(), query.value(5).toString(),
                query.value(6).toULongLong(), query.value(7).toInt(),
                query.value(8).toDouble(), query.value(9).toDouble(),
                query.value(10).toString()});
        }
    }
    return pairs;
}

std::vector<ResultPair> Database::pending_result_pairs() {
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlQuery query(connection());
    check(query.exec("SELECT username, problem, generation FROM server_result_pairs "
                     "WHERE pending=1 "
                     "ORDER BY username, problem"),
          query);
    std::vector<ResultPair> pairs;
    while (query.next()) {
        pairs.push_back(ResultPair{query.value(0).toString(), query.value(1).toString(),
                                   query.value(2).toULongLong()});
    }
    return pairs;
}

std::optional<std::uint64_t> Database::pending_result_generation(const QString& username,
                                                                 const QString& problem) {
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlQuery query(connection());
    query.prepare("SELECT generation FROM server_result_pairs "
                  "WHERE username=? AND problem=? AND pending=1");
    query.addBindValue(username);
    query.addBindValue(problem);
    check(query.exec(), query);
    if (!query.next()) {
        return std::nullopt;
    }
    return query.value(0).toULongLong();
}

bool Database::acknowledge_result_pair(const QString& username, const QString& problem,
                                       std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlQuery query(connection());
    query.prepare("UPDATE server_result_pairs SET pending=0 "
                  "WHERE username=? AND problem=? AND generation=? AND pending=1");
    query.addBindValue(username);
    query.addBindValue(problem);
    query.addBindValue(QVariant::fromValue<qulonglong>(generation));
    check(query.exec(), query);
    return query.numRowsAffected() == 1;
}

std::vector<QString> Database::contestant_usernames() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<QString> usernames;
    QSqlQuery query(connection());
    check(query.exec("SELECT username FROM users WHERE role='contestant' ORDER BY username"),
          query);
    while (query.next()) {
        usernames.push_back(query.value(0).toString());
    }
    return usernames;
}

std::vector<SourceSnapshot> Database::terminal_source_snapshots_to_prune(
    int retain_per_user, std::optional<int> user_id) {
    if (retain_per_user < 0) {
        throw std::invalid_argument("source snapshot retention cannot be negative");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    QSqlQuery query(connection());
    QString sql =
        "SELECT id, user_id, source_path FROM submissions "
        "WHERE status IN ('done','failed') AND source_path<>''";
    if (user_id) {
        sql += " AND user_id=?";
    }
    sql += " ORDER BY user_id, id DESC";
    query.prepare(sql);
    if (user_id) {
        query.addBindValue(*user_id);
    }
    check(query.exec(), query);

    std::vector<SourceSnapshot> snapshots;
    int current_user_id = -1;
    int retained_for_user = 0;
    while (query.next()) {
        const int row_user_id = query.value(1).toInt();
        if (row_user_id != current_user_id) {
            current_user_id = row_user_id;
            retained_for_user = 0;
        }
        if (retained_for_user++ >= retain_per_user) {
            snapshots.push_back(SourceSnapshot{query.value(0).toInt(), row_user_id,
                                               query.value(2).toString()});
        }
    }
    return snapshots;
}

bool Database::clear_terminal_source_snapshot(int submission_id,
                                              const QString& expected_source_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlQuery query(connection());
    query.prepare("UPDATE submissions SET source_path='' "
                  "WHERE id=? AND source_path=? AND status IN ('done','failed')");
    query.addBindValue(submission_id);
    query.addBindValue(expected_source_path);
    check(query.exec(), query);
    return query.numRowsAffected() == 1;
}

std::set<int> Database::source_snapshot_submission_ids() {
    std::lock_guard<std::mutex> lock(mutex_);
    QSqlQuery query(connection());
    check(query.exec("SELECT id FROM submissions WHERE source_path<>''"), query);
    std::set<int> ids;
    while (query.next()) {
        ids.insert(query.value(0).toInt());
    }
    return ids;
}

QSqlDatabase Database::connection() {
    const QString name = QString("neothemis_server_%1_%2")
                             .arg(instance_id_)
                             .arg(thread_connections.token);
    QSqlDatabase existing;
    {
        std::lock_guard<std::mutex> registry_lock(sql_connection_registry_mutex);
        if (QSqlDatabase::contains(name)) {
            existing = QSqlDatabase::database(name, false);
        }
    }
    if (existing.isValid()) {
        try {
            if (!existing.isOpen() && !existing.open()) {
                throw std::runtime_error(existing.lastError().text().toStdString());
            }
            exec(existing, "PRAGMA foreign_keys=ON");
            exec(existing, QString("PRAGMA busy_timeout=%1").arg(kDatabaseBusyTimeoutMs));
            return existing;
        } catch (...) {
            existing = QSqlDatabase();
            remove_sql_connection(name);
            thread_connections.names.erase(name);
            throw;
        }
    }
    if (!path_.parent_path().empty()) {
        fs::create_directories(path_.parent_path());
    }
    QSqlDatabase database;
    {
        // Qt documents the registry operations as thread-safe, but serializing
        // driver creation/destruction also avoids a MinGW/Qt SQLite heap race
        // when many short-lived worker connections finish simultaneously.
        std::lock_guard<std::mutex> registry_lock(sql_connection_registry_mutex);
        database = QSqlDatabase::addDatabase("QSQLITE", name);
    }
    thread_connections.names.insert(name);
    database.setDatabaseName(database_path_string(path_));
    database.setConnectOptions(QString("QSQLITE_BUSY_TIMEOUT=%1").arg(kDatabaseBusyTimeoutMs));
    try {
        if (!database.open()) {
            throw std::runtime_error(database.lastError().text().toStdString());
        }
        exec(database, "PRAGMA foreign_keys=ON");
        exec(database, QString("PRAGMA busy_timeout=%1").arg(kDatabaseBusyTimeoutMs));
        return database;
    } catch (...) {
        database = QSqlDatabase();
        remove_sql_connection(name);
        thread_connections.names.erase(name);
        throw;
    }
}

void Database::exec(QSqlDatabase& database, const QString& sql) {
    QSqlQuery query(database);
    check(query.exec(sql), query);
}

void Database::ensure_column(QSqlDatabase& database, const QString& table, const QString& column,
                             const QString& definition) {
    QSqlQuery info(database);
    check(info.exec("PRAGMA table_info(" + table + ")"), info);
    while (info.next()) {
        if (info.value(1).toString() == column) {
            return;
        }
    }
    exec(database, "ALTER TABLE " + table + " ADD COLUMN " + column + " " + definition);
}

void Database::check(bool ok, const QSqlQuery& query) {
    if (!ok) {
        throw std::runtime_error(query.lastError().text().toStdString());
    }
}

void Database::enqueue_result_pair(QSqlDatabase& database, const QString& username,
                                   const QString& problem) {
    QSqlQuery query(database);
    query.prepare(
        "INSERT INTO server_result_pairs(username, problem, generation, pending) "
        "VALUES(?, ?, 1, 1) "
        "ON CONFLICT(username, problem) DO UPDATE SET generation=generation+1, pending=1");
    query.addBindValue(username);
    query.addBindValue(problem);
    check(query.exec(), query);
}

SubmissionSummary Database::row_to_submission(const QSqlQuery& query) {
    return SubmissionSummary{
        query.value(0).toInt(),      query.value(1).toInt(),       query.value(2).toString(),
        query.value(3).toString(),   query.value(4).toString(),    query.value(5).toString(),
        query.value(6).toString(),   query.value(7).toDouble(),    query.value(8).toString(),
        query.value(9).toLongLong(), query.value(10).toLongLong(), query.value(11).toBool()};
}

std::vector<TestRow> Database::test_results_for_submission_locked(int submission_id) {
    QSqlQuery query(connection());
    query.prepare("SELECT test, verdict, time_ms, exit_code, max_points, earned_points, message "
                  "FROM test_results WHERE submission_id=? ORDER BY test");
    query.addBindValue(submission_id);
    check(query.exec(), query);
    std::vector<TestRow> rows;
    while (query.next()) {
        rows.push_back(TestRow{query.value(0).toString(), query.value(1).toString(),
                               query.value(2).toULongLong(), query.value(3).toInt(),
                               query.value(4).toDouble(), query.value(5).toDouble(),
                               query.value(6).toString()});
    }
    return rows;
}

} // namespace neothemis::server
