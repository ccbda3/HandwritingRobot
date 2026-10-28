#include "mainwindow.h"

#include "capturetraindialog.h"
#include "machinepanel.h"
#include "notebookeditor.h"
#include "previewcanvas.h"

#include "handwriting/style_model.hpp"

#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
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
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QTextCursor>
#include <QTimer>
#include <QToolBar>

#include <algorithm>
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

std::filesystem::path find_default_weights() {
    std::error_code error;
    std::filesystem::path current = std::filesystem::current_path(error);
    if (error) {
        return std::filesystem::path("data") / "exports" / "style-model.json";
    }
    for (int depth = 0; depth < 4; ++depth) {
        const std::filesystem::path candidate =
            current / "data" / "exports" / "style-model.json";
        if (std::filesystem::is_regular_file(candidate, error)) {
            return candidate;
        }
        if (!current.has_parent_path()) {
            break;
        }
        current = current.parent_path();
    }
    return std::filesystem::path("data") / "exports" / "style-model.json";
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

    restoreSettings();

    loading_ = false;
    updateModeEnabled();
    updateTextPreview();
    refresh();
    updateWindowTitle();
}

// ---------------------------------------------------------------------------
// 界面搭建
// ---------------------------------------------------------------------------

void MainWindow::buildUi() {
    auto* panel = new QWidget;
    auto* panel_layout = new QVBoxLayout(panel);
    panel_layout->setContentsMargins(10, 10, 10, 10);
    panel_layout->setSpacing(10);

    // ---- 文本：主界面只留「编辑文本…」，文本修改统一在独立窗口 ----
    text_edit_ = new QPlainTextEdit(QString::fromUtf8(kDefaultText), this);
    text_edit_->hide();
    connect(text_edit_, &QPlainTextEdit::textChanged, this, &MainWindow::scheduleRefresh);
    connect(text_edit_, &QPlainTextEdit::textChanged, this, &MainWindow::updateTextPreview);

    auto* text_group = new QGroupBox(QStringLiteral("文本"));
    auto* text_layout = new QVBoxLayout(text_group);

    text_stats_ = new QLabel;
    text_stats_->setStyleSheet(QStringLiteral("color:#8a8a96;"));
    text_layout->addWidget(text_stats_);

    auto* edit_text_button = new QPushButton(QStringLiteral("编辑文本…"));
    connect(edit_text_button, &QPushButton::clicked, this, &MainWindow::editText);
    text_layout->addWidget(edit_text_button);

    auto* text_hint = new QLabel(QStringLiteral("文本修改统一在独立窗口里进行。"));
    text_hint->setWordWrap(true);
    text_hint->setStyleSheet(QStringLiteral("color:#8a8a96;"));
    text_layout->addWidget(text_hint);

    panel_layout->addWidget(text_group);

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

    style_enabled_ = new QCheckBox(QStringLiteral("整行风格（字大小/字距/基线）"));
    common_form->addRow(QString(), style_enabled_);

    style_model_enabled_ = new QCheckBox(QStringLiteral("使用风格模型（需 weights）"));
    common_form->addRow(QString(), style_model_enabled_);

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

    // 整行风格页
    auto* style_profile_group = new QGroupBox(QStringLiteral("整行风格波动"));
    auto* sp_form = new QFormLayout(style_profile_group);
    style_size_cv_ = make_double(0.0, 0.30, 0.08, 2);
    style_size_cv_->setSingleStep(0.01);
    sp_form->addRow(QStringLiteral("字宽变化（CV）"), style_size_cv_);
    style_gap_mean_ = make_double(0.0, 20.0, 3.0, 1, QStringLiteral(" mm"));
    sp_form->addRow(QStringLiteral("平均字距"), style_gap_mean_);
    style_gap_cv_ = make_double(0.0, 0.6, 0.30, 2);
    style_gap_cv_->setSingleStep(0.05);
    sp_form->addRow(QStringLiteral("字距变化（CV）"), style_gap_cv_);
    style_baseline_drift_ = make_double(0.0, 2.0, 0.5, 2, QStringLiteral(" mm"));
    style_baseline_drift_->setSingleStep(0.1);
    sp_form->addRow(QStringLiteral("基线抖动"), style_baseline_drift_);
    style_baseline_slope_ = make_double(-0.2, 0.2, 0.03, 2, QStringLiteral(" mm/字"));
    style_baseline_slope_->setSingleStep(0.01);
    sp_form->addRow(QStringLiteral("基线倾斜"), style_baseline_slope_);
    auto* style_hint = new QLabel(
        QStringLiteral("勾选左栏「整行风格」后生效。全为 0 即工整版面；"
                       "横条本主要体现字大小与基线波动，字距变化在自由版面更明显。"));
    style_hint->setWordWrap(true);
    style_hint->setStyleSheet(QStringLiteral("color:#8a8a96;"));
    sp_form->addRow(style_hint);
    tabs->addTab(style_profile_group, QStringLiteral("整行风格"));

    // 风格模型页
    auto* model_group = new QGroupBox(QStringLiteral("风格模型"));
    auto* model_form = new QFormLayout(model_group);
    auto* path_row = new QHBoxLayout;
    style_model_path_ = new QLineEdit(
        QString::fromStdString(find_default_weights().string()));
    path_row->addWidget(style_model_path_);
    auto* model_browse = new QPushButton(QStringLiteral("选择…"));
    connect(model_browse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择风格模型"), style_model_path_->text(),
            QStringLiteral("权重 JSON (*.json)"));
        if (!path.isEmpty()) {
            style_model_path_->setText(path);
            scheduleRefresh();
        }
    });
    path_row->addWidget(model_browse);
    model_form->addRow(QStringLiteral("权重 JSON"), path_row);
    style_model_strength_ = make_double(0.0, 1.5, 1.0, 2);
    style_model_strength_->setSingleStep(0.05);
    model_form->addRow(QStringLiteral("风格强度"), style_model_strength_);
    auto* model_hint = new QLabel(
        QStringLiteral("先运行 handwriting export-weights 导出 JSON。"
                       "勾选左栏「使用风格模型」后生效；加载失败会回退标准骨架。"));
    model_hint->setWordWrap(true);
    model_hint->setStyleSheet(QStringLiteral("color:#8a8a96;"));
    model_form->addRow(model_hint);
    tabs->addTab(model_group, QStringLiteral("风格模型"));

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
          pen_up_z_, pen_down_z_, style_size_cv_, style_gap_mean_, style_gap_cv_,
          style_baseline_drift_, style_baseline_slope_, style_model_strength_}) {
        connect(box, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                &MainWindow::scheduleRefresh);
    }
    connect(style_enabled_, &QCheckBox::toggled, this, &MainWindow::scheduleRefresh);
    connect(style_model_enabled_, &QCheckBox::toggled, this,
            &MainWindow::scheduleRefresh);
    connect(style_model_path_, &QLineEdit::editingFinished, this,
            &MainWindow::scheduleRefresh);

    // 项目脏标记：文本/版面/风格/字形参数变化都算修改
    connect(text_edit_, &QPlainTextEdit::textChanged, this, &MainWindow::markDirty);
    connect(mode_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::markDirty);
    connect(line_count_, QOverload<int>::of(&QSpinBox::valueChanged), this,
            &MainWindow::markDirty);
    connect(first_line_, QOverload<int>::of(&QSpinBox::valueChanged), this,
            &MainWindow::markDirty);
    connect(chars_per_line_, QOverload<int>::of(&QSpinBox::valueChanged), this,
            &MainWindow::markDirty);
    for (QDoubleSpinBox* box :
         {page_width_, page_height_, char_size_, slant_, jitter_, pressure_amp_,
          style_size_cv_, style_gap_mean_, style_gap_cv_, style_baseline_drift_,
          style_baseline_slope_, style_model_strength_}) {
        connect(box, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                &MainWindow::markDirty);
    }
    connect(style_enabled_, &QCheckBox::toggled, this, &MainWindow::markDirty);
    connect(style_model_enabled_, &QCheckBox::toggled, this, &MainWindow::markDirty);
    connect(style_model_path_, &QLineEdit::editingFinished, this,
            &MainWindow::markDirty);
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

    auto* new_action = file_menu->addAction(QStringLiteral("新建(&N)"));
    new_action->setShortcut(QKeySequence::New);
    connect(new_action, &QAction::triggered, this, &MainWindow::newProject);

    auto* open_action = file_menu->addAction(QStringLiteral("打开(&O)..."));
    open_action->setShortcut(QKeySequence::Open);
    connect(open_action, &QAction::triggered, this, &MainWindow::openProject);

    recent_menu_ = file_menu->addMenu(QStringLiteral("最近打开(&R)"));

    save_action_ = file_menu->addAction(QStringLiteral("保存(&S)"));
    save_action_->setShortcut(QKeySequence::Save);
    connect(save_action_, &QAction::triggered, this, &MainWindow::saveProject);

    save_as_action_ = file_menu->addAction(QStringLiteral("另存为(&A)..."));
    save_as_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    connect(save_as_action_, &QAction::triggered, this, &MainWindow::saveProjectAs);

    file_menu->addSeparator();
    auto* export_action = file_menu->addAction(QStringLiteral("导出 G 代码(&G)..."));
    export_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+E")));
    connect(export_action, &QAction::triggered, this, &MainWindow::exportGcode);

    auto* export_all_action =
        file_menu->addAction(QStringLiteral("导出全部页 G 代码(&A)..."));
    export_all_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+E")));
    connect(export_all_action, &QAction::triggered, this, &MainWindow::exportAllPages);

    auto* image_action = file_menu->addAction(QStringLiteral("导出预览图(&P)..."));
    image_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+E")));
    connect(image_action, &QAction::triggered, this, &MainWindow::exportPreviewImage);

    file_menu->addSeparator();
    auto* quit_action = file_menu->addAction(QStringLiteral("退出(&Q)"));
    quit_action->setShortcut(QKeySequence::Quit);
    connect(quit_action, &QAction::triggered, this, &QWidget::close);

    updateRecentMenu();

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

    auto* tools_menu = menuBar()->addMenu(QStringLiteral("工具(&T)"));
    auto* capture_action = tools_menu->addAction(QStringLiteral("采集与训练…"));
    connect(capture_action, &QAction::triggered, this, &MainWindow::showCaptureTrain);

    auto* toolbar = addToolBar(QStringLiteral("主工具栏"));
    toolbar->setObjectName(QStringLiteral("mainToolBar"));
    toolbar->setMovable(false);
    toolbar->addAction(export_action);
    toolbar->addSeparator();
    toolbar->addAction(fit_action);
    toolbar->addAction(refresh_action);
    toolbar->addSeparator();
    toolbar->addAction(capture_action);
    toolbar->addAction(options_action);

    toolbar->addSeparator();
    prev_page_action_ = toolbar->addAction(QStringLiteral("上一页"));
    next_page_action_ = toolbar->addAction(QStringLiteral("下一页"));
    prev_page_action_->setShortcut(QKeySequence(Qt::Key_PageUp));
    next_page_action_->setShortcut(QKeySequence(Qt::Key_PageDown));
    connect(prev_page_action_, &QAction::triggered, this, &MainWindow::prevPage);
    connect(next_page_action_, &QAction::triggered, this, &MainWindow::nextPage);
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

void MainWindow::showCaptureTrain() {
    if (capture_dialog_ == nullptr) {
        capture_dialog_ = new CaptureTrainDialog(this);
        capture_dialog_->setCacheDir(cache_edit_->text());
        connect(capture_dialog_, &CaptureTrainDialog::modelReady, this,
                &MainWindow::applyTrainedModel);
    }
    capture_dialog_->show();
    capture_dialog_->raise();
    capture_dialog_->activateWindow();
}

void MainWindow::applyTrainedModel(const QString& weights) {
    if (style_model_path_ == nullptr) {
        return;
    }
    style_model_path_->setText(weights);
    if (style_model_enabled_ != nullptr) {
        style_model_enabled_->setChecked(true);
    }
    scheduleRefresh();
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

handwriting::layout::StyleProfile MainWindow::styleProfile() const {
    handwriting::layout::StyleProfile profile;
    profile.size_cv = style_size_cv_->value();
    profile.gap_mean_mm = style_gap_mean_->value();
    profile.gap_cv = style_gap_cv_->value();
    profile.baseline_drift_mm = style_baseline_drift_->value();
    profile.baseline_slope_mm = style_baseline_slope_->value();
    return profile;
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

    // chars_per_line 传 0：折行统一由 layout 负责，避免两处各折一次。
    // 分页后只渲染当前页；点字编辑用 PageText.char_offsets 回到原文。
    const std::string page_text =
        pages_ready_ ? current_page_text_ : text_edit_->toPlainText().toStdString();
    handwriting::render::Result rendered = handwriting::render::render_text(
        page_text, cache, render_settings, 0);

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

    out.page = style_enabled_->isChecked()
                   ? handwriting::layout::compute_styled(
                         row_counts, layoutSettings(), styleProfile())
                   : handwriting::layout::compute(row_counts, layoutSettings());

    for (auto& row : rendered.rows) {
        for (auto& glyph : row) {
            out.glyphs.push_back(std::move(glyph));
        }
    }

    out.ok = !out.glyphs.empty();

    // 训练好的 v0 模型：对每个字作用残差 + 笔压（与 dev server 同一条 C++ 路径）
    if (style_model_enabled_ != nullptr && style_model_enabled_->isChecked()) {
        const QString weights = style_model_path_->text();
        if (weights.isEmpty()) {
            out.style_warning = QStringLiteral("未设置权重路径");
        } else {
            try {
                const auto model = handwriting::style::Model::load(
                    std::filesystem::path(weights.toStdWString()));
                const double strength = style_model_strength_->value();
                for (auto& glyph : out.glyphs) {
                    if (glyph.stroke_count() > 0) {
                        glyph = model.stylize(glyph, strength);
                    }
                }
            } catch (const std::exception& error) {
                out.style_warning = QString::fromUtf8(error.what());
            }
        }
    }

    if (!out.ok) {
        out.error = QStringLiteral("版面计算失败");
    }
    return out;
}

void MainWindow::scheduleRefresh() {
    refresh_timer_->start();
}

void MainWindow::ensurePages() {
    pages_ = handwriting::layout::paginate(
        text_edit_->toPlainText().toStdString(), layoutSettings());
    if (pages_.empty()) {
        pages_.push_back(handwriting::layout::PageText{});
    }
    current_page_ = std::clamp(current_page_, 0, static_cast<int>(pages_.size()) - 1);
    current_page_text_ = pages_[current_page_].text;
    current_page_char_offsets_ = pages_[current_page_].char_offsets;
    pages_ready_ = true;
    updatePageNav();
}

void MainWindow::updatePageNav() {
    const int count = static_cast<int>(pages_.size());
    if (prev_page_action_ != nullptr) {
        prev_page_action_->setEnabled(current_page_ > 0);
    }
    if (next_page_action_ != nullptr) {
        next_page_action_->setEnabled(current_page_ + 1 < count);
    }
}

void MainWindow::prevPage() {
    if (current_page_ > 0) {
        --current_page_;
        refresh();
    }
}

void MainWindow::nextPage() {
    if (current_page_ + 1 < static_cast<int>(pages_.size())) {
        ++current_page_;
        refresh();
    }
}

void MainWindow::refresh() {
    ensurePages();
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
    if (index < 0 || value.isEmpty()) {
        return;
    }

    // 缺字会被 render_text 跳过，所以不能直接用字形序号索引 char_offsets；
    // 先用 source_offsets（页内字节偏移）映射回原全文的字节偏移。
    const auto source = sourceOffsetForGlyph(index);
    if (!source.has_value()) {
        return;
    }
    const std::size_t offset = *source;
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

std::optional<std::size_t> MainWindow::sourceOffsetForGlyph(int index) const {
    if (index < 0 ||
        static_cast<std::size_t>(index) >= last_rendered_.source_offsets.size()) {
        return std::nullopt;
    }
    const std::size_t target =
        last_rendered_.source_offsets[static_cast<std::size_t>(index)];

    std::size_t page_offset = 0;
    std::size_t char_index = 0;
    while (page_offset < current_page_text_.size()) {
        const unsigned char lead =
            static_cast<unsigned char>(current_page_text_[page_offset]);
        std::size_t length = 1;
        if ((lead & 0x80U) == 0) {
            length = 1;
        } else if ((lead & 0xE0U) == 0xC0U) {
            length = 2;
        } else if ((lead & 0xF0U) == 0xE0U) {
            length = 3;
        } else if ((lead & 0xF8U) == 0xF0U) {
            length = 4;
        }
        if (page_offset + length > current_page_text_.size()) {
            length = current_page_text_.size() - page_offset;
        }
        const bool newline =
            current_page_text_.compare(page_offset, length, "\n") == 0;
        if (!newline) {
            if (page_offset == target) {
                if (char_index < current_page_char_offsets_.size()) {
                    return current_page_char_offsets_[char_index];
                }
                return std::nullopt;
            }
            ++char_index;
        }
        page_offset += length;
    }
    return std::nullopt;
}

void MainWindow::updateTextPreview() {
    if (text_edit_ == nullptr) {
        return;
    }
    const QString text = text_edit_->toPlainText();
    const int per_line =
        handwriting::layout::resolve_chars_per_line(layoutSettings());
    int lines = 0;
    for (const QString& line : text.split('\n')) {
        const int length = static_cast<int>(line.size());
        lines += qMax(1, (length + per_line - 1) / per_line);
    }
    if (text_stats_ != nullptr) {
        const int chars =
            static_cast<int>(text.size()) - static_cast<int>(text.count('\n'));
        text_stats_->setText(
            QStringLiteral("%1 字符 · %2 行").arg(chars).arg(lines));
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

    const QString scope = pages_.size() > 1 ? QStringLiteral("本页 ") : QString();
    status_chars_->setText(QStringLiteral("%1字符 %2 · 笔画 %3 · 采样点 %4")
                               .arg(scope)
                               .arg(rendered.glyphs.size())
                               .arg(strokes)
                               .arg(points));

    QString page_info = QStringLiteral("幅面 %1 × %2 mm · 每行 %3 字")
                            .arg(rendered.page.width_mm, 0, 'f', 1)
                            .arg(rendered.page.height_mm, 0, 'f', 1)
                            .arg(rendered.page.chars_per_line);
    if (pages_.size() > 1) {
        page_info += QStringLiteral(" · 第 %1 / %2 页")
                         .arg(current_page_ + 1)
                         .arg(static_cast<int>(pages_.size()));
    }
    status_page_->setText(page_info);

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
    if (style_model_enabled_ != nullptr && style_model_enabled_->isChecked()) {
        if (!rendered.style_warning.isEmpty()) {
            machine += QStringLiteral("  ·  风格模型失败：%1")
                           .arg(rendered.style_warning.left(80));
        } else {
            machine += QStringLiteral("  ·  风格模型已启用");
        }
    }
    status_machine_->setText(machine);
}

// ---------------------------------------------------------------------------
// 操作
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 项目文件：文本 + 版面 + 字形 + 风格模型
// ---------------------------------------------------------------------------

void MainWindow::markDirty() {
    if (loading_) {
        return;
    }
    if (!dirty_) {
        dirty_ = true;
        updateWindowTitle();
    }
}

void MainWindow::newProject() {
    if (!maybeSave()) {
        return;
    }
    loading_ = true;
    text_edit_->setPlainText(QString::fromUtf8(kDefaultText));
    resetLayoutDefaults();
    project_path_.clear();
    current_page_ = 0;
    dirty_ = false;
    loading_ = false;
    updateWindowTitle();
    refresh();
}

void MainWindow::openProject() {
    if (!maybeSave()) {
        return;
    }
    const QString start =
        project_path_.isEmpty() ? QString() : QFileInfo(project_path_).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("打开项目"), start,
        QStringLiteral("HandwritingRobot 项目 (*.hrproj *.json)"));
    if (path.isEmpty()) {
        return;
    }
    QString error;
    if (!loadProjectFile(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("打开失败"), error);
    }
}

void MainWindow::saveProject() {
    if (project_path_.isEmpty()) {
        saveProjectAs();
        return;
    }
    if (!saveProjectFile(project_path_)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"),
                             QStringLiteral("无法写入：%1").arg(project_path_));
    }
}

void MainWindow::saveProjectAs() {
    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存项目"),
        project_path_.isEmpty() ? QStringLiteral("handwriting.hrproj") : project_path_,
        QStringLiteral("HandwritingRobot 项目 (*.hrproj)"));
    if (path.isEmpty()) {
        return;
    }
    if (!path.endsWith(QStringLiteral(".hrproj"), Qt::CaseInsensitive)) {
        path += QStringLiteral(".hrproj");
    }
    if (!saveProjectFile(path)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"),
                             QStringLiteral("无法写入：%1").arg(path));
    }
}

bool MainWindow::maybeSave() {
    if (!dirty_) {
        return true;
    }
    const auto reply = QMessageBox::warning(
        this, QStringLiteral("未保存的修改"),
        QStringLiteral("当前项目有未保存的修改，是否保存？"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (reply == QMessageBox::Save) {
        saveProject();
        return !dirty_;
    }
    return reply == QMessageBox::Discard;
}

QJsonObject MainWindow::projectJson() const {
    QJsonObject root;
    root[QStringLiteral("format")] = QStringLiteral("handwriting-project/1");
    root[QStringLiteral("text")] = text_edit_->toPlainText();
    root[QStringLiteral("currentPage")] = current_page_;

    QJsonObject layout;
    layout[QStringLiteral("mode")] = mode_combo_->currentIndex();
    layout[QStringLiteral("pageWidth")] = page_width_->value();
    layout[QStringLiteral("pageHeight")] = page_height_->value();
    layout[QStringLiteral("lineCount")] = line_count_->value();
    layout[QStringLiteral("firstLine")] = first_line_->value();
    layout[QStringLiteral("charSize")] = char_size_->value();
    layout[QStringLiteral("charsPerLine")] = chars_per_line_->value();
    layout[QStringLiteral("styleEnabled")] = style_enabled_->isChecked();
    layout[QStringLiteral("styleSizeCv")] = style_size_cv_->value();
    layout[QStringLiteral("styleGapMean")] = style_gap_mean_->value();
    layout[QStringLiteral("styleGapCv")] = style_gap_cv_->value();
    layout[QStringLiteral("styleBaselineDrift")] = style_baseline_drift_->value();
    layout[QStringLiteral("styleBaselineSlope")] = style_baseline_slope_->value();
    root[QStringLiteral("layout")] = layout;

    QJsonObject render;
    render[QStringLiteral("slant")] = slant_->value();
    render[QStringLiteral("jitter")] = jitter_->value();
    render[QStringLiteral("pressureAmp")] = pressure_amp_->value();
    root[QStringLiteral("render")] = render;

    QJsonObject model;
    model[QStringLiteral("enabled")] = style_model_enabled_->isChecked();
    model[QStringLiteral("path")] = style_model_path_->text();
    model[QStringLiteral("strength")] = style_model_strength_->value();
    root[QStringLiteral("styleModel")] = model;
    return root;
}

bool MainWindow::applyProjectJson(const QJsonObject& root, QString* error) {
    if (root[QStringLiteral("format")].toString() !=
        QStringLiteral("handwriting-project/1")) {
        *error = QStringLiteral("不是 HandwritingRobot 项目文件（format 不匹配）");
        return false;
    }
    if (!root[QStringLiteral("text")].isString()) {
        *error = QStringLiteral("缺少 text 字段");
        return false;
    }

    text_edit_->setPlainText(root[QStringLiteral("text")].toString());
    current_page_ = root[QStringLiteral("currentPage")].toInt(0);

    const QJsonObject layout = root[QStringLiteral("layout")].toObject();
    mode_combo_->setCurrentIndex(layout[QStringLiteral("mode")].toInt(0) == 0 ? 0 : 1);
    page_width_->setValue(layout[QStringLiteral("pageWidth")].toDouble(150.0));
    page_height_->setValue(layout[QStringLiteral("pageHeight")].toDouble(213.0));
    line_count_->setValue(layout[QStringLiteral("lineCount")].toInt(22));
    first_line_->setValue(layout[QStringLiteral("firstLine")].toInt(1));
    char_size_->setValue(layout[QStringLiteral("charSize")].toDouble(0.0));
    chars_per_line_->setValue(layout[QStringLiteral("charsPerLine")].toInt(0));
    style_enabled_->setChecked(layout[QStringLiteral("styleEnabled")].toBool(false));
    style_size_cv_->setValue(layout[QStringLiteral("styleSizeCv")].toDouble(0.08));
    style_gap_mean_->setValue(layout[QStringLiteral("styleGapMean")].toDouble(3.0));
    style_gap_cv_->setValue(layout[QStringLiteral("styleGapCv")].toDouble(0.30));
    style_baseline_drift_->setValue(
        layout[QStringLiteral("styleBaselineDrift")].toDouble(0.5));
    style_baseline_slope_->setValue(
        layout[QStringLiteral("styleBaselineSlope")].toDouble(0.03));

    const QJsonObject render = root[QStringLiteral("render")].toObject();
    slant_->setValue(render[QStringLiteral("slant")].toDouble(0.0));
    jitter_->setValue(render[QStringLiteral("jitter")].toDouble(0.0));
    pressure_amp_->setValue(render[QStringLiteral("pressureAmp")].toDouble(0.30));

    const QJsonObject model = root[QStringLiteral("styleModel")].toObject();
    style_model_enabled_->setChecked(model[QStringLiteral("enabled")].toBool(false));
    style_model_path_->setText(model[QStringLiteral("path")].toString());
    style_model_strength_->setValue(model[QStringLiteral("strength")].toDouble(1.0));

    updateModeEnabled();
    refresh();
    return true;
}

bool MainWindow::loadProjectFile(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("无法打开：%1").arg(path);
        return false;
    }
    QJsonParseError parse_error{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (!document.isObject()) {
        *error = QStringLiteral("JSON 解析失败：%1").arg(parse_error.errorString());
        return false;
    }

    loading_ = true;
    const bool ok = applyProjectJson(document.object(), error);
    loading_ = false;
    if (!ok) {
        return false;
    }
    project_path_ = QFileInfo(path).absoluteFilePath();
    dirty_ = false;
    addRecentFile(project_path_);
    updateWindowTitle();
    return true;
}

bool MainWindow::saveProjectFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(projectJson()).toJson(QJsonDocument::Indented));
    if (file.error() != QFileDevice::NoError) {
        return false;
    }
    file.close();

    project_path_ = QFileInfo(path).absoluteFilePath();
    dirty_ = false;
    addRecentFile(project_path_);
    updateWindowTitle();
    return true;
}

void MainWindow::addRecentFile(const QString& path) {
    const QString absolute = QFileInfo(path).absoluteFilePath();
    recent_files_.removeAll(absolute);
    recent_files_.prepend(absolute);
    while (recent_files_.size() > 8) {
        recent_files_.removeLast();
    }
    QSettings settings;
    settings.setValue(QStringLiteral("recentFiles"), recent_files_);
    updateRecentMenu();
}

void MainWindow::updateRecentMenu() {
    if (recent_menu_ == nullptr) {
        return;
    }
    recent_menu_->clear();
    if (recent_files_.isEmpty()) {
        auto* empty = recent_menu_->addAction(QStringLiteral("（无）"));
        empty->setEnabled(false);
        return;
    }
    for (const QString& path : recent_files_) {
        auto* action = recent_menu_->addAction(QFileInfo(path).fileName());
        action->setToolTip(path);
        connect(action, &QAction::triggered, this, [this, path] {
            if (!maybeSave()) {
                return;
            }
            QString error;
            if (!loadProjectFile(path, &error)) {
                QMessageBox::warning(this, QStringLiteral("打开失败"), error);
            }
        });
    }
    recent_menu_->addSeparator();
    auto* clear = recent_menu_->addAction(QStringLiteral("清除最近记录"));
    connect(clear, &QAction::triggered, this, [this] {
        recent_files_.clear();
        QSettings settings;
        settings.remove(QStringLiteral("recentFiles"));
        updateRecentMenu();
    });
}

void MainWindow::updateWindowTitle() {
    const QString name = project_path_.isEmpty()
                             ? QStringLiteral("未命名")
                             : QFileInfo(project_path_).fileName();
    setWindowTitle(QStringLiteral("HandwritingRobot — %1%2")
                       .arg(name, dirty_ ? QStringLiteral(" *") : QString()));
}

void MainWindow::restoreSettings() {
    QSettings settings;
    const QByteArray geometry = settings.value(QStringLiteral("window/geometry")).toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
    const QByteArray state = settings.value(QStringLiteral("window/state")).toByteArray();
    if (!state.isEmpty()) {
        restoreState(state);
    }
    if (settings.contains(QStringLiteral("view/showCells"))) {
        show_cells_->setChecked(settings.value(QStringLiteral("view/showCells")).toBool());
    }
    if (settings.contains(QStringLiteral("view/showRules"))) {
        show_rules_->setChecked(settings.value(QStringLiteral("view/showRules")).toBool());
    }
    if (settings.contains(QStringLiteral("layout/mode"))) {
        mode_combo_->setCurrentIndex(
            settings.value(QStringLiteral("layout/mode")).toInt());
        page_width_->setValue(
            settings.value(QStringLiteral("layout/pageWidth"), 150.0).toDouble());
        page_height_->setValue(
            settings.value(QStringLiteral("layout/pageHeight"), 213.0).toDouble());
        line_count_->setValue(
            settings.value(QStringLiteral("layout/lineCount"), 22).toInt());
        first_line_->setValue(
            settings.value(QStringLiteral("layout/firstLine"), 1).toInt());
        char_size_->setValue(
            settings.value(QStringLiteral("layout/charSize"), 0.0).toDouble());
        chars_per_line_->setValue(
            settings.value(QStringLiteral("layout/charsPerLine"), 0).toInt());
        style_enabled_->setChecked(
            settings.value(QStringLiteral("layout/styleEnabled"), false).toBool());
        style_size_cv_->setValue(
            settings.value(QStringLiteral("layout/styleSizeCv"), 0.08).toDouble());
        style_gap_mean_->setValue(
            settings.value(QStringLiteral("layout/styleGapMean"), 3.0).toDouble());
        style_gap_cv_->setValue(
            settings.value(QStringLiteral("layout/styleGapCv"), 0.30).toDouble());
        style_baseline_drift_->setValue(
            settings.value(QStringLiteral("layout/styleBaselineDrift"), 0.5).toDouble());
        style_baseline_slope_->setValue(
            settings.value(QStringLiteral("layout/styleBaselineSlope"), 0.03).toDouble());
    }
    if (settings.contains(QStringLiteral("render/slant"))) {
        slant_->setValue(
            settings.value(QStringLiteral("render/slant"), 0.0).toDouble());
        jitter_->setValue(
            settings.value(QStringLiteral("render/jitter"), 0.0).toDouble());
        pressure_amp_->setValue(
            settings.value(QStringLiteral("render/pressureAmp"), 0.30).toDouble());
    }
    if (settings.contains(QStringLiteral("model/path"))) {
        style_model_enabled_->setChecked(
            settings.value(QStringLiteral("model/enabled"), false).toBool());
        style_model_path_->setText(
            settings.value(QStringLiteral("model/path")).toString());
        style_model_strength_->setValue(
            settings.value(QStringLiteral("model/strength"), 1.0).toDouble());
    }
    if (settings.contains(QStringLiteral("machine/feed"))) {
        feed_draw_->setValue(
            settings.value(QStringLiteral("machine/feed"), 8000.0).toDouble());
        pen_up_z_->setValue(
            settings.value(QStringLiteral("machine/penUpZ"), 0.0).toDouble());
        pen_down_z_->setValue(
            settings.value(QStringLiteral("machine/penDownZ"), 6.0).toDouble());
        invert_y_->setChecked(
            settings.value(QStringLiteral("machine/invertY"), false).toBool());
    }
    recent_files_ = settings.value(QStringLiteral("recentFiles")).toStringList();
    updateRecentMenu();
}

void MainWindow::saveSettings() {
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/state"), saveState());
    settings.setValue(QStringLiteral("view/showCells"), show_cells_->isChecked());
    settings.setValue(QStringLiteral("view/showRules"), show_rules_->isChecked());
    settings.setValue(QStringLiteral("layout/mode"), mode_combo_->currentIndex());
    settings.setValue(QStringLiteral("layout/pageWidth"), page_width_->value());
    settings.setValue(QStringLiteral("layout/pageHeight"), page_height_->value());
    settings.setValue(QStringLiteral("layout/lineCount"), line_count_->value());
    settings.setValue(QStringLiteral("layout/firstLine"), first_line_->value());
    settings.setValue(QStringLiteral("layout/charSize"), char_size_->value());
    settings.setValue(QStringLiteral("layout/charsPerLine"), chars_per_line_->value());
    settings.setValue(QStringLiteral("layout/styleEnabled"), style_enabled_->isChecked());
    settings.setValue(QStringLiteral("layout/styleSizeCv"), style_size_cv_->value());
    settings.setValue(QStringLiteral("layout/styleGapMean"), style_gap_mean_->value());
    settings.setValue(QStringLiteral("layout/styleGapCv"), style_gap_cv_->value());
    settings.setValue(QStringLiteral("layout/styleBaselineDrift"),
                      style_baseline_drift_->value());
    settings.setValue(QStringLiteral("layout/styleBaselineSlope"),
                      style_baseline_slope_->value());
    settings.setValue(QStringLiteral("render/slant"), slant_->value());
    settings.setValue(QStringLiteral("render/jitter"), jitter_->value());
    settings.setValue(QStringLiteral("render/pressureAmp"), pressure_amp_->value());
    settings.setValue(QStringLiteral("model/enabled"), style_model_enabled_->isChecked());
    settings.setValue(QStringLiteral("model/path"), style_model_path_->text());
    settings.setValue(QStringLiteral("model/strength"), style_model_strength_->value());
    settings.setValue(QStringLiteral("machine/feed"), feed_draw_->value());
    settings.setValue(QStringLiteral("machine/penUpZ"), pen_up_z_->value());
    settings.setValue(QStringLiteral("machine/penDownZ"), pen_down_z_->value());
    settings.setValue(QStringLiteral("machine/invertY"), invert_y_->isChecked());
    settings.setValue(QStringLiteral("recentFiles"), recent_files_);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (!maybeSave()) {
        event->ignore();
        return;
    }
    saveSettings();
    event->accept();
}

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
    style_enabled_->setChecked(false);
    style_size_cv_->setValue(0.08);
    style_gap_mean_->setValue(3.0);
    style_gap_cv_->setValue(0.30);
    style_baseline_drift_->setValue(0.5);
    style_baseline_slope_->setValue(0.03);
    style_model_enabled_->setChecked(false);
    style_model_strength_->setValue(1.0);
    style_model_path_->setText(
        QString::fromStdString(find_default_weights().string()));
    updateModeEnabled();
    refresh();
    canvas_->zoomToFit();
}

void MainWindow::exportGcode() {
    ensurePages();
    const Rendered rendered = renderNow();
    if (!rendered.ok) {
        QMessageBox::warning(this, QStringLiteral("无法导出"), rendered.error);
        return;
    }

    const QString default_name =
        pages_.size() > 1
            ? QStringLiteral("handwriting-p%1.nc").arg(current_page_ + 1)
            : QStringLiteral("handwriting.nc");
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出当前页 G 代码"), default_name,
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

void MainWindow::exportAllPages() {
    ensurePages();
    if (pages_.empty()) {
        QMessageBox::warning(this, QStringLiteral("无法导出"),
                             QStringLiteral("当前没有可导出的页面"));
        return;
    }
    const QString base = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出全部页 G 代码"), QStringLiteral("handwriting.nc"),
        QStringLiteral("G 代码 (*.nc *.gcode *.tap *.txt)"));
    if (base.isEmpty()) {
        return;
    }

    const QFileInfo info(base);
    const QString dir = info.absolutePath();
    const QString stem = info.completeBaseName();
    const QString suffix =
        info.suffix().isEmpty() ? QStringLiteral("nc") : info.suffix();

    const int saved_page = current_page_;
    int written = 0;
    int skipped = 0;
    for (int i = 0; i < static_cast<int>(pages_.size()); ++i) {
        current_page_ = i;
        current_page_text_ = pages_[i].text;
        current_page_char_offsets_ = pages_[i].char_offsets;
        const Rendered rendered = renderNow();
        if (!rendered.ok) {
            ++skipped;
            continue;
        }
        const auto program = handwriting::gcode::glyphs_to_gcode(
            rendered.glyphs, rendered.page, plotSettings());
        const QString path =
            pages_.size() == 1
                ? base
                : dir + QStringLiteral("/") + stem +
                      QStringLiteral("-p%1.").arg(i + 1) + suffix;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            ++skipped;
            continue;
        }
        file.write(program.text.c_str());
        file.close();
        ++written;
    }

    current_page_ = saved_page;
    refresh();
    if (written == 0) {
        QMessageBox::warning(this, QStringLiteral("导出失败"),
                             QStringLiteral("没有页面成功导出，请检查缺字或写入权限。"));
        return;
    }
    statusBar()->showMessage(
        QStringLiteral("已导出 %1 页 G 代码%2")
            .arg(written)
            .arg(skipped > 0 ? QStringLiteral("（跳过 %1 页）").arg(skipped)
                             : QString()),
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

void MainWindow::setDemoStyle(bool enabled) {
    if (style_enabled_ == nullptr) {
        return;
    }
    if (enabled) {
        style_size_cv_->setValue(0.14);
        style_gap_cv_->setValue(0.35);
        style_baseline_drift_->setValue(1.0);
        style_baseline_slope_->setValue(0.06);
    }
    style_enabled_->setChecked(enabled);
}

void MainWindow::setDemoStyleModel(bool enabled) {
    if (style_model_enabled_ != nullptr) {
        style_model_enabled_->setChecked(enabled);
    }
}

void MainWindow::setDemoPage(int page) {
    current_page_ = std::max(0, page);
    refresh();
}

bool MainWindow::selfTestProject(const QString& path) {
    setDemoText(QStringLiteral("项目保存测试\n第二行"));
    setDemoStyle(true);
    if (!saveProjectFile(path)) {
        return false;
    }
    const QString expected = text_edit_->toPlainText();
    const bool expected_style = style_enabled_->isChecked();

    text_edit_->setPlainText(QStringLiteral("changed"));
    style_enabled_->setChecked(false);

    QString error;
    if (!loadProjectFile(path, &error)) {
        return false;
    }
    return text_edit_->toPlainText() == expected &&
           style_enabled_->isChecked() == expected_style;
}
