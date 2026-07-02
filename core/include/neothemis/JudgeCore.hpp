#pragma once

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
    WrongAnswer,
    CompileError,
    RuntimeError,
    TimeLimitExceeded,
    MemoryLimitExceeded,
    MissingSource,
    SecurityViolation,
    InternalError
};

struct JudgeOptions {
    std::filesystem::path contest_root;
    std::filesystem::path contestants_dir = "contestants";
    std::filesystem::path tests_dir = "tests";
    std::filesystem::path output_csv = "results.csv";
    std::filesystem::path scoreboard_csv = "scoreboard.csv";
    std::string core_name = "builtin";
    std::string compiler = "g++";
    std::string compile_flags = "-std=c++17 -O2 -pipe";
    std::filesystem::path testlib_dir = "testlib";
    unsigned int parallel_jobs = 0;
    std::vector<std::string> selected_problems;
    std::vector<std::string> selected_contestants;
    std::vector<std::string> forbidden_patterns;
    std::function<void(const std::string&)> progress;
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
std::unique_ptr<JudgeCore> make_judge_core(const std::string& name);
ContestOverview inspect_contest(const JudgeOptions& options);
void write_csv(std::ostream& output, const std::vector<TestResult>& results);
void write_scoreboard_csv(std::ostream& output, const std::vector<TestResult>& results);

} // namespace neothemis
