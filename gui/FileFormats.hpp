#pragma once

#include "neothemis/ContestArchive.hpp"
#include "neothemis/JudgeCore.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace neothemis::gui {

using ArchiveProgress = std::function<void(std::uint64_t, std::uint64_t, const char*)>;

neothemis::ArchiveProgress core_archive_progress(const ArchiveProgress& progress);
void extract_zip_file(const std::filesystem::path& archive_path,
                      const std::filesystem::path& destination,
                      const ArchiveProgress& progress = {});
void write_contest_archive(const std::filesystem::path& archive_path,
                           const std::filesystem::path& contest_root,
                           const ArchiveProgress& progress = {});

std::vector<std::vector<std::string>> read_csv_records(
    const std::filesystem::path& path);
} // namespace neothemis::gui
