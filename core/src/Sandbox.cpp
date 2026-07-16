#include "Sandbox.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <poll.h>
#include <unistd.h>
#endif

#if defined(__linux__) && defined(NEOTHEMIS_HAS_SECCOMP)
#include <fcntl.h>
#include <linux/memfd.h>
#include <sched.h>
#include <seccomp.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#endif

namespace fs = std::filesystem;

namespace neothemis::detail {
namespace {

#if defined(__linux__) && defined(NEOTHEMIS_HAS_SECCOMP)

fs::path trusted_root_executable(std::initializer_list<fs::path> candidates) {
    for (const fs::path& candidate : candidates) {
        std::error_code ec;
        if (!fs::is_regular_file(candidate, ec) || ec) {
            continue;
        }
        struct stat metadata {};
        if (stat(candidate.c_str(), &metadata) != 0 || metadata.st_uid != 0) {
            continue;
        }
        const fs::perms permissions = fs::status(candidate, ec).permissions();
        if (ec || (permissions & (fs::perms::group_write | fs::perms::others_write)) !=
                      fs::perms::none) {
            continue;
        }
        return candidate;
    }
    return {};
}

fs::path bubblewrap_path() {
    return trusted_root_executable({"/usr/bin/bwrap", "/bin/bwrap"});
}

fs::path prlimit_path() {
    return trusted_root_executable({"/usr/bin/prlimit", "/bin/prlimit"});
}

void add_denied_syscall(scmp_filter_ctx context, const char* name) {
    const int syscall_number = seccomp_syscall_resolve_name(name);
    if (syscall_number == __NR_SCMP_ERROR) {
        return;
    }
    const int result = seccomp_rule_add(context, SCMP_ACT_ERRNO(EPERM), syscall_number, 0);
    if (result < 0 && result != -EEXIST) {
        throw std::runtime_error(std::string("failed to add seccomp rule for ") + name +
                                 ": " + std::strerror(-result));
    }
}

int move_fd_above_standard_streams(int fd) {
    if (fd < 0 || fd >= 3) {
        return fd;
    }
    const int moved = fcntl(fd, F_DUPFD, 3);
    if (moved < 0) {
        const int saved_errno = errno;
        close(fd);
        throw std::runtime_error(std::string("failed to normalize sandbox descriptor: ") +
                                 std::strerror(saved_errno));
    }
    close(fd);
    return moved;
}

void restrict_clone_to_threads(scmp_filter_ctx context) {
    const int clone_number = seccomp_syscall_resolve_name("clone");
    if (clone_number == __NR_SCMP_ERROR) {
        return;
    }

    // A real pthread-style clone must share the address space, filesystem
    // context, descriptor table, signal handlers, and thread group. Denying
    // each missing bit permits threads without permitting a child process.
    constexpr std::uint64_t required_thread_flags[] = {
        CLONE_VM, CLONE_FS, CLONE_FILES, CLONE_SIGHAND, CLONE_THREAD
    };
    for (std::uint64_t flag : required_thread_flags) {
        const int result = seccomp_rule_add(
            context, SCMP_ACT_ERRNO(EPERM), clone_number, 1,
            SCMP_A0(SCMP_CMP_MASKED_EQ, flag, 0));
        if (result < 0 && result != -EEXIST) {
            throw std::runtime_error(std::string("failed to restrict clone: ") +
                                     std::strerror(-result));
        }
    }
}

void make_clone3_fall_back_to_clone(scmp_filter_ctx context) {
    const int clone3_number = seccomp_syscall_resolve_name("clone3");
    if (clone3_number == __NR_SCMP_ERROR) {
        return;
    }
    const int result = seccomp_rule_add(
        context, SCMP_ACT_ERRNO(ENOSYS), clone3_number, 0);
    if (result < 0 && result != -EEXIST) {
        throw std::runtime_error(std::string("failed to restrict clone3: ") +
                                 std::strerror(-result));
    }
}

std::vector<unsigned char> compile_seccomp_filter(SandboxProfile profile) {
    scmp_filter_ctx context = seccomp_init(SCMP_ACT_ALLOW);
    if (!context) {
        throw std::runtime_error("failed to initialize seccomp filter");
    }

    int fd = -1;
    try {
        const char* common_denied[] = {
            "socket", "socketpair", "connect", "bind", "listen", "accept", "accept4",
            "sendto", "sendmsg", "sendmmsg", "recvfrom", "recvmsg", "recvmmsg",
            "shutdown", "getsockname", "getpeername", "setsockopt", "getsockopt",
            "ptrace", "process_vm_readv", "process_vm_writev", "kcmp",
            "pidfd_open", "pidfd_getfd", "pidfd_send_signal",
            "mount", "umount", "umount2", "pivot_root", "chroot", "setns", "unshare",
            "fsopen", "fsconfig", "fsmount", "fspick", "open_tree", "move_mount",
            "mount_setattr",
            "bpf", "perf_event_open", "userfaultfd", "io_uring_setup", "io_uring_enter",
            "io_uring_register", "keyctl", "add_key", "request_key", "open_by_handle_at",
            "name_to_handle_at", "init_module", "finit_module", "delete_module", "kexec_load",
            "kexec_file_load", "reboot", "swapon", "swapoff", "acct", "quotactl",
            "fanotify_init", "fanotify_mark", "ioperm", "iopl", "syslog",
            "sched_setaffinity", "sync", "syncfs", "sync_file_range",
            "inotify_init", "inotify_init1", "inotify_add_watch", "inotify_rm_watch"
        };
        for (const char* name : common_denied) {
            add_denied_syscall(context, name);
        }

        if (profile != SandboxProfile::Compiler) {
            for (const char* name : {"fork", "vfork"}) {
                add_denied_syscall(context, name);
            }
            for (const char* name : {"memfd_create", "fallocate", "shmget", "shmat",
                                     "shmdt", "shmctl", "msgget", "msgsnd", "msgrcv",
                                     "msgctl", "semget", "semop", "semtimedop", "semctl",
                                     "ipc"}) {
                add_denied_syscall(context, name);
            }
            make_clone3_fall_back_to_clone(context);
            restrict_clone_to_threads(context);
        }

        fd = static_cast<int>(syscall(SYS_memfd_create, "neothemis-seccomp-build", 0));
        if (fd < 0) {
            throw std::runtime_error(std::string("failed to create seccomp memory file: ") +
                                     std::strerror(errno));
        }
        const int export_result = seccomp_export_bpf(context, fd);
        if (export_result < 0) {
            throw std::runtime_error(std::string("failed to export seccomp filter: ") +
                                     std::strerror(-export_result));
        }

        constexpr off_t maximum_filter_bytes = 1024 * 1024;
        const off_t filter_size = lseek(fd, 0, SEEK_END);
        if (filter_size < 0) {
            const int saved_errno = errno;
            throw std::runtime_error(std::string("failed to size seccomp filter: ") +
                                     std::strerror(saved_errno));
        }
        if (filter_size == 0 || filter_size > maximum_filter_bytes) {
            throw std::runtime_error("exported seccomp filter has an invalid size");
        }
        if (lseek(fd, 0, SEEK_SET) < 0) {
            const int saved_errno = errno;
            throw std::runtime_error(std::string("failed to rewind compiled seccomp filter: ") +
                                     std::strerror(saved_errno));
        }

        std::vector<unsigned char> bytes(static_cast<std::size_t>(filter_size));
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t count = read(fd, bytes.data() + offset, bytes.size() - offset);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                const int saved_errno = count < 0 ? errno : EIO;
                throw std::runtime_error(std::string("failed to read seccomp filter: ") +
                                         std::strerror(saved_errno));
            }
            offset += static_cast<std::size_t>(count);
        }

        close(fd);
        fd = -1;
        seccomp_release(context);
        return bytes;
    } catch (...) {
        if (fd >= 0) {
            close(fd);
        }
        seccomp_release(context);
        throw;
    }
}

const std::vector<unsigned char>& cached_seccomp_filter(SandboxProfile profile) {
    switch (profile) {
        case SandboxProfile::Compiler: {
            static const std::vector<unsigned char> filter =
                compile_seccomp_filter(SandboxProfile::Compiler);
            return filter;
        }
        case SandboxProfile::Submission: {
            static const std::vector<unsigned char> filter =
                compile_seccomp_filter(SandboxProfile::Submission);
            return filter;
        }
        case SandboxProfile::Checker: {
            static const std::vector<unsigned char> filter =
                compile_seccomp_filter(SandboxProfile::Checker);
            return filter;
        }
    }
    throw std::runtime_error("unknown sandbox profile");
}

int create_seccomp_filter(SandboxProfile profile) {
    const std::vector<unsigned char>& bytes = cached_seccomp_filter(profile);
    int fd = static_cast<int>(syscall(SYS_memfd_create, "neothemis-seccomp", 0));
    if (fd < 0) {
        throw std::runtime_error(std::string("failed to create seccomp memory file: ") +
                                 std::strerror(errno));
    }

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            const int saved_errno = count < 0 ? errno : EIO;
            close(fd);
            throw std::runtime_error(std::string("failed to write seccomp filter: ") +
                                     std::strerror(saved_errno));
        }
        offset += static_cast<std::size_t>(count);
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        const int saved_errno = errno;
        close(fd);
        throw std::runtime_error(std::string("failed to rewind seccomp filter: ") +
                                 std::strerror(saved_errno));
    }
    return move_fd_above_standard_streams(fd);
}

bool safe_guest_path(const std::string& value) {
    if (value.empty() || value.front() != '/') {
        return false;
    }
    const fs::path normalized = fs::path(value).lexically_normal();
    if (normalized.empty() || normalized.string() != value) {
        return false;
    }
    return std::none_of(normalized.begin(), normalized.end(), [](const fs::path& part) {
        return part == "..";
    });
}

void add_guest_parent_directories(std::vector<std::string>& arguments,
                                  const std::vector<SandboxMount>& mounts,
                                  const std::vector<SandboxTmpfs>& temporary_filesystems) {
    std::set<std::string> directories;
    for (const SandboxMount& mount : mounts) {
        fs::path parent = fs::path(mount.guest_path).parent_path();
        while (!parent.empty() && parent != "/") {
            directories.insert(parent.string());
            parent = parent.parent_path();
        }
    }
    for (const SandboxTmpfs& temporary : temporary_filesystems) {
        fs::path directory = temporary.guest_path;
        while (!directory.empty() && directory != "/") {
            directories.insert(directory.string());
            directory = directory.parent_path();
        }
    }
    std::vector<std::string> ordered(directories.begin(), directories.end());
    std::sort(ordered.begin(), ordered.end(), [](const std::string& left,
                                                 const std::string& right) {
        const auto left_depth = std::count(left.begin(), left.end(), '/');
        const auto right_depth = std::count(right.begin(), right.end(), '/');
        return left_depth == right_depth ? left < right : left_depth < right_depth;
    });
    for (const std::string& directory : ordered) {
        arguments.push_back("--dir");
        arguments.push_back(directory);
    }
}

void add_host_tree(std::vector<std::string>& arguments, const fs::path& path) {
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(path, ec)) && !ec) {
        arguments.push_back("--symlink");
        arguments.push_back(fs::read_symlink(path, ec).string());
        if (ec) {
            throw std::runtime_error("failed to read system symlink " + path.string());
        }
        arguments.push_back(path.string());
        return;
    }
    if (fs::exists(path, ec) && !ec) {
        arguments.push_back("--ro-bind");
        arguments.push_back(path.string());
        arguments.push_back(path.string());
    }
}

#endif

void close_fd(int& fd) {
#ifndef _WIN32
    if (fd >= 0) {
        close(fd);
    }
#endif
    fd = -1;
}

#ifndef _WIN32
void parse_sandbox_child_pid(SandboxLaunch& launch) {
    if (launch.child_pid > 0) {
        return;
    }
    constexpr const char* key = "\"child-pid\"";
    const std::size_t key_position = launch.status_buffer.find(key);
    if (key_position == std::string::npos) {
        return;
    }
    const std::size_t separator = launch.status_buffer.find(':', key_position);
    if (separator == std::string::npos) {
        return;
    }
    const std::size_t first = launch.status_buffer.find_first_of("0123456789", separator + 1);
    if (first == std::string::npos) {
        return;
    }
    const std::size_t last = launch.status_buffer.find_first_not_of("0123456789", first);
    // The status pipe is incremental. Do not permanently accept a PID when a
    // read happened to stop in the middle of its decimal representation.
    if (last == std::string::npos) {
        return;
    }
    try {
        const unsigned long long value =
            std::stoull(launch.status_buffer.substr(first, last - first));
        if (value > 0 &&
            value <= static_cast<unsigned long long>(
                         std::numeric_limits<std::int64_t>::max())) {
            launch.child_pid = static_cast<std::int64_t>(value);
        }
    } catch (const std::exception&) {
    }
}

void read_sandbox_status(SandboxLaunch& launch, bool until_eof) {
    constexpr std::size_t maximum_status_bytes = 64 * 1024;
    while (launch.status_read_fd >= 0 && !launch.status_eof) {
        if (!until_eof) {
            pollfd descriptor{};
            descriptor.fd = launch.status_read_fd;
            descriptor.events = POLLIN | POLLHUP;
            int ready = -1;
            do {
                ready = poll(&descriptor, 1, 0);
            } while (ready < 0 && errno == EINTR);
            if (ready <= 0 || (descriptor.revents & (POLLIN | POLLHUP)) == 0) {
                return;
            }
        }

        char buffer[1024];
        const ssize_t count = read(launch.status_read_fd, buffer, sizeof(buffer));
        if (count > 0) {
            const std::size_t incoming = static_cast<std::size_t>(count);
            if (launch.status_buffer.size() < maximum_status_bytes) {
                launch.status_buffer.append(
                    buffer, std::min(incoming,
                                     maximum_status_bytes - launch.status_buffer.size()));
                parse_sandbox_child_pid(launch);
            }
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && !until_eof) {
            return;
        }
        launch.status_eof = true;
        close_fd(launch.status_read_fd);
    }
}
#endif

} // namespace

bool sandbox_backend_configured(std::string& reason) {
#if defined(__linux__) && defined(NEOTHEMIS_HAS_SECCOMP)
    if (bubblewrap_path().empty()) {
        reason = "trusted bubblewrap executable not found at /usr/bin/bwrap or /bin/bwrap";
        return false;
    }
    if (prlimit_path().empty()) {
        reason = "trusted prlimit executable not found at /usr/bin/prlimit or /bin/prlimit";
        return false;
    }
    reason.clear();
    return true;
#elif defined(__linux__)
    reason = "NeoThemis was built without libseccomp support";
    return false;
#else
    reason = "secure execution isolation is not implemented on this platform";
    return false;
#endif
}

SandboxLaunch prepare_sandbox_launch(SandboxProfile profile,
                                     const std::vector<SandboxMount>& mounts,
                                     const std::vector<SandboxTmpfs>& temporary_filesystems,
                                     const std::string& guest_working_directory,
                                     const std::vector<std::string>& target_arguments) {
#if defined(__linux__) && defined(NEOTHEMIS_HAS_SECCOMP)
    std::string reason;
    if (!sandbox_backend_configured(reason)) {
        throw std::runtime_error("secure sandbox unavailable: " + reason);
    }
    if (!safe_guest_path(guest_working_directory) || target_arguments.empty()) {
        throw std::runtime_error("invalid sandbox launch specification");
    }
    for (const SandboxMount& mount : mounts) {
        std::error_code ec;
        if (!safe_guest_path(mount.guest_path) ||
            !fs::exists(mount.host_path, ec) || ec) {
            throw std::runtime_error("invalid sandbox mount: " + mount.host_path.string() +
                                     " -> " + mount.guest_path);
        }
    }
    for (const SandboxTmpfs& temporary : temporary_filesystems) {
        if (!safe_guest_path(temporary.guest_path) || temporary.size_bytes == 0) {
            throw std::runtime_error("invalid sandbox temporary filesystem");
        }
    }

    SandboxLaunch launch;
    launch.filter_fd = create_seccomp_filter(profile);
    int status_pipe[2] = {-1, -1};
    if (pipe(status_pipe) != 0) {
        close_fd(launch.filter_fd);
        throw std::runtime_error(std::string("failed to create sandbox status pipe: ") +
                                 std::strerror(errno));
    }
    try {
        launch.status_read_fd = move_fd_above_standard_streams(status_pipe[0]);
        launch.status_write_fd = move_fd_above_standard_streams(status_pipe[1]);
    } catch (...) {
        close_fd(launch.filter_fd);
        close_fd(status_pipe[0]);
        close_fd(status_pipe[1]);
        close_fd(launch.status_read_fd);
        throw;
    }

    auto& arguments = launch.arguments;
    arguments = {
        bubblewrap_path().string(),
        "--unshare-user", "--unshare-all", "--die-with-parent", "--new-session",
        "--disable-userns", "--uid", "65534", "--gid", "65534",
        "--hostname", "neothemis", "--cap-drop", "ALL", "--clearenv",
        "--setenv", "PATH", "/usr/bin:/bin",
        "--setenv", "LANG", "C",
        "--setenv", "LC_ALL", "C",
        "--setenv", "HOME", "/nonexistent",
        "--setenv", "TMPDIR", "/tmp",
        "--ro-bind", "/usr", "/usr"
    };
    add_host_tree(arguments, "/bin");
    add_host_tree(arguments, "/lib");
    add_host_tree(arguments, "/lib64");
    arguments.insert(arguments.end(), {"--dir", "/etc"});
    for (const fs::path& loader_file : {fs::path("/etc/ld.so.cache"),
                                       fs::path("/etc/ld.so.conf")}) {
        std::error_code ec;
        if (fs::is_regular_file(loader_file, ec) && !ec) {
            arguments.insert(arguments.end(),
                             {"--ro-bind", loader_file.string(), loader_file.string()});
        }
    }
    arguments.insert(arguments.end(), {
        "--proc", "/proc", "--dev", "/dev",
        "--size", "268435456", "--tmpfs", "/tmp"
    });
    add_guest_parent_directories(arguments, mounts, temporary_filesystems);
    for (const SandboxTmpfs& temporary : temporary_filesystems) {
        arguments.insert(arguments.end(),
                         {"--size", std::to_string(temporary.size_bytes),
                          "--tmpfs", temporary.guest_path});
    }
    for (const SandboxMount& mount : mounts) {
        arguments.push_back(mount.read_only ? "--ro-bind" : "--bind");
        arguments.push_back(fs::absolute(mount.host_path).string());
        arguments.push_back(mount.guest_path);
    }
    arguments.insert(arguments.end(), {
        "--remount-ro", "/", "--remount-ro", "/dev",
        "--chdir", guest_working_directory,
        "--seccomp", std::to_string(launch.filter_fd),
        "--json-status-fd", std::to_string(launch.status_write_fd),
        "--"
    });
    // Apply this after bubblewrap has entered its user namespace. Applying
    // RLIMIT_NPROC to the outer judge process would count unrelated host
    // processes belonging to the same account. Compilers need a larger budget
    // for cc1/assembler children; submissions and checkers retain enough room
    // for normal pthread use. The hard limit cannot be raised by the target.
    const char* task_limit = profile == SandboxProfile::Compiler
                                 ? "--nproc=128:128"
                                 : "--nproc=64:64";
    arguments.insert(arguments.end(), {prlimit_path().string(), task_limit, "--"});
    arguments.insert(arguments.end(), target_arguments.begin(), target_arguments.end());
    return launch;
#else
    (void)profile;
    (void)mounts;
    (void)temporary_filesystems;
    (void)guest_working_directory;
    (void)target_arguments;
    throw std::runtime_error("secure sandbox unavailable: secure execution isolation is not implemented in this build");
#endif
}

void close_sandbox_parent_fds_after_fork(SandboxLaunch& launch) {
    close_fd(launch.filter_fd);
    close_fd(launch.status_write_fd);
}

void close_sandbox_child_fds_before_exec(SandboxLaunch& launch) {
    close_fd(launch.status_read_fd);
}

std::int64_t sandbox_child_pid(SandboxLaunch& launch) {
#ifndef _WIN32
    read_sandbox_status(launch, false);
    return launch.child_pid;
#else
    (void)launch;
    return -1;
#endif
}

bool sandbox_reported_child_start(SandboxLaunch& launch) {
#ifndef _WIN32
    read_sandbox_status(launch, true);
    return launch.child_pid > 0;
#else
    (void)launch;
    return false;
#endif
}

void close_sandbox_launch(SandboxLaunch& launch) {
    close_fd(launch.filter_fd);
    close_fd(launch.status_read_fd);
    close_fd(launch.status_write_fd);
}

} // namespace neothemis::detail
