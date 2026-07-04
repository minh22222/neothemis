#pragma once

#include <filesystem>
#include <memory>

class QMainWindow;

namespace neothemis::gui {

std::unique_ptr<QMainWindow> create_main_window(
    std::filesystem::path initial_contest = {});

} // namespace neothemis::gui
