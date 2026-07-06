#include "neothemis/JudgeCore.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
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

#ifndef _WIN32
void apply_child_limits(std::uint64_t memory_limit_mb, std::uint64_t stack_limit_mb) {
    constexpr std::uint64_t bytes_per_mb = 1024ULL * 1024ULL;
    if (memory_limit_mb > 0) {
        rlimit limit{};
        limit.rlim_cur = static_cast<rlim_t>(memory_limit_mb * bytes_per_mb);
        limit.rlim_max = limit.rlim_cur;
        if (setrlimit(RLIMIT_AS, &limit) != 0) {
            _exit(124);
        }
    }
    if (stack_limit_mb > 0) {
        rlimit limit{};
        limit.rlim_cur = static_cast<rlim_t>(stack_limit_mb * bytes_per_mb);
        limit.rlim_max = limit.rlim_cur;
        if (setrlimit(RLIMIT_STACK, &limit) != 0) {
            _exit(124);
        }
    }
}

bool signal_can_indicate_memory_limit(int signal_number) {
    return signal_number == SIGKILL;
}

#if defined(__linux__)
unsigned int linux_performance_core_count() {
    const fs::path cpu_root = "/sys/devices/system/cpu";
    std::map<std::pair<std::string, std::string>, std::uint64_t> core_frequencies;
    std::error_code directory_error;
    fs::directory_iterator entries(cpu_root, directory_error);
    if (directory_error) {
        return 0;
    }

    for (const auto& entry : entries) {
        const std::string name = entry.path().filename().string();
        if (name.size() <= 3 || name.rfind("cpu", 0) != 0 ||
            !std::all_of(name.begin() + 3, name.end(), [](unsigned char ch) {
                return std::isdigit(ch) != 0;
            })) {
            continue;
        }

        auto read_value = [&](const fs::path& path, std::string& value) {
            std::ifstream input(path);
            return static_cast<bool>(input >> value);
        };
        std::string package_id;
        std::string core_id;
        std::string maximum_frequency;
        if (!read_value(entry.path() / "topology" / "physical_package_id", package_id) ||
            !read_value(entry.path() / "topology" / "core_id", core_id) ||
            !read_value(entry.path() / "cpufreq" / "cpuinfo_max_freq", maximum_frequency)) {
            continue;
        }
        try {
            const std::uint64_t frequency = std::stoull(maximum_frequency);
            auto& stored = core_frequencies[{package_id, core_id}];
            stored = std::max(stored, frequency);
        } catch (const std::exception&) {
        }
    }

    if (core_frequencies.empty()) {
        return 0;
    }
    std::uint64_t maximum_frequency = 0;
    for (const auto& [core, frequency] : core_frequencies) {
        (void)core;
        maximum_frequency = std::max(maximum_frequency, frequency);
    }
    const std::uint64_t performance_threshold = maximum_frequency * 9 / 10;
    unsigned int performance_cores = 0;
    for (const auto& [core, frequency] : core_frequencies) {
        (void)core;
        if (frequency >= performance_threshold) {
            ++performance_cores;
        }
    }
    return performance_cores;
}

unsigned int linux_physical_core_count() {
    std::ifstream cpuinfo("/proc/cpuinfo");
    if (!cpuinfo) {
        return 0;
    }

    std::set<std::pair<std::string, std::string>> cores;
    std::string physical_id;
    std::string core_id;

    auto flush_cpu = [&]() {
        if (!physical_id.empty() && !core_id.empty()) {
            cores.emplace(physical_id, core_id);
        }
        physical_id.clear();
        core_id.clear();
    };

    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (trim(line).empty()) {
            flush_cpu();
            continue;
        }

        std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        std::string key = trim(line.substr(0, colon));
        std::string value = trim(line.substr(colon + 1));
        if (key == "physical id") {
            physical_id = value;
        } else if (key == "core id") {
            core_id = value;
        }
    }
    flush_cpu();

    return static_cast<unsigned int>(cores.size());
}
#endif
#endif

#ifdef _WIN32
unsigned int windows_performance_core_count() {
    DWORD length = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return 0;
    }

    std::vector<unsigned char> buffer(length);
    if (!GetLogicalProcessorInformationEx(
            RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
            &length)) {
        return 0;
    }

    std::map<BYTE, unsigned int> cores_by_efficiency_class;
    DWORD offset = 0;
    while (offset < length) {
        auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
            buffer.data() + offset);
        if (info->Relationship == RelationProcessorCore) {
            ++cores_by_efficiency_class[info->Processor.EfficiencyClass];
        }
        if (info->Size == 0) {
            break;
        }
        offset += info->Size;
    }
    if (cores_by_efficiency_class.empty()) {
        return 0;
    }
    return cores_by_efficiency_class.rbegin()->second;
}

std::string windows_error_message(DWORD error) {
    LPSTR buffer = nullptr;
    DWORD size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                    FORMAT_MESSAGE_FROM_SYSTEM |
                                    FORMAT_MESSAGE_IGNORE_INSERTS,
                                nullptr, error, 0, reinterpret_cast<LPSTR>(&buffer),
                                0, nullptr);
    std::string message = size > 0 && buffer ? std::string(buffer, size)
                                             : "Windows error " + std::to_string(error);
    if (buffer) {
        LocalFree(buffer);
    }
    return trim(message);
}

std::string windows_quote_argument(const std::string& argument) {
    if (argument.empty()) {
        return "\"\"";
    }

    bool needs_quotes = argument.find_first_of(" \t\n\v\"") != std::string::npos;
    if (!needs_quotes) {
        return argument;
    }

    std::string quoted = "\"";
    std::size_t backslashes = 0;
    for (char ch : argument) {
        if (ch == '\\') {
            ++backslashes;
        } else if (ch == '"') {
            quoted.append(backslashes * 2 + 1, '\\');
            quoted.push_back(ch);
            backslashes = 0;
        } else {
            quoted.append(backslashes, '\\');
            backslashes = 0;
            quoted.push_back(ch);
        }
    }
    quoted.append(backslashes * 2, '\\');
    quoted.push_back('"');
    return quoted;
}

std::string windows_command_line(const std::vector<std::string>& args) {
    std::string command_line;
    for (const auto& arg : args) {
        if (!command_line.empty()) {
            command_line.push_back(' ');
        }
        command_line += windows_quote_argument(arg);
    }
    return command_line;
}

bool is_windows_null_device(const fs::path& path) {
    std::string value = path.string();
    std::replace(value.begin(), value.end(), '/', '\\');
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value == "nul" || value == "\\\\.\\nul";
}

std::string windows_open_path(const fs::path& path) {
    return is_windows_null_device(path) ? std::string("NUL") : fs::absolute(path).string();
}

std::uint64_t filetime_to_ms(const FILETIME& time) {
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart / 10000ULL;
}

std::uint64_t process_cpu_time_ms(HANDLE process) {
    FILETIME creation_time{};
    FILETIME exit_time{};
    FILETIME kernel_time{};
    FILETIME user_time{};
    if (!GetProcessTimes(process, &creation_time, &exit_time, &kernel_time, &user_time)) {
        return 0;
    }
    return filetime_to_ms(kernel_time) + filetime_to_ms(user_time);
}

std::uint64_t wall_timeout_guard_ms(std::uint64_t timeout_ms) {
    if (timeout_ms == 0) {
        return 0;
    }
    return std::max(timeout_ms * 4, timeout_ms + 5000);
}

std::uint64_t windows_cpu_timeout_ms(std::uint64_t timeout_ms) {
    if (timeout_ms == 0) {
        return 0;
    }
    return timeout_ms + std::max<std::uint64_t>(100, timeout_ms / 10);
}
#endif

ProcessResult run_command(const std::string& command,
                          const fs::path* working_dir,
                          const fs::path* stdin_path,
                          const fs::path* stdout_path,
                          const fs::path* stderr_path,
                          std::uint64_t timeout_ms,
                          std::uint64_t memory_limit_mb = 0,
                          std::uint64_t stack_limit_mb = 0,
                          const std::function<bool()>& should_cancel = {}) {
    fs::path absolute_working_dir;
    fs::path absolute_stdin;
    fs::path absolute_stdout;
    fs::path absolute_stderr;
    if (working_dir) {
        absolute_working_dir = fs::absolute(*working_dir);
        working_dir = &absolute_working_dir;
    }
    if (stdin_path) {
#ifdef _WIN32
        absolute_stdin = windows_open_path(*stdin_path);
#else
        absolute_stdin = fs::absolute(*stdin_path);
#endif
        stdin_path = &absolute_stdin;
    }
    if (stdout_path) {
#ifdef _WIN32
        absolute_stdout = windows_open_path(*stdout_path);
#else
        absolute_stdout = fs::absolute(*stdout_path);
#endif
        stdout_path = &absolute_stdout;
    }
    if (stderr_path) {
#ifdef _WIN32
        absolute_stderr = windows_open_path(*stderr_path);
#else
        absolute_stderr = fs::absolute(*stderr_path);
#endif
        stderr_path = &absolute_stderr;
    }

    auto begin = std::chrono::steady_clock::now();
#ifndef _WIN32
    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("fork failed");
    }

    if (pid == 0) {
        setpgid(0, 0);
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
        apply_child_limits(memory_limit_mb, stack_limit_mb);
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
        if (should_cancel && should_cancel()) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            throw std::runtime_error("judging cancelled");
        }
        if (timeout_ms > 0 && static_cast<std::uint64_t>(elapsed) > timeout_ms) {
            timed_out = true;
            kill(-pid, SIGKILL);
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
    (void)memory_limit_mb;
    (void)stack_limit_mb;

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
    BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED,
                             nullptr, working_dir ? working_dir->string().c_str() : nullptr, &si, &pi);
    if (!ok) {
        if (opened_stdin != INVALID_HANDLE_VALUE) CloseHandle(opened_stdin);
        if (opened_stdout != INVALID_HANDLE_VALUE) CloseHandle(opened_stdout);
        if (opened_stderr != INVALID_HANDLE_VALUE) CloseHandle(opened_stderr);
        throw std::runtime_error("CreateProcess failed");
    }

    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    bool has_job = job != nullptr && AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);

    bool timed_out = false;
    while (true) {
        DWORD wait_result = WaitForSingleObject(pi.hProcess, 50);
        if (wait_result == WAIT_OBJECT_0) {
            break;
        }
        if (wait_result != WAIT_TIMEOUT) {
            break;
        }
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - begin).count();
        if (should_cancel && should_cancel()) {
            if (has_job) {
                TerminateJobObject(job, 125);
            } else {
                TerminateProcess(pi.hProcess, 125);
            }
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            if (job) CloseHandle(job);
            if (opened_stdin != INVALID_HANDLE_VALUE) CloseHandle(opened_stdin);
            if (opened_stdout != INVALID_HANDLE_VALUE) CloseHandle(opened_stdout);
            if (opened_stderr != INVALID_HANDLE_VALUE) CloseHandle(opened_stderr);
            throw std::runtime_error("judging cancelled");
        }
        if (timeout_ms > 0 && static_cast<std::uint64_t>(elapsed) > timeout_ms) {
            timed_out = true;
            break;
        }
    }
    if (timed_out) {
        if (has_job) {
            TerminateJobObject(job, 124);
        } else {
            TerminateProcess(pi.hProcess, 124);
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
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
                          const fs::path* stdout_path,
                          const fs::path* stderr_path,
                          std::uint64_t timeout_ms,
                          std::uint64_t memory_limit_mb,
                          std::uint64_t stack_limit_mb = 0,
                          const std::function<bool()>& should_cancel = {}) {
    if (args.empty() || args.front().empty()) {
        throw std::runtime_error("empty program command");
    }

#ifndef _WIN32
    fs::path absolute_working_dir;
    fs::path absolute_stdout;
    fs::path absolute_stderr;
    if (working_dir) {
        absolute_working_dir = fs::absolute(*working_dir);
        working_dir = &absolute_working_dir;
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
    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("fork failed");
    }

    if (pid == 0) {
        setpgid(0, 0);
        if (working_dir && chdir(working_dir->c_str()) != 0) {
            _exit(125);
        }
        int null_stdin = open("/dev/null", O_RDONLY);
        if (null_stdin >= 0) {
            dup2(null_stdin, STDIN_FILENO);
            close(null_stdin);
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
        apply_child_limits(memory_limit_mb, stack_limit_mb);

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
        if (should_cancel && should_cancel()) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            throw std::runtime_error("judging cancelled");
        }
        if (timeout_ms > 0 && static_cast<std::uint64_t>(elapsed) > timeout_ms) {
            timed_out = true;
            kill(-pid, SIGKILL);
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
    (void)memory_limit_mb;
    (void)stack_limit_mb;

    fs::path absolute_working_dir;
    if (working_dir) {
        absolute_working_dir = fs::absolute(*working_dir);
        working_dir = &absolute_working_dir;
    }

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;

    auto open_file = [&](const std::string& path, DWORD access, DWORD creation) {
        return CreateFileA(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &sa, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
    };

    HANDLE child_stdin = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE child_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE child_stderr = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE opened_stdin = INVALID_HANDLE_VALUE;
    HANDLE opened_stdout = INVALID_HANDLE_VALUE;
    HANDLE opened_stderr = INVALID_HANDLE_VALUE;

    auto close_opened_files = [&]() {
        if (opened_stdin != INVALID_HANDLE_VALUE) CloseHandle(opened_stdin);
        if (opened_stdout != INVALID_HANDLE_VALUE) CloseHandle(opened_stdout);
        if (opened_stderr != INVALID_HANDLE_VALUE) CloseHandle(opened_stderr);
    };

    opened_stdin = open_file("NUL", GENERIC_READ, OPEN_EXISTING);
    if (opened_stdin == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("failed to open NUL: " + windows_error_message(GetLastError()));
    }
    child_stdin = opened_stdin;

    if (stdout_path) {
        opened_stdout = open_file(windows_open_path(*stdout_path), GENERIC_WRITE, CREATE_ALWAYS);
        if (opened_stdout == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            close_opened_files();
            throw std::runtime_error("failed to open stdout file: " + windows_error_message(error));
        }
        child_stdout = opened_stdout;
    }
    if (stderr_path) {
        opened_stderr = open_file(windows_open_path(*stderr_path), GENERIC_WRITE, CREATE_ALWAYS);
        if (opened_stderr == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            close_opened_files();
            throw std::runtime_error("failed to open stderr file: " + windows_error_message(error));
        }
        child_stderr = opened_stderr;
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = child_stdin;
    si.hStdOutput = child_stdout;
    si.hStdError = child_stderr;

    PROCESS_INFORMATION pi{};
    std::string command_line = windows_command_line(args);
    std::string application_name = args.front();
    auto begin = std::chrono::steady_clock::now();
    BOOL ok = CreateProcessA(application_name.c_str(), command_line.data(),
                             nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED,
                             nullptr,
                             working_dir ? working_dir->string().c_str() : nullptr,
                             &si, &pi);
    if (!ok) {
        DWORD error = GetLastError();
        close_opened_files();
        throw std::runtime_error("CreateProcess failed for " + application_name + ": " +
                                 windows_error_message(error));
    }

    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    bool has_job = job != nullptr && AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);

    bool timed_out = false;
    std::uint64_t cpu_elapsed_ms = 0;
    std::uint64_t wall_guard_ms = wall_timeout_guard_ms(timeout_ms);
    std::uint64_t cpu_timeout_ms = windows_cpu_timeout_ms(timeout_ms);
    while (true) {
        DWORD wait_result = WaitForSingleObject(pi.hProcess, 10);
        if (wait_result == WAIT_OBJECT_0) {
            cpu_elapsed_ms = process_cpu_time_ms(pi.hProcess);
            break;
        }
        if (wait_result != WAIT_TIMEOUT) {
            break;
        }
        auto now = std::chrono::steady_clock::now();
        auto wall_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - begin).count();
        cpu_elapsed_ms = process_cpu_time_ms(pi.hProcess);
        if (should_cancel && should_cancel()) {
            if (has_job) {
                TerminateJobObject(job, 125);
            } else {
                TerminateProcess(pi.hProcess, 125);
            }
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            if (job) CloseHandle(job);
            close_opened_files();
            throw std::runtime_error("judging cancelled");
        }
        if (cpu_timeout_ms > 0 && cpu_elapsed_ms > cpu_timeout_ms) {
            timed_out = true;
            break;
        }
        if (wall_guard_ms > 0 && static_cast<std::uint64_t>(wall_elapsed) > wall_guard_ms) {
            timed_out = true;
            break;
        }
    }
    if (timed_out) {
        if (has_job) {
            TerminateJobObject(job, 124);
        } else {
            TerminateProcess(pi.hProcess, 124);
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    cpu_elapsed_ms = process_cpu_time_ms(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    close_opened_files();

    return ProcessResult{static_cast<int>(exit_code), timed_out, false,
                         cpu_elapsed_ms};
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

std::vector<std::string> split_words(const std::string& text) {
    std::istringstream in(text);
    std::vector<std::string> result;
    std::string token;
    while (in >> token) {
        result.push_back(token);
    }
    return result;
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::vector<std::string> warning_tolerant_compile_flags(const std::string& compiler,
                                                        const std::string& flags) {
    std::vector<std::string> filtered;
    for (const std::string& flag : split_words(flags)) {
        const std::string lower = lower_ascii(flag);
        if (lower == "-werror" || lower.rfind("-werror=", 0) == 0 ||
            lower == "-pedantic-errors" || lower == "/wx" || lower.rfind("/wx:", 0) == 0) {
            continue;
        }
        filtered.push_back(flag);
    }

    const std::string compiler_name = lower_ascii(fs::path(compiler).filename().string());
    filtered.push_back(compiler_name == "cl" || compiler_name == "cl.exe"
                           ? "/WX-"
                           : "-Wno-error");
    return filtered;
}

bool has_shell_metachar(const std::string& value) {
    return value.find_first_of("&;|<>$`\n\r") != std::string::npos;
}

std::string quote_command_token(const std::string& token) {
    if (token.empty() || has_shell_metachar(token)) {
        throw std::runtime_error("unsafe compiler setting: " + token);
    }
#ifdef _WIN32
    if (token.find_first_of(" \t\"") == std::string::npos) {
        return token;
    }
#endif
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

std::string stack_compile_flags(const std::string& compiler, std::uint64_t stack_limit_mb) {
    if (stack_limit_mb == 0) {
        return {};
    }

#ifdef _WIN32
    constexpr std::uint64_t bytes_per_mb = 1024ULL * 1024ULL;
    std::string lower_compiler = lower_ascii(fs::path(compiler).filename().string());
    std::uint64_t bytes = stack_limit_mb * bytes_per_mb;
    if (lower_compiler == "cl" || lower_compiler == "cl.exe") {
        return quote_command_token("/F" + std::to_string(bytes)) + " " +
               quote_command_token("/link") + " " +
               quote_command_token("/STACK:" + std::to_string(bytes));
    }
    return quote_command_token("-Wl,--stack," + std::to_string(bytes));
#else
    (void)compiler;
    (void)stack_limit_mb;
    return {};
#endif
}

std::string stack_guard_compile_flags(const std::string& compiler, std::uint64_t stack_limit_mb) {
    if (stack_limit_mb == 0) {
        return {};
    }
    std::string result = stack_compile_flags(compiler, stack_limit_mb);
    std::string lower_compiler = lower_ascii(fs::path(compiler).filename().string());
    if (lower_compiler != "cl" && lower_compiler != "cl.exe") {
        if (!result.empty()) {
            result += ' ';
        }
        result += quote_command_token("-fno-optimize-sibling-calls");
    }
    return result;
}

class FastTokenReader {
public:
    explicit FastTokenReader(const fs::path& path)
        : input_(path, std::ios::binary), buffer_(1024 * 1024) {}

    bool ok() const {
        return static_cast<bool>(input_);
    }

    bool next(std::string& token) {
        token.clear();
        int ch = 0;
        do {
            ch = read_char();
            if (ch < 0) {
                return false;
            }
        } while (std::isspace(static_cast<unsigned char>(ch)));

        do {
            token.push_back(static_cast<char>(ch));
            ch = read_char();
        } while (ch >= 0 && !std::isspace(static_cast<unsigned char>(ch)));
        return true;
    }

private:
    int read_char() {
        if (pos_ >= limit_) {
            input_.read(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
            limit_ = static_cast<std::size_t>(input_.gcount());
            pos_ = 0;
            if (limit_ == 0) {
                return -1;
            }
        }
        return static_cast<unsigned char>(buffer_[pos_++]);
    }

    std::ifstream input_;
    std::vector<char> buffer_;
    std::size_t pos_ = 0;
    std::size_t limit_ = 0;
};

bool outputs_match(const fs::path& actual, const fs::path& expected) {
    FastTokenReader actual_in(actual);
    FastTokenReader expected_in(expected);
    if (!actual_in.ok() || !expected_in.ok()) {
        return false;
    }

    std::string actual_token;
    std::string expected_token;
    while (true) {
        bool actual_ok = actual_in.next(actual_token);
        bool expected_ok = expected_in.next(expected_token);
        if (!actual_ok || !expected_ok) {
            return actual_ok == expected_ok;
        }
        if (actual_token != expected_token) {
            return false;
        }
    }
}

void copy_file_alias(const fs::path& from, const fs::path& to) {
    std::error_code equivalent_ec;
    if (fs::equivalent(from, to, equivalent_ec)) {
        return;
    }
    std::error_code ec;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        throw std::runtime_error("failed to copy " + from.string() + " to " + to.string() +
                                 ": " + ec.message());
    }
}

void copy_input_aliases(const fs::path& input,
                        const fs::path& run_dir,
                        const std::string& problem,
                        const std::string& test_name) {
    std::set<std::string> names;
    std::set<std::string> seen_names;
    auto insert_name = [&](const std::string& name) {
#ifdef _WIN32
        std::string key = lower_ascii(name);
#else
        const std::string& key = name;
#endif
        if (seen_names.insert(key).second) {
            names.insert(name);
        }
    };
    auto add_name = [&](const std::string& stem) {
        if (stem.empty()) {
            return;
        }
        std::string lower = lower_ascii(stem);
        std::string upper = stem;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
        insert_name(stem + ".inp");
        insert_name(stem + ".INP");
        insert_name(lower + ".inp");
        insert_name(lower + ".INP");
        insert_name(upper + ".inp");
        insert_name(upper + ".INP");
    };

    add_name(problem);
    add_name(test_name);
    add_name(input.stem().string());
    fs::path primary;
    for (const auto& name : names) {
        fs::path target = run_dir / name;
        if (primary.empty()) {
            copy_file_alias(input, target);
            primary = target;
            continue;
        }
        std::error_code ec;
        fs::create_hard_link(primary, target, ec);
        if (ec) {
            copy_file_alias(primary, target);
        }
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
    return {};
}

fs::path null_output_path() {
#ifdef _WIN32
    return "NUL";
#else
    return "/dev/null";
#endif
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
                if (text.empty()) {
                    continue;
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
                                const fs::path& problem_dir) {
    if (settings.checker == "token") {
        return {};
    }

    if (starts_with(settings.checker, "testlib:")) {
        throw std::runtime_error("checker=testlib:<name> was removed; use checker=custom and put checker.cpp plus testlib.h in the problem folder");
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
    return first_existing_file_text({path});
}

bool nonempty_executable_exists(const fs::path& executable) {
    std::error_code error;
    return fs::is_regular_file(executable, error) && !error &&
           fs::file_size(executable, error) > 0 && !error;
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

bool checker_source_includes_testlib(const fs::path& checker_source) {
    std::string source = read_file(checker_source);
    return source.find("#include \"testlib.h\"") != std::string::npos ||
           source.find("#include <testlib.h>") != std::string::npos;
}

fs::path compile_checker(const ProblemSettings& settings,
                         const JudgeOptions& options,
                         const fs::path& problem_dir) {
    fs::path checker_source = resolve_checker_source(settings, problem_dir);
    if (checker_source.empty()) {
        return {};
    }
    bool needs_testlib = checker_source_includes_testlib(checker_source);
    fs::path local_testlib = problem_dir / "testlib.h";
    if (needs_testlib && !fs::exists(local_testlib)) {
        throw std::runtime_error("checker includes testlib.h but " + local_testlib.string() +
                                 " was not found");
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
        bool testlib_current = true;
        if (needs_testlib && !checker_time_ec) {
            std::error_code testlib_time_ec;
            auto testlib_time = fs::last_write_time(local_testlib, testlib_time_ec);
            testlib_current = !testlib_time_ec && checker_time >= testlib_time;
        }
        if (!checker_time_ec && !source_time_ec && checker_time >= source_time && testlib_current) {
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
                              quote_command_tokens(warning_tolerant_compile_flags(
                                  options.compiler, options.compile_flags)) + " " +
                              stack_guard_compile_flags(options.compiler, options.stack_limit_mb) + " " +
                              "-I " + quote_path(problem_dir) + " " +
                              quote_path(checker_source) + " -o " + quote_path(checker_executable);
    ProcessResult compile = run_command(compile_cmd, nullptr, nullptr, nullptr, &compile_log,
                                        0, 0, 0, options.should_cancel);
    if (!nonempty_executable_exists(checker_executable)) {
        std::string message = read_checker_message(compile_log);
        if (message.empty()) {
            message = "compiler exited with code " + std::to_string(compile.exit_code) +
                      " without producing a checker executable";
        }
        throw std::runtime_error("checker compile failed: " + message);
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
        << "# Checker options: token, custom:<path-in-this-problem-folder>\n"
        << "# Examples: checker=custom or checker=custom:checker.cpp\n"
        << "# Custom checkers that include testlib.h must keep testlib.h in this problem folder.\n"
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
    } else if (key == "stack_limit_mb") {
        (void)value;
        // Legacy per-problem stack settings are ignored; stack_limit_mb is contest-wide.
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
            if (trim(value).empty()) {
                return;
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
                              quote_command_tokens(warning_tolerant_compile_flags(
                                  options.compiler, options.compile_flags)) + " " +
                              stack_guard_compile_flags(options.compiler, options.stack_limit_mb) + " " +
                              quote_path(source) + " -o " + quote_path(prepared.executable);
    ProcessResult compile = run_command(compile_cmd, nullptr, nullptr, nullptr, &compile_log,
                                        0, 0, 0, options.should_cancel);
    if (!nonempty_executable_exists(prepared.executable)) {
        std::string message = first_existing_file_text({compile_log});
        if (message.empty()) {
            message = "compiler exited with code " + std::to_string(compile.exit_code) +
                      " without producing an executable";
        }
        prepared.immediate_results =
            rows_for_problem_tests(prepared.contestant, problem, Verdict::CompileError,
                                   compile.exit_code, compile.elapsed_ms,
                                   message);
        return prepared;
    }

    prepared.ready = true;
    return prepared;
}

TestResult judge_test_job(const TestJob& job, const JudgeOptions& options) {
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
    try {
        copy_input_aliases(input, run_dir, problem.name, test_name);
    } catch (const std::exception& ex) {
        row.verdict = Verdict::InternalError;
        row.message = ex.what();
        return row;
    }

    fs::path executable_path = fs::absolute(job.executable);
    fs::path stdout_sink = null_output_path();
    ProcessResult run = run_program({executable_path.string()}, &run_dir, &stdout_sink, &run_log,
                                    problem.settings.time_limit_ms,
                                    problem.settings.memory_limit_mb,
                                    options.stack_limit_mb,
                                    options.should_cancel);
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
            fs::path checker_stdout = run_dir / (test_name + ".checker.out");
            fs::path checker_stderr = run_dir / (test_name + ".checker.err");
            fs::path checker_executable = fs::absolute(problem.checker_executable);
            fs::path checker_input = find_case_insensitive_file(run_dir, {input.filename().string()});
            if (checker_input.empty()) {
                checker_input = input;
            }
            ProcessResult checker =
                run_program({checker_executable.string(), checker_input.filename().string(),
                             actual.filename().string(), fs::absolute(expected).string()},
                            &run_dir, &checker_stdout, &checker_stderr,
                            problem.settings.time_limit_ms,
                            problem.settings.memory_limit_mb,
                            options.stack_limit_mb,
                            options.should_cancel);
            row.exit_code = checker.exit_code;
            row.message = first_existing_file_text({checker_stdout, checker_stderr});
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
#ifdef _WIN32
        worker_count = windows_performance_core_count();
#elif defined(__linux__)
        worker_count = linux_performance_core_count();
        if (worker_count == 0) {
            worker_count = linux_physical_core_count();
        }
#endif
        if (worker_count == 0) {
            worker_count = std::thread::hardware_concurrency();
            if (worker_count > 1) {
                worker_count = (worker_count + 1) / 2;
            }
        }
        if (worker_count == 0) {
            worker_count = 1;
        }
    }
    return std::max(1U, worker_count);
}

fs::path work_root_for_contest(const JudgeOptions& options) {
    std::string contest_key = fs::absolute(options.contest_root).string();
    std::size_t hash = std::hash<std::string>{}(contest_key);
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
#ifndef _WIN32
    auto process_id = static_cast<unsigned long long>(getpid());
#else
    auto process_id = static_cast<unsigned long long>(GetCurrentProcessId());
#endif
    fs::path root = options.contest_root / ".neothemis-work";
    return root / ("run-" + std::to_string(hash) + "-" +
                   std::to_string(process_id) + "-" + std::to_string(now));
}

class ScopedWorkdirCleanup {
public:
    ScopedWorkdirCleanup(fs::path path, bool enabled)
        : path_(std::move(path)), enabled_(enabled) {}

    ~ScopedWorkdirCleanup() {
        cleanup();
    }

    void cleanup() {
        if (!enabled_) {
            return;
        }
        std::error_code ignored;
        fs::remove_all(path_, ignored);
        enabled_ = false;
    }

private:
    fs::path path_;
    bool enabled_ = false;
};

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

        ScopedWorkdirCleanup workdir_cleanup(work_root.parent_path(), !options.keep_workdir);
        if (!options.keep_workdir) {
            std::error_code ignored;
            fs::remove_all(work_root, ignored);
        }
        fs::create_directories(work_root);
        std::vector<TestResult> results;
        std::vector<ProblemContext> problems;
        for (const auto& problem_entry : sorted_directories(tests_root)) {
            if (options.should_cancel && options.should_cancel()) {
                throw std::runtime_error("judging cancelled");
            }
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

        std::mutex results_mutex;
        std::mutex prepare_progress_mutex;
        std::mutex progress_mutex;
        ProgressState prepare_state;
        prepare_state.total_jobs = contestants.size() * problems.size();
        ProgressState progress_state;

        auto publish_result = [&](const TestResult& result) {
            if (options.result) {
                options.result(result);
            }
        };

        auto run_test_jobs = [&](std::vector<TestJob> test_jobs) {
            if (test_jobs.empty()) {
                return;
            }
            std::sort(test_jobs.begin(), test_jobs.end(),
                      [](const TestJob& a, const TestJob& b) {
                          return std::make_tuple(a.problem->name,
                                                 a.test.path().filename().string(),
                                                 a.contestant) <
                                 std::make_tuple(b.problem->name,
                                                 b.test.path().filename().string(),
                                                 b.contestant);
                      });

            unsigned int worker_count =
                std::min<unsigned int>(base_worker_count,
                                       static_cast<unsigned int>(test_jobs.size()));
            {
                std::lock_guard<std::mutex> lock(progress_mutex);
                progress_state.total_jobs = test_jobs.size();
                progress_state.worker_labels.assign(worker_count, "idle");
            }

            std::atomic<std::size_t> next_job{0};
            std::mutex worker_error_mutex;
            std::exception_ptr worker_error;
            std::atomic<bool> stop_workers{false};
            std::atomic<bool> progress_done{false};
            auto worker = [&](unsigned int worker_id) {
                try {
                    while (true) {
                        if (stop_workers.load() ||
                            (options.should_cancel && options.should_cancel())) {
                            std::lock_guard<std::mutex> progress_lock(progress_mutex);
                            progress_state.worker_labels[worker_id - 1] = "done";
                            return;
                        }
                        std::size_t job_index = next_job.fetch_add(1);
                        if (job_index >= test_jobs.size()) {
                            std::lock_guard<std::mutex> progress_lock(progress_mutex);
                            progress_state.worker_labels[worker_id - 1] = "done";
                            return;
                        }
                        TestJob job = test_jobs[job_index];

                        std::string label = job.contestant + "/" + job.problem->name + "/" +
                                            job.test.path().filename().string();
                        {
                            std::lock_guard<std::mutex> lock(progress_mutex);
                            progress_state.worker_labels[worker_id - 1] = label;
                        }

                        TestResult job_result = judge_test_job(job, options);
                        if (!options.keep_workdir) {
                            std::error_code ignored;
                            fs::remove_all(job.executable.parent_path() /
                                           job.test.path().filename(), ignored);
                        }
                        publish_result(job_result);
                        {
                            std::lock_guard<std::mutex> lock(results_mutex);
                            results.push_back(std::move(job_result));
                        }
                        {
                            std::lock_guard<std::mutex> lock(progress_mutex);
                            ++progress_state.completed_jobs;
                        }
                    }
                } catch (...) {
                    stop_workers.store(true);
                    std::lock_guard<std::mutex> lock(worker_error_mutex);
                    if (!worker_error) {
                        worker_error = std::current_exception();
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
            if (worker_error) {
                std::rethrow_exception(worker_error);
            }
        };

        std::vector<PrepTask> prep_tasks;
        prep_tasks.reserve(contestants.size() * problems.size());
        for (const auto& contestant : contestants) {
            for (const auto& problem : problems) {
                prep_tasks.push_back(PrepTask{&contestant, &problem});
            }
        }

        std::vector<TestJob> all_jobs;
        if (!prep_tasks.empty()) {
            unsigned int prepare_worker_count =
                std::min<unsigned int>(base_worker_count,
                                       static_cast<unsigned int>(prep_tasks.size()));
            {
                std::lock_guard<std::mutex> progress_lock(prepare_progress_mutex);
                prepare_state.worker_labels.assign(prepare_worker_count, "idle");
            }
            std::atomic<std::size_t> next_prep_task{0};
            std::atomic<bool> prepare_progress_done{false};
            std::atomic<bool> stop_prepare_workers{false};
            std::mutex all_jobs_mutex;
            std::mutex prepare_error_mutex;
            std::exception_ptr prepare_error;

            auto prepare_worker = [&](unsigned int worker_id) {
                try {
                    while (true) {
                        if (stop_prepare_workers.load() ||
                            (options.should_cancel && options.should_cancel())) {
                            std::lock_guard<std::mutex> progress_lock(prepare_progress_mutex);
                            prepare_state.worker_labels[worker_id - 1] = "done";
                            return;
                        }
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
                        for (const auto& result : prepared.immediate_results) {
                            publish_result(result);
                        }
                        {
                            std::lock_guard<std::mutex> lock(results_mutex);
                            results.insert(results.end(), prepared.immediate_results.begin(),
                                           prepared.immediate_results.end());
                        }
                        if (prepared.ready) {
                            std::lock_guard<std::mutex> lock(all_jobs_mutex);
                            for (const auto& test : task.problem->tests) {
                                all_jobs.push_back(TestJob{prepared.contestant, task.problem, test,
                                                           prepared.executable});
                            }
                        }
                        {
                            std::lock_guard<std::mutex> progress_lock(prepare_progress_mutex);
                            ++prepare_state.completed_jobs;
                        }
                    }
                } catch (...) {
                    stop_prepare_workers.store(true);
                    std::lock_guard<std::mutex> lock(prepare_error_mutex);
                    if (!prepare_error) {
                        prepare_error = std::current_exception();
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
            if (prepare_error) {
                std::rethrow_exception(prepare_error);
            }
        }

        run_test_jobs(std::move(all_jobs));

        std::sort(results.begin(), results.end(), [](const TestResult& a, const TestResult& b) {
            return std::tie(a.contestant, a.problem, a.test) < std::tie(b.contestant, b.problem, b.test);
        });

        if (!options.keep_workdir) {
            workdir_cleanup.cleanup();
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

ContestOverview inspect_contest(const JudgeOptions& options) {
    auto lower = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    };
    auto list_dirs = [](const fs::path& path) {
        std::vector<fs::path> dirs;
        if (!fs::exists(path)) {
            return dirs;
        }
        for (const auto& entry : fs::directory_iterator(path)) {
            if (entry.is_directory()) {
                dirs.push_back(entry.path());
            }
        }
        std::sort(dirs.begin(), dirs.end());
        return dirs;
    };
    auto allowed = [&](const std::vector<std::string>& selected, const std::string& name) {
        if (selected.empty()) {
            return true;
        }
        std::string wanted = lower(name);
        for (const auto& item : selected) {
            if (lower(item) == wanted) {
                return true;
            }
        }
        return false;
    };

    ContestOverview overview;
    std::vector<std::string> problem_keys;
    for (const auto& problem_dir : list_dirs(options.contest_root / options.tests_dir)) {
        std::string problem = problem_dir.filename().string();
        if (!allowed(options.selected_problems, problem)) {
            continue;
        }
        overview.problems.push_back(problem);
        problem_keys.push_back(lower(problem));
    }

    for (const auto& contestant_dir : list_dirs(options.contest_root / options.contestants_dir)) {
        std::string contestant = contestant_dir.filename().string();
        if (!allowed(options.selected_contestants, contestant)) {
            continue;
        }

        std::set<std::string> source_stems;
        for (const auto& entry : fs::directory_iterator(contestant_dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            std::string ext = lower(entry.path().extension().string());
            if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") {
                source_stems.insert(lower(entry.path().stem().string()));
            }
        }

        overview.contestants.push_back(contestant);
        std::vector<bool> row;
        row.reserve(problem_keys.size());
        for (const auto& problem_key : problem_keys) {
            row.push_back(source_stems.find(problem_key) != source_stems.end());
        }
        overview.has_source.push_back(std::move(row));
    }
    return overview;
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
