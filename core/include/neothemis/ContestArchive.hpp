#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace neothemis {

using ArchiveProgress = std::function<void(std::uint64_t done,
                                           std::uint64_t total,
                                           const std::string& label)>;

bool has_zlib_support();
bool is_ncontest_file(const std::filesystem::path& path);
bool is_zip_like_file(const std::filesystem::path& path);
std::filesystem::path ensure_ncontest_extension(std::filesystem::path path);
std::filesystem::path make_temp_directory(const std::string& prefix);

void extract_zip_archive(const std::filesystem::path& archive_path,
                         const std::filesystem::path& destination,
                         const ArchiveProgress& progress = {});

void write_zip_archive_from_directory(const std::filesystem::path& archive_path,
                                      const std::filesystem::path& contest_root,
                                      const ArchiveProgress& progress = {});

void copy_directory_contents(const std::filesystem::path& source,
                             const std::filesystem::path& destination,
                             const ArchiveProgress& progress = {});

void convert_old_themis_contest(const std::filesystem::path& old_contest,
                                const std::filesystem::path& output_ncontest,
                                const ArchiveProgress& progress = {});

} // namespace neothemis
