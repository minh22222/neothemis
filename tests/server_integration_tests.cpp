#include "neothemis/server/Database.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void require(bool condition, const QString& message) {
    if (!condition) {
        throw std::runtime_error(message.toStdString());
    }
}

void write_file(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to create " + path.string());
    }
    output << contents;
    if (!output) {
        throw std::runtime_error("failed to write " + path.string());
    }
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to read " + path.string());
    }
    return std::string(std::istreambuf_iterator<char>(input), {});
}

quint16 available_port() {
    QTcpServer probe;
    require(probe.listen(QHostAddress::LocalHost, 0), "failed to reserve a local port");
    return probe.serverPort();
}

QByteArray form_encode(const std::vector<std::pair<QByteArray, QByteArray>>& fields) {
    QByteArray body;
    for (const auto& [name, value] : fields) {
        if (!body.isEmpty()) {
            body += '&';
        }
        body += QUrl::toPercentEncoding(QString::fromUtf8(name));
        body += '=';
        body += QUrl::toPercentEncoding(QString::fromUtf8(value));
    }
    return body;
}

QByteArray http_request(quint16 port,
                        const QByteArray& method,
                        const QByteArray& path,
                        const QByteArray& body = {},
                        const QByteArray& cookie = {}) {
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    require(socket.waitForConnected(3000), "failed to connect to test server");
    QByteArray request = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    if (!cookie.isEmpty()) {
        request += "Cookie: " + cookie + "\r\n";
    }
    if (!body.isEmpty() || method == "POST") {
        request += "Content-Type: application/x-www-form-urlencoded\r\n";
        request += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    }
    request += "Connection: close\r\n\r\n";
    request += body;
    require(socket.write(request) == request.size(), "failed to write HTTP request");
    require(socket.waitForBytesWritten(3000), "timed out writing HTTP request");

    QByteArray response;
    QElapsedTimer timer;
    timer.start();
    while (socket.state() != QAbstractSocket::UnconnectedState && timer.elapsed() < 10000) {
        if (socket.waitForReadyRead(250)) {
            response += socket.readAll();
        }
    }
    response += socket.readAll();
    require(response.startsWith("HTTP/1.1 "), "invalid HTTP response");
    return response;
}

QByteArray response_body(const QByteArray& response) {
    const int separator = response.indexOf("\r\n\r\n");
    return separator < 0 ? QByteArray{} : response.mid(separator + 4);
}

QByteArray response_cookie(const QByteArray& response) {
    QRegularExpression expression("Set-Cookie: (NTSID=[^;\\r\\n]+)",
                                  QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch match = expression.match(QString::fromUtf8(response));
    return match.hasMatch() ? match.captured(1).toUtf8() : QByteArray{};
}

QByteArray csrf_token(const QByteArray& response) {
    QRegularExpression expression("name=\"_csrf\" value=\"([^\"]+)\"");
    QRegularExpressionMatch match = expression.match(QString::fromUtf8(response_body(response)));
    return match.hasMatch() ? match.captured(1).toUtf8() : QByteArray{};
}

void wait_for_health(quint16 port, QProcess& process) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 10000) {
        if (process.state() == QProcess::NotRunning) {
            throw std::runtime_error(
                "server exited during startup: " + process.readAll().toStdString());
        }
        try {
            if (response_body(http_request(port, "GET", "/health")) == "ok\n") {
                return;
            }
        } catch (const std::exception&) {
        }
        QThread::msleep(25);
    }
    throw std::runtime_error("timed out waiting for server health endpoint");
}

void stop_server(QProcess& process) {
    if (process.state() == QProcess::NotRunning) {
        return;
    }
    process.terminate();
    if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(5000);
    }
}

struct SubmissionRow {
    int id = 0;
    QString status;
    QString source_path;
};

std::vector<SubmissionRow> submission_rows(QSqlDatabase& database) {
    QSqlQuery query(database);
    require(query.exec("SELECT id, status, source_path FROM submissions ORDER BY id"),
            query.lastError().text());
    std::vector<SubmissionRow> rows;
    while (query.next()) {
        rows.push_back({query.value(0).toInt(), query.value(1).toString(),
                        query.value(2).toString()});
    }
    return rows;
}

void wait_for_submission_count(QSqlDatabase& database, int count) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000) {
        if (static_cast<int>(submission_rows(database).size()) >= count) {
            return;
        }
        QThread::msleep(10);
    }
    throw std::runtime_error("timed out waiting for submission rows");
}

void wait_until_finished(QSqlDatabase& database, int expected_count) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 30000) {
        auto rows = submission_rows(database);
        int finished = 0;
        for (const SubmissionRow& row : rows) {
            if (row.status == "done" || row.status == "failed") {
                ++finished;
            }
        }
        if (finished >= expected_count) {
            return;
        }
        QThread::msleep(20);
    }
    throw std::runtime_error("timed out waiting for judged submissions");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        require(argc == 2, "server integration test requires the server executable path");
        QTemporaryDir temporary("neothemis-server-integration-XXXXXX");
        require(temporary.isValid(), "failed to create server integration directory");
        const fs::path root = fs::absolute(temporary.path().toStdString());
        const fs::path contest = root / "contest with spaces";
        const fs::path data = root / "server data";
        write_file(contest / "neothemis.conf",
                   "core=builtin\ncontestants_dir=contestants\ntests_dir=tests\n"
                   "output_csv=results.csv\nscoreboard_csv=scoreboard.csv\n"
                   "compiler=" NEOTHEMIS_TEST_COMPILER "\n"
                   "compile_flags=-std=c++14 -O0\nstack_limit_mb=64\nparallel_jobs=1\n"
                   "server_ranking_enabled=true\n");
        write_file(contest / "tests" / "A" / "problem.conf",
                   "time_limit_ms=1000\nmemory_limit_mb=256\ndefault_points=1\nchecker=token\n");
        write_file(contest / "tests" / "A" / "1" / "A.inp", "\n");
        write_file(contest / "tests" / "A" / "1" / "A.out", "1\n");

        const quint16 port = available_port();
        auto start_server = [&]() {
            auto process = std::make_unique<QProcess>();
            process->setProcessChannelMode(QProcess::MergedChannels);
            process->start(QString::fromLocal8Bit(argv[1]),
                           {"--contest", QString::fromStdString(contest.string()),
                            "--data", QString::fromStdString(data.string()),
                            "--port", QString::number(port),
                            "--join-code", "integration-code",
                            "--admin-password", "integration-admin"});
            require(process->waitForStarted(5000), "failed to start server process");
            return process;
        };

        auto server = start_server();
#ifndef Q_OS_LINUX
        require(server->waitForFinished(10000),
                "server did not fail closed on an unsupported sandbox platform");
        require(server->exitCode() != 0, "server unexpectedly ran without secure isolation");
        require(server->readAll().contains("secure sandbox unavailable"),
                "server did not explain its fail-closed sandbox refusal");
        std::cout << "server fail-closed integration test passed\n";
        return 0;
#else
        wait_for_health(port, *server);

        QProcess duplicate_server;
        duplicate_server.setProcessChannelMode(QProcess::MergedChannels);
        duplicate_server.start(
            QString::fromLocal8Bit(argv[1]),
            {"--contest", QString::fromStdString(contest.string()),
             "--data", QString::fromStdString(data.string()),
             "--port", QString::number(available_port()),
             "--join-code", "integration-code",
             "--admin-password", "integration-admin"});
        require(duplicate_server.waitForStarted(5000),
                "failed to start duplicate-server lock probe");
        require(duplicate_server.waitForFinished(10000),
                "second server did not quickly reject the shared data folder");
        require(duplicate_server.exitCode() != 0,
                "second server unexpectedly accepted a shared data folder");
        require(duplicate_server.readAll().contains("already in use"),
                "second server did not explain the data-folder singleton lock");

        QByteArray register_body = form_encode({
            {"join_code", "integration-code"}, {"username", "Test User"},
            {"password", "secret12"}});
        QByteArray registration = http_request(port, "POST", "/register", register_body);
        require(registration.startsWith("HTTP/1.1 303"),
                "registration failed: " + QString::fromUtf8(registration.left(500)));
        QByteArray login_body = form_encode({{"username", "Test User"},
                                             {"password", "secret12"}});
        QByteArray login = http_request(port, "POST", "/login", login_body);
        QByteArray cookie = response_cookie(login);
        require(!cookie.isEmpty(), "login did not set a session cookie");
        QByteArray csrf = csrf_token(http_request(port, "GET", "/submit", {}, cookie));
        require(!csrf.isEmpty(), "submit page did not contain a CSRF token");

        const QByteArray source_v1 =
            "#include <fstream>\nint main(){std::ofstream(\"A.out\")<<1;}\n";
        const QByteArray source_v2 =
            "#include <fstream>\nint main(){std::ofstream(\"A.out\")<<2;}\n";

        const QString connection_name = "neothemis_server_integration";
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection_name);
        database.setDatabaseName(QString::fromStdString((data / "server.db").string()));
        require(database.open(), database.lastError().text());
        QSqlQuery pragma(database);
        require(pragma.exec("PRAGMA busy_timeout=5000"), pragma.lastError().text());
        QSqlQuery user_query(database);
        require(user_query.exec("SELECT id FROM users WHERE username='Test User'"),
                user_query.lastError().text());
        require(user_query.next(), "test user disappeared");
        const int user_id = user_query.value(0).toInt();
        user_query.finish();

        for (int i = 0; i < 3; ++i) {
            QSqlQuery active_fixture(database);
            active_fixture.prepare(
                "INSERT INTO submissions(user_id,username,problem,source_path,status,verdict,"
                "score,message,submitted_at,judged_at) "
                "VALUES(?, 'Test User', 'A', '', 'staging', '', 0, '', 1, 0)");
            active_fixture.addBindValue(user_id);
            require(active_fixture.exec(), active_fixture.lastError().text());
        }
        const QByteArray rejected_body =
            form_encode({{"_csrf", csrf}, {"problem", "A"}, {"source", source_v1}});
        const QByteArray rejected =
            http_request(port, "POST", "/submit", rejected_body, cookie);
        require(rejected.startsWith("HTTP/1.1 429 Too Many Requests"),
                "per-user active limit did not return HTTP 429");
        require(response_body(rejected).contains(
                    "You already have 3 submissions queued or running"),
                "per-user active limit did not return a friendly explanation");
        require(submission_rows(database).size() == 3,
                "rejected submission unexpectedly created a database row");
        require(fs::is_empty(data / "submissions"),
                "rejected submission unexpectedly created a source snapshot");
        QSqlQuery remove_active_fixtures(database);
        remove_active_fixtures.prepare(
            "DELETE FROM submissions WHERE user_id=? AND status='staging'");
        remove_active_fixtures.addBindValue(user_id);
        require(remove_active_fixtures.exec(), remove_active_fixtures.lastError().text());
        require(submission_rows(database).empty(),
                "active-limit fixtures were not removed before judging tests");

        auto submit = [&](const QByteArray& source) {
            QByteArray body = form_encode({{"_csrf", csrf}, {"problem", "A"},
                                           {"source", source}});
            QByteArray response = http_request(port, "POST", "/submit", body, cookie);
            require(response.startsWith("HTTP/1.1 303"), "submission request failed");
        };
        submit(source_v1);
        submit(source_v2);
        for (int i = 0; i < 5; ++i) {
            const QByteArray ranking =
                http_request(port, "GET", "/ranking-fragment", {}, cookie);
            require(ranking.startsWith("HTTP/1.1 200") &&
                        response_body(ranking).contains("<table>"),
                    "ranking read failed while results could be updating");
        }
        wait_for_submission_count(database, 2);
        auto rows = submission_rows(database);
        require(rows[0].source_path != rows[1].source_path,
                "two submissions reused the same source snapshot");
        require(read_file(rows[0].source_path.toStdString()) == source_v1.toStdString(),
                "first submission snapshot was overwritten");
        require(read_file(rows[1].source_path.toStdString()) == source_v2.toStdString(),
                "second submission snapshot contents are wrong");
        require(fs::path(rows[0].source_path.toStdString()).parent_path().filename() ==
                    std::to_string(rows[0].id),
                "first source snapshot is not scoped by submission ID");
        require(fs::path(rows[1].source_path.toStdString()).parent_path().filename() ==
                    std::to_string(rows[1].id),
                "second source snapshot is not scoped by submission ID");

        wait_until_finished(database, 2);
        QThread::msleep(100);
        QElapsedTimer wake_latency;
        wake_latency.start();
        submit(source_v1);
        wait_for_submission_count(database, 3);
        while (wake_latency.elapsed() < 1500) {
            rows = submission_rows(database);
            if (rows.back().status == "running" || rows.back().status == "done" ||
                rows.back().status == "failed") {
                break;
            }
            QThread::msleep(10);
        }
        require(rows.back().status != "queued", "worker notification did not wake the idle queue");
        require(wake_latency.elapsed() < 1000,
                "idle worker wakeup retained the former polling delay");
        wait_until_finished(database, 3);
        const fs::path results_csv = contest / "results.csv";
        require(fs::exists(results_csv), "server did not commit results.csv");
        require(read_file(results_csv).rfind(
                    "contestant,problem,test,verdict,time_ms,exit_code,max_points,"
                    "earned_points,message\n",
                    0) == 0,
                "atomically committed results.csv has an invalid header");
        require(!fs::exists(results_csv.string() + ".tmp"),
                "legacy non-atomic results.csv temporary file was left behind");

        const QByteArray admin_login_body =
            form_encode({{"username", "admin"}, {"password", "integration-admin"}});
        const QByteArray admin_login =
            http_request(port, "POST", "/login", admin_login_body);
        const QByteArray admin_cookie = response_cookie(admin_login);
        require(!admin_cookie.isEmpty(), "administrator login did not set a session cookie");
        const QByteArray admin_csrf =
            csrf_token(http_request(port, "GET", "/submit", {}, admin_cookie));
        require(!admin_csrf.isEmpty(), "administrator session had no CSRF token");

        for (int i = 0; i < 6; ++i) {
            submit(source_v2);
            const int expected_count = 4 + i;
            wait_for_submission_count(database, expected_count);
            const auto current_rows = submission_rows(database);
            const int ignored_id = current_rows.back().id;
            const QByteArray ignore_body = form_encode(
                {{"_csrf", admin_csrf}, {"id", QByteArray::number(ignored_id)}});
            const QByteArray ignored =
                http_request(port, "POST", "/admin/ignore", ignore_body, admin_cookie);
            require(ignored.startsWith("HTTP/1.1 303"),
                    "administrator could not ignore a racing submission");
        }
        wait_until_finished(database, 9);
        QThread::msleep(250);
        const std::string post_ignore_results = read_file(results_csv);
        require(post_ignore_results.find(",AC,") != std::string::npos &&
                    post_ignore_results.find(",WA,") == std::string::npos,
                "a worker/admin race reintroduced an ignored submission score");

        stop_server(*server);
        QSqlQuery second_pair(database);
        second_pair.prepare(
            "INSERT INTO submissions(user_id,username,problem,source_path,status,verdict,"
            "score,message,submitted_at,judged_at) "
            "VALUES(?, 'Test User', 'B', '', 'done', 'AC', 2, '', 1, 1)");
        second_pair.addBindValue(user_id);
        require(second_pair.exec(), second_pair.lastError().text());
        const int second_pair_submission_id = second_pair.lastInsertId().toInt();
        second_pair.finish();
        QSqlQuery second_pair_result(database);
        second_pair_result.prepare(
            "INSERT INTO test_results(submission_id,test,verdict,time_ms,exit_code,max_points,"
            "earned_points,message) VALUES(?, '1', 'AC', 2, 0, 2, 2, "
            "'batched database row')");
        second_pair_result.addBindValue(second_pair_submission_id);
        require(second_pair_result.exec(), second_pair_result.lastError().text());
        QSqlQuery acknowledged_pairs(database);
        require(acknowledged_pairs.exec(
                    "INSERT INTO server_result_pairs(username,problem,generation,pending) VALUES"
                    "('Test User','B',7,0),('Removed User','C',3,0)"),
                acknowledged_pairs.lastError().text());
        QSqlQuery clean_outbox(database);
        require(clean_outbox.exec(
                    "SELECT COUNT(*) FROM server_result_pairs WHERE pending<>0"),
                clean_outbox.lastError().text());
        require(clean_outbox.next() && clean_outbox.value(0).toInt() == 0,
                "clean-restart fixture unexpectedly retained pending outbox work");
        clean_outbox.finish();
        write_file(results_csv,
                   "contestant,problem,test,verdict,time_ms,exit_code,max_points,"
                   "earned_points,message\n"
                   "Test User,A,1,WA,1,0,1,0,stale after clean acknowledgement\n"
                   "Test User,B,1,WA,1,0,2,0,stale second pair\n"
                   "Removed User,C,1,AC,1,0,1,1,stale tombstone pair\n"
                   "Local User,Z,1,AC,1,0,3,3,unrelated local row\n");
        server = start_server();
        wait_for_health(port, *server);
        const std::string clean_restart_results = read_file(results_csv);
        require(clean_restart_results.find("stale after clean acknowledgement") ==
                    std::string::npos &&
                    clean_restart_results.find("Test User,A,1,AC,") != std::string::npos,
                "startup did not rebuild an acknowledged server result pair");
        require(clean_restart_results.find("stale second pair") == std::string::npos &&
                    clean_restart_results.find(
                        "Test User,B,1,AC,2,0,2,2,batched database row") !=
                        std::string::npos,
                "batched startup reconciliation lost a second acknowledged pair");
        require(clean_restart_results.find("Removed User,C,") == std::string::npos,
                "batched startup reconciliation retained a server tombstone pair");
        require(clean_restart_results.find(
                    "Local User,Z,1,AC,1,0,3,3,unrelated local row") !=
                    std::string::npos,
                "batched startup reconciliation discarded an unrelated local pair");
        const std::string reconciled_scoreboard = read_file(contest / "scoreboard.csv");
        require(reconciled_scoreboard.find("Test User,1,2,0,3") != std::string::npos,
                "server reconciliation did not regenerate scoreboard.csv:\n" +
                    QString::fromStdString(reconciled_scoreboard));
        stop_server(*server);

        QSqlQuery staging(database);
        staging.prepare("INSERT INTO submissions(user_id,username,problem,source_path,status,verdict,"
                        "score,message,submitted_at,judged_at) "
                        "VALUES(?, 'Test User', 'A', '', 'staging', '', 0, '', 1, 0)");
        staging.addBindValue(user_id);
        require(staging.exec(), staging.lastError().text());
        const int staging_id = staging.lastInsertId().toInt();
        staging.finish();

        QSqlQuery failed_job(database);
        failed_job.prepare(
            "INSERT INTO submissions(user_id,username,problem,source_path,status,verdict,"
            "score,message,submitted_at,judged_at) "
            "VALUES(?, 'Test User', 'A', ?, 'queued', '', 0, '', 1, 0)");
        failed_job.addBindValue(user_id);
        failed_job.addBindValue(QString::fromStdString(
            (data / "submissions" / "missing" / "A.cpp").string()));
        require(failed_job.exec(), failed_job.lastError().text());
        const int failed_job_id = failed_job.lastInsertId().toInt();
        failed_job.finish();

        QSqlQuery retention_user(database);
        require(retention_user.exec(
                    "INSERT INTO users(username,salt,hash,password,role,created_at) "
                    "VALUES('Retention User','','','retention-password','contestant',1)"),
                retention_user.lastError().text());
        const int retention_user_id = retention_user.lastInsertId().toInt();
        retention_user.finish();

        auto insert_terminal_row = [&](const QString& source_path) {
            QSqlQuery insert(database);
            insert.prepare(
                "INSERT INTO submissions(user_id,username,problem,source_path,status,verdict,"
                "score,message,submitted_at,judged_at) "
                "VALUES(?, 'Retention User', 'A', ?, 'done', 'AC', 1, '', 1, 1)");
            insert.addBindValue(retention_user_id);
            insert.addBindValue(source_path);
            require(insert.exec(), insert.lastError().text());
            return insert.lastInsertId().toInt();
        };
        auto set_source_path = [&](int id, const fs::path& source_path) {
            QSqlQuery update(database);
            update.prepare("UPDATE submissions SET source_path=? WHERE id=?");
            update.addBindValue(QString::fromStdString(source_path.string()));
            update.addBindValue(id);
            require(update.exec(), update.lastError().text());
            require(update.numRowsAffected() == 1, "failed to set retention source path");
        };

        const fs::path outside_snapshot = root / "outside-snapshot.cpp";
        write_file(outside_snapshot, "outside sentinel\n");
        const int outside_id =
            insert_terminal_row(QString::fromStdString(outside_snapshot.string()));

        const fs::path sibling_snapshot = root / "server data sibling" / "source.cpp";
        write_file(sibling_snapshot, "sibling sentinel\n");
        const int sibling_id =
            insert_terminal_row(QString::fromStdString(sibling_snapshot.string()));

        const fs::path symlink_target = root / "symlink-target";
        const fs::path symlink_target_snapshot = symlink_target / "A.cpp";
        write_file(symlink_target_snapshot, "symlink sentinel\n");
        const int symlink_id = insert_terminal_row("");
        const fs::path symlink_parent = data / "submissions" / std::to_string(symlink_id);
        fs::create_directory_symlink(symlink_target, symlink_parent);
        const fs::path symlink_snapshot = symlink_parent / "A.cpp";
        set_source_path(symlink_id, symlink_snapshot);

        std::vector<std::pair<int, fs::path>> retained_fixtures;
        for (int i = 0; i < 22; ++i) {
            const int id = insert_terminal_row("");
            const fs::path source_path =
                data / "submissions" / std::to_string(id) / "A.cpp";
            write_file(source_path, "retention fixture " + std::to_string(i) + "\n");
            set_source_path(id, source_path);
            retained_fixtures.push_back({id, source_path});
        }
        QSqlQuery retained_test_result(database);
        retained_test_result.prepare(
            "INSERT INTO test_results(submission_id,test,verdict,time_ms,exit_code,max_points,"
            "earned_points,message) VALUES(?, '1', 'AC', 1, 0, 1, 1, '')");
        retained_test_result.addBindValue(retained_fixtures.front().first);
        require(retained_test_result.exec(), retained_test_result.lastError().text());

        const fs::path orphan_directory = data / "submissions" / "900001";
        const fs::path orphan_snapshot = orphan_directory / "A.cpp";
        write_file(orphan_snapshot, "deleted-user orphan\n");
        const fs::path orphan_symlink_target = root / "orphan-symlink-target";
        const fs::path orphan_symlink_sentinel = orphan_symlink_target / "A.cpp";
        write_file(orphan_symlink_sentinel, "orphan symlink sentinel\n");
        const fs::path orphan_symlink = data / "submissions" / "900002";
        fs::create_directory_symlink(orphan_symlink_target, orphan_symlink);
        const fs::path unexpected_orphan = data / "submissions" / "900003" / "nested";
        write_file(unexpected_orphan / "keep.txt", "unexpected layout sentinel\n");

        write_file(results_csv,
                   "contestant,problem,test,verdict,time_ms,exit_code,max_points,"
                   "earned_points,message\n"
                   "Test User,A,1,WA,1,0,1,0,simulated stale file\n");

        server = start_server();
        wait_for_health(port, *server);
        const std::string reconciled_results = read_file(results_csv);
        require(reconciled_results.find("simulated stale file") == std::string::npos &&
                    reconciled_results.find("Test User,A,1,AC,") != std::string::npos,
                "startup did not reconcile results.csv from committed database state");
        QElapsedTimer recovery_timer;
        recovery_timer.start();
        QString recovered_status;
        while (recovery_timer.elapsed() < 5000) {
            QSqlQuery recovered(database);
            recovered.prepare("SELECT status FROM submissions WHERE id=?");
            recovered.addBindValue(staging_id);
            require(recovered.exec(), recovered.lastError().text());
            if (recovered.next()) {
                recovered_status = recovered.value(0).toString();
            }
            if (recovered_status == "failed") {
                break;
            }
            QThread::msleep(10);
        }
        require(recovered_status == "failed",
                "stale staging submission was not failed on restart (id=" +
                    QString::number(staging_id) + ", status=" + recovered_status + "):\n" +
                    QString::fromUtf8(server->readAll()));
        QElapsedTimer failed_job_timer;
        failed_job_timer.start();
        QString failed_job_status;
        while (failed_job_timer.elapsed() < 5000) {
            QSqlQuery failed(database);
            failed.prepare("SELECT status FROM submissions WHERE id=?");
            failed.addBindValue(failed_job_id);
            require(failed.exec(), failed.lastError().text());
            if (failed.next()) {
                failed_job_status = failed.value(0).toString();
            }
            if (failed_job_status == "failed") {
                break;
            }
            QThread::msleep(10);
        }
        require(failed_job_status == "failed",
                "worker did not persist a failed mini-contest setup");
        require(!fs::exists(data / "jobs" / std::to_string(failed_job_id)),
                "failed mini-contest setup leaked its per-job work directory");

        auto stored_source_path = [&](int id) {
            QSqlQuery query(database);
            query.prepare("SELECT source_path FROM submissions WHERE id=?");
            query.addBindValue(id);
            require(query.exec(), query.lastError().text());
            require(query.next(), "retention audit row disappeared");
            return query.value(0).toString();
        };
        for (int i = 0; i < 2; ++i) {
            const auto& [id, source_path] = retained_fixtures[static_cast<std::size_t>(i)];
            require(!fs::exists(source_path),
                    "old terminal source snapshot was not pruned at " +
                        QString::fromStdString(source_path.string()) + ": " +
                        QString::fromUtf8(server->readAll()));
            require(stored_source_path(id).isEmpty(),
                    "pruned terminal snapshot path was not cleared from the audit row");
        }
        for (std::size_t i = 2; i < retained_fixtures.size(); ++i) {
            const auto& [id, source_path] = retained_fixtures[i];
            require(fs::exists(source_path), "one of the newest 20 snapshots was pruned");
            require(stored_source_path(id) == QString::fromStdString(source_path.string()),
                    "one of the newest 20 snapshot paths was cleared");
        }
        require(fs::exists(outside_snapshot) &&
                    stored_source_path(outside_id) ==
                        QString::fromStdString(outside_snapshot.string()),
                "retention cleanup deleted or cleared an outside-root snapshot path");
        require(fs::exists(sibling_snapshot) &&
                    stored_source_path(sibling_id) ==
                        QString::fromStdString(sibling_snapshot.string()),
                "retention cleanup accepted a sibling path with a matching string prefix");
        require(fs::exists(symlink_target_snapshot) && fs::is_symlink(symlink_parent) &&
                    stored_source_path(symlink_id) ==
                        QString::fromStdString(symlink_snapshot.string()),
                "retention cleanup followed a submission-directory symlink outside the root");
        require(!fs::exists(orphan_snapshot) && !fs::exists(orphan_directory),
                "startup cleanup retained a strict-layout unreferenced snapshot directory");
        require(fs::exists(orphan_symlink_sentinel) && fs::is_symlink(orphan_symlink),
                "startup orphan cleanup followed a symlink outside the submissions root");
        require(fs::exists(unexpected_orphan / "keep.txt"),
                "startup orphan cleanup deleted an unexpected nested layout");

        QSqlQuery audit_count(database);
        audit_count.prepare("SELECT COUNT(*) FROM submissions WHERE user_id=?");
        audit_count.addBindValue(retention_user_id);
        require(audit_count.exec(), audit_count.lastError().text());
        require(audit_count.next() && audit_count.value(0).toInt() == 25,
                "retention cleanup removed database audit rows");
        QSqlQuery retained_details(database);
        retained_details.prepare("SELECT COUNT(*) FROM test_results WHERE submission_id=?");
        retained_details.addBindValue(retained_fixtures.front().first);
        require(retained_details.exec(), retained_details.lastError().text());
        require(retained_details.next() && retained_details.value(0).toInt() == 1,
                "retention cleanup removed stored test-result audit details");

        neothemis::server::Database account_database(data / "server.db");
        account_database.migrate_schema();
        require(account_database.remove_user("Test User") ==
                    neothemis::server::RemoveUserResult::Removed,
                "live account deletion fixture failed");
        const QByteArray post_delete_ranking =
            http_request(port, "GET", "/ranking-fragment", {}, admin_cookie);
        require(post_delete_ranking.startsWith("HTTP/1.1 200"),
                "ranking failed while draining a deleted-user result tombstone");
        require(read_file(results_csv).find("Test User,A,") == std::string::npos,
                "deleted account left stale server-owned rows in results.csv");
        stop_server(*server);
        database.close();

        const fs::path escaped_jobs = root / "escaped jobs";
        const fs::path escaped_jobs_sentinel = escaped_jobs / "keep.txt";
        write_file(escaped_jobs_sentinel, "keep");
        fs::remove_all(data / "jobs");
        fs::create_directory_symlink(escaped_jobs, data / "jobs");
        auto unsafe_data_server = start_server();
        require(unsafe_data_server->waitForFinished(10000),
                "server did not reject a symlinked jobs directory");
        require(unsafe_data_server->exitCode() != 0 &&
                    unsafe_data_server->readAll().contains(
                        "server jobs path must be a real directory"),
                "server did not explain its unsafe jobs-directory refusal");
        require(fs::exists(escaped_jobs_sentinel),
                "server followed a jobs-directory symlink outside its data root");
        fs::remove(data / "jobs");
        fs::create_directory(data / "jobs");

        const fs::path escaped_lock = root / "escaped-server.lock";
        write_file(escaped_lock, "keep-lock-target");
        std::error_code lock_cleanup_error;
        fs::remove(data / "server.lock", lock_cleanup_error);
        fs::create_symlink(escaped_lock, data / "server.lock");
        auto unsafe_lock_server = start_server();
        require(unsafe_lock_server->waitForFinished(10000),
                "server did not reject a symlinked state file");
        require(unsafe_lock_server->exitCode() != 0 &&
                    unsafe_lock_server->readAll().contains(
                        "server state file server.lock must be a regular file"),
                "server did not explain its unsafe state-file refusal");
        require(read_file(escaped_lock) == "keep-lock-target",
                "server followed a state-file symlink outside its data root");
        fs::remove(data / "server.lock");
        std::cout << "server integration tests passed\n";
        return 0;
#endif
    } catch (const std::exception& ex) {
        std::cerr << "server integration test failed: " << ex.what() << '\n';
        return 1;
    }
}
