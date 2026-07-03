#include "neothemis/JudgeCore.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace {

struct CellScore {
    double earned = 0.0;
    double max = 0.0;
    int completed = 0;
};

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
                          const std::string& checker) {
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("failed to write " + path.string());
    }
    out << "time_limit_ms=" << time_ms << '\n'
        << "memory_limit_mb=" << memory_mb << '\n'
        << "default_points=" << default_points << '\n'
        << "checker=" << checker << '\n';
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

struct XlsxCell {
    bool is_number = false;
    double number = 0.0;
    std::string text;
};

using XlsxRow = std::vector<XlsxCell>;

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

std::string xlsx_xml_escape(const std::string& value) {
    std::string escaped;
    for (unsigned char ch : value) {
        switch (ch) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            case '\'': escaped += "&apos;"; break;
            default:
                if ((ch < 0x20 && ch != '\n' && ch != '\r' && ch != '\t') || ch == 0x7f) {
                    escaped += ' ';
                } else {
                    escaped.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return escaped;
}

std::string xlsx_column_name(std::size_t index) {
    std::string name;
    ++index;
    while (index > 0) {
        std::size_t remainder = (index - 1) % 26;
        name.push_back(static_cast<char>('A' + remainder));
        index = (index - 1) / 26;
    }
    std::reverse(name.begin(), name.end());
    return name;
}

std::string xlsx_number_text(double value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(15) << value;
    return out.str();
}

std::string xlsx_sheet_xml(const std::vector<XlsxRow>& rows,
                           const std::vector<double>& widths) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        << R"(<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">)"
        << R"(<sheetViews><sheetView workbookViewId="0">)";
    if (!rows.empty()) {
        out << R"(<pane ySplit="1" topLeftCell="A2" activePane="bottomLeft" state="frozen"/>)";
    }
    out << R"(</sheetView></sheetViews>)";
    if (!widths.empty()) {
        out << "<cols>";
        for (std::size_t i = 0; i < widths.size(); ++i) {
            out << "<col min=\"" << (i + 1) << "\" max=\"" << (i + 1)
                << "\" width=\"" << widths[i] << "\" customWidth=\"1\"/>";
        }
        out << "</cols>";
    }
    out << "<sheetData>";
    for (std::size_t r = 0; r < rows.size(); ++r) {
        out << "<row r=\"" << (r + 1) << "\">";
        const auto& row = rows[r];
        for (std::size_t c = 0; c < row.size(); ++c) {
            std::string ref = xlsx_column_name(c) + std::to_string(r + 1);
            const XlsxCell& cell = row[c];
            if (cell.is_number) {
                out << "<c r=\"" << ref << "\"><v>" << xlsx_number_text(cell.number)
                    << "</v></c>";
            } else {
                out << "<c r=\"" << ref << "\" t=\"inlineStr\"";
                if (r == 0) {
                    out << " s=\"1\"";
                }
                out << "><is><t>" << xlsx_xml_escape(cell.text) << "</t></is></c>";
            }
        }
        out << "</row>";
    }
    out << "</sheetData></worksheet>";
    return out.str();
}

std::uint32_t crc32_bytes(const std::string& data) {
    static std::uint32_t table[256]{};
    static bool initialized = false;
    if (!initialized) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1U) ? (0xedb88320U ^ (value >> 1U)) : (value >> 1U);
            }
            table[i] = value;
        }
        initialized = true;
    }

    std::uint32_t crc = 0xffffffffU;
    for (unsigned char ch : data) {
        crc = table[(crc ^ ch) & 0xffU] ^ (crc >> 8U);
    }
    return crc ^ 0xffffffffU;
}

void write_le16(std::ostream& out, std::uint16_t value) {
    out.put(static_cast<char>(value & 0xffU));
    out.put(static_cast<char>((value >> 8U) & 0xffU));
}

void write_le32(std::ostream& out, std::uint32_t value) {
    out.put(static_cast<char>(value & 0xffU));
    out.put(static_cast<char>((value >> 8U) & 0xffU));
    out.put(static_cast<char>((value >> 16U) & 0xffU));
    out.put(static_cast<char>((value >> 24U) & 0xffU));
}

struct ZipEntry {
    std::string name;
    std::string data;
    std::uint32_t crc = 0;
    std::uint32_t offset = 0;
};

void write_zip_store(const fs::path& path, std::vector<ZipEntry> entries) {
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path());
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("failed to write " + path.string());
    }

    for (auto& entry : entries) {
        entry.crc = crc32_bytes(entry.data);
        entry.offset = static_cast<std::uint32_t>(out.tellp());
        write_le32(out, 0x04034b50U);
        write_le16(out, 20);
        write_le16(out, 0);
        write_le16(out, 0);
        write_le16(out, 0);
        write_le16(out, 0);
        write_le32(out, entry.crc);
        write_le32(out, static_cast<std::uint32_t>(entry.data.size()));
        write_le32(out, static_cast<std::uint32_t>(entry.data.size()));
        write_le16(out, static_cast<std::uint16_t>(entry.name.size()));
        write_le16(out, 0);
        out.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
        out.write(entry.data.data(), static_cast<std::streamsize>(entry.data.size()));
    }

    std::uint32_t central_offset = static_cast<std::uint32_t>(out.tellp());
    for (const auto& entry : entries) {
        write_le32(out, 0x02014b50U);
        write_le16(out, 20);
        write_le16(out, 20);
        write_le16(out, 0);
        write_le16(out, 0);
        write_le16(out, 0);
        write_le16(out, 0);
        write_le32(out, entry.crc);
        write_le32(out, static_cast<std::uint32_t>(entry.data.size()));
        write_le32(out, static_cast<std::uint32_t>(entry.data.size()));
        write_le16(out, static_cast<std::uint16_t>(entry.name.size()));
        write_le16(out, 0);
        write_le16(out, 0);
        write_le16(out, 0);
        write_le16(out, 0);
        write_le32(out, 0);
        write_le32(out, entry.offset);
        out.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
    }
    std::uint32_t central_size = static_cast<std::uint32_t>(out.tellp()) - central_offset;

    write_le32(out, 0x06054b50U);
    write_le16(out, 0);
    write_le16(out, 0);
    write_le16(out, static_cast<std::uint16_t>(entries.size()));
    write_le16(out, static_cast<std::uint16_t>(entries.size()));
    write_le32(out, central_size);
    write_le32(out, central_offset);
    write_le16(out, 0);
}

void write_xlsx_file(const fs::path& path,
                     const std::string& sheet_name,
                     const std::vector<XlsxRow>& rows,
                     const std::vector<double>& widths) {
    std::vector<ZipEntry> entries;
    entries.push_back({"[Content_Types].xml",
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        R"(<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">)"
        R"(<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>)"
        R"(<Default Extension="xml" ContentType="application/xml"/>)"
        R"(<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>)"
        R"(<Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>)"
        R"(<Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>)"
        R"(</Types>)"});
    entries.push_back({"_rels/.rels",
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
        R"(<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>)"
        R"(</Relationships>)"});
    entries.push_back({"xl/workbook.xml",
        std::string(R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)") +
        R"(<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" )" +
        R"(xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">)" +
        R"(<sheets><sheet name=")" + xlsx_xml_escape(sheet_name) +
        R"(" sheetId="1" r:id="rId1"/></sheets></workbook>)"});
    entries.push_back({"xl/_rels/workbook.xml.rels",
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
        R"(<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>)"
        R"(<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>)"
        R"(</Relationships>)"});
    entries.push_back({"xl/styles.xml",
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        R"(<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">)"
        R"(<fonts count="2"><font><sz val="11"/><name val="Calibri"/></font><font><b/><sz val="11"/><name val="Calibri"/></font></fonts>)"
        R"(<fills count="2"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill></fills>)"
        R"(<borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>)"
        R"(<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>)"
        R"(<cellXfs count="2"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/><xf numFmtId="0" fontId="1" fillId="0" borderId="0" xfId="0" applyFont="1"/></cellXfs>)"
        R"(<cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles>)"
        R"(</styleSheet>)"});
    entries.push_back({"xl/worksheets/sheet1.xml", xlsx_sheet_xml(rows, widths)});
    write_zip_store(path, std::move(entries));
}

std::vector<std::vector<std::string>> read_csv_records(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    std::string text = buffer.str();

    std::vector<std::vector<std::string>> records;
    std::vector<std::string> row;
    std::string cell;
    bool in_quotes = false;
    bool have_data = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char ch = text[i];
        have_data = true;
        if (in_quotes) {
            if (ch == '"' && i + 1 < text.size() && text[i + 1] == '"') {
                cell.push_back('"');
                ++i;
            } else if (ch == '"') {
                in_quotes = false;
            } else {
                cell.push_back(ch);
            }
            continue;
        }
        if (ch == '"') {
            in_quotes = true;
        } else if (ch == ',') {
            row.push_back(cell);
            cell.clear();
        } else if (ch == '\n') {
            row.push_back(cell);
            cell.clear();
            records.push_back(row);
            row.clear();
            have_data = false;
        } else if (ch != '\r') {
            cell.push_back(ch);
        }
    }
    if (have_data || !cell.empty() || !row.empty()) {
        row.push_back(cell);
        records.push_back(row);
    }
    return records;
}

neothemis::Verdict verdict_from_string(const std::string& value) {
    if (value == "AC") return neothemis::Verdict::Accepted;
    if (value == "WA") return neothemis::Verdict::WrongAnswer;
    if (value == "CE") return neothemis::Verdict::CompileError;
    if (value == "RE") return neothemis::Verdict::RuntimeError;
    if (value == "TLE") return neothemis::Verdict::TimeLimitExceeded;
    if (value == "MLE") return neothemis::Verdict::MemoryLimitExceeded;
    if (value == "MS") return neothemis::Verdict::MissingSource;
    if (value == "SV") return neothemis::Verdict::SecurityViolation;
    return neothemis::Verdict::InternalError;
}

class MainWindow : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle("Contest Judge");
        setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
        resize(1240, 780);

        auto* central = new QWidget(this);
        auto* root = new QVBoxLayout(central);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);

        title_bar_ = new QWidget(central);
        title_bar_->setObjectName("WindowTitleBar");
        title_bar_->installEventFilter(this);
        auto* title_layout = new QHBoxLayout(title_bar_);
        title_layout->setContentsMargins(10, 0, 6, 0);
        title_layout->setSpacing(8);
        auto* logo = new QLabel(title_bar_);
        logo->setObjectName("AppLogo");
        QPixmap pixmap = load_logo_pixmap();
        if (!pixmap.isNull()) {
            logo->setPixmap(pixmap.scaled(24, 24, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
        logo->installEventFilter(this);
        auto* app_name = new QLabel("NeoThemis", title_bar_);
        app_name->setObjectName("WindowAppName");
        app_name->installEventFilter(this);
        title_layout->addWidget(logo);
        title_layout->addWidget(app_name);
        title_layout->addStretch(1);
        contest_title_ = new QLabel("No contest open", title_bar_);
        contest_title_->setObjectName("ContestTitle");
        contest_title_->installEventFilter(this);
        title_layout->addWidget(contest_title_);
        auto* minimize = new QToolButton(title_bar_);
        minimize->setObjectName("WindowButton");
        minimize->setText("-");
        auto* maximize = new QToolButton(title_bar_);
        maximize->setObjectName("WindowButton");
        maximize->setText("[]");
        auto* close = new QToolButton(title_bar_);
        close->setObjectName("WindowCloseButton");
        close->setText("x");
        title_layout->addWidget(minimize);
        title_layout->addWidget(maximize);
        title_layout->addWidget(close);
        root->addWidget(title_bar_);

        auto* menu_row = new QWidget(central);
        menu_row->setObjectName("MenuRow");
        auto* menu_layout = new QHBoxLayout(menu_row);
        menu_layout->setContentsMargins(10, 0, 10, 0);
        menu_layout->setSpacing(0);
        menu_bar_ = new QMenuBar(menu_row);
        menu_bar_->setNativeMenuBar(false);
        menu_layout->addWidget(menu_bar_, 0, Qt::AlignLeft);
        menu_layout->addStretch(1);
        root->addWidget(menu_row);

        build_toolbar();

        auto* content = new QHBoxLayout;
        content->setContentsMargins(12, 10, 12, 12);
        content->setSpacing(10);

        table_ = new QTableWidget(central);
        table_->setAlternatingRowColors(true);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        table_->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
        table_->verticalHeader()->setVisible(false);
        table_->verticalHeader()->setDefaultSectionSize(54);
        content->addWidget(table_, 1);

        auto* side = new QWidget(central);
        auto* side_layout = new QVBoxLayout(side);
        side->setFixedWidth(380);

        auto* actions = new QGroupBox("Judge", side);
        auto* action_layout = new QVBoxLayout(actions);
        judge_selected_button_ = new QPushButton("Judge Selected", actions);
        judge_all_button_ = new QPushButton("Judge All", actions);
        stop_button_ = new QPushButton("Stop", actions);
        stop_button_->setObjectName("StopButton");
        stop_button_->setEnabled(false);
        action_layout->addWidget(judge_selected_button_);
        action_layout->addWidget(judge_all_button_);
        action_layout->addWidget(stop_button_);
        side_layout->addWidget(actions);

        run_status_ = new QLabel("No active run", side);
        run_status_->setWordWrap(true);
        progress_label_ = new QLabel("Idle", side);
        progress_ = new QProgressBar(side);
        progress_->setRange(0, 100);
        progress_->setValue(0);
        side_layout->addWidget(run_status_);
        side_layout->addWidget(progress_label_);
        side_layout->addWidget(progress_);

        log_ = new QPlainTextEdit(side);
        log_->setReadOnly(true);
        side_layout->addWidget(log_, 1);

        content->addWidget(side);
        root->addLayout(content, 1);
        setCentralWidget(central);
        apply_dark_theme();

        QObject::connect(judge_selected_button_, &QPushButton::clicked, [this]() { start_judge(true); });
        QObject::connect(judge_all_button_, &QPushButton::clicked, [this]() { start_judge(false); });
        QObject::connect(stop_button_, &QPushButton::clicked, [this]() { request_stop_judge(); });
        QObject::connect(table_, &QTableWidget::cellDoubleClicked, [this](int row, int col) {
            show_result_details(row, col);
        });
        QObject::connect(table_->horizontalHeader(), &QHeaderView::sectionClicked,
                         [this](int section) { sort_by_column(section); });
        QObject::connect(table_->horizontalHeader(), &QWidget::customContextMenuRequested,
                         [this](const QPoint& pos) { show_header_menu(pos); });
        QObject::connect(minimize, &QToolButton::clicked, this, &QWidget::showMinimized);
        QObject::connect(maximize, &QToolButton::clicked, [this]() {
            isMaximized() ? showNormal() : showMaximized();
        });
        QObject::connect(close, &QToolButton::clicked, this, &QWidget::close);
    }

    ~MainWindow() override {
        stop_active_judge();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == title_bar_ || watched == contest_title_ ||
            (watched->isWidgetType() &&
             static_cast<QWidget*>(watched)->objectName() == "WindowAppName") ||
            (watched->isWidgetType() &&
             static_cast<QWidget*>(watched)->objectName() == "AppLogo")) {
            if (event->type() == QEvent::MouseButtonDblClick) {
                auto* mouse = static_cast<QMouseEvent*>(event);
                if (mouse->button() == Qt::LeftButton) {
                    isMaximized() ? showNormal() : showMaximized();
                    return true;
                }
            }
            if (event->type() == QEvent::MouseButtonPress) {
                auto* mouse = static_cast<QMouseEvent*>(event);
                if (mouse->button() == Qt::LeftButton) {
                    dragging_title_bar_ = true;
                    drag_offset_ = mouse->globalPosition().toPoint() - frameGeometry().topLeft();
                    return true;
                }
            }
            if (event->type() == QEvent::MouseMove && dragging_title_bar_) {
                auto* mouse = static_cast<QMouseEvent*>(event);
                if (!isMaximized()) {
                    move(mouse->globalPosition().toPoint() - drag_offset_);
                }
                return true;
            }
            if (event->type() == QEvent::MouseButtonRelease) {
                dragging_title_bar_ = false;
            }
        }
        return QMainWindow::eventFilter(watched, event);
    }

    void closeEvent(QCloseEvent* event) override {
        stop_active_judge();
        event->accept();
    }

private:
    void build_toolbar() {
        auto* bar = menu_bar_ ? menu_bar_ : menuBar();
        bar->clear();

        auto* contest_menu = bar->addMenu("Contest");
        contest_menu->addAction("Open Folder", [this]() { open_contest(); });
        contest_menu->addAction("New Folder", [this]() { new_contest(); });
        contest_menu->addSeparator();
        contest_menu->addAction("Refresh", [this]() { refresh_table(); });

        auto* judge_menu = bar->addMenu("Judge");
        judge_selected_action_ = judge_menu->addAction("Judge Selected", [this]() { start_judge(true); });
        judge_all_action_ = judge_menu->addAction("Judge All", [this]() { start_judge(false); });
        stop_action_ = judge_menu->addAction("Stop", [this]() { request_stop_judge(); });
        stop_action_->setEnabled(false);

        auto* export_menu = bar->addMenu("Export");
        export_menu->addAction("Export Scoreboard (xlsx)", [this]() { export_scoreboard_xlsx(); });
        export_menu->addAction("Export Data (xlsx)", [this]() { export_data_xlsx(); });

        auto* settings_menu = bar->addMenu("Settings");
        settings_menu->addAction("Application Settings", [this]() { open_settings_dialog(0); });
        settings_menu->addAction("Contest Config", [this]() { open_settings_dialog(1); });
        settings_menu->addAction("Problem Config", [this]() { open_settings_dialog(2); });

        auto* help_menu = bar->addMenu("Help");
        help_menu->addAction("About", [this]() { show_about_dialog(); });
    }

    void apply_dark_theme() {
        qApp->setStyleSheet(R"(
            QWidget { background: #111318; color: #e5e7eb; font-size: 13px; }
            QMainWindow, QDialog { background: #111318; }
            QWidget#WindowTitleBar {
                background: #0d1015; border-bottom: 1px solid #29303a; min-height: 36px;
            }
            QWidget#MenuRow {
                background: #12161d; border-bottom: 1px solid #22313c; min-height: 30px;
            }
            QLabel#WindowAppName {
                color: #f8fafc; font-size: 14px; font-weight: 800;
                padding-right: 10px;
            }
            QLabel#AppLogo {
                min-width: 28px; min-height: 28px;
            }
            QMenuBar {
                background: transparent; border: 0; padding: 0;
            }
            QMenuBar::item { background: transparent; padding: 6px 12px; border-radius: 4px; }
            QMenuBar::item:selected {
                background: #1c2b34; color: #7df9ff; border-bottom: 1px solid #ff2a6d;
            }
            QToolButton#WindowButton, QToolButton#WindowCloseButton {
                background: transparent; border: 1px solid transparent; color: #cbd5e1;
                min-width: 34px; min-height: 26px; border-radius: 4px; font-weight: 700;
            }
            QToolButton#WindowButton:hover { background: #1d2732; border-color: #2de2e6; color: #f8fafc; }
            QToolButton#WindowCloseButton:hover { background: #7f1d2d; border-color: #ff3864; color: #ffffff; }
            QPushButton {
                background: #121b22; color: #e7fbff; border: 1px solid #2de2e6; border-radius: 5px;
                padding: 8px 10px; font-weight: 700;
            }
            QPushButton:hover { background: #182932; border-color: #7df9ff; color: #ffffff; }
            QPushButton:pressed { background: #0d151b; border-color: #ff2a6d; }
            QPushButton:disabled { background: #29313a; color: #7f8b99; }
            QPushButton#StopButton { background: #29131a; border-color: #ff3864; }
            QPushButton#StopButton:hover { background: #401923; border-color: #ff6b8a; }
            QMenu { background: #181b21; border: 1px solid #343a45; padding: 4px; }
            QMenu::item { padding: 7px 24px; border-radius: 4px; }
            QMenu::item:selected { background: #20313a; color: #7df9ff; }
            QTableWidget, QPlainTextEdit, QLineEdit, QSpinBox, QComboBox {
                background: #171a20; border: 1px solid #303640; border-radius: 5px;
                padding: 4px; color: #e5e7eb;
            }
            QTableWidget {
                background: #12161d; alternate-background-color: #151b23;
                gridline-color: #28404b; selection-background-color: #263a44;
            }
            QTableWidget::item { border-bottom: 1px solid #1f2d36; }
            QTableWidget::item:selected {
                background: #243946; color: #ffffff; border: 1px solid #2de2e6;
            }
            QHeaderView::section {
                background: #17212a; color: #7df9ff; border: 0;
                border-right: 1px solid #28404b; border-bottom: 1px solid #ff2a6d;
                padding: 8px; font-weight: 700;
            }
            QGroupBox {
                border: 1px solid #303640; border-radius: 6px; margin-top: 10px;
                padding-top: 12px; background: #151820;
            }
            QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }
            QProgressBar {
                background: #171a20; border: 1px solid #303640; border-radius: 5px;
                height: 18px; text-align: center;
            }
            QProgressBar::chunk { background: #2de2e6; border-radius: 4px; }
            QLabel { color: #cbd5e1; }
            QLabel#ContestTitle { color: #94a3b8; }
            QTabWidget::pane { border: 1px solid #303640; border-radius: 5px; }
            QTabBar::tab { background: #181b21; padding: 8px 12px; border-top-left-radius: 5px; border-top-right-radius: 5px; }
            QTabBar::tab:selected { background: #20313a; color: #7df9ff; }
            QScrollBar:vertical, QScrollBar:horizontal { background: #10131a; border: 0; margin: 0; }
            QScrollBar:vertical { width: 10px; }
            QScrollBar:horizontal { height: 10px; }
            QScrollBar::handle { background: #3a4452; border-radius: 4px; }
            QScrollBar::handle:hover { background: #566274; }
            QScrollBar::handle:vertical { min-height: 26px; }
            QScrollBar::handle:horizontal { min-width: 26px; }
            QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
            QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
        )");
    }

    neothemis::JudgeOptions options_from_ui() const {
        neothemis::JudgeOptions options;
        options.contest_root = contest_root_;
        options.compiler = compiler_;
        options.compile_flags = compile_flags_;
        options.contestants_dir = contestants_dir_;
        options.tests_dir = tests_dir_;
        options.parallel_jobs = parallel_jobs_;
        options.stack_limit_mb = stack_limit_mb_;
        options.keep_workdir = keep_workdir_;
        options.forbidden_patterns = {
            "system(", "popen(", "fork(", "exec(", "#include <unistd.h>",
            "#include <sys/", "#include <windows.h>"
        };
        return options;
    }

    void open_contest() {
        QString dir = QFileDialog::getExistingDirectory(this, "Open contest");
        if (dir.isEmpty()) {
            return;
        }
        contest_root_ = dir.toStdString();
        contest_title_->setText(QString::fromStdString(contest_root_.filename().string()));
        load_contest_config();
        refresh_table();
    }

    void new_contest() {
        QString parent = QFileDialog::getExistingDirectory(this, "Choose parent folder");
        if (parent.isEmpty()) {
            return;
        }
        bool ok = false;
        QString name = QInputDialog::getText(this, "New contest", "Folder name",
                                             QLineEdit::Normal, "contest", &ok);
        if (!ok || name.trimmed().isEmpty()) {
            return;
        }
        fs::path root = fs::path(parent.toStdString()) / name.trimmed().toStdString();
        try {
            fs::create_directories(root / contestants_dir_);
            fs::create_directories(root / tests_dir_);
            contest_root_ = root;
            contest_title_->setText(QString::fromStdString(contest_root_.filename().string()));
            save_contest_config();
            refresh_table();
        } catch (const std::exception& ex) {
            QMessageBox::critical(this, "Create failed", ex.what());
        }
    }

    void load_contest_config() {
        auto values = read_config_file(contest_root_ / "neothemis.conf");
        if (values.count("compiler")) compiler_ = values["compiler"];
        if (values.count("compile_flags")) compile_flags_ = values["compile_flags"];
        if (values.count("contestants_dir")) contestants_dir_ = values["contestants_dir"];
        if (values.count("tests_dir")) tests_dir_ = values["tests_dir"];
        if (values.count("stack_limit_mb")) stack_limit_mb_ = std::stoull(values["stack_limit_mb"]);
        if (values.count("parallel_jobs")) parallel_jobs_ = std::stoul(values["parallel_jobs"]);
        if (values.count("keep_workdir")) {
            std::string value = values["keep_workdir"];
            keep_workdir_ = value == "true" || value == "1" || value == "yes" || value == "on";
        }
    }

    void save_contest_config() const {
        if (contest_root_.empty()) {
            return;
        }
        std::ofstream out(contest_root_ / "neothemis.conf");
        if (!out) {
            throw std::runtime_error("failed to write neothemis.conf");
        }
        out << "core=builtin\n"
            << "contestants_dir=" << contestants_dir_ << '\n'
            << "tests_dir=" << tests_dir_ << '\n'
            << "output_csv=results.csv\n"
            << "scoreboard_csv=scoreboard.csv\n"
            << "keep_workdir=" << (keep_workdir_ ? "true" : "false") << '\n'
            << "compiler=" << compiler_ << '\n'
            << "compile_flags=" << compile_flags_ << '\n'
            << "stack_limit_mb=" << stack_limit_mb_ << '\n'
            << "parallel_jobs=" << parallel_jobs_ << '\n'
            << "forbidden_pattern=system(\n"
            << "forbidden_pattern=popen(\n"
            << "forbidden_pattern=fork(\n"
            << "forbidden_pattern=exec(\n"
            << "forbidden_pattern=#include <unistd.h>\n"
            << "forbidden_pattern=#include <sys/\n"
            << "forbidden_pattern=#include <windows.h>\n";
    }

    void refresh_table() {
        if (contest_root_.empty()) {
            return;
        }
        try {
            auto overview = neothemis::inspect_contest(options_from_ui());
            contestants_ = overview.contestants;
            problems_ = overview.problems;
            source_ready_.clear();
            cell_texts_.clear();
            for (std::size_t row = 0; row < overview.contestants.size(); ++row) {
                for (std::size_t col = 0; col < overview.problems.size(); ++col) {
                    source_ready_[cell_key(overview.contestants[row], overview.problems[col])] =
                        overview.has_source[row][col];
                }
            }
            load_problem_test_counts();
            load_existing_results();
            populate_table();
            log_->appendPlainText("Opened " + QString::fromStdString(contest_root_.string()));
        } catch (const std::exception& ex) {
            QMessageBox::critical(this, "Open failed", ex.what());
        }
    }

    fs::path contest_output_path(const fs::path& path) const {
        return path.is_relative() ? contest_root_ / path : path;
    }

    void record_result(const neothemis::TestResult& result) {
        std::string key = cell_key(result.contestant, result.problem);
        result_details_[key].push_back(result);
        CellScore& score = score_cells_[key];
        score.earned += result.earned_points;
        score.max += result.max_points;
        ++score.completed;
    }

    std::vector<neothemis::TestResult> all_recorded_results() const {
        std::vector<neothemis::TestResult> rows;
        for (const auto& entry : result_details_) {
            rows.insert(rows.end(), entry.second.begin(), entry.second.end());
        }
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            return std::tie(a.contestant, a.problem, a.test) <
                   std::tie(b.contestant, b.problem, b.test);
        });
        return rows;
    }

    void load_existing_results() {
        score_cells_.clear();
        result_details_.clear();

        neothemis::JudgeOptions options = options_from_ui();
        fs::path details_path = contest_output_path(options.output_csv);
        if (!fs::exists(details_path)) {
            return;
        }

        std::set<std::string> known_contestants(contestants_.begin(), contestants_.end());
        std::set<std::string> known_problems(problems_.begin(), problems_.end());
        auto records = read_csv_records(details_path);
        if (records.size() <= 1) {
            return;
        }
        for (std::size_t i = 1; i < records.size(); ++i) {
            const auto& fields = records[i];
            if (fields.size() < 9 ||
                known_contestants.count(fields[0]) == 0 ||
                known_problems.count(fields[1]) == 0) {
                continue;
            }
            try {
                neothemis::TestResult result;
                result.contestant = fields[0];
                result.problem = fields[1];
                result.test = fields[2];
                result.verdict = verdict_from_string(fields[3]);
                result.time_ms = static_cast<std::uint64_t>(std::stoull(fields[4]));
                result.exit_code = std::stoi(fields[5]);
                result.max_points = std::stod(fields[6]);
                result.earned_points = std::stod(fields[7]);
                result.message = fields[8];
                record_result(result);
            } catch (...) {
                log_->appendPlainText("Skipped one malformed row in results.csv.");
            }
        }
    }

    void write_current_csv_outputs() const {
        neothemis::JudgeOptions options = options_from_ui();
        std::vector<neothemis::TestResult> rows = all_recorded_results();

        fs::path details_path = contest_output_path(options.output_csv);
        fs::create_directories(details_path.parent_path());
        std::ofstream details(details_path);
        if (!details) {
            throw std::runtime_error("failed to open CSV output: " + details_path.string());
        }
        neothemis::write_csv(details, rows);

        fs::path scoreboard_path = contest_output_path(options.scoreboard_csv);
        fs::create_directories(scoreboard_path.parent_path());
        std::ofstream scoreboard(scoreboard_path);
        if (!scoreboard) {
            throw std::runtime_error("failed to open scoreboard CSV output: " +
                                     scoreboard_path.string());
        }
        neothemis::write_scoreboard_csv(scoreboard, rows);
    }

    std::string status_for_scoreboard_cell(const std::string& contestant,
                                           const std::string& problem) const {
        auto found = result_details_.find(cell_key(contestant, problem));
        if (found == result_details_.end()) {
            return {};
        }
        std::string status;
        int priority = 0;
        for (const auto& result : found->second) {
            int candidate_priority = 0;
            std::string candidate;
            if (result.verdict == neothemis::Verdict::CompileError) {
                candidate = "CE";
                candidate_priority = 2;
            } else if (result.verdict == neothemis::Verdict::MissingSource) {
                candidate = "MS";
                candidate_priority = 1;
            }
            if (candidate_priority > priority) {
                status = candidate;
                priority = candidate_priority;
            }
        }
        return status;
    }

    fs::path choose_export_path(const std::string& filename) {
        QString default_path = contest_root_.empty()
                                   ? QString::fromStdString(filename)
                                   : QString::fromStdString((contest_root_ / filename).string());
        QString selected = QFileDialog::getSaveFileName(
            this, "Export workbook", default_path, "Excel workbook (*.xlsx)");
        if (selected.isEmpty()) {
            return {};
        }
        fs::path path = selected.toStdString();
        if (path.extension().string() != ".xlsx") {
            path += ".xlsx";
        }
        return path;
    }

    bool export_is_available() {
        if (judging_.load()) {
            QMessageBox::information(this, "Judge running",
                                     "Wait for the active judge run to finish before exporting.");
            return false;
        }
        if (contest_root_.empty()) {
            QMessageBox::information(this, "No contest",
                                     "Open or create a contest folder first.");
            return false;
        }
        if (all_recorded_results().empty()) {
            QMessageBox::information(this, "No results",
                                     "No judged results are available to export yet.");
            return false;
        }
        return true;
    }

    void export_scoreboard_xlsx() {
        if (!export_is_available()) {
            return;
        }
        fs::path path = choose_export_path("scoreboard.xlsx");
        if (path.empty()) {
            return;
        }

        std::vector<XlsxRow> rows;
        XlsxRow header{xlsx_text("Contestant")};
        for (const auto& problem : problems_) {
            header.push_back(xlsx_text(problem));
        }
        header.push_back(xlsx_text("Total"));
        rows.push_back(std::move(header));

        for (const auto& contestant : contestants_) {
            XlsxRow row{xlsx_text(contestant)};
            for (const auto& problem : problems_) {
                double score = earned_for_problem(contestant, problem);
                std::string status = status_for_scoreboard_cell(contestant, problem);
                if (score == 0.0 && !status.empty()) {
                    row.push_back(xlsx_text(status + "(0)"));
                } else {
                    row.push_back(xlsx_number(score));
                }
            }
            row.push_back(xlsx_number(total_earned_for(contestant)));
            rows.push_back(std::move(row));
        }

        std::vector<double> widths(rows.front().size(), 14.0);
        widths[0] = 28.0;
        try {
            write_xlsx_file(path, "Scoreboard", rows, widths);
            log_->appendPlainText("Exported " + QString::fromStdString(path.string()));
            QMessageBox::information(this, "Export complete",
                                     "Exported " + QString::fromStdString(path.string()));
        } catch (const std::exception& ex) {
            QMessageBox::critical(this, "Export failed", ex.what());
        }
    }

    void export_data_xlsx() {
        if (!export_is_available()) {
            return;
        }
        fs::path path = choose_export_path("results-data.xlsx");
        if (path.empty()) {
            return;
        }

        std::vector<XlsxRow> rows;
        rows.push_back({
            xlsx_text("contestant"),
            xlsx_text("problem"),
            xlsx_text("test"),
            xlsx_text("verdict"),
            xlsx_text("time_ms"),
            xlsx_text("exit_code"),
            xlsx_text("max_points"),
            xlsx_text("earned_points"),
            xlsx_text("message")
        });
        for (const auto& result : all_recorded_results()) {
            rows.push_back({
                xlsx_text(result.contestant),
                xlsx_text(result.problem),
                xlsx_text(result.test),
                xlsx_text(neothemis::to_string(result.verdict)),
                xlsx_number(static_cast<double>(result.time_ms)),
                xlsx_number(static_cast<double>(result.exit_code)),
                xlsx_number(result.max_points),
                xlsx_number(result.earned_points),
                xlsx_text(result.message)
            });
        }

        std::vector<double> widths{28.0, 14.0, 12.0, 10.0, 12.0, 12.0, 12.0, 14.0, 48.0};
        try {
            write_xlsx_file(path, "Data", rows, widths);
            log_->appendPlainText("Exported " + QString::fromStdString(path.string()));
            QMessageBox::information(this, "Export complete",
                                     "Exported " + QString::fromStdString(path.string()));
        } catch (const std::exception& ex) {
            QMessageBox::critical(this, "Export failed", ex.what());
        }
    }

    void rebuild_maps() {
        contestant_rows_.clear();
        problem_columns_.clear();
        for (std::size_t i = 0; i < contestants_.size(); ++i) {
            contestant_rows_[contestants_[i]] = static_cast<int>(i);
        }
        for (std::size_t i = 0; i < problems_.size(); ++i) {
            problem_columns_[problems_[i]] = static_cast<int>(i + 1);
        }
    }

    int total_column() const {
        return static_cast<int>(problems_.size() + 1);
    }

    std::string cell_key(const std::string& contestant, const std::string& problem) const {
        return contestant + "\n" + problem;
    }

    double earned_for_problem(const std::string& contestant, const std::string& problem) const {
        auto it = score_cells_.find(cell_key(contestant, problem));
        return it == score_cells_.end() ? 0.0 : it->second.earned;
    }

    double max_for_problem(const std::string& contestant, const std::string& problem) const {
        auto it = score_cells_.find(cell_key(contestant, problem));
        return it == score_cells_.end() ? 0.0 : it->second.max;
    }

    double total_earned_for(const std::string& contestant) const {
        double total = 0.0;
        for (const auto& problem : problems_) {
            total += earned_for_problem(contestant, problem);
        }
        return total;
    }

    double total_max_for(const std::string& contestant) const {
        double total = 0.0;
        for (const auto& problem : problems_) {
            total += max_for_problem(contestant, problem);
        }
        return total;
    }

    QString problem_cell_text(const std::string& contestant, const std::string& problem) const {
        std::string key = cell_key(contestant, problem);
        auto text_it = cell_texts_.find(key);
        if (text_it != cell_texts_.end()) {
            return text_it->second;
        }

        auto score_it = score_cells_.find(key);
        if (score_it == score_cells_.end()) {
            auto source_it = source_ready_.find(key);
            return source_it != source_ready_.end() && source_it->second ? "Ready" : "Missing";
        }
        const CellScore& score = score_it->second;
        int expected = 1;
        auto expected_it = problem_test_counts_.find(problem);
        if (expected_it != problem_test_counts_.end()) {
            expected = expected_it->second;
        }
        QString status = score.completed >= expected ? "Done" : "Running";
        return QString("%1/%2\n%3 %4/%5")
            .arg(format_points(score.earned))
            .arg(format_points(score.max))
            .arg(status)
            .arg(score.completed)
            .arg(expected);
    }

    void populate_table() {
        rebuild_maps();
        table_->clear();
        table_->setRowCount(static_cast<int>(contestants_.size()));
        table_->setColumnCount(total_column() + 1);
        table_->setHorizontalHeaderItem(0, new QTableWidgetItem("Contestant"));
        for (std::size_t col = 0; col < problems_.size(); ++col) {
            table_->setHorizontalHeaderItem(static_cast<int>(col + 1),
                                            new QTableWidgetItem(QString::fromStdString(problems_[col])));
        }
        table_->setHorizontalHeaderItem(total_column(), new QTableWidgetItem("Total"));

        for (std::size_t row = 0; row < contestants_.size(); ++row) {
            const std::string& contestant = contestants_[row];
            auto* name_item = new QTableWidgetItem(QString::fromStdString(contestant));
            style_name_item(name_item);
            table_->setItem(static_cast<int>(row), 0, name_item);
            for (std::size_t col = 0; col < problems_.size(); ++col) {
                auto* item = new QTableWidgetItem(problem_cell_text(contestant, problems_[col]));
                item->setTextAlignment(Qt::AlignCenter);
                style_problem_item(contestant, problems_[col], item);
                table_->setItem(static_cast<int>(row), static_cast<int>(col + 1), item);
            }
            update_total_cell(contestant);
        }
    }

    void load_problem_test_counts() {
        problem_test_counts_.clear();
        fs::path tests_root = contest_root_ / tests_dir_;
        for (const auto& problem : problems_) {
            int count = 0;
            fs::path problem_root = tests_root / problem;
            if (fs::exists(problem_root)) {
                for (const auto& entry : fs::directory_iterator(problem_root)) {
                    if (entry.is_directory()) {
                        ++count;
                    }
                }
            }
            problem_test_counts_[problem] = std::max(1, count);
        }
    }

    std::vector<std::string> selected_contestants() const {
        std::set<int> rows;
        for (const QModelIndex& index : table_->selectionModel()->selectedRows()) {
            rows.insert(index.row());
        }
        std::vector<std::string> selected;
        for (int row : rows) {
            if (row >= 0 && static_cast<std::size_t>(row) < contestants_.size()) {
                selected.push_back(contestants_[static_cast<std::size_t>(row)]);
            }
        }
        return selected;
    }

    void reset_run_cells(const std::vector<std::string>& selected, const std::string& selected_problem) {
        std::set<std::string> selected_set(selected.begin(), selected.end());
        for (const auto& contestant : contestants_) {
            if (!selected_set.empty() && selected_set.count(contestant) == 0) {
                continue;
            }
            for (const auto& problem : problems_) {
                if (!selected_problem.empty() && problem != selected_problem) {
                    continue;
                }
                std::string key = cell_key(contestant, problem);
                score_cells_.erase(key);
                result_details_.erase(key);
                cell_texts_.erase(key);
                set_table_cell(contestant, problem, "Queued");
            }
            update_total_cell(contestant);
        }
    }

    void set_table_cell(const std::string& contestant,
                        const std::string& problem,
                        const QString& text) {
        auto row_it = contestant_rows_.find(contestant);
        auto col_it = problem_columns_.find(problem);
        if (row_it == contestant_rows_.end() || col_it == problem_columns_.end()) {
            return;
        }
        auto* item = table_->item(row_it->second, col_it->second);
        if (!item) {
            item = new QTableWidgetItem;
            table_->setItem(row_it->second, col_it->second, item);
        }
        cell_texts_[cell_key(contestant, problem)] = text;
        item->setText(text);
        item->setTextAlignment(Qt::AlignCenter);
        style_problem_item(contestant, problem, item);
    }

    void update_total_cell(const std::string& contestant) {
        auto row_it = contestant_rows_.find(contestant);
        if (row_it == contestant_rows_.end()) {
            return;
        }
        auto* item = table_->item(row_it->second, total_column());
        if (!item) {
            item = new QTableWidgetItem;
            table_->setItem(row_it->second, total_column(), item);
        }
        item->setText(format_points(total_earned_for(contestant)) + "/" +
                      format_points(total_max_for(contestant)));
        item->setTextAlignment(Qt::AlignCenter);
        style_total_item(contestant, item);
    }

    void style_name_item(QTableWidgetItem* item) const {
        if (!item) {
            return;
        }
        item->setForeground(QColor("#e7fbff"));
        item->setBackground(QColor("#151b23"));
    }

    void style_problem_item(const std::string& contestant,
                            const std::string& problem,
                            QTableWidgetItem* item) const {
        if (!item) {
            return;
        }
        std::string key = cell_key(contestant, problem);
        auto score_it = score_cells_.find(key);
        if (score_it != score_cells_.end()) {
            const CellScore& score = score_it->second;
            int expected = 1;
            auto expected_it = problem_test_counts_.find(problem);
            if (expected_it != problem_test_counts_.end()) {
                expected = expected_it->second;
            }
            if (score.completed < expected) {
                item->setForeground(QColor("#7df9ff"));
                item->setBackground(QColor("#122631"));
            } else if (score.max > 0.0 && score.earned + 1e-9 >= score.max) {
                item->setForeground(QColor("#99ffcc"));
                item->setBackground(QColor("#123028"));
            } else if (score.earned > 0.0) {
                item->setForeground(QColor("#ffe680"));
                item->setBackground(QColor("#302512"));
            } else {
                item->setForeground(QColor("#ff8fab"));
                item->setBackground(QColor("#30151f"));
            }
            return;
        }

        QString text = item->text().toLower();
        if (text.contains("queued")) {
            item->setForeground(QColor("#ffe680"));
            item->setBackground(QColor("#2a2412"));
            return;
        }
        auto source_it = source_ready_.find(key);
        if (source_it != source_ready_.end() && source_it->second) {
            item->setForeground(QColor("#7df9ff"));
            item->setBackground(QColor("#122631"));
        } else {
            item->setForeground(QColor("#ff8fab"));
            item->setBackground(QColor("#281821"));
        }
    }

    void style_total_item(const std::string& contestant, QTableWidgetItem* item) const {
        if (!item) {
            return;
        }
        double earned = total_earned_for(contestant);
        double max = total_max_for(contestant);
        if (max > 0.0 && earned + 1e-9 >= max) {
            item->setForeground(QColor("#99ffcc"));
            item->setBackground(QColor("#102a24"));
        } else if (earned > 0.0) {
            item->setForeground(QColor("#ffe680"));
            item->setBackground(QColor("#2c2312"));
        } else {
            item->setForeground(QColor("#7df9ff"));
            item->setBackground(QColor("#121f2a"));
        }
    }

    void handle_result(const neothemis::TestResult& result) {
        record_result(result);
        const CellScore& score = score_cells_[cell_key(result.contestant, result.problem)];

        int expected = problem_test_counts_[result.problem];
        QString status = score.completed >= expected ? "Done" : "Running";
        QString text = QString("%1/%2\n%3 %4/%5")
                           .arg(format_points(score.earned))
                           .arg(format_points(score.max))
                           .arg(status)
                           .arg(score.completed)
                           .arg(expected);
        set_table_cell(result.contestant, result.problem, text);
        update_total_cell(result.contestant);
    }

    void show_result_details(int row, int col) {
        if (row < 0 || col <= 0 ||
            static_cast<std::size_t>(row) >= contestants_.size() ||
            static_cast<std::size_t>(col - 1) >= problems_.size()) {
            return;
        }

        std::string contestant = contestants_[static_cast<std::size_t>(row)];
        std::string problem = problems_[static_cast<std::size_t>(col - 1)];
        std::string key = contestant + "\n" + problem;
        std::vector<neothemis::TestResult> rows = result_details_[key];
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            return a.test < b.test;
        });

        QString text;
        if (rows.empty()) {
            text = "No judged tests for this cell yet.";
        } else {
            for (const auto& result : rows) {
                QString description = QString::fromStdString(neothemis::to_string(result.verdict));
                if (!result.message.empty()) {
                    description += ": " + QString::fromStdString(result.message);
                }
                text += QString::fromStdString(result.test) + ": " +
                        format_points(result.earned_points) + "/" +
                        format_points(result.max_points) + " Point\n";
                text += "Description: " + description + "\n\n";
            }
        }

        auto* dialog = new QDialog(this);
        dialog->setWindowTitle(QString::fromStdString(contestant + " - " + problem));
        dialog->resize(640, 520);
        auto* layout = new QVBoxLayout(dialog);
        auto* title = new QLabel(QString::fromStdString(contestant + " / " + problem), dialog);
        title->setObjectName("AppTitle");
        auto* details = new QPlainTextEdit(dialog);
        details->setReadOnly(true);
        details->setPlainText(text);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
        layout->addWidget(title);
        layout->addWidget(details, 1);
        layout->addWidget(buttons);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    }

    void sort_by_column(int section) {
        if (section < 0 || section > total_column()) {
            return;
        }
        if (sort_column_ == section) {
            sort_ascending_ = !sort_ascending_;
        } else {
            sort_column_ = section;
            sort_ascending_ = true;
        }

        auto name_less = [](const std::string& a, const std::string& b) {
            return QString::fromStdString(a).toCaseFolded() <
                   QString::fromStdString(b).toCaseFolded();
        };

        std::stable_sort(contestants_.begin(), contestants_.end(),
                         [&](const std::string& a, const std::string& b) {
            int cmp = 0;
            if (section == 0) {
                cmp = name_less(a, b) ? -1 : (name_less(b, a) ? 1 : 0);
            } else if (section == total_column()) {
                double av = total_earned_for(a);
                double bv = total_earned_for(b);
                cmp = av < bv ? -1 : (av > bv ? 1 : 0);
            } else {
                std::string problem = problems_[static_cast<std::size_t>(section - 1)];
                double av = earned_for_problem(a, problem);
                double bv = earned_for_problem(b, problem);
                cmp = av < bv ? -1 : (av > bv ? 1 : 0);
            }
            if (cmp == 0) {
                cmp = name_less(a, b) ? -1 : (name_less(b, a) ? 1 : 0);
            }
            return sort_ascending_ ? cmp < 0 : cmp > 0;
        });
        populate_table();
    }

    void show_header_menu(const QPoint& pos) {
        int section = table_->horizontalHeader()->logicalIndexAt(pos);
        if (section <= 0 || section > static_cast<int>(problems_.size())) {
            return;
        }
        std::string problem = problems_[static_cast<std::size_t>(section - 1)];
        QMenu menu(this);
        menu.addAction("Judge this problem for selected contestants",
                       [this, problem]() { start_judge(true, problem); });
        menu.addAction("Judge this problem for all contestants",
                       [this, problem]() { start_judge(false, problem); });
        menu.exec(table_->horizontalHeader()->mapToGlobal(pos));
    }

    void handle_progress_line(const std::string& raw) {
        QString line = QString::fromStdString(raw);
        QStringList parts = line.split(' ', Qt::SkipEmptyParts);
        if (parts.size() < 4 || parts.value(0) != "progress") {
            log_->appendPlainText(line);
            return;
        }
        QString counts;
        for (const QString& part : parts) {
            if (part.contains('/')) {
                counts = part;
                break;
            }
        }
        QStringList split = counts.split('/');
        if (split.size() != 2) {
            return;
        }
        bool done_ok = false;
        bool total_ok = false;
        int done = split[0].toInt(&done_ok);
        int total = split[1].toInt(&total_ok);
        if (!done_ok || !total_ok || total <= 0) {
            return;
        }
        progress_->setRange(0, total);
        progress_->setValue(done);
        QString phase = parts.value(1);
        QString elapsed;
        int elapsed_index = parts.indexOf("elapsed");
        if (elapsed_index >= 0 && elapsed_index + 1 < parts.size()) {
            elapsed = parts.value(elapsed_index + 1);
        }
        progress_label_->setText(phase + " " + QString::number(done) + "/" +
                                 QString::number(total) +
                                 (elapsed.isEmpty() ? QString() : " elapsed " + elapsed));
        run_status_->setText(line);
    }

    void start_judge(bool selected_only, const std::string& selected_problem = {}) {
        if (judging_.load()) {
            run_status_->setText("A judge run is already active");
            return;
        }
        if (contest_root_.empty()) {
            QMessageBox::information(this, "No contest", "Open or create a contest folder first.");
            return;
        }
        neothemis::JudgeOptions options = options_from_ui();
        if (!selected_problem.empty()) {
            options.selected_problems.push_back(selected_problem);
        }
        std::vector<std::string> selected;
        if (selected_only) {
            selected = selected_contestants();
            if (selected.empty()) {
                QMessageBox::information(this, "No selection", "Select one or more contestant rows.");
                return;
            }
            options.selected_contestants = selected;
        }
        if (judge_thread_.joinable()) {
            judge_thread_.join();
        }
        judging_.store(true);
        cancel_requested_.store(false);
        set_judge_controls_enabled(false);
        reset_run_cells(selected, selected_problem);
        progress_->setRange(0, 0);
        progress_label_->setText("Starting");
        QString scope = selected_only ? "selected contestants" : "all contestants";
        run_status_->setText(selected_problem.empty()
                                 ? "Judging " + scope
                                 : "Judging " + QString::fromStdString(selected_problem) +
                                       " for " + scope);
        log_->appendPlainText("Judging...");

        judge_thread_ = std::thread([this, options]() mutable {
            try {
                options.should_cancel = [this]() {
                    return cancel_requested_.load();
                };
                options.progress = [this](const std::string& line) {
                    QMetaObject::invokeMethod(this, [this, line]() {
                        handle_progress_line(line);
                    }, Qt::QueuedConnection);
                };
                options.result = [this](const neothemis::TestResult& result) {
                    QMetaObject::invokeMethod(this, [this, result]() {
                        handle_result(result);
                    }, Qt::QueuedConnection);
                };

                auto core = neothemis::make_judge_core(options.core_name);
                core->judge(options);
                QMetaObject::invokeMethod(this, [this]() {
                    try {
                        write_current_csv_outputs();
                        progress_->setRange(0, 100);
                        progress_->setValue(100);
                        progress_label_->setText("Done");
                        run_status_->setText("Judge run complete");
                        log_->appendPlainText("Done.");
                    } catch (const std::exception& ex) {
                        progress_->setRange(0, 100);
                        progress_->setValue(0);
                        progress_label_->setText("Failed");
                        run_status_->setText("Judge run finished but CSV write failed");
                        log_->appendPlainText(QString("CSV write failed: ") +
                                              QString::fromUtf8(ex.what()));
                        QMessageBox::critical(this, "CSV write failed", ex.what());
                    }
                    judging_.store(false);
                    set_judge_controls_enabled(true);
                }, Qt::QueuedConnection);
            } catch (const std::exception& ex) {
                bool cancelled = cancel_requested_.load();
                QMetaObject::invokeMethod(this, [this, message = QString::fromUtf8(ex.what()), cancelled]() {
                    progress_->setRange(0, 100);
                    progress_->setValue(0);
                    progress_label_->setText(cancelled ? "Cancelled" : "Failed");
                    run_status_->setText(cancelled ? "Judge run cancelled" : "Judge run failed");
                    log_->appendPlainText(cancelled ? QString("Cancelled.")
                                                    : QString("Failed: ") + message);
                    judging_.store(false);
                    set_judge_controls_enabled(true);
                    if (!cancelled) {
                        QMessageBox::critical(this, "Judge failed", message);
                    }
                }, Qt::QueuedConnection);
            }
        });
    }

    void set_judge_controls_enabled(bool enabled) {
        if (judge_selected_button_) {
            judge_selected_button_->setEnabled(enabled);
        }
        if (judge_all_button_) {
            judge_all_button_->setEnabled(enabled);
        }
        if (judge_selected_action_) {
            judge_selected_action_->setEnabled(enabled);
        }
        if (judge_all_action_) {
            judge_all_action_->setEnabled(enabled);
        }
        if (stop_button_) {
            stop_button_->setEnabled(!enabled);
        }
        if (stop_action_) {
            stop_action_->setEnabled(!enabled);
        }
    }

    void request_stop_judge() {
        if (!judging_.load()) {
            return;
        }
        cancel_requested_.store(true);
        run_status_->setText("Stopping active judge run...");
        progress_label_->setText("Stopping");
        if (stop_button_) {
            stop_button_->setEnabled(false);
        }
        if (stop_action_) {
            stop_action_->setEnabled(false);
        }
    }

    void stop_active_judge() {
        cancel_requested_.store(true);
        if (judge_thread_.joinable()) {
            judge_thread_.join();
        }
        judging_.store(false);
    }

    QWidget* build_visual_tab(QWidget* parent) {
        auto* tab = new QWidget(parent);
        auto* form = new QFormLayout(tab);
        auto* theme = new QComboBox(tab);
        theme->addItem("Dark");
        form->addRow("Theme", theme);
        return tab;
    }

    QWidget* build_contest_tab(QWidget* parent) {
        auto* tab = new QWidget(parent);
        auto* form = new QFormLayout(tab);
        auto* compiler = new QLineEdit(QString::fromStdString(compiler_), tab);
        auto* flags = new QLineEdit(QString::fromStdString(compile_flags_), tab);
        auto* contestants = new QLineEdit(QString::fromStdString(contestants_dir_), tab);
        auto* tests = new QLineEdit(QString::fromStdString(tests_dir_), tab);
        auto* stack = new QSpinBox(tab);
        stack->setRange(0, 1024 * 1024);
        stack->setValue(static_cast<int>(stack_limit_mb_));
        auto* parallel = new QSpinBox(tab);
        parallel->setRange(0, 256);
        parallel->setValue(static_cast<int>(parallel_jobs_));
        auto* keep = new QCheckBox(tab);
        keep->setChecked(keep_workdir_);
        auto* save = new QPushButton("Save Contest Config", tab);

        form->addRow("Compiler", compiler);
        form->addRow("Compile flags", flags);
        form->addRow("Contestants dir", contestants);
        form->addRow("Tests dir", tests);
        form->addRow("Stack MB", stack);
        form->addRow("Parallel jobs (0 = auto)", parallel);
        form->addRow("Keep workdir", keep);
        form->addRow(save);

        QObject::connect(save, &QPushButton::clicked, [this, compiler, flags,
                                                       contestants, tests, stack, parallel, keep]() {
            compiler_ = compiler->text().toStdString();
            compile_flags_ = flags->text().toStdString();
            contestants_dir_ = contestants->text().toStdString();
            tests_dir_ = tests->text().toStdString();
            stack_limit_mb_ = static_cast<std::uint64_t>(stack->value());
            parallel_jobs_ = static_cast<unsigned int>(parallel->value());
            keep_workdir_ = keep->isChecked();
            try {
                save_contest_config();
                refresh_table();
                log_->appendPlainText("Saved contest config.");
            } catch (const std::exception& ex) {
                QMessageBox::critical(this, "Save failed", ex.what());
            }
        });
        return tab;
    }

    QWidget* build_problem_tab(QWidget* parent) {
        auto* tab = new QWidget(parent);
        auto* form = new QFormLayout(tab);
        auto* problem = new QComboBox(tab);
        for (const auto& name : problems_) {
            problem->addItem(QString::fromStdString(name));
        }
        auto* time = new QSpinBox(tab);
        time->setRange(1, 60 * 60 * 1000);
        auto* memory = new QSpinBox(tab);
        memory->setRange(0, 1024 * 1024);
        auto* points = new QLineEdit(tab);
        auto* checker = new QLineEdit(tab);
        auto* save = new QPushButton("Save Problem Config", tab);

        auto load_problem = [this, problem, time, memory, points, checker]() {
            std::string name = problem->currentText().toStdString();
            auto values = read_config_file(contest_root_ / tests_dir_ / name / "problem.conf");
            time->setValue(values.count("time_limit_ms") ? std::stoi(values["time_limit_ms"]) : 1000);
            memory->setValue(values.count("memory_limit_mb") ? std::stoi(values["memory_limit_mb"]) : 256);
            points->setText(QString::fromStdString(values.count("default_points") ? values["default_points"] : "1"));
            checker->setText(QString::fromStdString(values.count("checker") ? values["checker"] : "token"));
        };
        QObject::connect(problem, &QComboBox::currentTextChanged, [load_problem]() { load_problem(); });
        QObject::connect(save, &QPushButton::clicked, [this, problem, time, memory, points, checker]() {
            try {
                std::string name = problem->currentText().toStdString();
                fs::create_directories(contest_root_ / tests_dir_ / name);
                write_problem_config(contest_root_ / tests_dir_ / name / "problem.conf",
                                     time->value(), memory->value(),
                                     points->text().toStdString(), checker->text().toStdString());
                log_->appendPlainText("Saved problem config for " + problem->currentText());
            } catch (const std::exception& ex) {
                QMessageBox::critical(this, "Save failed", ex.what());
            }
        });

        form->addRow("Problem", problem);
        form->addRow("Time limit ms", time);
        form->addRow("Memory MB", memory);
        form->addRow("Default points", points);
        form->addRow("Checker", checker);
        form->addRow(save);
        if (problem->count() > 0) {
            load_problem();
        }
        return tab;
    }

    void open_settings_dialog(int initial_tab) {
        auto* dialog = new QDialog(this);
        dialog->setWindowTitle("Settings");
        dialog->resize(560, 420);
        auto* layout = new QVBoxLayout(dialog);
        auto* tabs = new QTabWidget(dialog);
        tabs->addTab(build_visual_tab(tabs), "Application");
        tabs->addTab(build_contest_tab(tabs), "Contest");
        tabs->addTab(build_problem_tab(tabs), "Problems");
        tabs->setCurrentIndex(initial_tab);
        layout->addWidget(tabs);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
        layout->addWidget(buttons);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    }

    void show_about_dialog() {
        auto* dialog = new QDialog(this);
        dialog->setWindowTitle("About NeoThemis");
        dialog->resize(520, 360);
        auto* layout = new QVBoxLayout(dialog);

        auto* title = new QLabel("NeoThemis", dialog);
        title->setObjectName("WindowAppName");
        auto* details = new QPlainTextEdit(dialog);
        details->setReadOnly(true);
        details->setPlainText(
            "NeoThemis\n\n"
            "A local competitive-programming contest judge for C++ submissions.\n\n"
            "Features:\n"
            "- Contest and per-problem configuration\n"
            "- Parallel judging with live progress\n"
            "- Custom checkers stored in each problem folder\n"
            "- CSV result and scoreboard output\n"
            "- Qt desktop interface for Windows and Linux\n\n"
            "Checker note:\n"
            "Custom checkers that include testlib.h must keep testlib.h in the same problem folder.\n\n"
            "Build: Qt Widgets desktop application");
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);

        layout->addWidget(title);
        layout->addWidget(details, 1);
        layout->addWidget(buttons);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    }

    fs::path contest_root_;
    std::vector<std::string> contestants_;
    std::vector<std::string> problems_;
    std::map<std::string, int> contestant_rows_;
    std::map<std::string, int> problem_columns_;
    std::map<std::string, int> problem_test_counts_;
    std::map<std::string, bool> source_ready_;
    std::map<std::string, CellScore> score_cells_;
    std::map<std::string, QString> cell_texts_;
    std::map<std::string, std::vector<neothemis::TestResult>> result_details_;
    int sort_column_ = 0;
    bool sort_ascending_ = true;

    std::string compiler_ = "g++";
    std::string compile_flags_ = "-std=c++17 -O2 -pipe";
    std::string contestants_dir_ = "contestants";
    std::string tests_dir_ = "tests";
    std::uint64_t stack_limit_mb_ = 64;
    unsigned int parallel_jobs_ = 0;
    bool keep_workdir_ = false;
    std::atomic_bool judging_{false};
    std::atomic_bool cancel_requested_{false};
    std::thread judge_thread_;

    QTableWidget* table_ = nullptr;
    QWidget* title_bar_ = nullptr;
    QMenuBar* menu_bar_ = nullptr;
    QLabel* contest_title_ = nullptr;
    QPushButton* judge_selected_button_ = nullptr;
    QPushButton* judge_all_button_ = nullptr;
    QPushButton* stop_button_ = nullptr;
    QAction* judge_selected_action_ = nullptr;
    QAction* judge_all_action_ = nullptr;
    QAction* stop_action_ = nullptr;
    QLabel* run_status_ = nullptr;
    QLabel* progress_label_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QPoint drag_offset_;
    bool dragging_title_bar_ = false;
};

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    MainWindow window;
    window.show();
    return app.exec();
}
