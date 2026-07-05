#include "neothemis/JudgeCore.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QHostAddress>
#include <QRandomGenerator>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>
#include <QRegularExpression>
#include <QVariant>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
constexpr std::size_t kMaxBodyBytes = 512 * 1024;
constexpr std::size_t kMaxSourceBytes = 256 * 1024;
constexpr std::int64_t kSessionLifetimeSeconds = 12 * 60 * 60;

struct AppConfig {
    fs::path contest_root;
    fs::path data_dir;
    QHostAddress host = QHostAddress::LocalHost;
    quint16 port = 8080;
    bool allow_lan = false;
    QString admin_user = "admin";
    QString admin_password;
    QString join_code;
};

struct ContestSettings {
    fs::path contestants_dir = "contestants";
    fs::path tests_dir = "tests";
    fs::path output_csv = "results.csv";
    std::string core_name = "builtin";
    std::string compiler = "g++";
    std::string compile_flags = "-std=c++17 -O2 -pipe";
    std::uint64_t stack_limit_mb = 64;
    unsigned int parallel_jobs = 1;
    std::vector<std::string> forbidden_patterns;
    bool server_ranking_enabled = false;
    bool server_contestant_details_enabled = false;
};

struct User {
    int id = 0;
    QString username;
    QString role;
    QString csrf;
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

struct RankingRow {
    QString username;
    std::map<QString, double> problem_scores;
    double total = 0.0;
};

struct RankingTable {
    std::vector<QString> problems;
    std::vector<RankingRow> rows;
};

std::string trim(const std::string& value) {
    std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool parse_bool(const std::string& value) {
    return value == "true" || value == "1" || value == "yes" || value == "on";
}

std::vector<std::string> default_forbidden_patterns() {
    return {
        "system(",
        "popen(",
        "fork(",
        "exec(",
        "#include <unistd.h>",
        "#include <sys/",
        "#include <windows.h>"
    };
}

ContestSettings load_contest_settings(const fs::path& contest_root) {
    ContestSettings settings;
    settings.forbidden_patterns = default_forbidden_patterns();
    fs::path path = contest_root / "neothemis.conf";
    if (!fs::exists(path)) {
        return settings;
    }

    settings.forbidden_patterns.clear();
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("failed to open " + path.string());
    }

    std::string line;
    while (std::getline(in, line)) {
        std::string stripped = trim(line);
        if (stripped.empty() || stripped[0] == '#') {
            continue;
        }
        std::size_t equal = stripped.find('=');
        if (equal == std::string::npos) {
            continue;
        }
        std::string key = trim(stripped.substr(0, equal));
        std::string value = trim(stripped.substr(equal + 1));
        if (key == "core") {
            settings.core_name = value;
        } else if (key == "contestants_dir") {
            settings.contestants_dir = value;
        } else if (key == "tests_dir") {
            settings.tests_dir = value;
        } else if (key == "output_csv") {
            settings.output_csv = value;
        } else if (key == "compiler") {
            settings.compiler = value;
        } else if (key == "compile_flags") {
            settings.compile_flags = value;
        } else if (key == "stack_limit_mb") {
            settings.stack_limit_mb = static_cast<std::uint64_t>(std::stoull(value));
        } else if (key == "parallel_jobs") {
            settings.parallel_jobs = static_cast<unsigned int>(std::stoul(value));
        } else if (key == "forbidden_pattern") {
            settings.forbidden_patterns.push_back(value);
        } else if (key == "keep_workdir") {
            (void)parse_bool(value);
        } else if (key == "server_ranking_enabled") {
            settings.server_ranking_enabled = parse_bool(value);
        } else if (key == "server_contestant_details_enabled") {
            settings.server_contestant_details_enabled = parse_bool(value);
        }
    }
    if (settings.forbidden_patterns.empty()) {
        settings.forbidden_patterns = default_forbidden_patterns();
    }
    return settings;
}

QString html_escape(QString value) {
    value.replace('&', "&amp;");
    value.replace('<', "&lt;");
    value.replace('>', "&gt;");
    value.replace('"', "&quot;");
    value.replace('\'', "&#39;");
    return value;
}

QString random_token(std::size_t bytes = 24) {
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

bool valid_identifier(const QString& value) {
    static const QRegularExpression pattern("^[A-Za-z0-9_-]{1,32}$");
    return pattern.match(value).hasMatch();
}

bool valid_username(const QString& value) {
    static const QRegularExpression pattern("^[A-Za-z0-9 _-]{1,64}$");
    static const QRegularExpression has_visible(".*[A-Za-z0-9].*");
    return pattern.match(value).hasMatch() && has_visible.match(value).hasMatch();
}

QString now_string(std::int64_t epoch) {
    if (epoch <= 0) {
        return "-";
    }
    return QDateTime::fromSecsSinceEpoch(epoch).toString("yyyy-MM-dd HH:mm:ss");
}

std::vector<QString> list_problems(const fs::path& contest_root, const ContestSettings& settings) {
    std::vector<QString> problems;
    fs::path tests_root = contest_root / settings.tests_dir;
    if (!fs::exists(tests_root)) {
        return problems;
    }
    for (const auto& entry : fs::directory_iterator(tests_root)) {
        if (entry.is_directory()) {
            problems.push_back(QString::fromStdString(entry.path().filename().string()));
        }
    }
    std::sort(problems.begin(), problems.end());
    return problems;
}

bool problem_exists(const std::vector<QString>& problems, const QString& problem) {
    return std::find(problems.begin(), problems.end(), problem) != problems.end();
}

void safe_copy_problem(const fs::path& source, const fs::path& destination) {
    std::error_code ignored;
    fs::remove_all(destination, ignored);
    fs::create_directories(destination.parent_path());
    fs::copy(source, destination,
             fs::copy_options::recursive |
                 fs::copy_options::skip_symlinks |
                 fs::copy_options::skip_existing);
}

void write_text_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("failed to write " + path.string());
    }
    out << content;
}

std::vector<std::string> parse_csv_line(const std::string& line) {
    std::vector<std::string> fields;
    std::string cell;
    bool in_quotes = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        char ch = line[i];
        if (in_quotes) {
            if (ch == '"' && i + 1 < line.size() && line[i + 1] == '"') {
                cell.push_back('"');
                ++i;
            } else if (ch == '"') {
                in_quotes = false;
            } else {
                cell.push_back(ch);
            }
            continue;
        }
        if (ch == '"') {
            in_quotes = true;
        } else if (ch == ',') {
            fields.push_back(cell);
            cell.clear();
        } else if (ch != '\r') {
            cell.push_back(ch);
        }
    }
    fields.push_back(cell);
    return fields;
}

std::string csv_escape(std::string value) {
    bool needs_quotes = value.find_first_of(",\"\n\r") != std::string::npos;
    std::string escaped;
    for (char ch : value) {
        if (ch == '"') {
            escaped += "\"\"";
        } else if (ch != '\r') {
            escaped += ch;
        }
    }
    return needs_quotes ? "\"" + escaped + "\"" : escaped;
}

class Database {
public:
    explicit Database(fs::path path) : path_(std::move(path)) {}

    void initialize() {
        std::lock_guard<std::mutex> lock(mutex_);
        QSqlDatabase database = connection();
        exec(database, "PRAGMA journal_mode=WAL");
        exec(database, "PRAGMA foreign_keys=ON");
        exec(database,
             "CREATE TABLE IF NOT EXISTS users ("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "username TEXT UNIQUE NOT NULL,"
             "salt TEXT NOT NULL,"
             "hash TEXT NOT NULL,"
             "password TEXT NOT NULL DEFAULT '',"
             "role TEXT NOT NULL,"
             "created_at INTEGER NOT NULL)");
        exec(database,
             "CREATE TABLE IF NOT EXISTS sessions ("
             "token TEXT PRIMARY KEY,"
             "user_id INTEGER NOT NULL,"
             "csrf TEXT NOT NULL,"
             "expires_at INTEGER NOT NULL,"
             "FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE)");
        exec(database,
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
        exec(database,
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
        exec(database,
             "CREATE TABLE IF NOT EXISTS ignored_submissions ("
             "submission_id INTEGER PRIMARY KEY,"
             "ignored_at INTEGER NOT NULL,"
             "FOREIGN KEY(submission_id) REFERENCES submissions(id) ON DELETE CASCADE)");
        ensure_column(database, "test_results", "exit_code", "INTEGER NOT NULL DEFAULT 0");
        ensure_column(database, "users", "password", "TEXT NOT NULL DEFAULT ''");
        exec(database, "UPDATE submissions SET status='queued' WHERE status='running'");
    }

    bool has_admin() {
        std::lock_guard<std::mutex> lock(mutex_);
        QSqlQuery query(connection());
        check(query.exec("SELECT 1 FROM users WHERE role='admin' LIMIT 1"), query);
        return query.next();
    }

    void create_user(const QString& username, const QString& password, const QString& role) {
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

    std::optional<User> authenticate(const QString& username, const QString& password) {
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
        return User{query.value(0).toInt(), query.value(1).toString(),
                    query.value(5).toString(), {}};
    }

    QString create_session(int user_id) {
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

    void delete_session(const QString& token) {
        std::lock_guard<std::mutex> lock(mutex_);
        QSqlQuery query(connection());
        query.prepare("DELETE FROM sessions WHERE token=?");
        query.addBindValue(token);
        check(query.exec(), query);
    }

    std::optional<User> user_for_session(const QString& token) {
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
        return User{query.value(0).toInt(), query.value(1).toString(),
                    query.value(2).toString(), query.value(3).toString()};
    }

    int create_submission(int user_id,
                          const QString& username,
                          const QString& problem,
                          const QString& source_path) {
        std::lock_guard<std::mutex> lock(mutex_);
        QSqlQuery query(connection());
        query.prepare("INSERT INTO submissions(user_id, username, problem, source_path, status, "
                      "verdict, score, message, submitted_at, judged_at) "
                      "VALUES(?, ?, ?, ?, 'queued', '', 0, '', ?, 0)");
        query.addBindValue(user_id);
        query.addBindValue(username);
        query.addBindValue(problem);
        query.addBindValue(source_path);
        query.addBindValue(QDateTime::currentSecsSinceEpoch());
        check(query.exec(), query);
        return query.lastInsertId().toInt();
    }

    std::optional<SubmissionSummary> take_next_queued() {
        std::lock_guard<std::mutex> lock(mutex_);
        QSqlDatabase database = connection();
        QSqlQuery select(database);
        check(select.exec("SELECT id, user_id, username, problem, source_path, status, verdict, "
                          "score, message, submitted_at, judged_at, 0 "
                          "FROM submissions WHERE status='queued' "
                          "AND NOT EXISTS (SELECT 1 FROM ignored_submissions "
                          "WHERE ignored_submissions.submission_id=submissions.id) "
                          "ORDER BY id LIMIT 1"),
              select);
        if (!select.next()) {
            return std::nullopt;
        }
        SubmissionSummary submission = row_to_submission(select);
        select.finish();
        QSqlQuery update(database);
        update.prepare("UPDATE submissions SET status='running', message=? WHERE id=?");
        update.addBindValue("Judging...");
        update.addBindValue(submission.id);
        check(update.exec(), update);
        submission.status = "running";
        submission.message = "Judging...";
        return submission;
    }

    void finish_submission(int id,
                           const QString& status,
                           const QString& verdict,
                           double score,
                           const QString& message,
                           const std::vector<TestRow>& rows) {
        QString safe_message = message;
        if (safe_message.isNull()) {
            safe_message = "";
        }
        std::lock_guard<std::mutex> lock(mutex_);
        QSqlDatabase database = connection();
        QSqlQuery del(database);
        del.prepare("DELETE FROM test_results WHERE submission_id=?");
        del.addBindValue(id);
        check(del.exec(), del);

        QSqlQuery update(database);
        update.prepare("UPDATE submissions SET status=?, verdict=?, score=?, message=?, judged_at=? "
                       "WHERE id=?");
        update.addBindValue(status);
        update.addBindValue(verdict);
        update.addBindValue(score);
        update.addBindValue(safe_message);
        update.addBindValue(QDateTime::currentSecsSinceEpoch());
        update.addBindValue(id);
        check(update.exec(), update);

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
    }

    std::vector<SubmissionSummary> submissions_for_user(int user_id, bool admin) {
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

    std::vector<TestRow> test_results(int submission_id, int user_id, bool admin) {
        std::lock_guard<std::mutex> lock(mutex_);
        QSqlQuery owner(connection());
        owner.prepare("SELECT user_id FROM submissions WHERE id=?");
        owner.addBindValue(submission_id);
        check(owner.exec(), owner);
        if (!owner.next() || (!admin && owner.value(0).toInt() != user_id)) {
            return {};
        }
        QSqlQuery query(connection());
        query.prepare("SELECT test, verdict, time_ms, exit_code, max_points, earned_points, message "
                      "FROM test_results WHERE submission_id=? ORDER BY test");
        query.addBindValue(submission_id);
        check(query.exec(), query);
        std::vector<TestRow> rows;
        while (query.next()) {
            rows.push_back(TestRow{query.value(0).toString(),
                                   query.value(1).toString(),
                                   query.value(2).toULongLong(),
                                   query.value(3).toInt(),
                                   query.value(4).toDouble(),
                                   query.value(5).toDouble(),
                                   query.value(6).toString()});
        }
        return rows;
    }

    std::optional<int> submission_owner_id(int submission_id) {
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

    std::vector<TestRow> test_results_for_submission(int submission_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        return test_results_for_submission_locked(submission_id);
    }

    std::optional<SubmissionSummary> ignore_submission(int id) {
        std::lock_guard<std::mutex> lock(mutex_);
        QSqlDatabase database = connection();
        QSqlQuery select(database);
        select.prepare("SELECT id, user_id, username, problem, source_path, status, verdict, "
                       "score, message, submitted_at, judged_at, "
                       "EXISTS(SELECT 1 FROM ignored_submissions "
                       "WHERE ignored_submissions.submission_id=submissions.id) "
                       "FROM submissions WHERE id=?");
        select.addBindValue(id);
        check(select.exec(), select);
        if (!select.next()) {
            return std::nullopt;
        }
        SubmissionSummary submission = row_to_submission(select);
        select.finish();

        QSqlQuery insert(database);
        insert.prepare("INSERT OR IGNORE INTO ignored_submissions(submission_id, ignored_at) "
                       "VALUES(?, ?)");
        insert.addBindValue(id);
        insert.addBindValue(QDateTime::currentSecsSinceEpoch());
        check(insert.exec(), insert);
        submission.ignored = true;
        return submission;
    }

    std::optional<SubmissionSummary> latest_effective_submission(const QString& username,
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

    bool submission_ignored(int id) {
        std::lock_guard<std::mutex> lock(mutex_);
        QSqlQuery query(connection());
        query.prepare("SELECT 1 FROM ignored_submissions WHERE submission_id=?");
        query.addBindValue(id);
        check(query.exec(), query);
        return query.next();
    }

    std::vector<QString> contestant_usernames() {
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

private:
    QSqlDatabase connection() {
        QString name = QString("neothemis_server_%1_%2")
                           .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()))
                           .arg(qHash(QString::fromStdString(path_.string())));
        if (QSqlDatabase::contains(name)) {
            return QSqlDatabase::database(name);
        }
        fs::create_directories(path_.parent_path());
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", name);
        database.setDatabaseName(QString::fromStdString(path_.string()));
        if (!database.open()) {
            throw std::runtime_error(database.lastError().text().toStdString());
        }
        return database;
    }

    static void exec(QSqlDatabase& database, const QString& sql) {
        QSqlQuery query(database);
        check(query.exec(sql), query);
    }

    static void ensure_column(QSqlDatabase& database,
                              const QString& table,
                              const QString& column,
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

    static void check(bool ok, const QSqlQuery& query) {
        if (!ok) {
            throw std::runtime_error(query.lastError().text().toStdString());
        }
    }

    static SubmissionSummary row_to_submission(const QSqlQuery& query) {
        return SubmissionSummary{query.value(0).toInt(),
                                 query.value(1).toInt(),
                                 query.value(2).toString(),
                                 query.value(3).toString(),
                                 query.value(4).toString(),
                                 query.value(5).toString(),
                                 query.value(6).toString(),
                                 query.value(7).toDouble(),
                                 query.value(8).toString(),
                                 query.value(9).toLongLong(),
                                 query.value(10).toLongLong(),
                                 query.value(11).toBool()};
    }

    std::vector<TestRow> test_results_for_submission_locked(int submission_id) {
        QSqlQuery query(connection());
        query.prepare("SELECT test, verdict, time_ms, exit_code, max_points, earned_points, message "
                      "FROM test_results WHERE submission_id=? ORDER BY test");
        query.addBindValue(submission_id);
        check(query.exec(), query);
        std::vector<TestRow> rows;
        while (query.next()) {
            rows.push_back(TestRow{query.value(0).toString(),
                                   query.value(1).toString(),
                                   query.value(2).toULongLong(),
                                   query.value(3).toInt(),
                                   query.value(4).toDouble(),
                                   query.value(5).toDouble(),
                                   query.value(6).toString()});
        }
        return rows;
    }

    fs::path path_;
    std::mutex mutex_;
};

struct HttpRequest {
    QString method;
    QString path;
    QUrlQuery query;
    std::map<QString, QString> headers;
    QByteArray body;
};

struct HttpResponse {
    int status = 200;
    QString reason = "OK";
    QString content_type = "text/html; charset=utf-8";
    QByteArray body;
    std::vector<QString> headers;
};

QString header_value(const HttpRequest& request, const QString& key) {
    QString wanted = key.toLower();
    for (const auto& [name, value] : request.headers) {
        if (name.toLower() == wanted) {
            return value;
        }
    }
    return {};
}

QString cookie_value(const HttpRequest& request, const QString& key) {
    QString cookie = header_value(request, "cookie");
    for (const QString& part : cookie.split(';')) {
        QString trimmed = part.trimmed();
        int equal = trimmed.indexOf('=');
        if (equal > 0 && trimmed.left(equal) == key) {
            return trimmed.mid(equal + 1);
        }
    }
    return {};
}

using FormFields = std::map<QString, QString>;

QString form_decode(QByteArray value) {
    value.replace('+', ' ');
    return QUrl::fromPercentEncoding(value);
}

FormFields form_body(const HttpRequest& request) {
    FormFields fields;
    const QList<QByteArray> pairs = request.body.split('&');
    for (const QByteArray& pair : pairs) {
        int equal = pair.indexOf('=');
        QByteArray key = equal < 0 ? pair : pair.left(equal);
        QByteArray value = equal < 0 ? QByteArray() : pair.mid(equal + 1);
        fields[form_decode(key)] = form_decode(value);
    }
    return fields;
}

QString form_value(const FormFields& form, const QString& key) {
    auto found = form.find(key);
    return found == form.end() ? QString() : found->second;
}

HttpResponse html_response(const QString& html) {
    HttpResponse response;
    response.body = html.toUtf8();
    return response;
}

HttpResponse redirect_response(const QString& location) {
    HttpResponse response;
    response.status = 303;
    response.reason = "See Other";
    response.headers.push_back("Location: " + location);
    response.body = "Redirect";
    return response;
}

HttpResponse error_response(int status, const QString& message) {
    HttpResponse response;
    response.status = status;
    response.reason = status == 400 ? "Bad Request"
                    : status == 401 ? "Unauthorized"
                    : status == 403 ? "Forbidden"
                    : status == 404 ? "Not Found"
                    : "Error";
    response.body = ("<h1>" + QString::number(status) + "</h1><p>" +
                     html_escape(message) + "</p>").toUtf8();
    return response;
}

QString page_shell(const QString& title,
                   const QString& body,
                   const std::optional<User>& user,
                   bool show_ranking) {
    QString nav = "<nav><a class=\"brand\" href=\"/\"><img src=\"/assets/logo.png\" "
                  "alt=\"\"><strong>NeoThemis</strong></a><div class=\"nav-links\">"
                  "<a href=\"/\">Submit</a><a href=\"/submissions\">Submissions</a>";
    if (show_ranking) {
        nav += "<a href=\"/ranking\">Ranking</a>";
    }
    if (user && user->role == "admin") {
        nav += "<a href=\"/admin\">Admin</a>";
    }
    if (user) {
        nav += "<span class=\"nav-user\">" + html_escape(user->username) +
               "</span><a href=\"/logout\">Logout</a>";
    } else {
        nav += "<a href=\"/login\">Login</a><a href=\"/register\">Register</a>";
    }
    nav += "</div></nav>";

    return "<!doctype html><html><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
           "<link rel=\"icon\" type=\"image/png\" href=\"/assets/logo.png\">"
           "<title>" + html_escape(title) + "</title>"
           "<style>"
           ":root{color-scheme:dark;font-family:Inter,Segoe UI,Arial,sans-serif;background:#090d12;color:#edf3f7}"
           "*{box-sizing:border-box}body{margin:0;min-height:100vh;background:linear-gradient(135deg,#0d2b34 0%,#0b1821 31%,#1b1420 65%,#2b1523 84%,#190d17 100%);background-attachment:fixed}"
           "body:before{content:'';position:fixed;inset:0;pointer-events:none;background:linear-gradient(107deg,transparent 17%,rgba(83,220,203,.04) 17.2%,transparent 40%),linear-gradient(73deg,transparent 58%,rgba(211,76,112,.035) 58.2%,transparent 80%)}"
           "main{position:relative;max-width:1120px;margin:0 auto;padding:28px 22px 52px}"
           "nav{position:sticky;top:0;z-index:10;display:flex;align-items:center;gap:28px;min-height:62px;padding:9px max(22px,calc((100vw - 1120px)/2));background:rgba(15,22,29,.88);border-bottom:1px solid rgba(84,211,194,.25);backdrop-filter:blur(18px)}"
           "nav a{color:#d9e6ec;text-decoration:none;font-weight:700;white-space:nowrap}nav a:hover{color:#9dfdec}"
           ".brand{display:flex;align-items:center;gap:10px;margin-right:auto;color:#f8fafc!important;font-size:15px}.brand img{width:30px;height:30px;object-fit:contain}.nav-links{display:flex;align-items:center;gap:18px}.nav-user{padding-left:18px;margin-left:4px;border-left:1px solid rgba(255,255,255,.14);color:#b8c9cf;white-space:nowrap}"
           "h1{margin:0 0 18px;font-size:24px;letter-spacing:0}p{line-height:1.55}label{display:block;color:#cfe0e5;font-weight:700;margin-top:3px}"
           ".panel{overflow-x:auto;background:rgba(15,25,33,.82);border:1px solid rgba(255,255,255,.15);border-radius:8px;padding:22px;margin:18px 0;box-shadow:0 16px 38px rgba(0,0,0,.22);backdrop-filter:blur(16px)}"
           "input,select,textarea{box-sizing:border-box;width:100%;background:rgba(7,16,23,.88);color:#effbfc;border:1px solid rgba(116,224,207,.42);border-radius:7px;padding:11px 12px;margin:7px 0 16px;outline:none}input:focus,select:focus,textarea:focus{border-color:#8ef7e3;box-shadow:0 0 0 3px rgba(97,215,199,.12)}"
           "textarea{min-height:360px;font-family:Consolas,monospace;tab-size:4}"
           ".code-editor{position:relative;margin:7px 0 14px;background:#071017;border:1px solid rgba(116,224,207,.42);border-radius:7px;overflow:hidden}"
           ".code-editor textarea,.code-editor pre{box-sizing:border-box;width:100%;min-height:360px;margin:0;padding:10px;font-family:Consolas,monospace;font-size:14px;line-height:1.45;tab-size:4;white-space:pre-wrap;overflow:auto}"
           ".code-editor textarea{position:relative;z-index:2;background:transparent;border:0;resize:vertical;color:#effbfc}"
           ".code-editor pre{position:absolute;inset:0;z-index:1;pointer-events:none;color:#dcebed}"
           ".code-editor.highlighting textarea{color:transparent;caret-color:#effbfc}"
           ".tok-kw{color:#ff8fb3;font-weight:700}.tok-type{color:#8ef7e3}.tok-lit{color:#ffd37a}.tok-comment{color:#8398a0;font-style:italic}.tok-pre{color:#c3a6ff}"
           "button{background:#236d68;color:white;border:1px solid rgba(142,247,227,.35);border-radius:7px;padding:10px 15px;font-weight:800;cursor:pointer}button:hover{background:#2c8179;border-color:#8ef7e3}"
           "button.compact{padding:6px 10px;font-size:12px}.inline-form{margin:0}"
           "table{width:100%;border-collapse:collapse;background:rgba(10,16,22,.54);min-width:620px}th{color:#9dfdec;background:rgba(19,31,39,.78);border-bottom:1px solid rgba(232,106,130,.45)}th,td{padding:10px;text-align:left;vertical-align:top}td{border-bottom:1px solid rgba(255,255,255,.09)}tbody tr:hover{background:rgba(84,211,194,.06)}td a{color:#8ef7e3}"
           ".warn{border-color:rgba(255,112,137,.48);background:rgba(70,24,40,.56)}.muted{color:#a8bdc4}.err{color:#ff9aae}.ok{color:#8ef7e3}.pill{display:inline-block;padding:2px 8px;border-radius:999px;background:#35202b;color:#ffb3c4;font-weight:700}"
           "@media(max-width:700px){nav{position:relative;align-items:flex-start;gap:10px;padding:10px 14px}.brand{padding-top:3px}.nav-links{justify-content:flex-end;gap:10px;flex-wrap:wrap;font-size:13px}.nav-user{width:100%;padding:0;border:0;text-align:right;order:-1}main{padding:14px 10px 36px}.panel{padding:16px;margin:12px 0}h1{font-size:21px}}"
           "</style><script src=\"/assets/submit.js\" defer></script></head><body>" +
           nav + "<main>" + body + "</main></body></html>";
}

QString syntax_highlight_script() {
    return QString::fromLatin1(R"JS(
(() => {
  const textarea = document.querySelector('textarea[data-highlight="cpp"]');
  const output = document.getElementById('source-highlight');
  if (!textarea || !output) return;
  const editor = textarea.closest('.code-editor');
  const keywords = new Set('alignas alignof asm auto break case catch class concept const constexpr consteval constinit continue co_await co_return co_yield decltype default delete do else enum explicit export extern for friend goto if import inline mutable namespace new noexcept operator private protected public register requires return sizeof static static_assert struct switch template this thread_local throw try typedef typeid typename using virtual volatile while'.split(' '));
  const types = new Set('bool char char8_t char16_t char32_t double float int long short signed unsigned void wchar_t size_t string vector array map set unordered_map unordered_set pair tuple priority_queue queue stack deque'.split(' '));
  const escapeHtml = (text) => text.replace(/[&<>"']/g, ch => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[ch]));
  const span = (kind, text) => `<span class="tok-${kind}">${escapeHtml(text)}</span>`;
  function highlight(code) {
    let html = '';
    for (let i = 0; i < code.length;) {
      if (code[i] === '/' && code[i + 1] === '/') {
        let j = i + 2;
        while (j < code.length && code[j] !== '\n') j++;
        html += span('comment', code.slice(i, j));
        i = j;
      } else if (code[i] === '/' && code[i + 1] === '*') {
        let j = i + 2;
        while (j + 1 < code.length && !(code[j] === '*' && code[j + 1] === '/')) j++;
        j = Math.min(code.length, j + 2);
        html += span('comment', code.slice(i, j));
        i = j;
      } else if (code[i] === '"' || code[i] === "'") {
        const quote = code[i];
        let j = i + 1;
        while (j < code.length) {
          if (code[j] === '\\') j += 2;
          else if (code[j] === quote) { j++; break; }
          else j++;
        }
        html += span('lit', code.slice(i, j));
        i = j;
      } else if (code[i] === '#' && (i === 0 || code[i - 1] === '\n')) {
        let j = i + 1;
        while (j < code.length && code[j] !== '\n') j++;
        html += span('pre', code.slice(i, j));
        i = j;
      } else if (/[A-Za-z_]/.test(code[i])) {
        let j = i + 1;
        while (j < code.length && /[A-Za-z0-9_]/.test(code[j])) j++;
        const word = code.slice(i, j);
        html += keywords.has(word) ? span('kw', word) : (types.has(word) ? span('type', word) : escapeHtml(word));
        i = j;
      } else if (/[0-9]/.test(code[i])) {
        let j = i + 1;
        while (j < code.length && /[A-Za-z0-9_.]/.test(code[j])) j++;
        html += span('lit', code.slice(i, j));
        i = j;
      } else {
        html += escapeHtml(code[i++]);
      }
    }
    return html + (code.endsWith('\n') ? ' ' : '');
  }
  function sync() {
    output.innerHTML = highlight(textarea.value);
    output.parentElement.scrollTop = textarea.scrollTop;
    output.parentElement.scrollLeft = textarea.scrollLeft;
  }
  editor.classList.add('highlighting');
  textarea.addEventListener('input', sync);
	  textarea.addEventListener('scroll', sync);
	  sync();
	})();
	(() => {
	  const table = document.getElementById('ranking-table');
	  if (!table) return;
	  async function refreshRanking() {
	    try {
	      const response = await fetch('/ranking-fragment', {cache: 'no-store'});
	      if (response.ok) table.innerHTML = await response.text();
	    } catch (_) {}
	  }
	  setInterval(refreshRanking, 2000);
	})();
	)JS");
}

class LocalJudgeServer;

class HttpConnection : public QObject {
public:
    HttpConnection(QTcpSocket* socket, LocalJudgeServer* app)
        : socket_(socket), app_(app) {
        QObject::connect(socket_, &QTcpSocket::readyRead, [this]() { read_available(); });
        QObject::connect(socket_, &QTcpSocket::disconnected, [this]() { deleteLater(); });
    }

private:
    void read_available();
    bool try_parse();
    void send(HttpResponse response);

    QTcpSocket* socket_ = nullptr;
    LocalJudgeServer* app_ = nullptr;
    QByteArray buffer_;
};

class LocalJudgeServer {
public:
    LocalJudgeServer(AppConfig config, ContestSettings settings)
        : config_(std::move(config)),
          settings_(std::move(settings)),
          db_(config_.data_dir / "server.db") {}

    ~LocalJudgeServer() {
        stop_worker();
    }

    void initialize() {
        if (config_.contest_root.empty() || !fs::exists(config_.contest_root / settings_.tests_dir)) {
            throw std::runtime_error("contest tests directory not found");
        }
        fs::create_directories(config_.data_dir / "submissions");
        fs::create_directories(config_.data_dir / "jobs");
        db_.initialize();
        if (!db_.has_admin()) {
            if (config_.admin_password.isEmpty()) {
                config_.admin_password = random_token(9);
                generated_admin_password_ = true;
            }
            db_.create_user(config_.admin_user, config_.admin_password, "admin");
        }
        if (config_.join_code.isEmpty()) {
            config_.join_code = random_token(6);
            generated_join_code_ = true;
        }
    }

    void start() {
        if (!config_.allow_lan && !config_.host.isLoopback()) {
            throw std::runtime_error("refusing non-loopback bind without --allow-lan");
        }
        QObject::connect(&tcp_server_, &QTcpServer::newConnection, [&]() {
            while (QTcpSocket* socket = tcp_server_.nextPendingConnection()) {
                socket->setParent(&tcp_server_);
                new HttpConnection(socket, this);
            }
        });
        if (!tcp_server_.listen(config_.host, config_.port)) {
            throw std::runtime_error(tcp_server_.errorString().toStdString());
        }
        start_worker();
        std::cout << "NeoThemis server listening on http://"
                  << tcp_server_.serverAddress().toString().toStdString()
                  << ":" << tcp_server_.serverPort() << "\n";
        std::cout << "Contest root: " << config_.contest_root.string() << "\n";
        std::cout << "Data dir: " << config_.data_dir.string() << "\n";
        if (generated_admin_password_) {
            std::cout << "Generated admin login: " << config_.admin_user.toStdString()
                      << " / " << config_.admin_password.toStdString() << "\n";
        }
        if (generated_join_code_) {
            std::cout << "Generated contestant join code: "
                      << config_.join_code.toStdString() << "\n";
        }
        if (!config_.host.isLoopback()) {
            std::cout << "LAN mode is enabled. Run this inside a trusted network only.\n";
        }
    }

    HttpResponse handle(const HttpRequest& request) {
        try {
            std::optional<User> user = db_.user_for_session(cookie_value(request, "NTSID"));
            if (request.method == "GET" && request.path == "/health") {
                HttpResponse response;
                response.content_type = "text/plain; charset=utf-8";
                response.body = "ok\n";
                return response;
            }
            if (request.method == "GET" && request.path == "/assets/submit.js") {
                HttpResponse response;
                response.content_type = "application/javascript; charset=utf-8";
                response.body = syntax_highlight_script().toUtf8();
                return response;
            }
            if (request.method == "GET" && request.path == "/assets/logo.png") {
                QFile logo(":/materials/logo.png");
                if (!logo.open(QIODevice::ReadOnly)) {
                    return error_response(404, "logo not found");
                }
                HttpResponse response;
                response.content_type = "image/png";
                response.headers.push_back("Cache-Control: public, max-age=86400");
                response.body = logo.readAll();
                return response;
            }
            if (request.method == "GET" && request.path == "/login") {
                return login_page({}, user);
            }
            if (request.method == "POST" && request.path == "/login") {
                return login_post(request);
            }
            if (request.method == "GET" && request.path == "/register") {
                return register_page({}, user);
            }
            if (request.method == "POST" && request.path == "/register") {
                return register_post(request);
            }
            if (request.method == "GET" && request.path == "/logout") {
                QString token = cookie_value(request, "NTSID");
                if (!token.isEmpty()) {
                    db_.delete_session(token);
                }
                HttpResponse response = redirect_response("/login");
                response.headers.push_back("Set-Cookie: NTSID=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
                return response;
            }
            if (!user) {
                return redirect_response("/login");
            }
            if (request.method == "GET" && (request.path == "/" || request.path == "/submit")) {
                return submit_page({}, *user);
            }
            if (request.method == "POST" && request.path == "/submit") {
                return submit_post(request, *user);
            }
            if (request.method == "GET" && request.path == "/submissions") {
                return submissions_page(*user, user->role == "admin");
            }
            if (request.method == "GET" && request.path == "/details") {
                return details_page(request, *user);
            }
            if (request.method == "GET" && request.path == "/ranking") {
                return ranking_page(*user);
            }
            if (request.method == "GET" && request.path == "/ranking-fragment") {
                return ranking_fragment(*user);
            }
            if (request.method == "POST" && request.path == "/admin/ignore") {
                if (user->role != "admin") {
                    return error_response(403, "admin only");
                }
                return ignore_submission_post(request, *user);
            }
            if (request.method == "GET" && request.path == "/admin") {
                if (user->role != "admin") {
                    return error_response(403, "admin only");
                }
                return submissions_page(*user, true);
            }
            return error_response(404, "not found");
        } catch (const std::exception& ex) {
            return error_response(500, ex.what());
        }
    }

private:
    bool can_view_ranking(const User& user) const {
        return user.role == "admin" || settings_.server_ranking_enabled;
    }

    QString render_shell(const QString& title,
                         const QString& body,
                         const std::optional<User>& user) const {
        return page_shell(title, body, user, user && can_view_ranking(*user));
    }

    HttpResponse login_page(const QString& error, const std::optional<User>& user) {
        QString body = "<section class=\"panel\"><h1>Login</h1>";
        if (!error.isEmpty()) {
            body += "<p class=\"err\">" + html_escape(error) + "</p>";
        }
        body += "<form method=\"post\" action=\"/login\">"
                "<label>Username</label><input name=\"username\" autocomplete=\"username\" required>"
                "<label>Password</label><input name=\"password\" type=\"password\" autocomplete=\"current-password\" required>"
                "<button type=\"submit\">Login</button></form></section>";
        return html_response(render_shell("Login", body, user));
    }

    HttpResponse login_post(const HttpRequest& request) {
        FormFields form = form_body(request);
        QString username = form_value(form, "username").trimmed();
        QString password = form_value(form, "password");
        auto user = db_.authenticate(username, password);
        if (!user) {
            return login_page("Invalid username or password.", std::nullopt);
        }
        QString token = db_.create_session(user->id);
        HttpResponse response = redirect_response("/submit");
        response.headers.push_back("Set-Cookie: NTSID=" + token +
                                   "; Path=/; HttpOnly; SameSite=Strict");
        return response;
    }

    HttpResponse register_page(const QString& error, const std::optional<User>& user) {
        QString body =
            "<section class=\"panel warn\"><h1>Security Notice</h1>"
            "<p>This local server runs submitted C++ on this machine. Use a VM, container, "
            "or isolated contest machine for untrusted contestants.</p></section>"
            "<section class=\"panel\"><h1>Contestant Registration</h1>";
        if (!error.isEmpty()) {
            body += "<p class=\"err\">" + html_escape(error) + "</p>";
        }
        body += "<form method=\"post\" action=\"/register\">"
                "<label>Join code</label><input name=\"join_code\" required>"
                "<label>Username</label><input name=\"username\" autocomplete=\"username\" required>"
                "<p class=\"muted\">Use letters, digits, spaces, underscore, or dash. This becomes your contestant id.</p>"
                "<label>Password</label><input name=\"password\" type=\"password\" autocomplete=\"new-password\" required>"
                "<button type=\"submit\">Create account</button></form></section>";
        return html_response(render_shell("Register", body, user));
    }

    HttpResponse register_post(const HttpRequest& request) {
        FormFields form = form_body(request);
        QString join_code = form_value(form, "join_code").trimmed();
        QString username = form_value(form, "username").trimmed();
        QString password = form_value(form, "password");
        if (join_code != config_.join_code) {
            return register_page("Invalid join code.", std::nullopt);
        }
        if (!valid_username(username)) {
            return register_page("Username must be 1-64 chars: letters, digits, spaces, underscore, dash.", std::nullopt);
        }
        if (password.size() < 6 || password.size() > 128) {
            return register_page("Password must be between 6 and 128 characters.", std::nullopt);
        }
        try {
            db_.create_user(username, password, "contestant");
            ensure_contestant_folder(username);
        } catch (const std::exception&) {
            return register_page("Username already exists.", std::nullopt);
        }
        return redirect_response("/login");
    }

    HttpResponse submit_page(const QString& error, const User& user) {
        std::vector<QString> problems = list_problems(config_.contest_root, settings_);
        QString body =
            "<section class=\"panel warn\"><h1>Local Judge Security</h1>"
            "<p>Submitted programs are filtered and run in per-submission working folders, "
            "but this is not a complete sandbox. Keep the server on a trusted LAN or isolated VM.</p></section>"
            "<section class=\"panel\"><h1>Submit</h1>";
        if (!error.isEmpty()) {
            body += "<p class=\"err\">" + html_escape(error) + "</p>";
        }
        body += "<form method=\"post\" action=\"/submit\">"
                "<input type=\"hidden\" name=\"_csrf\" value=\"" + html_escape(user.csrf) + "\">"
                "<label>Problem</label><select name=\"problem\">";
        for (const QString& problem : problems) {
            body += "<option value=\"" + html_escape(problem) + "\">" + html_escape(problem) + "</option>";
        }
        body += "</select><label>C++ source</label>"
                "<div class=\"code-editor\"><pre aria-hidden=\"true\"><code id=\"source-highlight\"></code></pre>"
                "<textarea id=\"source-editor\" name=\"source\" data-highlight=\"cpp\" spellcheck=\"false\" required></textarea></div>"
                "<p class=\"muted\">Max source size: " + QString::number(kMaxSourceBytes / 1024) + " KiB.</p>"
                "<button type=\"submit\">Submit</button></form></section>";
        return html_response(render_shell("Submit", body, user));
    }

    HttpResponse submit_post(const HttpRequest& request, const User& user) {
        FormFields form = form_body(request);
        if (form_value(form, "_csrf") != user.csrf) {
            return error_response(403, "bad csrf token");
        }
        QString problem = form_value(form, "problem").trimmed();
        QString source = form_value(form, "source");
        std::vector<QString> problems = list_problems(config_.contest_root, settings_);
        if (!problem_exists(problems, problem)) {
            return submit_page("Unknown problem.", user);
        }
        QByteArray source_bytes = source.toUtf8();
        if (source_bytes.isEmpty() || static_cast<std::size_t>(source_bytes.size()) > kMaxSourceBytes) {
            return submit_page("Source is empty or too large.", user);
        }
        if (!valid_username(user.username) || !valid_identifier(problem)) {
            return submit_page("Unsafe contestant or problem name.", user);
        }

        write_text_file(contest_source_path(user.username, problem), source.toStdString());
        fs::path source_path = config_.data_dir / "submissions" /
                               user.username.toStdString() /
                               (problem.toStdString() + ".cpp");
        write_text_file(source_path, source.toStdString());
        int id = db_.create_submission(user.id, user.username, problem,
                                       QString::fromStdString(source_path.string()));
        (void)id;
        notify_worker();
        return redirect_response("/submissions");
    }

    HttpResponse submissions_page(const User& user, bool admin) {
        auto submissions = db_.submissions_for_user(user.id, admin);
        QString body = "<section class=\"panel\"><h1>" +
                       QString(admin ? "All Submissions" : "My Submissions") + "</h1>"
                       "<table><thead><tr><th>ID</th><th>User</th><th>Problem</th>"
                       "<th>Status</th><th>Verdict</th><th>Score</th><th>Submitted</th>"
                       "<th>Judged</th><th>Message</th>";
        if (admin) {
            body += "<th>Action</th>";
        }
        body += "</tr></thead><tbody>";
        for (const auto& row : submissions) {
            const bool detail_visible = admin || settings_.server_contestant_details_enabled;
            QString id_cell = "#" + QString::number(row.id);
            if (detail_visible) {
                id_cell = "<a href=\"/details?id=" + QString::number(row.id) + "\">" +
                          id_cell + "</a>";
            }
            QString status = row.ignored ? "ignored" : row.status;
            body += "<tr><td>" + id_cell + "</td><td>" + html_escape(row.username) +
                    "</td><td>" + html_escape(row.problem) + "</td><td>" +
                    html_escape(status) + "</td><td>" + html_escape(row.verdict) +
                    "</td><td>" + QString::number(row.score, 'f', 2) + "</td><td>" +
                    now_string(row.submitted_at) + "</td><td>" + now_string(row.judged_at) +
                    "</td><td>" + html_escape(row.message.left(300)) + "</td>";
            if (admin) {
                body += "<td>";
                if (row.ignored) {
                    body += "<span class=\"pill\">ignored</span>";
                } else {
                    body += "<form class=\"inline-form\" method=\"post\" action=\"/admin/ignore\">"
                            "<input type=\"hidden\" name=\"_csrf\" value=\"" + html_escape(user.csrf) + "\">"
                            "<input type=\"hidden\" name=\"id\" value=\"" + QString::number(row.id) + "\">"
                            "<button class=\"compact\" type=\"submit\">Ignore</button></form>";
                }
                body += "</td>";
            }
            body += "</tr>";
        }
        body += "</tbody></table></section>";
        return html_response(render_shell(admin ? "Admin" : "Submissions", body, user));
    }

    HttpResponse details_page(const HttpRequest& request, const User& user) {
        int id = request.query.queryItemValue("id").toInt();
        if (id <= 0) {
            return error_response(400, "bad submission id");
        }
        if (user.role != "admin") {
            if (!settings_.server_contestant_details_enabled) {
                return error_response(403, "submission details are disabled");
            }
            std::optional<int> owner_id = db_.submission_owner_id(id);
            if (!owner_id) {
                return error_response(404, "submission not found");
            }
            if (*owner_id != user.id) {
                return error_response(403, "submission details are private");
            }
        }
        auto rows = db_.test_results(id, user.id, user.role == "admin");
        QString body = "<section class=\"panel\"><h1>Submission #" + QString::number(id) +
                       "</h1><table><thead><tr><th>Test</th><th>Verdict</th><th>Time</th>"
                       "<th>Exit</th><th>Points</th><th>Message</th></tr></thead><tbody>";
        for (const auto& row : rows) {
            body += "<tr><td>" + html_escape(row.test) + "</td><td>" +
                    html_escape(row.verdict) + "</td><td>" +
                    QString::number(row.time_ms) + " ms</td><td>" +
                    QString::number(row.exit_code) + "</td><td>" +
                    QString::number(row.earned_points, 'f', 2) + "/" +
                    QString::number(row.max_points, 'f', 2) + "</td><td>" +
                    html_escape(row.message.left(500)) + "</td></tr>";
        }
        body += "</tbody></table></section>";
        return html_response(render_shell("Details", body, user));
    }

    RankingTable ranking_table() {
        RankingTable table;
        table.problems = list_problems(config_.contest_root, settings_);

        std::map<QString, std::size_t> row_by_username;
        for (const QString& username : db_.contestant_usernames()) {
            row_by_username[username] = table.rows.size();
            table.rows.push_back(RankingRow{username, {}, 0.0});
        }

        std::set<QString> known_problems(table.problems.begin(), table.problems.end());
        fs::path output_path = contest_output_path(settings_.output_csv);
        if (fs::exists(output_path)) {
            std::ifstream input(output_path, std::ios::binary);
            std::string line;
            bool first = true;
            while (std::getline(input, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (first) {
                    first = false;
                    if (line.rfind("contestant,problem,test,", 0) == 0) {
                        continue;
                    }
                }
                const std::vector<std::string> fields = parse_csv_line(line);
                if (fields.size() < 8) {
                    continue;
                }
                const QString username = QString::fromStdString(fields[0]);
                const QString problem = QString::fromStdString(fields[1]);
                auto row = row_by_username.find(username);
                if (row == row_by_username.end() || known_problems.count(problem) == 0) {
                    continue;
                }
                try {
                    table.rows[row->second].problem_scores[problem] += std::stod(fields[7]);
                } catch (const std::exception&) {
                    continue;
                }
            }
        }

        for (RankingRow& row : table.rows) {
            for (const QString& problem : table.problems) {
                row.total += row.problem_scores[problem];
            }
        }
        std::sort(table.rows.begin(), table.rows.end(), [](const RankingRow& a,
                                                           const RankingRow& b) {
            if (std::abs(a.total - b.total) > 1e-9) {
                return a.total > b.total;
            }
            return a.username.toCaseFolded() < b.username.toCaseFolded();
        });
        return table;
    }

    QString ranking_table_html() {
        const RankingTable table = ranking_table();
        QString body = "<table><thead><tr><th>Rank</th><th>Contestant</th>";
        for (const QString& problem : table.problems) {
            body += "<th>" + html_escape(problem) + "</th>";
        }
        body += "<th>Total</th></tr></thead><tbody>";
        int rank = 1;
        for (const RankingRow& row : table.rows) {
            body += "<tr><td>" + QString::number(rank++) + "</td><td>" +
                    html_escape(row.username) + "</td>";
            for (const QString& problem : table.problems) {
                auto score = row.problem_scores.find(problem);
                body += "<td>" + QString::number(
                    score == row.problem_scores.end() ? 0.0 : score->second, 'f', 2) + "</td>";
            }
            body += "<td><strong>" + QString::number(row.total, 'f', 2) +
                    "</strong></td></tr>";
        }
        if (table.rows.empty()) {
            body += "<tr><td colspan=\"" + QString::number(table.problems.size() + 3) +
                    "\" class=\"muted\">No ranking data yet.</td></tr>";
        }
        return body + "</tbody></table>";
    }

    HttpResponse ranking_page(const User& user) {
        if (!can_view_ranking(user)) {
            return error_response(403, "ranking is disabled");
        }
        QString body = "<section class=\"panel\"><div id=\"ranking-table\">" +
                       ranking_table_html() + "</div>"
                       "<p class=\"muted\">This table updates automatically.</p></section>";
        return html_response(render_shell("Ranking", body, user));
    }

    HttpResponse ranking_fragment(const User& user) {
        if (!can_view_ranking(user)) {
            return error_response(403, "ranking is disabled");
        }
        HttpResponse response;
        response.content_type = "text/html; charset=utf-8";
        response.headers.push_back("Cache-Control: no-store");
        response.body = ranking_table_html().toUtf8();
        return response;
    }

    HttpResponse ignore_submission_post(const HttpRequest& request, const User& user) {
        FormFields form = form_body(request);
        if (form_value(form, "_csrf") != user.csrf) {
            return error_response(403, "bad csrf token");
        }
        bool ok = false;
        int id = form_value(form, "id").toInt(&ok);
        if (!ok || id <= 0) {
            return error_response(400, "bad submission id");
        }
        auto submission = db_.ignore_submission(id);
        if (!submission) {
            return error_response(404, "submission not found");
        }
        try {
            sync_contest_results_for_pair(submission->username, submission->problem);
        } catch (const std::exception& ex) {
            std::cerr << "Failed to update contest results.csv after ignore: "
                      << ex.what() << "\n";
        }
        return redirect_response("/admin");
    }

    fs::path contest_output_path(const fs::path& path) const {
        return path.is_relative() ? config_.contest_root / path : path;
    }

    fs::path contest_source_path(const QString& username, const QString& problem) const {
        return config_.contest_root / settings_.contestants_dir /
               username.toStdString() / (problem.toStdString() + ".cpp");
    }

    void ensure_contestant_folder(const QString& username) const {
        fs::create_directories(config_.contest_root / settings_.contestants_dir /
                               username.toStdString());
    }

    void replace_contest_results_for_pair(const QString& contestant,
                                          const QString& problem,
                                          const std::vector<std::string>& replacement_lines) const {
        fs::path output_path = contest_output_path(settings_.output_csv);
        fs::create_directories(output_path.parent_path());

        std::vector<std::string> kept_lines;
        if (fs::exists(output_path)) {
            std::ifstream in(output_path, std::ios::binary);
            std::string line;
            bool first = true;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (first) {
                    first = false;
                    if (line.rfind("contestant,problem,test,", 0) == 0) {
                        continue;
                    }
                }
                std::vector<std::string> fields = parse_csv_line(line);
                if (fields.size() >= 2 &&
                    fields[0] == contestant.toStdString() &&
                    fields[1] == problem.toStdString()) {
                    continue;
                }
                if (!line.empty()) {
                    kept_lines.push_back(line);
                }
            }
        }

        kept_lines.insert(kept_lines.end(), replacement_lines.begin(), replacement_lines.end());

        fs::path temp_path = output_path;
        temp_path += ".tmp";
        {
            std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
            if (!out) {
                throw std::runtime_error("failed to write " + temp_path.string());
            }
            out << "contestant,problem,test,verdict,time_ms,exit_code,max_points,earned_points,message\n";
            for (const std::string& kept : kept_lines) {
                out << kept << '\n';
            }
        }

        std::error_code ignored;
        fs::remove(output_path, ignored);
        fs::rename(temp_path, output_path);
    }

    void merge_contest_results(const QString& contestant,
                               const QString& problem,
                               const std::vector<neothemis::TestResult>& results) const {
        std::vector<std::string> replacement_lines;
        std::ostringstream generated;
        neothemis::write_csv(generated, results);
        std::istringstream generated_input(generated.str());
        std::string line;
        bool first = true;
        while (std::getline(generated_input, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (first) {
                first = false;
                continue;
            }
            if (!line.empty()) {
                replacement_lines.push_back(line);
            }
        }

        replace_contest_results_for_pair(contestant, problem, replacement_lines);
    }

    std::vector<std::string> csv_lines_for_test_rows(const QString& contestant,
                                                     const QString& problem,
                                                     const std::vector<TestRow>& rows) const {
        std::vector<std::string> lines;
        lines.reserve(rows.size());
        for (const TestRow& row : rows) {
            std::ostringstream out;
            out << csv_escape(contestant.toStdString()) << ','
                << csv_escape(problem.toStdString()) << ','
                << csv_escape(row.test.toStdString()) << ','
                << csv_escape(row.verdict.toStdString()) << ','
                << row.time_ms << ','
                << row.exit_code << ','
                << row.max_points << ','
                << row.earned_points << ','
                << csv_escape(row.message.toStdString());
            lines.push_back(out.str());
        }
        return lines;
    }

    void sync_contest_results_for_pair(const QString& contestant, const QString& problem) {
        std::vector<std::string> replacement_lines;
        std::optional<SubmissionSummary> latest =
            db_.latest_effective_submission(contestant, problem);
        if (latest) {
            replacement_lines = csv_lines_for_test_rows(
                contestant, problem, db_.test_results_for_submission(latest->id));
        }
        replace_contest_results_for_pair(contestant, problem, replacement_lines);
    }

    void start_worker() {
        worker_ = std::thread([this]() { worker_loop(); });
        notify_worker();
    }

    void stop_worker() {
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            stopping_ = true;
        }
        worker_cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void notify_worker() {
        worker_cv_.notify_all();
    }

    void worker_loop() {
        while (true) {
            if (stopping_) {
                return;
            }
            std::optional<SubmissionSummary> submission = db_.take_next_queued();
            if (!submission) {
                std::unique_lock<std::mutex> lock(worker_mutex_);
                worker_cv_.wait_for(lock, std::chrono::seconds(2), [&]() { return stopping_; });
                continue;
            }
            judge_submission(*submission);
        }
    }

    void judge_submission(const SubmissionSummary& submission) {
        try {
            fs::path job_root = config_.data_dir / "jobs" / std::to_string(submission.id);
            fs::path contest = job_root / "contest";
            std::error_code ignored;
            fs::remove_all(job_root, ignored);
            fs::create_directories(contest / "contestants" / submission.username.toStdString());
            fs::create_directories(contest / "tests");

            fs::path original_problem = config_.contest_root / settings_.tests_dir /
                                        submission.problem.toStdString();
            fs::path copied_problem = contest / "tests" / submission.problem.toStdString();
            safe_copy_problem(original_problem, copied_problem);

            fs::copy_file(fs::path(submission.source_path.toStdString()),
                          contest / "contestants" / submission.username.toStdString() /
                              (submission.problem.toStdString() + ".cpp"),
                          fs::copy_options::overwrite_existing);

            neothemis::JudgeOptions options;
            options.contest_root = contest;
            options.contestants_dir = "contestants";
            options.tests_dir = "tests";
            options.core_name = settings_.core_name;
            options.compiler = settings_.compiler;
            options.compile_flags = settings_.compile_flags;
            options.stack_limit_mb = settings_.stack_limit_mb;
            options.parallel_jobs = 1;
            options.keep_workdir = false;
            options.selected_contestants = {submission.username.toStdString()};
            options.selected_problems = {submission.problem.toStdString()};
            options.forbidden_patterns = settings_.forbidden_patterns;

            auto core = neothemis::make_judge_core(options.core_name);
            auto results = core->judge(options);
            if (!db_.submission_ignored(submission.id)) {
                try {
                    merge_contest_results(submission.username, submission.problem, results);
                } catch (const std::exception& ex) {
                    std::cerr << "Failed to update contest results.csv: " << ex.what() << "\n";
                }
            }
            finish_from_results(submission.id, results);
            fs::remove_all(job_root, ignored);
        } catch (const std::exception& ex) {
            db_.finish_submission(submission.id, "failed", "IE", 0.0, ex.what(), {});
        }
    }

    void finish_from_results(int id, const std::vector<neothemis::TestResult>& results) {
        std::vector<TestRow> rows;
        rows.reserve(results.size());
        double score = 0.0;
        double max_score = 0.0;
        std::map<std::string, int> priority{{"IE", 90}, {"CE", 80}, {"SV", 70},
                                            {"TLE", 60}, {"MLE", 55}, {"RE", 50},
                                            {"WA", 40}, {"MS", 30}, {"AC", 0}};
        std::string summary = "AC";
        QString message;
        for (const auto& result : results) {
            std::string verdict = neothemis::to_string(result.verdict);
            score += result.earned_points;
            max_score += result.max_points;
            if (priority[verdict] > priority[summary]) {
                summary = verdict;
            }
            if (message.isEmpty() && !result.message.empty()) {
                message = QString::fromStdString(result.message).left(500);
            }
            rows.push_back(TestRow{QString::fromStdString(result.test),
                                   QString::fromStdString(verdict),
                                   result.time_ms,
                                   result.exit_code,
                                   result.max_points,
                                   result.earned_points,
                                   QString::fromStdString(result.message)});
        }
        if (results.empty()) {
            summary = "IE";
            message = "No test results produced.";
        } else if (summary == "AC" && score + 1e-9 < max_score) {
            summary = "WA";
        }
        if (message.isNull()) {
            message = "";
        }
        db_.finish_submission(id, "done", QString::fromStdString(summary), score, message, rows);
    }

    AppConfig config_;
    ContestSettings settings_;
    Database db_;
    QTcpServer tcp_server_;
    std::thread worker_;
    std::condition_variable worker_cv_;
    std::mutex worker_mutex_;
    bool stopping_ = false;
    bool generated_admin_password_ = false;
    bool generated_join_code_ = false;
};

void HttpConnection::read_available() {
    buffer_ += socket_->readAll();
    if (static_cast<std::size_t>(buffer_.size()) > kMaxHeaderBytes + kMaxBodyBytes) {
        send(error_response(400, "request too large"));
        return;
    }
    try_parse();
}

bool HttpConnection::try_parse() {
    int header_end = buffer_.indexOf("\r\n\r\n");
    if (header_end < 0) {
        if (static_cast<std::size_t>(buffer_.size()) > kMaxHeaderBytes) {
            send(error_response(400, "headers too large"));
        }
        return false;
    }
    QByteArray header_bytes = buffer_.left(header_end);
    QList<QByteArray> lines = header_bytes.split('\n');
    if (lines.isEmpty()) {
        send(error_response(400, "bad request"));
        return false;
    }
    QList<QByteArray> request_line = lines[0].trimmed().split(' ');
    if (request_line.size() < 2) {
        send(error_response(400, "bad request line"));
        return false;
    }
    HttpRequest request;
    request.method = QString::fromLatin1(request_line[0]).toUpper();
    QUrl url(QString::fromUtf8(request_line[1]));
    request.path = url.path();
    request.query = QUrlQuery(url);
    if (request.method != "GET" && request.method != "POST") {
        send(error_response(400, "unsupported method"));
        return false;
    }
    for (int i = 1; i < lines.size(); ++i) {
        QByteArray line = lines[i].trimmed();
        int colon = line.indexOf(':');
        if (colon <= 0) {
            continue;
        }
        request.headers[QString::fromLatin1(line.left(colon)).trimmed()] =
            QString::fromUtf8(line.mid(colon + 1)).trimmed();
    }
    bool ok = false;
    std::size_t content_length =
        header_value(request, "content-length").toULongLong(&ok);
    if (!ok) {
        content_length = 0;
    }
    if (content_length > kMaxBodyBytes) {
        send(error_response(400, "body too large"));
        return false;
    }
    int total = header_end + 4 + static_cast<int>(content_length);
    if (buffer_.size() < total) {
        return false;
    }
    request.body = buffer_.mid(header_end + 4, static_cast<int>(content_length));
    send(app_->handle(request));
    return true;
}

void HttpConnection::send(HttpResponse response) {
    QByteArray body = response.body;
    QByteArray header;
    header += "HTTP/1.1 " + QByteArray::number(response.status) + " " +
              response.reason.toUtf8() + "\r\n";
    header += "Content-Type: " + response.content_type.toUtf8() + "\r\n";
    header += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    header += "Connection: close\r\n";
    header += "X-Content-Type-Options: nosniff\r\n";
    header += "Referrer-Policy: same-origin\r\n";
    header += "Content-Security-Policy: default-src 'self'; style-src 'unsafe-inline'\r\n";
    for (const QString& extra : response.headers) {
        header += extra.toUtf8() + "\r\n";
    }
    header += "\r\n";
    socket_->write(header);
    socket_->write(body);
    socket_->disconnectFromHost();
}

void print_usage() {
    std::cout
        << "Usage: neothemis-server --contest <contest-folder> [options]\n"
        << "Options:\n"
        << "  --host <address>          Bind address. Default: 127.0.0.1\n"
        << "  --port <port>             Bind port. Default: 8080\n"
        << "  --data <folder>           Server data folder. Default: <contest>/.neothemis-server\n"
        << "  --admin-user <name>       Admin username. Default: admin\n"
        << "  --admin-password <pass>   Admin password. Generated on first run if omitted\n"
        << "  --join-code <code>        Contestant registration code. Generated if omitted\n"
        << "  --allow-lan               Required before binding to a non-loopback host\n";
}

AppConfig parse_args(const QStringList& args) {
    AppConfig config;
    for (int i = 1; i < args.size(); ++i) {
        QString arg = args[i];
        auto require_value = [&](const QString& name) {
            if (i + 1 >= args.size()) {
                throw std::runtime_error((name + " requires a value").toStdString());
            }
            return args[++i];
        };
        if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        } else if (arg == "--contest") {
            config.contest_root = require_value(arg).toStdString();
        } else if (arg == "--host") {
            config.host = QHostAddress(require_value(arg));
            if (config.host.isNull()) {
                throw std::runtime_error("invalid host");
            }
        } else if (arg == "--port") {
            bool ok = false;
            int port = require_value(arg).toInt(&ok);
            if (!ok || port <= 0 || port > 65535) {
                throw std::runtime_error("invalid port");
            }
            config.port = static_cast<quint16>(port);
        } else if (arg == "--data") {
            config.data_dir = require_value(arg).toStdString();
        } else if (arg == "--admin-user") {
            config.admin_user = require_value(arg);
            if (!valid_username(config.admin_user)) {
                throw std::runtime_error("admin username must use letters/digits/spaces/_/-");
            }
        } else if (arg == "--admin-password") {
            config.admin_password = require_value(arg);
        } else if (arg == "--join-code") {
            config.join_code = require_value(arg);
        } else if (arg == "--allow-lan") {
            config.allow_lan = true;
        } else {
            throw std::runtime_error(("unknown option: " + arg).toStdString());
        }
    }
    if (config.contest_root.empty()) {
        throw std::runtime_error("--contest is required");
    }
    if (config.admin_password.isEmpty()) {
        config.admin_password = qEnvironmentVariable("NEOTHEMIS_SERVER_ADMIN_PASSWORD");
    }
    if (config.join_code.isEmpty()) {
        config.join_code = qEnvironmentVariable("NEOTHEMIS_SERVER_JOIN_CODE");
    }
    config.contest_root = fs::absolute(config.contest_root);
    if (config.data_dir.empty()) {
        config.data_dir = config.contest_root / ".neothemis-server";
    }
    config.data_dir = fs::absolute(config.data_dir);
    return config;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        AppConfig config = parse_args(app.arguments());
        ContestSettings settings = load_contest_settings(config.contest_root);
        LocalJudgeServer server(std::move(config), std::move(settings));
        server.initialize();
        server.start();
        return app.exec();
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n\n";
        print_usage();
        return 1;
    }
}
