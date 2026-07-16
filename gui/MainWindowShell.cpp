#include "MainWindowPrivate.hpp"

#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>

namespace {

enum class SettingsTabIcon {
    Application,
    Contest,
    Problems,
    Server,
    Users,
};

class SettingsTabIconEngine final : public QIconEngine {
public:
    explicit SettingsTabIconEngine(SettingsTabIcon icon) : icon_(icon) {}

    QIconEngine* clone() const override {
        return new SettingsTabIconEngine(icon_);
    }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode,
               QIcon::State) override {
        if (!painter || rect.isEmpty()) {
            return;
        }
        QColor main = mode == QIcon::Selected ? QColor(246, 220, 227)
                                               : QColor(178, 193, 201);
        QColor accent = mode == QIcon::Selected ? QColor(105, 225, 209)
                                                 : QColor(105, 166, 160);
        if (mode == QIcon::Active) {
            main = QColor(224, 237, 241);
            accent = QColor(116, 218, 205);
        } else if (mode == QIcon::Disabled) {
            main.setAlpha(105);
            accent.setAlpha(90);
        }

        const qreal side = std::min(rect.width(), rect.height());
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->translate(rect.center());
        painter->scale(side / 24.0, side / 24.0);
        painter->translate(-12.0, -12.0);
        QPen main_pen(main, 1.75, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        QPen accent_pen(accent, 1.75, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter->setBrush(Qt::NoBrush);

        switch (icon_) {
        case SettingsTabIcon::Application:
            painter->setPen(main_pen);
            painter->drawLine(QPointF(4.0, 6.0), QPointF(20.0, 6.0));
            painter->drawLine(QPointF(4.0, 12.0), QPointF(20.0, 12.0));
            painter->drawLine(QPointF(4.0, 18.0), QPointF(20.0, 18.0));
            painter->setPen(accent_pen);
            painter->setBrush(QColor(accent.red(), accent.green(), accent.blue(), 54));
            painter->drawEllipse(QPointF(9.0, 6.0), 2.0, 2.0);
            painter->drawEllipse(QPointF(15.0, 12.0), 2.0, 2.0);
            painter->drawEllipse(QPointF(7.0, 18.0), 2.0, 2.0);
            break;
        case SettingsTabIcon::Contest: {
            painter->setPen(main_pen);
            QPainterPath cup;
            cup.moveTo(7.0, 5.0);
            cup.lineTo(17.0, 5.0);
            cup.lineTo(16.3, 10.0);
            cup.cubicTo(15.9, 13.0, 14.2, 14.5, 12.0, 14.5);
            cup.cubicTo(9.8, 14.5, 8.1, 13.0, 7.7, 10.0);
            cup.closeSubpath();
            painter->drawPath(cup);
            painter->drawArc(QRectF(3.5, 6.5, 6.0, 5.5), 90 * 16, 180 * 16);
            painter->drawArc(QRectF(14.5, 6.5, 6.0, 5.5), -90 * 16, 180 * 16);
            painter->drawLine(QPointF(12.0, 14.5), QPointF(12.0, 18.0));
            painter->drawLine(QPointF(8.0, 20.0), QPointF(16.0, 20.0));
            painter->setPen(accent_pen);
            painter->drawLine(QPointF(10.0, 18.0), QPointF(14.0, 18.0));
            break;
        }
        case SettingsTabIcon::Problems:
            painter->setPen(main_pen);
            painter->drawPolyline(
                QPolygonF({QPointF(9.0, 6.0), QPointF(4.0, 12.0), QPointF(9.0, 18.0)}));
            painter->drawPolyline(
                QPolygonF({QPointF(15.0, 6.0), QPointF(20.0, 12.0), QPointF(15.0, 18.0)}));
            painter->setPen(accent_pen);
            painter->drawLine(QPointF(14.0, 4.5), QPointF(10.0, 19.5));
            break;
        case SettingsTabIcon::Server:
            painter->setPen(main_pen);
            painter->drawRoundedRect(QRectF(4.0, 4.0, 16.0, 6.0), 2.0, 2.0);
            painter->drawRoundedRect(QRectF(4.0, 14.0, 16.0, 6.0), 2.0, 2.0);
            painter->drawLine(QPointF(10.0, 7.0), QPointF(17.0, 7.0));
            painter->drawLine(QPointF(10.0, 17.0), QPointF(17.0, 17.0));
            painter->setPen(accent_pen);
            painter->setBrush(accent);
            painter->drawEllipse(QPointF(7.0, 7.0), 0.9, 0.9);
            painter->drawEllipse(QPointF(7.0, 17.0), 0.9, 0.9);
            break;
        case SettingsTabIcon::Users: {
            painter->setPen(main_pen);
            painter->drawEllipse(QPointF(9.0, 8.0), 3.0, 3.0);
            QPainterPath primary_user;
            primary_user.moveTo(3.8, 19.0);
            primary_user.cubicTo(4.2, 15.3, 6.1, 13.5, 9.0, 13.5);
            primary_user.cubicTo(11.9, 13.5, 13.8, 15.3, 14.2, 19.0);
            painter->drawPath(primary_user);
            painter->setPen(accent_pen);
            painter->drawEllipse(QPointF(16.5, 9.0), 2.2, 2.2);
            QPainterPath secondary_user;
            secondary_user.moveTo(14.5, 14.0);
            secondary_user.cubicTo(18.1, 13.4, 20.0, 15.0, 20.3, 18.0);
            painter->drawPath(secondary_user);
            break;
        }
        }
        painter->restore();
    }

private:
    SettingsTabIcon icon_;
};

QIcon settings_tab_icon(SettingsTabIcon icon) {
    return QIcon(new SettingsTabIconEngine(icon));
}

} // namespace

namespace neothemis::gui {

AnimatedTabBar::AnimatedTabBar(QWidget* parent) : QTabBar(parent) {
    setObjectName("SettingsTabBar");
    setDrawBase(false);
    setExpanding(false);
    setUsesScrollButtons(true);
    setIconSize(QSize(20, 20));
}

void AnimatedTabBar::setAnimationsEnabled(bool enabled) {
    stopIndicatorAnimation();
    animations_enabled_ = enabled;
    indicator_rect_ = currentIndex() >= 0 ? QRectF(tabRect(currentIndex())) : QRectF();
    update();
}

void AnimatedTabBar::slideToIndex(int index) {
    if (!animations_enabled_ || index < 0 || index >= count()) {
        return;
    }
    const QRectF target(tabRect(index));
    if (!indicator_rect_.isValid()) {
        indicator_rect_ = target;
        update();
        return;
    }

    stopIndicatorAnimation();
    auto* animation = new QVariantAnimation(this);
    animation->setDuration(220);
    animation->setStartValue(indicator_rect_);
    animation->setEndValue(target);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    indicator_animation_ = animation;
    QObject::connect(animation, &QVariantAnimation::valueChanged, this,
                     [this](const QVariant& value) {
        indicator_rect_ = value.toRectF();
        update();
    });
    QObject::connect(animation, &QVariantAnimation::finished, this,
                     [this, animation, target]() {
        if (indicator_animation_ != animation) {
            return;
        }
        indicator_rect_ = target;
        indicator_animation_ = nullptr;
        animation->deleteLater();
        update();
    });
    animation->start();
}

void AnimatedTabBar::setCenteredIcon(int index, const QIcon& icon) {
    if (index < 0) {
        return;
    }
    if (static_cast<std::size_t>(index) >= centered_icons_.size()) {
        centered_icons_.resize(static_cast<std::size_t>(index) + 1);
    }
    centered_icons_[static_cast<std::size_t>(index)] = icon;
    update(tabRect(index));
}

void AnimatedTabBar::stopIndicatorAnimation() {
    if (!indicator_animation_) {
        return;
    }
    QObject::disconnect(indicator_animation_, nullptr, this, nullptr);
    indicator_animation_->stop();
    delete indicator_animation_;
    indicator_animation_ = nullptr;
}

void AnimatedTabBar::paintEvent(QPaintEvent* event) {
    if (currentIndex() >= 0 && (!animations_enabled_ || !indicator_rect_.isValid())) {
        indicator_rect_ = QRectF(tabRect(currentIndex()));
    } else if (currentIndex() >= 0 && !indicator_animation_) {
        indicator_rect_ = QRectF(tabRect(currentIndex()));
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (count() > 0) {
        QRectF rail = QRectF(tabRect(0)).united(QRectF(tabRect(count() - 1)));
        rail.adjust(1.0, 1.0, -1.0, -1.0);
        QColor rail_fill = qApp->property("neothemisTabRailFill").value<QColor>();
        QColor rail_border = qApp->property("neothemisTabRailBorder").value<QColor>();
        if (!rail_fill.isValid()) {
            rail_fill = QColor(6, 11, 17, 170);
        }
        if (!rail_border.isValid()) {
            rail_border = QColor(110, 226, 210, 54);
        }
        painter.setPen(QPen(rail_border, 1.0));
        painter.setBrush(rail_fill);
        painter.drawRoundedRect(rail, 12.0, 12.0);
    }

    if (indicator_rect_.isValid()) {
        const QRectF rect = indicator_rect_.adjusted(4.0, 4.0, -4.0, -4.0);
        const qreal radius = std::min<qreal>(10.0, rect.height() / 2.0);

        QColor fill = qApp->property("neothemisTabIndicatorFill").value<QColor>();
        QColor border = qApp->property("neothemisTabIndicatorBorder").value<QColor>();
        if (!fill.isValid()) {
            fill = QColor(40, 61, 69, 230);
        }
        if (!border.isValid()) {
            border = QColor(110, 226, 210, 150);
        }
        QLinearGradient surface(rect.topLeft(), rect.bottomLeft());
        QColor highlight = fill.lighter(112);
        highlight.setAlpha(fill.alpha());
        surface.setColorAt(0.0, highlight);
        surface.setColorAt(1.0, fill);
        painter.setPen(QPen(border, 1.0));
        painter.setBrush(surface);
        painter.drawRoundedRect(rect, radius, radius);

        QColor accent = border;
        accent.setAlpha(220);
        const qreal accent_width = std::min<qreal>(30.0, rect.width() * 0.34);
        const qreal accent_y = rect.bottom() - 2.0;
        painter.setPen(QPen(accent, 2.0, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(rect.center().x() - accent_width / 2.0, accent_y),
                         QPointF(rect.center().x() + accent_width / 2.0, accent_y));
    }
    painter.end();
    QTabBar::paintEvent(event);
    QPainter icon_painter(this);
    icon_painter.setRenderHint(QPainter::Antialiasing, true);
    for (int index = 0; index < count(); ++index) {
        if (static_cast<std::size_t>(index) >= centered_icons_.size()) {
            continue;
        }
        const QIcon& icon = centered_icons_[static_cast<std::size_t>(index)];
        if (icon.isNull()) {
            continue;
        }
        const QRect tab = tabRect(index);
        const QSize size = iconSize();
        const QRect icon_rect(tab.center().x() - size.width() / 2,
                              tab.center().y() - size.height() / 2,
                              size.width(), size.height());
        const QIcon::Mode mode = index == currentIndex() ? QIcon::Selected
                                                         : QIcon::Normal;
        icon.paint(&icon_painter, icon_rect, Qt::AlignCenter, mode, QIcon::On);
    }
}

SettingsTabWidget::SettingsTabWidget(QWidget* parent) : QTabWidget(parent) {
    setTabBar(new AnimatedTabBar(this));
}

SettingsDialog::~SettingsDialog() {
    QObject::disconnect(tab_change_connection_);
    stop_tab_animation();
}

void SettingsDialog::set_animated_tabs(QTabWidget* tabs) {
    if (animated_tabs_ == tabs) {
        return;
    }
    const bool reconnect = tab_animations_enabled_;
    QObject::disconnect(tab_change_connection_);
    tab_change_connection_ = {};
    stop_tab_animation();
    animated_tabs_ = tabs;
    if (reconnect) {
        set_tab_animations_enabled(true);
    }
}

void SettingsDialog::set_tab_animations_enabled(bool enabled) {
    QObject::disconnect(tab_change_connection_);
    tab_change_connection_ = {};
    stop_tab_animation();
    tab_animations_enabled_ = enabled;
    if (animated_tabs_) {
        if (auto* tab_bar = dynamic_cast<AnimatedTabBar*>(animated_tabs_->tabBar())) {
            tab_bar->setAnimationsEnabled(enabled);
        }
    }
    if (!enabled || !animated_tabs_) {
        return;
    }
    tab_change_connection_ = QObject::connect(
        animated_tabs_, &QTabWidget::currentChanged, this,
        [this](int index) { animate_tab(index); });
}

void SettingsDialog::stop_tab_animation() {
    if (tab_animation_) {
        QObject::disconnect(tab_animation_, nullptr, this, nullptr);
        tab_animation_->stop();
        delete tab_animation_;
        tab_animation_ = nullptr;
    }
    if (animated_page_ && animated_page_->graphicsEffect() == tab_opacity_effect_) {
        animated_page_->setGraphicsEffect(nullptr);
    }
    tab_opacity_effect_ = nullptr;
    animated_page_ = nullptr;
}

void SettingsDialog::animate_tab(int index) {
    stop_tab_animation();
    if (!tab_animations_enabled_ || !animated_tabs_) {
        return;
    }
    if (auto* tab_bar = dynamic_cast<AnimatedTabBar*>(animated_tabs_->tabBar())) {
        tab_bar->slideToIndex(index);
    }
    QWidget* page = animated_tabs_->widget(index);
    if (!page) {
        return;
    }

    auto* effect = new QGraphicsOpacityEffect(page);
    effect->setOpacity(0.18);
    page->setGraphicsEffect(effect);
    auto* animation = new QPropertyAnimation(effect, "opacity", this);
    animation->setDuration(180);
    animation->setStartValue(0.18);
    animation->setEndValue(1.0);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    animated_page_ = page;
    tab_opacity_effect_ = effect;
    tab_animation_ = animation;

    QObject::connect(animation, &QPropertyAnimation::finished, this,
                     [this, animation, page, effect]() {
        if (tab_animation_ != animation) {
            return;
        }
        tab_animation_ = nullptr;
        if (page->graphicsEffect() == effect) {
            page->setGraphicsEffect(nullptr);
        }
        tab_opacity_effect_ = nullptr;
        animated_page_ = nullptr;
        animation->deleteLater();
    });
    animation->start();
}

MainWindow::MainWindow(fs::path initial_contest) {
    load_app_settings();
#ifndef Q_OS_WIN
    setAttribute(Qt::WA_TranslucentBackground, true);
#endif
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAutoFillBackground(false);
    setWindowTitle(text("window_title"));
    setWindowIcon(QIcon(":/materials/logo.png"));
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
    resize(1320, 820);
    setMinimumSize(980, 640);

    auto* central = new QWidget(this);
    central->setObjectName("AppRoot");
    auto* stack = new QStackedLayout(central);
    stack->setContentsMargins(0, 0, 0, 0);
    stack->setStackingMode(QStackedLayout::StackAll);
    background_layer_ = new neothemis::gui::ThemeBackground(central);
    auto* content_surface = new QWidget(central);
    content_surface->setObjectName("AppContent");
    stack->addWidget(background_layer_);
    stack->addWidget(content_surface);
    stack->setCurrentWidget(content_surface);

    auto* root = new QVBoxLayout(content_surface);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    title_bar_ = new QWidget(content_surface);
    title_bar_->setObjectName("WindowTitleBar");
    title_bar_->installEventFilter(this);
    auto* title_layout = new QHBoxLayout(title_bar_);
    title_layout->setContentsMargins(16, 0, 6, 0);
    title_layout->setSpacing(10);
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
    contest_title_ = new QLabel(text("no_contest_open"), title_bar_);
    contest_title_->setObjectName("ContestTitle");
    contest_title_->installEventFilter(this);
    title_layout->addWidget(contest_title_);
    auto* minimize = new QToolButton(title_bar_);
    minimize->setObjectName("WindowButton");
    minimize->setText(QString::fromUtf8("\u2014"));
    auto* maximize = new QToolButton(title_bar_);
    maximize->setObjectName("WindowButton");
    maximize->setText(QString::fromUtf8("\u25a1"));
    auto* close = new QToolButton(title_bar_);
    close->setObjectName("WindowCloseButton");
    close->setText(QString::fromUtf8("\u00d7"));
    close->setIcon(QIcon());
    close->setToolButtonStyle(Qt::ToolButtonTextOnly);
    close->setFocusPolicy(Qt::NoFocus);
    close->setToolTip(text("close"));
    close->setAccessibleName(text("close"));
    title_layout->addWidget(minimize);
    title_layout->addWidget(maximize);
    title_layout->addWidget(close);
    root->addWidget(title_bar_);

    auto* menu_row = new QWidget(content_surface);
    menu_row->setObjectName("MenuRow");
    auto* menu_layout = new QHBoxLayout(menu_row);
    menu_layout->setContentsMargins(14, 0, 14, 0);
    menu_layout->setSpacing(0);
    menu_bar_ = new QMenuBar(menu_row);
    menu_bar_->setNativeMenuBar(false);
    menu_layout->addWidget(menu_bar_, 0, Qt::AlignLeft);
    menu_layout->addStretch(1);
    root->addWidget(menu_row);

    build_toolbar();

    auto* content = new QHBoxLayout;
    content->setContentsMargins(20, 18, 20, 20);
    content->setSpacing(16);

    auto* scoreboard_workspace = new QWidget(content_surface);
    scoreboard_workspace->setObjectName("WorkspacePanel");
    auto* scoreboard_layout = new QVBoxLayout(scoreboard_workspace);
    scoreboard_layout->setContentsMargins(0, 0, 0, 0);
    scoreboard_layout->setSpacing(0);

    table_ = new QTableWidget(scoreboard_workspace);
    table_->setObjectName("ScoreTable");
    table_->setAlternatingRowColors(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table_->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->horizontalHeader()->setMinimumSectionSize(96);
    table_->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(46);
    table_->setShowGrid(false);
    table_->setCornerButtonEnabled(false);
    scoreboard_layout->addWidget(table_, 1);
    content->addWidget(scoreboard_workspace, 1);

    auto* side = new QWidget(content_surface);
    side->setObjectName("SidePanel");
    auto* side_layout = new QVBoxLayout(side);
    side_layout->setContentsMargins(18, 18, 18, 18);
    side_layout->setSpacing(12);
    side->setFixedWidth(348);

    auto* workspace_title = new QLabel(text("judge_workspace"), side);
    workspace_title->setObjectName("SidePanelTitle");
    side_layout->addWidget(workspace_title);

    judge_group_ = new QGroupBox(side);
    judge_group_->setObjectName("ActionCard");
    auto* action_layout = new QVBoxLayout(judge_group_);
    action_layout->setContentsMargins(12, 12, 12, 12);
    action_layout->setSpacing(8);
    judge_selected_button_ = new QPushButton(text("judge_selected"), judge_group_);
    judge_all_button_ = new QPushButton(text("judge_all"), judge_group_);
    stop_button_ = new QPushButton(text("stop"), judge_group_);
    detail_view_button_ = new QPushButton(text("detail_view"), judge_group_);
    judge_selected_button_->setObjectName("PrimaryButton");
    judge_all_button_->setObjectName("SecondaryButton");
    stop_button_->setObjectName("StopButton");
    detail_view_button_->setObjectName("QuietButton");
    stop_button_->setEnabled(false);
    action_layout->addWidget(judge_selected_button_);
    action_layout->addWidget(judge_all_button_);
    action_layout->addWidget(stop_button_);
    action_layout->addWidget(detail_view_button_);
    side_layout->addWidget(judge_group_);

    progress_ = new QProgressBar(side);
    progress_->setRange(0, 100);
    progress_->setValue(0);
    progress_->setFixedHeight(32);
    progress_->setTextVisible(false);
    side_layout->addWidget(progress_);

    judge_elapsed_label_ = new QLabel(side);
    judge_elapsed_label_->setObjectName("JudgeElapsedLabel");
    judge_elapsed_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    side_layout->addWidget(judge_elapsed_label_);
    judge_elapsed_update_timer_ = new QTimer(this);
    judge_elapsed_update_timer_->setInterval(100);
    QObject::connect(judge_elapsed_update_timer_, &QTimer::timeout,
                     [this]() { update_judge_elapsed_label(); });
    update_judge_elapsed_label();

    log_ = new QPlainTextEdit(side);
    log_->setObjectName("LogPanel");
    log_->setReadOnly(true);
    side_layout->addWidget(log_, 1);

    content->addWidget(side);
    root->addLayout(content, 1);
    setCentralWidget(central);
    neothemis::gui::install_windows_resize_handles(this);
    side_panel_ = side;
    apply_selected_theme();

    QObject::connect(judge_selected_button_, &QPushButton::clicked,
                     [this]() { start_judge(true); });
    QObject::connect(judge_all_button_, &QPushButton::clicked, [this]() { start_judge(false); });
    QObject::connect(stop_button_, &QPushButton::clicked, [this]() { request_stop_judge(); });
    QObject::connect(detail_view_button_, &QPushButton::clicked,
                     [this]() { show_judge_detail_view(); });
    QObject::connect(table_, &QTableWidget::cellDoubleClicked,
                     [this](int row, int col) { show_result_details(row, col); });
    QObject::connect(table_->horizontalHeader(), &QHeaderView::sectionClicked,
                     [this](int section) { sort_by_column(section); });
    QObject::connect(table_->horizontalHeader(), &QWidget::customContextMenuRequested,
                     [this](const QPoint& pos) { show_header_menu(pos); });
    QObject::connect(minimize, &QToolButton::clicked, this, &QWidget::showMinimized);
    QObject::connect(maximize, &QToolButton::clicked,
                     [this]() { neothemis::gui::toggle_window_maximized(this); });
    QObject::connect(close, &QToolButton::clicked, this, &QWidget::close);

    if (!initial_contest.empty()) {
        // Give the first window frame time to paint before multicore extraction starts.
        QTimer::singleShot(
            100, this, [this, path = std::move(initial_contest)]() { open_contest_file(path); });
    }
}

MainWindow::~MainWindow() {
    neothemis::gui::release_windows_backdrop(this);
    stop_local_server(false);
    stop_active_judge();
    join_archive_thread();
    cleanup_temporary_contests();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == title_bar_ || watched == contest_title_ ||
        (watched->isWidgetType() &&
         static_cast<QWidget*>(watched)->objectName() == "WindowAppName") ||
        (watched->isWidgetType() && static_cast<QWidget*>(watched)->objectName() == "AppLogo")) {
        if (event->type() == QEvent::MouseButtonDblClick) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                neothemis::gui::toggle_window_maximized(this);
                return true;
            }
        }
        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                if (QWindow* handle = windowHandle(); handle && handle->startSystemMove()) {
                    dragging_title_bar_ = false;
                    return true;
                }
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

void MainWindow::closeEvent(QCloseEvent* event) {
    if (archive_running_.load()) {
        QMessageBox::information(this, text("archive_operation_running"),
                                 text("wait_for_archive_operation"));
        event->ignore();
        return;
    }
    if (!confirm_discard_unsaved_file(true)) {
        event->ignore();
        return;
    }
    stop_active_judge();
    join_archive_thread();
    stop_local_server(false);
    cleanup_temporary_contests();
    event->accept();
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    apply_selected_theme();
    schedule_backdrop_startup_passes();
}

bool MainWindow::nativeEvent(const QByteArray& event_type, void* message, qintptr* result) {
#ifdef Q_OS_WIN
    if (background_transparency_active() &&
        neothemis::gui::windows_backdrop_message_requires_refresh(message) &&
        !backdrop_refresh_queued_) {
        backdrop_refresh_queued_ = true;
        QTimer::singleShot(0, this, [this]() {
            backdrop_refresh_queued_ = false;
            if (background_transparency_active()) {
                apply_native_backdrop(true);
            }
        });
    }
#endif
    return QMainWindow::nativeEvent(event_type, message, result);
}

QString MainWindow::text(const char* key) const {
    return neothemis::gui::translated_text(key, language_ == "vi");
}

void MainWindow::load_app_settings() {
    QSettings settings("NeoThemis", "NeoThemis");
    language_ = settings.value("language", "en").toString().toStdString();
    if (language_ != "vi") {
        language_ = "en";
    }
    theme_ = settings.value("theme", "dark").toString().toStdString();
    if (theme_ == "glassy-dark") {
        theme_ = "cyber";
    }
    if (theme_ != "cyber") {
        theme_ = "dark";
    }
    const neothemis::gui::CyberThemeColors default_cyber_colors =
        neothemis::gui::default_cyber_theme_colors();
    cyber_background_color_ = color_setting_or_default(
        settings.value("cyber_background_color",
                       default_cyber_colors.background.name(QColor::HexRgb)),
        default_cyber_colors.background);
    cyber_primary_color_ = color_setting_or_default(
        settings.value("cyber_primary_color",
                       default_cyber_colors.primary.name(QColor::HexRgb)),
        default_cyber_colors.primary);
    cyber_secondary_color_ = color_setting_or_default(
        settings.value("cyber_secondary_color",
                       default_cyber_colors.secondary.name(QColor::HexRgb)),
        default_cyber_colors.secondary);
    cyber_text_color_ = color_setting_or_default(
        settings.value("cyber_text_color", default_cyber_colors.text.name(QColor::HexRgb)),
        default_cyber_colors.text);
    cyber_muted_text_color_ = color_setting_or_default(
        settings.value("cyber_muted_text_color",
                       default_cyber_colors.muted_text.name(QColor::HexRgb)),
        default_cyber_colors.muted_text);
    cyber_primary_text_color_ = color_setting_or_default(
        settings.value("cyber_primary_text_color",
                       default_cyber_colors.primary_text.name(QColor::HexRgb)),
        default_cyber_colors.primary_text);
    cyber_secondary_text_color_ = color_setting_or_default(
        settings.value("cyber_secondary_text_color",
                       default_cyber_colors.secondary_text.name(QColor::HexRgb)),
        default_cyber_colors.secondary_text);
    transparent_background_ = settings.value("transparent_background", false).toBool();
    background_transparency_ =
        std::clamp(settings.value("background_transparency", 20).toInt(), 0, 95);
    blur_background_ = settings.value("blur_background", false).toBool();
    animations_enabled_ = settings.value("animations_enabled", true).toBool();
    if (settings.contains("sandbox_enabled")) {
        sandbox_enabled_ = settings.value("sandbox_enabled", true).toBool();
    } else {
        // Migrate the former inverse setting without silently changing an existing
        // user's local execution policy.
        sandbox_enabled_ = !settings.value("allow_unsafe_judging", false).toBool();
    }
    temporary_dir_ =
        settings.value("temporary_dir", QString::fromStdString(default_temporary_dir().string()))
            .toString()
            .toStdString();
    if (temporary_dir_.empty()) {
        temporary_dir_ = default_temporary_dir();
    }
    server_port_ = std::clamp(settings.value("server_port", 8080).toInt(), 1024, 65535);
    server_allow_lan_ = settings.value("server_allow_lan", false).toBool();
    server_join_code_ = settings.value("server_join_code").toString().trimmed();
    if (server_join_code_.isEmpty()) {
        server_join_code_ = generated_server_secret(10);
    }
    server_admin_password_ = settings.value("server_admin_password").toString();
    if (server_admin_password_.isEmpty()) {
        server_admin_password_ = generated_server_secret(14);
    }
}

void MainWindow::save_app_settings() const {
    QSettings settings("NeoThemis", "NeoThemis");
    settings.setValue("language", QString::fromStdString(language_));
    settings.setValue("theme", QString::fromStdString(theme_));
    settings.setValue("cyber_background_color",
                      cyber_background_color_.name(QColor::HexRgb));
    settings.setValue("cyber_primary_color", cyber_primary_color_.name(QColor::HexRgb));
    settings.setValue("cyber_secondary_color",
                      cyber_secondary_color_.name(QColor::HexRgb));
    settings.setValue("cyber_text_color", cyber_text_color_.name(QColor::HexRgb));
    settings.setValue("cyber_muted_text_color",
                      cyber_muted_text_color_.name(QColor::HexRgb));
    settings.setValue("cyber_primary_text_color",
                      cyber_primary_text_color_.name(QColor::HexRgb));
    settings.setValue("cyber_secondary_text_color",
                      cyber_secondary_text_color_.name(QColor::HexRgb));
    settings.setValue("transparent_background", transparent_background_);
    settings.setValue("background_transparency", background_transparency_);
    settings.setValue("blur_background", blur_background_);
    settings.setValue("animations_enabled", animations_enabled_);
    settings.setValue("sandbox_enabled", sandbox_enabled_);
    settings.remove("allow_unsafe_judging");
    settings.remove("background_blur_radius");
    settings.setValue("temporary_dir", QString::fromStdString(temporary_dir_.string()));
    settings.setValue("server_port", server_port_);
    settings.setValue("server_allow_lan", server_allow_lan_);
    settings.setValue("server_join_code", server_join_code_);
    settings.setValue("server_admin_password", server_admin_password_);
}

void MainWindow::build_toolbar() {
    auto* bar = menu_bar_ ? menu_bar_ : menuBar();
    bar->clear();

    auto* contest_menu = bar->addMenu(text("contest"));
    auto* open_folder_action =
        contest_menu->addAction(text("open_folder"), [this]() { open_contest(); });
    open_folder_action->setShortcut(QKeySequence("Ctrl+O"));
    auto* open_file_action =
        contest_menu->addAction(text("open_contest_file"), [this]() { open_contest_file(); });
    open_file_action->setShortcut(QKeySequence("Ctrl+Shift+O"));
    contest_menu->addSeparator();
    auto* save_action = contest_menu->addAction(text("save_contest_file"),
                                                [this]() { save_contest_container(false); });
    save_action->setShortcut(QKeySequence("Ctrl+S"));
    auto* save_as_action = contest_menu->addAction(text("save_contest_file_as"),
                                                   [this]() { save_contest_container(true); });
    save_as_action->setShortcut(QKeySequence("Ctrl+Shift+S"));
    contest_menu->addSeparator();
    contest_menu->addAction(text("refresh"), [this]() { refresh_table(); });

    auto* contestant_menu = bar->addMenu(text("contestant_menu"));
    contestant_menu->addAction(text("add_contestants_from_folder"),
                               [this]() { add_contestants_from_folder(); });
    contestant_menu->addAction(text("sync_server_users"), [this]() {
        try {
            const ServerUserSyncResult result = sync_server_users_from_contest();
            if (server_users_table_) {
                refresh_server_users_table(server_users_table_);
            }
            const QString message = text("server_users_synced")
                                        .arg(result.created)
                                        .arg(result.existing)
                                        .arg(result.removed)
                                        .arg(result.skipped);
            log_->appendPlainText(message);
            QMessageBox::information(this, text("sync_server_users"), message);
        } catch (const std::exception& ex) {
            QMessageBox::critical(this, text("server_user_failed"), ex.what());
        }
    });

    auto* judge_menu = bar->addMenu(text("judge"));
    judge_selected_action_ =
        judge_menu->addAction(text("judge_selected"), [this]() { start_judge(true); });
    judge_all_action_ = judge_menu->addAction(text("judge_all"), [this]() { start_judge(false); });
    stop_action_ = judge_menu->addAction(text("stop"), [this]() { request_stop_judge(); });
    stop_action_->setEnabled(false);

    auto* server_menu = bar->addMenu(text("server"));
    server_start_action_ = server_menu->addAction(text("start_local_server"),
                                                  [this]() { open_start_server_dialog(); });
    server_stop_action_ =
        server_menu->addAction(text("stop_local_server"), [this]() { stop_local_server(true); });
    update_server_actions();

    auto* export_menu = bar->addMenu(text("export"));
    export_menu->addAction(text("export_scoreboard"), [this]() { export_scoreboard_xlsx(); });
    export_menu->addAction(text("export_data"), [this]() { export_data_xlsx(); });

    auto* converter_menu = bar->addMenu(text("converter"));
    converter_menu->addAction(text("convert_old_contest_file"),
                              [this]() { convert_old_contest_to_ncontest(false); });
    converter_menu->addAction(text("convert_old_contest_folder"),
                              [this]() { convert_old_contest_to_ncontest(true); });

    auto* settings_menu = bar->addMenu(text("settings"));
    settings_menu->addAction(text("application_settings"), [this]() { open_settings_dialog(0); });
    settings_menu->addAction(text("contest_config"), [this]() { open_settings_dialog(1); });
    settings_menu->addAction(text("problem_config"), [this]() { open_settings_dialog(2); });
    settings_menu->addAction(text("server_settings"), [this]() { open_settings_dialog(3); });
    settings_menu->addAction(text("server_users"), [this]() { open_settings_dialog(4); });

    auto* help_menu = bar->addMenu(text("help"));
    help_menu->addAction(text("about"), [this]() { show_about_dialog(); });

    for (QMenu* menu : {contest_menu, contestant_menu, judge_menu, server_menu, export_menu,
                        converter_menu, settings_menu, help_menu}) {
        menu->setAttribute(Qt::WA_TranslucentBackground);
        menu->setWindowFlag(Qt::NoDropShadowWindowHint, true);
        menu->setWindowFlag(Qt::FramelessWindowHint, true);
    }
}

void MainWindow::open_settings_dialog(int initial_tab) {
    auto* dialog = new SettingsDialog(this);
    dialog->setWindowTitle(text("settings_title"));
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setObjectName("SettingsDialog");
    dialog->resize(900, 720);
    dialog->setMinimumSize(760, 580);
    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(22, 20, 22, 18);
    layout->setSpacing(14);
    auto* header = new QLabel(text("settings_title"), dialog);
    header->setObjectName("DialogTitle");
    layout->addWidget(header);
    auto* tabs = new SettingsTabWidget(dialog);
    tabs->setObjectName("SettingsTabs");
    tabs->setDocumentMode(true);
    tabs->tabBar()->setDrawBase(false);
    auto add_icon_tab = [tabs](QWidget* page, const QIcon& icon, const QString& label) {
        const int index = tabs->addTab(page, QString());
        if (auto* tab_bar = dynamic_cast<AnimatedTabBar*>(tabs->tabBar())) {
            tab_bar->setCenteredIcon(index, icon);
        }
        tabs->setTabToolTip(index, label);
        page->setAccessibleName(label);
#if QT_CONFIG(accessibility)
        tabs->tabBar()->setAccessibleTabName(index, label);
#endif
    };
    add_icon_tab(build_visual_tab(tabs, dialog),
                 settings_tab_icon(SettingsTabIcon::Application), text("application"));
    add_icon_tab(build_contest_tab(tabs, dialog),
                 settings_tab_icon(SettingsTabIcon::Contest), text("contest"));
    add_icon_tab(build_problem_tab(tabs, dialog),
                 settings_tab_icon(SettingsTabIcon::Problems), text("problems"));
    add_icon_tab(build_server_settings_tab(tabs, dialog),
                 settings_tab_icon(SettingsTabIcon::Server), text("server_settings"));
    add_icon_tab(build_server_users_tab(tabs),
                 settings_tab_icon(SettingsTabIcon::Users), text("server_users"));
    tabs->setCurrentIndex(initial_tab);
    dialog->set_animated_tabs(tabs);
    dialog->set_tab_animations_enabled(animations_enabled_);
    layout->addWidget(tabs);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    buttons->button(QDialogButtonBox::Close)->setIcon(QIcon());
    buttons->button(QDialogButtonBox::Close)->setObjectName("QuietButton");
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void MainWindow::show_about_dialog() {
    auto* dialog = new QDialog(this);
    dialog->setWindowTitle(text("about_title"));
    dialog->resize(520, 360);
    auto* layout = new QVBoxLayout(dialog);

    auto* title = new QLabel("NeoThemis", dialog);
    title->setObjectName("WindowAppName");
    auto* details = new QPlainTextEdit(dialog);
    details->setReadOnly(true);
    details->setPlainText(text("about_details"));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);

    layout->addWidget(title);
    layout->addWidget(details, 1);
    layout->addWidget(buttons);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

} // namespace neothemis::gui
