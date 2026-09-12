#include "neothemis/JudgeCore.hpp"
#include "neothemis/ContestArchive.hpp"
#include "neothemis/Config.hpp"
#include "neothemis/Csv.hpp"
#include "neothemis/Spreadsheet.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/ioctl.h>
#include <unistd.h>
#else
#include <io.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr const char* kSettingsFilename = neothemis::kContestConfigFilename;

void print_usage(std::ostream& out) {
    out << "Usage:\n"
        << "  neothemis-cli judge <contest-folder-or-file.ncontest> [--problem <name>] [--contestant <name>] [--unsafe-no-sandbox]\n"
        << "  neothemis-cli rejudge <contest-folder-or-file.ncontest> [--problem <name>] [--contestant <name>] [--unsafe-no-sandbox]\n"
        << "  neothemis-cli config <contest-folder-or-file.ncontest> list\n"
        << "  neothemis-cli config <contest-folder-or-file.ncontest> available\n"
        << "  neothemis-cli config <contest-folder-or-file.ncontest> set <key> <value>\n"
        << "  neothemis-cli config <contest-folder-or-file.ncontest> problem <problem> list\n"
        << "  neothemis-cli config <contest-folder-or-file.ncontest> problem <problem> available\n"
        << "  neothemis-cli config <contest-folder-or-file.ncontest> problem <problem> set <key> <value>\n"
        << "  neothemis-cli config <contest-folder-or-file.ncontest> problem <problem> points <points|default> <tests...>\n"
        << "  neothemis-cli pack <contest-folder> <output.ncontest>\n"
        << "  neothemis-cli unpack <contest.ncontest|archive.zip> <output-folder>\n"
        << "  neothemis-cli convert <old-contest-folder-or-file.contest> <output.ncontest>\n"
        << "  neothemis-cli export-scoreboard <contest-folder-or-file.ncontest> <output.xlsx>\n"
        << "  neothemis-cli export-data <contest-folder-or-file.ncontest> <output.xlsx>\n"
        << "\n"
        << "Filters can be repeated. Quote contestant names that contain spaces.\n"
        << "Example: neothemis-cli rejudge contest --problem VENUE --contestant \"Tran Minh Duy\"\n"
        << "--unsafe-no-sandbox explicitly permits unsandboxed execution when a secure sandbox is unavailable.\n"
        << "\n"
        << "Test ranges are supported, for example: points 2 1-10 or points default 1 5 3 12.\n"
        << "Settings are loaded from <contest-folder>/" << kSettingsFilename << ".\n"
        << "A default settings file is created when it does not exist.\n"
        << "\n"
        << "Expected layout:\n"
        << "  contest/\n"
        << "    " << kSettingsFilename << "\n"
        << "    contestants/<contestant>/<problem>.cpp\n"
        << "    tests/<problem>/problem.conf\n"
        << "    tests/<problem>/<Num>/<problem>.inp\n"
        << "    tests/<problem>/<Num>/<problem>.out\n";
}

void print_contest_config_reference(std::ostream& out) {
    out << "Contest settings in neothemis.conf:\n"
        << "  core=builtin\n"
        << "      Judge backend. Currently only builtin is shipped.\n"
        << "  contestants_dir=contestants\n"
        << "      Directory of contestant folders, relative to the contest folder.\n"
        << "  tests_dir=tests\n"
        << "      Directory of problem test folders, relative to the contest folder.\n"
        << "  output_csv=results.csv\n"
        << "      Detailed CSV output path. Relative paths are inside the contest folder.\n"
        << "  scoreboard_csv=scoreboard.csv\n"
        << "      Simplified scoreboard CSV path. Relative paths are inside the contest folder.\n"
        << "  keep_workdir=false\n"
        << "      Keep .neothemis-work after judging. Values: true/false, 1/0, yes/no, on/off.\n"
        << "  server_ranking_enabled=false\n"
        << "      Let contestants view the live web ranking. Admins can always view it.\n"
        << "  server_contestant_details_enabled=false\n"
        << "      Let contestants view per-test details for their own web submissions.\n"
        << "  compiler=g++\n"
        << "      C++ compiler command used for submissions and checkers.\n"
        << "  compile_flags=-std=c++14 -O2 -pipe\n"
        << "      Compiler flags used for submissions and checkers.\n"
        << "  stack_limit_mb=64\n"
        << "      Contest-wide maximum stack size in megabytes. Use 0 for unlimited.\n"
        << "      Linux enforces this at runtime with RLIMIT_STACK and disables sibling-call optimization.\n"
        << "      Windows passes a compiler/linker stack reserve flag where supported.\n"
        << "  parallel_jobs=0\n"
        << "      Default worker limit for compilation and test execution.\n"
        << "      0 prefers performance cores; manual values are capped at physical cores.\n"
        << "  compile_jobs=0\n"
        << "      Compilation worker limit; 0 inherits parallel_jobs.\n"
        << "  test_jobs=0\n"
        << "      Test execution worker limit; 0 inherits parallel_jobs.\n"
        << "  timing_focused=false\n"
        << "      Run timed tests serially after parallel compilation; preserves test_jobs.\n"
        << "  forbidden_pattern=<text>\n"
        << "      Repeatable case-insensitive source-code security filter; matching submissions get SV.\n";
}

void print_problem_config_reference(std::ostream& out) {
    out << "Problem settings in tests/<problem>/problem.conf:\n"
        << "  time_limit_ms=1000\n"
        << "      User+kernel CPU-time limit for the full process tree, in milliseconds.\n"
        << "      Reported test time uses this metric on Linux and Windows.\n"
        << "      Custom checkers use it too; a separate wall guard stops sleepers.\n"
        << "  memory_limit_mb=256\n"
        << "      Per-test memory limit in megabytes. Use 0 for unlimited.\n"
        << "  default_points=1\n"
        << "      Points for each accepted test unless test_points.<test> overrides it.\n"
        << "  checker=token\n"
        << "      Built-in whitespace-token checker.\n"
        << "  checker=custom\n"
        << "      Compile checker.cpp in this problem folder.\n"
        << "      If it includes testlib.h, put testlib.h in this problem folder.\n"
        << "  checker=custom:<path>\n"
        << "      Compile checker source relative to this problem folder; exit-7 scores are absolute points.\n"
        << "  checker=testlib\n"
        << "      Compile checker.cpp and interpret testlib quitp values as percentages.\n"
        << "  checker=testlib:<path>\n"
        << "      Compile a relative checker source with the same percentage protocol.\n"
        << "  test_points.<test>=<points>\n"
        << "      Override points for one test folder. test01 also matches test_points.1.\n"
        << "\n"
        << "Point commands:\n"
        << "  points 2 1-10                 Set tests 1 through 10 to 2 points.\n"
        << "  points 5 1 5 3 12             Set listed tests to 5 points.\n";
}

std::string trim(const std::string& value) {
    std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool stderr_is_terminal() {
#ifndef _WIN32
    return isatty(STDERR_FILENO);
#else
    return _isatty(_fileno(stderr)) != 0;
#endif
}

std::size_t terminal_width() {
#ifndef _WIN32
    winsize size{};
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
        return size.ws_col;
    }
#endif
    return 80;
}

std::vector<std::string> split_progress_parts(const std::string& line) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= line.size()) {
        std::size_t pos = line.find(" | ", start);
        std::string part = pos == std::string::npos
                               ? line.substr(start)
                               : line.substr(start, pos - start);
        parts.push_back(trim(part));
        if (pos == std::string::npos) {
            break;
        }
        start = pos + 3;
    }
    return parts;
}

class ProgressRenderer {
public:
    explicit ProgressRenderer(bool interactive)
        : interactive_(interactive), width_(terminal_width()) {}

    ~ProgressRenderer() {
        finish();
    }

    void update(const std::string& line) {
        if (!interactive_) {
            std::cerr << line << '\n';
            return;
        }

        std::vector<std::string> lines;
        if (line.rfind("progress ", 0) == 0) {
            lines = split_progress_parts(line);
        } else {
            lines.push_back(line);
        }
        render(lines);
    }

    void finish() {
        if (!interactive_ || finished_) {
            return;
        }
        if (rendered_lines_ > 0) {
            std::cerr << '\n';
        }
        std::cerr.flush();
        finished_ = true;
    }

private:
    std::string fit_line(std::string line) const {
        if (width_ == 0 || line.size() < width_) {
            return line;
        }
        if (width_ <= 1) {
            return {};
        }
        line.resize(width_ - 1);
        return line;
    }

    void render(const std::vector<std::string>& lines) {
        if (rendered_lines_ > 0) {
            std::cerr << "\x1b[" << (rendered_lines_ - 1) << "A";
        }

        std::size_t max_lines = std::max(rendered_lines_, lines.size());
        for (std::size_t i = 0; i < max_lines; ++i) {
            std::cerr << "\r\x1b[2K";
            if (i < lines.size()) {
                std::cerr << fit_line(lines[i]);
            }
            if (i + 1 < max_lines) {
                std::cerr << '\n';
            }
        }
        rendered_lines_ = lines.size();
        std::cerr.flush();
    }

    bool interactive_ = false;
    bool finished_ = false;
    std::size_t width_ = 80;
    std::size_t rendered_lines_ = 0;
};

std::string display_archive_label(std::string label) {
    std::replace(label.begin(), label.end(), '_', ' ');
    return label;
}

neothemis::ArchiveProgress archive_progress_callback(ProgressRenderer& progress) {
    auto last_updates = std::make_shared<std::map<std::string, std::uint64_t>>();
    return [&, last_updates](std::uint64_t done, std::uint64_t total, const std::string& label) {
        if (total > 100 && done != 0 && done != total) {
            std::uint64_t step = std::max<std::uint64_t>(1, total / 100);
            std::uint64_t& last = (*last_updates)[label];
            if (done < last + step) {
                return;
            }
            last = done;
        }
        std::ostringstream line;
        line << "progress " << display_archive_label(label);
        if (total > 0) {
            line << " " << done << "/" << total;
        }
        progress.update(line.str());
    };
}

void load_or_create_settings(neothemis::JudgeOptions& options) {
    neothemis::apply_contest_config(
        neothemis::load_or_create_contest_config(options.contest_root), options);
}

fs::path detect_extracted_contest_root(const fs::path& root) {
    if (fs::exists(root / kSettingsFilename)) {
        return root;
    }
    if (fs::is_directory(root)) {
        for (const auto& entry : fs::directory_iterator(root)) {
            if (entry.is_directory() && fs::exists(entry.path() / kSettingsFilename)) {
                return entry.path();
            }
        }
    }
    throw std::runtime_error("NeoThemis contest settings not found in archive");
}

struct OpenedContest {
    fs::path original_path;
    fs::path root;
    fs::path temp_root;
    bool archive = false;
    bool save_back = false;

    OpenedContest() = default;
    OpenedContest(const OpenedContest&) = delete;
    OpenedContest& operator=(const OpenedContest&) = delete;

    OpenedContest(OpenedContest&& other) noexcept {
        *this = std::move(other);
    }

    OpenedContest& operator=(OpenedContest&& other) noexcept {
        if (this != &other) {
            cleanup();
            original_path = std::move(other.original_path);
            root = std::move(other.root);
            temp_root = std::move(other.temp_root);
            archive = other.archive;
            save_back = other.save_back;
            other.archive = false;
            other.save_back = false;
            other.temp_root.clear();
        }
        return *this;
    }

    ~OpenedContest() {
        cleanup();
    }

    void save(const neothemis::ArchiveProgress& progress) {
        if (archive && save_back) {
            neothemis::write_zip_archive_from_directory(original_path, root, progress);
        }
    }

    void cleanup() {
        if (!temp_root.empty()) {
            std::error_code ec;
            fs::remove_all(temp_root, ec);
            temp_root.clear();
        }
    }
};

OpenedContest open_contest_for_cli(const fs::path& path,
                                   bool save_back,
                                   const neothemis::ArchiveProgress& progress) {
    OpenedContest opened;
    opened.original_path = path;
    opened.save_back = save_back;
    if (fs::is_regular_file(path) && neothemis::is_ncontest_file(path)) {
        opened.archive = true;
        opened.temp_root = neothemis::make_temp_directory("neothemis-cli-contest");
        neothemis::extract_zip_archive(path, opened.temp_root, progress);
        opened.root = detect_extracted_contest_root(opened.temp_root);
        return opened;
    }
    opened.root = path;
    return opened;
}

bool config_command_changes_files(int argc, char** argv) {
    if (argc < 4) {
        return false;
    }
    std::string scope = argv[3];
    if (scope == "set") {
        return true;
    }
    if (scope == "problem" && argc >= 6) {
        std::string action = argv[5];
        return action == "set" || action == "points";
    }
    return false;
}

void print_settings_file(const fs::path& path, std::ostream& out) {
    auto parsed = neothemis::read_config_values(path);
    out << path.string() << ":\n";
    for (const auto& entry : parsed) {
        for (const auto& value : entry.second) {
            out << "  " << entry.first << '=' << value << '\n';
        }
    }
}

void set_single_setting(const fs::path& path, const std::string& key, const std::string& value) {
    auto parsed = neothemis::read_config_values(path);
    parsed[key] = {value};
    neothemis::write_config_values(path, parsed);
}

void set_problem_setting(const fs::path& path, const std::string& key, const std::string& value) {
    auto parsed = neothemis::read_config_values(path);
    std::string old_default = "1";
    auto old_default_entry = parsed.find("default_points");
    if (old_default_entry != parsed.end() && !old_default_entry->second.empty()) {
        old_default = old_default_entry->second.back();
    }

    parsed[key] = {value};
    if (key == "default_points") {
        constexpr const char* prefix = "test_points.";
        for (auto& entry : parsed) {
            if (entry.first.rfind(prefix, 0) != 0 || entry.second.size() != 1) {
                continue;
            }
            if (trim(entry.second.front()) == old_default) {
                entry.second = {value};
            }
        }
    }

    neothemis::write_config_values(path, parsed);
}

std::set<std::string> expand_tests(int first_index, int argc, char** argv) {
    std::set<std::string> tests;
    for (int i = first_index; i < argc; ++i) {
        std::string value = argv[i];
        std::size_t dash = value.find('-');
        if (dash == std::string::npos) {
            tests.insert(value);
            continue;
        }

        int first = std::stoi(value.substr(0, dash));
        int last = std::stoi(value.substr(dash + 1));
        if (first > last) {
            std::swap(first, last);
        }
        for (int test = first; test <= last; ++test) {
            tests.insert(std::to_string(test));
        }
    }
    return tests;
}

void validate_path_component(const std::string& value, const char* name) {
    const fs::path component(value);
    if (value.empty() || value == "." || value == ".." ||
        value.find('/') != std::string::npos || value.find('\\') != std::string::npos ||
        component.is_absolute() || component.has_parent_path()) {
        throw std::runtime_error(std::string(name) + " must be one path component");
    }
}

fs::path problem_settings_path(const fs::path& contest_root, const std::string& problem) {
    validate_path_component(problem, "problem name");

    neothemis::JudgeOptions loaded;
    loaded.contest_root = contest_root;
    load_or_create_settings(loaded);

    // This command only touches tests_dir. Keep the other configured paths out
    // of this check so an independently broken setting can be repaired later.
    neothemis::JudgeOptions checked;
    checked.contest_root = contest_root;
    checked.tests_dir = loaded.tests_dir;
    neothemis::validate_judge_paths(checked);
    return contest_root / loaded.tests_dir / problem / neothemis::kProblemConfigFilename;
}

int handle_config(int argc, char** argv, const fs::path& contest_root) {
    if (argc < 4) {
        throw std::runtime_error("missing config arguments");
    }

    fs::path contest_settings = contest_root / kSettingsFilename;
    if (!fs::exists(contest_settings)) {
        neothemis::write_default_contest_config(contest_settings);
    }

    std::string scope = argv[3];
    if (scope == "available" || scope == "help") {
        if (argc != 4) {
            throw std::runtime_error("usage: neothemis-cli config <contest> available");
        }
        print_contest_config_reference(std::cout);
        return 0;
    }

    if (scope == "list") {
        if (argc != 4) {
            throw std::runtime_error("usage: neothemis-cli config <contest> list");
        }
        print_settings_file(contest_settings, std::cout);
        return 0;
    }

    if (scope == "set") {
        if (argc != 6) {
            throw std::runtime_error("usage: neothemis-cli config <contest> set <key> <value>");
        }
        const std::string key = argv[4];
        const fs::path value = argv[5];
        if (key == "parallel_jobs" || key == "compile_jobs" || key == "test_jobs") {
            neothemis::parse_config_worker_count(argv[5], key);
        } else if (key == "timing_focused") {
            const std::string boolean = argv[5];
            if (boolean != "true" && boolean != "false" && boolean != "1" &&
                boolean != "0" && boolean != "yes" && boolean != "no" &&
                boolean != "on" && boolean != "off") {
                throw std::runtime_error("invalid boolean for " + key + ": " + boolean);
            }
        }
        neothemis::JudgeOptions proposed;
        proposed.contest_root = contest_root;
        if (key == "contestants_dir") {
            proposed.contestants_dir = value;
        } else if (key == "tests_dir") {
            proposed.tests_dir = value;
        } else if (key == "output_csv") {
            proposed.output_csv = value;
        } else if (key == "scoreboard_csv") {
            proposed.scoreboard_csv = value;
        }
        if (key == "contestants_dir" || key == "tests_dir" || key == "output_csv" ||
            key == "scoreboard_csv") {
            neothemis::validate_judge_paths(proposed);
        }
        set_single_setting(contest_settings, argv[4], argv[5]);
        std::cout << "Updated " << contest_settings.string() << '\n';
        return 0;
    }

    if (scope != "problem" || argc < 6) {
        throw std::runtime_error("usage: neothemis-cli config <contest> problem <problem> ...");
    }

    std::string problem = argv[4];
    std::string action = argv[5];
    if (action == "available" || action == "help") {
        if (argc != 6) {
            throw std::runtime_error("usage: neothemis-cli config <contest> problem <problem> available");
        }
        validate_path_component(problem, "problem name");
        print_problem_config_reference(std::cout);
        return 0;
    }

    fs::path settings_path = problem_settings_path(contest_root, problem);

    if (action == "list") {
        if (argc != 6) {
            throw std::runtime_error("usage: neothemis-cli config <contest> problem <problem> list");
        }
        if (!fs::exists(settings_path)) {
            throw std::runtime_error("problem settings file not found: " +
                                     settings_path.string());
        }
        print_settings_file(settings_path, std::cout);
        return 0;
    }

    if (!fs::exists(settings_path)) {
        fs::create_directories(settings_path.parent_path());
        neothemis::write_default_problem_config(
            settings_path, neothemis::ConfigTemplateStyle::Compact);
    }

    if (action == "set") {
        if (argc != 8) {
            throw std::runtime_error("usage: neothemis-cli config <contest> problem <problem> set <key> <value>");
        }
        set_problem_setting(settings_path, argv[6], argv[7]);
        std::cout << "Updated " << settings_path.string() << '\n';
        return 0;
    }

    if (action == "points") {
        if (argc < 8) {
            throw std::runtime_error("usage: neothemis-cli config <contest> problem <problem> points <points|default> <tests...>");
        }
        std::string points = argv[6];
        bool clear_override = trim(points).empty() ||
                              points == "default" ||
                              points == "blank" ||
                              points == "-";
        auto parsed = neothemis::read_config_values(settings_path);
        for (const auto& test : expand_tests(7, argc, argv)) {
            const auto keys = neothemis::test_point_keys(test);
            if (clear_override) {
                for (const auto& key : keys) {
                    auto found = parsed.find("test_points." + key);
                    if (found != parsed.end()) {
                        found->second = {""};
                    }
                }
                parsed["test_points." + keys.back()] = {""};
            } else {
                parsed["test_points." + test] = {points};
            }
        }
        neothemis::write_config_values(settings_path, parsed);
        std::cout << "Updated " << settings_path.string() << '\n';
        return 0;
    }

    throw std::runtime_error("unknown problem config action: " + action);
}

neothemis::JudgeOptions parse_judge_args(int argc, char** argv, const fs::path& contest_root) {
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
        print_usage(std::cout);
        std::exit(0);
    }
    if (argc < 3) {
        throw std::runtime_error("missing contest folder");
    }

    neothemis::JudgeOptions options;
    options.contest_root = contest_root;

    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            std::exit(0);
        } else if (arg == "--problem" || arg == "-p") {
            if (i + 1 >= argc) {
                throw std::runtime_error(arg + " requires a problem name");
            }
            options.selected_problems.push_back(argv[++i]);
        } else if (arg == "--contestant" || arg == "-c") {
            if (i + 1 >= argc) {
                throw std::runtime_error(arg + " requires a contestant name");
            }
            options.selected_contestants.push_back(argv[++i]);
        } else if (arg == "--unsafe-no-sandbox") {
            options.execution_security = neothemis::ExecutionSecurity::ExplicitlyUnsafe;
        } else {
            throw std::runtime_error("unknown judge option: " + arg);
        }
    }

    load_or_create_settings(options);
    if (options.output_csv.is_relative()) {
        options.output_csv = options.contest_root / options.output_csv;
    }
    if (options.scoreboard_csv.is_relative()) {
        options.scoreboard_csv = options.contest_root / options.scoreboard_csv;
    }
    return options;
}

int handle_pack(int argc, char** argv, ProgressRenderer& progress) {
    if (argc != 4) {
        throw std::runtime_error("usage: neothemis-cli pack <contest-folder> <output.ncontest>");
    }
    fs::path output = neothemis::ensure_ncontest_extension(argv[3]);
    neothemis::write_zip_archive_from_directory(output, argv[2],
                                                archive_progress_callback(progress));
    progress.finish();
    std::cout << "Wrote contest archive to " << output.string() << '\n';
    return 0;
}

int handle_unpack(int argc, char** argv, ProgressRenderer& progress) {
    if (argc != 4) {
        throw std::runtime_error("usage: neothemis-cli unpack <contest.ncontest|archive.zip> <output-folder>");
    }
    neothemis::extract_zip_archive(argv[2], argv[3], archive_progress_callback(progress));
    progress.finish();
    std::cout << "Extracted archive to " << fs::path(argv[3]).string() << '\n';
    return 0;
}

int handle_convert(int argc, char** argv, ProgressRenderer& progress) {
    if (argc != 4) {
        throw std::runtime_error("usage: neothemis-cli convert <old-contest-folder-or-file.contest> <output.ncontest>");
    }
    fs::path output = neothemis::ensure_ncontest_extension(argv[3]);
    neothemis::convert_old_themis_contest(argv[2], output,
                                          archive_progress_callback(progress));
    progress.finish();
    std::cout << "Converted old contest to " << output.string() << '\n';
    return 0;
}

int handle_export_xlsx(int argc,
                       char** argv,
                       ProgressRenderer& progress,
                       bool scoreboard) {
    if (argc != 4) {
        throw std::runtime_error(scoreboard
            ? "usage: neothemis-cli export-scoreboard <contest-folder-or-file.ncontest> <output.xlsx>"
            : "usage: neothemis-cli export-data <contest-folder-or-file.ncontest> <output.xlsx>");
    }

    OpenedContest contest =
        open_contest_for_cli(argv[2], false, archive_progress_callback(progress));
    neothemis::JudgeOptions options;
    options.contest_root = contest.root;
    load_or_create_settings(options);
    neothemis::validate_judge_paths(options);

    fs::path csv_path = scoreboard ? options.scoreboard_csv : options.output_csv;
    if (csv_path.is_relative()) {
        csv_path = options.contest_root / csv_path;
    }
    if (!fs::exists(csv_path)) {
        throw std::runtime_error("CSV file not found; run judge first: " + csv_path.string());
    }

    neothemis::XlsxSheet sheet;
    sheet.name = scoreboard ? "Scoreboard" : "Data";
    sheet.freeze_first_row = false;
    sheet.bold_first_row = false;
    const auto csv_rows = neothemis::read_csv_file_locked(csv_path);
    sheet.rows.reserve(csv_rows.size());
    for (const auto& csv_row : csv_rows) {
        neothemis::XlsxRow row;
        row.reserve(csv_row.size());
        for (const auto& value : csv_row) {
            row.push_back(neothemis::xlsx_text(value));
        }
        sheet.rows.push_back(std::move(row));
    }

    fs::path output = neothemis::ensure_xlsx_extension(argv[3]);
    neothemis::write_xlsx_file(output, sheet, archive_progress_callback(progress));
    progress.finish();
    std::cout << "Exported " << (scoreboard ? "scoreboard" : "data")
              << " to " << output.string() << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
            print_usage(std::cout);
            return 0;
        }
        std::string command = argv[1];
        ProgressRenderer progress(stderr_is_terminal());
        if (command == "pack") {
            return handle_pack(argc, argv, progress);
        }
        if (command == "unpack") {
            return handle_unpack(argc, argv, progress);
        }
        if (command == "convert") {
            return handle_convert(argc, argv, progress);
        }
        if (command == "export-scoreboard") {
            return handle_export_xlsx(argc, argv, progress, true);
        }
        if (command == "export-data") {
            return handle_export_xlsx(argc, argv, progress, false);
        }
        if (command == "config") {
            if (argc < 3) {
                throw std::runtime_error("missing contest folder or file");
            }
            bool save_config = config_command_changes_files(argc, argv);
            OpenedContest contest =
                open_contest_for_cli(argv[2], save_config, archive_progress_callback(progress));
            int result = handle_config(argc, argv, contest.root);
            contest.save(archive_progress_callback(progress));
            progress.finish();
            if (contest.archive && save_config) {
                std::cout << "Saved contest archive to " << contest.original_path.string() << '\n';
            }
            return result;
        }
        if (command != "judge" && command != "rejudge") {
            throw std::runtime_error("unknown command: " + std::string(argv[1]));
        }

        OpenedContest contest =
            open_contest_for_cli(argv[2], true, archive_progress_callback(progress));
        neothemis::JudgeOptions options = parse_judge_args(argc, argv, contest.root);
        options.progress = [&](const std::string& line) {
            progress.update(line);
        };
        const neothemis::ContestOverview judged_scope = neothemis::inspect_contest(options);
        auto core = neothemis::make_judge_core(options.core_name);
        auto results = core->judge(options);

        std::ostringstream output;
        neothemis::write_csv(output, results);
        neothemis::CsvTable result_rows = neothemis::parse_csv(output.str());
        if (result_rows.empty()) {
            throw std::runtime_error("judge produced an invalid empty CSV document");
        }
        const neothemis::CsvRow header = result_rows.front();
        result_rows.erase(result_rows.begin());
        std::set<neothemis::CsvKey> replaced_pairs;
        for (const std::string& contestant : judged_scope.contestants) {
            for (const std::string& problem : judged_scope.problems) {
                replaced_pairs.insert({contestant, problem});
            }
        }
        neothemis::replace_csv_rows_by_key_atomic(
            options.output_csv, header, result_rows, replaced_pairs,
            [&](const neothemis::CsvTable& merged_rows) {
                std::ostringstream scoreboard;
                neothemis::write_scoreboard_csv_from_results(scoreboard, merged_rows);
                neothemis::write_text_file_atomic(options.scoreboard_csv, scoreboard.str());
            });
        contest.save(archive_progress_callback(progress));
        progress.finish();

        if (contest.archive) {
            fs::path result_path = fs::relative(options.output_csv, options.contest_root);
            fs::path scoreboard_path = fs::relative(options.scoreboard_csv, options.contest_root);
            std::cout << "Wrote " << results.size() << " result rows to "
                      << result_path.string() << " inside " << contest.original_path.string() << '\n'
                      << "Wrote scoreboard to " << scoreboard_path.string() << " inside "
                      << contest.original_path.string() << '\n';
        } else {
            std::cout << "Wrote " << results.size() << " result rows to "
                      << options.output_csv.string() << '\n'
                      << "Wrote scoreboard to " << options.scoreboard_csv.string() << '\n';
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << '\n';
        std::cerr << "Run `neothemis-cli --help` for usage.\n";
        return 1;
    }
}
