#include "FileFormats.hpp"

#include "neothemis/Csv.hpp"

namespace fs = std::filesystem;

namespace neothemis::gui {

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

std::vector<std::vector<std::string>> read_csv_records(const fs::path& path) {
    std::error_code error;
    if (!fs::exists(path, error) || error) {
        return {};
    }
    return neothemis::read_csv_file_locked(path);
}

} // namespace neothemis::gui
