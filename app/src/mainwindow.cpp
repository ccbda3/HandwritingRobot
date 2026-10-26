#include "mainwindow.h"

#include "machinepanel.h"
#include "notebookeditor.h"
#include "previewcanvas.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QTextCursor>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <utility>

namespace {

constexpr int kRefreshDelayMs = 120;

// 常见作业本（横条本）去掉页边距后的书写区
constexpr double kNotebookWidthMm = 150.0;
constexpr double kNotebookHeightMm = 213.0;
constexpr int kNotebookLines = 22;

// GRBL @ 115200 baud 的实测吞吐，用于估算串口传输时间
constexpr double kGrblLinesPerSecond = 380.0;

const char* kDefaultText = "春眠不觉晓\n处处闻啼鸟";

// 字形缓存目录：从当前工作目录向上找，最多四层。
// 这样无论从项目根目录、build 目录还是 IDE 里启动都能找到。
std::filesystem::path find_default_cache() {
    std::error_code error;
    std::filesystem::path current = std::filesystem::current_path(error);
    if (error) {
        return std::filesystem::path(".cache") / "glyphs";
    }
    for (int depth = 0; depth < 4; ++depth) {
        const std::filesystem::path candidate = current / ".cache" / "glyphs";
        if (std::filesystem::is_directory(candidate, error)) {
            return candidate;
        }
        if (!current.has_parent_path()) {
            break;
        }
        current = current.parent_path();
    }
    return std::filesystem::path(".cache") / "glyphs";
}

QDoubleSpinBox* make_double(double minimum, double maximum, double value, int decimals,
                            const QString& suffix = {}) {
    auto* box = new QDoubleSpinBox;
    box->setRange(minimum, maximum);
    box->setDecimals(decimals);
    box->setValue(value);
    box->setSingleStep(std::max(0.01, (maximum - minimum) / 100.0));
    if (!suffix.isEmpty()) {
        box->setSuffix(suffix);
    }
    return box;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("HandwritingRobot Studio"));
    resize(1240, 820);

    buildUi();
    buildMenus();
    buildStatusBar();

    refresh_timer_ = new QTimer(this);
    refresh_timer_->setSingleShot(true);
    refresh_timer_->setInterval(kRefreshDelayMs);
    connect(refresh_timer_, &QTimer::timeout, this, &MainWindow::refresh);

    updateModeEnabled();
    refresh();
}

// ---------------------------------------------------------------------------
// 界面搭建
// ---------------------------------------------------------------------------

void MainWindow::buildUi() {
    auto* panel = new QWidget;
    auto* panel_layout = new QVBoxLayout(panel);
    panel_layout->setContentsMargins(10, 10, 10, 10);
    panel_layout->setSpacing(10);

    // ---- 文本：主界面只放预览 + 打开独立编辑窗口 ----
    text_edit_ = new QPlainTextEdit(QString::fromUtf8(kDefaultText), this);
    text_edit_->hide();
    connect(text_edit_, &QPlainTextEdit::textChanged, this, &MainWindow::scheduleRefresh);
    connect(text_edit_, &QPlainTextEdit::textChanged, this, &MainWindow::updateTextPreview);

    auto* text_group = new QGroupBox(QStringLiteral("文本"));
    auto* text_layout = new QVBoxLayout(text_group);
    text_preview_ = new QLabel;
    text_preview_->setWordWrap(true);
    text_preview_->setMinimumHeight(64);
    text_preview_->setStyleSheet(QStringLiteral(
        "QLabel { background:#ffffff; border:1px solid #d0d0d0; border-radius:4px; "
        "padding:6px; color:#333; }"));
    text_layout->addWidget(text_preview_);

    text_stats_ = new QLabel;
    text_stats_->setStyleSheet(QStringLiteral("color:#8a8a96;"));
    text_layout->addWidget(text_stats_);

    auto* edit_text_button = new QPushButton(QStringLiteral("编辑文本…"));
    connect(edit_text_button, &QPushButton::clicked, this, &MainWindow::editText);
    text_layout->addWidget(edit_text_button);

    panel_layout->addWidget(text_group);
    updateTextPreview();

    // ---- 常用版面（主界面只留高频项） ----
    auto* common_group = new QGroupBox(QStringLiteral("常用版面"));
    auto* common_form = new QFormLayout(common_group);

    mode_combo_ = new QComboBox;
    mode_combo_->addItem(QStringLiteral("横条本（字压横线）"), 0);
    mode_combo_->addItem(QStringLiteral("自由版面"), 1);
    connect(mode_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
                updateModeEnabled();
                scheduleRefresh();
            });
    common_form->addRow(QStringLiteral("模式"), mode_combo_);

    char_size_ = make_double(0.0, 200.0, 0.0, 2, QStringLiteral(" mm"));
    char_size_->setSpecialValueText(QStringLiteral("自动（等于行距）"));
    common_form->addRow(QStringLiteral("字宽"), char_size_);

    chars_per_line_ = new QSpinBox;
    chars_per_line_->setRange(0, 200);
    chars_per_line_->setValue(0);
    chars_per_line_->setSpecialValueText(QStringLiteral("自动"));
    common_form->addRow(QStringLiteral("每行字数"), chars_per_line_);

    panel_layout->addWidget(common_group);

    auto* settings_button = new QPushButton(QStringLiteral("高级设置…"));
    connect(settings_button, &QPushButton::clicked, this, &MainWindow::showSettings);
    panel_layout->addWidget(settings_button);

    auto* common_hint = new QLabel(
        QStringLiteral("页面尺寸、行数、字形、机器参数、字形数据都在「高级设置」里。"));
    common_hint->setWordWrap(true);
    common_hint->setStyleSheet(QStringLiteral("color:#8a8a96;"));
    panel_layout->addWidget(common_hint);

    // ---- 选项弹窗（非模态：改完立刻重排） ----
    settings_dialog_ = new QDialog(this);
    settings_dialog_->setWindowTitle(QStringLiteral("选项"));
    settings_dialog_->resize(520, 560);
    auto* dialog_layout = new QVBoxLayout(settings_dialog_);
    auto* tabs = new QTabWidget(settings_dialog_);
    dialog_layout->addWidget(tabs);

    // 版面页
    auto* page_group = new QGroupBox(QStringLiteral("页面与行"));
    auto* page_form = new QFormLayout(page_group);
    page_width_ = make_double(20.0, 1000.0, kNotebookWidthMm, 1, QStringLiteral(" mm"));
    page_height_ = make_double(20.0, 1000.0, kNotebookHeightMm, 1, QStringLiteral(" mm"));
    page_form->addRow(QStringLiteral("书写区宽"), page_width_);
    page_form->addRow(QStringLiteral("书写区高"), page_height_);

    line_count_ = new QSpinBox;
    line_count_->setRange(1, 200);
    line_count_->setValue(kNotebookLines);
    page_form->addRow(QStringLiteral("横线行数"), line_count_);

    first_line_ = new QSpinBox;
    first_line_->setRange(1, 200);
    first_line_->setValue(1);
    page_form->addRow(QStringLiteral("从第几行起"), first_line_);
    tabs->addTab(page_group, QStringLiteral("版面"));

    // 字形页
    auto* style_group = new QGroupBox(QStringLiteral("字形"));
    auto* style_form = new QFormLayout(style_group);
    slant_ = make_double(-20.0, 20.0, 0.0, 1, QStringLiteral("°"));
    style_form->addRow(QStringLiteral("倾斜（右倾为正）"), slant_);
    jitter_ = make_double(0.0, 0.02, 0.0, 4);
    jitter_->setSingleStep(0.0005);
    style_form->addRow(QStringLiteral("抖动"), jitter_);
    pressure_amp_ = make_double(0.0, 0.5, 0.30, 2);
    pressure_amp_->setSingleStep(0.05);
    style_form->addRow(QStringLiteral("提按幅度"), pressure_amp_);
    tabs->addTab(style_group, QStringLiteral("字形"));

    // 机器页
    auto* machine_group = new QGroupBox(QStringLiteral("机器"));
    auto* machine_form = new QFormLayout(machine_group);
    pen_combo_ = new QComboBox;
    pen_combo_->addItem(QStringLiteral("Z 轴（步进电机）"), 0);
    pen_combo_->addItem(QStringLiteral("主轴 PWM（舵机 / 电磁铁）"), 1);
    machine_form->addRow(QStringLiteral("抬落笔"), pen_combo_);

    // 默认值取自本机（奎享雕刻）实测配置：书写进给 8000 mm/min；
    // 抬笔 Z=0、落笔 Z=6 —— 这台机器 Z 越大越往下压，与常见「Z 向上为正」相反。
    // 进给若出墨不匀可调低；Z 值务必按自己机器实测。
    feed_draw_ = make_double(60.0, 12000.0, 8000.0, 0, QStringLiteral(" mm/min"));
    machine_form->addRow(QStringLiteral("书写进给"), feed_draw_);
    pen_up_z_ = make_double(-50.0, 50.0, 0.0, 2, QStringLiteral(" mm"));
    machine_form->addRow(QStringLiteral("抬笔 Z"), pen_up_z_);
    pen_down_z_ = make_double(-50.0, 50.0, 6.0, 2, QStringLiteral(" mm"));
    machine_form->addRow(QStringLiteral("落笔 Z"), pen_down_z_);

    // 有些机器 Y+ 朝上（纸上向下走是 -Y），勾上后输出的 Y 取反。
    invert_y_ = new QCheckBox(QStringLiteral("Y 轴翻转（向下为负）"));
    machine_form->addRow(QString(), invert_y_);
    tabs->addTab(machine_group, QStringLiteral("机器"));

    // 字形数据页
    auto* source_group = new QGroupBox(QStringLiteral("字形数据"));
    auto* source_layout = new QVBoxLayout(source_group);
    cache_edit_ = new QLineEdit(QString::fromStdString(find_default_cache().string()));
    source_layout->addWidget(cache_edit_);

    auto* source_buttons = new QHBoxLayout;
    auto* browse = new QPushButton(QStringLiteral("选择目录"));
    connect(browse, &QPushButton::clicked, this, &MainWindow::browseGlyphCache);
    source_buttons->addWidget(browse);
    auto* open_folder = new QPushButton(QStringLiteral("打开目录"));
    connect(open_folder, &QPushButton::clicked, this, &MainWindow::openGlyphCacheFolder);
    source_buttons->addWidget(open_folder);
    source_buttons->addStretch();
    source_layout->addLayout(source_buttons);

    auto* source_hint =
        new QLabel(QStringLiteral("字形需先用命令行预取：\nhandwriting preload \"要写的文字\""));
    source_hint->setWordWrap(true);
    source_hint->setStyleSheet(QStringLiteral("color:#8a8a96;"));
    source_layout->addWidget(source_hint);
    tabs->addTab(source_group, QStringLiteral("字形数据"));

    // 显示页
    auto* display_group = new QGroupBox(QStringLiteral("预览显示"));
    auto* display_layout = new QVBoxLayout(display_group);
    show_cells_ = new QCheckBox(QStringLiteral("显示字格"));
    show_cells_->setChecked(true);
    show_rules_ = new QCheckBox(QStringLiteral("显示横线"));
    show_rules_->setChecked(true);
    display_layout->addWidget(show_cells_);
    display_layout->addWidget(show_rules_);
    display_layout->addStretch();
    tabs->addTab(display_group, QStringLiteral("显示"));

    auto* dialog_buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto* reset_button = dialog_buttons->addButton(
        QStringLiteral("恢复默认"), QDialogButtonBox::ResetRole);
    connect(reset_button, &QPushButton::clicked, this,
            &MainWindow::resetLayoutDefaults);
    auto* close_button = dialog_buttons->button(QDialogButtonBox::Close);
    if (close_button != nullptr) {
        close_button->setText(QStringLiteral("关闭"));
    }
    connect(dialog_buttons, &QDialogButtonBox::rejected, settings_dialog_, &QDialog::hide);
    dialog_layout->addWidget(dialog_buttons);
    panel_layout->addStretch();

    auto* scroll = new QScrollArea;
    scroll->setWidget(panel);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    // ---- 左侧停靠：参数面板（可滚动） ----
    auto* params_dock = new QDockWidget(QStringLiteral("常用"), this);
    params_dock->setObjectName(QStringLiteral("paramsDock"));
    params_dock->setWidget(scroll);
    params_dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::LeftDockWidgetArea, params_dock);
    params_dock_ = params_dock;

    // ---- 中央：预览画布（主角，独占整片区域，不再被底部面板挤压） ----
    canvas_ = new PreviewCanvas;
    connect(canvas_, &PreviewCanvas::zoomChanged, this, [this](double scale) {
        status_zoom_->setText(QStringLiteral("缩放 %1%").arg(scale * 100.0 / 3.0, 0, 'f', 0));
    });
    setCentralWidget(canvas_);

    // 行内字格编辑器：点预览里的字格时浮在该格上，回车/失焦回写原文
    cell_editor_ = new QLineEdit(canvas_);
    cell_editor_->setVisible(false);
    cell_editor_->setAlignment(Qt::AlignCenter);
    cell_editor_->setMaxLength(1);
    cell_editor_->setFrame(false);
    cell_editor_->setStyleSheet(QStringLiteral(
        "QLineEdit { background: rgba(255,255,255,0.92); border: 1px solid #c0392b; "
        "border-radius: 2px; padding: 0; }"));
    connect(cell_editor_, &QLineEdit::returnPressed, this, &MainWindow::commitCellEdit);
    connect(cell_editor_, &QLineEdit::editingFinished, this, &MainWindow::commitCellEdit);
    connect(canvas_, &PreviewCanvas::cellActivated, this, &MainWindow::beginCellEdit);
    connect(canvas_, &PreviewCanvas::zoomChanged, this, [this](double) {
        if (cell_editor_ != nullptr) {
            cell_editor_->hide();
        }
    });

    for (QDoubleSpinBox* box :
         {page_width_, page_height_, char_size_, slant_, jitter_, pressure_amp_, feed_draw_,
          pen_up_z_, pen_down_z_}) {
        connect(box, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                &MainWindow::scheduleRefresh);
    }
    for (QSpinBox* box : {line_count_, first_line_, chars_per_line_}) {
        connect(box, QOverload<int>::of(&QSpinBox::valueChanged), this,
                &MainWindow::scheduleRefresh);
    }
    connect(pen_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::scheduleRefresh);
    connect(invert_y_, &QCheckBox::toggled, this, &MainWindow::scheduleRefresh);
    connect(show_cells_, &QCheckBox::toggled, this,
            [this](bool on) { canvas_->setShowCells(on); });
    connect(show_rules_, &QCheckBox::toggled, this,
            [this](bool on) { canvas_->setShowRules(on); });
    connect(cache_edit_, &QLineEdit::editingFinished, this, &MainWindow::scheduleRefresh);

    // ---- 机器控制面板（右侧停靠，作为竖向侧栏） ----
    auto* dock = new QDockWidget(QStringLiteral("机器控制 · 串口直连"), this);
    dock->setObjectName(QStringLiteral("machineDock"));
    machine_panel_ = new MachinePanel(dock);
    machine_panel_->setProgramProvider([this] { return current_program_; });
    machine_panel_->setPenZProvider(
        [this] { return std::make_pair(pen_up_z_->value(), pen_down_z_->value()); });
    dock->setWidget(machine_panel_);
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    dock_ = dock;

    // 初始停靠宽度：参数窄一点，机器面板留出放网格的空间
    resizeDocks({params_dock_, dock_}, {330, 440}, Qt::Horizontal);
}

void MainWindow::buildMenus() {
    auto* file_menu = menuBar()->addMenu(QStringLiteral("文件(&F)"));

    auto* export_action = file_menu->addAction(QStringLiteral("导出 G 代码(&G)..."));
    export_action->setShortcut(QKeySequence::Save);
    connect(export_action, &QAction::triggered, this, &MainWindow::exportGcode);

    auto* image_action = file_menu->addAction(QStringLiteral("导出预览图(&P)..."));
    image_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    connect(image_action, &QAction::triggered, this, &MainWindow::exportPreviewImage);

    file_menu->addSeparator();
    auto* quit_action = file_menu->addAction(QStringLiteral("退出(&Q)"));
    quit_action->setShortcut(QKeySequence::Quit);
    connect(quit_action, &QAction::triggered, this, &QWidget::close);

    auto* view_menu = menuBar()->addMenu(QStringLiteral("视图(&V)"));

    auto* fit_action = view_menu->addAction(QStringLiteral("适应窗口(&F)"));
    fit_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
    connect(fit_action, &QAction::triggered, this, [this] { canvas_->zoomToFit(); });

    auto* reset_action = view_menu->addAction(QStringLiteral("恢复默认版面(&R)"));
    connect(reset_action, &QAction::triggered, this, &MainWindow::resetLayoutDefaults);

    view_menu->addSeparator();

    auto* cells_action = view_menu->addAction(QStringLiteral("显示字格"));
    cells_action->setCheckable(true);
    cells_action->setChecked(true);
    connect(cells_action, &QAction::toggled, show_cells_, &QCheckBox::setChecked);

    auto* rules_action = view_menu->addAction(QStringLiteral("显示横线"));
    rules_action->setCheckable(true);
    rules_action->setChecked(true);
    connect(rules_action, &QAction::toggled, show_rules_, &QCheckBox::setChecked);

    view_menu->addSeparator();
    auto* params_action = view_menu->addAction(QStringLiteral("常用面板(&P)"));
    params_action->setCheckable(true);
    params_action->setChecked(true);
    connect(params_action, &QAction::toggled, this, [this](bool on) {
        if (params_dock_ != nullptr) {
            params_dock_->setVisible(on);
        }
    });

    auto* dock_action = view_menu->addAction(QStringLiteral("机器控制面板(&M)"));
    dock_action->setCheckable(true);
    dock_action->setChecked(true);
    connect(dock_action, &QAction::toggled, this, [this](bool on) {
        if (dock_ != nullptr) {
            dock_->setVisible(on);
        }
    });

    auto* refresh_action = view_menu->addAction(QStringLiteral("重新生成(&R)"));
    refresh_action->setShortcut(QKeySequence(QStringLiteral("F5")));
    connect(refresh_action, &QAction::triggered, this, &MainWindow::refresh);

    auto* settings_menu = menuBar()->addMenu(QStringLiteral("设置(&S)"));
    auto* options_action = settings_menu->addAction(QStringLiteral("选项(&O)..."));
    options_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    connect(options_action, &QAction::triggered, this, &MainWindow::showSettings);

    auto* toolbar = addToolBar(QStringLiteral("主工具栏"));
    toolbar->setMovable(false);
    toolbar->addAction(export_action);
    toolbar->addSeparator();
    toolbar->addAction(fit_action);
    toolbar->addAction(refresh_action);
    toolbar->addSeparator();
    toolbar->addAction(options_action);
}

void MainWindow::buildStatusBar() {
    status_chars_ = new QLabel(QStringLiteral("—"));
    status_page_ = new QLabel(QStringLiteral("—"));
    status_machine_ = new QLabel(QStringLiteral("—"));
    status_zoom_ = new QLabel(QStringLiteral("缩放 100%"));

    statusBar()->addWidget(status_chars_);
    statusBar()->addWidget(status_page_);
    statusBar()->addWidget(status_machine_, 1);
    statusBar()->addPermanentWidget(status_zoom_);
}

void MainWindow::updateModeEnabled() {
    const bool notebook = mode_combo_->currentData().toInt() == 0;
    page_width_->setEnabled(notebook);
    page_height_->setEnabled(notebook);
    line_count_->setEnabled(notebook);
    first_line_->setEnabled(notebook);
}

void MainWindow::showSettings() {
    if (settings_dialog_ == nullptr) {
        return;
    }
    settings_dialog_->show();
    settings_dialog_->raise();
    settings_dialog_->activateWindow();
}

// ---------------------------------------------------------------------------
// 生成
// ---------------------------------------------------------------------------

handwriting::render::Settings MainWindow::renderSettings() const {
    handwriting::render::Settings settings;
    settings.slant_deg = slant_->value();
    settings.jitter = jitter_->value();
    settings.pressure_amplitude = pressure_amp_->value();
    return settings;
}

handwriting::layout::Settings MainWindow::layoutSettings() const {
    handwriting::layout::Settings settings;
    settings.mode = (mode_combo_->currentData().toInt() == 0)
                        ? handwriting::layout::Mode::Notebook
                        : handwriting::layout::Mode::Grid;
    settings.page_width_mm = page_width_->value();
    settings.page_height_mm = page_height_->value();
    settings.line_count = line_count_->value();
    settings.first_line = first_line_->value();
    settings.char_size_mm = char_size_->value();
    settings.chars_per_line = chars_per_line_->value();
    settings.grid_chars_per_line = (chars_per_line_->value() > 0) ? chars_per_line_->value() : 10;
    return settings;
}

handwriting::gcode::PlotSettings MainWindow::plotSettings() const {
    handwriting::gcode::PlotSettings settings;
    settings.use_z_axis = (pen_combo_->currentData().toInt() == 0);
    settings.feed_draw = feed_draw_->value();
    settings.pen_up_z = pen_up_z_->value();
    settings.pen_down_z = pen_down_z_->value();
    settings.invert_y = (invert_y_ != nullptr) && invert_y_->isChecked();
    return settings;
}

MainWindow::Rendered MainWindow::renderNow() const {
    Rendered out;

    const handwriting::glyph::GlyphCache cache(cache_edit_->text().toStdString());
    const handwriting::render::Settings render_settings = renderSettings();

    // chars_per_line 传 0：折行统一由 layout 负责，避免两处各折一次
    handwriting::render::Result rendered = handwriting::render::render_text(
        text_edit_->toPlainText().toStdString(), cache, render_settings, 0);

    out.missing = rendered.missing;
    out.source_offsets = rendered.source_offsets;

    if (rendered.rows.empty()) {
        out.ok = false;
        out.error = rendered.missing.empty()
                        ? QStringLiteral("没有可书写的文字")
                        : QStringLiteral("字形数据不可用。请先在命令行执行：\n"
                                         "handwriting preload \"要写的文字\"");
        return out;
    }

    std::vector<std::size_t> row_counts;
    row_counts.reserve(rendered.rows.size());
    for (const auto& row : rendered.rows) {
        row_counts.push_back(row.size());
    }

    out.page = handwriting::layout::compute(row_counts, layoutSettings());

    for (auto& row : rendered.rows) {
        for (auto& glyph : row) {
            out.glyphs.push_back(std::move(glyph));
        }
    }

    out.ok = !out.glyphs.empty();
    if (!out.ok) {
        out.error = QStringLiteral("版面计算失败");
    }
    return out;
}

void MainWindow::scheduleRefresh() {
    refresh_timer_->start();
}

void MainWindow::refresh() {
    const Rendered rendered = renderNow();
    last_rendered_ = rendered;

    if (!rendered.ok) {
        canvas_->clearContent();
        current_program_.clear();
        if (machine_panel_ != nullptr) {
            machine_panel_->notifyProgramChanged();
        }
        status_chars_->setText(QStringLiteral("—"));
        status_page_->setText(QStringLiteral("—"));
        status_machine_->setText(rendered.error);
        return;
    }

    canvas_->setContent(rendered.page, rendered.glyphs);
    updateStatus(rendered);
    if (machine_panel_ != nullptr) {
        machine_panel_->notifyProgramChanged();
    }
}

void MainWindow::beginCellEdit(int index) {
    if (index < 0 ||
        static_cast<std::size_t>(index) >= last_rendered_.source_offsets.size() ||
        static_cast<std::size_t>(index) >= last_rendered_.glyphs.size()) {
        return;
    }
    const QRect rect = canvas_->cellRectInWidget(index);
    if (!rect.isValid() || rect.width() < 6 || rect.height() < 6) {
        return;
    }
    edit_index_ = index;
    cell_editor_->setGeometry(rect.adjusted(1, 1, -1, -1));
    cell_editor_->setText(QString::fromStdString(
        last_rendered_.glyphs[static_cast<std::size_t>(index)].character()));
    cell_editor_->selectAll();
    cell_editor_->show();
    cell_editor_->raise();
    cell_editor_->setFocus();
}

void MainWindow::commitCellEdit() {
    if (!cell_editor_->isVisible()) {
        return;
    }
    const QString value = cell_editor_->text().trimmed();
    cell_editor_->hide();
    const int index = edit_index_;
    edit_index_ = -1;
    if (index < 0 || value.isEmpty() ||
        static_cast<std::size_t>(index) >= last_rendered_.source_offsets.size()) {
        return;
    }

    // source_offsets 是 UTF-8 字节偏移；转成 QString 位置再替换，避免中文/缺字错位
    const std::size_t offset =
        last_rendered_.source_offsets[static_cast<std::size_t>(index)];
    const QString current = text_edit_->toPlainText();
    const QString prefix =
        QString::fromStdString(current.toStdString().substr(0, offset));
    const int position = prefix.size();
    if (position < 0 || position >= current.size()) {
        return;
    }
    QString updated = current;
    updated.replace(position, 1, value.left(1));
    if (updated != current) {
        text_edit_->setPlainText(updated);  // textChanged 会触发 scheduleRefresh
    }
}

void MainWindow::updateTextPreview() {
    if (text_edit_ == nullptr || text_preview_ == nullptr) {
        return;
    }
    const QString text = text_edit_->toPlainText();
    const QStringList lines = text.split('\n');
    QString preview;
    for (int i = 0; i < qMin(2, lines.size()); ++i) {
        if (i > 0) {
            preview += '\n';
        }
        preview += lines[i];
    }
    if (lines.size() > 2) {
        preview += QStringLiteral("\n…");
    }
    text_preview_->setText(preview.isEmpty() ? QStringLiteral("（空）") : preview);
    if (text_stats_ != nullptr) {
        text_stats_->setText(
            QStringLiteral("%1 字符 · %2 行").arg(text.size()).arg(lines.size()));
    }
}

void MainWindow::buildTextDialog() {
    text_dialog_ = new QDialog(this);
    text_dialog_->setWindowTitle(QStringLiteral("编辑文本 · 横条本"));
    text_dialog_->resize(760, 640);

    auto* layout = new QVBoxLayout(text_dialog_);

    auto* row1 = new QHBoxLayout;
    auto* indent_button = new QPushButton(QStringLiteral("首行缩进"));
    indent_button->setToolTip(QStringLiteral("每个自然段首行缩进两个全角空格"));
    auto* first_para_button = new QPushButton(QStringLiteral("首段缩进"));
    first_para_button->setToolTip(QStringLiteral("只给第一段加两个全角空格"));
    auto* first_char_button = new QPushButton(QStringLiteral("首字缩进"));
    first_char_button->setToolTip(QStringLiteral("每个自然段首字缩进一个全角空格"));
    auto* center_first_button = new QPushButton(QStringLiteral("首行居中"));
    center_first_button->setToolTip(QStringLiteral("把第一行按当前每行字数居中"));
    row1->addWidget(indent_button);
    row1->addWidget(first_para_button);
    row1->addWidget(first_char_button);
    row1->addWidget(center_first_button);
    row1->addStretch();
    layout->addLayout(row1);

    auto* row2 = new QHBoxLayout;
    auto* left_button = new QPushButton(QStringLiteral("左对齐"));
    auto* center_button = new QPushButton(QStringLiteral("居中"));
    auto* right_button = new QPushButton(QStringLiteral("右对齐"));
    auto* space_button = new QPushButton(QStringLiteral("插入全角空格"));
    auto* blank_button = new QPushButton(QStringLiteral("删除空行"));
    row2->addWidget(left_button);
    row2->addWidget(center_button);
    row2->addWidget(right_button);
    row2->addWidget(space_button);
    row2->addWidget(blank_button);
    row2->addStretch();
    layout->addLayout(row2);

    text_dialog_editor_ = new NotebookEditor(text_dialog_);
    text_dialog_editor_->setPlaceholderText(
        QStringLiteral("在横线上输入文字；空格会保留为空白字格"));
    layout->addWidget(text_dialog_editor_, 1);

    text_dialog_stats_ = new QLabel(text_dialog_);
    text_dialog_stats_->setStyleSheet(QStringLiteral("color:#8a8a96;"));
    layout->addWidget(text_dialog_stats_);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, text_dialog_);
    if (auto* ok = buttons->button(QDialogButtonBox::Ok)) {
        ok->setText(QStringLiteral("确定"));
    }
    if (auto* cancel = buttons->button(QDialogButtonBox::Cancel)) {
        cancel->setText(QStringLiteral("取消"));
    }
    connect(buttons, &QDialogButtonBox::accepted, this, &MainWindow::applyTextDialog);
    connect(buttons, &QDialogButtonBox::rejected, text_dialog_, &QDialog::reject);
    layout->addWidget(buttons);

    // 统一对齐：先清掉行首缩进，再按当前每行字数补全角空格
    auto reformat = [this](Qt::Alignment alignment) {
        const int per_line = text_dialog_editor_->charsPerLine();
        QStringList lines = text_dialog_editor_->toPlainText().split('\n');
        for (QString& line : lines) {
            while (!line.isEmpty() &&
                   (line.at(0) == QChar(0x3000) || line.at(0) == QLatin1Char(' '))) {
                line.remove(0, 1);
            }
            if (line.isEmpty()) {
                continue;
            }
            int pad = 0;
            if (alignment == Qt::AlignHCenter) {
                pad = qMax(0, (per_line - line.length()) / 2);
            } else if (alignment == Qt::AlignRight) {
                pad = qMax(0, per_line - line.length());
            }
            if (pad > 0) {
                line.prepend(QString(pad, QChar(0x3000)));
            }
        }
        text_dialog_editor_->setPlainText(lines.join('\n'));
    };

    connect(indent_button, &QPushButton::clicked, this, [this] {
        const QStringList lines = text_dialog_editor_->toPlainText().split('\n');
        QStringList out;
        out.reserve(lines.size());
        for (QString line : lines) {
            if (!line.trimmed().isEmpty() && !line.startsWith(QChar(0x3000))) {
                line.prepend(QString(QChar(0x3000)) + QString(QChar(0x3000)));
            }
            out.append(line);
        }
        text_dialog_editor_->setPlainText(out.join('\n'));
    });
    connect(first_para_button, &QPushButton::clicked, this, [this] {
        QStringList lines = text_dialog_editor_->toPlainText().split('\n');
        for (QString& line : lines) {
            if (line.trimmed().isEmpty()) {
                continue;
            }
            if (!line.startsWith(QChar(0x3000))) {
                line.prepend(QString(QChar(0x3000)) + QString(QChar(0x3000)));
            }
            break;
        }
        text_dialog_editor_->setPlainText(lines.join('\n'));
    });
    connect(first_char_button, &QPushButton::clicked, this, [this] {
        const QStringList lines = text_dialog_editor_->toPlainText().split('\n');
        QStringList out;
        out.reserve(lines.size());
        for (QString line : lines) {
            if (!line.trimmed().isEmpty() && !line.startsWith(QChar(0x3000))) {
                line.prepend(QString(QChar(0x3000)));
            }
            out.append(line);
        }
        text_dialog_editor_->setPlainText(out.join('\n'));
    });
    connect(center_first_button, &QPushButton::clicked, this, [this] {
        const int per_line = text_dialog_editor_->charsPerLine();
        QStringList lines = text_dialog_editor_->toPlainText().split('\n');
        if (lines.isEmpty()) {
            return;
        }
        QString& first = lines[0];
        while (!first.isEmpty() &&
               (first.at(0) == QChar(0x3000) || first.at(0) == QLatin1Char(' '))) {
            first.remove(0, 1);
        }
        const int pad = qMax(0, (per_line - first.length()) / 2);
        if (pad > 0) {
            first.prepend(QString(pad, QChar(0x3000)));
        }
        text_dialog_editor_->setPlainText(lines.join('\n'));
    });
    connect(left_button, &QPushButton::clicked, this,
            [reformat] { reformat(Qt::AlignLeft); });
    connect(center_button, &QPushButton::clicked, this,
            [reformat] { reformat(Qt::AlignHCenter); });
    connect(right_button, &QPushButton::clicked, this,
            [reformat] { reformat(Qt::AlignRight); });
    connect(space_button, &QPushButton::clicked, this, [this] {
        text_dialog_editor_->insertPlainText(QString(QChar(0x3000)));
    });
    connect(blank_button, &QPushButton::clicked, this, [this] {
        const QStringList lines = text_dialog_editor_->toPlainText().split('\n');
        QStringList out;
        for (const QString& line : lines) {
            if (!line.trimmed().isEmpty()) {
                out.append(line);
            }
        }
        text_dialog_editor_->setPlainText(out.join('\n'));
    });
    connect(text_dialog_editor_, &QPlainTextEdit::textChanged, this,
            &MainWindow::updateTextDialogStats);
}

void MainWindow::applyTextDialog() {
    if (text_edit_ == nullptr || text_dialog_editor_ == nullptr) {
        return;
    }
    text_edit_->setPlainText(text_dialog_editor_->toPlainText());
    if (text_dialog_ != nullptr) {
        text_dialog_->accept();
    }
}

void MainWindow::updateTextDialogStats() {
    if (text_dialog_editor_ == nullptr || text_dialog_stats_ == nullptr) {
        return;
    }
    const QString text = text_dialog_editor_->toPlainText();
    const int lines = text.count('\n') + 1;
    text_dialog_stats_->setText(
        QStringLiteral("%1 字符 · %2 行（空格保留为空白字格）")
            .arg(text.size())
            .arg(lines));
}

void MainWindow::editText() {
    if (text_dialog_ == nullptr) {
        buildTextDialog();
    }
    if (text_dialog_editor_ == nullptr) {
        return;
    }
    text_dialog_editor_->setCharsPerLine(
        handwriting::layout::resolve_chars_per_line(layoutSettings()));
    text_dialog_editor_->setPlainText(text_edit_->toPlainText());
    updateTextDialogStats();
    text_dialog_editor_->moveCursor(QTextCursor::End);
    text_dialog_->exec();
}

void MainWindow::updateStatus(const Rendered& rendered) {
    std::size_t strokes = 0;
    std::size_t points = 0;
    for (const auto& glyph : rendered.glyphs) {
        strokes += glyph.stroke_count();
        points += glyph.point_count();
    }

    const auto program =
        handwriting::gcode::glyphs_to_gcode(rendered.glyphs, rendered.page, plotSettings());
    current_program_ = program.text;  // 缓存给机器面板用
    const double serial_seconds =
        static_cast<double>(program.stats.line_count) / kGrblLinesPerSecond;

    status_chars_->setText(QStringLiteral("字符 %1 · 笔画 %2 · 采样点 %3")
                               .arg(rendered.glyphs.size())
                               .arg(strokes)
                               .arg(points));

    status_page_->setText(QStringLiteral("幅面 %1 × %2 mm · 每行 %3 字")
                              .arg(rendered.page.width_mm, 0, 'f', 1)
                              .arg(rendered.page.height_mm, 0, 'f', 1)
                              .arg(rendered.page.chars_per_line));

    QString machine = QStringLiteral("G 代码 %1 行 · 串口约 %2 s · 运动约 %3 s")
                          .arg(program.stats.line_count)
                          .arg(serial_seconds, 0, 'f', 1)
                          .arg(program.stats.estimated_seconds, 0, 'f', 1);
    if (!rendered.missing.empty()) {
        QString list;
        for (const std::string& item : rendered.missing) {
            if (!list.isEmpty()) {
                list += QStringLiteral(" ");
            }
            list += QString::fromStdString(item);
        }
        machine += QStringLiteral("  ·  缺失字形：%1").arg(list);
    }
    status_machine_->setText(machine);
}

// ---------------------------------------------------------------------------
// 操作
// ---------------------------------------------------------------------------

void MainWindow::resetLayoutDefaults() {
    mode_combo_->setCurrentIndex(0);
    page_width_->setValue(kNotebookWidthMm);
    page_height_->setValue(kNotebookHeightMm);
    line_count_->setValue(kNotebookLines);
    first_line_->setValue(1);
    char_size_->setValue(0.0);
    chars_per_line_->setValue(0);
    slant_->setValue(0.0);
    jitter_->setValue(0.0);
    pressure_amp_->setValue(0.30);
    updateModeEnabled();
    refresh();
    canvas_->zoomToFit();
}

void MainWindow::exportGcode() {
    const Rendered rendered = renderNow();
    if (!rendered.ok) {
        QMessageBox::warning(this, QStringLiteral("无法导出"), rendered.error);
        return;
    }

    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出 G 代码"), QStringLiteral("handwriting.nc"),
        QStringLiteral("G 代码 (*.nc *.gcode *.tap *.txt)"));
    if (path.isEmpty()) {
        return;
    }

    const auto program =
        handwriting::gcode::glyphs_to_gcode(rendered.glyphs, rendered.page, plotSettings());

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this, QStringLiteral("写入失败"),
                              QStringLiteral("无法写入 %1").arg(path));
        return;
    }
    file.write(program.text.c_str());
    file.close();

    statusBar()->showMessage(
        QStringLiteral("已导出 %1（%2 行，串口约 %3 s）")
            .arg(QFileInfo(path).fileName())
            .arg(program.stats.line_count)
            .arg(static_cast<double>(program.stats.line_count) / kGrblLinesPerSecond, 0, 'f', 1),
        6000);
}

void MainWindow::exportPreviewImage() {
    if (!canvas_->hasContent()) {
        QMessageBox::warning(this, QStringLiteral("无法导出"),
                             QStringLiteral("当前没有可导出的预览"));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出预览图"), QStringLiteral("handwriting-preview.png"),
        QStringLiteral("PNG 图片 (*.png)"));
    if (path.isEmpty()) {
        return;
    }
    if (!canvas_->grab().save(path)) {
        QMessageBox::critical(this, QStringLiteral("写入失败"),
                              QStringLiteral("无法写入 %1").arg(path));
        return;
    }
    statusBar()->showMessage(QStringLiteral("已导出预览图 %1").arg(QFileInfo(path).fileName()),
                             6000);
}

void MainWindow::browseGlyphCache() {
    const QString path = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择字形缓存目录"), cache_edit_->text());
    if (path.isEmpty()) {
        return;
    }
    cache_edit_->setText(path);
    refresh();
}

void MainWindow::openGlyphCacheFolder() {
    const QString path = cache_edit_->text();
    if (path.isEmpty()) {
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void MainWindow::setDemoWorkspace(const handwriting::bounds::Workspace& ws) {
    if (machine_panel_ != nullptr) {
        machine_panel_->setWorkspace(ws);
    }
}

void MainWindow::setDemoHead(double x, double y) {
    if (machine_panel_ != nullptr) {
        machine_panel_->setDemoHead(x, y);
    }
}

void MainWindow::setDemoText(const QString& text) {
    if (text_edit_ != nullptr) {
        text_edit_->setPlainText(text);
    }
}
