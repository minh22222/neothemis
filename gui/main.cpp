#include "MainWindow.hpp"
#include "FileAssociation.hpp"
#include "GuiSupport.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QIcon>
#include <QMainWindow>

#include <exception>
#include <utility>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("NeoThemis");
    QCoreApplication::setOrganizationName("NeoThemis");
    app.setWindowIcon(QIcon(":/materials/logo.png"));

    QCommandLineParser parser;
    parser.setApplicationDescription("NeoThemis competitive programming judge");
    parser.addHelpOption();
    QCommandLineOption register_association(
        QStringLiteral("register-file-association"),
        QStringLiteral("Register NeoThemis as the user-level opener for .ncontest files."));
    parser.addOption(register_association);
    parser.addPositionalArgument("contest", "A .ncontest file to open.", "[contest.ncontest]");
    parser.process(app);

    if (parser.isSet(register_association)) {
        try {
            neothemis::gui::register_contest_file_association();
            return 0;
        } catch (const std::exception& ex) {
            qCritical().noquote() << ex.what();
            return 1;
        }
    }

    const QStringList positional = parser.positionalArguments();
    if (positional.size() > 1) {
        parser.showHelp(2);
    }
    std::filesystem::path initial_contest;
    if (!positional.isEmpty()) {
        initial_contest = neothemis::gui::path_from_qstring(positional.front());
    }

    auto window = neothemis::gui::create_main_window(std::move(initial_contest));
    window->show();
    return app.exec();
}
