#include "neothemis/JudgeCore.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <set>
#include <vector>

#ifndef _WIN32
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr const char* kSettingsFilename = "neothemis.conf";

void print_usage(std::ostream& out) {
    out << "Usage:\n"
        << "  neothemis-cli judge <contest-folder> [--problem <name>] [--contestant <name>]\n"
        << "  neothemis-cli rejudge <contest-folder> [--problem <name>] [--contestant <name>]\n"
        << "  neothemis-cli config <contest-folder> list\n"
        << "  neothemis-cli config <contest-folder> available\n"
        << "  neothemis-cli config <contest-folder> set <key> <value>\n"
        << "  neothemis-cli config <contest-folder> problem <problem> list\n"
        << "  neothemis-cli config <contest-folder> problem <problem> available\n"
        << "  neothemis-cli config <contest-folder> problem <problem> set <key> <value>\n"
        << "  neothemis-cli config <contest-folder> problem <problem> points <points> <tests...>\n"
        << "\n"
        << "Filters can be repeated. Quote contestant names that contain spaces.\n"
        << "Example: neothemis-cli rejudge contest --problem VENUE --contestant \"Tran Minh Duy\"\n"
        << "\n"
        << "Test ranges are supported, for example: points 2 1-10 or points 2 1 5 3 12.\n"
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
        << "  compiler=g++\n"
        << "      C++ compiler command used for submissions and checkers.\n"
        << "  compile_flags=-std=c++17 -O2 -pipe\n"
        << "      Compiler flags used for submissions and checkers.\n"
        << "  testlib_dir=testlib\n"
        << "      Folder containing testlib.h and checkers/. Relative paths also search cwd and cwd/..\n"
        << "  parallel_jobs=0\n"
        << "      Worker count for compile preparation and judging. 0 auto-detects CPU cores.\n"
        << "  forbidden_pattern=<text>\n"
        << "      Repeatable case-insensitive source-code security filter; matching submissions get SV.\n";
}

void print_problem_config_reference(std::ostream& out) {
    out << "Problem settings in tests/<problem>/problem.conf:\n"
        << "  time_limit_ms=1000\n"
        << "      Per-test runtime limit in milliseconds. Custom checkers use this limit too.\n"
        << "  memory_limit_mb=256\n"
        << "      Per-test memory limit in megabytes. Use 0 for unlimited.\n"
        << "  stack_limit_mb=64\n"
        << "      Per-test stack limit in megabytes. Use 0 for unlimited.\n"
        << "  default_points=1\n"
        << "      Points for each accepted test unless test_points.<test> overrides it.\n"
        << "  checker=token\n"
        << "      Built-in whitespace-token checker.\n"
        << "  checker=testlib:<name>\n"
        << "      Compile testlib/checkers/<name>.cpp, for example testlib:wcmp.\n"
        << "  checker=custom\n"
        << "      Compile checker.cpp in this problem folder.\n"
        << "  checker=custom:<path>\n"
        << "      Compile checker source relative to this problem folder.\n"
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
    return true;
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

bool parse_bool(const std::string& value, const std::string& key) {
    if (value == "true" || value == "1" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "false" || value == "0" || value == "no" || value == "off") {
        return false;
    }
    throw std::runtime_error("invalid boolean for " + key + ": " + value);
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

void write_default_settings(const fs::path& settings_path) {
    std::ofstream out(settings_path);
    if (!out) {
        throw std::runtime_error("failed to create settings file: " + settings_path.string());
    }
    out << "# NeoThemis contest settings\n"
        << "core=builtin\n"
        << "contestants_dir=contestants\n"
        << "tests_dir=tests\n"
        << "output_csv=results.csv\n"
        << "scoreboard_csv=scoreboard.csv\n"
        << "keep_workdir=false\n"
        << "compiler=g++\n"
        << "compile_flags=-std=c++17 -O2 -pipe\n"
        << "testlib_dir=testlib\n"
        << "parallel_jobs=0\n"
        << "\n"
        << "# Submissions containing these text patterns are rejected with SV.\n";
    for (const auto& pattern : default_forbidden_patterns()) {
        out << "forbidden_pattern=" << pattern << '\n';
    }
}

fs::path testlib_dir_with_header(const fs::path& candidate) {
    if (fs::exists(candidate / "testlib.h")) {
        return candidate;
    }

    fs::path nested = candidate / "testlib";
    if (fs::exists(nested / "testlib.h")) {
        return nested;
    }

    return {};
}

fs::path resolve_testlib_dir(const fs::path& configured, const fs::path& contest_root) {
    if (configured.is_absolute()) {
        fs::path found = testlib_dir_with_header(configured);
        return found.empty() ? configured : found;
    }

    std::vector<fs::path> candidates = {
        contest_root / configured,
        fs::current_path() / configured,
        fs::current_path().parent_path() / configured
    };

    for (const auto& candidate : candidates) {
        fs::path found = testlib_dir_with_header(candidate);
        if (!found.empty()) {
            return fs::absolute(found);
        }
    }

    return fs::absolute(fs::current_path() / configured);
}

void apply_setting(neothemis::JudgeOptions& options,
                   const std::string& key,
                   const std::string& value) {
    if (key == "core") {
        options.core_name = value;
    } else if (key == "contestants_dir") {
        options.contestants_dir = value;
    } else if (key == "tests_dir") {
        options.tests_dir = value;
    } else if (key == "output_csv") {
        options.output_csv = value;
    } else if (key == "scoreboard_csv") {
        options.scoreboard_csv = value;
    } else if (key == "keep_workdir") {
        options.keep_workdir = parse_bool(value, key);
    } else if (key == "compiler") {
        options.compiler = value;
    } else if (key == "compile_flags") {
        options.compile_flags = value;
    } else if (key == "testlib_dir") {
        options.testlib_dir = value;
    } else if (key == "parallel_jobs") {
        options.parallel_jobs = static_cast<unsigned int>(std::stoul(value));
    } else if (key == "forbidden_pattern") {
        options.forbidden_patterns.push_back(value);
    } else {
        throw std::runtime_error("unknown setting in " + std::string(kSettingsFilename) + ": " + key);
    }
}

void load_or_create_settings(neothemis::JudgeOptions& options) {
    fs::path settings_path = options.contest_root / kSettingsFilename;
    if (!fs::exists(settings_path)) {
        write_default_settings(settings_path);
    }

    options.forbidden_patterns.clear();
    std::ifstream in(settings_path);
    if (!in) {
        throw std::runtime_error("failed to open settings file: " + settings_path.string());
    }

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(in, line)) {
        ++line_number;
        std::string stripped = trim(line);
        if (stripped.empty() || stripped[0] == '#') {
            continue;
        }

        std::size_t equal = stripped.find('=');
        if (equal == std::string::npos) {
            throw std::runtime_error(settings_path.string() + ":" +
                                     std::to_string(line_number) + ": expected key=value");
        }

        std::string key = trim(stripped.substr(0, equal));
        std::string value = trim(stripped.substr(equal + 1));
        if (key.empty()) {
            throw std::runtime_error(settings_path.string() + ":" +
                                     std::to_string(line_number) + ": empty setting key");
        }
        apply_setting(options, key, value);
    }
}

std::map<std::string, std::vector<std::string>> read_settings_file(const fs::path& path) {
    std::map<std::string, std::vector<std::string>> settings;
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("failed to open settings file: " + path.string());
    }

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(in, line)) {
        ++line_number;
        std::string stripped = trim(line);
        if (stripped.empty() || stripped[0] == '#') {
            continue;
        }
        std::size_t equal = stripped.find('=');
        if (equal == std::string::npos) {
            throw std::runtime_error(path.string() + ":" +
                                     std::to_string(line_number) + ": expected key=value");
        }
        std::string key = trim(stripped.substr(0, equal));
        std::string value = trim(stripped.substr(equal + 1));
        if (key.empty()) {
            throw std::runtime_error(path.string() + ":" +
                                     std::to_string(line_number) + ": empty setting key");
        }
        settings[key].push_back(value);
    }
    return settings;
}

void write_settings_file(const fs::path& path,
                         const std::vector<std::pair<std::string, std::string>>& settings) {
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("failed to write settings file: " + path.string());
    }
    for (const auto& item : settings) {
        out << item.first << '=' << item.second << '\n';
    }
}

void print_settings_file(const fs::path& path, std::ostream& out) {
    auto parsed = read_settings_file(path);
    out << path.string() << ":\n";
    for (const auto& entry : parsed) {
        for (const auto& value : entry.second) {
            out << "  " << entry.first << '=' << value << '\n';
        }
    }
}

void set_single_setting(const fs::path& path, const std::string& key, const std::string& value) {
    auto parsed = read_settings_file(path);
    parsed[key] = {value};

    std::vector<std::pair<std::string, std::string>> flattened;
    for (const auto& entry : parsed) {
        for (const auto& entry_value : entry.second) {
            flattened.push_back({entry.first, entry_value});
        }
    }
    write_settings_file(path, flattened);
}

void set_problem_setting(const fs::path& path, const std::string& key, const std::string& value) {
    auto parsed = read_settings_file(path);
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

    std::vector<std::pair<std::string, std::string>> flattened;
    for (const auto& entry : parsed) {
        for (const auto& entry_value : entry.second) {
            flattened.push_back({entry.first, entry_value});
        }
    }
    write_settings_file(path, flattened);
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

fs::path problem_settings_path(const fs::path& contest_root, const std::string& problem) {
    neothemis::JudgeOptions options;
    options.contest_root = contest_root;
    options.forbidden_patterns = default_forbidden_patterns();
    load_or_create_settings(options);
    return contest_root / options.tests_dir / problem / "problem.conf";
}

int handle_config(int argc, char** argv) {
    if (argc < 4) {
        throw std::runtime_error("missing config arguments");
    }

    fs::path contest_root = argv[2];
    fs::path contest_settings = contest_root / kSettingsFilename;
    if (!fs::exists(contest_settings)) {
        write_default_settings(contest_settings);
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
        set_single_setting(contest_settings, argv[4], argv[5]);
        std::cout << "Updated " << contest_settings.string() << '\n';
        return 0;
    }

    if (scope != "problem" || argc < 6) {
        throw std::runtime_error("usage: neothemis-cli config <contest> problem <problem> ...");
    }

    std::string problem = argv[4];
    std::string action = argv[5];
    fs::path settings_path = problem_settings_path(contest_root, problem);
    if (!fs::exists(settings_path)) {
        fs::create_directories(settings_path.parent_path());
        std::ofstream out(settings_path);
        if (!out) {
            throw std::runtime_error("failed to create " + settings_path.string());
        }
        out << "time_limit_ms=1000\n"
            << "memory_limit_mb=256\n"
            << "stack_limit_mb=64\n"
            << "default_points=1\n"
            << "checker=token\n";
    }

    if (action == "available" || action == "help") {
        if (argc != 6) {
            throw std::runtime_error("usage: neothemis-cli config <contest> problem <problem> available");
        }
        print_problem_config_reference(std::cout);
        return 0;
    }

    if (action == "list") {
        if (argc != 6) {
            throw std::runtime_error("usage: neothemis-cli config <contest> problem <problem> list");
        }
        print_settings_file(settings_path, std::cout);
        return 0;
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
            throw std::runtime_error("usage: neothemis-cli config <contest> problem <problem> points <points> <tests...>");
        }
        std::string points = argv[6];
        auto parsed = read_settings_file(settings_path);
        for (const auto& test : expand_tests(7, argc, argv)) {
            parsed["test_points." + test] = {points};
        }

        std::vector<std::pair<std::string, std::string>> flattened;
        for (const auto& entry : parsed) {
            for (const auto& value : entry.second) {
                flattened.push_back({entry.first, value});
            }
        }
        write_settings_file(settings_path, flattened);
        std::cout << "Updated " << settings_path.string() << '\n';
        return 0;
    }

    throw std::runtime_error("unknown problem config action: " + action);
}

neothemis::JudgeOptions parse_judge_args(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
        print_usage(std::cout);
        std::exit(0);
    }
    if (argc < 3) {
        throw std::runtime_error("missing contest folder");
    }

    neothemis::JudgeOptions options;
    options.contest_root = argv[2];
    options.forbidden_patterns = default_forbidden_patterns();

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
    options.testlib_dir = resolve_testlib_dir(options.testlib_dir, options.contest_root);
    return options;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
            print_usage(std::cout);
            return 0;
        }
        if (std::string(argv[1]) == "config") {
            return handle_config(argc, argv);
        }
        std::string command = argv[1];
        if (command != "judge" && command != "rejudge") {
            throw std::runtime_error("unknown command: " + std::string(argv[1]));
        }

        neothemis::JudgeOptions options = parse_judge_args(argc, argv);
        ProgressRenderer progress(stderr_is_terminal());
        options.progress = [&](const std::string& line) {
            progress.update(line);
        };
        auto core = neothemis::make_judge_core(options.core_name);
        auto results = core->judge(options);
        progress.finish();

        fs::create_directories(options.output_csv.parent_path());
        std::ofstream output(options.output_csv);
        if (!output) {
            throw std::runtime_error("failed to open CSV output: " + options.output_csv.string());
        }
        neothemis::write_csv(output, results);

        fs::create_directories(options.scoreboard_csv.parent_path());
        std::ofstream scoreboard(options.scoreboard_csv);
        if (!scoreboard) {
            throw std::runtime_error("failed to open scoreboard CSV output: " +
                                     options.scoreboard_csv.string());
        }
        neothemis::write_scoreboard_csv(scoreboard, results);

        std::cout << "Wrote " << results.size() << " result rows to "
                  << options.output_csv.string() << '\n'
                  << "Wrote scoreboard to " << options.scoreboard_csv.string() << '\n';
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << '\n';
        std::cerr << "Run `neothemis-cli --help` for usage.\n";
        return 1;
    }
}
