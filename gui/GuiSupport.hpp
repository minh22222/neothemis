#pragma once

#include <QPixmap>
#include <QString>

#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace neothemis::gui {

std::string trim(const std::string& value);
std::map<std::string, std::string> read_config_file(const std::filesystem::path& path);
void write_problem_config(
    const std::filesystem::path& path,
    int time_ms,
    int memory_mb,
    const std::string& default_points,
    const std::string& checker,
    const std::vector<std::pair<std::string, std::string>>& test_points);
std::vector<std::string> test_point_keys(const std::string& test_name);
std::vector<std::string> test_names_for_problem(
    const std::filesystem::path& problem_root);
std::filesystem::path default_temporary_dir();
QString format_points(double value);
QPixmap load_logo_pixmap();
std::filesystem::path path_from_qstring(const QString& value);

} // namespace neothemis::gui
