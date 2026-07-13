#include "neothemis/Spreadsheet.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;

namespace neothemis {
namespace {

class ScopedDirectory {
public:
    explicit ScopedDirectory(fs::path path) : path_(std::move(path)) {}

    ScopedDirectory(const ScopedDirectory&) = delete;
    ScopedDirectory& operator=(const ScopedDirectory&) = delete;

    ~ScopedDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

private:
    fs::path path_;
};

void write_text_file(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to write " + path.string());
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output) {
        throw std::runtime_error("failed to write " + path.string());
    }
}

std::string xml_escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const unsigned char ch : value) {
        switch (ch) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            case '\'': escaped += "&apos;"; break;
            default:
                if ((ch < 0x20 && ch != '\n' && ch != '\r' && ch != '\t') || ch == 0x7f) {
                    escaped.push_back(' ');
                } else {
                    escaped.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return escaped;
}

std::string column_name(std::size_t index) {
    std::string name;
    ++index;
    while (index > 0) {
        const std::size_t remainder = (index - 1) % 26;
        name.push_back(static_cast<char>('A' + remainder));
        index = (index - 1) / 26;
    }
    std::reverse(name.begin(), name.end());
    return name;
}

std::string number_text(double value) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(15) << value;
    return output.str();
}

std::string worksheet_xml(const XlsxSheet& sheet) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
           << R"(<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">)";

    if (sheet.freeze_first_row) {
        output << R"(<sheetViews><sheetView workbookViewId="0">)";
        if (!sheet.rows.empty()) {
            output << R"(<pane ySplit="1" topLeftCell="A2" activePane="bottomLeft" state="frozen"/>)";
        }
        output << "</sheetView></sheetViews>";
    }

    if (!sheet.column_widths.empty()) {
        output << "<cols>";
        for (std::size_t index = 0; index < sheet.column_widths.size(); ++index) {
            output << "<col min=\"" << (index + 1) << "\" max=\"" << (index + 1)
                   << "\" width=\"" << sheet.column_widths[index]
                   << "\" customWidth=\"1\"/>";
        }
        output << "</cols>";
    }

    output << "<sheetData>";
    for (std::size_t row_index = 0; row_index < sheet.rows.size(); ++row_index) {
        output << "<row r=\"" << (row_index + 1) << "\">";
        const auto& row = sheet.rows[row_index];
        for (std::size_t column_index = 0; column_index < row.size(); ++column_index) {
            const std::string reference =
                column_name(column_index) + std::to_string(row_index + 1);
            const XlsxCell& cell = row[column_index];
            if (cell.is_number) {
                output << "<c r=\"" << reference << "\"><v>"
                       << number_text(cell.number) << "</v></c>";
            } else {
                output << "<c r=\"" << reference << "\" t=\"inlineStr\"";
                if (sheet.bold_first_row && row_index == 0) {
                    output << " s=\"1\"";
                }
                output << "><is><t>" << xml_escape(cell.text) << "</t></is></c>";
            }
        }
        output << "</row>";
    }
    output << "</sheetData></worksheet>";
    return output.str();
}

std::string content_types_xml(bool include_styles) {
    std::string xml =
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        R"(<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">)"
        R"(<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>)"
        R"(<Default Extension="xml" ContentType="application/xml"/>)"
        R"(<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>)"
        R"(<Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>)";
    if (include_styles) {
        xml += R"(<Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>)";
    }
    return xml + "</Types>";
}

std::string workbook_relationships_xml(bool include_styles) {
    std::string xml =
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
        R"(<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>)";
    if (include_styles) {
        xml += R"(<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>)";
    }
    return xml + "</Relationships>";
}

constexpr const char* kRootRelationshipsXml =
    R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
    R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
    R"(<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>)"
    R"(</Relationships>)";

constexpr const char* kStylesXml =
    R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
    R"(<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">)"
    R"(<fonts count="2"><font><sz val="11"/><name val="Calibri"/></font><font><b/><sz val="11"/><name val="Calibri"/></font></fonts>)"
    R"(<fills count="2"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill></fills>)"
    R"(<borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>)"
    R"(<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>)"
    R"(<cellXfs count="2"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/><xf numFmtId="0" fontId="1" fillId="0" borderId="0" xfId="0" applyFont="1"/></cellXfs>)"
    R"(<cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles>)"
    R"(</styleSheet>)";

} // namespace

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

fs::path ensure_xlsx_extension(fs::path path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (extension != ".xlsx") {
        path += ".xlsx";
    }
    return path;
}

void write_xlsx_file(const fs::path& path,
                     const XlsxSheet& sheet,
                     const ArchiveProgress& progress) {
    const fs::path temporary = make_temp_directory("neothemis-xlsx");
    ScopedDirectory cleanup(temporary);

    write_text_file(temporary / "[Content_Types].xml",
                    content_types_xml(sheet.bold_first_row));
    write_text_file(temporary / "_rels" / ".rels", kRootRelationshipsXml);
    write_text_file(
        temporary / "xl" / "workbook.xml",
        std::string(R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)") +
            R"(<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" )"
            R"(xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">)" +
            R"(<sheets><sheet name=")" + xml_escape(sheet.name) +
            R"(" sheetId="1" r:id="rId1"/></sheets></workbook>)");
    write_text_file(temporary / "xl" / "_rels" / "workbook.xml.rels",
                    workbook_relationships_xml(sheet.bold_first_row));
    if (sheet.bold_first_row) {
        write_text_file(temporary / "xl" / "styles.xml", kStylesXml);
    }
    write_text_file(temporary / "xl" / "worksheets" / "sheet1.xml",
                    worksheet_xml(sheet));

    write_zip_archive_from_directory(path, temporary, progress);
}

void write_xlsx_file(const fs::path& path,
                     const std::string& sheet_name,
                     const std::vector<XlsxRow>& rows,
                     const std::vector<double>& column_widths,
                     const ArchiveProgress& progress) {
    XlsxSheet sheet;
    sheet.name = sheet_name;
    sheet.rows = rows;
    sheet.column_widths = column_widths;
    write_xlsx_file(path, sheet, progress);
}

} // namespace neothemis
