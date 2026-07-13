#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace neothemis {

inline constexpr const char* kContestConfigFilename = "neothemis.conf";
inline constexpr const char* kProblemConfigFilename = "problem.conf";

struct ConfigEntry {
    std::string key;
    std::string value;
    std::size_t line_number = 0;
};

using ConfigEntries = std::vector<ConfigEntry>;
using ConfigValues = std::map<std::string, std::vector<std::string>>;

ConfigEntries read_config_entries(const std::filesystem::path& path);
ConfigValues config_values(const ConfigEntries& entries);
ConfigValues read_config_values(const std::filesystem::path& path);
void write_config_entries(const std::filesystem::path& path, const ConfigEntries& entries);
void write_config_values(const std::filesystem::path& path, const ConfigValues& values);

enum class ConfigTemplateStyle { Compact, Documented };

enum class UnknownConfigKeyPolicy { Reject, Ignore };

std::vector<std::string> default_forbidden_patterns();

struct ContestConfig {
    std::filesystem::path contestants_dir = "contestants";
    std::filesystem::path tests_dir = "tests";
    std::filesystem::path output_csv = "results.csv";
    std::filesystem::path scoreboard_csv = "scoreboard.csv";
    std::string core_name = "builtin";
    std::string compiler = "g++";
    std::string compile_flags = "-std=c++14 -O2 -pipe";
    std::uint64_t stack_limit_mb = 64;
    unsigned int parallel_jobs = 0;
    std::vector<std::string> forbidden_patterns;
    bool keep_workdir = false;
    bool server_ranking_enabled = false;
    bool server_contestant_details_enabled = false;
    // Entries accepted under UnknownConfigKeyPolicy::Ignore, plus harmless
    // legacy entries. Typed editors can round-trip these without interpreting
    // or silently deleting settings introduced by a newer NeoThemis version.
    ConfigEntries preserved_entries;
};

struct JudgeOptions;

ContestConfig
load_contest_config(const std::filesystem::path& path,
                    UnknownConfigKeyPolicy unknown_keys = UnknownConfigKeyPolicy::Reject);
ContestConfig
load_or_create_contest_config(const std::filesystem::path& contest_root,
                              ConfigTemplateStyle style = ConfigTemplateStyle::Documented,
                              UnknownConfigKeyPolicy unknown_keys = UnknownConfigKeyPolicy::Reject);
void write_contest_config(const std::filesystem::path& path, const ContestConfig& config);
void apply_contest_config(const ContestConfig& config, JudgeOptions& options);
void write_default_contest_config(const std::filesystem::path& path,
                                  ConfigTemplateStyle style = ConfigTemplateStyle::Documented);

struct ProblemConfig {
    std::uint64_t time_limit_ms = 1000;
    std::uint64_t memory_limit_mb = 256;
    double default_points = 1.0;
    std::string checker = "token";
    std::map<std::string, double> test_points;
    ConfigEntries preserved_entries;
};

ProblemConfig
load_problem_config(const std::filesystem::path& path,
                    UnknownConfigKeyPolicy unknown_keys = UnknownConfigKeyPolicy::Reject);
ProblemConfig
load_or_create_problem_config(const std::filesystem::path& problem_root,
                              ConfigTemplateStyle style = ConfigTemplateStyle::Documented,
                              UnknownConfigKeyPolicy unknown_keys = UnknownConfigKeyPolicy::Reject);
void write_problem_config(const std::filesystem::path& path, const ProblemConfig& config);
void write_default_problem_config(const std::filesystem::path& path,
                                  ConfigTemplateStyle style = ConfigTemplateStyle::Documented);

std::vector<std::string> test_point_keys(const std::string& test_name);
double points_for_test(const ProblemConfig& config, const std::string& test_name);

} // namespace neothemis
