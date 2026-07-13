#pragma once

#include "neothemis/ContestArchive.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace neothemis {

struct XlsxCell {
    bool is_number = false;
    double number = 0.0;
    std::string text;
};

using XlsxRow = std::vector<XlsxCell>;

struct XlsxSheet {
    std::string name;
    std::vector<XlsxRow> rows;
    std::vector<double> column_widths;
    bool freeze_first_row = true;
    bool bold_first_row = true;
};

XlsxCell xlsx_text(std::string text);
XlsxCell xlsx_number(double value);

std::filesystem::path ensure_xlsx_extension(std::filesystem::path path);

void write_xlsx_file(const std::filesystem::path& path,
                     const XlsxSheet& sheet,
                     const ArchiveProgress& progress = {});

void write_xlsx_file(const std::filesystem::path& path,
                     const std::string& sheet_name,
                     const std::vector<XlsxRow>& rows,
                     const std::vector<double>& column_widths = {},
                     const ArchiveProgress& progress = {});

} // namespace neothemis
