#include "MainWindow.hpp"
#include "MainWindowPrivate.hpp"

#include <utility>

namespace neothemis::gui {

std::unique_ptr<QMainWindow> create_main_window(std::filesystem::path initial_contest) {
    return std::make_unique<MainWindow>(std::move(initial_contest));
}

} // namespace neothemis::gui
