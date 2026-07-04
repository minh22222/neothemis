#include "FileFormats.hpp"

#include "neothemis/ContestArchive.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <utility>

#ifdef NEOTHEMIS_HAS_ZLIB
#include <zlib.h>
#endif

namespace fs = std::filesystem;

namespace neothemis::gui {

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

struct ZipEntry {
    std::string name;
    std::string data;
    std::uint32_t crc = 0;
    std::uint32_t offset = 0;
    std::uint16_t method = 0;
    std::uint32_t uncompressed_size = 0;
};

void report_archive_progress(const ArchiveProgress& progress,
                             std::uint64_t done,
                             std::uint64_t total,
                             const char* label_key) {
    if (progress) {
        progress(done, total, label_key);
    }
}

neothemis::ArchiveProgress core_archive_progress(const ArchiveProgress& progress) {
    if (!progress) {
        return {};
    }
    return [progress](std::uint64_t done,
                      std::uint64_t total,
                      const std::string& label) {
        progress(done, total, label.c_str());
    };
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
                      const ArchiveProgress& progress) {
    neothemis::extract_zip_archive(archive_path, destination,
                                   core_archive_progress(progress));
}

void write_contest_archive(const fs::path& archive_path,
                           const fs::path& contest_root,
                           const ArchiveProgress& progress) {
    neothemis::write_zip_archive_from_directory(
        archive_path, contest_root, core_archive_progress(progress));
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

} // namespace neothemis::gui
