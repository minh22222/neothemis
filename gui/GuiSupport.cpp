#include "GuiSupport.hpp"

#include <QApplication>
#include <QStandardPaths>

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace neothemis::gui {

std::string trim(const std::string& value) {
    std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

void validate_path_component(const std::string& value, const char* name) {
    const fs::path component(value);
    if (value.empty() || value == "." || value == ".." ||
        value.find('/') != std::string::npos || value.find('\\') != std::string::npos ||
        component.is_absolute() || component.has_root_name() || component.has_root_directory() ||
        component.has_parent_path()) {
        throw std::runtime_error(std::string(name) + " must be one path component");
    }
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
    fs::path base = qt_temp.isEmpty() ? fs::temp_directory_path() : fs::path(qt_temp.toStdString());
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
        fs::current_path().parent_path() / "materials" / "logo.png"};
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

fs::path path_from_qstring(const QString& value) {
#ifdef Q_OS_WIN
    return fs::path(value.toStdWString());
#else
    return fs::path(value.toStdString());
#endif
}

} // namespace neothemis::gui
