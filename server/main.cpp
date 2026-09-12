#include "neothemis/Config.hpp"
#include "neothemis/Csv.hpp"
#include "neothemis/JudgeCore.hpp"
#include "neothemis/server/Database.hpp"
#include "neothemis/server/Http.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QHostAddress>
#include <QLockFile>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QRegularExpression>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxSourceBytes = 256 * 1024;
constexpr neothemis::server::SubmissionAdmissionLimits kSubmissionAdmissionLimits{};

using neothemis::server::Database;
using neothemis::server::AuthoritativeResultPair;
using neothemis::server::ResultPair;
using neothemis::server::SourceSnapshot;
using neothemis::server::SubmissionAdmissionStatus;
using neothemis::server::SubmissionSummary;
using neothemis::server::TestRow;
using neothemis::server::User;
using neothemis::ContestConfig;
using neothemis::server::http::FormFields;
using neothemis::server::http::HttpRequest;
using neothemis::server::http::HttpResponse;
using neothemis::server::http::cookie_value;
using neothemis::server::http::error_response;
using neothemis::server::http::form_body;
using neothemis::server::http::form_value;
using neothemis::server::http::html_escape;
using neothemis::server::http::html_response;
using neothemis::server::http::redirect_response;
using neothemis::server::http::serve_connection;

struct AppConfig {
    fs::path contest_root;
    fs::path data_dir;
    QHostAddress host = QHostAddress::LocalHost;
    quint16 port = 8080;
    bool allow_lan = false;
    bool secure_password_storage = false;
    fs::path tls_certificate;
    fs::path tls_private_key;
    QString admin_user = "admin";
    QString admin_password;
    QString join_code;

    [[nodiscard]] bool tls_enabled() const noexcept {
        return !tls_certificate.empty() && !tls_private_key.empty();
    }
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

QString random_token(std::size_t bytes = 24) {
    QByteArray data;
    data.resize(static_cast<qsizetype>(bytes));
    for (std::size_t i = 0; i < bytes; ++i) {
        data[static_cast<qsizetype>(i)] =
            static_cast<char>(QRandomGenerator::system()->bounded(256));
    }
    return data.toHex();
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

QString qstring_from_path(const fs::path& value) {
#ifdef _WIN32
    return QString::fromStdWString(value.wstring());
#else
    return QString::fromUtf8(value.string());
#endif
}

fs::path path_from_qstring(const QString& value) {
#ifdef _WIN32
    return fs::path(value.toStdWString());
#else
    return fs::path(value.toStdString());
#endif
}

QString now_string(std::int64_t epoch) {
    if (epoch <= 0) {
        return "-";
    }
    return QDateTime::fromSecsSinceEpoch(epoch).toString("yyyy-MM-dd HH:mm:ss");
}

std::vector<QString> list_problems(const fs::path& contest_root, const ContestConfig& settings) {
    std::vector<QString> problems;
    fs::path tests_root = contest_root / settings.tests_dir;
    if (!fs::exists(tests_root)) {
        return problems;
    }
    for (const auto& entry : fs::directory_iterator(tests_root)) {
        if (entry.is_directory()) {
            problems.push_back(qstring_from_path(entry.path().filename()));
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

void write_text_file(const fs::path& path, const std::string& content,
                     bool require_new = false) {
    fs::create_directories(path.parent_path());
    std::error_code exists_error;
    if (require_new && fs::exists(path, exists_error)) {
        throw std::runtime_error("refusing to overwrite immutable file " + path.string());
    }
    if (exists_error) {
        throw std::runtime_error("failed to inspect " + path.string() + ": " +
                                 exists_error.message());
    }
    QSaveFile output(qstring_from_path(path));
    if (!output.open(QIODevice::WriteOnly)) {
        throw std::runtime_error("failed to write " + path.string());
    }
    const qint64 size = static_cast<qint64>(content.size());
    if (output.write(content.data(), size) != size || !output.commit()) {
        throw std::runtime_error("failed to commit " + path.string());
    }
}

class ScopedDirectoryCleanup {
public:
    explicit ScopedDirectoryCleanup(fs::path path) : path_(std::move(path)) {}

    ~ScopedDirectoryCleanup() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    ScopedDirectoryCleanup(const ScopedDirectoryCleanup&) = delete;
    ScopedDirectoryCleanup& operator=(const ScopedDirectoryCleanup&) = delete;

private:
    fs::path path_;
};

bool is_strictly_within(const fs::path& path, const fs::path& root) {
    auto path_part = path.begin();
    for (auto root_part = root.begin(); root_part != root.end(); ++root_part, ++path_part) {
        if (path_part == path.end() || *path_part != *root_part) {
            return false;
        }
    }
    return path_part != path.end();
}

fs::path prepare_server_data_root(const fs::path& configured_root) {
    std::error_code status_error;
    fs::file_status status = fs::symlink_status(configured_root, status_error);
    if (status_error && status_error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error("failed to inspect server data directory: " +
                                 status_error.message());
    }
    if (!status_error && fs::exists(status) &&
        (fs::is_symlink(status) || !fs::is_directory(status))) {
        throw std::runtime_error("server data path must be a real directory");
    }
    if (status_error == std::errc::no_such_file_or_directory || !fs::exists(status)) {
        std::error_code create_error;
        if (!fs::create_directories(configured_root, create_error) || create_error) {
            throw std::runtime_error("failed to create server data directory: " +
                                     create_error.message());
        }
    }

    std::error_code canonical_error;
    const fs::path canonical_root =
        fs::weakly_canonical(configured_root, canonical_error);
    if (canonical_error || !canonical_root.is_absolute()) {
        throw std::runtime_error("failed to resolve server data directory");
    }
    return canonical_root;
}

fs::path prepare_server_state_directory(const fs::path& data_root,
                                        const char* name) {
    const fs::path directory = data_root / name;
    std::error_code status_error;
    fs::file_status status = fs::symlink_status(directory, status_error);
    if (status_error && status_error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error(std::string("failed to inspect server ") + name +
                                 " directory: " + status_error.message());
    }
    if (!status_error && fs::exists(status) &&
        (fs::is_symlink(status) || !fs::is_directory(status))) {
        throw std::runtime_error(std::string("server ") + name +
                                 " path must be a real directory");
    }
    if (status_error == std::errc::no_such_file_or_directory || !fs::exists(status)) {
        std::error_code create_error;
        if (!fs::create_directory(directory, create_error) || create_error) {
            throw std::runtime_error(std::string("failed to create server ") + name +
                                     " directory: " + create_error.message());
        }
    }

    std::error_code canonical_error;
    const fs::path canonical_directory =
        fs::weakly_canonical(directory, canonical_error);
    if (canonical_error || canonical_directory.parent_path() != data_root) {
        throw std::runtime_error(std::string("server ") + name +
                                 " directory escapes the data root");
    }
    return canonical_directory;
}

void validate_server_state_file(const fs::path& data_root, const char* name) {
    const fs::path path = data_root / name;
    std::error_code status_error;
    const fs::file_status status = fs::symlink_status(path, status_error);
    if (status_error == std::errc::no_such_file_or_directory ||
        (!status_error && !fs::exists(status))) {
        return;
    }
    if (status_error) {
        throw std::runtime_error(std::string("failed to inspect server state file ") +
                                 name + ": " + status_error.message());
    }
    if (fs::is_symlink(status) || !fs::is_regular_file(status)) {
        throw std::runtime_error(std::string("server state file ") + name +
                                 " must be a regular file");
    }
}

fs::path create_new_snapshot_directory(const fs::path& submissions_root, int submission_id) {
    fs::create_directories(submissions_root);
    std::error_code root_error;
    const fs::path canonical_root = fs::weakly_canonical(submissions_root, root_error);
    if (root_error || !canonical_root.is_absolute()) {
        throw std::runtime_error("failed to resolve submissions directory");
    }

    const fs::path directory = canonical_root / std::to_string(submission_id);
    std::error_code status_error;
    const fs::file_status status = fs::symlink_status(directory, status_error);
    if (status_error && status_error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error("failed to inspect submission snapshot directory: " +
                                 status_error.message());
    }
    if (!status_error && fs::exists(status)) {
        throw std::runtime_error("submission snapshot directory already exists: " +
                                 directory.string());
    }
    std::error_code create_error;
    if (!fs::create_directory(directory, create_error) || create_error) {
        throw std::runtime_error("failed to create submission snapshot directory: " +
                                 create_error.message());
    }
    return directory;
}

QString status_badge(const QString& value, bool verdict = false) {
    const QString normalized = value.trimmed().toLower();
    QString tone = "badge-muted";
    if (normalized == "done" || normalized == "accepted" || normalized == "ac") {
        tone = "badge-ok";
    } else if (normalized == "running" || normalized == "staging") {
        tone = "badge-run";
    } else if (normalized == "queued" || normalized == "partial" || normalized == "pc") {
        tone = "badge-warn";
    } else if (normalized == "failed" || normalized == "ignored" ||
               normalized == "wa" || normalized == "ce" || normalized == "re" ||
               normalized == "tle" || normalized == "mle" || normalized == "ie" ||
               normalized == "sv" || normalized == "cancelled") {
        tone = "badge-bad";
    } else if (verdict && normalized.isEmpty()) {
        return "<span class=\"badge badge-muted\">Pending</span>";
    }
    const QString label = value.trimmed().isEmpty() ? QString("Pending") : value;
    return "<span class=\"badge " + tone + "\">" + html_escape(label) + "</span>";
}

QString page_shell(const QString& title,
                   const QString& body,
                   const std::optional<User>& user,
                   bool show_ranking) {
    auto nav_link = [&](const QString& label, const QString& href,
                        const QStringList& active_titles = {}) {
        const bool active = active_titles.contains(title);
        return "<a class=\"nav-link" + QString(active ? " active" : "") +
               "\" href=\"" + href + "\">" + label + "</a>";
    };
    QString nav =
        "<nav><div class=\"nav-shell\"><a class=\"brand\" href=\"/\">"
        "<span class=\"brand-mark\"><img src=\"/assets/logo.png\" alt=\"\"></span>"
        "<span class=\"brand-copy\"><strong>NeoThemis</strong>"
        "<small>Contest judge</small></span></a><div class=\"nav-links\">" +
        nav_link("Submit", "/", {"Submit"}) +
        nav_link("Submissions", "/submissions", {"Submissions", "Details"});
    if (show_ranking) {
        nav += nav_link("Ranking", "/ranking", {"Ranking"});
    }
    if (user && user->role == "admin") {
        nav += nav_link("Admin", "/admin", {"Admin"});
    }
    if (user) {
        nav += "<span class=\"nav-user\"><span class=\"nav-avatar\">" +
               html_escape(user->username.left(1).toUpper()) + "</span><span>" +
               html_escape(user->username) + "</span></span>"
               "<a class=\"nav-link nav-logout\" href=\"/logout\">Logout</a>";
    } else {
        nav += nav_link("Login", "/login", {"Login"}) +
               "<a class=\"nav-cta\" href=\"/register\">Register</a>";
    }
    nav += "</div></div></nav>";

    const QString styles = QString::fromUtf8(R"CSS(
        :root {
          color-scheme: dark;
          font-family: Inter, "Segoe UI Variable Text", "Segoe UI", system-ui, sans-serif;
          --bg: #070b12;
          --surface: rgba(14, 22, 32, .88);
          --surface-strong: rgba(17, 27, 39, .96);
          --surface-soft: rgba(255, 255, 255, .035);
          --line: rgba(173, 205, 216, .13);
          --line-strong: rgba(122, 227, 213, .30);
          --text: #eaf1f6;
          --muted: #91a3b0;
          --primary: #5ed8c8;
          --primary-strong: #2d8f85;
          --secondary: #e47792;
          --warning: #ffc76b;
          --danger: #ff8ca4;
          background: var(--bg);
          color: var(--text);
        }
        * { box-sizing: border-box; }
        html { min-height: 100%; background: var(--bg); }
        body {
          margin: 0;
          min-height: 100vh;
          background:
            radial-gradient(circle at 10% -10%, rgba(64, 184, 176, .18), transparent 32rem),
            radial-gradient(circle at 92% 105%, rgba(211, 76, 112, .15), transparent 36rem),
            linear-gradient(145deg, #0a121b 0%, #080d15 48%, #100b14 100%);
          background-attachment: fixed;
          font-size: 14px;
          line-height: 1.55;
        }
        body::before {
          content: "";
          position: fixed;
          inset: 0;
          z-index: 0;
          pointer-events: none;
          opacity: .18;
          background-image:
            linear-gradient(rgba(255,255,255,.026) 1px, transparent 1px),
            linear-gradient(90deg, rgba(255,255,255,.026) 1px, transparent 1px);
          background-size: 48px 48px;
          mask-image: linear-gradient(to bottom, black, transparent 78%);
        }
        a { color: var(--primary); }
        nav {
          position: sticky;
          top: 0;
          z-index: 20;
          border-bottom: 1px solid rgba(255,255,255,.08);
          background: rgba(7, 12, 19, .82);
          backdrop-filter: blur(22px) saturate(140%);
        }
        .nav-shell {
          width: min(1180px, calc(100% - 40px));
          min-height: 70px;
          margin: 0 auto;
          display: flex;
          align-items: center;
          gap: 28px;
        }
        .brand {
          display: flex;
          align-items: center;
          gap: 11px;
          margin-right: auto;
          color: var(--text);
          text-decoration: none;
          white-space: nowrap;
        }
        .brand-mark {
          width: 38px;
          height: 38px;
          display: grid;
          place-items: center;
          border: 1px solid rgba(126, 236, 221, .24);
          border-radius: 12px;
          background: linear-gradient(145deg, rgba(94,216,200,.14), rgba(228,119,146,.10));
          box-shadow: 0 8px 24px rgba(0,0,0,.22);
        }
        .brand img { width: 27px; height: 27px; object-fit: contain; }
        .brand-copy { display: grid; line-height: 1.12; }
        .brand-copy strong { font-size: 15px; letter-spacing: .01em; }
        .brand-copy small { margin-top: 3px; color: var(--muted); font-size: 10px; letter-spacing: .08em; text-transform: uppercase; }
        .nav-links { display: flex; align-items: center; gap: 5px; }
        .nav-link, .nav-cta {
          color: #b9c7d0;
          text-decoration: none;
          font-size: 13px;
          font-weight: 650;
          white-space: nowrap;
          padding: 8px 11px;
          border: 1px solid transparent;
          border-radius: 9px;
          transition: color .16s ease, background .16s ease, border-color .16s ease;
        }
        .nav-link:hover { color: #ecfffb; background: rgba(255,255,255,.045); }
        .nav-link.active { color: #b9fff5; background: rgba(94,216,200,.09); border-color: rgba(94,216,200,.16); }
        .nav-cta { color: #fff; background: #287b74; border-color: rgba(142,247,227,.28); }
        .nav-cta:hover { background: #329087; }
        .nav-user {
          display: flex;
          align-items: center;
          gap: 8px;
          margin-left: 9px;
          padding-left: 14px;
          border-left: 1px solid rgba(255,255,255,.10);
          color: #c7d3da;
          font-size: 13px;
          white-space: nowrap;
        }
        .nav-avatar {
          width: 28px;
          height: 28px;
          display: grid;
          place-items: center;
          border-radius: 9px;
          color: #cafff7;
          background: rgba(94,216,200,.12);
          border: 1px solid rgba(94,216,200,.18);
          font-size: 12px;
          font-weight: 800;
        }
        .nav-logout { color: var(--muted); }
        main {
          position: relative;
          z-index: 1;
          width: min(1180px, calc(100% - 40px));
          margin: 0 auto;
          padding: 38px 0 64px;
        }
        h1 { margin: 0; font-size: clamp(24px, 3vw, 32px); line-height: 1.18; letter-spacing: -.025em; }
        h2 { margin: 0 0 8px; font-size: 17px; }
        p { margin: 10px 0; }
        .page-heading { margin: 0 2px 24px; }
        .eyebrow { margin-bottom: 7px; color: var(--primary); font-size: 11px; font-weight: 800; letter-spacing: .12em; text-transform: uppercase; }
        .page-subtitle { max-width: 680px; margin: 8px 0 0; color: var(--muted); }
        .content-grid { display: grid; grid-template-columns: minmax(0, 1fr) 310px; gap: 18px; align-items: start; }
        .content-grid > .panel { margin: 0; }
        .panel {
          overflow-x: auto;
          margin: 18px 0;
          padding: 26px;
          border: 1px solid var(--line);
          border-radius: 18px;
          background: linear-gradient(145deg, rgba(18,29,41,.91), rgba(11,18,27,.88));
          box-shadow: 0 24px 60px rgba(0,0,0,.24), inset 0 1px rgba(255,255,255,.025);
          backdrop-filter: blur(18px);
        }
        .panel-header { display: flex; align-items: flex-start; justify-content: space-between; gap: 16px; margin-bottom: 22px; }
        .panel-header p { margin: 5px 0 0; color: var(--muted); }
        .notice { overflow: hidden; position: relative; padding: 22px; }
        .notice::before { content: ""; position: absolute; inset: 0 auto 0 0; width: 3px; background: linear-gradient(var(--secondary), var(--warning)); }
        .notice h2 { display: flex; align-items: center; gap: 9px; color: #ffe9ee; }
        .notice h2::before { content: ""; width: 9px; height: 9px; border-radius: 99px; background: var(--secondary); box-shadow: 0 0 0 5px rgba(228,119,146,.11); }
        .notice p { color: #b9aeb6; font-size: 13px; }
        .warn { border-color: rgba(228,119,146,.25); background: linear-gradient(145deg, rgba(58,25,39,.70), rgba(25,18,28,.82)); }
        form { margin: 0; }
        label { display: block; margin: 0 0 7px; color: #cfdae1; font-size: 12px; font-weight: 750; letter-spacing: .015em; }
        input, select, textarea {
          width: 100%;
          margin: 0 0 18px;
          padding: 11px 13px;
          border: 1px solid rgba(178,205,215,.18);
          border-radius: 10px;
          outline: none;
          background: rgba(6,12,19,.74);
          color: #f0f7f8;
          font: inherit;
          transition: border-color .16s ease, box-shadow .16s ease, background .16s ease;
        }
        input:hover, select:hover, textarea:hover { border-color: rgba(122,227,213,.30); }
        input:focus, select:focus, textarea:focus { border-color: rgba(122,227,213,.72); background: rgba(7,15,23,.92); box-shadow: 0 0 0 4px rgba(94,216,200,.09); }
        textarea { min-height: 390px; font-family: "Cascadia Code", "SFMono-Regular", Consolas, monospace; tab-size: 4; }
        .field-help { margin: -10px 0 18px; color: var(--muted); font-size: 12px; }
        .code-editor { position: relative; margin: 0 0 12px; overflow: hidden; border: 1px solid rgba(122,227,213,.22); border-radius: 13px; background: #071019; box-shadow: inset 0 1px 14px rgba(0,0,0,.22); }
        .code-editor:focus-within { border-color: rgba(122,227,213,.68); box-shadow: 0 0 0 4px rgba(94,216,200,.08); }
        .code-editor textarea, .code-editor pre { box-sizing: border-box; width: 100%; min-height: 390px; margin: 0; padding: 17px; font-family: "Cascadia Code", "SFMono-Regular", Consolas, monospace; font-size: 13.5px; line-height: 1.55; tab-size: 4; white-space: pre-wrap; overflow: auto; }
        .code-editor textarea { position: relative; z-index: 2; border: 0; resize: vertical; background: transparent; color: #effbfc; }
        .code-editor pre { position: absolute; inset: 0; z-index: 1; pointer-events: none; color: #dcebed; }
        .code-editor.highlighting textarea { color: transparent; caret-color: #effbfc; }
        .tok-kw { color: #ff8fb3; font-weight: 700; }.tok-type { color: #8ef7e3; }.tok-lit { color: #ffd37a; }.tok-comment { color: #718893; font-style: italic; }.tok-pre { color: #c3a6ff; }
        button {
          min-height: 42px;
          padding: 10px 17px;
          border: 1px solid rgba(142,247,227,.28);
          border-radius: 10px;
          background: #287b74;
          color: white;
          font: inherit;
          font-weight: 750;
          cursor: pointer;
          transition: transform .14s ease, background .14s ease, border-color .14s ease;
        }
        button:hover { transform: translateY(-1px); background: #329087; border-color: rgba(142,247,227,.62); }
        button:active { transform: translateY(0); }
        button.compact { min-height: 32px; padding: 6px 10px; border-color: rgba(255,140,164,.25); background: rgba(126,43,64,.48); font-size: 12px; }
        button.compact:hover { background: rgba(154,50,76,.72); border-color: rgba(255,140,164,.52); }
        .form-actions { display: flex; align-items: center; justify-content: space-between; gap: 14px; margin-top: 6px; }
        .inline-form { margin: 0; }
        table { width: 100%; min-width: 660px; border-spacing: 0; border-collapse: separate; color: #dce6eb; }
        th, td { padding: 13px 14px; text-align: left; vertical-align: middle; border-bottom: 1px solid rgba(255,255,255,.07); }
        th { position: sticky; top: 0; z-index: 1; color: #9dfdec; background: rgba(18,31,41,.98); font-size: 11px; font-weight: 800; letter-spacing: .055em; text-transform: uppercase; }
        th:first-child { border-top-left-radius: 11px; } th:last-child { border-top-right-radius: 11px; }
        tbody tr { transition: background .12s ease; } tbody tr:hover { background: rgba(94,216,200,.045); }
        tbody tr:last-child td { border-bottom: 0; }
        td a { color: #9dfdec; font-weight: 700; text-decoration: none; } td a:hover { text-decoration: underline; }
        .badge, .pill { display: inline-flex; align-items: center; gap: 6px; min-height: 25px; padding: 3px 9px; border: 1px solid rgba(255,255,255,.10); border-radius: 999px; background: rgba(255,255,255,.055); color: #c6d1d8; font-size: 11px; font-weight: 800; white-space: nowrap; text-transform: uppercase; letter-spacing: .035em; }
        .badge::before { content: ""; width: 6px; height: 6px; border-radius: 50%; background: currentColor; opacity: .82; }
        .badge-ok { color: #8ff1c4; background: rgba(53,179,121,.10); border-color: rgba(80,218,151,.18); }
        .badge-run { color: #89e9ff; background: rgba(60,166,209,.10); border-color: rgba(89,206,244,.18); }
        .badge-warn { color: #ffd27d; background: rgba(213,156,53,.10); border-color: rgba(255,196,87,.18); }
        .badge-bad { color: #ff9fb3; background: rgba(211,76,112,.11); border-color: rgba(255,126,157,.20); }
        .badge-muted { color: #9baab4; }
        .rank { display: inline-grid; width: 29px; height: 29px; place-items: center; border-radius: 9px; background: rgba(255,255,255,.05); color: #b8c7cf; font-size: 12px; font-weight: 800; }
        .rank-top { color: #ffe59a; background: rgba(226,179,69,.12); border: 1px solid rgba(255,214,111,.16); }
        .score-total { color: #c6fff7; font-size: 15px; }
        .muted { color: var(--muted); }.err { color: var(--danger); }.ok { color: #8ff1c4; }
        .alert { margin: 0 0 18px; padding: 11px 13px; border: 1px solid rgba(255,140,164,.22); border-radius: 10px; background: rgba(126,43,64,.16); color: #ffb6c5; }
        .empty-state { padding: 38px 18px; text-align: center; color: var(--muted); }
        .auth-main { max-width: 540px; padding-top: 64px; }
        .auth-main .panel { margin: 0; }
        @media (max-width: 900px) {
          .nav-shell { width: min(100% - 24px, 1180px); gap: 12px; }
          .brand-copy small { display: none; }
          .nav-links { overflow-x: auto; padding: 8px 0; scrollbar-width: none; }
          .nav-links::-webkit-scrollbar { display: none; }
          .nav-user { display: none; }
          main { width: min(100% - 24px, 1180px); padding-top: 26px; }
          .content-grid { grid-template-columns: 1fr; }
          .content-grid .notice { order: -1; }
        }
        @media (max-width: 620px) {
          .nav-shell { align-items: flex-start; flex-wrap: wrap; padding: 10px 0 8px; }
          .brand-mark { width: 34px; height: 34px; }.brand img { width: 24px; height: 24px; }
          .nav-links { width: 100%; order: 2; }
          .nav-link, .nav-cta { padding: 7px 9px; }
          main { padding-bottom: 36px; }
          .panel { margin: 12px 0; padding: 19px; border-radius: 15px; }
          .page-heading { margin-bottom: 18px; }
          .panel-header { display: block; margin-bottom: 18px; }
          .form-actions { align-items: stretch; flex-direction: column; }
          button { width: 100%; }
          .code-editor textarea, .code-editor pre { min-height: 340px; padding: 13px; font-size: 12.5px; }
        }
        @media (prefers-reduced-motion: reduce) { *, *::before, *::after { scroll-behavior: auto !important; transition: none !important; } }
    )CSS");
    const QString main_class = !user && (title == "Login" || title == "Register")
                                   ? " class=\"auth-main\""
                                   : QString();
    return "<!doctype html><html><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
           "<meta name=\"theme-color\" content=\"#080d15\">"
           "<link rel=\"icon\" type=\"image/png\" href=\"/assets/logo.png\">"
           "<title>" + html_escape(title) + " · NeoThemis</title><style>" + styles +
           "</style><script src=\"/assets/submit.js\" defer></script></head><body>" + nav +
           "<main" + main_class + ">" + body + "</main></body></html>";
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


class LocalJudgeServer {
public:
    LocalJudgeServer(AppConfig config, ContestConfig settings)
        : config_(std::move(config)),
          settings_(std::move(settings)),
          db_(config_.data_dir / "server.db", config_.secure_password_storage) {}

    ~LocalJudgeServer() {
        stop_worker();
    }

    void initialize() {
        neothemis::JudgeOptions path_validation;
        path_validation.contest_root = config_.contest_root;
        neothemis::apply_contest_config(settings_, path_validation);
        neothemis::validate_judge_paths(path_validation);
        if (!fs::exists(config_.contest_root / settings_.tests_dir)) {
            throw std::runtime_error("contest tests directory not found");
        }
        configure_tls();
        std::string sandbox_reason;
        if (!neothemis::secure_sandbox_available(&sandbox_reason)) {
            throw std::runtime_error("secure sandbox unavailable: " + sandbox_reason);
        }
        config_.data_dir = prepare_server_data_root(config_.data_dir);
        (void)prepare_server_state_directory(config_.data_dir, "submissions");
        (void)prepare_server_state_directory(config_.data_dir, "jobs");
        for (const char* state_file : {"server.db", "server.db-journal", "server.db-wal",
                                       "server.db-shm", "server.lock"}) {
            validate_server_state_file(config_.data_dir, state_file);
        }
        data_lock_ = std::make_unique<QLockFile>(
            qstring_from_path(config_.data_dir / "server.lock"));
        if (!data_lock_->tryLock()) {
            throw std::runtime_error(
                "server data folder is already in use by another NeoThemis server");
        }
        db_.migrate_schema();
        db_.recover_interrupted_submissions();
        prune_terminal_source_snapshots();
        prune_unreferenced_source_snapshots();
        reconcile_contest_results_from_database();
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
        QTcpServer* listener = &tcp_server_;
        if (config_.tls_enabled()) {
            tls_server_.setSslConfiguration(tls_configuration_);
            tls_server_.setHandshakeTimeout(10 * 1000);
            listener = &tls_server_;
        }
        QObject::connect(listener, &QTcpServer::newConnection, [this, listener]() {
            while (QTcpSocket* socket = listener->nextPendingConnection()) {
                socket->setParent(listener);
                serve_connection(socket, [this](const HttpRequest& request) {
                    return handle(request);
                });
            }
        });
        if (!listener->listen(config_.host, config_.port)) {
            throw std::runtime_error(listener->errorString().toStdString());
        }
        start_worker();
        std::cout << "NeoThemis server listening on "
                  << (config_.tls_enabled() ? "https://" : "http://")
                  << listener->serverAddress().toString().toStdString() << ":"
                  << listener->serverPort() << "\n";
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
            if (!config_.tls_enabled()) {
                std::cout << "WARNING: HTTPS is disabled; network traffic is unencrypted.\n";
            }
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
                response.headers.push_back(
                    QStringLiteral("Set-Cookie: NTSID=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict") +
                    (config_.tls_enabled() ? QStringLiteral("; Secure") : QString()));
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
        QString body =
            "<div class=\"page-heading\"><div class=\"eyebrow\">Contest access</div>"
            "<h1>Welcome back</h1><p class=\"page-subtitle\">Sign in to submit code and "
            "follow your judge results.</p></div>"
            "<section class=\"panel\"><div class=\"panel-header\"><div><h2>Sign in</h2>"
            "<p>Use the account provided by your contest administrator.</p></div></div>";
        if (!error.isEmpty()) {
            body += "<p class=\"alert\">" + html_escape(error) + "</p>";
        }
        body += "<form method=\"post\" action=\"/login\">"
                "<label>Username</label><input name=\"username\" autocomplete=\"username\" required>"
                "<label>Password</label><input name=\"password\" type=\"password\" autocomplete=\"current-password\" required>"
                "<div class=\"form-actions\"><span class=\"muted\">New contestant? "
                "<a href=\"/register\">Create an account</a></span>"
                "<button type=\"submit\">Sign in</button></div></form></section>";
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
                                   "; Path=/; HttpOnly; SameSite=Strict" +
                                   (config_.tls_enabled() ? "; Secure" : ""));
        return response;
    }

    HttpResponse register_page(const QString& error, const std::optional<User>& user) {
        QString body =
            "<div class=\"page-heading\"><div class=\"eyebrow\">Contest registration</div>"
            "<h1>Create your contestant account</h1><p class=\"page-subtitle\">Join this "
            "contest using the code shared by an administrator.</p></div>"
            "<section class=\"panel notice warn\"><h2>Security notice</h2>"
            "<p>This server requires supported Linux kernel isolation before it starts. "
            "For untrusted contestants, a dedicated VM or isolated contest machine remains "
            "an additional defense-in-depth boundary.</p></section>"
            "<section class=\"panel\"><div class=\"panel-header\"><div>"
            "<h2>Account details</h2><p>Your username becomes your contestant ID.</p>"
            "</div></div>";
        if (!error.isEmpty()) {
            body += "<p class=\"alert\">" + html_escape(error) + "</p>";
        }
        body += "<form method=\"post\" action=\"/register\">"
                "<label>Join code</label><input name=\"join_code\" required>"
                "<label>Username</label><input name=\"username\" autocomplete=\"username\" required>"
                "<p class=\"field-help\">Use letters, digits, spaces, underscore, or dash.</p>"
                "<label>Password</label><input name=\"password\" type=\"password\" autocomplete=\"new-password\" required>"
                "<div class=\"form-actions\"><span class=\"muted\">Already registered? "
                "<a href=\"/login\">Sign in</a></span>"
                "<button type=\"submit\">Create account</button></div></form></section>";
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
            ensure_contestant_folder(username);
            db_.create_user(username, password, "contestant");
        } catch (const std::exception&) {
            return register_page("Username already exists.", std::nullopt);
        }
        return redirect_response("/login");
    }

    HttpResponse submit_page(const QString& error, const User& user) {
        std::vector<QString> problems = list_problems(config_.contest_root, settings_);
        QString body =
            "<div class=\"page-heading\"><div class=\"eyebrow\">Submission workspace</div>"
            "<h1>Submit C++ source</h1><p class=\"page-subtitle\">Choose a problem, paste "
            "your solution, and follow its progress from the submissions page.</p></div>"
            "<div class=\"content-grid\"><section class=\"panel\">"
            "<div class=\"panel-header\"><div><h2>New submission</h2>"
            "<p>Your latest accepted source is also saved to your contestant folder.</p></div>"
            "</div>";
        if (!error.isEmpty()) {
            body += "<p class=\"alert\">" + html_escape(error) + "</p>";
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
                "<div class=\"form-actions\"><span class=\"muted\">Maximum source size: " +
                QString::number(kMaxSourceBytes / 1024) + " KiB</span>"
                "<button type=\"submit\">Submit solution</button></div></form></section>"
                "<aside class=\"panel notice warn\"><h2>Local judge security</h2>"
            "<p>Submissions run under mandatory Linux namespace, capability, resource, and "
            "system-call isolation. A dedicated VM or isolated contest machine is still "
            "recommended as an additional defense-in-depth boundary.</p>"
            "<p class=\"muted\">Never submit secrets or credentials in source code.</p>"
            "</aside></div>";
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

        const auto admission = db_.create_submission_draft(
            user.id, user.username, problem, kSubmissionAdmissionLimits);
        if (!admission.accepted()) {
            HttpResponse response;
            switch (admission.status) {
            case SubmissionAdmissionStatus::UserActiveLimit:
                response = submit_page(
                    "You already have " +
                        QString::number(kSubmissionAdmissionLimits.max_active_per_user) +
                        " submissions queued or running. Wait for one to finish.",
                    user);
                response.status = 429;
                response.reason = "Too Many Requests";
                response.headers.push_back("Retry-After: 1");
                return response;
            case SubmissionAdmissionStatus::GlobalActiveLimit:
                response = submit_page(
                    "The judge queue is full (" +
                        QString::number(kSubmissionAdmissionLimits.max_active_global) +
                        " active submissions). Please try again shortly.",
                    user);
                response.status = 503;
                response.reason = "Service Unavailable";
                response.headers.push_back("Retry-After: 1");
                return response;
            case SubmissionAdmissionStatus::RateLimit:
                response = submit_page(
                    "Submission rate limit reached (" +
                        QString::number(kSubmissionAdmissionLimits.max_recent_per_user) +
                        " per " +
                        QString::number(kSubmissionAdmissionLimits.recent_window_seconds) +
                        " seconds). Please wait and try again.",
                    user);
                response.status = 429;
                response.reason = "Too Many Requests";
                response.headers.push_back(
                    "Retry-After: " +
                    QString::number(kSubmissionAdmissionLimits.recent_window_seconds));
                return response;
            case SubmissionAdmissionStatus::Accepted:
                break;
            }
            throw std::runtime_error("unknown submission admission result");
        }
        const int id = admission.submission_id;
        fs::path source_path;
        try {
            const fs::path snapshot_directory = create_new_snapshot_directory(
                config_.data_dir / "submissions", id);
            source_path = snapshot_directory / (problem.toStdString() + ".cpp");
            write_text_file(source_path, source.toStdString(), true);
            write_text_file(contest_source_path(user.username, problem), source.toStdString());
            db_.queue_submission(id, qstring_from_path(source_path));
        } catch (const std::exception& ex) {
            db_.finish_submission(id, "failed", "IE", 0.0,
                                  QString::fromUtf8(ex.what()), {});
            std::error_code cleanup_error;
            if (!source_path.empty()) {
                fs::remove(source_path, cleanup_error);
                cleanup_error.clear();
                fs::remove(source_path.parent_path(), cleanup_error);
            }
            prune_terminal_source_snapshots(user.id);
            throw;
        }
        notify_worker();
        return redirect_response("/submissions");
    }

    HttpResponse submissions_page(const User& user, bool admin) {
        auto submissions = db_.submissions_for_user(user.id, admin);
        const QString heading = admin ? "All submissions" : "My submissions";
        QString body =
            "<div class=\"page-heading\"><div class=\"eyebrow\">Judge history</div><h1>" +
            heading + "</h1><p class=\"page-subtitle\">Review queue state, verdicts, scores, "
            "and judge messages.</p></div>"
            "<section class=\"panel\"><div class=\"panel-header\"><div><h2>Submission log</h2>"
            "<p>Newest submissions appear first.</p></div><span class=\"pill\">" +
            QString::number(submissions.size()) + " total</span></div>"
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
                    status_badge(status) + "</td><td>" + status_badge(row.verdict, true) +
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
        if (submissions.empty()) {
            body += "<tr><td colspan=\"" + QString::number(admin ? 10 : 9) +
                    "\" class=\"empty-state\">No submissions yet.</td></tr>";
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
        QString body =
            "<div class=\"page-heading\"><div class=\"eyebrow\">Judge details</div>"
            "<h1>Submission #" + QString::number(id) +
            "</h1><p class=\"page-subtitle\">Per-test timing, points, and checker messages.</p>"
            "</div><section class=\"panel\"><div class=\"panel-header\"><div>"
            "<h2>Test results</h2><p>CPU time is shown in milliseconds.</p></div>"
            "<span class=\"pill\">" + QString::number(rows.size()) + " tests</span></div>"
                       "<table><thead><tr><th>Test</th><th>Verdict</th><th>Time</th>"
                       "<th>Exit</th><th>Points</th><th>Message</th></tr></thead><tbody>";
        for (const auto& row : rows) {
            body += "<tr><td>" + html_escape(row.test) + "</td><td>" +
                    status_badge(row.verdict, true) + "</td><td>" +
                    QString::number(row.time_ms) + " ms</td><td>" +
                    QString::number(row.exit_code) + "</td><td>" +
                    QString::number(row.earned_points, 'f', 2) + "/" +
                    QString::number(row.max_points, 'f', 2) + "</td><td>" +
                    html_escape(row.message) + "</td></tr>";
        }
        if (rows.empty()) {
            body += "<tr><td colspan=\"6\" class=\"empty-state\">No test details are available.</td></tr>";
        }
        body += "</tbody></table></section>";
        return html_response(render_shell("Details", body, user));
    }

    RankingTable ranking_table() {
        // Completion, ignore, recovery, and GUI account deletion enqueue a
        // transactional CSV update in SQLite. Drain it before every ranking so
        // cross-process changes and transient write failures converge without
        // requiring a restart.
        reconcile_pending_contest_results();
        RankingTable table;
        table.problems = list_problems(config_.contest_root, settings_);

        std::map<QString, std::size_t> row_by_username;
        for (const QString& username : db_.contestant_usernames()) {
            row_by_username[username] = table.rows.size();
            table.rows.push_back(RankingRow{username, {}, 0.0});
        }

        std::set<QString> known_problems(table.problems.begin(), table.problems.end());
        fs::path output_path = contest_output_path(settings_.output_csv);
        neothemis::CsvTable records;
        {
            std::lock_guard<std::mutex> results_lock(contest_results_mutex_);
            if (fs::exists(output_path)) {
                records = neothemis::read_csv_file_locked(output_path);
            }
        }
        for (std::size_t index = 0; index < records.size(); ++index) {
            const neothemis::CsvRow& fields = records[index];
            if (index == 0 && !fields.empty() && fields[0] == "contestant") {
                continue;
            }
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
            const int current_rank = rank++;
            body += "<tr><td><span class=\"rank" +
                    QString(current_rank <= 3 ? " rank-top" : "") + "\">" +
                    QString::number(current_rank) + "</span></td><td><strong>" +
                    html_escape(row.username) + "</strong></td>";
            for (const QString& problem : table.problems) {
                auto score = row.problem_scores.find(problem);
                body += "<td>" + QString::number(
                    score == row.problem_scores.end() ? 0.0 : score->second, 'f', 2) + "</td>";
            }
            body += "<td><strong class=\"score-total\">" +
                    QString::number(row.total, 'f', 2) + "</strong></td></tr>";
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
        QString body =
                       "<div class=\"page-heading\"><div class=\"eyebrow\">Live standings</div>"
                       "<h1>Contest ranking</h1><p class=\"page-subtitle\">Scores update "
                       "automatically as judging completes.</p></div>"
                       "<section class=\"panel\"><div class=\"panel-header\"><div>"
                       "<h2>Scoreboard</h2><p>Higher total scores rank first.</p></div>"
                       "<span class=\"badge badge-run\">Live</span></div>"
                       "<div id=\"ranking-table\">" +
                       ranking_table_html() + "</div>"
                       "</section>";
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
        prune_terminal_source_snapshots(submission->user_id);
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

    fs::path safe_contestant_folder(const QString& username) const {
        if (!valid_username(username)) {
            throw std::runtime_error("unsafe contestant name");
        }
        const fs::path configured_root =
            config_.contest_root / settings_.contestants_dir;
        fs::create_directories(configured_root);
        std::error_code root_error;
        const fs::path canonical_root = fs::weakly_canonical(configured_root, root_error);
        if (root_error || !canonical_root.is_absolute()) {
            throw std::runtime_error("failed to resolve contestants directory");
        }

        const fs::path candidate = canonical_root / username.toStdString();
        std::error_code status_error;
        fs::file_status status = fs::symlink_status(candidate, status_error);
        if (status_error && status_error != std::errc::no_such_file_or_directory) {
            throw std::runtime_error("failed to inspect contestant directory: " +
                                     status_error.message());
        }
        if (status_error == std::errc::no_such_file_or_directory || !fs::exists(status)) {
            std::error_code create_error;
            if (!fs::create_directory(candidate, create_error) || create_error) {
                throw std::runtime_error("failed to create contestant directory: " +
                                         create_error.message());
            }
            status_error.clear();
            status = fs::symlink_status(candidate, status_error);
        }
        if (status_error || fs::is_symlink(status) || !fs::is_directory(status)) {
            throw std::runtime_error("contestant path is not a safe directory");
        }

        std::error_code candidate_error;
        const fs::path canonical_candidate =
            fs::weakly_canonical(candidate, candidate_error);
        if (candidate_error || canonical_candidate.parent_path() != canonical_root) {
            throw std::runtime_error("contestant directory escapes the contest");
        }
        return canonical_candidate;
    }

    fs::path contest_source_path(const QString& username, const QString& problem) const {
        if (!valid_identifier(problem)) {
            throw std::runtime_error("unsafe problem name");
        }
        const fs::path path =
            safe_contestant_folder(username) / (problem.toStdString() + ".cpp");
        std::error_code status_error;
        const fs::file_status status = fs::symlink_status(path, status_error);
        if (status_error && status_error != std::errc::no_such_file_or_directory) {
            throw std::runtime_error("failed to inspect contestant source: " +
                                     status_error.message());
        }
        if (!status_error && fs::exists(status) &&
            (fs::is_symlink(status) || !fs::is_regular_file(status))) {
            throw std::runtime_error("contestant source path is not a safe regular file");
        }
        return path;
    }

    void ensure_contestant_folder(const QString& username) const {
        (void)safe_contestant_folder(username);
    }

    void replace_contest_results_locked(
        const neothemis::CsvTable& replacement_rows,
        const std::set<neothemis::CsvKey>& replaced_pairs) const {
        fs::path output_path = contest_output_path(settings_.output_csv);
        neothemis::replace_csv_rows_by_key_atomic(
            output_path,
            {"contestant", "problem", "test", "verdict", "time_ms", "exit_code",
             "max_points", "earned_points", "message"},
            replacement_rows, replaced_pairs,
            [&](const neothemis::CsvTable& merged_rows) {
                std::ostringstream scoreboard;
                neothemis::write_scoreboard_csv_from_results(scoreboard, merged_rows);
                neothemis::write_text_file_atomic(
                    contest_output_path(settings_.scoreboard_csv), scoreboard.str());
            });
    }

    neothemis::CsvTable csv_rows_for_test_rows(const QString& contestant,
                                               const QString& problem,
                                               const std::vector<TestRow>& rows) const {
        neothemis::CsvTable csv_rows;
        csv_rows.reserve(rows.size());
        for (const TestRow& row : rows) {
            std::ostringstream max_points;
            std::ostringstream earned_points;
            max_points << row.max_points;
            earned_points << row.earned_points;
            csv_rows.push_back({contestant.toStdString(),
                                problem.toStdString(),
                                row.test.toStdString(),
                                row.verdict.toStdString(),
                                std::to_string(row.time_ms),
                                std::to_string(row.exit_code),
                                max_points.str(),
                                earned_points.str(),
                                row.message.toStdString()});
        }
        return csv_rows;
    }

    void sync_contest_results_for_pair(
        const QString& contestant, const QString& problem,
        std::optional<std::uint64_t> expected_generation = std::nullopt) {
        std::lock_guard<std::mutex> results_lock(contest_results_mutex_);
        if (!expected_generation) {
            expected_generation = db_.pending_result_generation(contestant, problem);
        }
        if (!expected_generation) {
            return;
        }
        neothemis::CsvTable replacement_rows;
        std::optional<SubmissionSummary> latest =
            db_.latest_effective_submission(contestant, problem);
        if (latest) {
            replacement_rows = csv_rows_for_test_rows(
                contestant, problem, db_.test_results_for_submission(latest->id));
        }
        replace_contest_results_locked(
            replacement_rows, {{contestant.toStdString(), problem.toStdString()}});
        // Generation-checked acknowledgement makes the database mutation and
        // filesystem replacement an idempotent outbox: a concurrent newer
        // mutation leaves its incremented generation pending.
        db_.acknowledge_result_pair(contestant, problem, *expected_generation);
    }

    void reconcile_contest_results_from_database() {
        // Rebuild every server-owned pair, not only pending outbox rows. This
        // repairs a deleted/corrupted CSV or a same-pair GUI overwrite even
        // after the prior generation was successfully acknowledged.
        const std::vector<AuthoritativeResultPair> snapshots =
            db_.authoritative_result_pairs();
        if (!snapshots.empty()) {
            neothemis::CsvTable replacement_rows;
            std::set<neothemis::CsvKey> replaced_pairs;
            for (const AuthoritativeResultPair& snapshot : snapshots) {
                neothemis::CsvTable pair_rows = csv_rows_for_test_rows(
                    snapshot.username, snapshot.problem, snapshot.rows);
                replacement_rows.insert(replacement_rows.end(),
                                        std::make_move_iterator(pair_rows.begin()),
                                        std::make_move_iterator(pair_rows.end()));
                replaced_pairs.insert(
                    {snapshot.username.toStdString(), snapshot.problem.toStdString()});
            }
            {
                std::lock_guard<std::mutex> results_lock(contest_results_mutex_);
                replace_contest_results_locked(replacement_rows, replaced_pairs);
            }
            // A newer mutation increments its pair generation, so these
            // snapshot acknowledgements cannot clear newer pending work.
            for (const AuthoritativeResultPair& snapshot : snapshots) {
                db_.acknowledge_result_pair(snapshot.username, snapshot.problem,
                                            snapshot.generation);
            }
        }
        reconcile_pending_contest_results();
    }

    void reconcile_pending_contest_results() {
        for (const ResultPair& pair : db_.pending_result_pairs()) {
            sync_contest_results_for_pair(pair.username, pair.problem, pair.generation);
        }
    }

    void prune_terminal_source_snapshots(std::optional<int> user_id = std::nullopt) {
        try {
            const fs::path submissions_root = config_.data_dir / "submissions";
            std::error_code root_error;
            const fs::path canonical_root = fs::weakly_canonical(submissions_root, root_error);
            if (root_error || !canonical_root.is_absolute()) {
                std::cerr << "Skipping source snapshot pruning: submissions root could not be "
                             "canonicalized\n";
                return;
            }

            const auto snapshots = db_.terminal_source_snapshots_to_prune(
                neothemis::server::kRetainedTerminalSourceSnapshotsPerUser, user_id);
            for (const SourceSnapshot& snapshot : snapshots) {
                try {
                    const fs::path stored_path(snapshot.source_path.toStdString());
                    if (!stored_path.is_absolute()) {
                        std::cerr << "Refusing to prune non-absolute source snapshot for submission "
                                  << snapshot.submission_id << "\n";
                        continue;
                    }

                    std::error_code candidate_error;
                    const fs::path canonical_candidate =
                        fs::weakly_canonical(stored_path, candidate_error);
                    const fs::path expected_parent =
                        canonical_root / std::to_string(snapshot.submission_id);
                    if (candidate_error || !is_strictly_within(canonical_candidate,
                                                                canonical_root) ||
                        canonical_candidate.parent_path() != expected_parent) {
                        std::cerr << "Refusing to prune source snapshot outside its canonical "
                                     "submission directory for submission "
                                  << snapshot.submission_id << "\n";
                        continue;
                    }

                    std::error_code status_error;
                    const fs::file_status status = fs::symlink_status(stored_path, status_error);
                    if (status_error == std::errc::no_such_file_or_directory ||
                        (!status_error && !fs::exists(status))) {
                        db_.clear_terminal_source_snapshot(snapshot.submission_id,
                                                           snapshot.source_path);
                        continue;
                    }
                    if (status_error || !fs::is_regular_file(status)) {
                        std::cerr << "Refusing to prune non-regular source snapshot for submission "
                                  << snapshot.submission_id << "\n";
                        continue;
                    }

                    std::error_code remove_error;
                    if (!fs::remove(stored_path, remove_error) || remove_error) {
                        std::cerr << "Failed to prune source snapshot for submission "
                                  << snapshot.submission_id << ": "
                                  << remove_error.message() << "\n";
                        continue;
                    }
                    db_.clear_terminal_source_snapshot(snapshot.submission_id,
                                                       snapshot.source_path);

                    std::error_code directory_error;
                    fs::remove(stored_path.parent_path(), directory_error);
                } catch (const std::exception& ex) {
                    std::cerr << "Failed to prune source snapshot for submission "
                              << snapshot.submission_id << ": " << ex.what() << "\n";
                }
            }
        } catch (const std::exception& ex) {
            std::cerr << "Failed to identify source snapshots for pruning: " << ex.what()
                      << "\n";
        }
    }

    void prune_unreferenced_source_snapshots() {
        try {
            const fs::path submissions_root = config_.data_dir / "submissions";
            std::error_code root_error;
            const fs::path canonical_root = fs::weakly_canonical(submissions_root, root_error);
            if (root_error || !canonical_root.is_absolute()) {
                std::cerr << "Skipping orphan snapshot pruning: submissions root could not be "
                             "canonicalized\n";
                return;
            }
            const std::set<int> referenced_ids = db_.source_snapshot_submission_ids();

            std::error_code iteration_error;
            fs::directory_iterator directory(submissions_root, iteration_error);
            const fs::directory_iterator end;
            while (!iteration_error && directory != end) {
                const fs::path candidate_directory = directory->path();
                directory.increment(iteration_error);

                bool id_ok = false;
                const QString id_text = qstring_from_path(candidate_directory.filename());
                const int submission_id = id_text.toInt(&id_ok);
                if (!id_ok || submission_id <= 0 ||
                    id_text != QString::number(submission_id) ||
                    referenced_ids.count(submission_id) != 0) {
                    continue;
                }

                std::error_code status_error;
                const fs::file_status directory_status =
                    fs::symlink_status(candidate_directory, status_error);
                std::error_code canonical_error;
                const fs::path canonical_directory =
                    fs::weakly_canonical(candidate_directory, canonical_error);
                if (status_error || !fs::is_directory(directory_status) || canonical_error ||
                    !is_strictly_within(canonical_directory, canonical_root) ||
                    canonical_directory.parent_path() != canonical_root) {
                    std::cerr << "Refusing to prune unsafe orphan snapshot directory "
                              << candidate_directory << "\n";
                    continue;
                }

                std::vector<fs::path> files;
                bool safe_layout = true;
                std::error_code contents_error;
                fs::directory_iterator child(candidate_directory, contents_error);
                while (!contents_error && child != end) {
                    const fs::path child_path = child->path();
                    child.increment(contents_error);
                    std::error_code child_status_error;
                    const fs::file_status child_status =
                        fs::symlink_status(child_path, child_status_error);
                    std::error_code child_canonical_error;
                    const fs::path canonical_child =
                        fs::weakly_canonical(child_path, child_canonical_error);
                    if (child_status_error || !fs::is_regular_file(child_status) ||
                        child_canonical_error ||
                        canonical_child.parent_path() != canonical_directory ||
                        child_path.extension() != ".cpp" ||
                        !valid_identifier(QString::fromStdString(
                            child_path.stem().string()))) {
                        safe_layout = false;
                        break;
                    }
                    files.push_back(child_path);
                    if (files.size() > 1) {
                        safe_layout = false;
                        break;
                    }
                }
                if (contents_error || !safe_layout) {
                    std::cerr << "Refusing to prune orphan snapshot with unexpected contents "
                              << candidate_directory << "\n";
                    continue;
                }

                bool file_removed = true;
                if (!files.empty()) {
                    std::error_code remove_error;
                    file_removed = fs::remove(files.front(), remove_error) && !remove_error;
                }
                if (!file_removed) {
                    std::cerr << "Failed to prune orphan source snapshot in "
                              << candidate_directory << "\n";
                    continue;
                }
                std::error_code directory_error;
                if (!fs::remove(candidate_directory, directory_error) || directory_error) {
                    std::cerr << "Failed to remove orphan source snapshot directory "
                              << candidate_directory << "\n";
                }
            }
            if (iteration_error) {
                std::cerr << "Failed while scanning orphan source snapshots: "
                          << iteration_error.message() << "\n";
            }
        } catch (const std::exception& ex) {
            std::cerr << "Failed to prune orphan source snapshots: " << ex.what() << "\n";
        }
    }

    void start_worker() {
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            stopping_.store(false, std::memory_order_relaxed);
            work_pending_ = true;
        }
        worker_ = std::thread([this]() { worker_loop(); });
    }

    void stop_worker() {
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            stopping_.store(true, std::memory_order_relaxed);
        }
        worker_cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void notify_worker() {
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            work_pending_ = true;
        }
        worker_cv_.notify_all();
    }

    void worker_loop() {
        std::unique_lock<std::mutex> lock(worker_mutex_);
        while (true) {
            worker_cv_.wait(lock, [&]() {
                return stopping_.load(std::memory_order_relaxed) || work_pending_;
            });
            if (stopping_.load(std::memory_order_relaxed)) {
                return;
            }
            work_pending_ = false;
            lock.unlock();
            std::optional<int> claimed_submission_id;
            try {
                while (std::optional<SubmissionSummary> submission = db_.take_next_queued()) {
                    claimed_submission_id = submission->id;
                    judge_submission(*submission);
                    claimed_submission_id.reset();
                    std::lock_guard<std::mutex> state_lock(worker_mutex_);
                    if (stopping_.load(std::memory_order_relaxed)) {
                        return;
                    }
                }
            } catch (const std::exception& ex) {
                std::cerr << "Judge worker error: " << ex.what() << "\n";
                bool recovered = false;
                while (!recovered) {
                    lock.lock();
                    if (stopping_.load(std::memory_order_relaxed)) {
                        return;
                    }
                    worker_cv_.wait_for(lock, std::chrono::milliseconds(500),
                                        [&]() {
                                            return stopping_.load(std::memory_order_relaxed);
                                        });
                    if (stopping_.load(std::memory_order_relaxed)) {
                        return;
                    }
                    lock.unlock();
                    try {
                        if (claimed_submission_id) {
                            db_.recover_running_submission(*claimed_submission_id);
                        }
                        recovered = true;
                    } catch (const std::exception& recovery_error) {
                        std::cerr << "Judge worker recovery error: "
                                  << recovery_error.what() << "\n";
                    }
                }
                lock.lock();
                work_pending_ = true;
                continue;
            }
            lock.lock();
        }
    }

    void judge_submission(const SubmissionSummary& submission) {
        const fs::path job_root =
            config_.data_dir / "jobs" / std::to_string(submission.id);
        ScopedDirectoryCleanup job_cleanup(job_root);
        try {
            fs::path contest = job_root / "contest";
            std::error_code ignored;
            fs::remove_all(job_root, ignored);
            fs::create_directories(contest / "contestants" / submission.username.toStdString());
            fs::create_directories(contest / "tests");

            fs::path original_problem = config_.contest_root / settings_.tests_dir /
                                        submission.problem.toStdString();
            fs::path copied_problem = contest / "tests" / submission.problem.toStdString();
            safe_copy_problem(original_problem, copied_problem);

            fs::copy_file(path_from_qstring(submission.source_path),
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
            options.compile_jobs = 1;
            options.test_jobs = 1;
            options.keep_workdir = false;
            options.selected_contestants = {submission.username.toStdString()};
            options.selected_problems = {submission.problem.toStdString()};
            options.forbidden_patterns = settings_.forbidden_patterns;
            // Destruction must stop the active compiler/test process tree before
            // joining the queue worker, even when a problem has many long tests.
            options.should_cancel = [this]() {
                return stopping_.load(std::memory_order_relaxed);
            };

            auto core = neothemis::make_judge_core(options.core_name);
            auto results = core->judge(options);
            finish_from_results(submission.id, results);
            try {
                // Rebuild from committed database state so an ignore racing
                // with completion cannot reintroduce the ignored score.
                sync_contest_results_for_pair(submission.username, submission.problem);
            } catch (const std::exception& ex) {
                std::cerr << "Failed to update contest results.csv: " << ex.what() << "\n";
            }
        } catch (const std::exception& ex) {
            if (stopping_.load(std::memory_order_relaxed)) {
                // Shutdown is not a contestant failure. Preserve the snapshot
                // and let the next server instance retry the interrupted job.
                db_.recover_running_submission(submission.id);
                return;
            }
            try {
                db_.finish_submission(submission.id, "failed", "IE", 0.0, ex.what(), {});
            } catch (const std::exception& persistence_error) {
                throw std::runtime_error(
                    std::string("judging failed: ") + ex.what() +
                    "; failed to persist the error: " + persistence_error.what());
            }
        }
        prune_terminal_source_snapshots(submission.user_id);
    }

    void finish_from_results(int id, const std::vector<neothemis::TestResult>& results) {
        std::vector<TestRow> rows;
        rows.reserve(results.size());
        double score = 0.0;
        double max_score = 0.0;
        std::map<std::string, int> priority{{"IE", 90}, {"CE", 80}, {"SV", 70},
                                            {"TLE", 60}, {"MLE", 55}, {"RE", 50},
                                            {"WA", 40}, {"MS", 30}, {"PC", 10},
                                            {"AC", 0}};
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
            summary = "PC";
        }
        if (message.isNull()) {
            message = "";
        }
        db_.finish_submission(id, "done", QString::fromStdString(summary), score, message, rows);
    }

    void configure_tls() {
        if (!config_.tls_enabled()) {
            return;
        }
        if (!QSslSocket::supportsSsl()) {
            throw std::runtime_error("TLS support is unavailable in this Qt installation");
        }
        QFile certificate_file(qstring_from_path(config_.tls_certificate));
        QFile key_file(qstring_from_path(config_.tls_private_key));
        if (!certificate_file.open(QIODevice::ReadOnly) || !key_file.open(QIODevice::ReadOnly)) {
            throw std::runtime_error("failed to open TLS certificate or private key");
        }
        const QList<QSslCertificate> certificates =
            QSslCertificate::fromData(certificate_file.readAll(), QSsl::Pem);
        if (certificates.isEmpty()) {
            throw std::runtime_error("TLS certificate file does not contain a PEM certificate");
        }
        const QByteArray key_data = key_file.readAll();
        QSslKey private_key(key_data, QSsl::Rsa, QSsl::Pem);
        if (private_key.isNull()) {
            private_key = QSslKey(key_data, QSsl::Ec, QSsl::Pem);
        }
        if (private_key.isNull()) {
            throw std::runtime_error("TLS private key is not a supported PEM RSA or EC key");
        }
        tls_configuration_ = QSslConfiguration::defaultConfiguration();
        tls_configuration_.setProtocol(QSsl::TlsV1_2OrLater);
        tls_configuration_.setLocalCertificateChain(certificates);
        tls_configuration_.setPrivateKey(private_key);
    }

    AppConfig config_;
    ContestConfig settings_;
    Database db_;
    std::unique_ptr<QLockFile> data_lock_;
    QTcpServer tcp_server_;
    QSslServer tls_server_;
    QSslConfiguration tls_configuration_;
    std::thread worker_;
    std::condition_variable worker_cv_;
    std::mutex worker_mutex_;
    mutable std::mutex contest_results_mutex_;
    std::atomic<bool> stopping_{false};
    bool work_pending_ = false;
    bool generated_admin_password_ = false;
    bool generated_join_code_ = false;
};

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
        << "  --allow-lan               Allow network clients (HTTP unless TLS files are supplied)\n"
        << "  --tls-cert <file>         PEM certificate; enables HTTPS when paired with --tls-key\n"
        << "  --tls-key <file>          PEM private key; enables HTTPS when paired with --tls-cert\n"
        << "  --secure-password-storage Store account passwords as PBKDF2 hashes\n";
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
            config.contest_root = path_from_qstring(require_value(arg));
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
            config.data_dir = path_from_qstring(require_value(arg));
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
        } else if (arg == "--tls-cert") {
            config.tls_certificate = path_from_qstring(require_value(arg));
        } else if (arg == "--tls-key") {
            config.tls_private_key = path_from_qstring(require_value(arg));
        } else if (arg == "--secure-password-storage") {
            config.secure_password_storage = true;
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
    if (config.tls_certificate.empty() != config.tls_private_key.empty()) {
        throw std::runtime_error("--tls-cert and --tls-key must be provided together");
    }
    config.secure_password_storage =
        config.secure_password_storage ||
        qEnvironmentVariable("NEOTHEMIS_SECURE_PASSWORD_STORAGE") == "1";
    config.contest_root = fs::absolute(config.contest_root);
    if (config.data_dir.empty()) {
        config.data_dir = config.contest_root / ".neothemis-server";
    }
    config.data_dir = fs::absolute(config.data_dir);
    config.tls_certificate = config.tls_certificate.empty()
                                ? fs::path()
                                : fs::absolute(config.tls_certificate);
    config.tls_private_key = config.tls_private_key.empty()
                                 ? fs::path()
                                 : fs::absolute(config.tls_private_key);
    return config;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        AppConfig config = parse_args(app.arguments());
        ContestConfig settings;
        const fs::path settings_path =
            config.contest_root / neothemis::kContestConfigFilename;
        if (fs::exists(settings_path)) {
            settings = neothemis::load_contest_config(
                settings_path, neothemis::UnknownConfigKeyPolicy::Ignore);
        }
        if (settings.forbidden_patterns.empty()) {
            settings.forbidden_patterns = neothemis::default_forbidden_patterns();
        }
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
