#include "WindowResizeHandles.hpp"

#include <QEvent>
#include <QMouseEvent>
#include <QObject>
#include <QWindow>
#include <QWidget>

#include <array>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr int kResizeBorderWidth = 7;

void enable_windows_aero_snap(QWidget* window) {
#ifdef Q_OS_WIN
    if (!window) {
        return;
    }
    const HWND handle = reinterpret_cast<HWND>(window->winId());
    if (!handle) {
        return;
    }
    constexpr LONG_PTR required_styles =
        WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
    const LONG_PTR current_styles = GetWindowLongPtrW(handle, GWL_STYLE);
    if ((current_styles & required_styles) == required_styles) {
        return;
    }
    SetWindowLongPtrW(handle, GWL_STYLE, current_styles | required_styles);
    SetWindowPos(handle, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
#else
    (void)window;
#endif
}

Qt::CursorShape resize_cursor(Qt::Edges edges) {
    if (edges == (Qt::TopEdge | Qt::LeftEdge) ||
        edges == (Qt::BottomEdge | Qt::RightEdge)) {
        return Qt::SizeFDiagCursor;
    }
    if (edges == (Qt::TopEdge | Qt::RightEdge) ||
        edges == (Qt::BottomEdge | Qt::LeftEdge)) {
        return Qt::SizeBDiagCursor;
    }
    if (edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge)) {
        return Qt::SizeHorCursor;
    }
    return Qt::SizeVerCursor;
}

class ResizeHandle final : public QWidget {
public:
    ResizeHandle(QWidget* target, Qt::Edges edges)
        : QWidget(target), target_(target), edges_(edges) {
        setCursor(resize_cursor(edges));
        setStyleSheet("background: transparent; border: 0;");
        setAttribute(Qt::WA_NoSystemBackground);
    }

    Qt::Edges edges() const {
        return edges_;
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && !target_->isMaximized() &&
            !target_->isFullScreen()) {
            QWindow* native_window = target_->windowHandle();
            if (native_window && native_window->startSystemResize(edges_)) {
                event->accept();
                return;
            }
        }
        QWidget::mousePressEvent(event);
    }

private:
    QWidget* target_ = nullptr;
    Qt::Edges edges_;
};

class WindowResizeHandles final : public QObject {
public:
    explicit WindowResizeHandles(QWidget* window) : QObject(window), window_(window) {
        constexpr std::array<Qt::Edges, 8> edges = {
            Qt::Edges(Qt::TopEdge),
            Qt::Edges(Qt::BottomEdge),
            Qt::Edges(Qt::LeftEdge),
            Qt::Edges(Qt::RightEdge),
            Qt::TopEdge | Qt::LeftEdge,
            Qt::TopEdge | Qt::RightEdge,
            Qt::BottomEdge | Qt::LeftEdge,
            Qt::BottomEdge | Qt::RightEdge
        };
        for (std::size_t index = 0; index < edges.size(); ++index) {
            handles_[index] = new ResizeHandle(window_, edges[index]);
        }
        window_->installEventFilter(this);
        enable_windows_aero_snap(window_);
        update_handles();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == window_ &&
            (event->type() == QEvent::Show ||
             event->type() == QEvent::WinIdChange)) {
            enable_windows_aero_snap(window_);
        }
        if (watched == window_ &&
            (event->type() == QEvent::Resize || event->type() == QEvent::Show ||
             event->type() == QEvent::WindowStateChange)) {
            update_handles();
        }
        return QObject::eventFilter(watched, event);
    }

private:
    void update_handles() {
        const int width = window_->width();
        const int height = window_->height();
        const int border = kResizeBorderWidth;
        const int inner_width = qMax(0, width - border * 2);
        const int inner_height = qMax(0, height - border * 2);
        const bool visible = !window_->isMaximized() && !window_->isFullScreen();

        for (ResizeHandle* handle : handles_) {
            const Qt::Edges edges = handle->edges();
            if (edges == Qt::Edges(Qt::TopEdge)) {
                handle->setGeometry(border, 0, inner_width, border);
            } else if (edges == Qt::Edges(Qt::BottomEdge)) {
                handle->setGeometry(border, height - border, inner_width, border);
            } else if (edges == Qt::Edges(Qt::LeftEdge)) {
                handle->setGeometry(0, border, border, inner_height);
            } else if (edges == Qt::Edges(Qt::RightEdge)) {
                handle->setGeometry(width - border, border, border, inner_height);
            } else if (edges == (Qt::TopEdge | Qt::LeftEdge)) {
                handle->setGeometry(0, 0, border, border);
            } else if (edges == (Qt::TopEdge | Qt::RightEdge)) {
                handle->setGeometry(width - border, 0, border, border);
            } else if (edges == (Qt::BottomEdge | Qt::LeftEdge)) {
                handle->setGeometry(0, height - border, border, border);
            } else {
                handle->setGeometry(width - border, height - border, border, border);
            }
            handle->setVisible(visible);
            handle->raise();
        }
    }

    QWidget* window_ = nullptr;
    std::array<ResizeHandle*, 8> handles_{};
};

} // namespace

namespace neothemis::gui {

void install_windows_resize_handles(QWidget* window) {
#ifdef Q_OS_WIN
    new WindowResizeHandles(window);
#else
    (void)window;
#endif
}

void toggle_window_maximized(QWidget* window) {
    if (!window) {
        return;
    }
#ifdef Q_OS_WIN
    const HWND handle = reinterpret_cast<HWND>(window->winId());
    if (handle) {
        ShowWindow(handle, IsZoomed(handle) ? SW_RESTORE : SW_MAXIMIZE);
        return;
    }
#endif
    window->isMaximized() ? window->showNormal() : window->showMaximized();
}

} // namespace neothemis::gui
