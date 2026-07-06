#include "neothemis/ContestArchive.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <filesystem>
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
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef NEOTHEMIS_HAS_ZLIB
#include <zlib.h>
#endif

namespace fs = std::filesystem;

namespace neothemis {
namespace {

struct ZipEntry {
    std::string name;
    std::uint32_t crc = 0;
    std::uint32_t offset = 0;
    std::uint16_t method = 0;
    std::uint32_t compressed_size = 0;
    std::uint32_t uncompressed_size = 0;
};

struct PendingZipEntry {
    fs::path path;
    std::string name;
    bool directory = false;
};

struct PreparedZipEntry {
    ZipEntry entry;
    std::string content;
};

struct ZipExtractionEntry {
    fs::path output_path;
    std::size_t data_offset = 0;
    std::uint16_t method = 0;
    std::uint32_t crc = 0;
    std::uint32_t compressed_size = 0;
    std::uint32_t uncompressed_size = 0;
};

struct ScopedDirectory {
    fs::path path;

    explicit ScopedDirectory(fs::path value) : path(std::move(value)) {}
    ScopedDirectory(const ScopedDirectory&) = delete;
    ScopedDirectory& operator=(const ScopedDirectory&) = delete;

    ~ScopedDirectory() {
        if (!path.empty()) {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    }
};

std::string trim(const std::string& value) {
    std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string normalized_number(std::string value) {
    std::size_t first_non_zero = value.find_first_not_of('0');
    if (first_non_zero == std::string::npos) {
        return "0";
    }
    return value.substr(first_non_zero);
}

std::string point_key_for_test_name(const std::string& test_name) {
    std::string digits;
    for (auto it = test_name.rbegin(); it != test_name.rend(); ++it) {
        if (!std::isdigit(static_cast<unsigned char>(*it))) {
            break;
        }
        digits.push_back(*it);
    }
    if (digits.empty()) {
        return test_name;
    }
    std::reverse(digits.begin(), digits.end());
    return normalized_number(digits);
}

bool points_equal(double a, double b) {
    return std::abs(a - b) < 0.0000001;
}

std::string format_number(double value) {
    std::ostringstream out;
    out << std::setprecision(12) << value;
    std::string result = out.str();
    if (result.find('.') != std::string::npos) {
        while (!result.empty() && result.back() == '0') {
            result.pop_back();
        }
        if (!result.empty() && result.back() == '.') {
            result.pop_back();
        }
    }
    return result.empty() ? "0" : result;
}

void report_progress(const ArchiveProgress& progress,
                     std::uint64_t done,
                     std::uint64_t total,
                     const std::string& label) {
    if (progress) {
        progress(done, total, label);
    }
}

unsigned int archive_worker_count(std::size_t task_count) {
    if (task_count == 0) {
        return 0;
    }
    unsigned int detected = std::thread::hardware_concurrency();
    if (detected == 0) {
        detected = 1;
    }
    return static_cast<unsigned int>(
        std::min<std::size_t>(detected, task_count));
}

void run_parallel_archive_tasks(
    std::size_t task_count,
    const std::function<void(std::size_t)>& task) {
    unsigned int worker_count = archive_worker_count(task_count);
    if (worker_count <= 1) {
        for (std::size_t index = 0; index < task_count; ++index) {
            task(index);
        }
        return;
    }

    std::atomic<std::size_t> next_task{0};
    std::atomic<bool> stop{false};
    std::mutex error_mutex;
    std::exception_ptr error;

    auto worker = [&]() {
        try {
            while (!stop.load()) {
                std::size_t index = next_task.fetch_add(1);
                if (index >= task_count) {
                    return;
                }
                task(index);
            }
        } catch (...) {
            stop.store(true);
            std::lock_guard<std::mutex> lock(error_mutex);
            if (!error) {
                error = std::current_exception();
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    try {
        for (unsigned int index = 0; index < worker_count; ++index) {
            workers.emplace_back(worker);
        }
    } catch (...) {
        stop.store(true);
        for (auto& thread : workers) {
            thread.join();
        }
        throw;
    }
    for (auto& thread : workers) {
        thread.join();
    }
    if (error) {
        std::rethrow_exception(error);
    }
}

std::uint32_t crc32_bytes(const std::string& data) {
    static const std::array<std::uint32_t, 256> table = []() {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1U) ? (0xedb88320U ^ (value >> 1U)) : (value >> 1U);
            }
            values[i] = value;
        }
        return values;
    }();

    std::uint32_t crc = 0xffffffffU;
    for (unsigned char ch : data) {
        crc = table[(crc ^ ch) & 0xffU] ^ (crc >> 8U);
    }
    return crc ^ 0xffffffffU;
}

void write_le16(std::ostream& out, std::uint16_t value) {
    out.put(static_cast<char>(value & 0xffU));
    out.put(static_cast<char>((value >> 8U) & 0xffU));
}

void write_le32(std::ostream& out, std::uint32_t value) {
    out.put(static_cast<char>(value & 0xffU));
    out.put(static_cast<char>((value >> 8U) & 0xffU));
    out.put(static_cast<char>((value >> 16U) & 0xffU));
    out.put(static_cast<char>((value >> 24U) & 0xffU));
}

std::uint16_t read_le16(const std::string& data, std::size_t offset) {
    if (offset + 2 > data.size()) {
        throw std::runtime_error("invalid zip file");
    }
    return static_cast<std::uint16_t>(
        static_cast<unsigned char>(data[offset]) |
        (static_cast<unsigned char>(data[offset + 1]) << 8));
}

std::uint32_t read_le32(const std::string& data, std::size_t offset) {
    if (offset + 4 > data.size()) {
        throw std::runtime_error("invalid zip file");
    }
    return static_cast<std::uint32_t>(
        static_cast<unsigned char>(data[offset]) |
        (static_cast<unsigned char>(data[offset + 1]) << 8) |
        (static_cast<unsigned char>(data[offset + 2]) << 16) |
        (static_cast<unsigned char>(data[offset + 3]) << 24));
}

std::string read_binary_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to read " + path.string());
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void write_binary_file(const fs::path& path, const std::string& data) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("failed to write " + path.string());
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::vector<std::string> read_list_file(const fs::path& path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    if (!in) {
        return lines;
    }
    std::string line;
    bool first_line = true;
    while (std::getline(in, line)) {
        if (first_line) {
            first_line = false;
            if (line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xef &&
                static_cast<unsigned char>(line[1]) == 0xbb &&
                static_cast<unsigned char>(line[2]) == 0xbf) {
                line.erase(0, 3);
            }
        }
        std::string stripped = trim(line);
        if (!stripped.empty()) {
            lines.push_back(stripped);
        }
    }
    return lines;
}

#ifdef NEOTHEMIS_HAS_ZLIB
std::string zip_deflate_raw(const std::string& input) {
    if (input.empty()) {
        return {};
    }

    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS,
                     8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("failed to initialize zip compression");
    }

    std::string output;
    output.resize(compressBound(static_cast<uLong>(input.size())));
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());

    int result = deflate(&stream, Z_FINISH);
    if (result != Z_STREAM_END) {
        deflateEnd(&stream);
        throw std::runtime_error("failed to compress zip entry");
    }
    output.resize(stream.total_out);
    deflateEnd(&stream);
    return output;
}

std::string inflate_with_window_bits(const std::string& input, int window_bits) {
    z_stream stream{};
    if (inflateInit2(&stream, window_bits) != Z_OK) {
        throw std::runtime_error("failed to initialize zlib decompression");
    }

    std::string output;
    output.resize(std::max<std::size_t>(4096, input.size() * 4));
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());

    for (;;) {
        if (stream.total_out == output.size()) {
            output.resize(output.size() * 2);
        }
        stream.next_out = reinterpret_cast<Bytef*>(output.data() + stream.total_out);
        stream.avail_out = static_cast<uInt>(output.size() - stream.total_out);

        int result = inflate(&stream, Z_NO_FLUSH);
        if (result == Z_STREAM_END) {
            output.resize(stream.total_out);
            inflateEnd(&stream);
            return output;
        }
        if (result != Z_OK) {
            inflateEnd(&stream);
            throw std::runtime_error("failed to decompress zlib data");
        }
    }
}

std::string zip_inflate_raw(const std::string& input, std::uint32_t output_size) {
    std::string output;
    output.resize(output_size);
    if (output_size == 0) {
        return output;
    }

    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        throw std::runtime_error("failed to initialize zip extraction");
    }

    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());

    int result = inflate(&stream, Z_FINISH);
    if (result != Z_STREAM_END || stream.total_out != output_size) {
        inflateEnd(&stream);
        throw std::runtime_error("failed to extract compressed zip entry");
    }
    inflateEnd(&stream);
    return output;
}
#endif

std::string inflate_old_config(const std::string& compressed, const fs::path& path) {
#ifdef NEOTHEMIS_HAS_ZLIB
    try {
        return inflate_with_window_bits(compressed, MAX_WBITS);
    } catch (const std::exception&) {
        try {
            return inflate_with_window_bits(compressed, -MAX_WBITS);
        } catch (const std::exception&) {
            throw std::runtime_error("failed to decompress old Themis config: " +
                                     path.string());
        }
    }
#else
    (void)compressed;
    throw std::runtime_error("zlib support is required to read old Themis config: " +
                             path.string());
#endif
}

bool archive_name_is_directory(const std::string& name) {
    return !name.empty() && name.back() == '/';
}

std::string normalized_archive_name(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    while (!name.empty() && name.front() == '/') {
        name.erase(name.begin());
    }
    if (name.empty() || (name.size() >= 2 && name[1] == ':')) {
        throw std::runtime_error("unsafe zip entry path");
    }

    std::stringstream stream(name);
    std::string segment;
    std::vector<std::string> safe_segments;
    while (std::getline(stream, segment, '/')) {
        if (segment.empty()) {
            continue;
        }
        if (segment == "." || segment == "..") {
            throw std::runtime_error("unsafe zip entry path");
        }
        safe_segments.push_back(segment);
    }
    if (safe_segments.empty()) {
        throw std::runtime_error("unsafe zip entry path");
    }

    std::string normalized;
    for (const auto& safe_segment : safe_segments) {
        if (!normalized.empty()) {
            normalized.push_back('/');
        }
        normalized += safe_segment;
    }
    if (archive_name_is_directory(name)) {
        normalized.push_back('/');
    }
    return normalized;
}

fs::path archive_output_path(const fs::path& root, const std::string& name) {
    std::string normalized = normalized_archive_name(name);
    fs::path result = root;
    std::stringstream stream(normalized);
    std::string segment;
    while (std::getline(stream, segment, '/')) {
        if (!segment.empty()) {
            result /= segment;
        }
    }
    return result;
}

void prepare_zip_content(const std::string& name,
                         std::string original,
                         bool compress,
                         ZipEntry& entry,
                         std::string& content) {
    (void)name;
    entry.crc = crc32_bytes(original);
    entry.uncompressed_size = static_cast<std::uint32_t>(original.size());
    entry.method = 0;
#ifdef NEOTHEMIS_HAS_ZLIB
    if (compress && !archive_name_is_directory(name) && !original.empty()) {
        std::string compressed = zip_deflate_raw(original);
        if (compressed.size() < original.size()) {
            content = std::move(compressed);
            entry.method = 8;
            entry.compressed_size = static_cast<std::uint32_t>(content.size());
            return;
        }
    }
#else
    (void)compress;
#endif
    content = std::move(original);
    entry.compressed_size = static_cast<std::uint32_t>(content.size());
}

void write_zip_local_entry(std::ostream& out,
                           ZipEntry& entry,
                           const std::string& content) {
    entry.offset = static_cast<std::uint32_t>(out.tellp());
    write_le32(out, 0x04034b50U);
    write_le16(out, 20);
    write_le16(out, 0);
    write_le16(out, entry.method);
    write_le16(out, 0);
    write_le16(out, 0);
    write_le32(out, entry.crc);
    write_le32(out, entry.compressed_size);
    write_le32(out, entry.uncompressed_size);
    write_le16(out, static_cast<std::uint16_t>(entry.name.size()));
    write_le16(out, 0);
    out.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

void write_zip_archive(const fs::path& path,
                       const std::vector<PendingZipEntry>& pending,
                       bool compress,
                       const ArchiveProgress& progress) {
    std::uint64_t total_entries = static_cast<std::uint64_t>(pending.size());
    report_progress(progress, 0, total_entries, "compressing_archive");

    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path());
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("failed to write " + path.string());
    }

    unsigned int worker_count = archive_worker_count(pending.size());
    std::size_t max_pending = std::max<std::size_t>(1, worker_count);
    std::vector<std::unique_ptr<PreparedZipEntry>> ready(pending.size());
    std::mutex state_mutex;
    std::condition_variable state_changed;
    std::size_t next_task = 0;
    std::size_t next_write = 0;
    bool stop = false;
    std::exception_ptr worker_error;
    std::mutex progress_mutex;
    std::uint64_t completed_entries = 0;

    auto worker = [&]() {
        try {
            for (;;) {
                std::size_t index = 0;
                {
                    std::unique_lock<std::mutex> lock(state_mutex);
                    state_changed.wait(lock, [&]() {
                        return stop || next_task >= pending.size() ||
                               next_task < next_write + max_pending;
                    });
                    if (stop || next_task >= pending.size()) {
                        return;
                    }
                    index = next_task++;
                }

                auto result = std::make_unique<PreparedZipEntry>();
                result->entry.name = normalized_archive_name(pending[index].name);
                std::string original;
                if (!pending[index].directory) {
                    original = read_binary_file(pending[index].path);
                }
                prepare_zip_content(result->entry.name, std::move(original), compress,
                                    result->entry, result->content);

                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    ready[index] = std::move(result);
                }
                {
                    std::lock_guard<std::mutex> lock(progress_mutex);
                    ++completed_entries;
                    report_progress(progress, completed_entries, total_entries,
                                    "compressing_archive");
                }
                state_changed.notify_all();
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                if (!worker_error) {
                    worker_error = std::current_exception();
                }
                stop = true;
            }
            state_changed.notify_all();
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    try {
        for (unsigned int index = 0; index < worker_count; ++index) {
            workers.emplace_back(worker);
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            stop = true;
        }
        state_changed.notify_all();
        for (auto& thread : workers) {
            thread.join();
        }
        throw;
    }

    std::vector<ZipEntry> entries;
    entries.reserve(pending.size());
    std::exception_ptr write_error;
    for (std::size_t index = 0; index < pending.size(); ++index) {
        std::unique_ptr<PreparedZipEntry> result;
        {
            std::unique_lock<std::mutex> lock(state_mutex);
            state_changed.wait(lock, [&]() {
                return stop || worker_error || ready[index] != nullptr;
            });
            if (worker_error) {
                break;
            }
            result = std::move(ready[index]);
            next_write = index + 1;
        }
        state_changed.notify_all();

        try {
            write_zip_local_entry(out, result->entry, result->content);
            if (!out) {
                throw std::runtime_error("failed to write " + path.string());
            }
            entries.push_back(std::move(result->entry));
        } catch (...) {
            write_error = std::current_exception();
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                stop = true;
            }
            state_changed.notify_all();
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex);
        stop = true;
    }
    state_changed.notify_all();
    for (auto& thread : workers) {
        thread.join();
    }
    if (worker_error) {
        std::rethrow_exception(worker_error);
    }
    if (write_error) {
        std::rethrow_exception(write_error);
    }

    std::uint32_t central_offset = static_cast<std::uint32_t>(out.tellp());
    for (const auto& entry : entries) {
        write_le32(out, 0x02014b50U);
        write_le16(out, 20);
        write_le16(out, 20);
        write_le16(out, 0);
        write_le16(out, entry.method);
        write_le16(out, 0);
        write_le16(out, 0);
        write_le32(out, entry.crc);
        write_le32(out, entry.compressed_size);
        write_le32(out, entry.uncompressed_size);
        write_le16(out, static_cast<std::uint16_t>(entry.name.size()));
        write_le16(out, 0);
        write_le16(out, 0);
        write_le16(out, 0);
        write_le16(out, 0);
        write_le32(out, 0);
        write_le32(out, entry.offset);
        out.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
    }
    std::uint32_t central_size = static_cast<std::uint32_t>(out.tellp()) - central_offset;

    write_le32(out, 0x06054b50U);
    write_le16(out, 0);
    write_le16(out, 0);
    write_le16(out, static_cast<std::uint16_t>(entries.size()));
    write_le16(out, static_cast<std::uint16_t>(entries.size()));
    write_le32(out, central_size);
    write_le32(out, central_offset);
    write_le16(out, 0);
    if (!out) {
        throw std::runtime_error("failed to write " + path.string());
    }
}

bool should_skip_contest_archive_path(const fs::path& path) {
    for (const auto& part : path) {
        if (part == ".neothemis-work") {
            return true;
        }
    }
    return false;
}

std::string archive_name_for_path(const fs::path& relative) {
    return normalized_archive_name(relative.generic_string());
}

std::vector<PendingZipEntry> collect_contest_archive_entries(const fs::path& root,
                                                             const ArchiveProgress& progress) {
    if (!fs::exists(root)) {
        throw std::runtime_error("contest folder not found: " + root.string());
    }

    report_progress(progress, 0, 0, "scanning_contest");
    std::vector<fs::path> paths;
    for (fs::recursive_directory_iterator it(root), end; it != end; ++it) {
        fs::path relative = fs::relative(it->path(), root);
        if (should_skip_contest_archive_path(relative)) {
            if (it->is_directory()) {
                it.disable_recursion_pending();
            }
            continue;
        }
        paths.push_back(it->path());
    }
    std::sort(paths.begin(), paths.end());

    std::vector<PendingZipEntry> entries;
    entries.reserve(paths.size());
    std::uint64_t total_paths = static_cast<std::uint64_t>(paths.size());
    report_progress(progress, 0, total_paths, "reading_contest_files");
    for (std::size_t index = 0; index < paths.size(); ++index) {
        const fs::path& path = paths[index];
        fs::path relative = fs::relative(path, root);
        std::string name = archive_name_for_path(relative);
        if (fs::is_directory(path)) {
            if (!archive_name_is_directory(name)) {
                name.push_back('/');
            }
            entries.push_back({path, name, true});
        } else if (fs::is_regular_file(path)) {
            entries.push_back({path, name, false});
        }
        report_progress(progress, static_cast<std::uint64_t>(index + 1), total_paths,
                        "reading_contest_files");
    }
    return entries;
}

bool path_is_same_or_inside(const fs::path& base, const fs::path& candidate) {
    fs::path base_normal = fs::absolute(base).lexically_normal();
    fs::path candidate_normal = fs::absolute(candidate).lexically_normal();
    auto base_it = base_normal.begin();
    auto candidate_it = candidate_normal.begin();
    for (; base_it != base_normal.end(); ++base_it, ++candidate_it) {
        if (candidate_it == candidate_normal.end() || *base_it != *candidate_it) {
            return false;
        }
    }
    return true;
}

fs::path relative_path_from_old_entry(const std::string& value) {
    std::string normalized = value;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    while (!normalized.empty() && normalized.front() == '/') {
        normalized.erase(normalized.begin());
    }
    fs::path path;
    std::stringstream stream(normalized);
    std::string part;
    while (std::getline(stream, part, '/')) {
        part = trim(part);
        if (!part.empty() && part != "." && part != "..") {
            path /= part;
        }
    }
    return path;
}

fs::path find_child_directory_case_insensitive(const fs::path& parent,
                                               const std::string& name) {
    fs::path exact = parent / name;
    if (fs::is_directory(exact)) {
        return exact;
    }
    if (!fs::is_directory(parent)) {
        return {};
    }
    std::string wanted = lower_ascii(name);
    for (const auto& entry : fs::directory_iterator(parent)) {
        if (entry.is_directory() &&
            lower_ascii(entry.path().filename().string()) == wanted) {
            return entry.path();
        }
    }
    return {};
}

fs::path find_child_file_case_insensitive(const fs::path& parent,
                                          const std::string& name) {
    fs::path exact = parent / name;
    if (fs::is_regular_file(exact)) {
        return exact;
    }
    if (!fs::is_directory(parent)) {
        return {};
    }
    std::string wanted = lower_ascii(name);
    for (const auto& entry : fs::directory_iterator(parent)) {
        if (entry.is_regular_file() &&
            lower_ascii(entry.path().filename().string()) == wanted) {
            return entry.path();
        }
    }
    return {};
}

std::vector<std::string> path_parts(const fs::path& path) {
    std::vector<std::string> parts;
    for (const auto& part : path) {
        std::string text = part.string();
        if (!text.empty() && text != "." && text != "..") {
            parts.push_back(text);
        }
    }
    return parts;
}

std::string flattened_child_path_name(const fs::path& path) {
    std::vector<std::string> parts = path_parts(path);
    std::string result;
    for (const auto& part : parts) {
        if (!result.empty()) {
            result.push_back('_');
        }
        result += part;
    }
    return result;
}

std::string unique_folder_name(const std::string& preferred, std::set<std::string>& used) {
    std::string base = preferred.empty() ? "unnamed" : preferred;
    std::string candidate = base;
    int suffix = 2;
    while (used.count(lower_ascii(candidate)) != 0) {
        candidate = base + "_" + std::to_string(suffix++);
    }
    used.insert(lower_ascii(candidate));
    return candidate;
}

std::string xml_unescape(std::string value) {
    struct Replacement {
        const char* from;
        const char* to;
    };
    const Replacement replacements[] = {
        {"&quot;", "\""},
        {"&apos;", "'"},
        {"&lt;", "<"},
        {"&gt;", ">"},
        {"&amp;", "&"},
    };
    for (const auto& replacement : replacements) {
        std::size_t pos = 0;
        while ((pos = value.find(replacement.from, pos)) != std::string::npos) {
            value.replace(pos, std::strlen(replacement.from), replacement.to);
            pos += std::strlen(replacement.to);
        }
    }
    return value;
}

std::map<std::string, std::string> parse_xml_attributes(const std::string& tag) {
    std::map<std::string, std::string> attributes;
    std::size_t pos = 0;
    while (true) {
        std::size_t equal = tag.find('=', pos);
        if (equal == std::string::npos) {
            break;
        }
        std::size_t key_end = equal;
        while (key_end > 0 && std::isspace(static_cast<unsigned char>(tag[key_end - 1]))) {
            --key_end;
        }
        std::size_t key_start = key_end;
        while (key_start > 0) {
            unsigned char ch = static_cast<unsigned char>(tag[key_start - 1]);
            if (!std::isalnum(ch) && ch != '_' && ch != '-' && ch != ':') {
                break;
            }
            --key_start;
        }
        std::string key = tag.substr(key_start, key_end - key_start);
        std::size_t value_start = equal + 1;
        while (value_start < tag.size() &&
               std::isspace(static_cast<unsigned char>(tag[value_start]))) {
            ++value_start;
        }
        if (value_start >= tag.size() ||
            (tag[value_start] != '"' && tag[value_start] != '\'')) {
            pos = equal + 1;
            continue;
        }
        char quote = tag[value_start++];
        std::size_t value_end = tag.find(quote, value_start);
        if (value_end == std::string::npos) {
            break;
        }
        if (!key.empty()) {
            attributes[key] = xml_unescape(tag.substr(value_start, value_end - value_start));
        }
        pos = value_end + 1;
    }
    return attributes;
}

std::string find_tag(const std::string& xml, const std::string& tag_name) {
    std::string needle = "<" + tag_name;
    std::size_t start = xml.find(needle);
    while (start != std::string::npos) {
        std::size_t after = start + needle.size();
        if (after >= xml.size() ||
            std::isspace(static_cast<unsigned char>(xml[after])) ||
            xml[after] == '>' || xml[after] == '/') {
            std::size_t end = xml.find('>', start);
            if (end == std::string::npos) {
                return {};
            }
            return xml.substr(start, end - start + 1);
        }
        start = xml.find(needle, after);
    }
    return {};
}

std::vector<std::map<std::string, std::string>> find_all_tags(const std::string& xml,
                                                              const std::string& tag_name) {
    std::vector<std::map<std::string, std::string>> tags;
    std::string needle = "<" + tag_name;
    std::size_t start = 0;
    while ((start = xml.find(needle, start)) != std::string::npos) {
        std::size_t after = start + needle.size();
        if (after < xml.size() &&
            !std::isspace(static_cast<unsigned char>(xml[after])) &&
            xml[after] != '>' && xml[after] != '/') {
            start = after;
            continue;
        }
        std::size_t end = xml.find('>', start);
        if (end == std::string::npos) {
            break;
        }
        tags.push_back(parse_xml_attributes(xml.substr(start, end - start + 1)));
        start = end + 1;
    }
    return tags;
}

double parse_double_attribute(const std::map<std::string, std::string>& attributes,
                              const std::string& key,
                              double fallback) {
    auto found = attributes.find(key);
    if (found == attributes.end() || trim(found->second).empty()) {
        return fallback;
    }
    try {
        return std::stod(found->second);
    } catch (const std::exception&) {
        return fallback;
    }
}

std::string parse_string_attribute(const std::map<std::string, std::string>& attributes,
                                   const std::string& key,
                                   const std::string& fallback) {
    auto found = attributes.find(key);
    if (found == attributes.end() || trim(found->second).empty()) {
        return fallback;
    }
    return found->second;
}

struct OldTestCaseSettings {
    std::string name;
    double mark = -1.0;
};

struct OldProblemSettings {
    std::string name;
    std::string input_file;
    std::string output_file;
    double mark = 1.0;
    double time_limit_seconds = 1.0;
    double memory_limit_mb = 256.0;
    std::vector<OldTestCaseSettings> tests;
};

OldProblemSettings parse_old_problem_xml(const std::string& xml,
                                         const std::string& fallback_name) {
    OldProblemSettings settings;
    settings.name = fallback_name;
    settings.input_file = fallback_name + ".INP";
    settings.output_file = fallback_name + ".OUT";

    std::string root = find_tag(xml, "ExamInformation");
    if (root.empty()) {
        root = find_tag(xml, "Exam");
    }
    if (!root.empty()) {
        auto attributes = parse_xml_attributes(root);
        settings.name = parse_string_attribute(attributes, "Name", settings.name);
        settings.input_file = parse_string_attribute(attributes, "InputFile", settings.input_file);
        settings.output_file = parse_string_attribute(attributes, "OutputFile", settings.output_file);
        settings.mark = parse_double_attribute(attributes, "Mark", settings.mark);
        settings.time_limit_seconds = parse_double_attribute(attributes, "TimeLimit",
                                                             settings.time_limit_seconds);
        settings.memory_limit_mb = parse_double_attribute(attributes, "MemoryLimit",
                                                          settings.memory_limit_mb);
    }

    for (const auto& attributes : find_all_tags(xml, "TestCase")) {
        OldTestCaseSettings test;
        test.name = parse_string_attribute(attributes, "Name", {});
        test.mark = parse_double_attribute(attributes, "Mark", -1.0);
        if (!test.name.empty()) {
            settings.tests.push_back(test);
        }
    }
    return settings;
}

OldProblemSettings read_old_problem_settings(const fs::path& old_task_dir,
                                             const std::string& problem) {
    fs::path settings_path = find_child_file_case_insensitive(old_task_dir, "Settings.cfg");
    if (settings_path.empty()) {
        OldProblemSettings defaults;
        defaults.name = problem;
        defaults.input_file = problem + ".INP";
        defaults.output_file = problem + ".OUT";
        return defaults;
    }
    std::string xml = inflate_old_config(read_binary_file(settings_path), settings_path);
    OldProblemSettings settings = parse_old_problem_xml(xml, problem);
    if (settings.name.empty()) {
        settings.name = problem;
    }
    return settings;
}

fs::path find_io_file(const fs::path& test_dir,
                      const std::string& preferred,
                      const std::string& extension) {
    if (!preferred.empty()) {
        fs::path found = find_child_file_case_insensitive(test_dir, preferred);
        if (!found.empty()) {
            return found;
        }
    }
    if (!fs::is_directory(test_dir)) {
        return {};
    }
    std::string wanted_ext = lower_ascii(extension);
    for (const auto& entry : fs::directory_iterator(test_dir)) {
        if (entry.is_regular_file() &&
            lower_ascii(entry.path().extension().string()) == wanted_ext) {
            return entry.path();
        }
    }
    return {};
}

bool has_old_contest_markers(const fs::path& path) {
    return fs::is_directory(find_child_directory_case_insensitive(path, "Contestants")) &&
           fs::is_directory(find_child_directory_case_insensitive(path, "Tasks"));
}

fs::path detect_old_contest_root(const fs::path& root) {
    if (has_old_contest_markers(root)) {
        return root;
    }
    if (fs::is_directory(root)) {
        for (const auto& child : fs::directory_iterator(root)) {
            if (child.is_directory() && has_old_contest_markers(child.path())) {
                return child.path();
            }
        }
        for (const auto& child : fs::recursive_directory_iterator(root)) {
            if (child.is_directory() && has_old_contest_markers(child.path())) {
                return child.path();
            }
        }
    }
    throw std::runtime_error("old Themis contest root not found: " + root.string());
}

void write_default_neothemis_config(const fs::path& new_root) {
    std::ofstream out(new_root / "neothemis.conf");
    if (!out) {
        throw std::runtime_error("failed to write " + (new_root / "neothemis.conf").string());
    }
    out << "core=builtin\n"
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
        << "forbidden_pattern=system(\n"
        << "forbidden_pattern=popen(\n"
        << "forbidden_pattern=fork(\n"
        << "forbidden_pattern=exec(\n"
        << "forbidden_pattern=#include <unistd.h>\n"
        << "forbidden_pattern=#include <sys/\n"
        << "forbidden_pattern=#include <windows.h>\n";
}

void copy_old_contestants(const fs::path& old_root,
                          const fs::path& new_root,
                          const ArchiveProgress& progress) {
    fs::path old_contestants = find_child_directory_case_insensitive(old_root, "Contestants");
    fs::path new_contestants = new_root / "contestants";
    fs::create_directories(new_contestants);

    std::vector<std::string> listed =
        read_list_file(find_child_file_case_insensitive(old_root, "ContestantDirectories.txt"));
    if (listed.empty() && fs::is_directory(old_contestants)) {
        for (const auto& entry : fs::directory_iterator(old_contestants)) {
            if (entry.is_directory()) {
                listed.push_back(entry.path().filename().string());
            }
        }
        std::sort(listed.begin(), listed.end());
    }

    std::set<std::string> used_names;
    report_progress(progress, 0, static_cast<std::uint64_t>(listed.size()),
                    "converting_old_contest");
    for (std::size_t index = 0; index < listed.size(); ++index) {
        fs::path relative = relative_path_from_old_entry(listed[index]);
        fs::path source = old_contestants / relative;
        if (!fs::exists(source)) {
            source = find_child_directory_case_insensitive(old_contestants,
                                                           relative.filename().string());
        }
        if (!fs::is_directory(source)) {
            report_progress(progress, static_cast<std::uint64_t>(index + 1),
                            static_cast<std::uint64_t>(listed.size()),
                            "converting_old_contest");
            continue;
        }

        std::string preferred = relative.filename().string();
        if (preferred.empty()) {
            preferred = flattened_child_path_name(relative);
        }
        fs::path destination = new_contestants / unique_folder_name(preferred, used_names);
        copy_directory_contents(source, destination);
        report_progress(progress, static_cast<std::uint64_t>(index + 1),
                        static_cast<std::uint64_t>(listed.size()),
                        "converting_old_contest");
    }
}

struct OldTaskPlan {
    std::vector<std::string> problems;
    std::map<std::string, std::vector<fs::path>> tests;
};

OldTaskPlan read_old_task_plan(const fs::path& old_root) {
    OldTaskPlan plan;
    std::vector<std::string> listed =
        read_list_file(find_child_file_case_insensitive(old_root, "TaskDirectories.txt"));
    for (const auto& line : listed) {
        fs::path relative = relative_path_from_old_entry(line);
        std::vector<std::string> parts = path_parts(relative);
        if (parts.empty()) {
            continue;
        }
        const std::string& problem = parts.front();
        if (parts.size() == 1) {
            if (std::find(plan.problems.begin(), plan.problems.end(), problem) ==
                plan.problems.end()) {
                plan.problems.push_back(problem);
            }
            continue;
        }
        fs::path test_relative;
        for (std::size_t i = 1; i < parts.size(); ++i) {
            test_relative /= parts[i];
        }
        if (std::find(plan.problems.begin(), plan.problems.end(), problem) ==
            plan.problems.end()) {
            plan.problems.push_back(problem);
        }
        plan.tests[problem].push_back(test_relative);
    }

    fs::path tasks_root = find_child_directory_case_insensitive(old_root, "Tasks");
    if (plan.problems.empty() && fs::is_directory(tasks_root)) {
        for (const auto& entry : fs::directory_iterator(tasks_root)) {
            if (entry.is_directory()) {
                plan.problems.push_back(entry.path().filename().string());
            }
        }
        std::sort(plan.problems.begin(), plan.problems.end());
    }

    for (const auto& problem : plan.problems) {
        auto& tests = plan.tests[problem];
        if (!tests.empty()) {
            continue;
        }
        fs::path task_dir = find_child_directory_case_insensitive(tasks_root, problem);
        if (!fs::is_directory(task_dir)) {
            continue;
        }
        for (const auto& entry : fs::directory_iterator(task_dir)) {
            if (entry.is_directory()) {
                tests.push_back(entry.path().filename());
            }
        }
        std::sort(tests.begin(), tests.end());
    }
    return plan;
}

std::map<std::string, double> test_marks_by_name(const OldProblemSettings& settings) {
    std::map<std::string, double> marks;
    for (const auto& test : settings.tests) {
        if (test.mark >= 0.0) {
            marks[test.name] = test.mark;
            marks[point_key_for_test_name(test.name)] = test.mark;
        }
    }
    return marks;
}

std::string copy_optional_checker_sources(const fs::path& old_task_dir,
                                          const fs::path& new_problem_dir) {
    std::string checker_setting = "token";
    for (const char* name : {"checker.cpp", "check.cpp", "testlib.h"}) {
        fs::path source = find_child_file_case_insensitive(old_task_dir, name);
        if (!source.empty()) {
            fs::copy_file(source, new_problem_dir / source.filename(),
                          fs::copy_options::overwrite_existing);
            std::string copied_name = source.filename().string();
            if (lower_ascii(copied_name) == "checker.cpp") {
                checker_setting = "custom";
            } else if (lower_ascii(copied_name) == "check.cpp" &&
                       checker_setting == "token") {
                checker_setting = "custom:" + copied_name;
            }
        }
    }
    return checker_setting;
}

void write_problem_config(const fs::path& path,
                          const OldProblemSettings& settings,
                          const std::vector<fs::path>& tests,
                          const std::string& checker_setting) {
    double default_points = settings.mark > 0.0 ? settings.mark : 1.0;
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("failed to write " + path.string());
    }
    std::uint64_t time_limit_ms =
        settings.time_limit_seconds > 0.0
            ? static_cast<std::uint64_t>(settings.time_limit_seconds * 1000.0 + 0.5)
            : 1000;
    std::uint64_t memory_limit_mb =
        settings.memory_limit_mb > 0.0
            ? static_cast<std::uint64_t>(settings.memory_limit_mb + 0.5)
            : 256;

    out << "time_limit_ms=" << time_limit_ms << '\n'
        << "memory_limit_mb=" << memory_limit_mb << '\n'
        << "default_points=" << format_number(default_points) << '\n'
        << "checker=" << checker_setting << '\n';

    auto marks = test_marks_by_name(settings);
    for (const auto& test : tests) {
        std::string test_name = test.filename().string();
        auto by_name = marks.find(test_name);
        if (by_name == marks.end()) {
            by_name = marks.find(point_key_for_test_name(test_name));
        }
        if (by_name == marks.end()) {
            continue;
        }
        if (settings.mark > 0.0 && points_equal(by_name->second, default_points)) {
            continue;
        }
        out << "test_points." << point_key_for_test_name(test_name) << '='
            << format_number(by_name->second) << '\n';
    }
}

void copy_old_tasks(const fs::path& old_root,
                    const fs::path& new_root,
                    const ArchiveProgress& progress) {
    fs::path old_tasks = find_child_directory_case_insensitive(old_root, "Tasks");
    fs::path new_tests = new_root / "tests";
    fs::create_directories(new_tests);
    OldTaskPlan plan = read_old_task_plan(old_root);
    report_progress(progress, 0, static_cast<std::uint64_t>(plan.problems.size()),
                    "converting_old_contest");

    for (std::size_t problem_index = 0; problem_index < plan.problems.size(); ++problem_index) {
        const std::string& problem = plan.problems[problem_index];
        fs::path old_task_dir = find_child_directory_case_insensitive(old_tasks, problem);
        if (!fs::is_directory(old_task_dir)) {
            report_progress(progress, static_cast<std::uint64_t>(problem_index + 1),
                            static_cast<std::uint64_t>(plan.problems.size()),
                            "converting_old_contest");
            continue;
        }

        fs::path new_problem_dir = new_tests / problem;
        fs::create_directories(new_problem_dir);
        OldProblemSettings settings = read_old_problem_settings(old_task_dir, problem);
        std::string checker_setting = copy_optional_checker_sources(old_task_dir, new_problem_dir);

        const auto& tests = plan.tests[problem];
        for (const auto& old_test_relative : tests) {
            fs::path old_test_dir = old_task_dir / old_test_relative;
            if (!fs::is_directory(old_test_dir)) {
                continue;
            }
            std::string new_test_name = old_test_relative.filename().string();
            if (new_test_name.empty()) {
                new_test_name = flattened_child_path_name(old_test_relative);
            }
            fs::path new_test_dir = new_problem_dir / new_test_name;
            fs::create_directories(new_test_dir);

            fs::path input = find_io_file(old_test_dir, settings.input_file, ".inp");
            fs::path output = find_io_file(old_test_dir, settings.output_file, ".out");
            if (!input.empty()) {
                fs::copy_file(input, new_test_dir / (problem + ".inp"),
                              fs::copy_options::overwrite_existing);
            }
            if (!output.empty()) {
                fs::copy_file(output, new_test_dir / (problem + ".out"),
                              fs::copy_options::overwrite_existing);
            }
        }

        write_problem_config(new_problem_dir / "problem.conf", settings, tests, checker_setting);
        report_progress(progress, static_cast<std::uint64_t>(problem_index + 1),
                        static_cast<std::uint64_t>(plan.problems.size()),
                        "converting_old_contest");
    }
}

} // namespace

bool has_zlib_support() {
#ifdef NEOTHEMIS_HAS_ZLIB
    return true;
#else
    return false;
#endif
}

bool is_ncontest_file(const fs::path& path) {
    return lower_ascii(path.extension().string()) == ".ncontest";
}

bool is_zip_like_file(const fs::path& path) {
    std::string extension = lower_ascii(path.extension().string());
    return extension == ".ncontest" || extension == ".contest" || extension == ".zip";
}

fs::path ensure_ncontest_extension(fs::path path) {
    if (lower_ascii(path.extension().string()) != ".ncontest") {
        path += ".ncontest";
    }
    return path;
}

fs::path make_temp_directory(const std::string& prefix) {
    fs::path base = fs::temp_directory_path();
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 1000; ++attempt) {
        fs::path candidate = base / (prefix + "-" + std::to_string(now) + "-" +
                                     std::to_string(attempt));
        std::error_code ec;
        if (fs::create_directories(candidate, ec)) {
            return candidate;
        }
    }
    throw std::runtime_error("failed to create temporary directory");
}

void extract_zip_archive(const fs::path& archive_path,
                         const fs::path& destination,
                         const ArchiveProgress& progress) {
    report_progress(progress, 0, 0, "reading_archive");
    std::string zip = read_binary_file(archive_path);
    if (zip.size() < 22) {
        throw std::runtime_error("invalid zip file");
    }

    std::size_t eocd = std::string::npos;
    std::size_t min_offset = zip.size() > 65557 ? zip.size() - 65557 : 0;
    for (std::size_t pos = zip.size() - 22;; --pos) {
        if (read_le32(zip, pos) == 0x06054b50U) {
            eocd = pos;
            break;
        }
        if (pos == min_offset) {
            break;
        }
    }
    if (eocd == std::string::npos) {
        throw std::runtime_error("invalid zip file");
    }

    std::uint16_t entry_count = read_le16(zip, eocd + 10);
    std::uint32_t central_offset = read_le32(zip, eocd + 16);
    std::size_t cursor = central_offset;
    fs::create_directories(destination);
    report_progress(progress, 0, entry_count, "extracting_archive");

    std::vector<ZipExtractionEntry> files;
    files.reserve(entry_count);
    std::uint64_t completed_entries = 0;
    for (std::uint16_t index = 0; index < entry_count; ++index) {
        if (read_le32(zip, cursor) != 0x02014b50U) {
            throw std::runtime_error("invalid zip central directory");
        }
        std::uint16_t flags = read_le16(zip, cursor + 8);
        std::uint16_t method = read_le16(zip, cursor + 10);
        std::uint32_t crc = read_le32(zip, cursor + 16);
        std::uint32_t compressed_size = read_le32(zip, cursor + 20);
        std::uint32_t uncompressed_size = read_le32(zip, cursor + 24);
        std::uint16_t name_length = read_le16(zip, cursor + 28);
        std::uint16_t extra_length = read_le16(zip, cursor + 30);
        std::uint16_t comment_length = read_le16(zip, cursor + 32);
        std::uint32_t local_offset = read_le32(zip, cursor + 42);
        if ((flags & 1U) != 0) {
            throw std::runtime_error("encrypted zip entries are not supported");
        }
        if (cursor + 46 + name_length + extra_length + comment_length > zip.size()) {
            throw std::runtime_error("invalid zip central directory");
        }
        std::string name = zip.substr(cursor + 46, name_length);
        cursor += 46 + name_length + extra_length + comment_length;
        std::string normalized_name = normalized_archive_name(name);

        if (read_le32(zip, local_offset) != 0x04034b50U) {
            throw std::runtime_error("invalid zip local header");
        }
        std::uint16_t local_name_length = read_le16(zip, local_offset + 26);
        std::uint16_t local_extra_length = read_le16(zip, local_offset + 28);
        std::size_t data_offset = local_offset + 30 + local_name_length + local_extra_length;
        if (data_offset + compressed_size > zip.size()) {
            throw std::runtime_error("invalid zip entry data");
        }

        fs::path output_path = archive_output_path(destination, normalized_name);
        if (archive_name_is_directory(normalized_name)) {
            fs::create_directories(output_path);
            ++completed_entries;
            report_progress(progress, completed_entries, entry_count, "extracting_archive");
            continue;
        }

        if (method != 0 && method != 8) {
            throw std::runtime_error("unsupported zip compression method");
        }
#ifndef NEOTHEMIS_HAS_ZLIB
        if (method == 8) {
            throw std::runtime_error("compressed zip entries require zlib support");
        }
#endif
        files.push_back(ZipExtractionEntry{output_path, data_offset, method, crc,
                                           compressed_size, uncompressed_size});
    }

    std::mutex progress_mutex;
    run_parallel_archive_tasks(files.size(), [&](std::size_t index) {
        const ZipExtractionEntry& entry = files[index];
        std::string compressed = zip.substr(entry.data_offset, entry.compressed_size);
        std::string content;
        if (entry.method == 0) {
            content = std::move(compressed);
            if (content.size() != entry.uncompressed_size) {
                throw std::runtime_error("invalid stored zip entry");
            }
        } else {
#ifdef NEOTHEMIS_HAS_ZLIB
            content = zip_inflate_raw(compressed, entry.uncompressed_size);
#endif
        }
        if (crc32_bytes(content) != entry.crc) {
            throw std::runtime_error("zip entry checksum failed");
        }

        write_binary_file(entry.output_path, content);
        std::lock_guard<std::mutex> lock(progress_mutex);
        ++completed_entries;
        report_progress(progress, completed_entries, entry_count, "extracting_archive");
    });
}

void write_zip_archive_from_directory(const fs::path& archive_path,
                                      const fs::path& contest_root,
                                      const ArchiveProgress& progress) {
    write_zip_archive(archive_path, collect_contest_archive_entries(contest_root, progress),
                      true, progress);
}

void copy_directory_contents(const fs::path& source,
                             const fs::path& destination,
                             const ArchiveProgress& progress) {
    if (path_is_same_or_inside(source, destination)) {
        if (fs::absolute(source).lexically_normal() ==
            fs::absolute(destination).lexically_normal()) {
            return;
        }
        throw std::runtime_error("cannot copy a contest folder inside itself");
    }
    fs::create_directories(destination);
    report_progress(progress, 0, 0, "scanning_contest");
    std::vector<fs::path> paths;
    for (fs::recursive_directory_iterator it(source), end; it != end; ++it) {
        fs::path relative = fs::relative(it->path(), source);
        if (should_skip_contest_archive_path(relative)) {
            if (it->is_directory()) {
                it.disable_recursion_pending();
            }
            continue;
        }
        paths.push_back(it->path());
    }
    std::sort(paths.begin(), paths.end());

    std::uint64_t total_paths = static_cast<std::uint64_t>(paths.size());
    report_progress(progress, 0, total_paths, "copying_contest_files");
    for (std::size_t index = 0; index < paths.size(); ++index) {
        const fs::path& path = paths[index];
        fs::path relative = fs::relative(path, source);
        fs::path target = destination / relative;
        if (fs::is_directory(path)) {
            fs::create_directories(target);
        } else if (fs::is_regular_file(path)) {
            fs::create_directories(target.parent_path());
            fs::copy_file(path, target, fs::copy_options::overwrite_existing);
        }
        report_progress(progress, static_cast<std::uint64_t>(index + 1), total_paths,
                        "copying_contest_files");
    }
}

void convert_old_themis_contest(const fs::path& old_contest,
                                const fs::path& output_ncontest,
                                const ArchiveProgress& progress) {
    fs::path old_root_source = old_contest;
    std::unique_ptr<ScopedDirectory> extracted_old;
    if (fs::is_regular_file(old_contest)) {
        fs::path temp = make_temp_directory("neothemis-old-contest");
        extracted_old = std::make_unique<ScopedDirectory>(temp);
        extract_zip_archive(old_contest, temp, progress);
        old_root_source = temp;
    }

    fs::path old_root = detect_old_contest_root(old_root_source);
    fs::path new_root = make_temp_directory("neothemis-converted-contest");
    ScopedDirectory converted(new_root);
    fs::create_directories(new_root / "contestants");
    fs::create_directories(new_root / "tests");

    write_default_neothemis_config(new_root);
    copy_old_contestants(old_root, new_root, progress);
    copy_old_tasks(old_root, new_root, progress);

    write_zip_archive_from_directory(ensure_ncontest_extension(output_ncontest), new_root,
                                     progress);
}

} // namespace neothemis
