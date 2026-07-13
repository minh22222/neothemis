#include "neothemis/JudgeCore.hpp"
#include "neothemis/Config.hpp"
#include "neothemis/Csv.hpp"
#include "Sandbox.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
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
#if defined(__linux__)
#include <sched.h>
#include <sys/syscall.h>
#endif
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
    bool sandbox_setup_failed = false;
    std::uint64_t elapsed_ms = 0;
    std::string sandbox_error;
};

struct SandboxRunSpec {
    detail::SandboxProfile profile = detail::SandboxProfile::Submission;
    std::vector<detail::SandboxMount> mounts;
    std::vector<detail::SandboxTmpfs> temporary_filesystems;
    std::string guest_working_directory = "/work";
};

enum class CheckerPointScale {
    Absolute,
    Percentage
};

struct ProblemContext {
    std::string name;
    fs::path dir;
    std::vector<fs::directory_entry> tests;
    ProblemConfig settings;
    CheckerPointScale checker_point_scale = CheckerPointScale::Absolute;
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

#ifndef _WIN32
void apply_child_limits(std::uint64_t memory_limit_mb,
                        std::uint64_t stack_limit_mb,
                        std::uint64_t file_limit_mb) {
    constexpr std::uint64_t bytes_per_mb = 1024ULL * 1024ULL;
    auto bytes_for_limit = [](std::uint64_t megabytes) {
        const std::uint64_t maximum =
            static_cast<std::uint64_t>(std::numeric_limits<rlim_t>::max());
        if (megabytes > maximum / bytes_per_mb) {
            return std::numeric_limits<rlim_t>::max();
        }
        return static_cast<rlim_t>(megabytes * bytes_per_mb);
    };
    if (memory_limit_mb > 0) {
        rlimit limit{};
        limit.rlim_cur = bytes_for_limit(memory_limit_mb);
        limit.rlim_max = limit.rlim_cur;
        if (setrlimit(RLIMIT_AS, &limit) != 0) {
            _exit(124);
        }
    }
    if (stack_limit_mb > 0) {
        rlimit limit{};
        limit.rlim_cur = bytes_for_limit(stack_limit_mb);
        limit.rlim_max = limit.rlim_cur;
        if (setrlimit(RLIMIT_STACK, &limit) != 0) {
            _exit(124);
        }
    }
    if (file_limit_mb > 0) {
        rlimit limit{};
        limit.rlim_cur = bytes_for_limit(file_limit_mb);
        limit.rlim_max = limit.rlim_cur;
        if (setrlimit(RLIMIT_FSIZE, &limit) != 0) {
            _exit(124);
        }
    }
    rlimit core_limit{};
    core_limit.rlim_cur = 0;
    core_limit.rlim_max = 0;
    if (setrlimit(RLIMIT_CORE, &core_limit) != 0) {
        _exit(124);
    }
    rlimit file_descriptors{};
    file_descriptors.rlim_cur = 64;
    file_descriptors.rlim_max = 64;
    if (setrlimit(RLIMIT_NOFILE, &file_descriptors) != 0) {
        _exit(124);
    }
}

void close_child_file_descriptors_except(const std::vector<int>& keep) {
#if defined(__linux__) && defined(SYS_close_range)
    unsigned int first = 3;
    bool ranges_closed = true;
    for (int fd : keep) {
        const unsigned int kept = static_cast<unsigned int>(fd);
        if (first < kept) {
            if (syscall(SYS_close_range, first, kept - 1, 0) != 0) {
                ranges_closed = false;
                break;
            }
        }
        if (kept < std::numeric_limits<unsigned int>::max()) {
            first = kept + 1;
        }
    }
    if (ranges_closed &&
        syscall(SYS_close_range, first, std::numeric_limits<unsigned int>::max(), 0) == 0) {
        return;
    }
#endif
    // Headers can expose close_range while the running kernel (or an outer
    // seccomp profile) does not. Fall back instead of leaking inherited host
    // descriptors into the sandbox in that case.
    long maximum = sysconf(_SC_OPEN_MAX);
    if (maximum < 0) {
        maximum = 65536;
    }
    for (int fd = 3; fd < maximum; ++fd) {
        if (!std::binary_search(keep.begin(), keep.end(), fd)) {
            close(fd);
        }
    }
}

std::string resolve_posix_executable(const std::string& program,
                                     const fs::path* working_dir) {
    auto executable = [&](const fs::path& candidate) -> std::string {
        std::error_code error;
        const fs::path absolute = candidate.is_absolute()
                                      ? candidate
                                      : fs::absolute(working_dir ? *working_dir / candidate
                                                                 : candidate,
                                                     error);
        if (error || access(absolute.c_str(), X_OK) != 0) {
            return {};
        }
        return absolute.string();
    };

    if (program.find('/') != std::string::npos) {
        const std::string resolved = executable(program);
        return resolved.empty() ? program : resolved;
    }

    const char* environment_path = std::getenv("PATH");
    const std::string search_path = environment_path ? environment_path : "/usr/bin:/bin";
    std::size_t start = 0;
    while (start <= search_path.size()) {
        const std::size_t separator = search_path.find(':', start);
        const std::string directory =
            search_path.substr(start, separator == std::string::npos
                                          ? std::string::npos
                                          : separator - start);
        const std::string resolved = executable(fs::path(directory.empty() ? "." : directory) /
                                                program);
        if (!resolved.empty()) {
            return resolved;
        }
        if (separator == std::string::npos) {
            break;
        }
        start = separator + 1;
    }
    return program;
}

pid_t waitpid_retry(pid_t pid, int* status, int options) {
    pid_t result = -1;
    do {
        result = waitpid(pid, status, options);
    } while (result < 0 && errno == EINTR);
    return result;
}

void kill_and_reap_process_group(pid_t pid) noexcept {
    if (pid <= 0) {
        return;
    }
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    int ignored_status = 0;
    (void)waitpid_retry(pid, &ignored_status, 0);
}

class ChildProcessGuard {
public:
    explicit ChildProcessGuard(pid_t pid) : pid_(pid) {}
    ~ChildProcessGuard() {
        if (armed_) {
            kill_and_reap_process_group(pid_);
        }
    }
    void disarm() noexcept { armed_ = false; }

private:
    pid_t pid_ = -1;
    bool armed_ = true;
};

class SandboxLaunchGuard {
public:
    explicit SandboxLaunchGuard(detail::SandboxLaunch& launch) : launch_(launch) {}
    ~SandboxLaunchGuard() { detail::close_sandbox_launch(launch_); }

private:
    detail::SandboxLaunch& launch_;
};

#if defined(__linux__)
void restrict_child_to_one_cpu() {
    cpu_set_t available;
    CPU_ZERO(&available);
    if (sched_getaffinity(0, sizeof(available), &available) != 0) {
        _exit(124);
    }

    int available_count = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &available)) {
            ++available_count;
        }
    }
    if (available_count == 0) {
        _exit(124);
    }
    int selected_index = static_cast<int>(getpid() % available_count);
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &available)) {
            continue;
        }
        if (selected_index-- != 0) {
            continue;
        }
        cpu_set_t selected;
        CPU_ZERO(&selected);
        CPU_SET(cpu, &selected);
        if (sched_setaffinity(0, sizeof(selected), &selected) != 0) {
            _exit(124);
        }
        return;
    }
    _exit(124);
}
#endif

#if defined(__linux__)
unsigned int linux_allowed_cpu_count() {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
        return 0;
    }
    return static_cast<unsigned int>(CPU_COUNT(&allowed));
}

bool process_tree_near_address_space_limit(pid_t root_pid,
                                           std::uint64_t memory_limit_mb) {
    if (root_pid <= 0 || memory_limit_mb == 0) {
        return false;
    }

    constexpr std::uint64_t kilobytes_per_mb = 1024;
    const std::uint64_t limit_kb =
        memory_limit_mb > std::numeric_limits<std::uint64_t>::max() / kilobytes_per_mb
            ? std::numeric_limits<std::uint64_t>::max()
            : memory_limit_mb * kilobytes_per_mb;
    // RLIMIT_AS rejects allocations before VmSize can exceed the limit. This
    // observation is used only to attribute an already-abnormal exit; it does
    // not lower the configured limit or kill the process.
    const std::uint64_t pressure_threshold_kb = limit_kb - limit_kb / 20;

    std::vector<pid_t> pending{root_pid};
    std::set<pid_t> visited;
    while (!pending.empty()) {
        const pid_t pid = pending.back();
        pending.pop_back();
        if (!visited.insert(pid).second) {
            continue;
        }

        std::ifstream status("/proc/" + std::to_string(pid) + "/status");
        std::string line;
        while (std::getline(status, line)) {
            if (line.rfind("VmSize:", 0) != 0) {
                continue;
            }
            std::istringstream value(line.substr(std::string("VmSize:").size()));
            std::uint64_t virtual_kb = 0;
            if (value >> virtual_kb && virtual_kb >= pressure_threshold_kb) {
                return true;
            }
            break;
        }

        std::ifstream children("/proc/" + std::to_string(pid) + "/task/" +
                               std::to_string(pid) + "/children");
        pid_t child = 0;
        while (children >> child) {
            pending.push_back(child);
        }
    }
    return false;
}

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
            // Older MinGW headers name the documented EfficiencyClass byte as padding.
            const auto* relationship_bytes =
                reinterpret_cast<const unsigned char*>(&info->Processor);
            ++cores_by_efficiency_class[relationship_bytes[1]];
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

unsigned int windows_physical_core_count() {
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

    unsigned int cores = 0;
    DWORD offset = 0;
    while (offset < length) {
        auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
            buffer.data() + offset);
        if (info->Relationship == RelationProcessorCore) {
            ++cores;
        }
        if (info->Size == 0) {
            break;
        }
        offset += info->Size;
    }
    return cores;
}

std::string windows_utf8_text(const wchar_t* value, std::size_t size) {
    if (!value || size == 0) {
        return {};
    }
    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Windows text is too long to encode as UTF-8");
    }
    const int input_size = static_cast<int>(size);
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value,
                                             input_size, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        throw std::runtime_error("failed to encode Windows text as UTF-8");
    }
    std::string encoded(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, input_size,
                            encoded.data(), required, nullptr, nullptr) != required) {
        throw std::runtime_error("failed to encode Windows text as UTF-8");
    }
    return encoded;
}

std::string windows_error_message(DWORD error) {
    LPWSTR buffer = nullptr;
    DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                    FORMAT_MESSAGE_FROM_SYSTEM |
                                    FORMAT_MESSAGE_IGNORE_INSERTS,
                                nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer),
                                0, nullptr);
    std::string message = "Windows error " + std::to_string(error);
    if (size > 0 && buffer) {
        try {
            message = windows_utf8_text(buffer, size);
        } catch (...) {
            // Keep the numeric error when a localized system message cannot
            // be represented as valid UTF-8.
        }
    }
    if (buffer) {
        LocalFree(buffer);
    }
    return trim(message);
}

std::wstring windows_widen_argument(const std::string& value) {
    if (value.find('\0') != std::string::npos) {
        throw std::runtime_error("Windows process argument contains a null byte");
    }
    if (value.empty()) {
        return {};
    }
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Windows process argument is too long");
    }
    const int input_size = static_cast<int>(value.size());
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           input_size, nullptr, 0);
    if (length <= 0) {
        const DWORD error = GetLastError();
        throw std::runtime_error("Windows process argument is not valid UTF-8: " +
                                 windows_error_message(error));
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_size,
                            result.data(), length) != length) {
        const DWORD error = GetLastError();
        throw std::runtime_error("failed to encode Windows process argument: " +
                                 windows_error_message(error));
    }
    return result;
}

std::wstring windows_quote_argument(const std::wstring& argument) {
    if (argument.empty()) {
        return L"\"\"";
    }

    bool needs_quotes = argument.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needs_quotes) {
        return argument;
    }

    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(ch);
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(ch);
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring windows_command_line(const std::vector<std::string>& args) {
    constexpr std::size_t maximum_command_line_chars = 32766;
    std::wstring command_line;
    for (const auto& arg : args) {
        const std::wstring quoted = windows_quote_argument(windows_widen_argument(arg));
        const std::size_t separator = command_line.empty() ? 0 : 1;
        if (quoted.size() > maximum_command_line_chars -
                                std::min(maximum_command_line_chars,
                                         command_line.size() + separator)) {
            throw std::runtime_error("Windows process command line exceeds 32767 characters");
        }
        if (separator != 0) {
            command_line.push_back(L' ');
        }
        command_line += quoted;
    }
    return command_line;
}

bool windows_environment_key_is(const std::wstring& entry,
                                const std::wstring& expected_key) {
    // Drive-current-directory entries have the special form "=C:=C:\\...".
    // Start after their leading '=' so they can never be mistaken for a
    // conventional environment variable.
    const std::size_t key_start = !entry.empty() && entry.front() == L'=' ? 1 : 0;
    const std::size_t separator = entry.find(L'=', key_start);
    if (separator == std::wstring::npos || separator - key_start != expected_key.size()) {
        return false;
    }
    return std::equal(expected_key.begin(), expected_key.end(),
                      entry.begin() + static_cast<std::ptrdiff_t>(key_start),
                      [](wchar_t left, wchar_t right) {
                          return std::towlower(left) == std::towlower(right);
                      });
}

std::vector<wchar_t> windows_environment_with_search_paths(
    const std::vector<fs::path>& search_paths) {
    std::vector<std::wstring> prefixes;
    for (const fs::path& path : search_paths) {
        if (path.empty()) {
            continue;
        }
        std::error_code absolute_error;
        const fs::path absolute = fs::absolute(path, absolute_error);
        if (absolute_error) {
            throw std::runtime_error("failed to resolve compiler runtime directory: " +
                                     path.u8string());
        }
        std::wstring value = absolute.native();
        if (value.find(L';') != std::wstring::npos) {
            throw std::runtime_error(
                "compiler runtime directory contains ';' and cannot be represented in PATH");
        }
        const bool duplicate = std::any_of(
            prefixes.begin(), prefixes.end(), [&](const std::wstring& existing) {
                return existing.size() == value.size() &&
                       std::equal(existing.begin(), existing.end(), value.begin(),
                                  [](wchar_t left, wchar_t right) {
                                      return std::towlower(left) == std::towlower(right);
                                  });
            });
        if (!duplicate) {
            prefixes.push_back(std::move(value));
        }
    }
    if (prefixes.empty()) {
        return {};
    }

    LPWCH raw_environment = GetEnvironmentStringsW();
    if (!raw_environment) {
        throw std::runtime_error("failed to read Windows process environment: " +
                                 windows_error_message(GetLastError()));
    }

    std::vector<std::wstring> entries;
    try {
        for (const wchar_t* entry = raw_environment; *entry != L'\0';
             entry += std::wcslen(entry) + 1) {
            entries.emplace_back(entry);
        }
    } catch (...) {
        FreeEnvironmentStringsW(raw_environment);
        throw;
    }
    if (!FreeEnvironmentStringsW(raw_environment)) {
        throw std::runtime_error("failed to release Windows process environment: " +
                                 windows_error_message(GetLastError()));
    }

    std::wstring prefix;
    for (const std::wstring& path : prefixes) {
        if (!prefix.empty()) {
            prefix.push_back(L';');
        }
        prefix += path;
    }

    bool replaced_path = false;
    std::vector<std::wstring> updated_entries;
    updated_entries.reserve(entries.size() + 1);
    for (const std::wstring& entry : entries) {
        if (!windows_environment_key_is(entry, L"Path")) {
            updated_entries.push_back(entry);
            continue;
        }
        if (replaced_path) {
            // Windows variable names are case-insensitive. Avoid passing an
            // ambiguous block if a malformed parent environment has duplicates.
            continue;
        }
        const std::size_t separator = entry.find(L'=');
        std::wstring value = entry.substr(0, separator + 1) + prefix;
        if (separator + 1 < entry.size()) {
            value.push_back(L';');
            value += entry.substr(separator + 1);
        }
        updated_entries.push_back(std::move(value));
        replaced_path = true;
    }
    if (!replaced_path) {
        updated_entries.push_back(L"Path=" + prefix);
    }

    // Environment blocks are conventionally sorted case-insensitively. Keep
    // the special '=C:' entries first and make a newly-added Path deterministic.
    std::sort(updated_entries.begin(), updated_entries.end(),
              [](const std::wstring& left, const std::wstring& right) {
                  return std::lexicographical_compare(
                      left.begin(), left.end(), right.begin(), right.end(),
                      [](wchar_t lhs, wchar_t rhs) {
                          return std::towlower(lhs) < std::towlower(rhs);
                      });
              });

    std::size_t character_count = 1;
    for (const std::wstring& entry : updated_entries) {
        if (entry.size() > std::numeric_limits<std::size_t>::max() - character_count - 1) {
            throw std::runtime_error("Windows child environment is too large");
        }
        character_count += entry.size() + 1;
    }
    constexpr std::size_t maximum_environment_characters = 32767;
    if (character_count > maximum_environment_characters) {
        throw std::runtime_error("Windows child environment exceeds 32767 characters");
    }
    std::vector<wchar_t> block;
    block.reserve(character_count);
    for (const std::wstring& entry : updated_entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

bool is_windows_null_device(const fs::path& path) {
    std::wstring value = path.native();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value == L"nul" || value == L"nul:" || value == L"\\\\.\\nul" ||
           value == L"\\\\.\\nul:";
}

std::wstring windows_open_path(const fs::path& path) {
    return is_windows_null_device(path) ? std::wstring(L"\\\\.\\NUL")
                                        : fs::absolute(path).native();
}

class WindowsHandle {
public:
    WindowsHandle() = default;
    explicit WindowsHandle(HANDLE handle) : handle_(handle) {}
    ~WindowsHandle() { reset(); }

    WindowsHandle(const WindowsHandle&) = delete;
    WindowsHandle& operator=(const WindowsHandle&) = delete;

    WindowsHandle(WindowsHandle&& other) noexcept : handle_(other.release()) {}
    WindowsHandle& operator=(WindowsHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    bool valid() const {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    HANDLE get() const { return handle_; }
    HANDLE release() {
        HANDLE result = handle_;
        handle_ = nullptr;
        return result;
    }
    void reset(HANDLE handle = nullptr) {
        if (valid()) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

class WindowsAttributeList {
public:
    explicit WindowsAttributeList(const std::vector<HANDLE>& inherited_handles) {
        SIZE_T size = 0;
        if (InitializeProcThreadAttributeList(nullptr, 1, 0, &size) ||
            GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
            throw std::runtime_error("failed to size Windows process attribute list: " +
                                     windows_error_message(GetLastError()));
        }
        list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            HeapAlloc(GetProcessHeap(), 0, size));
        if (!list_) {
            throw std::runtime_error("failed to allocate Windows process attribute list");
        }
        if (!InitializeProcThreadAttributeList(list_, 1, 0, &size)) {
            const DWORD error = GetLastError();
            HeapFree(GetProcessHeap(), 0, list_);
            list_ = nullptr;
            throw std::runtime_error("failed to initialize Windows process attribute list: " +
                                     windows_error_message(error));
        }
        if (!UpdateProcThreadAttribute(
                list_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                const_cast<HANDLE*>(inherited_handles.data()),
                inherited_handles.size() * sizeof(HANDLE), nullptr, nullptr)) {
            const DWORD error = GetLastError();
            DeleteProcThreadAttributeList(list_);
            HeapFree(GetProcessHeap(), 0, list_);
            list_ = nullptr;
            throw std::runtime_error("failed to restrict Windows inherited handles: " +
                                     windows_error_message(error));
        }
    }

    ~WindowsAttributeList() {
        if (list_) {
            DeleteProcThreadAttributeList(list_);
            HeapFree(GetProcessHeap(), 0, list_);
        }
    }

    WindowsAttributeList(const WindowsAttributeList&) = delete;
    WindowsAttributeList& operator=(const WindowsAttributeList&) = delete;

    LPPROC_THREAD_ATTRIBUTE_LIST get() const { return list_; }

private:
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
};

class WindowsChildGuard {
public:
    WindowsChildGuard(HANDLE process, HANDLE job) : process_(process), job_(job) {}
    ~WindowsChildGuard() {
        if (!armed_) {
            return;
        }
        if (assigned_to_job_) {
            if (!TerminateJobObject(job_, 125)) {
                TerminateProcess(process_, 125);
            }
        } else {
            TerminateProcess(process_, 125);
        }
        WaitForSingleObject(process_, INFINITE);
    }

    void assigned_to_job() { assigned_to_job_ = true; }
    void disarm() { armed_ = false; }

private:
    HANDLE process_ = nullptr;
    HANDLE job_ = nullptr;
    bool assigned_to_job_ = false;
    bool armed_ = true;
};

struct WindowsJobEvents {
    bool memory_limit_exceeded = false;
};

void drain_windows_job_events(HANDLE completion_port, WindowsJobEvents& events) {
    while (true) {
        DWORD message = 0;
        ULONG_PTR completion_key = 0;
        LPOVERLAPPED overlapped = nullptr;
        if (!GetQueuedCompletionStatus(completion_port, &message, &completion_key,
                                       &overlapped, 0)) {
            const DWORD error = GetLastError();
            if (error == WAIT_TIMEOUT) {
                return;
            }
            throw std::runtime_error("failed to read Windows job event: " +
                                     windows_error_message(error));
        }
        (void)completion_key;
        (void)overlapped;
        if (message == JOB_OBJECT_MSG_PROCESS_MEMORY_LIMIT ||
            message == JOB_OBJECT_MSG_JOB_MEMORY_LIMIT) {
            events.memory_limit_exceeded = true;
        }
    }
}

JOBOBJECT_BASIC_ACCOUNTING_INFORMATION windows_job_accounting(HANDLE job) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                                   &accounting, sizeof(accounting), nullptr)) {
        throw std::runtime_error("failed to query Windows job accounting: " +
                                 windows_error_message(GetLastError()));
    }
    return accounting;
}

std::uint64_t windows_job_cpu_time_ms(HANDLE job) {
    const auto accounting = windows_job_accounting(job);
    const std::uint64_t kernel = accounting.TotalKernelTime.QuadPart > 0
                                     ? static_cast<std::uint64_t>(
                                           accounting.TotalKernelTime.QuadPart) / 10000ULL
                                     : 0;
    const std::uint64_t user = accounting.TotalUserTime.QuadPart > 0
                                   ? static_cast<std::uint64_t>(
                                         accounting.TotalUserTime.QuadPart) / 10000ULL
                                   : 0;
    return kernel > std::numeric_limits<std::uint64_t>::max() - user
               ? std::numeric_limits<std::uint64_t>::max()
               : kernel + user;
}

void wait_for_windows_job_exit(HANDLE job, HANDLE completion_port,
                               WindowsJobEvents& events) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (true) {
        drain_windows_job_events(completion_port, events);
        if (windows_job_accounting(job).ActiveProcesses == 0) {
            return;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("Windows job did not terminate within 10 seconds");
        }
        Sleep(1);
    }
}

void terminate_windows_job(HANDLE job, HANDLE process, HANDLE completion_port,
                           WindowsJobEvents& events, DWORD exit_code) {
    const auto accounting = windows_job_accounting(job);
    if (accounting.ActiveProcesses != 0 && !TerminateJobObject(job, exit_code)) {
        throw std::runtime_error("failed to terminate Windows job: " +
                                 windows_error_message(GetLastError()));
    }
    const DWORD wait_result = WaitForSingleObject(process, INFINITE);
    if (wait_result != WAIT_OBJECT_0) {
        const DWORD error = wait_result == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
        throw std::runtime_error("failed to wait for Windows process termination: " +
                                 windows_error_message(error));
    }
    wait_for_windows_job_exit(job, completion_port, events);
}

SIZE_T windows_memory_limit_bytes(std::uint64_t memory_limit_mb) {
    constexpr std::uint64_t bytes_per_mb = 1024ULL * 1024ULL;
    if (memory_limit_mb == 0) {
        return 0;
    }
    if (memory_limit_mb >
        static_cast<std::uint64_t>(std::numeric_limits<SIZE_T>::max()) / bytes_per_mb) {
        throw std::runtime_error("memory limit exceeds the Windows job object capacity");
    }
    return static_cast<SIZE_T>(memory_limit_mb * bytes_per_mb);
}

std::uint64_t wall_timeout_guard_ms(std::uint64_t timeout_ms) {
    if (timeout_ms == 0) {
        return 0;
    }
    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    const std::uint64_t multiplied = timeout_ms > maximum / 4 ? maximum : timeout_ms * 4;
    const std::uint64_t padded = timeout_ms > maximum - 5000 ? maximum : timeout_ms + 5000;
    return std::max(multiplied, padded);
}

std::uint64_t windows_cpu_timeout_ms(std::uint64_t timeout_ms) {
    if (timeout_ms == 0) {
        return 0;
    }
    const std::uint64_t padding = std::max<std::uint64_t>(100, timeout_ms / 10);
    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    return timeout_ms > maximum - padding ? maximum : timeout_ms + padding;
}
#endif

std::string process_path_argument(const fs::path& path) {
#ifdef _WIN32
    return path.u8string();
#else
    return path.string();
#endif
}

ProcessResult run_program(const std::vector<std::string>& args,
                          const fs::path* working_dir,
                          const fs::path* stdout_path,
                          const fs::path* stderr_path,
                          std::uint64_t timeout_ms,
                          std::uint64_t memory_limit_mb,
                          std::uint64_t stack_limit_mb = 0,
                          std::uint64_t file_limit_mb = 0,
                          const std::function<bool()>& should_cancel = {},
                          const SandboxRunSpec* sandbox = nullptr,
                          bool attribute_memory_limit = false,
                          const std::vector<fs::path>& runtime_search_paths = {}) {
    if (args.empty() || args.front().empty()) {
        throw std::runtime_error("empty program command");
    }

    // Required isolation must retain a finite host-safety ceiling even when a
    // contest requests an unlimited or absurd resource value. Explicitly
    // unsafe local judging continues to honor the raw configuration.
    if (sandbox) {
        constexpr std::uint64_t kMaximumSandboxWallTimeMs = 5ULL * 60ULL * 1000ULL;
        constexpr std::uint64_t kMaximumSandboxMemoryMb = 4096;
        timeout_ms = timeout_ms == 0
                         ? kMaximumSandboxWallTimeMs
                         : std::min(timeout_ms, kMaximumSandboxWallTimeMs);
        memory_limit_mb = memory_limit_mb == 0
                              ? kMaximumSandboxMemoryMb
                              : std::min(memory_limit_mb, kMaximumSandboxMemoryMb);
        if (stack_limit_mb > kMaximumSandboxMemoryMb) {
            stack_limit_mb = kMaximumSandboxMemoryMb;
        }
    }

    std::vector<std::string> launch_arguments = args;
    detail::SandboxLaunch sandbox_launch;
    if (sandbox) {
        try {
            sandbox_launch = detail::prepare_sandbox_launch(
                sandbox->profile, sandbox->mounts, sandbox->temporary_filesystems,
                sandbox->guest_working_directory, args);
            launch_arguments = sandbox_launch.arguments;
        } catch (const std::exception& ex) {
            ProcessResult failure;
            failure.sandbox_setup_failed = true;
            failure.sandbox_error = ex.what();
            return failure;
        }
    }

#ifndef _WIN32
    (void)runtime_search_paths;
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

    launch_arguments.front() = resolve_posix_executable(launch_arguments.front(), working_dir);
    std::vector<char*> child_argv;
    child_argv.reserve(launch_arguments.size() + 1);
    for (std::string& argument : launch_arguments) {
        child_argv.push_back(argument.data());
    }
    child_argv.push_back(nullptr);

    std::vector<int> child_keep_fds;
    for (int fd : {sandbox_launch.filter_fd, sandbox_launch.status_write_fd}) {
        if (fd >= 3) {
            child_keep_fds.push_back(fd);
        }
    }
    std::sort(child_keep_fds.begin(), child_keep_fds.end());
    child_keep_fds.erase(std::unique(child_keep_fds.begin(), child_keep_fds.end()),
                         child_keep_fds.end());
    SandboxLaunchGuard sandbox_guard(sandbox_launch);

    auto begin = std::chrono::steady_clock::now();
    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("fork failed");
    }

    if (pid == 0) {
        detail::close_sandbox_child_fds_before_exec(sandbox_launch);
        setpgid(0, 0);
        if (working_dir && chdir(working_dir->c_str()) != 0) {
            _exit(125);
        }
        int null_stdin = open("/dev/null", O_RDONLY);
        if (null_stdin < 0) {
            _exit(126);
        }
        if (null_stdin != STDIN_FILENO) {
            if (dup2(null_stdin, STDIN_FILENO) < 0) {
                _exit(126);
            }
            close(null_stdin);
        }
        if (stdout_path) {
            int fd = open(stdout_path->c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) _exit(126);
            if (fd != STDOUT_FILENO) {
                if (dup2(fd, STDOUT_FILENO) < 0) _exit(126);
                close(fd);
            }
        }
        if (stderr_path) {
            int fd = open(stderr_path->c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) _exit(126);
            if (fd != STDERR_FILENO) {
                if (dup2(fd, STDERR_FILENO) < 0) _exit(126);
                close(fd);
            }
        }
        close_child_file_descriptors_except(child_keep_fds);
#if defined(__linux__)
        if (sandbox) {
            // Affinity is inherited by compiler children and target threads.
            // Different invocations select different allowed CPUs, preserving
            // judge parallelism without letting one invocation fan out across
            // the whole host.
            restrict_child_to_one_cpu();
        }
#endif
        apply_child_limits(memory_limit_mb, stack_limit_mb, file_limit_mb);

        execv(child_argv.front(), child_argv.data());
        _exit(127);
    }

    // Establish the process group from both sides of the fork so every error
    // path can reliably terminate the whole sandbox tree.
    if (setpgid(pid, pid) != 0 && errno != EACCES && errno != ESRCH) {
        kill_and_reap_process_group(pid);
        throw std::runtime_error("failed to establish child process group");
    }
    ChildProcessGuard child_guard(pid);
    detail::close_sandbox_parent_fds_after_fork(sandbox_launch);

    int status = 0;
    bool timed_out = false;
    bool memory_pressure_observed = false;
#if defined(__linux__)
    auto next_memory_sample = begin;
#else
    (void)attribute_memory_limit;
#endif
    while (true) {
        const auto now = std::chrono::steady_clock::now();
#if defined(__linux__)
        if (attribute_memory_limit && !memory_pressure_observed &&
            now >= next_memory_sample) {
            memory_pressure_observed =
                process_tree_near_address_space_limit(pid, memory_limit_mb);
            next_memory_sample = now + std::chrono::milliseconds(5);
        }
#endif
        pid_t done = waitpid_retry(pid, &status, WNOHANG);
        if (done == pid) {
            child_guard.disarm();
            break;
        }
        if (done < 0) {
            throw std::runtime_error("waitpid failed");
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - begin).count();
        if (should_cancel && should_cancel()) {
            kill_and_reap_process_group(pid);
            child_guard.disarm();
            throw std::runtime_error("judging cancelled");
        }
        if (timeout_ms > 0 && static_cast<std::uint64_t>(elapsed) > timeout_ms) {
            timed_out = true;
            kill_and_reap_process_group(pid);
            child_guard.disarm();
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
    }
    result.memory_exceeded = !timed_out && result.exit_code != 0 &&
                             memory_pressure_observed;
    if (sandbox && !detail::sandbox_reported_child_start(sandbox_launch)) {
        result.sandbox_setup_failed = true;
        result.sandbox_error = "bubblewrap failed before starting the isolated process";
    }
    detail::close_sandbox_launch(sandbox_launch);
    return result;
#else
    (void)stack_limit_mb;
    (void)file_limit_mb;
    (void)sandbox;

    fs::path absolute_working_dir;
    if (working_dir) {
        absolute_working_dir = fs::absolute(*working_dir);
        working_dir = &absolute_working_dir;
    }

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;

    auto open_file = [&](const std::wstring& path, DWORD access, DWORD creation) {
        return CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &sa, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
    };

    WindowsHandle opened_stdin(open_file(L"\\\\.\\NUL", GENERIC_READ, OPEN_EXISTING));
    if (!opened_stdin.valid()) {
        throw std::runtime_error("failed to open NUL: " + windows_error_message(GetLastError()));
    }

    WindowsHandle opened_stdout;
    if (stdout_path) {
        opened_stdout.reset(
            open_file(windows_open_path(*stdout_path), GENERIC_WRITE, CREATE_ALWAYS));
    } else {
        opened_stdout.reset(open_file(L"\\\\.\\NUL", GENERIC_WRITE, OPEN_EXISTING));
    }
    if (!opened_stdout.valid()) {
        const DWORD error = GetLastError();
        throw std::runtime_error(std::string("failed to open stdout ") +
                                 (stdout_path ? "file: " : "NUL: ") +
                                 windows_error_message(error));
    }

    WindowsHandle opened_stderr;
    if (stderr_path) {
        opened_stderr.reset(
            open_file(windows_open_path(*stderr_path), GENERIC_WRITE, CREATE_ALWAYS));
    } else {
        opened_stderr.reset(open_file(L"\\\\.\\NUL", GENERIC_WRITE, OPEN_EXISTING));
    }
    if (!opened_stderr.valid()) {
        const DWORD error = GetLastError();
        throw std::runtime_error(std::string("failed to open stderr ") +
                                 (stderr_path ? "file: " : "NUL: ") +
                                 windows_error_message(error));
    }

    const std::vector<HANDLE> inherited_handles = {
        opened_stdin.get(), opened_stdout.get(), opened_stderr.get()
    };
    WindowsAttributeList attribute_list(inherited_handles);

    WindowsHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job.valid()) {
        throw std::runtime_error("failed to create Windows job object: " +
                                 windows_error_message(GetLastError()));
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    const SIZE_T memory_limit_bytes = windows_memory_limit_bytes(memory_limit_mb);
    if (memory_limit_bytes != 0) {
        limits.BasicLimitInformation.LimitFlags |=
            JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_JOB_MEMORY;
        limits.ProcessMemoryLimit = memory_limit_bytes;
        limits.JobMemoryLimit = memory_limit_bytes;
    }
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
        throw std::runtime_error("failed to configure Windows job limits: " +
                                 windows_error_message(GetLastError()));
    }

    WindowsHandle completion_port(
        CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
    if (!completion_port.valid()) {
        throw std::runtime_error("failed to create Windows job completion port: " +
                                 windows_error_message(GetLastError()));
    }
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT association{};
    association.CompletionKey = job.get();
    association.CompletionPort = completion_port.get();
    if (!SetInformationJobObject(job.get(), JobObjectAssociateCompletionPortInformation,
                                 &association, sizeof(association))) {
        throw std::runtime_error("failed to monitor Windows job object: " +
                                 windows_error_message(GetLastError()));
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = opened_stdin.get();
    startup.StartupInfo.hStdOutput = opened_stdout.get();
    startup.StartupInfo.hStdError = opened_stderr.get();
    startup.lpAttributeList = attribute_list.get();

    PROCESS_INFORMATION raw_process{};
    std::wstring command_line = windows_command_line(launch_arguments);
    std::vector<wchar_t> child_environment =
        windows_environment_with_search_paths(runtime_search_paths);
    const std::string application_name = launch_arguments.front();
    const std::wstring working_directory =
        working_dir ? working_dir->native() : std::wstring();
    const BOOL ok = CreateProcessW(
        nullptr, command_line.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT |
            (child_environment.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT),
        child_environment.empty() ? nullptr : child_environment.data(),
        working_dir ? working_directory.c_str() : nullptr,
        &startup.StartupInfo, &raw_process);
    if (!ok) {
        const DWORD error = GetLastError();
        throw std::runtime_error("CreateProcess failed for " + application_name + ": " +
                                 windows_error_message(error));
    }

    WindowsHandle process(raw_process.hProcess);
    WindowsHandle primary_thread(raw_process.hThread);
    WindowsChildGuard child_guard(process.get(), job.get());
    if (!AssignProcessToJobObject(job.get(), process.get())) {
        throw std::runtime_error("failed to assign suspended process to Windows job: " +
                                 windows_error_message(GetLastError()));
    }
    child_guard.assigned_to_job();

    const auto begin = std::chrono::steady_clock::now();
    if (ResumeThread(primary_thread.get()) == static_cast<DWORD>(-1)) {
        throw std::runtime_error("failed to resume Windows process: " +
                                 windows_error_message(GetLastError()));
    }

    bool timed_out = false;
    std::uint64_t cpu_elapsed_ms = 0;
    const std::uint64_t wall_guard_ms = wall_timeout_guard_ms(timeout_ms);
    const std::uint64_t cpu_timeout_ms = windows_cpu_timeout_ms(timeout_ms);
    WindowsJobEvents job_events;
    while (true) {
        const DWORD wait_result = WaitForSingleObject(process.get(), 10);
        drain_windows_job_events(completion_port.get(), job_events);
        if (wait_result == WAIT_OBJECT_0) {
            break;
        }
        if (wait_result != WAIT_TIMEOUT) {
            const DWORD error = wait_result == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
            throw std::runtime_error("failed while waiting for Windows process: " +
                                     windows_error_message(error));
        }
        const auto now = std::chrono::steady_clock::now();
        const auto wall_elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - begin).count();
        cpu_elapsed_ms = windows_job_cpu_time_ms(job.get());
        if (should_cancel && should_cancel()) {
            terminate_windows_job(job.get(), process.get(), completion_port.get(),
                                  job_events, 125);
            child_guard.disarm();
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

    DWORD exit_code = 0;
    if (timed_out) {
        terminate_windows_job(job.get(), process.get(), completion_port.get(),
                              job_events, 124);
    } else {
        if (!GetExitCodeProcess(process.get(), &exit_code)) {
            const DWORD error = GetLastError();
            throw std::runtime_error("failed to read Windows process exit code: " +
                                     windows_error_message(error));
        }
        // The primary process may have spawned descendants and exited. Do not
        // return until the whole job is empty, even on an otherwise successful run.
        terminate_windows_job(job.get(), process.get(), completion_port.get(),
                              job_events, 125);
    }
    if (timed_out && !GetExitCodeProcess(process.get(), &exit_code)) {
        throw std::runtime_error("failed to read terminated Windows process exit code: " +
                                 windows_error_message(GetLastError()));
    }
    drain_windows_job_events(completion_port.get(), job_events);
    cpu_elapsed_ms = windows_job_cpu_time_ms(job.get());
    child_guard.disarm();

    ProcessResult result;
    result.exit_code = static_cast<int>(exit_code);
    result.timed_out = timed_out;
    result.memory_exceeded = attribute_memory_limit && !timed_out &&
                             job_events.memory_limit_exceeded;
    result.elapsed_ms = cpu_elapsed_ms;
    return result;
#endif
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to read " + process_path_argument(path));
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string read_file_limited(const fs::path& path, std::size_t maximum_bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to read " + process_path_argument(path));
    }
    std::string contents(maximum_bytes + 1, '\0');
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    const std::size_t count = static_cast<std::size_t>(input.gcount());
    contents.resize(std::min(count, maximum_bytes));
    if (count > maximum_bytes) {
        contents += "\n[diagnostic truncated by NeoThemis]\n";
    }
    return contents;
}

void prepare_empty_file(const fs::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to prepare " + process_path_argument(path));
    }
}

class ScopedPathCleanup {
public:
    ScopedPathCleanup(std::initializer_list<fs::path> paths, bool enabled)
        : paths_(paths), enabled_(enabled) {}

    ~ScopedPathCleanup() {
        if (!enabled_) {
            return;
        }
        for (const fs::path& path : paths_) {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    }

private:
    std::vector<fs::path> paths_;
    bool enabled_ = false;
};

std::vector<std::string> split_words(const std::string& text) {
    std::vector<std::string> result;
    std::string token;
    char quote = '\0';

    auto flush = [&]() {
        if (!token.empty()) {
            result.push_back(token);
            token.clear();
        }
    };

    for (std::size_t i = 0; i < text.size(); ++i) {
        char ch = text[i];
        if (ch == '\\' && quote != '\0' && i + 1 < text.size() &&
            (text[i + 1] == quote || text[i + 1] == '\\')) {
            token.push_back(text[++i]);
            continue;
        }
        if ((ch == '"' || ch == '\'') && (quote == '\0' || quote == ch)) {
            quote = quote == ch ? '\0' : ch;
            continue;
        }
        if (quote == '\0' && std::isspace(static_cast<unsigned char>(ch))) {
            flush();
            continue;
        }
        token.push_back(ch);
    }
    flush();
    return result;
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool path_components_equal(const fs::path& left, const fs::path& right) {
#ifdef _WIN32
    std::wstring folded_left = left.native();
    std::wstring folded_right = right.native();
    std::transform(folded_left.begin(), folded_left.end(), folded_left.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    std::transform(folded_right.begin(), folded_right.end(), folded_right.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return folded_left == folded_right;
#else
    return left == right;
#endif
}

bool canonical_paths_equal(const fs::path& left, const fs::path& right) {
    auto left_part = left.begin();
    auto right_part = right.begin();
    for (; left_part != left.end() && right_part != right.end();
         ++left_part, ++right_part) {
        if (!path_components_equal(*left_part, *right_part)) {
            return false;
        }
    }
    return left_part == left.end() && right_part == right.end();
}

bool canonical_path_is_below(const fs::path& candidate, const fs::path& parent) {
    auto parent_part = parent.begin();
    auto candidate_part = candidate.begin();
    for (; parent_part != parent.end(); ++parent_part, ++candidate_part) {
        if (candidate_part == candidate.end() ||
            !path_components_equal(*candidate_part, *parent_part)) {
            return false;
        }
    }
    return candidate_part != candidate.end();
}

fs::path canonical_real_direct_entry(const fs::directory_entry& entry,
                                     const fs::path& canonical_parent,
                                     fs::file_type expected_type) {
    std::error_code status_error;
    const fs::file_status status = entry.symlink_status(status_error);
    if (status_error || fs::is_symlink(status) || status.type() != expected_type) {
        return {};
    }

    std::error_code canonical_error;
    const fs::path canonical_entry = fs::canonical(entry.path(), canonical_error);
    const fs::path direct_entry = canonical_parent / entry.path().filename();
    if (canonical_error || !canonical_path_is_below(canonical_entry, canonical_parent) ||
        !canonical_paths_equal(canonical_entry.parent_path(), canonical_parent) ||
        !canonical_paths_equal(canonical_entry, direct_entry)) {
        return {};
    }
    return canonical_entry;
}

fs::path canonical_real_direct_file(const fs::path& parent, const fs::path& candidate) {
    std::error_code parent_error;
    const fs::path canonical_parent = fs::canonical(parent, parent_error);
    if (parent_error) {
        return {};
    }
    return canonical_real_direct_entry(fs::directory_entry(candidate), canonical_parent,
                                       fs::file_type::regular);
}

fs::path require_real_file_within(const fs::path& parent,
                                  const fs::path& relative,
                                  const std::string& description) {
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
        relative.has_root_directory()) {
        throw std::runtime_error(description + " path must be relative to its folder");
    }

    std::error_code parent_error;
    const fs::path canonical_parent = fs::canonical(parent, parent_error);
    if (parent_error) {
        throw std::runtime_error("failed to resolve " + description + " folder");
    }

    fs::path current = canonical_parent;
    std::size_t component_count = 0;
    for (const fs::path& component : relative) {
        if (component.empty() || component == "." || component == "..") {
            throw std::runtime_error(description +
                                     " path contains an unsafe path component");
        }
        ++component_count;
    }
    if (component_count == 0) {
        throw std::runtime_error(description + " path is empty");
    }

    std::size_t component_index = 0;
    for (const fs::path& component : relative) {
        ++component_index;
        current /= component;
        std::error_code status_error;
        const fs::file_status status = fs::symlink_status(current, status_error);
        if (status_error || fs::is_symlink(status)) {
            throw std::runtime_error(description + " path must not contain symlinks");
        }
        const bool final_component = component_index == component_count;
        if ((final_component && !fs::is_regular_file(status)) ||
            (!final_component && !fs::is_directory(status))) {
            throw std::runtime_error(description +
                                     (final_component ? " is not a regular file"
                                                      : " path contains a non-directory"));
        }
    }

    std::error_code canonical_error;
    const fs::path canonical_file = fs::canonical(current, canonical_error);
    if (canonical_error || !canonical_path_is_below(canonical_file, canonical_parent)) {
        throw std::runtime_error(description + " path escapes its folder");
    }
    return canonical_file;
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

    const std::string compiler_name =
        lower_ascii(fs::u8path(compiler).filename().u8string());
    filtered.push_back(compiler_name == "cl" || compiler_name == "cl.exe"
                           ? "/WX-"
                           : "-Wno-error");
    return filtered;
}

std::string unquote_configured_program(std::string value) {
    value = trim(value);
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

std::vector<std::string> configured_compiler_args(std::string value) {
    value = trim(value);
    if (value.empty()) {
        return {""};
    }

    const bool starts_with_quote = value.front() == '"' || value.front() == '\'';
    if (!starts_with_quote) {
        std::error_code ec;
        if (fs::exists(fs::u8path(value), ec) && !ec) {
            return {value};
        }
    }

    std::vector<std::string> args = split_words(value);
    if (args.empty()) {
        args.push_back(unquote_configured_program(value));
    }
    return args;
}

std::vector<std::string> stack_compile_args(const std::string& compiler,
                                            std::uint64_t stack_limit_mb) {
    if (stack_limit_mb == 0) {
        return {};
    }

#ifdef _WIN32
    constexpr std::uint64_t bytes_per_mb = 1024ULL * 1024ULL;
    std::string lower_compiler =
        lower_ascii(fs::u8path(compiler).filename().u8string());
    if (stack_limit_mb > std::numeric_limits<std::uint64_t>::max() / bytes_per_mb) {
        throw std::runtime_error("stack limit is too large for the Windows linker");
    }
    const std::uint64_t bytes = stack_limit_mb * bytes_per_mb;
    if (lower_compiler == "cl" || lower_compiler == "cl.exe") {
        return {"/F" + std::to_string(bytes), "/link", "/STACK:" + std::to_string(bytes)};
    }
    return {"-Wl,--stack," + std::to_string(bytes)};
#else
    (void)compiler;
    (void)stack_limit_mb;
    return {};
#endif
}

std::vector<std::string> stack_guard_compile_args(const std::string& compiler,
                                                  std::uint64_t stack_limit_mb) {
    std::vector<std::string> result = stack_compile_args(compiler, stack_limit_mb);
    std::string lower_compiler =
        lower_ascii(fs::u8path(compiler).filename().u8string());
    if (stack_limit_mb > 0 && lower_compiler != "cl" && lower_compiler != "cl.exe") {
        result.push_back("-fno-optimize-sibling-calls");
    }
    return result;
}

std::vector<std::string> compiler_base_args(const JudgeOptions& options) {
    std::vector<std::string> args = configured_compiler_args(options.compiler);
    const std::string compiler = args.empty() ? std::string() : args.front();
    std::vector<std::string> flags =
        warning_tolerant_compile_flags(compiler, options.compile_flags);
    args.insert(args.end(), flags.begin(), flags.end());
    std::vector<std::string> stack_args =
        stack_guard_compile_args(compiler, options.stack_limit_mb);
    args.insert(args.end(), stack_args.begin(), stack_args.end());
    return args;
}

std::vector<fs::path> compiler_runtime_search_paths(const JudgeOptions& options) {
    const std::vector<std::string> args = configured_compiler_args(options.compiler);
    if (args.empty() || args.front().empty()) {
        return {};
    }

    const fs::path compiler_path = fs::u8path(args.front());
    const fs::path compiler_directory = compiler_path.parent_path();
    if (compiler_directory.empty()) {
        // A bare program name is already resolved through the inherited PATH.
        return {};
    }
    return {compiler_directory};
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
        throw std::runtime_error("failed to copy " + process_path_argument(from) + " to " +
                                 process_path_argument(to) + ": " + ec.message());
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
    add_name(process_path_argument(input.stem()));
    fs::path primary;
    for (const auto& name : names) {
        fs::path target = run_dir / fs::u8path(name);
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

    std::error_code parent_error;
    const fs::path canonical_dir = fs::canonical(dir, parent_error);
    if (parent_error) {
        return {};
    }
    for (const auto& entry : fs::directory_iterator(canonical_dir)) {
        const fs::path candidate = canonical_real_direct_entry(
            entry, canonical_dir, fs::file_type::regular);
        if (candidate.empty()) {
            continue;
        }
        std::string filename = lower_ascii(process_path_argument(candidate.filename()));
        if (lowered_names.find(filename) != lowered_names.end()) {
            return candidate;
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

std::set<std::string> output_alias_names(const std::string& problem,
                                         const std::string& test_name) {
    std::set<std::string> candidates;
    auto add_names = [&](const std::string& stem) {
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
    add_names(problem);
    add_names(test_name);
    return candidates;
}

std::vector<fs::path> prepare_sandbox_output_aliases(const fs::path& run_dir,
                                                     const std::string& problem,
                                                     const std::string& test_name,
                                                     const std::string& sentinel) {
    const std::set<std::string> names = output_alias_names(problem, test_name);
    std::vector<fs::path> paths;
    if (names.empty()) {
        return paths;
    }
    const fs::path primary = run_dir / fs::u8path(*names.begin());
    {
        std::ofstream output(primary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("failed to prepare sandbox output file");
        }
        output.write(sentinel.data(), static_cast<std::streamsize>(sentinel.size()));
    }
    paths.push_back(primary);
    for (auto name = std::next(names.begin()); name != names.end(); ++name) {
        fs::path alias = run_dir / fs::u8path(*name);
        std::error_code ec;
        fs::create_hard_link(primary, alias, ec);
        if (ec) {
            fs::copy_file(primary, alias, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            throw std::runtime_error("failed to prepare sandbox output alias " +
                                     process_path_argument(alias));
        }
        paths.push_back(std::move(alias));
    }
    return paths;
}

fs::path sandbox_written_output(const std::vector<fs::path>& aliases,
                                const std::string& sentinel) {
    for (const fs::path& alias : aliases) {
        std::error_code ec;
        if (!fs::is_regular_file(alias, ec) || ec) {
            continue;
        }
        if (fs::file_size(alias, ec) != sentinel.size() || ec) {
            return alias;
        }
        try {
            if (read_file(alias) != sentinel) {
                return alias;
            }
        } catch (const std::exception&) {
            return alias;
        }
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
    std::error_code parent_error;
    const fs::path canonical_parent = fs::canonical(path, parent_error);
    if (parent_error) {
        return entries;
    }
    for (const auto& entry : fs::directory_iterator(canonical_parent)) {
        const fs::path candidate = canonical_real_direct_entry(
            entry, canonical_parent, fs::file_type::directory);
        if (!candidate.empty()) {
            entries.emplace_back(candidate);
        }
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return process_path_argument(a.path().filename()) <
               process_path_argument(b.path().filename());
    });
    return entries;
}

std::vector<fs::path> sorted_sources(const fs::path& contestant_dir) {
    static const std::vector<std::string> extensions{".cpp", ".cc", ".cxx"};
    std::vector<fs::path> files;
    std::error_code parent_error;
    const fs::path canonical_parent = fs::canonical(contestant_dir, parent_error);
    if (parent_error) {
        return files;
    }
    for (const auto& entry : fs::directory_iterator(canonical_parent)) {
        const fs::path candidate = canonical_real_direct_entry(
            entry, canonical_parent, fs::file_type::regular);
        if (candidate.empty()) {
            continue;
        }
        std::string ext = process_path_argument(candidate.extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (std::find(extensions.begin(), extensions.end(), ext) != extensions.end()) {
            files.push_back(candidate);
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::vector<ContestantContext> build_contestant_contexts(const fs::path& contestants_root) {
    std::vector<ContestantContext> contestants;
    for (const auto& contestant_entry : sorted_directories(contestants_root)) {
        ContestantContext contestant;
        contestant.name = process_path_argument(contestant_entry.path().filename());
        contestant.dir = contestant_entry.path();
        for (const auto& source : sorted_sources(contestant.dir)) {
            std::string key = lower_ascii(process_path_argument(source.stem()));
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
    constexpr std::size_t kMaximumDiagnosticBytes = 64 * 1024;
    for (const auto& path : paths) {
        if (fs::exists(path)) {
            try {
                std::string text = read_file_limited(path, kMaximumDiagnosticBytes);
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

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool is_testlib_checker_setting(const std::string& setting) {
    return setting == "testlib" || starts_with(setting, "testlib:");
}

fs::path resolve_checker_source(const ProblemConfig& settings,
                                const fs::path& problem_dir) {
    if (settings.checker == "token") {
        return {};
    }

    const bool custom =
        settings.checker == "custom" || starts_with(settings.checker, "custom:");
    const bool testlib = is_testlib_checker_setting(settings.checker);
    if (custom || testlib) {
        const std::string prefix = testlib ? "testlib:" : "custom:";
        fs::path source = settings.checker == (testlib ? "testlib" : "custom")
                              ? fs::path("checker.cpp")
                              : fs::u8path(settings.checker.substr(prefix.size()));
        if (source.empty()) {
            source = "checker.cpp";
        }
        return require_real_file_within(
            problem_dir, source, testlib ? "testlib checker" : "custom checker");
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

fs::path compile_checker(const ProblemConfig& settings,
                         const JudgeOptions& options,
                         const fs::path& problem_dir,
                         const fs::path& checker_build_dir) {
    fs::path checker_source = resolve_checker_source(settings, problem_dir);
    if (checker_source.empty()) {
        return {};
    }
    bool needs_testlib = checker_source_includes_testlib(checker_source);
    fs::path local_testlib = problem_dir / "testlib.h";
    if (needs_testlib && canonical_real_direct_file(problem_dir, local_testlib).empty()) {
        throw std::runtime_error(
            "checker includes testlib.h but a real, non-symlinked testlib.h was not found");
    }

    fs::create_directories(checker_build_dir);
#ifdef _WIN32
    fs::path checker_executable = checker_build_dir / "checker.exe";
#else
    fs::path checker_executable = checker_build_dir / "checker";
#endif
    fs::path compile_log = checker_build_dir / "checker-compile.err";
    ScopedPathCleanup compile_log_cleanup({compile_log}, !options.keep_workdir);
    std::vector<std::string> compile_args = compiler_base_args(options);
    const std::vector<fs::path> compiler_search_paths =
        compiler_runtime_search_paths(options);
    SandboxRunSpec sandbox_spec;
    const SandboxRunSpec* sandbox = nullptr;
    if (options.execution_security == ExecutionSecurity::Required) {
        prepare_empty_file(checker_executable);
        const fs::path relative_source = fs::relative(checker_source, problem_dir);
        compile_args.insert(compile_args.end(),
                            {"-I", "/src", "/src/" + relative_source.generic_string(),
                             "-o", "/out/" +
                                       process_path_argument(checker_executable.filename())});
        sandbox_spec.profile = detail::SandboxProfile::Compiler;
        sandbox_spec.guest_working_directory = "/out";
        sandbox_spec.mounts = {{problem_dir, "/src", true},
                               {checker_executable,
                                "/out/" +
                                    process_path_argument(checker_executable.filename()), false}};
        sandbox = &sandbox_spec;
    } else {
        compile_args.insert(compile_args.end(),
                            {"-I", process_path_argument(problem_dir),
                             process_path_argument(checker_source), "-o",
                             process_path_argument(checker_executable)});
    }
    ProcessResult compile = run_program(compile_args, nullptr, nullptr, &compile_log,
                                        30000, 1024, 256, 64,
                                        options.should_cancel, sandbox, false,
                                        compiler_search_paths);
    if (compile.sandbox_setup_failed) {
        throw std::runtime_error("checker sandbox setup failed: " + compile.sandbox_error);
    }
    if (compile.timed_out || compile.exit_code != 0 ||
        !nonempty_executable_exists(checker_executable)) {
        std::string message = read_checker_message(compile_log);
        if (message.empty()) {
            message = compile.timed_out
                          ? "checker compiler time limit exceeded"
                          : "compiler exited with code " + std::to_string(compile.exit_code) +
                                " without producing a checker executable";
        }
        throw std::runtime_error("checker compile failed: " + message);
    }
    return checker_executable;
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

std::vector<TestResult> rows_for_problem_tests(const std::string& contestant,
                                               const ProblemContext& problem,
                                               Verdict verdict,
                                               int exit_code,
                                               std::uint64_t time_ms,
                                               const std::string& message) {
    std::vector<TestResult> rows;
    for (const auto& test_entry : problem.tests) {
        std::string test_name = process_path_argument(test_entry.path().filename());
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

    // Keep every user-controlled path component below a fixed artifact namespace.
    // In particular, a contestant called "checkers" must not share a directory
    // with NeoThemis' custom checker binaries, and a test named after its problem
    // must not be able to replace the submission executable.
    fs::path submission_dir = work_root / "submissions" /
                              fs::u8path(prepared.contestant) /
                              fs::u8path(problem.name);
    fs::path build_dir = submission_dir / "build";
    fs::create_directories(build_dir);
#ifdef _WIN32
    prepared.executable = build_dir / "program.exe";
#else
    prepared.executable = build_dir / "program";
#endif
    fs::path compile_log = build_dir / "compile.err";
    ScopedPathCleanup compile_log_cleanup({compile_log}, !options.keep_workdir);
    std::vector<std::string> compile_args = compiler_base_args(options);
    const std::vector<fs::path> compiler_search_paths =
        compiler_runtime_search_paths(options);
    SandboxRunSpec sandbox_spec;
    const SandboxRunSpec* sandbox = nullptr;
    if (options.execution_security == ExecutionSecurity::Required) {
        prepare_empty_file(prepared.executable);
        compile_args.insert(compile_args.end(),
                            {"/src/main" + process_path_argument(source.extension()), "-o",
                             "/out/" +
                                 process_path_argument(prepared.executable.filename())});
        sandbox_spec.profile = detail::SandboxProfile::Compiler;
        sandbox_spec.guest_working_directory = "/out";
        sandbox_spec.mounts = {
            {source, "/src/main" + process_path_argument(source.extension()), true},
                               {prepared.executable,
                                "/out/" +
                                    process_path_argument(prepared.executable.filename()), false}};
        sandbox = &sandbox_spec;
    } else {
        compile_args.insert(compile_args.end(),
                            {process_path_argument(source), "-o",
                             process_path_argument(prepared.executable)});
    }
    ProcessResult compile = run_program(compile_args, nullptr, nullptr, &compile_log,
                                        30000, 1024, 256, 64,
                                        options.should_cancel, sandbox, false,
                                        compiler_search_paths);
    if (compile.sandbox_setup_failed) {
        prepared.immediate_results =
            rows_for_problem_tests(prepared.contestant, problem, Verdict::InternalError,
                                   -1, 0, compile.sandbox_error);
        return prepared;
    }
    if (compile.timed_out || compile.exit_code != 0 ||
        !nonempty_executable_exists(prepared.executable)) {
        std::string message = first_existing_file_text({compile_log});
        if (message.empty()) {
            message = compile.timed_out
                          ? "compiler time limit exceeded"
                          : "compiler exited with code " + std::to_string(compile.exit_code) +
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
    std::string test_name = process_path_argument(job.test.path().filename());
    fs::path input = find_test_file(job.test.path(), problem.name, ".inp");
    fs::path expected = find_test_file(job.test.path(), problem.name, ".out");
    const fs::path submission_dir = job.executable.parent_path().parent_path();
    const fs::path test_artifact_dir = submission_dir / "tests" / fs::u8path(test_name);
    fs::path run_dir = test_artifact_dir / "work";
    fs::path run_log = test_artifact_dir / "run.err";
    fs::path checker_stdout = test_artifact_dir / "checker.out";
    fs::path checker_stderr = test_artifact_dir / "checker.err";
    ScopedPathCleanup artifact_cleanup({test_artifact_dir}, !options.keep_workdir);

    TestResult row;
    row.contestant = job.contestant;
    row.problem = problem.name;
    row.test = test_name;
    row.max_points = points_for_test(problem.settings, test_name);
    const std::vector<fs::path> compiler_search_paths =
        compiler_runtime_search_paths(options);

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
    std::vector<std::string> run_arguments{process_path_argument(executable_path)};
    std::vector<fs::path> sandbox_outputs;
    std::string sandbox_output_sentinel;
    SandboxRunSpec submission_sandbox;
    const SandboxRunSpec* submission_sandbox_ptr = nullptr;
    if (options.execution_security == ExecutionSecurity::Required) {
        std::vector<fs::path> input_aliases;
        for (const auto& entry : fs::directory_iterator(run_dir)) {
            std::error_code ec;
            if (entry.is_regular_file(ec) && !ec) {
                input_aliases.push_back(entry.path());
            }
        }
        sandbox_output_sentinel = "NEOTHEMIS-UNWRITTEN-" +
                                  std::to_string(std::hash<std::string>{}(
                                      process_path_argument(run_dir))) +
                                  "-" + std::to_string(
                                      std::chrono::steady_clock::now().time_since_epoch().count());
        sandbox_outputs = prepare_sandbox_output_aliases(
            run_dir, problem.name, test_name, sandbox_output_sentinel);
        run_arguments = {"/program"};
        submission_sandbox.profile = detail::SandboxProfile::Submission;
        submission_sandbox.guest_working_directory = "/work";
        submission_sandbox.temporary_filesystems = {{"/work", 64ULL * 1024ULL * 1024ULL}};
        submission_sandbox.mounts = {{executable_path, "/program", true}};
        for (const fs::path& alias : input_aliases) {
            submission_sandbox.mounts.push_back(
                {alias, "/work/" + process_path_argument(alias.filename()), true});
        }
        for (const fs::path& alias : sandbox_outputs) {
            submission_sandbox.mounts.push_back(
                {alias, "/work/" + process_path_argument(alias.filename()), false});
        }
        submission_sandbox_ptr = &submission_sandbox;
    }
    ProcessResult run = run_program(run_arguments, &run_dir, &stdout_sink, &run_log,
                                    problem.settings.time_limit_ms,
                                    problem.settings.memory_limit_mb,
                                    options.stack_limit_mb, 64,
                                    options.should_cancel, submission_sandbox_ptr, true,
                                    compiler_search_paths);
    row.time_ms = run.elapsed_ms;
    row.exit_code = run.exit_code;
    if (run.sandbox_setup_failed) {
        row.verdict = Verdict::InternalError;
        row.message = run.sandbox_error;
    } else if (run.timed_out) {
        row.verdict = Verdict::TimeLimitExceeded;
        row.message = "time limit exceeded";
    } else if (run.memory_exceeded) {
        row.verdict = Verdict::MemoryLimitExceeded;
        row.message = "memory limit exceeded";
    } else if (run.exit_code != 0) {
        row.verdict = Verdict::RuntimeError;
        row.message = first_existing_file_text({run_log});
    } else {
        fs::path actual = options.execution_security == ExecutionSecurity::Required
                              ? sandbox_written_output(sandbox_outputs,
                                                       sandbox_output_sentinel)
                              : find_actual_output(run_dir, problem.name, test_name);
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
            fs::path checker_executable = fs::absolute(problem.checker_executable);
            fs::path checker_input = find_case_insensitive_file(
                run_dir, {process_path_argument(input.filename())});
            if (checker_input.empty()) {
                checker_input = input;
            }
            std::vector<std::string> checker_arguments{
                process_path_argument(checker_executable),
                process_path_argument(checker_input.filename()),
                process_path_argument(actual.filename()),
                process_path_argument(fs::absolute(expected))};
            SandboxRunSpec checker_sandbox;
            const SandboxRunSpec* checker_sandbox_ptr = nullptr;
            if (options.execution_security == ExecutionSecurity::Required) {
                checker_arguments = {
                    "/checker", process_path_argument(checker_input.filename()),
                    process_path_argument(actual.filename()), "/answer/expected.out"};
                checker_sandbox.profile = detail::SandboxProfile::Checker;
                checker_sandbox.guest_working_directory = "/work";
                checker_sandbox.temporary_filesystems = {{"/work", 16ULL * 1024ULL * 1024ULL}};
                checker_sandbox.mounts = {{checker_executable, "/checker", true},
                                          {fs::absolute(input), "/input/original.inp", true},
                                          {fs::absolute(actual), "/actual/output.out", true},
                                          {fs::absolute(expected), "/answer/expected.out", true}};
                checker_arguments[1] = "/input/original.inp";
                checker_arguments[2] = "/actual/output.out";
                checker_sandbox_ptr = &checker_sandbox;
            }
            ProcessResult checker = run_program(
                checker_arguments, &run_dir, &checker_stdout, &checker_stderr,
                problem.settings.time_limit_ms, problem.settings.memory_limit_mb,
                options.stack_limit_mb, 16, options.should_cancel, checker_sandbox_ptr, true,
                compiler_search_paths);
            row.exit_code = checker.exit_code;
            row.message = first_existing_file_text({checker_stdout, checker_stderr});
            if (checker.sandbox_setup_failed) {
                row.verdict = Verdict::InternalError;
                row.message = checker.sandbox_error;
            } else if (checker.timed_out) {
                row.verdict = Verdict::InternalError;
                row.message = "checker time limit exceeded";
            } else if (checker.exit_code == 0) {
                row.verdict = Verdict::Accepted;
                row.earned_points = row.max_points;
            } else if (checker.exit_code == 1) {
                row.verdict = Verdict::WrongAnswer;
            } else if (checker.exit_code == 7) {
                double checker_points = 0.0;
                if (!parse_first_number(row.message, checker_points) ||
                    !std::isfinite(checker_points) || !std::isfinite(row.max_points) ||
                    row.max_points < 0.0) {
                    row.verdict = Verdict::InternalError;
                    row.message =
                        "checker returned points but no finite numeric score was found";
                } else {
                    if (problem.checker_point_scale == CheckerPointScale::Percentage) {
                        const double percentage = std::clamp(checker_points, 0.0, 100.0);
                        row.earned_points = row.max_points * (percentage / 100.0);
                        row.verdict = percentage <= 0.0
                                          ? Verdict::WrongAnswer
                                          : (percentage >= 100.0 ? Verdict::Accepted
                                                                 : Verdict::Partial);
                    } else {
                        row.earned_points =
                            std::clamp(checker_points, 0.0, row.max_points);
                        row.verdict = checker_points <= 0.0
                                          ? Verdict::WrongAnswer
                                          : (row.earned_points + 1e-9 >= row.max_points
                                                 ? Verdict::Accepted
                                                 : Verdict::Partial);
                    }
                }
            } else if (checker.memory_exceeded) {
                row.verdict = Verdict::InternalError;
                row.message = "checker memory limit exceeded";
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
    unsigned int physical_cores = 0;
    unsigned int performance_cores = 0;
#ifdef _WIN32
    physical_cores = windows_physical_core_count();
    performance_cores = windows_performance_core_count();
#elif defined(__linux__)
    physical_cores = linux_physical_core_count();
    performance_cores = linux_performance_core_count();
#endif

    unsigned int worker_count = options.parallel_jobs;
    if (worker_count == 0) {
        worker_count = performance_cores;
        if (worker_count == 0) {
            worker_count = physical_cores;
        }
        if (worker_count == 0) {
            worker_count = std::thread::hardware_concurrency();
            if (worker_count > 1) {
                worker_count = (worker_count + 1) / 2;
            }
        }
        if (worker_count == 0) {
            worker_count = 1;
        }
    } else {
        unsigned int cap = physical_cores;
        if (cap == 0) {
            cap = std::thread::hardware_concurrency();
        }
        if (cap > 0) {
            worker_count = std::min(worker_count, cap);
        }
    }
#if defined(__linux__)
    const unsigned int allowed_cpus = linux_allowed_cpu_count();
    if (allowed_cpus > 0) {
        worker_count = std::min(worker_count, allowed_cpus);
    }
#endif
    return std::max(1U, worker_count);
}

fs::path work_root_for_contest(const JudgeOptions& options) {
    static std::atomic<std::uint64_t> invocation_sequence{0};
    std::string contest_key = process_path_argument(fs::absolute(options.contest_root));
    std::size_t hash = std::hash<std::string>{}(contest_key);
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::uint64_t sequence = invocation_sequence.fetch_add(1, std::memory_order_relaxed);
#ifndef _WIN32
    auto process_id = static_cast<unsigned long long>(getpid());
#else
    auto process_id = static_cast<unsigned long long>(GetCurrentProcessId());
#endif
    fs::path root = options.contest_root / ".neothemis-work";
    return root / ("run-" + std::to_string(hash) + "-" +
                   std::to_string(process_id) + "-" + std::to_string(now) + "-" +
                   std::to_string(sequence));
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
        validate_judge_paths(options);
        fs::path contestants_root = options.contest_root / options.contestants_dir;
        fs::path tests_root = options.contest_root / options.tests_dir;
        fs::path work_root = work_root_for_contest(options);

        if (options.execution_security == ExecutionSecurity::Required) {
            std::string reason;
            if (!secure_sandbox_available(&reason)) {
                throw std::runtime_error("secure sandbox unavailable: " + reason);
            }
        }

        if (!fs::exists(contestants_root)) {
            throw std::runtime_error("contestants directory not found: " +
                                     process_path_argument(contestants_root));
        }
        if (!fs::exists(tests_root)) {
            throw std::runtime_error("tests directory not found: " +
                                     process_path_argument(tests_root));
        }

        ScopedWorkdirCleanup workdir_cleanup(work_root, !options.keep_workdir);
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
            problem.name = process_path_argument(problem_entry.path().filename());
            if (!selection_allows(options.selected_problems, problem.name)) {
                continue;
            }
            problem.dir = problem_entry.path();
            problem.tests = sorted_directories(problem.dir);
            const fs::path problem_config = problem.dir / kProblemConfigFilename;
            std::error_code problem_config_error;
            const fs::file_status problem_config_status =
                fs::symlink_status(problem_config, problem_config_error);
            if (!problem_config_error && fs::exists(problem_config_status) &&
                canonical_real_direct_file(problem.dir, problem_config).empty()) {
                throw std::runtime_error("problem settings must be a real file inside " +
                                         process_path_argument(problem.dir));
            }
            if (problem_config_error &&
                problem_config_error != std::errc::no_such_file_or_directory) {
                throw std::runtime_error("failed to inspect problem settings: " +
                                         problem_config_error.message());
            }
            problem.settings = load_or_create_problem_config(problem.dir);
            if (canonical_real_direct_file(problem.dir, problem_config).empty()) {
                throw std::runtime_error("problem settings must be a real file inside " +
                                         process_path_argument(problem.dir));
            }
            problem.checker_point_scale =
                is_testlib_checker_setting(problem.settings.checker)
                    ? CheckerPointScale::Percentage
                    : CheckerPointScale::Absolute;
            try {
                problem.checker_executable = compile_checker(
                    problem.settings, options, problem.dir,
                    work_root / "internal" / "checkers" /
                        fs::u8path(problem.name) / "build");
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
                                                 process_path_argument(
                                                     a.test.path().filename()),
                                                 a.contestant) <
                                 std::make_tuple(b.problem->name,
                                                 process_path_argument(
                                                     b.test.path().filename()),
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
                                            process_path_argument(
                                                job.test.path().filename());
                        {
                            std::lock_guard<std::mutex> lock(progress_mutex);
                            progress_state.worker_labels[worker_id - 1] = label;
                        }

                        TestResult job_result = judge_test_job(job, options);
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

} // namespace

std::string to_string(Verdict verdict) {
    switch (verdict) {
        case Verdict::Accepted: return "AC";
        case Verdict::Partial: return "PC";
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

Verdict verdict_from_string(const std::string& value) {
    if (value == "AC") return Verdict::Accepted;
    if (value == "PC") return Verdict::Partial;
    if (value == "WA") return Verdict::WrongAnswer;
    if (value == "CE") return Verdict::CompileError;
    if (value == "RE") return Verdict::RuntimeError;
    if (value == "TLE") return Verdict::TimeLimitExceeded;
    if (value == "MLE") return Verdict::MemoryLimitExceeded;
    if (value == "MS") return Verdict::MissingSource;
    if (value == "SV") return Verdict::SecurityViolation;
    return Verdict::InternalError;
}

bool secure_sandbox_available(std::string* reason) {
    std::string backend_reason;
    if (!detail::sandbox_backend_configured(backend_reason)) {
        if (reason) {
            *reason = backend_reason;
        }
        return false;
    }

    SandboxRunSpec probe;
    probe.profile = detail::SandboxProfile::Submission;
    probe.guest_working_directory = "/";
    fs::path null_path = null_output_path();
    ProcessResult result = run_program({"/bin/true"}, nullptr, &null_path, &null_path,
                                       5000, 128, 64, 1, {}, &probe);
    if (result.sandbox_setup_failed || result.timed_out || result.exit_code != 0) {
        if (reason) {
            *reason = result.sandbox_error.empty()
                          ? "bubblewrap isolation probe failed"
                          : result.sandbox_error;
        }
        return false;
    }
    if (reason) {
        reason->clear();
    }
    return true;
}

void validate_judge_paths(const JudgeOptions& options) {
    if (options.contest_root.empty()) {
        throw std::runtime_error("contest root is empty");
    }
    std::error_code root_error;
    const fs::path canonical_root = fs::weakly_canonical(options.contest_root, root_error);
    if (root_error || !canonical_root.is_absolute()) {
        throw std::runtime_error("failed to resolve contest root: " +
                                 process_path_argument(options.contest_root));
    }

    auto components_equal = [](const fs::path& left, const fs::path& right) {
#ifdef _WIN32
        std::wstring folded_left = left.native();
        std::wstring folded_right = right.native();
        std::transform(folded_left.begin(), folded_left.end(), folded_left.begin(),
                       [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        std::transform(folded_right.begin(), folded_right.end(), folded_right.begin(),
                       [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        return folded_left == folded_right;
#else
        return left == right;
#endif
    };

    auto require_inside = [&](const fs::path& configured, const char* name) {
        if (configured.empty()) {
            throw std::runtime_error(std::string(name) + " path is empty");
        }
        const fs::path candidate = configured.is_absolute()
                                       ? configured
                                       : canonical_root / configured;
        std::error_code candidate_error;
        const fs::path canonical_candidate = fs::weakly_canonical(candidate, candidate_error);
        if (candidate_error || !canonical_candidate.is_absolute()) {
            throw std::runtime_error(std::string("failed to resolve ") + name + " path: " +
                                     process_path_argument(configured));
        }
        auto root_part = canonical_root.begin();
        auto candidate_part = canonical_candidate.begin();
        for (; root_part != canonical_root.end(); ++root_part, ++candidate_part) {
            if (candidate_part == canonical_candidate.end() ||
                !components_equal(*root_part, *candidate_part)) {
                throw std::runtime_error(std::string(name) +
                                         " path escapes the contest root: " +
                                         process_path_argument(configured));
            }
        }
        if (candidate_part == canonical_candidate.end()) {
            throw std::runtime_error(std::string(name) +
                                     " path must be below the contest root");
        }
        return canonical_candidate;
    };

    const fs::path contestants =
        require_inside(options.contestants_dir, "contestants_dir");
    const fs::path tests = require_inside(options.tests_dir, "tests_dir");
    const fs::path output = require_inside(options.output_csv, "output_csv");
    const fs::path scoreboard =
        require_inside(options.scoreboard_csv, "scoreboard_csv");
    const fs::path configured_work = canonical_root / ".neothemis-work";
    std::error_code work_status_error;
    const fs::file_status work_status =
        fs::symlink_status(configured_work, work_status_error);
    if (work_status_error &&
        work_status_error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error("failed to inspect judge work directory: " +
                                 work_status_error.message());
    }
    if (!work_status_error && fs::exists(work_status) &&
        (fs::is_symlink(work_status) || !fs::is_directory(work_status))) {
        throw std::runtime_error(
            "judge work path must be a real directory inside the contest");
    }
    const fs::path work = require_inside(".neothemis-work", "judge work");

    auto same_or_below = [&](const fs::path& candidate, const fs::path& protected_path) {
        auto protected_part = protected_path.begin();
        auto candidate_part = candidate.begin();
        for (; protected_part != protected_path.end();
             ++protected_part, ++candidate_part) {
            if (candidate_part == candidate.end() ||
                !components_equal(*candidate_part, *protected_part)) {
                return false;
            }
        }
        return true;
    };
    auto paths_equal = [&](const fs::path& left, const fs::path& right) {
        return same_or_below(left, right) && same_or_below(right, left);
    };
    auto reject_unsafe_output = [&](const fs::path& path, const char* name) {
#ifdef _WIN32
        std::wstring filename = path.filename().native();
        std::transform(filename.begin(), filename.end(), filename.begin(),
                       [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        const std::wstring lock_suffix = L".neothemis-lock";
#else
        const std::string filename = path.filename().string();
        const std::string lock_suffix = ".neothemis-lock";
#endif
        if (filename.size() >= lock_suffix.size() &&
            filename.compare(filename.size() - lock_suffix.size(), lock_suffix.size(),
                             lock_suffix) == 0) {
            throw std::runtime_error(std::string(name) +
                                     " cannot use a NeoThemis lock filename");
        }
        const std::vector<fs::path> protected_trees{
            contestants,
            tests,
            work,
            fs::weakly_canonical(canonical_root / ".neothemis-server")};
        for (const fs::path& protected_tree : protected_trees) {
            if (same_or_below(path, protected_tree)) {
                throw std::runtime_error(std::string(name) +
                                         " overlaps protected contest state");
            }
        }
        if (paths_equal(path,
                        fs::weakly_canonical(canonical_root / kContestConfigFilename))) {
            throw std::runtime_error(std::string(name) +
                                     " cannot overwrite the contest config");
        }
    };
    reject_unsafe_output(output, "output_csv");
    reject_unsafe_output(scoreboard, "scoreboard_csv");
    if (paths_equal(output, scoreboard)) {
        throw std::runtime_error("output_csv and scoreboard_csv must be different files");
    }
}

std::unique_ptr<JudgeCore> make_judge_core(const std::string& name) {
    if (name == "builtin") {
        return std::make_unique<BuiltinJudgeCore>();
    }
    throw std::runtime_error("unknown judge core: " + name);
}

ContestOverview inspect_contest(const JudgeOptions& options) {
    validate_judge_paths(options);
    auto lower = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    };
    auto list_dirs = [](const fs::path& path) {
        std::vector<fs::path> dirs;
        for (const auto& entry : sorted_directories(path)) {
            dirs.push_back(entry.path());
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
        std::string problem = process_path_argument(problem_dir.filename());
        if (!allowed(options.selected_problems, problem)) {
            continue;
        }
        overview.problems.push_back(problem);
        problem_keys.push_back(lower(problem));
    }

    for (const auto& contestant_dir : list_dirs(options.contest_root / options.contestants_dir)) {
        std::string contestant = process_path_argument(contestant_dir.filename());
        if (!allowed(options.selected_contestants, contestant)) {
            continue;
        }

        std::set<std::string> source_stems;
        for (const fs::path& source : sorted_sources(contestant_dir)) {
            source_stems.insert(lower(process_path_argument(source.stem())));
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
        output << escape_csv_field(row.contestant) << ','
               << escape_csv_field(row.problem) << ','
               << escape_csv_field(row.test) << ','
               << escape_csv_field(to_string(row.verdict)) << ','
               << row.time_ms << ','
               << row.exit_code << ','
               << format_points(row.max_points) << ','
               << format_points(row.earned_points) << ','
               << escape_csv_field(row.message) << '\n';
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
        output << ',' << escape_csv_field(problem);
    }
    output << ",total\n";

    for (const auto& contestant : contestants) {
        double total = 0.0;
        output << escape_csv_field(contestant);
        for (const auto& problem : problems) {
            double score = scores[contestant][problem];
            total += score;
            const std::string& status = statuses[contestant][problem];
            if (score == 0.0 && !status.empty()) {
                output << ',' << escape_csv_field(status + "(0)");
            } else {
                output << ',' << format_points(score);
            }
        }
        output << ',' << format_points(total) << '\n';
    }
}

void write_scoreboard_csv_from_results(std::ostream& output, const CsvTable& rows) {
    std::vector<TestResult> results;
    results.reserve(rows.size() > 0 ? rows.size() - 1 : 0);
    for (std::size_t index = 1; index < rows.size(); ++index) {
        const CsvRow& fields = rows[index];
        if (fields.size() < 9) {
            continue;
        }
        try {
            TestResult result;
            result.contestant = fields[0];
            result.problem = fields[1];
            result.test = fields[2];
            result.verdict = verdict_from_string(fields[3]);
            result.time_ms = static_cast<std::uint64_t>(std::stoull(fields[4]));
            result.exit_code = std::stoi(fields[5]);
            result.max_points = std::stod(fields[6]);
            result.earned_points = std::stod(fields[7]);
            result.message = fields[8];
            results.push_back(std::move(result));
        } catch (const std::exception&) {
        }
    }
    write_scoreboard_csv(output, results);
}

} // namespace neothemis
