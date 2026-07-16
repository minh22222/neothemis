#include "neothemis/server/Database.hpp"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <algorithm>
#include <exception>
#include <future>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

namespace server = neothemis::server;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

server::ManagedUser find_user(const std::vector<server::ManagedUser>& users,
                              const QString& username) {
    const auto found = std::find_if(users.begin(), users.end(),
                                    [&](const auto& user) { return user.username == username; });
    require(found != users.end(), "missing managed user: " + username.toStdString());
    return *found;
}

std::vector<server::SubmissionAdmissionResult> run_concurrent_admissions(
    const std::filesystem::path& database_path,
    const std::vector<server::User>& applicants,
    const server::SubmissionAdmissionLimits& limits) {
    std::promise<void> start_promise;
    const std::shared_future<void> start = start_promise.get_future().share();
    std::vector<std::future<server::SubmissionAdmissionResult>> attempts;
    attempts.reserve(applicants.size());
    for (const server::User& applicant : applicants) {
        attempts.push_back(std::async(std::launch::async, [=]() {
            server::Database concurrent_database(database_path);
            start.wait();
            return concurrent_database.create_submission_draft(
                applicant.id, applicant.username, "A", limits);
        }));
    }
    start_promise.set_value();

    std::vector<server::SubmissionAdmissionResult> results;
    results.reserve(attempts.size());
    for (auto& attempt : attempts) {
        results.push_back(attempt.get());
    }
    return results;
}

int count_status(const std::vector<server::SubmissionAdmissionResult>& results,
                 server::SubmissionAdmissionStatus status) {
    return static_cast<int>(std::count_if(results.begin(), results.end(),
                                          [=](const auto& result) {
                                              return result.status == status;
                                          }));
}

const server::SubmissionSummary&
find_submission(const std::vector<server::SubmissionSummary>& submissions, int id) {
    const auto found = std::find_if(submissions.begin(), submissions.end(),
                                    [=](const auto& submission) { return submission.id == id; });
    require(found != submissions.end(), "missing submission row");
    return *found;
}

const server::AuthoritativeResultPair& find_authoritative_pair(
    const std::vector<server::AuthoritativeResultPair>& pairs, const QString& username,
    const QString& problem) {
    const auto found = std::find_if(pairs.begin(), pairs.end(), [&](const auto& pair) {
        return pair.username == username && pair.problem == problem;
    });
    require(found != pairs.end(),
            "missing authoritative result pair: " + username.toStdString() + "/" +
                problem.toStdString());
    return *found;
}

void test_user_management() {
    QTemporaryDir temporary("neothemis-database-tests-XXXXXX");
    require(temporary.isValid(), "failed to create temporary database directory");

    const std::filesystem::path database_path =
        std::filesystem::path(temporary.path().toStdString()) / "server.db";
    server::Database database(database_path);
    database.migrate_schema();
    require(!database.has_admin(), "new database unexpectedly has an administrator");

    database.create_user("primary", "initial-password", "admin");
    auto primary = database.authenticate("primary", "initial-password");
    require(primary.has_value(), "new administrator could not authenticate");
    const QString primary_session = database.create_session(primary->id);
    require(database.user_for_session(primary_session).has_value(),
            "new administrator session was not stored");

    require(database.change_user_password("primary", "changed-password"),
            "password change did not update an existing user");
    require(!database.change_user_password("missing", "changed-password"),
            "password change reported a missing user as updated");
    require(!database.user_for_session(primary_session).has_value(),
            "password change did not invalidate existing sessions");
    require(!database.authenticate("primary", "initial-password").has_value(),
            "old password remained valid");
    require(database.authenticate("primary", "changed-password").has_value(),
            "new password was not accepted");

    require(database.remove_user("primary") == server::RemoveUserResult::LastAdmin,
            "the final administrator could be removed");
    require(database.authenticate("primary", "changed-password").has_value(),
            "final-admin guard removed the protected user");
    require(database.remove_user("missing") == server::RemoveUserResult::NotFound,
            "missing user removal returned the wrong result");

    database.create_user("backup", "backup-password", "admin");
    database.create_user("contestant", "contestant-password", "contestant");
    const auto contestant = database.authenticate("contestant", "contestant-password");
    require(contestant.has_value(), "contestant could not authenticate");
    const QString contestant_session = database.create_session(contestant->id);

    const auto admission =
        database.create_submission_draft(contestant->id, contestant->username, "A");
    require(admission.accepted(), "ordinary submission draft was rejected");
    const int submission_id = admission.submission_id;
    const auto draft_pairs = database.pending_result_pairs();
    require(draft_pairs.size() == 1 && draft_pairs[0].username == "contestant" &&
                draft_pairs[0].problem == "A" && draft_pairs[0].generation > 0,
            "submission draft did not transactionally enqueue its result pair");
    const std::uint64_t draft_generation = draft_pairs[0].generation;
    database.queue_submission(submission_id, "/tmp/submission.cpp");
    const auto claimed = database.take_next_queued();
    require(claimed.has_value() && claimed->id == submission_id && claimed->status == "running",
            "queued submission was not claimed");

    database.migrate_schema();
    const auto while_running = database.submissions_for_user(contestant->id, false);
    require(find_submission(while_running, submission_id).status == "running",
            "schema migration performed interrupted-job recovery");

    database.finish_submission(submission_id, "done", "AC", 1.0, "",
                               {server::TestRow{"1", "AC", 5, 0, 1.0, 1.0, ""}});
    require(database.test_results_for_submission(submission_id).size() == 1,
            "submission test result was not stored");
    const auto finished_generation =
        database.pending_result_generation("contestant", "A");
    require(finished_generation.has_value() && *finished_generation > draft_generation,
            "submission completion did not advance the result outbox generation");
    require(!database.acknowledge_result_pair("contestant", "A", draft_generation),
            "stale result generation incorrectly acknowledged a newer update");
    require(database.acknowledge_result_pair("contestant", "A", *finished_generation) &&
                !database.pending_result_generation("contestant", "A").has_value(),
            "matching result generation was not acknowledged");
    const auto ignored_finished = database.ignore_submission(submission_id);
    require(ignored_finished.has_value(), "finished submission could not be ignored");
    const auto after_ack_generation =
        database.pending_result_generation("contestant", "A");
    require(after_ack_generation.has_value() &&
                *after_ack_generation > *finished_generation &&
                !database.acknowledge_result_pair("contestant", "A", *finished_generation),
            "an acknowledged result pair reused a generation and accepted a stale acknowledgement");
    require(database.acknowledge_result_pair("contestant", "A", *after_ack_generation),
            "post-acknowledgement generation could not be acknowledged");

    const auto interrupted_admission =
        database.create_submission_draft(contestant->id, contestant->username, "B");
    require(interrupted_admission.accepted(), "interrupted-running fixture was rejected");
    database.queue_submission(interrupted_admission.submission_id, "/tmp/interrupted.cpp");
    const auto interrupted = database.take_next_queued();
    require(interrupted.has_value() && interrupted->id == interrupted_admission.submission_id,
            "interrupted-running fixture was not claimed");
    database.recover_running_submission(interrupted->id);
    const auto retry_claim = database.take_next_queued();
    require(retry_claim.has_value() && retry_claim->id == interrupted->id,
            "targeted worker recovery did not requeue its running submission");
    const auto ignored_running = database.ignore_submission(retry_claim->id);
    require(ignored_running.has_value() && ignored_running->status == "running",
            "running ignore fixture was unexpectedly terminalized before recovery");
    database.recover_interrupted_submissions();
    const auto after_recovery = database.submissions_for_user(contestant->id, false);
    const auto& recovered_ignored = find_submission(after_recovery, interrupted->id);
    require(recovered_ignored.status == "failed" && recovered_ignored.verdict == "IGN",
            "ignored running submission was requeued instead of terminalized on recovery");

    server::SubmissionAdmissionLimits single_active;
    single_active.max_active_per_user = 1;
    single_active.max_active_global = 10;
    single_active.max_recent_per_user = 10;
    const auto ignored_admission = database.create_submission_draft(
        contestant->id, contestant->username, "A", single_active);
    require(ignored_admission.accepted(), "ignored-submission fixture was rejected");
    database.queue_submission(ignored_admission.submission_id, "/tmp/ignored.cpp");
    const auto blocked_by_queued = database.create_submission_draft(
        contestant->id, contestant->username, "A", single_active);
    require(blocked_by_queued.status == server::SubmissionAdmissionStatus::UserActiveLimit,
            "queued submission did not consume the per-user active quota");
    const auto ignored = database.ignore_submission(ignored_admission.submission_id);
    require(ignored.has_value() && ignored->status == "failed" && ignored->verdict == "IGN",
            "ignoring a queued submission did not atomically terminalize it");
    require(!database.take_next_queued().has_value(),
            "ignored queued submission remained claimable");
    const auto after_ignore = database.create_submission_draft(
        contestant->id, contestant->username, "A", single_active);
    require(after_ignore.accepted(), "ignored terminal submission did not release active quota");
    database.finish_submission(after_ignore.submission_id, "failed", "IE", 0.0, "fixture", {});
    const auto ignored_prunable =
        database.terminal_source_snapshots_to_prune(0, contestant->id);
    require(std::any_of(ignored_prunable.begin(), ignored_prunable.end(), [&](const auto& row) {
                return row.submission_id == ignored_admission.submission_id;
            }),
            "ignored queued snapshot was not eligible for terminal retention pruning");

    require(database.remove_user("contestant") == server::RemoveUserResult::Removed,
            "contestant was not removed");
    const auto deletion_generation = database.pending_result_generation("contestant", "A");
    require(deletion_generation.has_value() &&
                database.acknowledge_result_pair("contestant", "A", *deletion_generation),
            "user deletion did not leave an acknowledgeable result-pair tombstone");
    require(!database.submission_owner_id(submission_id).has_value(),
            "user removal did not cascade to submissions");
    require(database.source_snapshot_submission_ids().count(submission_id) == 0,
            "submission-ID inventory retained a cascade-deleted audit row");
    require(database.test_results_for_submission(submission_id).empty(),
            "user removal did not cascade to test results");
    require(!database.user_for_session(contestant_session).has_value(),
            "user removal did not cascade to sessions");

    database.create_user("stale", "stale-password", "contestant");
    const server::UserSyncResult sync = database.sync_contestant_users({"Ada", "backup"}, "123456");
    require(sync.created == 1 && sync.existing == 1 && sync.removed == 1 && sync.skipped == 0,
            "contestant synchronization returned incorrect counts");

    const auto users = database.list_users();
    require(users.size() == 3, "managed user listing returned the wrong number of users");
    require(users[0].username == "backup" && users[0].role == "admin" &&
                users[1].username == "primary" && users[1].role == "admin",
            "managed users were not ordered with administrators first");
    const server::ManagedUser ada = find_user(users, "Ada");
    require(ada.role == "contestant" && ada.password == "123456" && ada.created_at > 0,
            "synchronized contestant fields were not stored");
    require(find_user(users, "backup").role == "admin",
            "sync changed the role of an existing administrator");
    require(std::none_of(users.begin(), users.end(),
                         [](const auto& user) { return user.username == "stale"; }),
            "sync retained a stale contestant");
    require(database.authenticate("Ada", "123456").has_value(),
            "synchronized contestant could not authenticate");

    const server::UserSyncResult repeated =
        database.sync_contestant_users({"Ada", "backup"}, "123456");
    require(repeated.created == 0 && repeated.existing == 2 && repeated.removed == 0,
            "repeated synchronization was not stable");

    std::promise<void> start_removals;
    std::shared_future<void> start = start_removals.get_future().share();
    auto remove_admin = [&](const QString& username) {
        return std::async(std::launch::async, [&, username]() {
            server::Database concurrent_database(database_path);
            start.wait();
            return concurrent_database.remove_user(username);
        });
    };
    auto remove_primary = remove_admin("primary");
    auto remove_backup = remove_admin("backup");
    start_removals.set_value();
    const server::RemoveUserResult primary_result = remove_primary.get();
    const server::RemoveUserResult backup_result = remove_backup.get();
    require((primary_result == server::RemoveUserResult::Removed &&
             backup_result == server::RemoveUserResult::LastAdmin) ||
                (primary_result == server::RemoveUserResult::LastAdmin &&
                 backup_result == server::RemoveUserResult::Removed),
            "concurrent removal did not preserve exactly one administrator");
    require(database.has_admin(), "concurrent removal deleted every administrator");
}

void test_atomic_submission_admission() {
    constexpr int concurrent_applicants = 12;
    {
        std::cerr << "database tests: per-user admission race\n" << std::flush;
        QTemporaryDir temporary("neothemis-user-admission-tests-XXXXXX");
        require(temporary.isValid(), "failed to create per-user admission directory");
        const std::filesystem::path path =
            std::filesystem::path(temporary.path().toStdString()) / "server.db";
        server::Database database(path);
        database.migrate_schema();
        database.create_user("limited", "password", "contestant");
        const server::User user = *database.authenticate("limited", "password");

        server::SubmissionAdmissionLimits limits;
        limits.max_active_per_user = 2;
        limits.max_active_global = 50;
        limits.max_recent_per_user = 50;
        std::vector<server::User> applicants(concurrent_applicants, user);
        const auto results = run_concurrent_admissions(path, applicants, limits);
        require(count_status(results, server::SubmissionAdmissionStatus::Accepted) == 2,
                "concurrent per-user admission accepted more than the limit");
        require(count_status(results, server::SubmissionAdmissionStatus::UserActiveLimit) ==
                    concurrent_applicants - 2,
                "concurrent per-user admission returned incorrect rejection reasons");
        require(database.submissions_for_user(user.id, false).size() == 2,
                "per-user admission persisted the wrong number of rows");
    }

    {
        std::cerr << "database tests: global admission race\n" << std::flush;
        QTemporaryDir temporary("neothemis-global-admission-tests-XXXXXX");
        require(temporary.isValid(), "failed to create global admission directory");
        const std::filesystem::path path =
            std::filesystem::path(temporary.path().toStdString()) / "server.db";
        server::Database database(path);
        database.migrate_schema();
        std::vector<server::User> applicants;
        for (int i = 0; i < concurrent_applicants; ++i) {
            const QString username = "global-" + QString::number(i);
            database.create_user(username, "password", "contestant");
            applicants.push_back(*database.authenticate(username, "password"));
        }

        server::SubmissionAdmissionLimits limits;
        limits.max_active_per_user = 10;
        limits.max_active_global = 3;
        limits.max_recent_per_user = 10;
        const auto results = run_concurrent_admissions(path, applicants, limits);
        require(count_status(results, server::SubmissionAdmissionStatus::Accepted) == 3,
                "concurrent global admission accepted more than the limit");
        require(count_status(results, server::SubmissionAdmissionStatus::GlobalActiveLimit) ==
                    concurrent_applicants - 3,
                "concurrent global admission returned incorrect rejection reasons");
        require(database.submissions_for_user(0, true).size() == 3,
                "global admission persisted the wrong number of rows");
    }

    {
        std::cerr << "database tests: rate admission race\n" << std::flush;
        QTemporaryDir temporary("neothemis-rate-admission-tests-XXXXXX");
        require(temporary.isValid(), "failed to create rate admission directory");
        const std::filesystem::path path =
            std::filesystem::path(temporary.path().toStdString()) / "server.db";
        server::Database database(path);
        database.migrate_schema();
        database.create_user("rate-limited", "password", "contestant");
        const server::User user = *database.authenticate("rate-limited", "password");

        server::SubmissionAdmissionLimits limits;
        limits.max_active_per_user = 50;
        limits.max_active_global = 50;
        limits.max_recent_per_user = 2;
        limits.recent_window_seconds = 60;
        std::vector<server::User> applicants(concurrent_applicants, user);
        const auto results = run_concurrent_admissions(path, applicants, limits);
        require(count_status(results, server::SubmissionAdmissionStatus::Accepted) == 2,
                "concurrent rate admission accepted more than the limit");
        require(count_status(results, server::SubmissionAdmissionStatus::RateLimit) ==
                    concurrent_applicants - 2,
                "concurrent rate admission returned incorrect rejection reasons");
        require(database.submissions_for_user(user.id, false).size() == 2,
                "rate admission persisted the wrong number of rows");

        for (const auto& result : results) {
            if (result.accepted()) {
                database.finish_submission(result.submission_id, "failed", "IE", 0.0,
                                           "terminal rate fixture", {});
                database.ignore_submission(result.submission_id);
            }
        }
        const auto terminal_attempt =
            database.create_submission_draft(user.id, user.username, "A", limits);
        require(terminal_attempt.status == server::SubmissionAdmissionStatus::RateLimit,
                "terminal and ignored submissions did not count toward the recent rate limit");
    }
}

void test_terminal_snapshot_retention_queries() {
    QTemporaryDir temporary("neothemis-retention-tests-XXXXXX");
    require(temporary.isValid(), "failed to create retention test directory");
    const std::filesystem::path path =
        std::filesystem::path(temporary.path().toStdString()) / "server.db";
    server::Database database(path);
    database.migrate_schema();
    database.create_user("retention", "password", "contestant");
    const server::User user = *database.authenticate("retention", "password");

    server::SubmissionAdmissionLimits limits;
    limits.max_active_per_user = 100;
    limits.max_active_global = 100;
    limits.max_recent_per_user = 100;
    std::vector<int> ids;
    for (int i = 0; i < 23; ++i) {
        const auto admission =
            database.create_submission_draft(user.id, user.username, "A", limits);
        require(admission.accepted(), "retention fixture admission was rejected");
        ids.push_back(admission.submission_id);
        const QString source_path = "/tmp/retention-" + QString::number(i) + ".cpp";
        database.queue_submission(admission.submission_id, source_path);
        database.finish_submission(admission.submission_id, "done", "AC", 1.0, "", {});
    }

    const auto candidates = database.terminal_source_snapshots_to_prune(
        server::kRetainedTerminalSourceSnapshotsPerUser, user.id);
    require(candidates.size() == 3,
            "retention query did not select exactly the snapshots beyond the retained 20");
    std::set<int> candidate_ids;
    for (const auto& candidate : candidates) {
        candidate_ids.insert(candidate.submission_id);
    }
    require(candidate_ids == std::set<int>({ids[0], ids[1], ids[2]}),
            "retention query did not select the oldest terminal snapshots");
    require(!database.clear_terminal_source_snapshot(ids[0], "/tmp/wrong.cpp"),
            "retention metadata clear ignored the expected path guard");
    require(database.clear_terminal_source_snapshot(ids[0], "/tmp/retention-0.cpp"),
            "retention metadata clear rejected the matching terminal snapshot");
    const auto referenced_snapshot_ids = database.source_snapshot_submission_ids();
    require(referenced_snapshot_ids.count(ids[0]) == 0 &&
                referenced_snapshot_ids.count(ids[1]) == 1,
            "source snapshot inventory did not follow retained metadata");
    const auto rows = database.submissions_for_user(user.id, false);
    require(rows.size() == 23, "snapshot metadata cleanup deleted database audit rows");
    require(find_submission(rows, ids[0]).source_path.isEmpty(),
            "snapshot metadata was not cleared after pruning");
}

void test_authoritative_result_pair_snapshot() {
    QTemporaryDir temporary("neothemis-authoritative-results-XXXXXX");
    require(temporary.isValid(), "failed to create authoritative result test directory");
    const std::filesystem::path path =
        std::filesystem::path(temporary.path().toStdString()) / "server.db";
    server::Database database(path);
    database.migrate_schema();
    database.create_user("batch-user", "password", "contestant");
    const server::User user = *database.authenticate("batch-user", "password");

    const auto first = database.create_submission_draft(user.id, user.username, "A");
    require(first.accepted(), "first authoritative result fixture was rejected");
    database.queue_submission(first.submission_id, "/tmp/batch-first.cpp");
    database.finish_submission(
        first.submission_id, "done", "PC", 1.5, "first",
        {server::TestRow{"2", "PC", 8, 7, 2.0, 0.5, "partial second test"},
         server::TestRow{"1", "AC", 4, 0, 1.0, 1.0, "first test"}});

    const auto ignored_latest =
        database.create_submission_draft(user.id, user.username, "A");
    require(ignored_latest.accepted(), "ignored latest result fixture was rejected");
    database.queue_submission(ignored_latest.submission_id, "/tmp/batch-ignored.cpp");
    database.finish_submission(
        ignored_latest.submission_id, "done", "AC", 9.0, "ignored",
        {server::TestRow{"ignored", "AC", 1, 0, 9.0, 9.0, "ignored row"}});
    require(database.ignore_submission(ignored_latest.submission_id).has_value(),
            "latest result fixture could not be ignored");

    const auto queued = database.create_submission_draft(user.id, user.username, "B");
    require(queued.accepted(), "empty authoritative result fixture was rejected");
    database.queue_submission(queued.submission_id, "/tmp/batch-queued.cpp");

    database.create_user("deleted-user", "password", "contestant");
    const server::User deleted = *database.authenticate("deleted-user", "password");
    const auto deleted_result =
        database.create_submission_draft(deleted.id, deleted.username, "C");
    require(deleted_result.accepted(), "deleted authoritative result fixture was rejected");
    database.queue_submission(deleted_result.submission_id, "/tmp/batch-deleted.cpp");
    database.finish_submission(
        deleted_result.submission_id, "done", "AC", 1.0, "",
        {server::TestRow{"1", "AC", 1, 0, 1.0, 1.0, ""}});
    require(database.remove_user(deleted.username) == server::RemoveUserResult::Removed,
            "deleted authoritative result fixture user was not removed");

    const auto pairs = database.authoritative_result_pairs();
    const auto& effective = find_authoritative_pair(pairs, "batch-user", "A");
    require(effective.generation > 0 && effective.rows.size() == 2,
            "authoritative snapshot did not return the fallback effective result");
    require(effective.rows[0].test == "1" && effective.rows[0].verdict == "AC" &&
                effective.rows[0].time_ms == 4 && effective.rows[0].exit_code == 0 &&
                effective.rows[0].max_points == 1.0 &&
                effective.rows[0].earned_points == 1.0 &&
                effective.rows[0].message == "first test" &&
                effective.rows[1].test == "2" && effective.rows[1].verdict == "PC" &&
                effective.rows[1].exit_code == 7 &&
                effective.rows[1].earned_points == 0.5 &&
                effective.rows[1].message == "partial second test",
            "authoritative snapshot returned wrong or unsorted test rows");

    const auto& empty = find_authoritative_pair(pairs, "batch-user", "B");
    require(empty.generation > 0 && empty.rows.empty(),
            "queued pair did not remain an empty authoritative snapshot");
    const auto& deleted_pair = find_authoritative_pair(pairs, "deleted-user", "C");
    require(deleted_pair.generation > 0 && deleted_pair.rows.empty(),
            "deleted-user tombstone did not remain an empty authoritative snapshot");
}

void test_connection_lifecycle_and_unicode_paths() {
    QTemporaryDir temporary("neothemis-connection-tests-XXXXXX");
    require(temporary.isValid(), "failed to create connection lifecycle directory");
    const std::filesystem::path unicode_directory =
        std::filesystem::path(temporary.path().toStdString()) /
        std::filesystem::u8path(u8"dữ-liệu-测试");
    const std::filesystem::path path = unicode_directory / "server.db";

    for (int iteration = 0; iteration < 20; ++iteration) {
        std::exception_ptr failure;
        std::thread worker([&]() {
            try {
                server::Database database(path);
                database.migrate_schema();
                (void)database.has_admin();
            } catch (...) {
                failure = std::current_exception();
            }
        });
        worker.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    {
        server::Database database(path);
        database.migrate_schema();
        database.create_user("unicode-path-user", "password", "admin");
        require(database.authenticate("unicode-path-user", "password").has_value(),
                "database at a Unicode path could not be reopened");
    }

    std::error_code remove_error;
    require(std::filesystem::remove(path, remove_error) && !remove_error,
            "database connection handle remained open after Database destruction");

    const std::filesystem::path retry_path =
        std::filesystem::path(temporary.path().toStdString()) / "retry.db";
    std::filesystem::create_directory(retry_path);
    {
        server::Database database(retry_path);
        bool failed_as_expected = false;
        try {
            database.migrate_schema();
        } catch (const std::exception&) {
            failed_as_expected = true;
        }
        require(failed_as_expected, "opening SQLite on a directory unexpectedly succeeded");
        std::filesystem::remove(retry_path);
        database.migrate_schema();
        require(!database.has_admin(),
                "database connection did not recover after a transient open failure");
    }
    remove_error.clear();
    require(std::filesystem::remove(retry_path, remove_error) && !remove_error,
            "retried database connection remained open after destruction");

    const std::filesystem::path shared_path =
        std::filesystem::path(temporary.path().toStdString()) / "shared-worker.db";
    {
        server::Database database(shared_path);
        std::exception_ptr failure;
        std::thread worker([&]() {
            try {
                database.migrate_schema();
            } catch (...) {
                failure = std::current_exception();
            }
        });
        worker.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
        remove_error.clear();
        require(std::filesystem::remove(shared_path, remove_error) && !remove_error,
                "worker-thread connection remained open after the worker exited");
    }
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        std::cerr << "database tests: user management\n" << std::flush;
        test_user_management();
        std::cerr << "database tests: atomic submission admission\n" << std::flush;
        test_atomic_submission_admission();
        std::cerr << "database tests: terminal snapshot retention\n" << std::flush;
        test_terminal_snapshot_retention_queries();
        std::cerr << "database tests: authoritative result snapshot\n" << std::flush;
        test_authoritative_result_pair_snapshot();
        std::cerr << "database tests: connection lifecycle and Unicode paths\n" << std::flush;
        test_connection_lifecycle_and_unicode_paths();
        std::cout << "database tests passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "database tests failed: " << ex.what() << '\n';
        return 1;
    }
}
