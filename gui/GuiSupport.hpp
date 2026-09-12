#pragma once

#include <QPixmap>
#include <QString>

#include <filesystem>
#include <string>
#include <vector>

namespace neothemis::gui {

std::string trim(const std::string& value);
void validate_path_component(const std::string& value, const char* name);
std::vector<std::string> test_names_for_problem(const std::filesystem::path& problem_root);
std::filesystem::path default_temporary_dir();
QString format_points(double value);
QPixmap load_logo_pixmap();
std::filesystem::path path_from_qstring(const QString& value);
QString qstring_from_path(const std::filesystem::path& value);

} // namespace neothemis::gui
