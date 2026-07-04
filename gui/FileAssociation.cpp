#include "FileAssociation.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

#include <stdexcept>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#endif

namespace {

constexpr auto kContestMimeType = "application/x-neothemis-contest";
constexpr auto kDesktopFileName = "neothemis-gui.desktop";

#ifdef Q_OS_WIN

QString executable_path() {
    return QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
}

QString windows_open_command() {
    return QStringLiteral("\"") + executable_path() + QStringLiteral("\" \"%1\"");
}

#elif defined(Q_OS_LINUX)

QString user_data_root() {
    QString root = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (root.isEmpty()) {
        root = QDir::homePath() + QStringLiteral("/.local/share");
    }
    return root;
}

QString desktop_exec_argument(QString value) {
    value.replace(QStringLiteral("%"), QStringLiteral("%%"));
    value.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    value.replace(QStringLiteral("\""), QStringLiteral("\\\""));
    value.replace(QStringLiteral("`"), QStringLiteral("\\`"));
    value.replace(QStringLiteral("$"), QStringLiteral("\\$"));
    return QStringLiteral("\"") + value + QStringLiteral("\"");
}

QString desktop_file_path() {
    return user_data_root() + QStringLiteral("/applications/") + kDesktopFileName;
}

QString mime_package_path() {
    return user_data_root() + QStringLiteral("/mime/packages/neothemis.xml");
}

QByteArray desktop_file_contents() {
    return QStringLiteral(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=NeoThemis\n"
        "Comment=Open a NeoThemis contest\n"
        "Exec=%1 %f\n"
        "Terminal=false\n"
        "MimeType=%2;\n"
        "Categories=Development;\n")
        .arg(desktop_exec_argument(QCoreApplication::applicationFilePath()),
             QString::fromLatin1(kContestMimeType))
        .toUtf8();
}

void write_user_file(const QString& path, const QByteArray& contents) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        throw std::runtime_error("failed to create the file association directory");
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() ||
        !file.commit()) {
        throw std::runtime_error("failed to write " + path.toStdString());
    }
}

void run_if_available(const QString& program, const QStringList& arguments) {
    const QString executable = QStandardPaths::findExecutable(program);
    if (!executable.isEmpty()) {
        QProcess::execute(executable, arguments);
    }
}

#endif

} // namespace

namespace neothemis::gui {

bool contest_file_association_is_registered() {
#ifdef Q_OS_WIN
    QSettings classes(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"),
                      QSettings::NativeFormat);
    return classes.value(QStringLiteral(".ncontest/Default")).toString() ==
               QStringLiteral("NeoThemis.Contest") &&
           classes.value(QStringLiteral(
               "NeoThemis.Contest/shell/open/command/Default")).toString() ==
               windows_open_command();
#elif defined(Q_OS_LINUX)
    QFile desktop_file(desktop_file_path());
    if (!desktop_file.open(QIODevice::ReadOnly)) {
        return false;
    }
    return desktop_file.readAll() == desktop_file_contents();
#else
    return false;
#endif
}

void register_contest_file_association() {
#ifdef Q_OS_WIN
    QSettings classes(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"),
                      QSettings::NativeFormat);
    const QString command = windows_open_command();
    classes.setValue(QStringLiteral(".ncontest/Default"),
                     QStringLiteral("NeoThemis.Contest"));
    classes.setValue(QStringLiteral(
                         ".ncontest/OpenWithProgids/NeoThemis.Contest"),
                     QString());
    classes.setValue(QStringLiteral("NeoThemis.Contest/Default"),
                     QStringLiteral("NeoThemis Contest"));
    classes.setValue(QStringLiteral("NeoThemis.Contest/DefaultIcon/Default"),
                     QStringLiteral("\"") + executable_path() +
                         QStringLiteral("\",0"));
    classes.setValue(QStringLiteral("NeoThemis.Contest/shell/open/command/Default"),
                     command);
    classes.setValue(QStringLiteral(
                         "Applications/neothemis-gui.exe/shell/open/command/Default"),
                     command);
    classes.setValue(QStringLiteral("Applications/neothemis-gui.exe/FriendlyAppName"),
                     QStringLiteral("NeoThemis"));
    classes.setValue(QStringLiteral(
                         "Applications/neothemis-gui.exe/SupportedTypes/.ncontest"),
                     QString());
    classes.sync();
    if (classes.status() != QSettings::NoError) {
        throw std::runtime_error("failed to update the Windows file association");
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
#elif defined(Q_OS_LINUX)
    const QByteArray mime_contents = QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<mime-info xmlns=\"http://www.freedesktop.org/standards/shared-mime-info\">\n"
        "  <mime-type type=\"application/x-neothemis-contest\">\n"
        "    <comment>NeoThemis contest</comment>\n"
        "    <glob pattern=\"*.ncontest\"/>\n"
        "  </mime-type>\n"
        "</mime-info>\n");
    write_user_file(desktop_file_path(), desktop_file_contents());
    write_user_file(mime_package_path(), mime_contents);

    const QString data_root = user_data_root();
    run_if_available(QStringLiteral("update-mime-database"),
                     {data_root + QStringLiteral("/mime")});
    run_if_available(QStringLiteral("update-desktop-database"),
                     {data_root + QStringLiteral("/applications")});
    run_if_available(QStringLiteral("xdg-mime"),
                     {QStringLiteral("default"), QString::fromLatin1(kDesktopFileName),
                      QString::fromLatin1(kContestMimeType)});
#else
    throw std::runtime_error("file association is not supported on this platform");
#endif
}

} // namespace neothemis::gui
