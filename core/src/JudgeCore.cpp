#include "neothemis/JudgeCore.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <tuple>

#ifndef _WIN32
#include <csignal>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace neothemis {
namespace {

struct ProcessResult {
    int exit_code = -1;
    bool timed_out = false;
    bool memory_exceeded = false;
    std::uint64_t elapsed_ms = 0;
};

struct ProblemSettings {
    std::uint64_t time_limit_ms = 1000;
    std::uint64_t memory_limit_mb = 256;
    double default_points = 1.0;
    std::string checker = "token";
    std::map<std::string, double> test_points;
};

struct ProblemContext {
    std::string name;
    fs::path dir;
    std::vector<fs::directory_entry> tests;
    ProblemSettings settings;
    fs::path checker_executable;
    std::string checker_error;
};

struct ContestantContext {
    std::string name;
    fs::path dir;
    std::map<std::string, fs::path> sources_by_problem;
};

struct PreparedSubmission {
    std::string contestant;
    const ProblemContext* problem = nullptr;
    fs::path executable;
    std::vector<TestResult> immediate_results;
    bool ready = false;
};

struct PrepTask {
    const ContestantContext* contestant = nullptr;
    const ProblemContext* problem = nullptr;
};

struct TestJob {
    std::string contestant;
    const ProblemContext* problem = nullptr;
    fs::directory_entry test;
    fs::path executable;
};

struct ProgressState {
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::size_t total_jobs = 0;
    std::size_t completed_jobs = 0;
    std::vector<std::string> worker_labels;
};

constexpr const char* kProblemSettingsFilename = "problem.conf";

std::string trim(const std::string& value) {
    std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string format_points(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(6) << value;
    std::string result = out.str();
    while (result.size() > 1 && result.back() == '0') {
        result.pop_back();
    }
    if (!result.empty() && result.back() == '.') {
        result.pop_back();
    }
    return result;
}

std::string format_elapsed(std::uint64_t ms) {
    std::uint64_t total_seconds = ms / 1000;
    std::uint64_t minutes = total_seconds / 60;
    std::uint64_t seconds = total_seconds % 60;
    std::ostringstream out;
    out << minutes << ':'
        << std::setw(2) << std::setfill('0') << seconds;
    return out.str();
}

std::string progress_bar(std::size_t done, std::size_t total) {
    constexpr std::size_t width = 24;
    std::size_t filled = total == 0 ? width : (done * width) / total;
    if (filled > width) {
        filled = width;
    }
    return "[" + std::string(filled, '#') + std::string(width - filled, '.') + "]";
}

std::string normalized_number(std::string value) {
    std::size_t first_non_zero = value.find_first_not_of('0');
    if (first_non_zero == std::string::npos) {
        return "0";
    }
    return value.substr(first_non_zero);
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
        keys.push_back(normalized_number(digits));
    }

    std::vector<std::string> unique_keys;
    for (const auto& key : keys) {
        if (std::find(unique_keys.begin(), unique_keys.end(), key) == unique_keys.end()) {
            unique_keys.push_back(key);
        }
    }
    keys = unique_keys;
    return keys;
}

std::string quote_path(const fs::path& path) {
    std::string s = path.string();
#ifdef _WIN32
    std::string out = "\"";
    for (char ch : s) {
        if (ch == '"') {
            out += "\\\"";
        } else {
            out += ch;
        }
    }
    out += "\"";
    return out;
#else
    std::string out = "'";
    for (char ch : s) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out += ch;
        }
    }
    out += "'";
    return out;
#endif
}

void apply_child_memory_limit(std::uint64_t memory_limit_mb) {
#ifndef _WIN32
    if (memory_limit_mb == 0) {
        return;
    }
    constexpr std::uint64_t bytes_per_mb = 1024ULL * 1024ULL;
    rlimit limit{};
    limit.rlim_cur = static_cast<rlim_t>(memory_limit_mb * bytes_per_mb);
    limit.rlim_max = limit.rlim_cur;
    if (setrlimit(RLIMIT_AS, &limit) != 0) {
        _exit(124);
    }
#else
    (void)memory_limit_mb;
#endif
}

bool signal_can_indicate_memory_limit(int signal_number) {
#ifndef _WIN32
    return signal_number == SIGABRT || signal_number == SIGKILL || signal_number == SIGSEGV;
#else
    (void)signal_number;
    return false;
#endif
}

ProcessResult run_command(const std::string& command,
                          const fs::path* working_dir,
                          const fs::path* stdin_path,
                          const fs::path* stdout_path,
                          const fs::path* stderr_path,
                          std::uint64_t timeout_ms,
                          std::uint64_t memory_limit_mb = 0) {
    fs::path absolute_working_dir;
    fs::path absolute_stdin;
    fs::path absolute_stdout;
    fs::path absolute_stderr;
    if (working_dir) {
        absolute_working_dir = fs::absolute(*working_dir);
        working_dir = &absolute_working_dir;
    }
    if (stdin_path) {
        absolute_stdin = fs::absolute(*stdin_path);
        stdin_path = &absolute_stdin;
    }
    if (stdout_path) {
        absolute_stdout = fs::absolute(*stdout_path);
        stdout_path = &absolute_stdout;
    }
    if (stderr_path) {
        absolute_stderr = fs::absolute(*stderr_path);
        stderr_path = &absolute_stderr;
    }

    auto begin = std::chrono::steady_clock::now();
#ifndef _WIN32
    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("fork failed");
    }

    if (pid == 0) {
        if (working_dir && chdir(working_dir->c_str()) != 0) {
            _exit(125);
        }
        if (stdin_path) {
            int fd = open(stdin_path->c_str(), O_RDONLY);
            if (fd < 0) _exit(126);
            dup2(fd, STDIN_FILENO);
            close(fd);
        }
        if (stdout_path) {
            int fd = open(stdout_path->c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) _exit(126);
            dup2(fd, STDOUT_FILENO);
            close(fd);
        }
        if (stderr_path) {
            int fd = open(stderr_path->c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) _exit(126);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        apply_child_memory_limit(memory_limit_mb);
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    int status = 0;
    bool timed_out = false;
    while (true) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            break;
        }
        if (done < 0) {
            throw std::runtime_error("waitpid failed");
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - begin).count();
        if (timeout_ms > 0 && static_cast<std::uint64_t>(elapsed) > timeout_ms) {
            timed_out = true;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        usleep(1000);
    }

    auto end = std::chrono::steady_clock::now();
    ProcessResult result;
    result.timed_out = timed_out;
    result.elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        int signal_number = WTERMSIG(status);
        result.exit_code = 128 + signal_number;
        result.memory_exceeded = !timed_out && memory_limit_mb > 0 &&
                                 signal_can_indicate_memory_limit(signal_number);
    }
    return result;
#else
    HANDLE child_stdin = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE child_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE child_stderr = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE opened_stdin = INVALID_HANDLE_VALUE;
    HANDLE opened_stdout = INVALID_HANDLE_VALUE;
    HANDLE opened_stderr = INVALID_HANDLE_VALUE;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;

    auto open_file = [&](const fs::path& p, DWORD access, DWORD creation) {
        return CreateFileA(p.string().c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &sa, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
    };

    if (stdin_path) {
        opened_stdin = open_file(*stdin_path, GENERIC_READ, OPEN_EXISTING);
        if (opened_stdin == INVALID_HANDLE_VALUE) throw std::runtime_error("failed to open stdin file");
        child_stdin = opened_stdin;
    }
    if (stdout_path) {
        opened_stdout = open_file(*stdout_path, GENERIC_WRITE, CREATE_ALWAYS);
        if (opened_stdout == INVALID_HANDLE_VALUE) throw std::runtime_error("failed to open stdout file");
        child_stdout = opened_stdout;
    }
    if (stderr_path) {
        opened_stderr = open_file(*stderr_path, GENERIC_WRITE, CREATE_ALWAYS);
        if (opened_stderr == INVALID_HANDLE_VALUE) throw std::runtime_error("failed to open stderr file");
        child_stderr = opened_stderr;
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = child_stdin;
    si.hStdOutput = child_stdout;
    si.hStdError = child_stderr;

    PROCESS_INFORMATION pi{};
    std::string cmd = "cmd.exe /C " + command;
    BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, working_dir ? working_dir->string().c_str() : nullptr, &si, &pi);
    if (!ok) {
        if (opened_stdin != INVALID_HANDLE_VALUE) CloseHandle(opened_stdin);
        if (opened_stdout != INVALID_HANDLE_VALUE) CloseHandle(opened_stdout);
        if (opened_stderr != INVALID_HANDLE_VALUE) CloseHandle(opened_stderr);
        throw std::runtime_error("CreateProcess failed");
    }

    DWORD wait_ms = timeout_ms == 0 ? INFINITE : static_cast<DWORD>(timeout_ms);
    DWORD wait_result = WaitForSingleObject(pi.hProcess, wait_ms);
    bool timed_out = wait_result == WAIT_TIMEOUT;
    if (timed_out) {
        TerminateProcess(pi.hProcess, 124);
        WaitForSingleObject(pi.hProcess, INFINITE);
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (opened_stdin != INVALID_HANDLE_VALUE) CloseHandle(opened_stdin);
    if (opened_stdout != INVALID_HANDLE_VALUE) CloseHandle(opened_stdout);
    if (opened_stderr != INVALID_HANDLE_VALUE) CloseHandle(opened_stderr);

    auto end = std::chrono::steady_clock::now();
    return ProcessResult{static_cast<int>(exit_code), timed_out, false,
                         static_cast<std::uint64_t>(
                             std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count())};
#endif
}

ProcessResult run_program(const std::vector<std::string>& args,
                          const fs::path* working_dir,
                          const fs::path* stderr_path,
                          std::uint64_t timeout_ms,
                          std::uint64_t memory_limit_mb) {
    if (args.empty() || args.front().empty()) {
        throw std::runtime_error("empty program command");
    }

#ifndef _WIN32
    fs::path absolute_working_dir;
    fs::path absolute_stderr;
    if (working_dir) {
        absolute_working_dir = fs::absolute(*working_dir);
        working_dir = &absolute_working_dir;
    }
    if (stderr_path) {
        absolute_stderr = fs::absolute(*stderr_path);
        stderr_path = &absolute_stderr;
    }

    auto begin = std::chrono::steady_clock::now();
    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("fork failed");
    }

    if (pid == 0) {
        if (working_dir && chdir(working_dir->c_str()) != 0) {
            _exit(125);
        }
        if (stderr_path) {
            int fd = open(stderr_path->c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) _exit(126);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        apply_child_memory_limit(memory_limit_mb);

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execv(argv.front(), argv.data());
        _exit(127);
    }

    int status = 0;
    bool timed_out = false;
    while (true) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            break;
        }
        if (done < 0) {
            throw std::runtime_error("waitpid failed");
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - begin).count();
        if (timeout_ms > 0 && static_cast<std::uint64_t>(elapsed) > timeout_ms) {
            timed_out = true;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        usleep(1000);
    }

    auto end = std::chrono::steady_clock::now();
    ProcessResult result;
    result.timed_out = timed_out;
    result.elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        int signal_number = WTERMSIG(status);
        result.exit_code = 128 + signal_number;
        result.memory_exceeded = !timed_out && memory_limit_mb > 0 &&
                                 signal_can_indicate_memory_limit(signal_number);
    }
    return result;
#else
    std::string command;
    for (const auto& arg : args) {
        if (!command.empty()) {
            command += ' ';
        }
        command += quote_path(arg);
    }
    return run_command(command, working_dir, nullptr, nullptr, stderr_path, timeout_ms,
                       memory_limit_mb);
#endif
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to read " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::vector<std::string> tokens(const std::string& text) {
    std::istringstream in(text);
    std::vector<std::string> result;
    std::string token;
    while (in >> token) {
        result.push_back(token);
    }
    return result;
}

std::vector<std::string> split_words(const std::string& text) {
    return tokens(text);
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool has_shell_metachar(const std::string& value) {
    return value.find_first_of("&;|<>$`\n\r") != std::string::npos;
}

std::string quote_command_token(const std::string& token) {
    if (token.empty() || has_shell_metachar(token)) {
        throw std::runtime_error("unsafe compiler setting: " + token);
    }
    return quote_path(fs::path(token));
}

std::string quote_command_tokens(const std::vector<std::string>& values) {
    std::string result;
    for (const auto& value : values) {
        if (!result.empty()) {
            result += ' ';
        }
        result += quote_command_token(value);
    }
    return result;
}

bool outputs_match(const fs::path& actual, const fs::path& expected) {
    return tokens(read_file(actual)) == tokens(read_file(expected));
}

void copy_file_alias(const fs::path& from, const fs::path& to) {
    std::error_code ignored;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ignored);
}

void copy_input_aliases(const fs::path& input,
                        const fs::path& run_dir,
                        const std::string& problem,
                        const std::string& test_name) {
    std::set<std::string> names;
    auto add_name = [&](const std::string& stem) {
        if (stem.empty()) {
            return;
        }
        std::string lower = lower_ascii(stem);
        std::string upper = stem;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
        names.insert(stem + ".inp");
        names.insert(stem + ".INP");
        names.insert(lower + ".inp");
        names.insert(lower + ".INP");
        names.insert(upper + ".inp");
        names.insert(upper + ".INP");
    };

    add_name(problem);
    add_name(test_name);
    add_name(input.stem().string());
    for (const auto& name : names) {
        copy_file_alias(input, run_dir / name);
    }
}

fs::path find_case_insensitive_file(const fs::path& dir, const std::set<std::string>& names) {
    std::set<std::string> lowered_names;
    for (const auto& name : names) {
        lowered_names.insert(lower_ascii(name));
    }

    if (!fs::exists(dir)) {
        return {};
    }
    for (const auto& entry : fs::directory_iterator(dir)) {
        std::error_code ec;
        if (fs::is_symlink(entry.symlink_status(ec)) || !entry.is_regular_file()) {
            continue;
        }
        std::string filename = lower_ascii(entry.path().filename().string());
        if (lowered_names.find(filename) != lowered_names.end()) {
            return entry.path();
        }
    }
    return {};
}

fs::path find_actual_output(const fs::path& run_dir,
                            const std::string& problem,
                            const std::string& test_name) {
    if (!fs::exists(run_dir) || !fs::is_directory(run_dir)) {
        return {};
    }

    std::set<std::string> candidates;
    auto add_output_names = [&](const std::string& stem) {
        if (stem.empty()) {
            return;
        }
        std::string lower = lower_ascii(stem);
        std::string upper = stem;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
        candidates.insert(stem + ".out");
        candidates.insert(stem + ".OUT");
        candidates.insert(lower + ".out");
        candidates.insert(lower + ".OUT");
        candidates.insert(upper + ".out");
        candidates.insert(upper + ".OUT");
    };

    add_output_names(problem);
    add_output_names(test_name);
    fs::path matched = find_case_insensitive_file(run_dir, candidates);
    if (!matched.empty()) {
        return matched;
    }

    std::vector<fs::path> out_files;
    std::error_code iter_ec;
    fs::directory_iterator begin(run_dir, iter_ec);
    if (iter_ec) {
        return {};
    }
    for (const auto& entry : begin) {
        std::error_code ec;
        if (!fs::is_symlink(entry.symlink_status(ec)) &&
            entry.is_regular_file() &&
            lower_ascii(entry.path().extension().string()) == ".out") {
            out_files.push_back(entry.path());
        }
    }
    if (out_files.size() == 1) {
        return out_files.front();
    }
    return {};
}

fs::path find_test_file(const fs::path& test_dir,
                        const std::string& problem,
                        const std::string& extension) {
    std::set<std::string> candidates;
    auto add_name = [&](const std::string& stem) {
        if (stem.empty()) {
            return;
        }
        std::string lower = lower_ascii(stem);
        std::string upper = stem;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
        candidates.insert(stem + extension);
        candidates.insert(lower + extension);
        candidates.insert(upper + extension);
    };

    add_name(problem);
    return find_case_insensitive_file(test_dir, candidates);
}

std::vector<fs::directory_entry> sorted_directories(const fs::path& path) {
    std::vector<fs::directory_entry> entries;
    if (!fs::exists(path)) {
        return entries;
    }
    for (const auto& entry : fs::directory_iterator(path)) {
        if (entry.is_directory()) {
            entries.push_back(entry);
        }
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return a.path().filename().string() < b.path().filename().string();
    });
    return entries;
}

std::vector<fs::path> sorted_sources(const fs::path& contestant_dir) {
    static const std::vector<std::string> extensions{".cpp", ".cc", ".cxx"};
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(contestant_dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (std::find(extensions.begin(), extensions.end(), ext) != extensions.end()) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::vector<ContestantContext> build_contestant_contexts(const fs::path& contestants_root) {
    std::vector<ContestantContext> contestants;
    for (const auto& contestant_entry : sorted_directories(contestants_root)) {
        ContestantContext contestant;
        contestant.name = contestant_entry.path().filename().string();
        contestant.dir = contestant_entry.path();
        for (const auto& source : sorted_sources(contestant.dir)) {
            std::string key = lower_ascii(source.stem().string());
            if (contestant.sources_by_problem.find(key) == contestant.sources_by_problem.end()) {
                contestant.sources_by_problem[key] = source;
            }
        }
        contestants.push_back(std::move(contestant));
    }
    return contestants;
}

fs::path find_source_for_problem(const ContestantContext& contestant, const std::string& problem) {
    auto found = contestant.sources_by_problem.find(lower_ascii(problem));
    return found == contestant.sources_by_problem.end() ? fs::path{} : found->second;
}

bool selection_allows(const std::vector<std::string>& selected, const std::string& name) {
    if (selected.empty()) {
        return true;
    }
    std::string wanted = lower_ascii(name);
    for (const auto& item : selected) {
        if (lower_ascii(item) == wanted) {
            return true;
        }
    }
    return false;
}

std::string first_existing_file_text(const std::initializer_list<fs::path>& paths) {
    for (const auto& path : paths) {
        if (fs::exists(path)) {
            try {
                std::string text = read_file(path);
                constexpr std::size_t max_len = 300;
                if (text.size() > max_len) {
                    text.resize(max_len);
                    text += "...";
                }
                return text;
            } catch (...) {
            }
        }
    }
    return {};
}

std::string first_forbidden_pattern(const fs::path& source,
                                    const std::vector<std::string>& forbidden_patterns) {
    std::string text = lower_ascii(read_file(source));
    for (const auto& pattern : forbidden_patterns) {
        if (!pattern.empty() && text.find(lower_ascii(pattern)) != std::string::npos) {
            return pattern;
        }
    }
    return {};
}

double points_for_test(const ProblemSettings& settings, const std::string& test_name) {
    for (const auto& key : test_point_keys(test_name)) {
        auto found = settings.test_points.find(key);
        if (found != settings.test_points.end()) {
            return found->second;
        }
    }
    return settings.default_points;
}

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

fs::path resolve_checker_source(const ProblemSettings& settings,
                                const JudgeOptions& options,
                                const fs::path& problem_dir) {
    if (settings.checker == "token") {
        return {};
    }

    if (starts_with(settings.checker, "testlib:")) {
        std::string checker_name = settings.checker.substr(std::string("testlib:").size());
        if (checker_name.empty() || checker_name.find('/') != std::string::npos ||
            checker_name.find('\\') != std::string::npos) {
            throw std::runtime_error("invalid testlib checker name: " + checker_name);
        }
        fs::path source = options.testlib_dir / "checkers" / (checker_name + ".cpp");
        if (!fs::exists(source)) {
            throw std::runtime_error("testlib checker not found: " + source.string());
        }
        return source;
    }

    if (settings.checker == "custom" || starts_with(settings.checker, "custom:")) {
        fs::path source = settings.checker == "custom"
                              ? fs::path("checker.cpp")
                              : fs::path(settings.checker.substr(std::string("custom:").size()));
        if (source.empty()) {
            source = "checker.cpp";
        }
        if (source.is_relative()) {
            source = problem_dir / source;
        }
        if (!fs::exists(source)) {
            throw std::runtime_error("custom checker not found: " + source.string());
        }
        return source;
    }

    throw std::runtime_error("unknown checker setting: " + settings.checker);
}

std::string read_checker_message(const fs::path& path) {
    std::string text = first_existing_file_text({path});
    constexpr std::size_t max_len = 300;
    if (text.size() > max_len) {
        text.resize(max_len);
        text += "...";
    }
    return text;
}

bool parse_first_number(const std::string& text, double& value) {
    std::istringstream in(text);
    std::string token;
    while (in >> token) {
        try {
            std::size_t parsed = 0;
            double candidate = std::stod(token, &parsed);
            if (parsed > 0) {
                value = candidate;
                return true;
            }
        } catch (...) {
        }
    }
    return false;
}

fs::path compile_checker(const ProblemSettings& settings,
                         const JudgeOptions& options,
                         const fs::path& problem_dir) {
    fs::path checker_source = resolve_checker_source(settings, options, problem_dir);
    if (checker_source.empty()) {
        return {};
    }

#ifdef _WIN32
    fs::path checker_executable = problem_dir / "checker.exe";
#else
    fs::path checker_executable = problem_dir / "checker";
#endif
    if (fs::exists(checker_executable)) {
        std::error_code checker_time_ec;
        std::error_code source_time_ec;
        auto checker_time = fs::last_write_time(checker_executable, checker_time_ec);
        auto source_time = fs::last_write_time(checker_source, source_time_ec);
        if (!checker_time_ec && !source_time_ec && checker_time >= source_time) {
#ifndef _WIN32
            std::error_code ec;
            fs::permissions(checker_executable,
                            fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                            fs::perm_options::add,
                            ec);
#endif
            return checker_executable;
        }
    }

    fs::path compile_log = problem_dir / "checker-compile.err";
    std::string compile_cmd = quote_command_token(options.compiler) + " " +
                              quote_command_tokens(split_words(options.compile_flags)) + " " +
                              "-I " + quote_path(options.testlib_dir) + " " +
                              quote_path(checker_source) + " -o " + quote_path(checker_executable);
    ProcessResult compile = run_command(compile_cmd, nullptr, nullptr, nullptr, &compile_log, 0);
    if (compile.exit_code != 0) {
        throw std::runtime_error("checker compile failed: " + read_checker_message(compile_log));
    }
    return checker_executable;
}

void write_default_problem_settings(const fs::path& settings_path,
                                    const std::vector<fs::directory_entry>& tests) {
    (void)tests;
    std::ofstream out(settings_path);
    if (!out) {
        throw std::runtime_error("failed to create problem settings file: " +
                                 settings_path.string());
    }

    out << "# NeoThemis problem settings\n"
        << "time_limit_ms=1000\n"
        << "memory_limit_mb=256\n"
        << "default_points=1\n"
        << "checker=token\n"
        << "\n"
        << "# Checker options: token, testlib:<name>, custom:<path-in-this-problem-folder>\n"
        << "# Examples: checker=testlib:wcmp or checker=custom:checker.cpp\n"
        << "\n"
        << "# Optional per-test overrides. Test names match the test folder names.\n"
        << "# Example: test_points.1=2\n";
}

void emit_progress_summary(const JudgeOptions& options,
                           const ProgressState& state,
                           const std::string& phase) {
    if (!options.progress) {
        return;
    }
    auto now = std::chrono::steady_clock::now();
    auto elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - state.started).count());
    std::ostringstream out;
    out << "progress " << phase << " "
        << progress_bar(state.completed_jobs, state.total_jobs) << " "
        << state.completed_jobs << "/" << state.total_jobs
        << " elapsed " << format_elapsed(elapsed);
    for (std::size_t i = 0; i < state.worker_labels.size(); ++i) {
        out << " | core " << (i + 1) << ": " << state.worker_labels[i];
    }
    options.progress(out.str());
}

void apply_problem_setting(ProblemSettings& settings,
                           const fs::path& settings_path,
                           const std::string& key,
                           const std::string& value) {
    if (key == "time_limit_ms") {
        settings.time_limit_ms = static_cast<std::uint64_t>(std::stoull(value));
    } else if (key == "memory_limit_mb") {
        settings.memory_limit_mb = static_cast<std::uint64_t>(std::stoull(value));
    } else if (key == "default_points") {
        settings.default_points = std::stod(value);
    } else if (key == "checker") {
        settings.checker = value;
    } else {
        constexpr const char* prefix = "test_points.";
        std::string prefix_text(prefix);
        if (key.rfind(prefix_text, 0) == 0) {
            std::string test_name = key.substr(prefix_text.size());
            if (test_name.empty()) {
                throw std::runtime_error("empty test name in " + settings_path.string());
            }
            settings.test_points[test_name] = std::stod(value);
        } else {
            throw std::runtime_error("unknown problem setting in " +
                                     settings_path.string() + ": " + key);
        }
    }
}

ProblemSettings load_or_create_problem_settings(const fs::path& problem_dir,
                                                const std::vector<fs::directory_entry>& tests) {
    fs::path settings_path = problem_dir / kProblemSettingsFilename;
    if (!fs::exists(settings_path)) {
        write_default_problem_settings(settings_path, tests);
    }

    ProblemSettings settings;
    std::ifstream in(settings_path);
    if (!in) {
        throw std::runtime_error("failed to open problem settings file: " +
                                 settings_path.string());
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
        apply_problem_setting(settings, settings_path, key, value);
    }

    return settings;
}

std::vector<TestResult> rows_for_problem_tests(const std::string& contestant,
                                               const ProblemContext& problem,
                                               Verdict verdict,
                                               int exit_code,
                                               std::uint64_t time_ms,
                                               const std::string& message) {
    std::vector<TestResult> rows;
    for (const auto& test_entry : problem.tests) {
        std::string test_name = test_entry.path().filename().string();
        rows.push_back(TestResult{contestant, problem.name, test_name, verdict, time_ms,
                                  exit_code, points_for_test(problem.settings, test_name),
                                  0.0, message});
    }
    return rows;
}

PreparedSubmission prepare_submission(const JudgeOptions& options,
                                      const fs::path& work_root,
                                      const ContestantContext& contestant,
                                      const ProblemContext& problem) {
    PreparedSubmission prepared;
    prepared.contestant = contestant.name;
    prepared.problem = &problem;

    if (!problem.checker_error.empty()) {
        prepared.immediate_results =
            rows_for_problem_tests(prepared.contestant, problem, Verdict::InternalError,
                                   -1, 0, problem.checker_error);
        return prepared;
    }

    fs::path source = find_source_for_problem(contestant, problem.name);
    if (source.empty()) {
        prepared.immediate_results =
            rows_for_problem_tests(prepared.contestant, problem, Verdict::MissingSource,
                                   -1, 0, "source file named " + problem.name +
                                               ".cpp/.cc/.cxx not found");
        return prepared;
    }

    std::string forbidden_pattern = first_forbidden_pattern(source, options.forbidden_patterns);
    if (!forbidden_pattern.empty()) {
        prepared.immediate_results =
            rows_for_problem_tests(prepared.contestant, problem, Verdict::SecurityViolation,
                                   -1, 0, "forbidden source pattern: " + forbidden_pattern);
        return prepared;
    }

    fs::path build_dir = work_root / prepared.contestant / problem.name;
    fs::create_directories(build_dir);
#ifdef _WIN32
    prepared.executable = build_dir / (problem.name + ".exe");
#else
    prepared.executable = build_dir / problem.name;
#endif
    fs::path compile_log = build_dir / "compile.err";
    std::string compile_cmd = quote_command_token(options.compiler) + " " +
                              quote_command_tokens(split_words(options.compile_flags)) + " " +
                              quote_path(source) + " -o " + quote_path(prepared.executable);
    ProcessResult compile = run_command(compile_cmd, nullptr, nullptr, nullptr, &compile_log, 0);
    if (compile.exit_code != 0) {
        prepared.immediate_results =
            rows_for_problem_tests(prepared.contestant, problem, Verdict::CompileError,
                                   compile.exit_code, compile.elapsed_ms,
                                   first_existing_file_text({compile_log}));
        return prepared;
    }

    prepared.ready = true;
    return prepared;
}

TestResult judge_test_job(const TestJob& job) {
    const ProblemContext& problem = *job.problem;
    std::string test_name = job.test.path().filename().string();
    fs::path input = find_test_file(job.test.path(), problem.name, ".inp");
    fs::path expected = find_test_file(job.test.path(), problem.name, ".out");
    fs::path build_dir = job.executable.parent_path();
    fs::path run_dir = build_dir / test_name;
    fs::path run_log = run_dir / (test_name + ".err");

    TestResult row;
    row.contestant = job.contestant;
    row.problem = problem.name;
    row.test = test_name;
    row.max_points = points_for_test(problem.settings, test_name);

    if (input.empty() || expected.empty()) {
        row.verdict = Verdict::InternalError;
        row.message = "missing " + problem.name + ".inp or " + problem.name + ".out";
        return row;
    }

    fs::remove_all(run_dir);
    fs::create_directories(run_dir);
    copy_input_aliases(input, run_dir, problem.name, test_name);

    fs::path executable_path = fs::absolute(job.executable);
    ProcessResult run = run_program({executable_path.string()}, &run_dir, &run_log,
                                    problem.settings.time_limit_ms,
                                    problem.settings.memory_limit_mb);
    row.time_ms = run.elapsed_ms;
    row.exit_code = run.exit_code;
    if (run.timed_out) {
        row.verdict = Verdict::TimeLimitExceeded;
        row.message = "time limit exceeded";
    } else if (run.memory_exceeded) {
        row.verdict = Verdict::MemoryLimitExceeded;
        row.message = "memory limit exceeded";
    } else if (run.exit_code != 0) {
        row.verdict = Verdict::RuntimeError;
        row.message = first_existing_file_text({run_log});
    } else {
        fs::path actual = find_actual_output(run_dir, problem.name, test_name);
        if (actual.empty()) {
            row.verdict = Verdict::WrongAnswer;
            row.message = "output file not found";
        } else if (problem.checker_executable.empty()) {
            if (outputs_match(actual, expected)) {
                row.verdict = Verdict::Accepted;
                row.earned_points = row.max_points;
            } else {
                row.verdict = Verdict::WrongAnswer;
            }
        } else {
            fs::path checker_log = run_dir / (test_name + ".checker.err");
            fs::path checker_executable = fs::absolute(problem.checker_executable);
            ProcessResult checker =
                run_program({checker_executable.string(), input.string(), actual.string(),
                             expected.string()},
                            nullptr, &checker_log, problem.settings.time_limit_ms,
                            problem.settings.memory_limit_mb);
            row.exit_code = checker.exit_code;
            row.message = read_checker_message(checker_log);
            if (checker.timed_out) {
                row.verdict = Verdict::InternalError;
                row.message = "checker time limit exceeded";
            } else if (checker.memory_exceeded) {
                row.verdict = Verdict::InternalError;
                row.message = "checker memory limit exceeded";
            } else if (checker.exit_code == 0) {
                row.verdict = Verdict::Accepted;
                row.earned_points = row.max_points;
            } else if (checker.exit_code == 1) {
                row.verdict = Verdict::WrongAnswer;
            } else if (checker.exit_code == 7) {
                double checker_points = 0.0;
                if (!parse_first_number(row.message, checker_points)) {
                    row.verdict = Verdict::InternalError;
                    row.message = "checker returned points but no numeric score was found";
                } else {
                    row.verdict = checker_points > 0.0 ? Verdict::Accepted : Verdict::WrongAnswer;
                    row.earned_points = std::max(0.0, std::min(row.max_points, checker_points));
                }
            } else {
                row.verdict = Verdict::InternalError;
                if (row.message.empty()) {
                    row.message = "checker failed";
                }
            }
        }
    }
    return row;
}

unsigned int configured_worker_count(const JudgeOptions& options) {
    unsigned int worker_count = options.parallel_jobs;
    if (worker_count == 0) {
        worker_count = std::thread::hardware_concurrency();
        if (worker_count == 0) {
            worker_count = 1;
        }
    }
    return std::max(1U, worker_count);
}

fs::path work_root_for_contest(const JudgeOptions& options) {
    if (options.keep_workdir) {
        return options.contest_root / ".neothemis-work";
    }
    std::string contest_key = fs::absolute(options.contest_root).string();
    std::size_t hash = std::hash<std::string>{}(contest_key);
    return fs::temp_directory_path() / ("neothemis-work-" + std::to_string(hash));
}

class BuiltinJudgeCore final : public JudgeCore {
public:
    std::vector<TestResult> judge(const JudgeOptions& options) override {
        fs::path contestants_root = options.contest_root / options.contestants_dir;
        fs::path tests_root = options.contest_root / options.tests_dir;
        fs::path work_root = work_root_for_contest(options);

        if (!fs::exists(contestants_root)) {
            throw std::runtime_error("contestants directory not found: " + contestants_root.string());
        }
        if (!fs::exists(tests_root)) {
            throw std::runtime_error("tests directory not found: " + tests_root.string());
        }

        if (!options.keep_workdir) {
            std::error_code ignored;
            fs::remove_all(work_root, ignored);
        }
        fs::create_directories(work_root);
        std::vector<TestResult> results;
        std::vector<ProblemContext> problems;
        for (const auto& problem_entry : sorted_directories(tests_root)) {
            ProblemContext problem;
            problem.name = problem_entry.path().filename().string();
            if (!selection_allows(options.selected_problems, problem.name)) {
                continue;
            }
            problem.dir = problem_entry.path();
            problem.tests = sorted_directories(problem.dir);
            problem.settings = load_or_create_problem_settings(problem.dir, problem.tests);
            try {
                problem.checker_executable = compile_checker(problem.settings, options, problem.dir);
            } catch (const std::exception& ex) {
                problem.checker_error = ex.what();
            }
            problems.push_back(problem);
        }
        auto contestants = build_contestant_contexts(contestants_root);
        contestants.erase(std::remove_if(contestants.begin(), contestants.end(),
                                         [&](const ContestantContext& contestant) {
                                             return !selection_allows(options.selected_contestants,
                                                                      contestant.name);
                                         }),
                          contestants.end());
        unsigned int base_worker_count = configured_worker_count(options);

        std::vector<PrepTask> prep_tasks;
        for (const auto& contestant : contestants) {
            for (const auto& problem : problems) {
                prep_tasks.push_back(PrepTask{&contestant, &problem});
            }
        }

        std::vector<TestJob> prepared_jobs;
        if (!prep_tasks.empty()) {
            unsigned int prepare_worker_count =
                std::min<unsigned int>(base_worker_count,
                                       static_cast<unsigned int>(prep_tasks.size()));
            ProgressState prepare_state;
            prepare_state.total_jobs = prep_tasks.size();
            prepare_state.worker_labels.assign(prepare_worker_count, "idle");
            std::atomic<std::size_t> next_prep_task{0};
            std::atomic<bool> prepare_progress_done{false};
            std::mutex prepare_results_mutex;
            std::mutex prepared_jobs_mutex;
            std::mutex prepare_progress_mutex;

            auto prepare_worker = [&](unsigned int worker_id) {
                while (true) {
                    std::size_t index = next_prep_task.fetch_add(1);
                    if (index >= prep_tasks.size()) {
                        std::lock_guard<std::mutex> progress_lock(prepare_progress_mutex);
                        prepare_state.worker_labels[worker_id - 1] = "done";
                        return;
                    }

                    const PrepTask& task = prep_tasks[index];
                    {
                        std::lock_guard<std::mutex> progress_lock(prepare_progress_mutex);
                        prepare_state.worker_labels[worker_id - 1] =
                            task.contestant->name + "/" + task.problem->name;
                    }

                    PreparedSubmission prepared =
                        prepare_submission(options, work_root, *task.contestant, *task.problem);
                    {
                        std::lock_guard<std::mutex> lock(prepare_results_mutex);
                        results.insert(results.end(), prepared.immediate_results.begin(),
                                       prepared.immediate_results.end());
                    }
                    if (prepared.ready) {
                        std::lock_guard<std::mutex> lock(prepared_jobs_mutex);
                        for (const auto& test : task.problem->tests) {
                            prepared_jobs.push_back(
                                TestJob{prepared.contestant, task.problem, test, prepared.executable});
                        }
                    }
                    {
                        std::lock_guard<std::mutex> progress_lock(prepare_progress_mutex);
                        ++prepare_state.completed_jobs;
                    }
                }
            };

            std::thread prepare_progress_monitor;
            if (options.progress && prepare_worker_count > 0) {
                prepare_progress_monitor = std::thread([&]() {
                    while (!prepare_progress_done.load()) {
                        {
                            std::lock_guard<std::mutex> lock(prepare_progress_mutex);
                            emit_progress_summary(options, prepare_state, "prepare");
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    }
                    {
                        std::lock_guard<std::mutex> lock(prepare_progress_mutex);
                        emit_progress_summary(options, prepare_state, "prepare");
                    }
                });
            }

            std::vector<std::thread> prepare_workers;
            for (unsigned int i = 0; i < prepare_worker_count; ++i) {
                prepare_workers.emplace_back(prepare_worker, i + 1);
            }
            for (auto& thread : prepare_workers) {
                thread.join();
            }
            prepare_progress_done.store(true);
            if (prepare_progress_monitor.joinable()) {
                prepare_progress_monitor.join();
            }
        }

        std::queue<TestJob> jobs;
        for (const auto& job : prepared_jobs) {
            jobs.push(job);
        }

        unsigned int worker_count = jobs.empty()
                                        ? 0
                                        : std::min<unsigned int>(base_worker_count,
                                                                 static_cast<unsigned int>(jobs.size()));

        ProgressState progress_state;
        progress_state.total_jobs = jobs.size();
        progress_state.worker_labels.assign(worker_count, "idle");
        std::mutex jobs_mutex;
        std::mutex results_mutex;
        std::mutex progress_mutex;
        std::atomic<bool> progress_done{false};
        auto worker = [&](unsigned int worker_id) {
            while (true) {
                TestJob job;
                {
                    std::lock_guard<std::mutex> lock(jobs_mutex);
                    if (jobs.empty()) {
                        if (worker_id > 0) {
                            std::lock_guard<std::mutex> progress_lock(progress_mutex);
                            progress_state.worker_labels[worker_id - 1] = "done";
                        }
                        return;
                    }
                    job = jobs.front();
                    jobs.pop();
                }

                std::string label = job.contestant + "/" + job.problem->name + "/" +
                                    job.test.path().filename().string();
                {
                    std::lock_guard<std::mutex> lock(progress_mutex);
                    progress_state.worker_labels[worker_id - 1] = label;
                }

                TestResult job_result = judge_test_job(job);
                {
                    std::lock_guard<std::mutex> lock(results_mutex);
                    results.push_back(std::move(job_result));
                }
                {
                    std::lock_guard<std::mutex> lock(progress_mutex);
                    ++progress_state.completed_jobs;
                }
            }
        };

        std::thread progress_monitor;
        if (options.progress && worker_count > 0) {
            progress_monitor = std::thread([&]() {
                while (!progress_done.load()) {
                    {
                        std::lock_guard<std::mutex> lock(progress_mutex);
                        emit_progress_summary(options, progress_state, "judge");
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                {
                    std::lock_guard<std::mutex> lock(progress_mutex);
                    emit_progress_summary(options, progress_state, "judge");
                }
            });
        }

        std::vector<std::thread> workers;
        for (unsigned int i = 0; i < worker_count; ++i) {
            workers.emplace_back(worker, i + 1);
        }
        for (auto& thread : workers) {
            thread.join();
        }
        progress_done.store(true);
        if (progress_monitor.joinable()) {
            progress_monitor.join();
        }

        std::sort(results.begin(), results.end(), [](const TestResult& a, const TestResult& b) {
            return std::tie(a.contestant, a.problem, a.test) < std::tie(b.contestant, b.problem, b.test);
        });

        if (!options.keep_workdir) {
            std::error_code ignored;
            fs::remove_all(work_root, ignored);
        }
        return results;
    }
};

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
    if (!needs_quotes) {
        return escaped;
    }
    return "\"" + escaped + "\"";
}

} // namespace

std::string to_string(Verdict verdict) {
    switch (verdict) {
        case Verdict::Accepted: return "AC";
        case Verdict::WrongAnswer: return "WA";
        case Verdict::CompileError: return "CE";
        case Verdict::RuntimeError: return "RE";
        case Verdict::TimeLimitExceeded: return "TLE";
        case Verdict::MemoryLimitExceeded: return "MLE";
        case Verdict::MissingSource: return "MS";
        case Verdict::SecurityViolation: return "SV";
        case Verdict::InternalError: return "IE";
    }
    return "IE";
}

std::unique_ptr<JudgeCore> make_judge_core(const std::string& name) {
    if (name == "builtin") {
        return std::make_unique<BuiltinJudgeCore>();
    }
    throw std::runtime_error("unknown judge core: " + name);
}

void write_csv(std::ostream& output, const std::vector<TestResult>& results) {
    output << "contestant,problem,test,verdict,time_ms,exit_code,max_points,earned_points,message\n";
    for (const auto& row : results) {
        output << csv_escape(row.contestant) << ','
               << csv_escape(row.problem) << ','
               << csv_escape(row.test) << ','
               << csv_escape(to_string(row.verdict)) << ','
               << row.time_ms << ','
               << row.exit_code << ','
               << format_points(row.max_points) << ','
               << format_points(row.earned_points) << ','
               << csv_escape(row.message) << '\n';
    }
}

std::string scoreboard_status(Verdict verdict) {
    switch (verdict) {
        case Verdict::CompileError:
            return "CE";
        case Verdict::MissingSource:
            return "MS";
        default:
            return {};
    }
}

int scoreboard_status_priority(const std::string& status) {
    if (status == "CE") {
        return 2;
    }
    if (status == "MS") {
        return 1;
    }
    return 0;
}

void write_scoreboard_csv(std::ostream& output, const std::vector<TestResult>& results) {
    std::set<std::string> contestants;
    std::set<std::string> problems;
    std::map<std::string, std::map<std::string, double>> scores;
    std::map<std::string, std::map<std::string, std::string>> statuses;

    for (const auto& row : results) {
        contestants.insert(row.contestant);
        problems.insert(row.problem);
        scores[row.contestant][row.problem] += row.earned_points;
        std::string status = scoreboard_status(row.verdict);
        if (!status.empty() &&
            scoreboard_status_priority(status) >
                scoreboard_status_priority(statuses[row.contestant][row.problem])) {
            statuses[row.contestant][row.problem] = status;
        }
    }

    output << "contestant";
    for (const auto& problem : problems) {
        output << ',' << csv_escape(problem);
    }
    output << ",total\n";

    for (const auto& contestant : contestants) {
        double total = 0.0;
        output << csv_escape(contestant);
        for (const auto& problem : problems) {
            double score = scores[contestant][problem];
            total += score;
            const std::string& status = statuses[contestant][problem];
            if (score == 0.0 && !status.empty()) {
                output << ',' << csv_escape(status + "(0)");
            } else {
                output << ',' << format_points(score);
            }
        }
        output << ',' << format_points(total) << '\n';
    }
}

} // namespace neothemis
