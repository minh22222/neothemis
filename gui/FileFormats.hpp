#pragma once

#include "neothemis/ContestArchive.hpp"
#include "neothemis/JudgeCore.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace neothemis::gui {

struct XlsxCell {
    bool is_number = false;
    double number = 0.0;
    std::string text;
};

using XlsxRow = std::vector<XlsxCell>;
using ArchiveProgress = std::function<void(std::uint64_t, std::uint64_t, const char*)>;

XlsxCell xlsx_text(std::string text);
XlsxCell xlsx_number(double value);

neothemis::ArchiveProgress core_archive_progress(const ArchiveProgress& progress);
void extract_zip_file(const std::filesystem::path& archive_path,
                      const std::filesystem::path& destination,
                      const ArchiveProgress& progress = {});
void write_contest_archive(const std::filesystem::path& archive_path,
                           const std::filesystem::path& contest_root,
                           const ArchiveProgress& progress = {});

void write_xlsx_file(const std::filesystem::path& path,
                     const std::string& sheet_name,
                     const std::vector<XlsxRow>& rows,
                     const std::vector<double>& widths);
std::vector<std::vector<std::string>> read_csv_records(
    const std::filesystem::path& path);
neothemis::Verdict verdict_from_string(const std::string& value);

} // namespace neothemis::gui
