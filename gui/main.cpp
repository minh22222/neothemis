#include "neothemis/JudgeCore.hpp"
#include "neothemis/ContestArchive.hpp"

#include <QAction>
#include <QAbstractItemView>
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGraphicsDropShadowEffect>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#ifdef NEOTHEMIS_HAS_ZLIB
#include <zlib.h>
#endif

namespace fs = std::filesystem;

namespace {

struct CellScore {
    double earned = 0.0;
    double max = 0.0;
    int completed = 0;
};

std::string trim(const std::string& value) {
    std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::map<std::string, std::string> read_config_file(const fs::path& path) {
    std::map<std::string, std::string> values;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::string stripped = trim(line);
        if (stripped.empty() || stripped[0] == '#') {
            continue;
        }
        std::size_t equal = stripped.find('=');
        if (equal != std::string::npos) {
            values[trim(stripped.substr(0, equal))] = trim(stripped.substr(equal + 1));
        }
    }
    return values;
}

void write_problem_config(const fs::path& path,
                          int time_ms,
                          int memory_mb,
                          const std::string& default_points,
                          const std::string& checker,
                          const std::vector<std::pair<std::string, std::string>>& test_points) {
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("failed to write " + path.string());
    }
    out << "time_limit_ms=" << time_ms << '\n'
        << "memory_limit_mb=" << memory_mb << '\n'
        << "default_points=" << default_points << '\n'
        << "checker=" << checker << '\n';
    for (const auto& entry : test_points) {
        out << "test_points." << entry.first << '=' << entry.second << '\n';
    }
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
    return unique_keys;
}

std::vector<std::string> test_names_for_problem(const fs::path& problem_root) {
    std::vector<std::string> names;
    if (!fs::exists(problem_root)) {
        return names;
    }
    for (const auto& entry : fs::directory_iterator(problem_root)) {
        if (entry.is_directory()) {
            names.push_back(entry.path().filename().string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

fs::path default_temporary_dir() {
    QString qt_temp = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    fs::path base = qt_temp.isEmpty() ? fs::temp_directory_path()
                                      : fs::path(qt_temp.toStdString());
    return base / "neothemis";
}

QString format_points(double value) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    out << value;
    std::string text = out.str();
    while (text.size() > 1 && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return QString::fromStdString(text);
}

QPixmap load_logo_pixmap() {
    QPixmap resource_pixmap(":/materials/logo.png");
    if (!resource_pixmap.isNull()) {
        return resource_pixmap;
    }

    std::vector<fs::path> candidates = {
        fs::path(QApplication::applicationDirPath().toStdString()) / "materials" / "logo.png",
        fs::current_path() / "materials" / "logo.png",
        fs::current_path().parent_path() / "materials" / "logo.png"
    };
    for (const auto& candidate : candidates) {
        if (fs::exists(candidate)) {
            QPixmap pixmap(QString::fromStdString(candidate.string()));
            if (!pixmap.isNull()) {
                return pixmap;
            }
        }
    }
    return {};
}

struct XlsxCell {
    bool is_number = false;
    double number = 0.0;
    std::string text;
};

using XlsxRow = std::vector<XlsxCell>;

XlsxCell xlsx_text(std::string text) {
    XlsxCell cell;
    cell.text = std::move(text);
    return cell;
}

XlsxCell xlsx_number(double value) {
    XlsxCell cell;
    cell.is_number = true;
    cell.number = value;
    return cell;
}

std::string xlsx_xml_escape(const std::string& value) {
    std::string escaped;
    for (unsigned char ch : value) {
        switch (ch) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            case '\'': escaped += "&apos;"; break;
            default:
                if ((ch < 0x20 && ch != '\n' && ch != '\r' && ch != '\t') || ch == 0x7f) {
                    escaped += ' ';
                } else {
                    escaped.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return escaped;
}

std::string xlsx_column_name(std::size_t index) {
    std::string name;
    ++index;
    while (index > 0) {
        std::size_t remainder = (index - 1) % 26;
        name.push_back(static_cast<char>('A' + remainder));
        index = (index - 1) / 26;
    }
    std::reverse(name.begin(), name.end());
    return name;
}

std::string xlsx_number_text(double value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(15) << value;
    return out.str();
}

std::string xlsx_sheet_xml(const std::vector<XlsxRow>& rows,
                           const std::vector<double>& widths) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        << R"(<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">)"
        << R"(<sheetViews><sheetView workbookViewId="0">)";
    if (!rows.empty()) {
        out << R"(<pane ySplit="1" topLeftCell="A2" activePane="bottomLeft" state="frozen"/>)";
    }
    out << R"(</sheetView></sheetViews>)";
    if (!widths.empty()) {
        out << "<cols>";
        for (std::size_t i = 0; i < widths.size(); ++i) {
            out << "<col min=\"" << (i + 1) << "\" max=\"" << (i + 1)
                << "\" width=\"" << widths[i] << "\" customWidth=\"1\"/>";
        }
        out << "</cols>";
    }
    out << "<sheetData>";
    for (std::size_t r = 0; r < rows.size(); ++r) {
        out << "<row r=\"" << (r + 1) << "\">";
        const auto& row = rows[r];
        for (std::size_t c = 0; c < row.size(); ++c) {
            std::string ref = xlsx_column_name(c) + std::to_string(r + 1);
            const XlsxCell& cell = row[c];
            if (cell.is_number) {
                out << "<c r=\"" << ref << "\"><v>" << xlsx_number_text(cell.number)
                    << "</v></c>";
            } else {
                out << "<c r=\"" << ref << "\" t=\"inlineStr\"";
                if (r == 0) {
                    out << " s=\"1\"";
                }
                out << "><is><t>" << xlsx_xml_escape(cell.text) << "</t></is></c>";
            }
        }
        out << "</row>";
    }
    out << "</sheetData></worksheet>";
    return out.str();
}

std::uint32_t crc32_bytes(const std::string& data) {
    static std::uint32_t table[256]{};
    static bool initialized = false;
    if (!initialized) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1U) ? (0xedb88320U ^ (value >> 1U)) : (value >> 1U);
            }
            table[i] = value;
        }
        initialized = true;
    }

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

struct ZipEntry {
    std::string name;
    std::string data;
    std::uint32_t crc = 0;
    std::uint32_t offset = 0;
    std::uint16_t method = 0;
    std::uint32_t uncompressed_size = 0;
};

using ArchiveProgress = std::function<void(std::uint64_t, std::uint64_t, const char*)>;

void report_archive_progress(const ArchiveProgress& progress,
                             std::uint64_t done,
                             std::uint64_t total,
                             const char* label_key) {
    if (progress) {
        progress(done, total, label_key);
    }
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

void prepare_zip_entry(ZipEntry& entry, bool compress) {
    std::string original = entry.data;
    entry.crc = crc32_bytes(original);
    entry.uncompressed_size = static_cast<std::uint32_t>(original.size());
    entry.method = 0;
#ifdef NEOTHEMIS_HAS_ZLIB
    if (compress && !archive_name_is_directory(entry.name) && !original.empty()) {
        std::string compressed = zip_deflate_raw(original);
        if (compressed.size() < original.size()) {
            entry.data = std::move(compressed);
            entry.method = 8;
            return;
        }
    }
#else
    (void)compress;
#endif
    entry.data = std::move(original);
}

void write_zip_store(const fs::path& path,
                     std::vector<ZipEntry> entries,
                     bool compress = false,
                     const ArchiveProgress& progress = {}) {
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path());
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("failed to write " + path.string());
    }

    std::uint64_t total_entries = static_cast<std::uint64_t>(entries.size());
    for (auto& entry : entries) {
        report_archive_progress(progress,
                                static_cast<std::uint64_t>(&entry - entries.data()),
                                total_entries,
                                "compressing_archive");
        entry.name = normalized_archive_name(entry.name);
        prepare_zip_entry(entry, compress);
        entry.offset = static_cast<std::uint32_t>(out.tellp());
        write_le32(out, 0x04034b50U);
        write_le16(out, 20);
        write_le16(out, 0);
        write_le16(out, entry.method);
        write_le16(out, 0);
        write_le16(out, 0);
        write_le32(out, entry.crc);
        write_le32(out, static_cast<std::uint32_t>(entry.data.size()));
        write_le32(out, entry.uncompressed_size);
        write_le16(out, static_cast<std::uint16_t>(entry.name.size()));
        write_le16(out, 0);
        out.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
        out.write(entry.data.data(), static_cast<std::streamsize>(entry.data.size()));
        report_archive_progress(progress,
                                static_cast<std::uint64_t>(&entry - entries.data() + 1),
                                total_entries,
                                "compressing_archive");
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
        write_le32(out, static_cast<std::uint32_t>(entry.data.size()));
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
}

void extract_zip_file(const fs::path& archive_path,
                      const fs::path& destination,
                      const ArchiveProgress& progress = {}) {
    report_archive_progress(progress, 0, 0, "reading_archive");
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
    report_archive_progress(progress, 0, entry_count, "extracting_archive");

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

        if (read_le32(zip, local_offset) != 0x04034b50U) {
            throw std::runtime_error("invalid zip local header");
        }
        std::uint16_t local_name_length = read_le16(zip, local_offset + 26);
        std::uint16_t local_extra_length = read_le16(zip, local_offset + 28);
        std::size_t data_offset = local_offset + 30 + local_name_length + local_extra_length;
        if (data_offset + compressed_size > zip.size()) {
            throw std::runtime_error("invalid zip entry data");
        }

        fs::path output_path = archive_output_path(destination, name);
        if (archive_name_is_directory(normalized_archive_name(name))) {
            fs::create_directories(output_path);
            report_archive_progress(progress, index + 1, entry_count, "extracting_archive");
            continue;
        }

        std::string compressed = zip.substr(data_offset, compressed_size);
        std::string content;
        if (method == 0) {
            content = std::move(compressed);
            if (content.size() != uncompressed_size) {
                throw std::runtime_error("invalid stored zip entry");
            }
        } else if (method == 8) {
#ifdef NEOTHEMIS_HAS_ZLIB
            content = zip_inflate_raw(compressed, uncompressed_size);
#else
            throw std::runtime_error("compressed zip entries require zlib support");
#endif
        } else {
            throw std::runtime_error("unsupported zip compression method");
        }
        if (crc32_bytes(content) != crc) {
            throw std::runtime_error("zip entry checksum failed");
        }

        fs::create_directories(output_path.parent_path());
        std::ofstream out(output_path, std::ios::binary);
        if (!out) {
            throw std::runtime_error("failed to write " + output_path.string());
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        report_archive_progress(progress, index + 1, entry_count, "extracting_archive");
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
    std::string name = relative.generic_string();
    return normalized_archive_name(name);
}

std::vector<ZipEntry> collect_contest_archive_entries(const fs::path& root,
                                                      const ArchiveProgress& progress = {}) {
    std::vector<ZipEntry> entries;
    if (!fs::exists(root)) {
        throw std::runtime_error("contest folder not found: " + root.string());
    }

    report_archive_progress(progress, 0, 0, "scanning_contest");
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

    std::uint64_t total_paths = static_cast<std::uint64_t>(paths.size());
    std::uint64_t done_paths = 0;
    report_archive_progress(progress, done_paths, total_paths, "reading_contest_files");
    for (const auto& path : paths) {
        fs::path relative = fs::relative(path, root);
        std::string name = archive_name_for_path(relative);
        if (fs::is_directory(path)) {
            if (!archive_name_is_directory(name)) {
                name.push_back('/');
            }
            entries.push_back({name, {}});
        } else if (fs::is_regular_file(path)) {
            entries.push_back({name, read_binary_file(path)});
        }
        ++done_paths;
        report_archive_progress(progress, done_paths, total_paths, "reading_contest_files");
    }
    return entries;
}

void write_contest_archive(const fs::path& archive_path,
                           const fs::path& contest_root,
                           const ArchiveProgress& progress = {}) {
    write_zip_store(archive_path, collect_contest_archive_entries(contest_root, progress),
                    true, progress);
}

void write_xlsx_file(const fs::path& path,
                     const std::string& sheet_name,
                     const std::vector<XlsxRow>& rows,
                     const std::vector<double>& widths) {
    std::vector<ZipEntry> entries;
    entries.push_back({"[Content_Types].xml",
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        R"(<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">)"
        R"(<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>)"
        R"(<Default Extension="xml" ContentType="application/xml"/>)"
        R"(<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>)"
        R"(<Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>)"
        R"(<Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>)"
        R"(</Types>)"});
    entries.push_back({"_rels/.rels",
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
        R"(<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>)"
        R"(</Relationships>)"});
    entries.push_back({"xl/workbook.xml",
        std::string(R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)") +
        R"(<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" )" +
        R"(xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">)" +
        R"(<sheets><sheet name=")" + xlsx_xml_escape(sheet_name) +
        R"(" sheetId="1" r:id="rId1"/></sheets></workbook>)"});
    entries.push_back({"xl/_rels/workbook.xml.rels",
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
        R"(<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>)"
        R"(<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>)"
        R"(</Relationships>)"});
    entries.push_back({"xl/styles.xml",
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        R"(<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">)"
        R"(<fonts count="2"><font><sz val="11"/><name val="Calibri"/></font><font><b/><sz val="11"/><name val="Calibri"/></font></fonts>)"
        R"(<fills count="2"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill></fills>)"
        R"(<borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>)"
        R"(<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>)"
        R"(<cellXfs count="2"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/><xf numFmtId="0" fontId="1" fillId="0" borderId="0" xfId="0" applyFont="1"/></cellXfs>)"
        R"(<cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles>)"
        R"(</styleSheet>)"});
    entries.push_back({"xl/worksheets/sheet1.xml", xlsx_sheet_xml(rows, widths)});
    write_zip_store(path, std::move(entries));
}

std::vector<std::vector<std::string>> read_csv_records(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    std::string text = buffer.str();

    std::vector<std::vector<std::string>> records;
    std::vector<std::string> row;
    std::string cell;
    bool in_quotes = false;
    bool have_data = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char ch = text[i];
        have_data = true;
        if (in_quotes) {
            if (ch == '"' && i + 1 < text.size() && text[i + 1] == '"') {
                cell.push_back('"');
                ++i;
            } else if (ch == '"') {
                in_quotes = false;
            } else {
                cell.push_back(ch);
            }
            continue;
        }
        if (ch == '"') {
            in_quotes = true;
        } else if (ch == ',') {
            row.push_back(cell);
            cell.clear();
        } else if (ch == '\n') {
            row.push_back(cell);
            cell.clear();
            records.push_back(row);
            row.clear();
            have_data = false;
        } else if (ch != '\r') {
            cell.push_back(ch);
        }
    }
    if (have_data || !cell.empty() || !row.empty()) {
        row.push_back(cell);
        records.push_back(row);
    }
    return records;
}

neothemis::Verdict verdict_from_string(const std::string& value) {
    if (value == "AC") return neothemis::Verdict::Accepted;
    if (value == "WA") return neothemis::Verdict::WrongAnswer;
    if (value == "CE") return neothemis::Verdict::CompileError;
    if (value == "RE") return neothemis::Verdict::RuntimeError;
    if (value == "TLE") return neothemis::Verdict::TimeLimitExceeded;
    if (value == "MLE") return neothemis::Verdict::MemoryLimitExceeded;
    if (value == "MS") return neothemis::Verdict::MissingSource;
    if (value == "SV") return neothemis::Verdict::SecurityViolation;
    return neothemis::Verdict::InternalError;
}

struct UiTextEntry {
    const char* key;
    const char* en;
    const char* vi;
};

const UiTextEntry kUiText[] = {
    {"window_title", "Contest Judge", u8"Chấm bài"},
    {"no_contest_open", "No contest open", u8"Chưa mở kỳ thi"},
    {"contest", "Contest", u8"Kỳ thi"},
    {"open_folder", "Open Folder", u8"Mở thư mục"},
    {"open_contest_file", "Open Contest File (.ncontest)", u8"Mở tệp kỳ thi (.ncontest)"},
    {"save_contest_file", "Save Contest File", u8"Lưu tệp kỳ thi"},
    {"save_contest_file_as", "Save Contest File As", u8"Lưu tệp kỳ thi thành"},
    {"refresh", "Refresh", u8"Làm mới"},
    {"judge", "Judge", u8"Chấm bài"},
    {"judge_selected", "Judge Selected", u8"Chấm đã chọn"},
    {"judge_all", "Judge All", u8"Chấm tất cả"},
    {"stop", "Stop", u8"Dừng"},
    {"export", "Export", u8"Xuất"},
    {"export_scoreboard", "Export Scoreboard (xlsx)", u8"Xuất bảng điểm (xlsx)"},
    {"export_data", "Export Data (xlsx)", u8"Xuất dữ liệu (xlsx)"},
    {"converter", "Converter", u8"Chuyển đổi"},
    {"convert_old_contest_file", "Convert Contest File to .ncontest", u8"Chuyển tệp kỳ thi sang .ncontest"},
    {"convert_old_contest_folder", "Convert Contest Folder to .ncontest", u8"Chuyển thư mục kỳ thi sang .ncontest"},
    {"converting_contest_file", "Converting contest", u8"Đang chuyển đổi kỳ thi"},
    {"convert_complete", "Convert complete", u8"Chuyển đổi hoàn tất"},
    {"settings", "Settings", u8"Cài đặt"},
    {"application_settings", "Application Settings", u8"Cài đặt ứng dụng"},
    {"contest_config", "Contest Config", u8"Cấu hình kỳ thi"},
    {"problem_config", "Problem Config", u8"Cấu hình bài"},
    {"help", "Help", u8"Trợ giúp"},
    {"about", "About", u8"Giới thiệu"},
    {"no_active_run", "No active run", u8"Không có lượt chấm đang chạy"},
    {"idle", "Idle", u8"Đang nghỉ"},
    {"contestant", "Contestant", u8"Thí sinh"},
    {"total", "Total", u8"Tổng"},
    {"ready", "Ready", u8"Sẵn sàng"},
    {"missing", "Missing", u8"Thiếu"},
    {"done", "Done", u8"Xong"},
    {"running", "Running", u8"Đang chạy"},
    {"queued", "Queued", u8"Đang chờ"},
    {"stopping_active_run", "Stopping active judge run...", u8"Đang dừng lượt chấm..."},
    {"stopping", "Stopping", u8"Đang dừng"},
    {"settings_title", "Settings", u8"Cài đặt"},
    {"application", "Application", u8"Ứng dụng"},
    {"problems", "Problems", u8"Bài"},
    {"theme", "Theme", u8"Giao diện"},
    {"dark", "Dark", u8"Tối"},
    {"language", "Language", u8"Ngôn ngữ"},
    {"temporary_dir", "Temporary directory", u8"Thư mục tạm"},
    {"browse", "Browse", u8"Chọn"},
    {"english", "English", u8"Tiếng Anh"},
    {"vietnamese", "Vietnamese", u8"Tiếng Việt"},
    {"save_application_settings", "Save Application Settings", u8"Lưu cài đặt ứng dụng"},
    {"compiler", "Compiler", u8"Trình biên dịch"},
    {"compile_flags", "Compile flags", u8"Cờ biên dịch"},
    {"contestants_dir", "Contestants dir", u8"Thư mục thí sinh"},
    {"tests_dir", "Tests dir", u8"Thư mục test"},
    {"stack_mb", "Stack MB", u8"Bộ nhớ stack MB"},
    {"parallel_jobs", "Parallel jobs (0 = auto)", u8"Số luồng chấm (0 = tự động)"},
    {"keep_workdir", "Keep workdir", u8"Giữ thư mục tạm"},
    {"save_contest_config", "Save Contest Config", u8"Lưu cấu hình kỳ thi"},
    {"problem", "Problem", u8"Bài"},
    {"time_limit_ms", "Time limit ms", u8"Giới hạn thời gian ms"},
    {"memory_mb", "Memory MB", u8"Bộ nhớ MB"},
    {"default_points", "Default points", u8"Điểm mặc định"},
    {"checker", "Checker", u8"Trình chấm"},
    {"test_points", "Test points", u8"Điểm từng test"},
    {"test", "Test", u8"Test"},
    {"point_override", "Point override", u8"Điểm riêng"},
    {"selected_point", "Selected point", u8"Điểm cho test đã chọn"},
    {"apply_to_selected", "Apply to selected tests", u8"Áp dụng cho test đã chọn"},
    {"save_problem_config", "Save Problem Config", u8"Lưu cấu hình bài"},
    {"close", "Close", u8"Đóng"},
    {"save_failed", "Save failed", u8"Lưu thất bại"},
    {"open_failed", "Open failed", u8"Mở thất bại"},
    {"save_complete", "Save complete", u8"Đã lưu"},
    {"save_as_complete", "Save as complete", u8"Đã lưu thành"},
    {"unsaved_title", "Unsaved contest file", u8"Tệp kỳ thi chưa lưu"},
    {"unsaved_message", "The open contest file has unsaved changes. Close it anyway?", u8"Tệp kỳ thi đang mở có thay đổi chưa lưu. Vẫn đóng?"},
    {"save", "Save", u8"Lưu"},
    {"cancel", "Cancel", u8"Hủy"},
    {"close_anyway", "Close anyway", u8"Vẫn đóng"},
    {"wait_for_judge_operation", "Wait for the active judge run to finish first.", u8"Hãy chờ lượt chấm hiện tại hoàn tất trước."},
    {"judge_running", "Judge running", u8"Đang chấm bài"},
    {"wait_for_export_judge", "Wait for the active judge run to finish before exporting.", u8"Hãy chờ lượt chấm hiện tại hoàn tất trước khi xuất."},
    {"no_contest", "No contest", u8"Chưa có kỳ thi"},
    {"open_contest_first", "Open or create a contest folder first.", u8"Hãy mở hoặc tạo thư mục kỳ thi trước."},
    {"no_results", "No results", u8"Chưa có kết quả"},
    {"no_results_detail", "No judged results are available to export yet.", u8"Chưa có kết quả chấm để xuất."},
    {"no_selection", "No selection", u8"Chưa chọn"},
    {"select_contestant_rows", "Select one or more contestant rows.", u8"Hãy chọn một hoặc nhiều dòng thí sinh."},
    {"judge_already_active", "A judge run is already active", u8"Một lượt chấm đang chạy"},
    {"starting", "Starting", u8"Đang bắt đầu"},
    {"selected_contestants", "selected contestants", u8"các thí sinh đã chọn"},
    {"all_contestants", "all contestants", u8"tất cả thí sinh"},
    {"judging", "Judging", u8"Đang chấm"},
    {"judging_log", "Judging...", u8"Đang chấm..."},
    {"judge_run_complete", "Judge run complete", u8"Lượt chấm hoàn tất"},
    {"judge_run_cancelled", "Judge run cancelled", u8"Lượt chấm đã hủy"},
    {"judge_run_failed", "Judge run failed", u8"Lượt chấm thất bại"},
    {"judge_csv_failed", "Judge run finished but CSV write failed", u8"Lượt chấm đã xong nhưng ghi CSV thất bại"},
    {"csv_write_failed", "CSV write failed", u8"Ghi CSV thất bại"},
    {"judge_failed", "Judge failed", u8"Chấm bài thất bại"},
    {"failed", "Failed", u8"Thất bại"},
    {"cancelled", "Cancelled", u8"Đã hủy"},
    {"export_workbook", "Export workbook", u8"Xuất workbook"},
    {"xlsx_filter", "Excel workbook (*.xlsx)", u8"Workbook Excel (*.xlsx)"},
    {"ncontest_open_filter", "NeoThemis contest (*.ncontest);;Zip archive (*.zip);;All files (*)", u8"Kỳ thi NeoThemis (*.ncontest);;Tệp nén zip (*.zip);;Tất cả tệp (*)"},
    {"ncontest_save_filter", "NeoThemis contest (*.ncontest)", u8"Kỳ thi NeoThemis (*.ncontest)"},
    {"themis_contest_filter", "Themis contest (*.contest *.zip);;All files (*)", u8"Kỳ thi Themis (*.contest *.zip);;Tất cả tệp (*)"},
    {"export_complete", "Export complete", u8"Xuất hoàn tất"},
    {"export_failed", "Export failed", u8"Xuất thất bại"},
    {"exported", "Exported", u8"Đã xuất"},
    {"opened", "Opened", u8"Đã mở"},
    {"malformed_row_skipped", "Skipped one malformed row in results.csv.", u8"Đã bỏ qua một dòng results.csv không hợp lệ."},
    {"about_title", "About NeoThemis", u8"Giới thiệu NeoThemis"},
    {"about_details", "NeoThemis\n\nA local competitive-programming contest judge for C++ submissions.\n\nFeatures:\n- Contest and per-problem configuration\n- Parallel judging with live progress\n- Custom checkers stored in each problem folder\n- CSV result and scoreboard output\n- Qt desktop interface for Windows and Linux\n\nChecker note:\nCustom checkers that include testlib.h must keep testlib.h in the same problem folder.\n\nBuild: Qt Widgets desktop application\n\nMade by vibe-coding\n\n\nstbchr", u8"NeoThemis\n\nTrình chấm kỳ thi lập trình thi đấu cục bộ cho bài nộp C++.\n\nTính năng:\n- Cấu hình kỳ thi và từng bài\n- Chấm song song với tiến trình trực tiếp\n- Trình chấm riêng đặt trong từng thư mục bài\n- Xuất kết quả CSV và bảng điểm\n- Giao diện Qt cho Windows và Linux\n\nGhi chú trình chấm:\nTrình chấm riêng dùng testlib.h phải đặt testlib.h trong cùng thư mục bài.\n\nBản dựng: Ứng dụng Qt Widgets\n\nMade by vibe-coding\n\n\nstbchr"},
    {"saved_app_settings", "Saved application settings.", u8"Đã lưu cài đặt ứng dụng."},
    {"saved_contest_config", "Saved contest config.", u8"Đã lưu cấu hình kỳ thi."},
    {"saved_problem_config", "Saved problem config for ", u8"Đã lưu cấu hình bài "},
    {"invalid_points", "Invalid points", u8"Điểm không hợp lệ"},
    {"invalid_points_detail", "Point values must be blank or numeric.", u8"Điểm phải để trống hoặc là số."},
    {"archive_operation_running", "A file operation is already running.", u8"Một thao tác tệp đang chạy."},
    {"wait_for_archive_operation", "Wait for the file operation to finish first.", u8"Hãy chờ thao tác tệp hoàn tất trước."},
    {"opening_contest_file", "Opening contest file", u8"Đang mở tệp kỳ thi"},
    {"saving_contest_file", "Saving contest file", u8"Đang lưu tệp kỳ thi"},
    {"reading_archive", "Reading archive", u8"Đang đọc tệp nén"},
    {"extracting_archive", "Extracting archive", u8"Đang giải nén"},
    {"scanning_contest", "Scanning contest", u8"Đang quét kỳ thi"},
    {"reading_contest_files", "Reading contest files", u8"Đang đọc tệp kỳ thi"},
    {"compressing_archive", "Compressing archive", u8"Đang nén"},
    {"copying_contest_files", "Copying contest files", u8"Đang sao chép tệp kỳ thi"},
    {"converting_old_contest", "Converting old contest", u8"Đang chuyển đổi kỳ thi cũ"},
    {"file_operation_complete", "File operation complete", u8"Thao tác tệp hoàn tất"},
    {"file_operation_failed", "File operation failed", u8"Thao tác tệp thất bại"}
};

class MainWindow : public QMainWindow {
public:
    MainWindow() {
        load_app_settings();
        setWindowTitle(text("window_title"));
        setWindowIcon(QIcon(":/materials/logo.png"));
        setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
        resize(1240, 780);

        auto* central = new QWidget(this);
        central->setObjectName("AppRoot");
        auto* root = new QVBoxLayout(central);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);

        title_bar_ = new QWidget(central);
        title_bar_->setObjectName("WindowTitleBar");
        title_bar_->installEventFilter(this);
        auto* title_layout = new QHBoxLayout(title_bar_);
        title_layout->setContentsMargins(10, 0, 6, 0);
        title_layout->setSpacing(8);
        auto* logo = new QLabel(title_bar_);
        logo->setObjectName("AppLogo");
        QPixmap pixmap = load_logo_pixmap();
        if (!pixmap.isNull()) {
            logo->setPixmap(pixmap.scaled(24, 24, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
        logo->installEventFilter(this);
        auto* app_name = new QLabel("NeoThemis", title_bar_);
        app_name->setObjectName("WindowAppName");
        app_name->installEventFilter(this);
        title_layout->addWidget(logo);
        title_layout->addWidget(app_name);
        title_layout->addStretch(1);
        contest_title_ = new QLabel(text("no_contest_open"), title_bar_);
        contest_title_->setObjectName("ContestTitle");
        contest_title_->installEventFilter(this);
        title_layout->addWidget(contest_title_);
        auto* minimize = new QToolButton(title_bar_);
        minimize->setObjectName("WindowButton");
        minimize->setText("-");
        auto* maximize = new QToolButton(title_bar_);
        maximize->setObjectName("WindowButton");
        maximize->setText("[]");
        auto* close = new QToolButton(title_bar_);
        close->setObjectName("WindowCloseButton");
        close->setText("x");
        title_layout->addWidget(minimize);
        title_layout->addWidget(maximize);
        title_layout->addWidget(close);
        root->addWidget(title_bar_);

        auto* menu_row = new QWidget(central);
        menu_row->setObjectName("MenuRow");
        auto* menu_layout = new QHBoxLayout(menu_row);
        menu_layout->setContentsMargins(10, 0, 10, 0);
        menu_layout->setSpacing(0);
        menu_bar_ = new QMenuBar(menu_row);
        menu_bar_->setNativeMenuBar(false);
        menu_layout->addWidget(menu_bar_, 0, Qt::AlignLeft);
        menu_layout->addStretch(1);
        root->addWidget(menu_row);

        build_toolbar();

        auto* content = new QHBoxLayout;
        content->setContentsMargins(12, 10, 12, 12);
        content->setSpacing(10);

        table_ = new QTableWidget(central);
        table_->setObjectName("ScoreTable");
        table_->setAlternatingRowColors(true);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        table_->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
        table_->verticalHeader()->setVisible(false);
        table_->verticalHeader()->setDefaultSectionSize(54);
        content->addWidget(table_, 1);

        auto* side = new QWidget(central);
        side->setObjectName("SidePanel");
        auto* side_layout = new QVBoxLayout(side);
        side_layout->setContentsMargins(14, 14, 14, 14);
        side_layout->setSpacing(10);
        side->setFixedWidth(380);

        judge_group_ = new QGroupBox(text("judge"), side);
        judge_group_->setObjectName("GlassGroup");
        auto* action_layout = new QVBoxLayout(judge_group_);
        judge_selected_button_ = new QPushButton(text("judge_selected"), judge_group_);
        judge_all_button_ = new QPushButton(text("judge_all"), judge_group_);
        stop_button_ = new QPushButton(text("stop"), judge_group_);
        stop_button_->setObjectName("StopButton");
        stop_button_->setEnabled(false);
        action_layout->addWidget(judge_selected_button_);
        action_layout->addWidget(judge_all_button_);
        action_layout->addWidget(stop_button_);
        side_layout->addWidget(judge_group_);

        run_status_ = new QLabel(text("no_active_run"), side);
        run_status_->setWordWrap(true);
        progress_label_ = new QLabel(text("idle"), side);
        progress_ = new QProgressBar(side);
        progress_->setRange(0, 100);
        progress_->setValue(0);
        side_layout->addWidget(run_status_);
        side_layout->addWidget(progress_label_);
        side_layout->addWidget(progress_);

        log_ = new QPlainTextEdit(side);
        log_->setObjectName("LogPanel");
        log_->setReadOnly(true);
        side_layout->addWidget(log_, 1);

        content->addWidget(side);
        root->addLayout(content, 1);
        setCentralWidget(central);
        add_soft_shadow(table_, 34, 120);
        add_soft_shadow(side, 36, 135);
        apply_dark_theme();

        QObject::connect(judge_selected_button_, &QPushButton::clicked, [this]() { start_judge(true); });
        QObject::connect(judge_all_button_, &QPushButton::clicked, [this]() { start_judge(false); });
        QObject::connect(stop_button_, &QPushButton::clicked, [this]() { request_stop_judge(); });
        QObject::connect(table_, &QTableWidget::cellDoubleClicked, [this](int row, int col) {
            show_result_details(row, col);
        });
        QObject::connect(table_->horizontalHeader(), &QHeaderView::sectionClicked,
                         [this](int section) { sort_by_column(section); });
        QObject::connect(table_->horizontalHeader(), &QWidget::customContextMenuRequested,
                         [this](const QPoint& pos) { show_header_menu(pos); });
        QObject::connect(minimize, &QToolButton::clicked, this, &QWidget::showMinimized);
        QObject::connect(maximize, &QToolButton::clicked, [this]() {
            isMaximized() ? showNormal() : showMaximized();
        });
        QObject::connect(close, &QToolButton::clicked, this, &QWidget::close);
    }

    ~MainWindow() override {
        stop_active_judge();
        join_archive_thread();
        cleanup_temporary_contests();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == title_bar_ || watched == contest_title_ ||
            (watched->isWidgetType() &&
             static_cast<QWidget*>(watched)->objectName() == "WindowAppName") ||
            (watched->isWidgetType() &&
             static_cast<QWidget*>(watched)->objectName() == "AppLogo")) {
            if (event->type() == QEvent::MouseButtonDblClick) {
                auto* mouse = static_cast<QMouseEvent*>(event);
                if (mouse->button() == Qt::LeftButton) {
                    isMaximized() ? showNormal() : showMaximized();
                    return true;
                }
            }
            if (event->type() == QEvent::MouseButtonPress) {
                auto* mouse = static_cast<QMouseEvent*>(event);
                if (mouse->button() == Qt::LeftButton) {
                    dragging_title_bar_ = true;
                    drag_offset_ = mouse->globalPosition().toPoint() - frameGeometry().topLeft();
                    return true;
                }
            }
            if (event->type() == QEvent::MouseMove && dragging_title_bar_) {
                auto* mouse = static_cast<QMouseEvent*>(event);
                if (!isMaximized()) {
                    move(mouse->globalPosition().toPoint() - drag_offset_);
                }
                return true;
            }
            if (event->type() == QEvent::MouseButtonRelease) {
                dragging_title_bar_ = false;
            }
        }
        return QMainWindow::eventFilter(watched, event);
    }

    void closeEvent(QCloseEvent* event) override {
        if (archive_running_.load()) {
            QMessageBox::information(this, text("archive_operation_running"),
                                     text("wait_for_archive_operation"));
            event->ignore();
            return;
        }
        if (!confirm_discard_unsaved_file(true)) {
            event->ignore();
            return;
        }
        stop_active_judge();
        join_archive_thread();
        cleanup_temporary_contests();
        event->accept();
    }

private:
    QString text(const char* key) const {
        for (const auto& entry : kUiText) {
            if (std::strcmp(entry.key, key) == 0) {
                return QString::fromUtf8(language_ == "vi" ? entry.vi : entry.en);
            }
        }
        return QString::fromUtf8(key);
    }

    void load_app_settings() {
        QSettings settings("NeoThemis", "NeoThemis");
        language_ = settings.value("language", "en").toString().toStdString();
        if (language_ != "vi") {
            language_ = "en";
        }
        temporary_dir_ = settings.value(
            "temporary_dir",
            QString::fromStdString(default_temporary_dir().string())).toString().toStdString();
        if (temporary_dir_.empty()) {
            temporary_dir_ = default_temporary_dir();
        }
    }

    void save_app_settings() const {
        QSettings settings("NeoThemis", "NeoThemis");
        settings.setValue("language", QString::fromStdString(language_));
        settings.setValue("temporary_dir", QString::fromStdString(temporary_dir_.string()));
    }

    void apply_language_to_main_window() {
        setWindowTitle(text("window_title"));
        if (contest_root_.empty() && contest_title_) {
            contest_title_->setText(text("no_contest_open"));
        }
        if (judge_group_) {
            judge_group_->setTitle(text("judge"));
        }
        if (judge_selected_button_) {
            judge_selected_button_->setText(text("judge_selected"));
        }
        if (judge_all_button_) {
            judge_all_button_->setText(text("judge_all"));
        }
        if (stop_button_) {
            stop_button_->setText(text("stop"));
        }
        if (!judging_.load()) {
            if (run_status_) {
                run_status_->setText(text("no_active_run"));
            }
            if (progress_label_) {
                progress_label_->setText(text("idle"));
            }
        }
        build_toolbar();
        if (!contestants_.empty() || !problems_.empty()) {
            populate_table();
        }
    }

    void build_toolbar() {
        auto* bar = menu_bar_ ? menu_bar_ : menuBar();
        bar->clear();

        auto* contest_menu = bar->addMenu(text("contest"));
        contest_menu->addAction(text("open_folder"), [this]() { open_contest(); });
        contest_menu->addAction(text("open_contest_file"), [this]() { open_contest_file(); });
        contest_menu->addSeparator();
        contest_menu->addAction(text("save_contest_file"), [this]() { save_contest_container(false); });
        contest_menu->addAction(text("save_contest_file_as"), [this]() { save_contest_container(true); });
        contest_menu->addSeparator();
        contest_menu->addAction(text("refresh"), [this]() { refresh_table(); });

        auto* judge_menu = bar->addMenu(text("judge"));
        judge_selected_action_ = judge_menu->addAction(text("judge_selected"), [this]() { start_judge(true); });
        judge_all_action_ = judge_menu->addAction(text("judge_all"), [this]() { start_judge(false); });
        stop_action_ = judge_menu->addAction(text("stop"), [this]() { request_stop_judge(); });
        stop_action_->setEnabled(false);

        auto* export_menu = bar->addMenu(text("export"));
        export_menu->addAction(text("export_scoreboard"), [this]() { export_scoreboard_xlsx(); });
        export_menu->addAction(text("export_data"), [this]() { export_data_xlsx(); });

        auto* converter_menu = bar->addMenu(text("converter"));
        converter_menu->addAction(text("convert_old_contest_file"), [this]() {
            convert_old_contest_to_ncontest(false);
        });
        converter_menu->addAction(text("convert_old_contest_folder"), [this]() {
            convert_old_contest_to_ncontest(true);
        });

        auto* settings_menu = bar->addMenu(text("settings"));
        settings_menu->addAction(text("application_settings"), [this]() { open_settings_dialog(0); });
        settings_menu->addAction(text("contest_config"), [this]() { open_settings_dialog(1); });
        settings_menu->addAction(text("problem_config"), [this]() { open_settings_dialog(2); });

        auto* help_menu = bar->addMenu(text("help"));
        help_menu->addAction(text("about"), [this]() { show_about_dialog(); });
    }

    void add_soft_shadow(QWidget* widget, qreal blur_radius, int alpha) {
        if (!widget) {
            return;
        }
        auto* shadow = new QGraphicsDropShadowEffect(widget);
        shadow->setBlurRadius(blur_radius);
        shadow->setOffset(0, 12);
        shadow->setColor(QColor(0, 0, 0, alpha));
        widget->setGraphicsEffect(shadow);
    }

    void apply_dark_theme() {
        qApp->setStyleSheet(R"(
            QWidget {
                background: #090d12;
                color: #edf3f7;
                font-size: 13px;
                selection-background-color: #2a6f72;
                selection-color: #ffffff;
            }
            QMainWindow {
                background: #03070a;
            }
            QDialog {
                background: rgba(10, 16, 23, 238);
            }
            QWidget#AppRoot {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                    stop:0 rgba(17, 38, 48, 224),
                    stop:0.45 rgba(12, 19, 27, 232),
                    stop:1 rgba(30, 19, 30, 224));
                border: 1px solid rgba(255, 255, 255, 44);
            }
            QWidget#WindowTitleBar {
                background: rgba(18, 27, 34, 212);
                border-bottom: 1px solid rgba(255, 255, 255, 34);
                min-height: 38px;
            }
            QWidget#MenuRow {
                background: rgba(15, 22, 29, 178);
                border-bottom: 1px solid rgba(84, 211, 194, 50);
                min-height: 34px;
            }
            QLabel#WindowAppName {
                color: #f8fafc; font-size: 14px; font-weight: 800;
                padding-right: 10px;
            }
            QLabel#AppLogo {
                min-width: 28px; min-height: 28px;
            }
            QMenuBar {
                background: transparent; border: 0; padding: 0;
            }
            QMenuBar::item {
                background: transparent;
                padding: 7px 13px;
                border-radius: 8px;
                margin: 2px 1px;
            }
            QMenuBar::item:selected {
                background: rgba(84, 211, 194, 42);
                color: #8ef7e3;
                border: 1px solid rgba(142, 247, 227, 90);
            }
            QToolButton#WindowButton, QToolButton#WindowCloseButton {
                background: transparent;
                border: 0;
                color: #d9e6ec;
                min-width: 46px;
                min-height: 38px;
                border-radius: 0;
                font-weight: 700;
            }
            QToolButton#WindowButton:hover {
                background: rgba(84, 211, 194, 44);
                color: #ffffff;
            }
            QToolButton#WindowCloseButton:hover {
                background: rgba(207, 63, 88, 180);
                color: #ffffff;
            }
            QPushButton {
                background: rgba(24, 35, 43, 206);
                color: #ecfffb;
                border: 1px solid rgba(116, 224, 207, 120);
                border-radius: 8px;
                padding: 9px 12px;
                font-weight: 700;
            }
            QPushButton:hover {
                background: rgba(39, 65, 70, 230);
                border-color: #8ef7e3;
                color: #ffffff;
            }
            QPushButton:pressed { background: rgba(13, 20, 24, 235); border-color: #e86a82; }
            QPushButton:disabled {
                background: rgba(58, 65, 72, 160);
                color: #80909a;
                border-color: rgba(255, 255, 255, 30);
            }
            QPushButton#StopButton {
                background: rgba(76, 28, 42, 212);
                border-color: rgba(255, 112, 137, 145);
            }
            QPushButton#StopButton:hover { background: rgba(111, 34, 54, 230); border-color: #ff8ba1; }
            QPushButton#DangerButton {
                background: rgba(138, 35, 55, 220);
                border: 1px solid rgba(255, 121, 145, 180);
                color: #ffffff;
            }
            QPushButton#DangerButton:hover {
                background: rgba(180, 47, 72, 235);
                border-color: #ff9aae;
            }
            QMenu {
                background: rgba(18, 25, 32, 238);
                border: 1px solid rgba(255, 255, 255, 44);
                border-radius: 10px;
                padding: 6px;
            }
            QMenu::item { padding: 8px 24px; border-radius: 7px; }
            QMenu::item:selected {
                background: rgba(84, 211, 194, 38);
                color: #9dfdec;
            }
            QTableWidget, QPlainTextEdit, QLineEdit, QSpinBox, QComboBox {
                background: rgba(17, 24, 31, 204);
                border: 1px solid rgba(255, 255, 255, 36);
                border-radius: 8px;
                padding: 5px;
                color: #edf3f7;
            }
            QWidget#SidePanel {
                background: rgba(16, 23, 31, 174);
                border: 1px solid rgba(255, 255, 255, 45);
                border-radius: 14px;
            }
            QTableWidget#ScoreTable {
                background: rgba(10, 16, 22, 178);
                alternate-background-color: rgba(21, 31, 38, 192);
                gridline-color: rgba(99, 131, 141, 70);
                selection-background-color: rgba(56, 112, 119, 160);
                border-radius: 14px;
                outline: 0;
            }
            QPlainTextEdit#LogPanel {
                background: rgba(7, 12, 17, 178);
            }
            QTableWidget::item {
                border-bottom: 1px solid rgba(255, 255, 255, 22);
                padding: 6px;
            }
            QTableWidget::item:selected {
                background: rgba(62, 118, 124, 174);
                color: #ffffff;
                border: 0;
            }
            QTableWidget::item:focus {
                border: 0;
                outline: none;
            }
            QHeaderView::section {
                background: rgba(19, 31, 39, 220);
                color: #9dfdec;
                border: 0;
                border-right: 1px solid rgba(255, 255, 255, 28);
                border-bottom: 1px solid rgba(232, 106, 130, 120);
                padding: 8px;
                font-weight: 700;
            }
            QGroupBox {
                border: 1px solid rgba(255, 255, 255, 42);
                border-radius: 12px;
                margin-top: 10px;
                padding-top: 12px;
                background: rgba(22, 31, 39, 166);
            }
            QGroupBox::title {
                subcontrol-origin: margin;
                left: 12px;
                padding: 0 6px;
                color: #c4fff3;
            }
            QProgressBar {
                background: rgba(7, 12, 17, 190);
                border: 1px solid rgba(255, 255, 255, 42);
                border-radius: 8px;
                height: 18px;
                text-align: center;
            }
            QProgressBar::chunk {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                    stop:0 #61d7c7, stop:0.68 #8ef7e3, stop:1 #e86a82);
                border-radius: 7px;
            }
            QLabel { color: #d7e3e8; background: transparent; }
            QLabel#ContestTitle { color: #9db0b9; }
            QTabWidget::pane {
                border: 1px solid rgba(255, 255, 255, 42);
                border-radius: 10px;
                background: rgba(13, 19, 26, 164);
            }
            QTabBar::tab {
                background: rgba(24, 32, 39, 184);
                padding: 8px 12px;
                border-top-left-radius: 8px;
                border-top-right-radius: 8px;
                margin-right: 2px;
            }
            QTabBar::tab:selected {
                background: rgba(84, 211, 194, 38);
                color: #9dfdec;
                border: 1px solid rgba(142, 247, 227, 82);
            }
            QScrollBar:vertical, QScrollBar:horizontal {
                background: rgba(7, 12, 17, 120);
                border: 0;
                margin: 0;
            }
            QScrollBar:vertical { width: 10px; }
            QScrollBar:horizontal { height: 10px; }
            QScrollBar::handle { background: rgba(132, 156, 164, 122); border-radius: 5px; }
            QScrollBar::handle:hover { background: rgba(142, 247, 227, 155); }
            QScrollBar::handle:vertical { min-height: 26px; }
            QScrollBar::handle:horizontal { min-width: 26px; }
            QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
            QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
        )");
    }

    neothemis::JudgeOptions options_from_ui() const {
        neothemis::JudgeOptions options;
        options.contest_root = contest_root_;
        options.compiler = compiler_;
        options.compile_flags = compile_flags_;
        options.contestants_dir = contestants_dir_;
        options.tests_dir = tests_dir_;
        options.parallel_jobs = parallel_jobs_;
        options.stack_limit_mb = stack_limit_mb_;
        options.keep_workdir = keep_workdir_;
        options.forbidden_patterns = {
            "system(", "popen(", "fork(", "exec(", "#include <unistd.h>",
            "#include <sys/", "#include <windows.h>"
        };
        return options;
    }

    bool archive_operation_available() {
        if (archive_running_.load()) {
            QMessageBox::information(this, text("archive_operation_running"),
                                     text("wait_for_archive_operation"));
            return false;
        }
        if (judging_.load()) {
            QMessageBox::information(this, text("judge"),
                                     text("wait_for_judge_operation"));
            return false;
        }
        return true;
    }

    void join_archive_thread() {
        if (archive_thread_.joinable()) {
            archive_thread_.join();
        }
    }

    void set_archive_controls_enabled(bool enabled) {
        if (judge_selected_button_) {
            judge_selected_button_->setEnabled(enabled);
        }
        if (judge_all_button_) {
            judge_all_button_->setEnabled(enabled);
        }
        if (judge_selected_action_) {
            judge_selected_action_->setEnabled(enabled);
        }
        if (judge_all_action_) {
            judge_all_action_->setEnabled(enabled);
        }
        if (stop_button_) {
            stop_button_->setEnabled(false);
        }
        if (stop_action_) {
            stop_action_->setEnabled(false);
        }
    }

    void begin_archive_operation(const QString& label) {
        join_archive_thread();
        archive_running_.store(true);
        set_archive_controls_enabled(false);
        progress_->setRange(0, 0);
        progress_->setValue(0);
        progress_label_->setText(label);
        run_status_->setText(label);
        log_->appendPlainText(label);
    }

    void update_archive_progress(std::uint64_t done,
                                 std::uint64_t total,
                                 const std::string& label_key) {
        QString label = text(label_key.c_str());
        if (total == 0) {
            progress_->setRange(0, 0);
            progress_label_->setText(label);
            run_status_->setText(label);
            return;
        }

        int max = total > static_cast<std::uint64_t>(std::numeric_limits<int>::max())
                      ? std::numeric_limits<int>::max()
                      : static_cast<int>(total);
        int value = done > total ? max
                    : static_cast<int>((done * static_cast<std::uint64_t>(max)) / total);
        progress_->setRange(0, max);
        progress_->setValue(value);
        QString status = label + " " + QString::number(done) + "/" + QString::number(total);
        progress_label_->setText(status);
        run_status_->setText(status);
    }

    ArchiveProgress archive_progress_callback() {
        return [this](std::uint64_t done, std::uint64_t total, const char* label_key) {
            std::string key = label_key ? label_key : "";
            QMetaObject::invokeMethod(this, [this, done, total, key]() {
                update_archive_progress(done, total, key);
            }, Qt::QueuedConnection);
        };
    }

    void finish_archive_operation(const QString& label) {
        join_archive_thread();
        archive_running_.store(false);
        set_archive_controls_enabled(true);
        progress_->setRange(0, 100);
        progress_->setValue(100);
        progress_label_->setText(label);
        run_status_->setText(label);
    }

    void fail_archive_operation(const QString& label) {
        join_archive_thread();
        archive_running_.store(false);
        set_archive_controls_enabled(true);
        progress_->setRange(0, 100);
        progress_->setValue(0);
        progress_label_->setText(label);
        run_status_->setText(label);
    }

    void reset_contest_config_defaults() {
        compiler_ = "g++";
        compile_flags_ = "-std=c++17 -O2 -pipe";
        contestants_dir_ = "contestants";
        tests_dir_ = "tests";
        stack_limit_mb_ = 64;
        parallel_jobs_ = 0;
        keep_workdir_ = false;
    }

    fs::path ensure_ncontest_extension(fs::path path) const {
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (extension != ".ncontest") {
            path += ".ncontest";
        }
        return path;
    }

    bool confirm_discard_unsaved_file(bool close_after_save = false) {
        if (!contest_from_file_ || !contest_dirty_) {
            return true;
        }
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(text("unsaved_title"));
        box.setText(text("unsaved_message"));
        QPushButton* save_button = box.addButton(text("save"), QMessageBox::AcceptRole);
        QPushButton* cancel_button = box.addButton(text("cancel"), QMessageBox::RejectRole);
        QPushButton* close_anyway_button =
            box.addButton(text("close_anyway"), QMessageBox::DestructiveRole);
        close_anyway_button->setObjectName("DangerButton");
        close_anyway_button->setStyleSheet(
            "QPushButton#DangerButton {"
            "background: rgba(138, 35, 55, 220);"
            "border: 1px solid rgba(255, 121, 145, 180);"
            "color: white; border-radius: 8px; padding: 8px 12px; font-weight: 700;"
            "}"
            "QPushButton#DangerButton:hover {"
            "background: rgba(180, 47, 72, 235); border-color: #ff9aae;"
            "}");
        box.setDefaultButton(save_button);
        box.exec();

        QAbstractButton* clicked = box.clickedButton();
        if (clicked == close_anyway_button) {
            pending_close_after_save_ = false;
            return true;
        }
        if (clicked == save_button) {
            pending_close_after_save_ = close_after_save;
            if (!save_contest_container(false)) {
                pending_close_after_save_ = false;
            }
            return false;
        }
        if (clicked == cancel_button) {
            pending_close_after_save_ = false;
        }
        return false;
    }

    void mark_contest_dirty() {
        if (contest_from_file_) {
            contest_dirty_ = true;
        }
    }

    void cleanup_temp_root(const fs::path& root) {
        if (root.empty()) {
            return;
        }
        std::error_code ignored;
        fs::remove_all(root, ignored);
        temporary_roots_.erase(std::remove(temporary_roots_.begin(), temporary_roots_.end(), root),
                               temporary_roots_.end());
    }

    void cleanup_temporary_contests() {
        for (const auto& root : temporary_roots_) {
            std::error_code ignored;
            fs::remove_all(root, ignored);
        }
        temporary_roots_.clear();
        active_temp_root_.clear();
    }

    bool prepare_to_replace_contest() {
        if (!confirm_discard_unsaved_file()) {
            return false;
        }
        cleanup_temp_root(active_temp_root_);
        active_temp_root_.clear();
        contest_from_file_ = false;
        contest_dirty_ = false;
        contest_file_path_.clear();
        return true;
    }

    fs::path make_temporary_contest_root() {
        fs::create_directories(temporary_dir_);
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 1000; ++attempt) {
            fs::path root = temporary_dir_ /
                ("ncontest-" + std::to_string(now) + "-" + std::to_string(attempt));
            std::error_code ec;
            if (fs::create_directory(root, ec)) {
                temporary_roots_.push_back(root);
                return root;
            }
        }
        throw std::runtime_error("failed to create temporary contest folder");
    }

    fs::path detect_extracted_contest_root(const fs::path& root) const {
        if (fs::exists(root / "neothemis.conf")) {
            return root;
        }
        std::vector<fs::path> candidates;
        for (const auto& entry : fs::directory_iterator(root)) {
            if (entry.is_directory() && fs::exists(entry.path() / "neothemis.conf")) {
                candidates.push_back(entry.path());
            }
        }
        return candidates.size() == 1 ? candidates.front() : root;
    }

    void open_contest() {
        if (!archive_operation_available()) {
            return;
        }
        QString dir = QFileDialog::getExistingDirectory(this, text("open_folder"));
        if (dir.isEmpty()) {
            return;
        }
        if (!prepare_to_replace_contest()) {
            return;
        }
        contest_root_ = dir.toStdString();
        contest_from_file_ = false;
        contest_dirty_ = false;
        contest_file_path_.clear();
        contest_title_->setText(QString::fromStdString(contest_root_.filename().string()));
        load_contest_config();
        refresh_table();
    }

    void open_contest_file() {
        if (!archive_operation_available()) {
            return;
        }
        QString selected = QFileDialog::getOpenFileName(
            this, text("open_contest_file"), QString(),
            text("ncontest_open_filter"));
        if (selected.isEmpty()) {
            return;
        }
        if (!prepare_to_replace_contest()) {
            return;
        }

        fs::path temp_root;
        try {
            temp_root = make_temporary_contest_root();
            fs::path archive_path = selected.toStdString();
            begin_archive_operation(text("opening_contest_file"));
            ArchiveProgress progress = archive_progress_callback();
            archive_thread_ = std::thread([this, archive_path, temp_root, progress]() {
                try {
                    extract_zip_file(archive_path, temp_root, progress);
                    QMetaObject::invokeMethod(this, [this, archive_path, temp_root]() {
                        try {
                            contest_root_ = detect_extracted_contest_root(temp_root);
                            contest_file_path_ = archive_path;
                            active_temp_root_ = temp_root;
                            contest_from_file_ = true;
                            contest_dirty_ = false;
                            contest_title_->setText(QString::fromStdString(contest_file_path_.filename().string()));
                            load_contest_config();
                            refresh_table();
                            finish_archive_operation(text("file_operation_complete"));
                        } catch (const std::exception& ex) {
                            cleanup_temp_root(temp_root);
                            fail_archive_operation(text("file_operation_failed"));
                            QMessageBox::critical(this, text("open_failed"), ex.what());
                        }
                    }, Qt::QueuedConnection);
                } catch (const std::exception& ex) {
                    QMetaObject::invokeMethod(this, [this, temp_root, message = QString::fromUtf8(ex.what())]() {
                        cleanup_temp_root(temp_root);
                        fail_archive_operation(text("file_operation_failed"));
                        QMessageBox::critical(this, text("open_failed"), message);
                    }, Qt::QueuedConnection);
                }
            });
        } catch (const std::exception& ex) {
            cleanup_temp_root(temp_root);
            QMessageBox::critical(this, text("open_failed"), ex.what());
        }
    }

    void load_contest_config() {
        reset_contest_config_defaults();
        auto values = read_config_file(contest_root_ / "neothemis.conf");
        if (values.count("compiler")) compiler_ = values["compiler"];
        if (values.count("compile_flags")) compile_flags_ = values["compile_flags"];
        if (values.count("contestants_dir")) contestants_dir_ = values["contestants_dir"];
        if (values.count("tests_dir")) tests_dir_ = values["tests_dir"];
        if (values.count("stack_limit_mb")) stack_limit_mb_ = std::stoull(values["stack_limit_mb"]);
        if (values.count("parallel_jobs")) parallel_jobs_ = std::stoul(values["parallel_jobs"]);
        if (values.count("keep_workdir")) {
            std::string value = values["keep_workdir"];
            keep_workdir_ = value == "true" || value == "1" || value == "yes" || value == "on";
        }
    }

    void save_contest_config() const {
        if (contest_root_.empty()) {
            return;
        }
        std::ofstream out(contest_root_ / "neothemis.conf");
        if (!out) {
            throw std::runtime_error("failed to write neothemis.conf");
        }
        out << "core=builtin\n"
            << "contestants_dir=" << contestants_dir_ << '\n'
            << "tests_dir=" << tests_dir_ << '\n'
            << "output_csv=results.csv\n"
            << "scoreboard_csv=scoreboard.csv\n"
            << "keep_workdir=" << (keep_workdir_ ? "true" : "false") << '\n'
            << "compiler=" << compiler_ << '\n'
            << "compile_flags=" << compile_flags_ << '\n'
            << "stack_limit_mb=" << stack_limit_mb_ << '\n'
            << "parallel_jobs=" << parallel_jobs_ << '\n'
            << "forbidden_pattern=system(\n"
            << "forbidden_pattern=popen(\n"
            << "forbidden_pattern=fork(\n"
            << "forbidden_pattern=exec(\n"
            << "forbidden_pattern=#include <unistd.h>\n"
            << "forbidden_pattern=#include <sys/\n"
            << "forbidden_pattern=#include <windows.h>\n";
    }

    bool save_contest_container(bool save_as) {
        if (!archive_operation_available()) {
            return false;
        }
        if (contest_root_.empty()) {
            QMessageBox::information(this, text("contest"), text("no_contest_open"));
            return false;
        }

        try {
            save_contest_config();
            if (contest_from_file_) {
                fs::path target = contest_file_path_;
                if (save_as || target.empty()) {
                    QString selected = QFileDialog::getSaveFileName(
                        this, text("save_contest_file_as"),
                        QString::fromStdString((target.empty()
                            ? fs::path("contest.ncontest")
                            : target).string()),
                        text("ncontest_save_filter"));
                    if (selected.isEmpty()) {
                        return false;
                    }
                    target = ensure_ncontest_extension(selected.toStdString());
                }
                fs::path source_root = contest_root_;
                bool was_save_as = save_as;
                begin_archive_operation(text("saving_contest_file"));
                ArchiveProgress progress = archive_progress_callback();
                archive_thread_ = std::thread([this, source_root, target, was_save_as, progress]() {
                    try {
                        write_contest_archive(target, source_root, progress);
                        QMetaObject::invokeMethod(this, [this, target, was_save_as]() {
                            contest_file_path_ = target;
                            contest_dirty_ = false;
                            contest_title_->setText(QString::fromStdString(contest_file_path_.filename().string()));
                            finish_archive_operation(text("file_operation_complete"));
                            log_->appendPlainText(text(was_save_as ? "save_as_complete" : "save_complete") +
                                                  ": " + QString::fromStdString(target.string()));
                            if (pending_close_after_save_) {
                                pending_close_after_save_ = false;
                                QTimer::singleShot(0, this, &QWidget::close);
                            }
                        }, Qt::QueuedConnection);
                    } catch (const std::exception& ex) {
                        QMetaObject::invokeMethod(this, [this, message = QString::fromUtf8(ex.what())]() {
                            pending_close_after_save_ = false;
                            mark_contest_dirty();
                            fail_archive_operation(text("file_operation_failed"));
                            QMessageBox::critical(this, text("save_failed"), message);
                        }, Qt::QueuedConnection);
                    }
                });
            } else {
                fs::path default_target = contest_file_path_.empty()
                                              ? contest_root_.parent_path() /
                                                    (contest_root_.filename().string() + ".ncontest")
                                              : contest_file_path_;
                QString selected = QFileDialog::getSaveFileName(
                    this, text(save_as ? "save_contest_file_as" : "save_contest_file"),
                    QString::fromStdString(default_target.string()),
                    text("ncontest_save_filter"));
                if (selected.isEmpty()) {
                    return false;
                }
                fs::path source_root = contest_root_;
                fs::path target = ensure_ncontest_extension(selected.toStdString());
                bool was_save_as = save_as;
                begin_archive_operation(text("saving_contest_file"));
                ArchiveProgress progress = archive_progress_callback();
                archive_thread_ = std::thread([this, source_root, target, was_save_as, progress]() {
                    try {
                        write_contest_archive(target, source_root, progress);
                        QMetaObject::invokeMethod(this, [this, target, was_save_as]() {
                            contest_file_path_ = target;
                            contest_from_file_ = true;
                            contest_dirty_ = false;
                            contest_title_->setText(QString::fromStdString(contest_file_path_.filename().string()));
                            finish_archive_operation(text("file_operation_complete"));
                            log_->appendPlainText(text(was_save_as ? "save_as_complete" : "save_complete") +
                                                  ": " + QString::fromStdString(target.string()));
                            if (pending_close_after_save_) {
                                pending_close_after_save_ = false;
                                QTimer::singleShot(0, this, &QWidget::close);
                            }
                        }, Qt::QueuedConnection);
                    } catch (const std::exception& ex) {
                        QMetaObject::invokeMethod(this, [this, message = QString::fromUtf8(ex.what())]() {
                            pending_close_after_save_ = false;
                            fail_archive_operation(text("file_operation_failed"));
                            QMessageBox::critical(this, text("save_failed"), message);
                        }, Qt::QueuedConnection);
                    }
                });
            }
        } catch (const std::exception& ex) {
            pending_close_after_save_ = false;
            mark_contest_dirty();
            QMessageBox::critical(this, text("save_failed"), ex.what());
            return false;
        }
        return true;
    }

    void convert_old_contest_to_ncontest(bool source_is_folder) {
        if (!archive_operation_available()) {
            return;
        }

        QString source;
        if (source_is_folder) {
            source = QFileDialog::getExistingDirectory(
                this, text("convert_old_contest_folder"));
        } else {
            source = QFileDialog::getOpenFileName(
                this, text("convert_old_contest_file"), QString(),
                text("themis_contest_filter"));
        }
        if (source.isEmpty()) {
            return;
        }

        fs::path source_path = source.toStdString();
        fs::path default_output = source_path;
        if (source_is_folder) {
            default_output = source_path.parent_path() /
                             (source_path.filename().string() + ".ncontest");
        } else {
            default_output.replace_extension(".ncontest");
        }

        QString selected_output = QFileDialog::getSaveFileName(
            this, text("convert_old_contest_file"),
            QString::fromStdString(default_output.string()),
            text("ncontest_save_filter"));
        if (selected_output.isEmpty()) {
            return;
        }

        fs::path output_path = ensure_ncontest_extension(selected_output.toStdString());
        begin_archive_operation(text("converting_contest_file"));
        ArchiveProgress ui_progress = archive_progress_callback();
        neothemis::ArchiveProgress core_progress =
            [ui_progress](std::uint64_t done,
                          std::uint64_t total,
                          const std::string& label) {
                ui_progress(done, total, label.c_str());
            };

        archive_thread_ = std::thread([this, source_path, output_path, core_progress]() {
            try {
                neothemis::convert_old_themis_contest(source_path, output_path, core_progress);
                QMetaObject::invokeMethod(this, [this, output_path]() {
                    finish_archive_operation(text("convert_complete"));
                    log_->appendPlainText(text("convert_complete") + ": " +
                                          QString::fromStdString(output_path.string()));
                    QMessageBox::information(this, text("converter"),
                                             text("convert_complete") + "\n" +
                                             QString::fromStdString(output_path.string()));
                }, Qt::QueuedConnection);
            } catch (const std::exception& ex) {
                QMetaObject::invokeMethod(this, [this, message = QString::fromUtf8(ex.what())]() {
                    fail_archive_operation(text("file_operation_failed"));
                    QMessageBox::critical(this, text("file_operation_failed"), message);
                }, Qt::QueuedConnection);
            }
        });
    }

    void refresh_table() {
        if (contest_root_.empty()) {
            return;
        }
        try {
            auto overview = neothemis::inspect_contest(options_from_ui());
            contestants_ = overview.contestants;
            problems_ = overview.problems;
            source_ready_.clear();
            cell_texts_.clear();
            for (std::size_t row = 0; row < overview.contestants.size(); ++row) {
                for (std::size_t col = 0; col < overview.problems.size(); ++col) {
                    source_ready_[cell_key(overview.contestants[row], overview.problems[col])] =
                        overview.has_source[row][col];
                }
            }
            load_problem_test_counts();
            load_existing_results();
            populate_table();
            log_->appendPlainText(text("opened") + " " +
                                  QString::fromStdString(contest_root_.string()));
        } catch (const std::exception& ex) {
            QMessageBox::critical(this, text("open_failed"), ex.what());
        }
    }

    fs::path contest_output_path(const fs::path& path) const {
        return path.is_relative() ? contest_root_ / path : path;
    }

    void record_result(const neothemis::TestResult& result) {
        std::string key = cell_key(result.contestant, result.problem);
        result_details_[key].push_back(result);
        CellScore& score = score_cells_[key];
        score.earned += result.earned_points;
        score.max += result.max_points;
        ++score.completed;
    }

    std::vector<neothemis::TestResult> all_recorded_results() const {
        std::vector<neothemis::TestResult> rows;
        for (const auto& entry : result_details_) {
            rows.insert(rows.end(), entry.second.begin(), entry.second.end());
        }
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            return std::tie(a.contestant, a.problem, a.test) <
                   std::tie(b.contestant, b.problem, b.test);
        });
        return rows;
    }

    void load_existing_results() {
        score_cells_.clear();
        result_details_.clear();

        neothemis::JudgeOptions options = options_from_ui();
        fs::path details_path = contest_output_path(options.output_csv);
        if (!fs::exists(details_path)) {
            return;
        }

        std::set<std::string> known_contestants(contestants_.begin(), contestants_.end());
        std::set<std::string> known_problems(problems_.begin(), problems_.end());
        auto records = read_csv_records(details_path);
        if (records.size() <= 1) {
            return;
        }
        for (std::size_t i = 1; i < records.size(); ++i) {
            const auto& fields = records[i];
            if (fields.size() < 9 ||
                known_contestants.count(fields[0]) == 0 ||
                known_problems.count(fields[1]) == 0) {
                continue;
            }
            try {
                neothemis::TestResult result;
                result.contestant = fields[0];
                result.problem = fields[1];
                result.test = fields[2];
                result.verdict = verdict_from_string(fields[3]);
                result.time_ms = static_cast<std::uint64_t>(std::stoull(fields[4]));
                result.exit_code = std::stoi(fields[5]);
                result.max_points = std::stod(fields[6]);
                result.earned_points = std::stod(fields[7]);
                result.message = fields[8];
                record_result(result);
            } catch (...) {
                log_->appendPlainText(text("malformed_row_skipped"));
            }
        }
    }

    void write_current_csv_outputs() {
        neothemis::JudgeOptions options = options_from_ui();
        std::vector<neothemis::TestResult> rows = all_recorded_results();

        fs::path details_path = contest_output_path(options.output_csv);
        fs::create_directories(details_path.parent_path());
        std::ofstream details(details_path);
        if (!details) {
            throw std::runtime_error("failed to open CSV output: " + details_path.string());
        }
        neothemis::write_csv(details, rows);

        fs::path scoreboard_path = contest_output_path(options.scoreboard_csv);
        fs::create_directories(scoreboard_path.parent_path());
        std::ofstream scoreboard(scoreboard_path);
        if (!scoreboard) {
            throw std::runtime_error("failed to open scoreboard CSV output: " +
                                     scoreboard_path.string());
        }
        neothemis::write_scoreboard_csv(scoreboard, rows);
        mark_contest_dirty();
    }

    std::string status_for_scoreboard_cell(const std::string& contestant,
                                           const std::string& problem) const {
        auto found = result_details_.find(cell_key(contestant, problem));
        if (found == result_details_.end()) {
            return {};
        }
        std::string status;
        int priority = 0;
        for (const auto& result : found->second) {
            int candidate_priority = 0;
            std::string candidate;
            if (result.verdict == neothemis::Verdict::CompileError) {
                candidate = "CE";
                candidate_priority = 2;
            } else if (result.verdict == neothemis::Verdict::MissingSource) {
                candidate = "MS";
                candidate_priority = 1;
            }
            if (candidate_priority > priority) {
                status = candidate;
                priority = candidate_priority;
            }
        }
        return status;
    }

    fs::path choose_export_path(const std::string& filename) {
        QString default_path = contest_root_.empty()
                                   ? QString::fromStdString(filename)
                                   : QString::fromStdString((contest_root_ / filename).string());
        QString selected = QFileDialog::getSaveFileName(
            this, text("export_workbook"), default_path, text("xlsx_filter"));
        if (selected.isEmpty()) {
            return {};
        }
        fs::path path = selected.toStdString();
        if (path.extension().string() != ".xlsx") {
            path += ".xlsx";
        }
        return path;
    }

    bool export_is_available() {
        if (archive_running_.load()) {
            QMessageBox::information(this, text("archive_operation_running"),
                                     text("wait_for_archive_operation"));
            return false;
        }
        if (judging_.load()) {
            QMessageBox::information(this, text("judge_running"),
                                     text("wait_for_export_judge"));
            return false;
        }
        if (contest_root_.empty()) {
            QMessageBox::information(this, text("no_contest"),
                                     text("open_contest_first"));
            return false;
        }
        if (all_recorded_results().empty()) {
            QMessageBox::information(this, text("no_results"),
                                     text("no_results_detail"));
            return false;
        }
        return true;
    }

    void export_scoreboard_xlsx() {
        if (!export_is_available()) {
            return;
        }
        fs::path path = choose_export_path("scoreboard.xlsx");
        if (path.empty()) {
            return;
        }

        std::vector<XlsxRow> rows;
        XlsxRow header{xlsx_text("Contestant")};
        for (const auto& problem : problems_) {
            header.push_back(xlsx_text(problem));
        }
        header.push_back(xlsx_text("Total"));
        rows.push_back(std::move(header));

        for (const auto& contestant : contestants_) {
            XlsxRow row{xlsx_text(contestant)};
            for (const auto& problem : problems_) {
                double score = earned_for_problem(contestant, problem);
                std::string status = status_for_scoreboard_cell(contestant, problem);
                if (score == 0.0 && !status.empty()) {
                    row.push_back(xlsx_text(status + "(0)"));
                } else {
                    row.push_back(xlsx_number(score));
                }
            }
            row.push_back(xlsx_number(total_earned_for(contestant)));
            rows.push_back(std::move(row));
        }

        std::vector<double> widths(rows.front().size(), 14.0);
        widths[0] = 28.0;
        try {
            write_xlsx_file(path, "Scoreboard", rows, widths);
            log_->appendPlainText(text("exported") + " " + QString::fromStdString(path.string()));
            QMessageBox::information(this, text("export_complete"),
                                     text("exported") + " " + QString::fromStdString(path.string()));
        } catch (const std::exception& ex) {
            QMessageBox::critical(this, text("export_failed"), ex.what());
        }
    }

    void export_data_xlsx() {
        if (!export_is_available()) {
            return;
        }
        fs::path path = choose_export_path("results-data.xlsx");
        if (path.empty()) {
            return;
        }

        std::vector<XlsxRow> rows;
        rows.push_back({
            xlsx_text("contestant"),
            xlsx_text("problem"),
            xlsx_text("test"),
            xlsx_text("verdict"),
            xlsx_text("time_ms"),
            xlsx_text("exit_code"),
            xlsx_text("max_points"),
            xlsx_text("earned_points"),
            xlsx_text("message")
        });
        for (const auto& result : all_recorded_results()) {
            rows.push_back({
                xlsx_text(result.contestant),
                xlsx_text(result.problem),
                xlsx_text(result.test),
                xlsx_text(neothemis::to_string(result.verdict)),
                xlsx_number(static_cast<double>(result.time_ms)),
                xlsx_number(static_cast<double>(result.exit_code)),
                xlsx_number(result.max_points),
                xlsx_number(result.earned_points),
                xlsx_text(result.message)
            });
        }

        std::vector<double> widths{28.0, 14.0, 12.0, 10.0, 12.0, 12.0, 12.0, 14.0, 48.0};
        try {
            write_xlsx_file(path, "Data", rows, widths);
            log_->appendPlainText(text("exported") + " " + QString::fromStdString(path.string()));
            QMessageBox::information(this, text("export_complete"),
                                     text("exported") + " " + QString::fromStdString(path.string()));
        } catch (const std::exception& ex) {
            QMessageBox::critical(this, text("export_failed"), ex.what());
        }
    }

    void rebuild_maps() {
        contestant_rows_.clear();
        problem_columns_.clear();
        for (std::size_t i = 0; i < contestants_.size(); ++i) {
            contestant_rows_[contestants_[i]] = static_cast<int>(i);
        }
        for (std::size_t i = 0; i < problems_.size(); ++i) {
            problem_columns_[problems_[i]] = static_cast<int>(i + 1);
        }
    }

    int total_column() const {
        return static_cast<int>(problems_.size() + 1);
    }

    std::string cell_key(const std::string& contestant, const std::string& problem) const {
        return contestant + "\n" + problem;
    }

    double earned_for_problem(const std::string& contestant, const std::string& problem) const {
        auto it = score_cells_.find(cell_key(contestant, problem));
        return it == score_cells_.end() ? 0.0 : it->second.earned;
    }

    double max_for_problem(const std::string& contestant, const std::string& problem) const {
        auto it = score_cells_.find(cell_key(contestant, problem));
        return it == score_cells_.end() ? 0.0 : it->second.max;
    }

    double total_earned_for(const std::string& contestant) const {
        double total = 0.0;
        for (const auto& problem : problems_) {
            total += earned_for_problem(contestant, problem);
        }
        return total;
    }

    double total_max_for(const std::string& contestant) const {
        double total = 0.0;
        for (const auto& problem : problems_) {
            total += max_for_problem(contestant, problem);
        }
        return total;
    }

    QString problem_cell_text(const std::string& contestant, const std::string& problem) const {
        std::string key = cell_key(contestant, problem);
        auto text_it = cell_texts_.find(key);
        if (text_it != cell_texts_.end()) {
            return text_it->second;
        }

        auto score_it = score_cells_.find(key);
        if (score_it == score_cells_.end()) {
            auto source_it = source_ready_.find(key);
            return source_it != source_ready_.end() && source_it->second
                       ? text("ready")
                       : text("missing");
        }
        const CellScore& score = score_it->second;
        int expected = 1;
        auto expected_it = problem_test_counts_.find(problem);
        if (expected_it != problem_test_counts_.end()) {
            expected = expected_it->second;
        }
        QString status = score.completed >= expected ? text("done") : text("running");
        return QString("%1/%2\n%3 %4/%5")
            .arg(format_points(score.earned))
            .arg(format_points(score.max))
            .arg(status)
            .arg(score.completed)
            .arg(expected);
    }

    void populate_table() {
        rebuild_maps();
        table_->clear();
        table_->setRowCount(static_cast<int>(contestants_.size()));
        table_->setColumnCount(total_column() + 1);
        table_->setHorizontalHeaderItem(0, new QTableWidgetItem(text("contestant")));
        for (std::size_t col = 0; col < problems_.size(); ++col) {
            table_->setHorizontalHeaderItem(static_cast<int>(col + 1),
                                            new QTableWidgetItem(QString::fromStdString(problems_[col])));
        }
        table_->setHorizontalHeaderItem(total_column(), new QTableWidgetItem(text("total")));

        for (std::size_t row = 0; row < contestants_.size(); ++row) {
            const std::string& contestant = contestants_[row];
            auto* name_item = new QTableWidgetItem(QString::fromStdString(contestant));
            style_name_item(name_item);
            table_->setItem(static_cast<int>(row), 0, name_item);
            for (std::size_t col = 0; col < problems_.size(); ++col) {
                auto* item = new QTableWidgetItem(problem_cell_text(contestant, problems_[col]));
                item->setTextAlignment(Qt::AlignCenter);
                style_problem_item(contestant, problems_[col], item);
                table_->setItem(static_cast<int>(row), static_cast<int>(col + 1), item);
            }
            update_total_cell(contestant);
        }
    }

    void load_problem_test_counts() {
        problem_test_counts_.clear();
        fs::path tests_root = contest_root_ / tests_dir_;
        for (const auto& problem : problems_) {
            int count = 0;
            fs::path problem_root = tests_root / problem;
            if (fs::exists(problem_root)) {
                for (const auto& entry : fs::directory_iterator(problem_root)) {
                    if (entry.is_directory()) {
                        ++count;
                    }
                }
            }
            problem_test_counts_[problem] = std::max(1, count);
        }
    }

    std::vector<std::string> selected_contestants() const {
        std::set<int> rows;
        for (const QModelIndex& index : table_->selectionModel()->selectedRows()) {
            rows.insert(index.row());
        }
        std::vector<std::string> selected;
        for (int row : rows) {
            if (row >= 0 && static_cast<std::size_t>(row) < contestants_.size()) {
                selected.push_back(contestants_[static_cast<std::size_t>(row)]);
            }
        }
        return selected;
    }

    void reset_run_cells(const std::vector<std::string>& selected, const std::string& selected_problem) {
        std::set<std::string> selected_set(selected.begin(), selected.end());
        for (const auto& contestant : contestants_) {
            if (!selected_set.empty() && selected_set.count(contestant) == 0) {
                continue;
            }
            for (const auto& problem : problems_) {
                if (!selected_problem.empty() && problem != selected_problem) {
                    continue;
                }
                std::string key = cell_key(contestant, problem);
                score_cells_.erase(key);
                result_details_.erase(key);
                cell_texts_.erase(key);
                set_table_cell(contestant, problem, text("queued"));
            }
            update_total_cell(contestant);
        }
    }

    void set_table_cell(const std::string& contestant,
                        const std::string& problem,
                        const QString& text) {
        auto row_it = contestant_rows_.find(contestant);
        auto col_it = problem_columns_.find(problem);
        if (row_it == contestant_rows_.end() || col_it == problem_columns_.end()) {
            return;
        }
        auto* item = table_->item(row_it->second, col_it->second);
        if (!item) {
            item = new QTableWidgetItem;
            table_->setItem(row_it->second, col_it->second, item);
        }
        cell_texts_[cell_key(contestant, problem)] = text;
        item->setText(text);
        item->setTextAlignment(Qt::AlignCenter);
        style_problem_item(contestant, problem, item);
    }

    void update_total_cell(const std::string& contestant) {
        auto row_it = contestant_rows_.find(contestant);
        if (row_it == contestant_rows_.end()) {
            return;
        }
        auto* item = table_->item(row_it->second, total_column());
        if (!item) {
            item = new QTableWidgetItem;
            table_->setItem(row_it->second, total_column(), item);
        }
        item->setText(format_points(total_earned_for(contestant)) + "/" +
                      format_points(total_max_for(contestant)));
        item->setTextAlignment(Qt::AlignCenter);
        style_total_item(contestant, item);
    }

    void style_name_item(QTableWidgetItem* item) const {
        if (!item) {
            return;
        }
        item->setForeground(QColor("#e7fbff"));
        item->setBackground(QColor("#151b23"));
    }

    void style_problem_item(const std::string& contestant,
                            const std::string& problem,
                            QTableWidgetItem* item) const {
        if (!item) {
            return;
        }
        std::string key = cell_key(contestant, problem);
        auto score_it = score_cells_.find(key);
        if (score_it != score_cells_.end()) {
            const CellScore& score = score_it->second;
            int expected = 1;
            auto expected_it = problem_test_counts_.find(problem);
            if (expected_it != problem_test_counts_.end()) {
                expected = expected_it->second;
            }
            if (score.completed < expected) {
                item->setForeground(QColor("#7df9ff"));
                item->setBackground(QColor("#122631"));
            } else if (score.max > 0.0 && score.earned + 1e-9 >= score.max) {
                item->setForeground(QColor("#99ffcc"));
                item->setBackground(QColor("#123028"));
            } else if (score.earned > 0.0) {
                item->setForeground(QColor("#ffe680"));
                item->setBackground(QColor("#302512"));
            } else {
                item->setForeground(QColor("#ff8fab"));
                item->setBackground(QColor("#30151f"));
            }
            return;
        }

        QString text = item->text().toLower();
        if (text.contains(this->text("queued").toLower()) || text.contains("queued")) {
            item->setForeground(QColor("#ffe680"));
            item->setBackground(QColor("#2a2412"));
            return;
        }
        auto source_it = source_ready_.find(key);
        if (source_it != source_ready_.end() && source_it->second) {
            item->setForeground(QColor("#7df9ff"));
            item->setBackground(QColor("#122631"));
        } else {
            item->setForeground(QColor("#ff8fab"));
            item->setBackground(QColor("#281821"));
        }
    }

    void style_total_item(const std::string& contestant, QTableWidgetItem* item) const {
        if (!item) {
            return;
        }
        double earned = total_earned_for(contestant);
        double max = total_max_for(contestant);
        if (max > 0.0 && earned + 1e-9 >= max) {
            item->setForeground(QColor("#99ffcc"));
            item->setBackground(QColor("#102a24"));
        } else if (earned > 0.0) {
            item->setForeground(QColor("#ffe680"));
            item->setBackground(QColor("#2c2312"));
        } else {
            item->setForeground(QColor("#7df9ff"));
            item->setBackground(QColor("#121f2a"));
        }
    }

    void handle_result(const neothemis::TestResult& result) {
        record_result(result);
        const CellScore& score = score_cells_[cell_key(result.contestant, result.problem)];

        int expected = problem_test_counts_[result.problem];
        QString status = score.completed >= expected ? text("done") : text("running");
        QString text = QString("%1/%2\n%3 %4/%5")
                           .arg(format_points(score.earned))
                           .arg(format_points(score.max))
                           .arg(status)
                           .arg(score.completed)
                           .arg(expected);
        set_table_cell(result.contestant, result.problem, text);
        update_total_cell(result.contestant);
    }

    void show_result_details(int row, int col) {
        if (row < 0 || col <= 0 ||
            static_cast<std::size_t>(row) >= contestants_.size() ||
            static_cast<std::size_t>(col - 1) >= problems_.size()) {
            return;
        }

        std::string contestant = contestants_[static_cast<std::size_t>(row)];
        std::string problem = problems_[static_cast<std::size_t>(col - 1)];
        std::string key = contestant + "\n" + problem;
        std::vector<neothemis::TestResult> rows = result_details_[key];
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            return a.test < b.test;
        });

        QString text;
        if (rows.empty()) {
            text = "No judged tests for this cell yet.";
        } else {
            for (const auto& result : rows) {
                QString description = QString::fromStdString(neothemis::to_string(result.verdict));
                if (!result.message.empty()) {
                    description += ": " + QString::fromStdString(result.message);
                }
                text += QString::fromStdString(result.test) + ": " +
                        format_points(result.earned_points) + "/" +
                        format_points(result.max_points) + " Point\n";
                text += "Description: " + description + "\n\n";
            }
        }

        auto* dialog = new QDialog(this);
        dialog->setWindowTitle(QString::fromStdString(contestant + " - " + problem));
        dialog->resize(640, 520);
        auto* layout = new QVBoxLayout(dialog);
        auto* title = new QLabel(QString::fromStdString(contestant + " / " + problem), dialog);
        title->setObjectName("AppTitle");
        auto* details = new QPlainTextEdit(dialog);
        details->setReadOnly(true);
        details->setPlainText(text);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
        layout->addWidget(title);
        layout->addWidget(details, 1);
        layout->addWidget(buttons);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    }

    void sort_by_column(int section) {
        if (section < 0 || section > total_column()) {
            return;
        }
        if (sort_column_ == section) {
            sort_ascending_ = !sort_ascending_;
        } else {
            sort_column_ = section;
            sort_ascending_ = true;
        }

        auto name_less = [](const std::string& a, const std::string& b) {
            return QString::fromStdString(a).toCaseFolded() <
                   QString::fromStdString(b).toCaseFolded();
        };

        std::stable_sort(contestants_.begin(), contestants_.end(),
                         [&](const std::string& a, const std::string& b) {
            int cmp = 0;
            if (section == 0) {
                cmp = name_less(a, b) ? -1 : (name_less(b, a) ? 1 : 0);
            } else if (section == total_column()) {
                double av = total_earned_for(a);
                double bv = total_earned_for(b);
                cmp = av < bv ? -1 : (av > bv ? 1 : 0);
            } else {
                std::string problem = problems_[static_cast<std::size_t>(section - 1)];
                double av = earned_for_problem(a, problem);
                double bv = earned_for_problem(b, problem);
                cmp = av < bv ? -1 : (av > bv ? 1 : 0);
            }
            if (cmp == 0) {
                cmp = name_less(a, b) ? -1 : (name_less(b, a) ? 1 : 0);
            }
            return sort_ascending_ ? cmp < 0 : cmp > 0;
        });
        populate_table();
    }

    void show_header_menu(const QPoint& pos) {
        int section = table_->horizontalHeader()->logicalIndexAt(pos);
        if (section <= 0 || section > static_cast<int>(problems_.size())) {
            return;
        }
        std::string problem = problems_[static_cast<std::size_t>(section - 1)];
        QMenu menu(this);
        menu.addAction("Judge this problem for selected contestants",
                       [this, problem]() { start_judge(true, problem); });
        menu.addAction("Judge this problem for all contestants",
                       [this, problem]() { start_judge(false, problem); });
        menu.exec(table_->horizontalHeader()->mapToGlobal(pos));
    }

    void handle_progress_line(const std::string& raw) {
        QString line = QString::fromStdString(raw);
        QStringList parts = line.split(' ', Qt::SkipEmptyParts);
        if (parts.size() < 4 || parts.value(0) != "progress") {
            log_->appendPlainText(line);
            return;
        }
        QString counts;
        for (const QString& part : parts) {
            if (part.contains('/')) {
                counts = part;
                break;
            }
        }
        QStringList split = counts.split('/');
        if (split.size() != 2) {
            return;
        }
        bool done_ok = false;
        bool total_ok = false;
        int done = split[0].toInt(&done_ok);
        int total = split[1].toInt(&total_ok);
        if (!done_ok || !total_ok || total <= 0) {
            return;
        }
        progress_->setRange(0, total);
        progress_->setValue(done);
        QString phase = parts.value(1);
        QString elapsed;
        int elapsed_index = parts.indexOf("elapsed");
        if (elapsed_index >= 0 && elapsed_index + 1 < parts.size()) {
            elapsed = parts.value(elapsed_index + 1);
        }
        progress_label_->setText(phase + " " + QString::number(done) + "/" +
                                 QString::number(total) +
                                 (elapsed.isEmpty() ? QString() : " elapsed " + elapsed));
        run_status_->setText(line);
    }

    void start_judge(bool selected_only, const std::string& selected_problem = {}) {
        if (archive_running_.load()) {
            QMessageBox::information(this, text("archive_operation_running"),
                                     text("wait_for_archive_operation"));
            return;
        }
        if (judging_.load()) {
            run_status_->setText(text("judge_already_active"));
            return;
        }
        if (contest_root_.empty()) {
            QMessageBox::information(this, text("no_contest"), text("open_contest_first"));
            return;
        }
        neothemis::JudgeOptions options = options_from_ui();
        if (!selected_problem.empty()) {
            options.selected_problems.push_back(selected_problem);
        }
        std::vector<std::string> selected;
        if (selected_only) {
            selected = selected_contestants();
            if (selected.empty()) {
                QMessageBox::information(this, text("no_selection"), text("select_contestant_rows"));
                return;
            }
            options.selected_contestants = selected;
        }
        if (judge_thread_.joinable()) {
            judge_thread_.join();
        }
        judging_.store(true);
        cancel_requested_.store(false);
        set_judge_controls_enabled(false);
        reset_run_cells(selected, selected_problem);
        progress_->setRange(0, 0);
        progress_label_->setText(text("starting"));
        QString scope = selected_only ? text("selected_contestants") : text("all_contestants");
        run_status_->setText(selected_problem.empty()
                                 ? text("judging") + " " + scope
                                 : text("judging") + " " + QString::fromStdString(selected_problem) +
                                       " - " + scope);
        log_->appendPlainText(text("judging_log"));

        judge_thread_ = std::thread([this, options]() mutable {
            try {
                options.should_cancel = [this]() {
                    return cancel_requested_.load();
                };
                options.progress = [this](const std::string& line) {
                    QMetaObject::invokeMethod(this, [this, line]() {
                        handle_progress_line(line);
                    }, Qt::QueuedConnection);
                };
                options.result = [this](const neothemis::TestResult& result) {
                    QMetaObject::invokeMethod(this, [this, result]() {
                        handle_result(result);
                    }, Qt::QueuedConnection);
                };

                auto core = neothemis::make_judge_core(options.core_name);
                core->judge(options);
                QMetaObject::invokeMethod(this, [this]() {
                    try {
                        write_current_csv_outputs();
                        progress_->setRange(0, 100);
                        progress_->setValue(100);
                        progress_label_->setText(text("done"));
                        run_status_->setText(text("judge_run_complete"));
                        log_->appendPlainText(text("done") + ".");
                    } catch (const std::exception& ex) {
                        progress_->setRange(0, 100);
                        progress_->setValue(0);
                        progress_label_->setText(text("failed"));
                        run_status_->setText(text("judge_csv_failed"));
                        log_->appendPlainText(text("csv_write_failed") + ": " +
                                              QString::fromUtf8(ex.what()));
                        QMessageBox::critical(this, text("csv_write_failed"), ex.what());
                    }
                    judging_.store(false);
                    set_judge_controls_enabled(true);
                }, Qt::QueuedConnection);
            } catch (const std::exception& ex) {
                bool cancelled = cancel_requested_.load();
                QMetaObject::invokeMethod(this, [this, message = QString::fromUtf8(ex.what()), cancelled]() {
                    progress_->setRange(0, 100);
                    progress_->setValue(0);
                    progress_label_->setText(cancelled ? text("cancelled") : text("failed"));
                    run_status_->setText(cancelled ? text("judge_run_cancelled") : text("judge_run_failed"));
                    log_->appendPlainText(cancelled ? text("cancelled") + "."
                                                    : text("failed") + ": " + message);
                    judging_.store(false);
                    set_judge_controls_enabled(true);
                    if (!cancelled) {
                        QMessageBox::critical(this, text("judge_failed"), message);
                    }
                }, Qt::QueuedConnection);
            }
        });
    }

    void set_judge_controls_enabled(bool enabled) {
        if (judge_selected_button_) {
            judge_selected_button_->setEnabled(enabled);
        }
        if (judge_all_button_) {
            judge_all_button_->setEnabled(enabled);
        }
        if (judge_selected_action_) {
            judge_selected_action_->setEnabled(enabled);
        }
        if (judge_all_action_) {
            judge_all_action_->setEnabled(enabled);
        }
        if (stop_button_) {
            stop_button_->setEnabled(!enabled);
        }
        if (stop_action_) {
            stop_action_->setEnabled(!enabled);
        }
    }

    void request_stop_judge() {
        if (!judging_.load()) {
            return;
        }
        cancel_requested_.store(true);
        run_status_->setText(text("stopping_active_run"));
        progress_label_->setText(text("stopping"));
        if (stop_button_) {
            stop_button_->setEnabled(false);
        }
        if (stop_action_) {
            stop_action_->setEnabled(false);
        }
    }

    void stop_active_judge() {
        cancel_requested_.store(true);
        if (judge_thread_.joinable()) {
            judge_thread_.join();
        }
        judging_.store(false);
    }

    QWidget* build_visual_tab(QWidget* parent) {
        auto* tab = new QWidget(parent);
        auto* form = new QFormLayout(tab);
        auto* theme = new QComboBox(tab);
        theme->addItem(text("dark"));
        auto* language = new QComboBox(tab);
        language->addItem(text("english"), "en");
        language->addItem(text("vietnamese"), "vi");
        int language_index = language->findData(QString::fromStdString(language_));
        if (language_index >= 0) {
            language->setCurrentIndex(language_index);
        }
        auto* temp_widget = new QWidget(tab);
        auto* temp_layout = new QHBoxLayout(temp_widget);
        temp_layout->setContentsMargins(0, 0, 0, 0);
        auto* temp_dir = new QLineEdit(QString::fromStdString(temporary_dir_.string()), temp_widget);
        auto* browse_temp = new QPushButton(text("browse"), temp_widget);
        temp_layout->addWidget(temp_dir, 1);
        temp_layout->addWidget(browse_temp);
        auto* save = new QPushButton(text("save_application_settings"), tab);

        form->addRow(text("theme"), theme);
        form->addRow(text("language"), language);
        form->addRow(text("temporary_dir"), temp_widget);
        form->addRow(save);

        QObject::connect(browse_temp, &QPushButton::clicked, [this, temp_dir]() {
            QString dir = QFileDialog::getExistingDirectory(
                this, text("temporary_dir"), temp_dir->text());
            if (!dir.isEmpty()) {
                temp_dir->setText(dir);
            }
        });

        QObject::connect(save, &QPushButton::clicked, [this, language, temp_dir]() {
            try {
                language_ = language->currentData().toString().toStdString();
                temporary_dir_ = temp_dir->text().toStdString();
                if (temporary_dir_.empty()) {
                    temporary_dir_ = default_temporary_dir();
                }
                fs::create_directories(temporary_dir_);
                save_app_settings();
                apply_language_to_main_window();
                log_->appendPlainText(text("saved_app_settings"));
            } catch (const std::exception& ex) {
                QMessageBox::critical(this, text("save_failed"), ex.what());
            }
        });
        return tab;
    }

    QWidget* build_contest_tab(QWidget* parent) {
        auto* tab = new QWidget(parent);
        auto* form = new QFormLayout(tab);
        auto* compiler = new QLineEdit(QString::fromStdString(compiler_), tab);
        auto* flags = new QLineEdit(QString::fromStdString(compile_flags_), tab);
        auto* contestants = new QLineEdit(QString::fromStdString(contestants_dir_), tab);
        auto* tests = new QLineEdit(QString::fromStdString(tests_dir_), tab);
        auto* stack = new QSpinBox(tab);
        stack->setRange(0, 1024 * 1024);
        stack->setValue(static_cast<int>(stack_limit_mb_));
        auto* parallel = new QSpinBox(tab);
        parallel->setRange(0, 256);
        parallel->setValue(static_cast<int>(parallel_jobs_));
        auto* keep = new QCheckBox(tab);
        keep->setChecked(keep_workdir_);
        auto* save = new QPushButton(text("save_contest_config"), tab);

        form->addRow(text("compiler"), compiler);
        form->addRow(text("compile_flags"), flags);
        form->addRow(text("contestants_dir"), contestants);
        form->addRow(text("tests_dir"), tests);
        form->addRow(text("stack_mb"), stack);
        form->addRow(text("parallel_jobs"), parallel);
        form->addRow(text("keep_workdir"), keep);
        form->addRow(save);

        QObject::connect(save, &QPushButton::clicked, [this, compiler, flags,
                                                       contestants, tests, stack, parallel, keep]() {
            compiler_ = compiler->text().toStdString();
            compile_flags_ = flags->text().toStdString();
            contestants_dir_ = contestants->text().toStdString();
            tests_dir_ = tests->text().toStdString();
            stack_limit_mb_ = static_cast<std::uint64_t>(stack->value());
            parallel_jobs_ = static_cast<unsigned int>(parallel->value());
            keep_workdir_ = keep->isChecked();
            try {
                save_contest_config();
                mark_contest_dirty();
                refresh_table();
                log_->appendPlainText(text("saved_contest_config"));
            } catch (const std::exception& ex) {
                QMessageBox::critical(this, text("save_failed"), ex.what());
            }
        });
        return tab;
    }

    QWidget* build_problem_tab(QWidget* parent) {
        auto* tab = new QWidget(parent);
        auto* form = new QFormLayout(tab);
        auto* problem = new QComboBox(tab);
        for (const auto& name : problems_) {
            problem->addItem(QString::fromStdString(name));
        }
        auto* time = new QSpinBox(tab);
        time->setRange(1, 60 * 60 * 1000);
        auto* memory = new QSpinBox(tab);
        memory->setRange(0, 1024 * 1024);
        auto* points = new QLineEdit(tab);
        auto* checker = new QLineEdit(tab);
        auto* test_table = new QTableWidget(tab);
        test_table->setColumnCount(2);
        test_table->setHorizontalHeaderItem(0, new QTableWidgetItem(text("test")));
        test_table->setHorizontalHeaderItem(1, new QTableWidgetItem(text("point_override")));
        test_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        test_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        test_table->verticalHeader()->setVisible(false);
        test_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        test_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
        test_table->setMinimumHeight(220);

        auto* batch_widget = new QWidget(tab);
        auto* batch_layout = new QHBoxLayout(batch_widget);
        batch_layout->setContentsMargins(0, 0, 0, 0);
        auto* batch_points = new QLineEdit(batch_widget);
        auto* apply_batch = new QPushButton(text("apply_to_selected"), batch_widget);
        batch_layout->addWidget(batch_points, 1);
        batch_layout->addWidget(apply_batch);

        auto* save = new QPushButton(text("save_problem_config"), tab);

        auto load_problem = [this, problem, time, memory, points, checker, test_table]() {
            std::string name = problem->currentText().toStdString();
            if (name.empty()) {
                test_table->setRowCount(0);
                return;
            }
            auto values = read_config_file(contest_root_ / tests_dir_ / name / "problem.conf");
            time->setValue(values.count("time_limit_ms") ? std::stoi(values["time_limit_ms"]) : 1000);
            memory->setValue(values.count("memory_limit_mb") ? std::stoi(values["memory_limit_mb"]) : 256);
            points->setText(QString::fromStdString(values.count("default_points") ? values["default_points"] : "1"));
            checker->setText(QString::fromStdString(values.count("checker") ? values["checker"] : "token"));

            std::vector<std::string> tests = test_names_for_problem(contest_root_ / tests_dir_ / name);
            test_table->setRowCount(static_cast<int>(tests.size()));
            for (std::size_t row = 0; row < tests.size(); ++row) {
                const std::string& test_name = tests[row];
                auto* test_item = new QTableWidgetItem(QString::fromStdString(test_name));
                test_item->setFlags(test_item->flags() & ~Qt::ItemIsEditable);
                test_table->setItem(static_cast<int>(row), 0, test_item);

                std::string override_points;
                for (const auto& key : test_point_keys(test_name)) {
                    auto found = values.find("test_points." + key);
                    if (found != values.end()) {
                        override_points = found->second;
                        break;
                    }
                }
                test_table->setItem(static_cast<int>(row), 1,
                                    new QTableWidgetItem(QString::fromStdString(override_points)));
            }
        };
        QObject::connect(problem, &QComboBox::currentTextChanged, [load_problem]() { load_problem(); });

        QObject::connect(apply_batch, &QPushButton::clicked, [test_table, batch_points]() {
            std::set<int> rows;
            for (const QModelIndex& index : test_table->selectionModel()->selectedRows()) {
                rows.insert(index.row());
            }
            for (int row : rows) {
                auto* item = test_table->item(row, 1);
                if (!item) {
                    item = new QTableWidgetItem;
                    test_table->setItem(row, 1, item);
                }
                item->setText(batch_points->text());
            }
        });

        QObject::connect(save, &QPushButton::clicked, [this, problem, time, memory, points, checker, test_table]() {
            try {
                std::string name = problem->currentText().toStdString();
                if (name.empty()) {
                    return;
                }
                std::string default_points = trim(points->text().toStdString());
                auto ensure_points = [&](const std::string& value) {
                    if (value.empty()) {
                        return;
                    }
                    std::size_t parsed = 0;
                    (void)std::stod(value, &parsed);
                    if (parsed != value.size()) {
                        throw std::runtime_error(text("invalid_points_detail").toStdString());
                    }
                };
                ensure_points(default_points);

                std::vector<std::pair<std::string, std::string>> overrides;
                for (int row = 0; row < test_table->rowCount(); ++row) {
                    auto* test_item = test_table->item(row, 0);
                    auto* point_item = test_table->item(row, 1);
                    if (!test_item) {
                        continue;
                    }
                    std::string value = point_item ? trim(point_item->text().toStdString()) : std::string();
                    ensure_points(value);
                    if (!value.empty()) {
                        overrides.push_back({test_item->text().toStdString(), value});
                    }
                }

                fs::create_directories(contest_root_ / tests_dir_ / name);
                write_problem_config(contest_root_ / tests_dir_ / name / "problem.conf",
                                     time->value(), memory->value(),
                                     default_points.empty() ? std::string("1") : default_points,
                                     checker->text().toStdString(), overrides);
                mark_contest_dirty();
                log_->appendPlainText(text("saved_problem_config") + problem->currentText());
            } catch (const std::exception& ex) {
                QMessageBox::critical(this, text("save_failed"), ex.what());
            }
        });

        form->addRow(text("problem"), problem);
        form->addRow(text("time_limit_ms"), time);
        form->addRow(text("memory_mb"), memory);
        form->addRow(text("default_points"), points);
        form->addRow(text("checker"), checker);
        form->addRow(text("test_points"), test_table);
        form->addRow(text("selected_point"), batch_widget);
        form->addRow(save);
        if (problem->count() > 0) {
            load_problem();
        }
        return tab;
    }

    void open_settings_dialog(int initial_tab) {
        auto* dialog = new QDialog(this);
        dialog->setWindowTitle(text("settings_title"));
        dialog->resize(760, 640);
        auto* layout = new QVBoxLayout(dialog);
        auto* tabs = new QTabWidget(dialog);
        tabs->addTab(build_visual_tab(tabs), text("application"));
        tabs->addTab(build_contest_tab(tabs), text("contest"));
        tabs->addTab(build_problem_tab(tabs), text("problems"));
        tabs->setCurrentIndex(initial_tab);
        layout->addWidget(tabs);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
        layout->addWidget(buttons);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    }

    void show_about_dialog() {
        auto* dialog = new QDialog(this);
        dialog->setWindowTitle(text("about_title"));
        dialog->resize(520, 360);
        auto* layout = new QVBoxLayout(dialog);

        auto* title = new QLabel("NeoThemis", dialog);
        title->setObjectName("WindowAppName");
        auto* details = new QPlainTextEdit(dialog);
        details->setReadOnly(true);
        details->setPlainText(text("about_details"));
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);

        layout->addWidget(title);
        layout->addWidget(details, 1);
        layout->addWidget(buttons);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    }

    fs::path contest_root_;
    std::vector<std::string> contestants_;
    std::vector<std::string> problems_;
    std::map<std::string, int> contestant_rows_;
    std::map<std::string, int> problem_columns_;
    std::map<std::string, int> problem_test_counts_;
    std::map<std::string, bool> source_ready_;
    std::map<std::string, CellScore> score_cells_;
    std::map<std::string, QString> cell_texts_;
    std::map<std::string, std::vector<neothemis::TestResult>> result_details_;
    int sort_column_ = 0;
    bool sort_ascending_ = true;

    std::string compiler_ = "g++";
    std::string compile_flags_ = "-std=c++17 -O2 -pipe";
    std::string contestants_dir_ = "contestants";
    std::string tests_dir_ = "tests";
    std::string language_ = "en";
    fs::path temporary_dir_;
    fs::path contest_file_path_;
    fs::path active_temp_root_;
    std::vector<fs::path> temporary_roots_;
    std::uint64_t stack_limit_mb_ = 64;
    unsigned int parallel_jobs_ = 0;
    bool keep_workdir_ = false;
    bool contest_from_file_ = false;
    bool contest_dirty_ = false;
    bool pending_close_after_save_ = false;
    std::atomic_bool judging_{false};
    std::atomic_bool archive_running_{false};
    std::atomic_bool cancel_requested_{false};
    std::thread judge_thread_;
    std::thread archive_thread_;

    QTableWidget* table_ = nullptr;
    QWidget* title_bar_ = nullptr;
    QGroupBox* judge_group_ = nullptr;
    QMenuBar* menu_bar_ = nullptr;
    QLabel* contest_title_ = nullptr;
    QPushButton* judge_selected_button_ = nullptr;
    QPushButton* judge_all_button_ = nullptr;
    QPushButton* stop_button_ = nullptr;
    QAction* judge_selected_action_ = nullptr;
    QAction* judge_all_action_ = nullptr;
    QAction* stop_action_ = nullptr;
    QLabel* run_status_ = nullptr;
    QLabel* progress_label_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QPoint drag_offset_;
    bool dragging_title_bar_ = false;
};

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setWindowIcon(QIcon(":/materials/logo.png"));
    MainWindow window;
    window.show();
    return app.exec();
}
