#include "neothemis/Csv.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <system_error>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace neothemis {
namespace {

namespace fs = std::filesystem;

fs::path lock_path_for(const fs::path& target) {
    return target.parent_path() /
           (target.filename().string() + ".neothemis-lock");
}

class InterprocessFileLock {
public:
    explicit InterprocessFileLock(const fs::path& target) {
        const fs::path lock_path = lock_path_for(target);
        if (!lock_path.parent_path().empty()) {
            fs::create_directories(lock_path.parent_path());
        }
#ifndef _WIN32
        fd_ = open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd_ < 0) {
            throw std::runtime_error("failed to open CSV lock " + lock_path.string() + ": " +
                                     std::strerror(errno));
        }
        while (flock(fd_, LOCK_EX) != 0) {
            if (errno == EINTR) {
                continue;
            }
            const int saved_errno = errno;
            close(fd_);
            fd_ = -1;
            throw std::runtime_error("failed to lock CSV file " + target.string() + ": " +
                                     std::strerror(saved_errno));
        }
#else
        handle_ = CreateFileW(lock_path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("failed to open CSV lock " + lock_path.string());
        }
        OVERLAPPED overlapped{};
        if (!LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &overlapped)) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error("failed to lock CSV file " + target.string());
        }
#endif
    }

    ~InterprocessFileLock() {
#ifndef _WIN32
        if (fd_ >= 0) {
            (void)flock(fd_, LOCK_UN);
            close(fd_);
        }
#else
        if (handle_ != INVALID_HANDLE_VALUE) {
            OVERLAPPED overlapped{};
            (void)UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped);
            CloseHandle(handle_);
        }
#endif
    }

    InterprocessFileLock(const InterprocessFileLock&) = delete;
    InterprocessFileLock& operator=(const InterprocessFileLock&) = delete;

private:
#ifndef _WIN32
    int fd_ = -1;
#else
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#endif
};

std::string encode_csv(const CsvTable& rows) {
    std::ostringstream output;
    for (const CsvRow& row : rows) {
        write_csv_row(output, row);
    }
    return output.str();
}

void write_text_file_atomic_unlocked(const fs::path& path, std::string_view contents) {
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path());
    }
    static std::atomic<std::uint64_t> sequence{0};
#ifndef _WIN32
    const auto process_id = static_cast<unsigned long long>(getpid());
#else
    const auto process_id = static_cast<unsigned long long>(GetCurrentProcessId());
#endif
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path temporary =
        path.parent_path() /
        ("." + path.filename().string() + ".tmp-" + std::to_string(process_id) + "-" +
         std::to_string(timestamp) + "-" +
         std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));

    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("failed to open atomic CSV temporary file: " +
                                     temporary.string());
        }
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.flush();
        if (!output) {
            throw std::runtime_error("failed to write atomic CSV temporary file: " +
                                     temporary.string());
        }
        output.close();
        if (!output) {
            throw std::runtime_error("failed to close atomic CSV temporary file: " +
                                     temporary.string());
        }

#ifndef _WIN32
        const int temporary_fd = open(temporary.c_str(), O_RDONLY | O_CLOEXEC);
        if (temporary_fd < 0 || fsync(temporary_fd) != 0) {
            const int saved_errno = errno;
            if (temporary_fd >= 0) {
                close(temporary_fd);
            }
            throw std::runtime_error("failed to flush atomic CSV temporary file: " +
                                     std::string(std::strerror(saved_errno)));
        }
        close(temporary_fd);
        if (rename(temporary.c_str(), path.c_str()) != 0) {
            throw std::runtime_error("failed to replace CSV file " + path.string() + ": " +
                                     std::strerror(errno));
        }
        const fs::path parent = path.parent_path().empty() ? fs::path(".") : path.parent_path();
        const int parent_fd = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent_fd >= 0) {
            (void)fsync(parent_fd);
            close(parent_fd);
        }
#else
        HANDLE temporary_handle = CreateFileW(
            temporary.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (temporary_handle == INVALID_HANDLE_VALUE ||
            !FlushFileBuffers(temporary_handle)) {
            if (temporary_handle != INVALID_HANDLE_VALUE) {
                CloseHandle(temporary_handle);
            }
            throw std::runtime_error("failed to flush atomic CSV temporary file: " +
                                     temporary.string());
        }
        CloseHandle(temporary_handle);
        if (!MoveFileExW(temporary.wstring().c_str(), path.wstring().c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("failed to replace CSV file " + path.string());
        }
#endif
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

} // namespace

CsvTable parse_csv(std::string_view text) {
    CsvTable rows;
    CsvRow row;
    std::string cell;
    bool quoted = false;
    bool have_data = false;

    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        have_data = true;
        if (quoted) {
            if (ch == '"' && i + 1 < text.size() && text[i + 1] == '"') {
                cell.push_back('"');
                ++i;
            } else if (ch == '"') {
                quoted = false;
            } else {
                cell.push_back(ch);
            }
            continue;
        }

        if (ch == '"') {
            quoted = true;
        } else if (ch == ',') {
            row.push_back(cell);
            cell.clear();
        } else if (ch == '\n') {
            row.push_back(cell);
            cell.clear();
            rows.push_back(row);
            row.clear();
            have_data = false;
        } else if (ch != '\r') {
            cell.push_back(ch);
        }
    }

    if (have_data || !cell.empty() || !row.empty()) {
        row.push_back(cell);
        rows.push_back(row);
    }
    return rows;
}

CsvTable read_csv_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to read CSV file: " + path.string());
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return parse_csv(contents.str());
}

CsvTable read_csv_file_locked(const std::filesystem::path& path) {
    InterprocessFileLock lock(path);
    return read_csv_file(path);
}

std::string escape_csv_field(std::string_view value) {
    const bool needs_quotes = value.find_first_of(",\"\n\r") != std::string_view::npos;
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        if (ch == '"') {
            escaped += "\"\"";
        } else if (ch != '\r') {
            escaped.push_back(ch);
        }
    }
    return needs_quotes ? '"' + escaped + '"' : escaped;
}

void write_csv_row(std::ostream& output, const CsvRow& row) {
    for (std::size_t index = 0; index < row.size(); ++index) {
        if (index > 0) {
            output.put(',');
        }
        output << escape_csv_field(row[index]);
    }
    output.put('\n');
}

void write_text_file_atomic(const std::filesystem::path& path, std::string_view contents) {
    InterprocessFileLock lock(path);
    write_text_file_atomic_unlocked(path, contents);
}

CsvTable replace_csv_rows_by_key_atomic(const std::filesystem::path& path,
                                        const CsvRow& header,
                                        const CsvTable& replacement_rows,
                                        const std::set<CsvKey>& replaced_keys,
                                        const CsvLockedMergeCallback& while_locked) {
    InterprocessFileLock lock(path);
    CsvTable merged;
    merged.push_back(header);
    std::error_code exists_error;
    if (fs::exists(path, exists_error) && !exists_error) {
        const CsvTable existing = read_csv_file(path);
        for (std::size_t index = 0; index < existing.size(); ++index) {
            const CsvRow& row = existing[index];
            const bool header_row =
                index == 0 && row.size() >= 2 && header.size() >= 2 &&
                row[0] == header[0] && row[1] == header[1];
            if (header_row) {
                continue;
            }
            if (row.size() >= 2 && replaced_keys.count({row[0], row[1]}) != 0) {
                continue;
            }
            if (!row.empty()) {
                merged.push_back(row);
            }
        }
    } else if (exists_error) {
        throw std::runtime_error("failed to inspect CSV file: " + path.string());
    }
    merged.insert(merged.end(), replacement_rows.begin(), replacement_rows.end());
    write_text_file_atomic_unlocked(path, encode_csv(merged));
    if (while_locked) {
        while_locked(merged);
    }
    return merged;
}

} // namespace neothemis
