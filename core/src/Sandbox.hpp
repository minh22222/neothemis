#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace neothemis::detail {

enum class SandboxProfile {
    Compiler,
    Submission,
    Checker
};

struct SandboxMount {
    std::filesystem::path host_path;
    std::string guest_path;
    bool read_only = true;
};

struct SandboxTmpfs {
    std::string guest_path;
    std::uint64_t size_bytes = 0;
};

struct SandboxLaunch {
    std::vector<std::string> arguments;
    int filter_fd = -1;
    int status_read_fd = -1;
    int status_write_fd = -1;
    std::string status_buffer;
    std::int64_t child_pid = -1;
    bool status_eof = false;
};

bool sandbox_backend_configured(std::string& reason);

SandboxLaunch prepare_sandbox_launch(SandboxProfile profile,
                                     const std::vector<SandboxMount>& mounts,
                                     const std::vector<SandboxTmpfs>& temporary_filesystems,
                                     const std::string& guest_working_directory,
                                     const std::vector<std::string>& target_arguments);

void close_sandbox_parent_fds_after_fork(SandboxLaunch& launch);
void close_sandbox_child_fds_before_exec(SandboxLaunch& launch);
std::int64_t sandbox_child_pid(SandboxLaunch& launch);
bool sandbox_reported_child_start(SandboxLaunch& launch);
void close_sandbox_launch(SandboxLaunch& launch);

} // namespace neothemis::detail
