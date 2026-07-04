#include "GuiSupport.hpp"

#include <QApplication>
#include <QStandardPaths>

#include <algorithm>
#include <cctype>
#include <fstream>
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

std::map<std::string, std::string> read_config_file(const fs::path& path) {
    std::map<std::string, std::string> values;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::string stripped = trim(line);
        if (stripped.empty() || stripped[0] == '#') {
            continue;
        }
        std::size_t equal = stripped.find('=');
        if (equal != std::string::npos) {
            values[trim(stripped.substr(0, equal))] = trim(stripped.substr(equal + 1));
        }
    }
    return values;
}

void write_problem_config(const fs::path& path,
                          int time_ms,
                          int memory_mb,
                          const std::string& default_points,
                          const std::string& checker,
                          const std::vector<std::pair<std::string, std::string>>& test_points) {
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("failed to write " + path.string());
    }
    out << "time_limit_ms=" << time_ms << '\n'
        << "memory_limit_mb=" << memory_mb << '\n'
        << "default_points=" << default_points << '\n'
        << "checker=" << checker << '\n';
    for (const auto& entry : test_points) {
        out << "test_points." << entry.first << '=' << entry.second << '\n';
    }
}

std::string normalized_number(std::string value) {
    std::size_t first_non_zero = value.find_first_not_of('0');
    if (first_non_zero == std::string::npos) {
        return "0";
    }
    return value.substr(first_non_zero);
}

std::vector<std::string> test_point_keys(const std::string& test_name) {
    std::vector<std::string> keys{test_name};
    std::string digits;
    for (auto it = test_name.rbegin(); it != test_name.rend(); ++it) {
        if (!std::isdigit(static_cast<unsigned char>(*it))) {
            break;
        }
        digits.push_back(*it);
    }
    if (!digits.empty()) {
        std::reverse(digits.begin(), digits.end());
        keys.push_back(digits);
        keys.push_back(normalized_number(digits));
    }

    std::vector<std::string> unique_keys;
    for (const auto& key : keys) {
        if (std::find(unique_keys.begin(), unique_keys.end(), key) == unique_keys.end()) {
            unique_keys.push_back(key);
        }
    }
    return unique_keys;
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
    fs::path base = qt_temp.isEmpty() ? fs::temp_directory_path()
                                      : fs::path(qt_temp.toStdString());
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
        fs::current_path().parent_path() / "materials" / "logo.png"
    };
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
