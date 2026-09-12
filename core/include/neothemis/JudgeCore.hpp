#pragma once

#include "neothemis/Csv.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace neothemis {

enum class Verdict {
    Accepted,
    Partial,
    WrongAnswer,
    CompileError,
    RuntimeError,
    TimeLimitExceeded,
    MemoryLimitExceeded,
    MissingSource,
    SecurityViolation,
    InternalError
};

enum class ExecutionSecurity {
    Required,
    ExplicitlyUnsafe
};

struct TestResult;

struct JudgeOptions {
    std::filesystem::path contest_root;
    std::filesystem::path contestants_dir = "contestants";
    std::filesystem::path tests_dir = "tests";
    std::filesystem::path output_csv = "results.csv";
    std::filesystem::path scoreboard_csv = "scoreboard.csv";
    std::string core_name = "builtin";
    std::string compiler = "g++";
    std::string compile_flags = "-std=c++14 -O2 -pipe";
    std::uint64_t stack_limit_mb = 64;
    unsigned int parallel_jobs = 0;
    // Zero inherits parallel_jobs. Timing-focused runs always execute tests serially.
    unsigned int compile_jobs = 0;
    unsigned int test_jobs = 0;
    bool timing_focused = false;
    std::vector<std::string> selected_problems;
    std::vector<std::string> selected_contestants;
    std::vector<std::string> forbidden_patterns;
    std::function<void(const std::string&)> progress;
    std::function<void(const TestResult&)> result;
    std::function<bool()> should_cancel;
    ExecutionSecurity execution_security = ExecutionSecurity::Required;
    bool keep_workdir = false;
};

struct TestResult {
    std::string contestant;
    std::string problem;
    std::string test;
    Verdict verdict = Verdict::InternalError;
    std::uint64_t time_ms = 0;
    int exit_code = -1;
    double max_points = 0.0;
    double earned_points = 0.0;
    std::string message;
};

struct ContestOverview {
    std::vector<std::string> contestants;
    std::vector<std::string> problems;
    std::vector<std::vector<bool>> has_source;
};

class JudgeCore {
public:
    virtual ~JudgeCore() = default;
    virtual std::vector<TestResult> judge(const JudgeOptions& options) = 0;
};

std::string to_string(Verdict verdict);
Verdict verdict_from_string(const std::string& value);
std::unique_ptr<JudgeCore> make_judge_core(const std::string& name);
bool secure_sandbox_available(std::string* reason = nullptr);
void validate_judge_paths(const JudgeOptions& options);
ContestOverview inspect_contest(const JudgeOptions& options);
void write_csv(std::ostream& output, const std::vector<TestResult>& results);
void write_scoreboard_csv(std::ostream& output, const std::vector<TestResult>& results);
void write_scoreboard_csv_from_results(std::ostream& output, const CsvTable& rows);

} // namespace neothemis
