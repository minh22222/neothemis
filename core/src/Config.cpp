#include "neothemis/Config.hpp"

#include "neothemis/JudgeCore.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

namespace neothemis {
namespace {

std::string trim(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

ConfigEntries read_entries(const fs::path& path, const std::string& description) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("failed to open " + description + ": " + path.string());
    }

    ConfigEntries entries;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const std::string stripped = trim(line);
        if (stripped.empty() || stripped.front() == '#') {
            continue;
        }
        const std::size_t equal = stripped.find('=');
        if (equal == std::string::npos) {
            throw std::runtime_error(path.string() + ":" + std::to_string(line_number) +
                                     ": expected key=value");
        }
        const std::string key = trim(stripped.substr(0, equal));
        if (key.empty()) {
            throw std::runtime_error(path.string() + ":" + std::to_string(line_number) +
                                     ": empty setting key");
        }
        entries.push_back({key, trim(stripped.substr(equal + 1)), line_number});
    }
    return entries;
}

bool parse_bool(const std::string& value, const std::string& key) {
    if (value == "true" || value == "1" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "false" || value == "0" || value == "no" || value == "off") {
        return false;
    }
    throw std::runtime_error("invalid boolean for " + key + ": " + value);
}

std::uint64_t parse_config_uint64(const std::string& value, const std::string& key) {
    if (value.empty() || value.front() == '+' || value.front() == '-') {
        throw std::runtime_error("invalid non-negative integer for " + key + ": " + value);
    }
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        throw std::runtime_error("invalid non-negative integer for " + key + ": " + value);
    }
    return parsed;
}

double parse_config_points(const std::string& value, const std::string& key) {
    std::size_t consumed = 0;
    double parsed = 0.0;
    try {
        parsed = std::stod(value, &consumed);
    } catch (const std::exception&) {
        throw std::runtime_error("invalid non-negative number for " + key + ": " + value);
    }
    if (consumed != value.size() || !std::isfinite(parsed) || parsed < 0.0) {
        throw std::runtime_error("invalid non-negative number for " + key + ": " + value);
    }
    return parsed;
}

std::string documented_contest_config() {
    std::ostringstream output;
    output << "# NeoThemis contest settings\n"
           << "core=builtin\n"
           << "contestants_dir=contestants\n"
           << "tests_dir=tests\n"
           << "output_csv=results.csv\n"
           << "scoreboard_csv=scoreboard.csv\n"
           << "keep_workdir=false\n"
           << "server_ranking_enabled=false\n"
           << "server_contestant_details_enabled=false\n"
           << "compiler=g++\n"
           << "compile_flags=-std=c++14 -O2 -pipe\n"
           << "stack_limit_mb=64\n"
           << "# 0 prefers performance cores. Manual values above physical cores are capped.\n"
           << "parallel_jobs=0\n"
           << "# Phase limits: 0 inherits parallel_jobs. Tests run after compilation finishes.\n"
           << "compile_jobs=0\n"
           << "test_jobs=0\n"
           << "# Keep parallel compilation but execute timed tests one at a time.\n"
           << "timing_focused=false\n"
           << "\n"
           << "# Submissions containing these text patterns are rejected with SV.\n";
    for (const auto& pattern : default_forbidden_patterns()) {
        output << "forbidden_pattern=" << pattern << '\n';
    }
    return output.str();
}

std::string compact_contest_config() {
    std::ostringstream output;
    output << "core=builtin\n"
           << "contestants_dir=contestants\n"
           << "tests_dir=tests\n"
           << "output_csv=results.csv\n"
           << "scoreboard_csv=scoreboard.csv\n"
           << "keep_workdir=false\n"
           << "server_ranking_enabled=false\n"
           << "server_contestant_details_enabled=false\n"
           << "compiler=g++\n"
           << "compile_flags=-std=c++14 -O2 -pipe\n"
           << "stack_limit_mb=64\n"
           << "parallel_jobs=0\n"
           << "compile_jobs=0\n"
           << "test_jobs=0\n"
           << "timing_focused=false\n";
    for (const auto& pattern : default_forbidden_patterns()) {
        output << "forbidden_pattern=" << pattern << '\n';
    }
    return output.str();
}

std::string documented_problem_config() {
    return "# NeoThemis problem settings\n"
           "time_limit_ms=1000\n"
           "memory_limit_mb=256\n"
           "default_points=1\n"
           "checker=token\n"
           "\n"
           "# Checker options: token, custom:<path>, testlib:<path>\n"
           "# Examples: checker=custom, checker=testlib, or checker=testlib:checker.cpp\n"
           "# Custom checkers that include testlib.h must keep testlib.h in this problem folder.\n"
           "# In testlib mode, quitp(50, ...) awards 50% of this test's points.\n"
           "\n"
           "# Optional per-test overrides. Test names match the test folder names.\n"
           "# Example: test_points.1=2\n";
}

std::string compact_problem_config() {
    return "time_limit_ms=1000\n"
           "memory_limit_mb=256\n"
           "default_points=1\n"
           "checker=token\n";
}

void write_template(const fs::path& path, const std::string& contents, const std::string& action) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to " + action + ": " + path.string());
    }
    output << contents;
}

} // namespace

unsigned int parse_config_worker_count(const std::string& value, const std::string& key) {
    unsigned int count = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), count);
    if (value.empty() || result.ec != std::errc{} ||
        result.ptr != value.data() + value.size()) {
        throw std::runtime_error("invalid worker count for " + key + ": " + value +
                                 " (expected a non-negative decimal integer)");
    }
    return count;
}

std::string format_config_number(double value) {
    if (!std::isfinite(value) || value < 0.0) {
        throw std::runtime_error("cannot format a negative or non-finite configuration number");
    }
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                      std::chars_format::general);
    if (result.ec != std::errc{}) {
        throw std::runtime_error("failed to format numeric configuration value");
    }
    return std::string(buffer, result.ptr);
}

ConfigEntries read_config_entries(const fs::path& path) {
    return read_entries(path, "settings file");
}

ConfigValues config_values(const ConfigEntries& entries) {
    ConfigValues values;
    for (const auto& entry : entries) {
        values[entry.key].push_back(entry.value);
    }
    return values;
}

ConfigValues read_config_values(const fs::path& path) {
    return config_values(read_config_entries(path));
}

void write_config_entries(const fs::path& path, const ConfigEntries& entries) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to write settings file: " + path.string());
    }
    for (const auto& entry : entries) {
        output << entry.key << '=' << entry.value << '\n';
    }
}

void write_config_values(const fs::path& path, const ConfigValues& values) {
    ConfigEntries entries;
    for (const auto& [key, repeated_values] : values) {
        for (const auto& value : repeated_values) {
            entries.push_back({key, value, 0});
        }
    }
    write_config_entries(path, entries);
}

std::vector<std::string> default_forbidden_patterns() {
    return {"system(",
            "popen(",
            "fork(",
            "exec(",
            "#include <unistd.h>",
            "#include <sys/",
            "#include <windows.h>"};
}

ContestConfig load_contest_config(const fs::path& path, UnknownConfigKeyPolicy unknown_keys) {
    ContestConfig config;
    for (const auto& entry : read_entries(path, "settings file")) {
        const std::string& key = entry.key;
        const std::string& value = entry.value;
        if (key == "core") {
            config.core_name = value;
        } else if (key == "contestants_dir") {
            config.contestants_dir = value;
        } else if (key == "tests_dir") {
            config.tests_dir = value;
        } else if (key == "output_csv") {
            config.output_csv = value;
        } else if (key == "scoreboard_csv") {
            config.scoreboard_csv = value;
        } else if (key == "keep_workdir") {
            config.keep_workdir = parse_bool(value, key);
        } else if (key == "server_ranking_enabled") {
            config.server_ranking_enabled = parse_bool(value, key);
        } else if (key == "server_contestant_details_enabled") {
            config.server_contestant_details_enabled = parse_bool(value, key);
        } else if (key == "compiler") {
            config.compiler = value;
        } else if (key == "compile_flags") {
            config.compile_flags = value;
        } else if (key == "testlib_dir") {
            // Legacy global directory setting is preserved but unused. Testlib
            // checkers now keep testlib.h in their problem folder.
            config.preserved_entries.push_back(entry);
        } else if (key == "stack_limit_mb") {
            config.stack_limit_mb = parse_config_uint64(value, key);
        } else if (key == "parallel_jobs") {
            config.parallel_jobs = parse_config_worker_count(value, key);
        } else if (key == "compile_jobs") {
            config.compile_jobs = parse_config_worker_count(value, key);
        } else if (key == "test_jobs") {
            config.test_jobs = parse_config_worker_count(value, key);
        } else if (key == "timing_focused") {
            config.timing_focused = parse_bool(value, key);
        } else if (key == "forbidden_pattern") {
            config.forbidden_patterns.push_back(value);
        } else if (unknown_keys == UnknownConfigKeyPolicy::Reject) {
            throw std::runtime_error("unknown setting in " + path.filename().string() + ": " + key);
        } else {
            config.preserved_entries.push_back(entry);
        }
    }
    return config;
}

ContestConfig load_or_create_contest_config(const fs::path& contest_root, ConfigTemplateStyle style,
                                            UnknownConfigKeyPolicy unknown_keys) {
    const fs::path path = contest_root / kContestConfigFilename;
    if (!fs::exists(path)) {
        write_default_contest_config(path, style);
    }
    return load_contest_config(path, unknown_keys);
}

void write_contest_config(const fs::path& path, const ContestConfig& config) {
    ConfigEntries entries = {
        {"core", config.core_name, 0},
        {"contestants_dir", config.contestants_dir.string(), 0},
        {"tests_dir", config.tests_dir.string(), 0},
        {"output_csv", config.output_csv.string(), 0},
        {"scoreboard_csv", config.scoreboard_csv.string(), 0},
        {"keep_workdir", config.keep_workdir ? "true" : "false", 0},
        {"server_ranking_enabled", config.server_ranking_enabled ? "true" : "false", 0},
        {"server_contestant_details_enabled",
         config.server_contestant_details_enabled ? "true" : "false", 0},
        {"compiler", config.compiler, 0},
        {"compile_flags", config.compile_flags, 0},
        {"stack_limit_mb", std::to_string(config.stack_limit_mb), 0},
        {"parallel_jobs", std::to_string(config.parallel_jobs), 0},
        {"compile_jobs", std::to_string(config.compile_jobs), 0},
        {"test_jobs", std::to_string(config.test_jobs), 0},
        {"timing_focused", config.timing_focused ? "true" : "false", 0},
    };
    for (const auto& pattern : config.forbidden_patterns) {
        entries.push_back({"forbidden_pattern", pattern, 0});
    }
    entries.insert(entries.end(), config.preserved_entries.begin(), config.preserved_entries.end());
    write_config_entries(path, entries);
}

void apply_contest_config(const ContestConfig& config, JudgeOptions& options) {
    options.contestants_dir = config.contestants_dir;
    options.tests_dir = config.tests_dir;
    options.output_csv = config.output_csv;
    options.scoreboard_csv = config.scoreboard_csv;
    options.core_name = config.core_name;
    options.compiler = config.compiler;
    options.compile_flags = config.compile_flags;
    options.stack_limit_mb = config.stack_limit_mb;
    options.parallel_jobs = config.parallel_jobs;
    options.compile_jobs = config.compile_jobs;
    options.test_jobs = config.test_jobs;
    options.timing_focused = config.timing_focused;
    options.forbidden_patterns = config.forbidden_patterns;
    options.keep_workdir = config.keep_workdir;
}

void write_default_contest_config(const fs::path& path, ConfigTemplateStyle style) {
    write_template(path,
                   style == ConfigTemplateStyle::Documented ? documented_contest_config()
                                                            : compact_contest_config(),
                   style == ConfigTemplateStyle::Documented ? "create settings file" : "write");
}

ProblemConfig load_problem_config(const fs::path& path, UnknownConfigKeyPolicy unknown_keys) {
    ProblemConfig config;
    for (const auto& entry : read_entries(path, "problem settings file")) {
        const std::string& key = entry.key;
        const std::string& value = entry.value;
        if (key == "time_limit_ms") {
            config.time_limit_ms = parse_config_uint64(value, key);
        } else if (key == "memory_limit_mb") {
            config.memory_limit_mb = parse_config_uint64(value, key);
        } else if (key == "stack_limit_mb") {
            // Legacy per-problem stack settings are ignored; the setting is contest-wide.
            config.preserved_entries.push_back(entry);
        } else if (key == "default_points") {
            config.default_points = parse_config_points(value, key);
        } else if (key == "checker") {
            config.checker = value;
        } else if (key.rfind("test_points.", 0) == 0) {
            const std::string test_name = key.substr(std::string("test_points.").size());
            if (test_name.empty()) {
                throw std::runtime_error("empty test name in " + path.string());
            }
            if (!trim(value).empty()) {
                config.test_points[test_name] = parse_config_points(value, key);
            }
        } else if (unknown_keys == UnknownConfigKeyPolicy::Reject) {
            throw std::runtime_error("unknown problem setting in " + path.string() + ": " + key);
        } else {
            config.preserved_entries.push_back(entry);
        }
    }
    return config;
}

ProblemConfig load_or_create_problem_config(const fs::path& problem_root, ConfigTemplateStyle style,
                                            UnknownConfigKeyPolicy unknown_keys) {
    const fs::path path = problem_root / kProblemConfigFilename;
    if (!fs::exists(path)) {
        write_default_problem_config(path, style);
    }
    return load_problem_config(path, unknown_keys);
}

void write_problem_config(const fs::path& path, const ProblemConfig& config) {
    ConfigEntries entries = {
        {"time_limit_ms", std::to_string(config.time_limit_ms), 0},
        {"memory_limit_mb", std::to_string(config.memory_limit_mb), 0},
        {"default_points", format_config_number(config.default_points), 0},
        {"checker", config.checker, 0},
    };
    for (const auto& [test_name, points] : config.test_points) {
        entries.push_back({"test_points." + test_name, format_config_number(points), 0});
    }
    entries.insert(entries.end(), config.preserved_entries.begin(), config.preserved_entries.end());
    write_config_entries(path, entries);
}

void write_default_problem_config(const fs::path& path, ConfigTemplateStyle style) {
    write_template(path,
                   style == ConfigTemplateStyle::Documented ? documented_problem_config()
                                                            : compact_problem_config(),
                   style == ConfigTemplateStyle::Documented ? "create problem settings file"
                                                            : "create");
}

std::vector<std::string> test_point_keys(const std::string& test_name) {
    std::vector<std::string> keys{test_name};
    std::string digits;
    for (auto it = test_name.rbegin(); it != test_name.rend(); ++it) {
        if (!std::isdigit(static_cast<unsigned char>(*it))) {
            break;
        }
        digits.push_back(*it);
    }
    if (!digits.empty()) {
        std::reverse(digits.begin(), digits.end());
        keys.push_back(digits);
        const std::size_t first_non_zero = digits.find_first_not_of('0');
        keys.push_back(first_non_zero == std::string::npos ? "0" : digits.substr(first_non_zero));
    }

    std::vector<std::string> unique;
    for (const auto& key : keys) {
        if (std::find(unique.begin(), unique.end(), key) == unique.end()) {
            unique.push_back(key);
        }
    }
    return unique;
}

double points_for_test(const ProblemConfig& config, const std::string& test_name) {
    for (const auto& key : test_point_keys(test_name)) {
        const auto found = config.test_points.find(key);
        if (found != config.test_points.end()) {
            return found->second;
        }
    }
    return config.default_points;
}

} // namespace neothemis
