#include "mainwindow.h"

#include "machinepanel.h"
#include "previewcanvas.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
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
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

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

    // ---- 文本 ----
    text_edit_ = new QPlainTextEdit(QString::fromUtf8(kDefaultText));
    text_edit_->setPlaceholderText(QStringLiteral("在这里输入要书写的文字"));
    text_edit_->setMinimumHeight(110);
    QFont text_font = text_edit_->font();
    text_font.setPointSizeF(text_font.pointSizeF() + 3.0);
    text_edit_->setFont(text_font);
    connect(text_edit_, &QPlainTextEdit::textChanged, this, &MainWindow::scheduleRefresh);

    auto* text_group = new QGroupBox(QStringLiteral("文本"));
    auto* text_layout = new QVBoxLayout(text_group);
    text_layout->addWidget(text_edit_);
    panel_layout->addWidget(text_group);

    // ---- 版面 ----
    auto* layout_group = new QGroupBox(QStringLiteral("版面"));
    auto* form = new QFormLayout(layout_group);

    mode_combo_ = new QComboBox;
    mode_combo_->addItem(QStringLiteral("横条本（字压横线）"), 0);
    mode_combo_->addItem(QStringLiteral("自由版面"), 1);
    connect(mode_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
                updateModeEnabled();
                scheduleRefresh();
            });
    form->addRow(QStringLiteral("模式"), mode_combo_);

    page_width_ = make_double(20.0, 1000.0, kNotebookWidthMm, 1, QStringLiteral(" mm"));
    page_height_ = make_double(20.0, 1000.0, kNotebookHeightMm, 1, QStringLiteral(" mm"));
    form->addRow(QStringLiteral("书写区宽"), page_width_);
    form->addRow(QStringLiteral("书写区高"), page_height_);

    line_count_ = new QSpinBox;
    line_count_->setRange(1, 200);
    line_count_->setValue(kNotebookLines);
    form->addRow(QStringLiteral("横线行数"), line_count_);

    first_line_ = new QSpinBox;
    first_line_->setRange(1, 200);
    first_line_->setValue(1);
    form->addRow(QStringLiteral("从第几行起"), first_line_);

    char_size_ = make_double(0.0, 200.0, 0.0, 2, QStringLiteral(" mm"));
    char_size_->setSpecialValueText(QStringLiteral("自动（等于行距）"));
    form->addRow(QStringLiteral("字宽"), char_size_);

    chars_per_line_ = new QSpinBox;
    chars_per_line_->setRange(0, 200);
    chars_per_line_->setValue(0);
    chars_per_line_->setSpecialValueText(QStringLiteral("自动"));
    form->addRow(QStringLiteral("每行字数"), chars_per_line_);

    show_cells_ = new QCheckBox(QStringLiteral("显示字格"));
    show_cells_->setChecked(true);
    show_rules_ = new QCheckBox(QStringLiteral("显示横线"));
    show_rules_->setChecked(true);
    form->addRow(QString(), show_cells_);
    form->addRow(QString(), show_rules_);

    panel_layout->addWidget(layout_group);

    // ---- 字形 ----
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

    panel_layout->addWidget(style_group);

    // ---- 机器 ----
    auto* machine_group = new QGroupBox(QStringLiteral("机器"));
    auto* machine_form = new QFormLayout(machine_group);

    pen_combo_ = new QComboBox;
    pen_combo_->addItem(QStringLiteral("Z 轴（步进电机）"), 0);
    pen_combo_->addItem(QStringLiteral("主轴 PWM（舵机 / 电磁铁）"), 1);
    machine_form->addRow(QStringLiteral("抬落笔"), pen_combo_);

    feed_draw_ = make_double(60.0, 12000.0, 1200.0, 0, QStringLiteral(" mm/min"));
    machine_form->addRow(QStringLiteral("书写进给"), feed_draw_);
    pen_up_z_ = make_double(-50.0, 50.0, 3.0, 2, QStringLiteral(" mm"));
    machine_form->addRow(QStringLiteral("抬笔 Z"), pen_up_z_);
    pen_down_z_ = make_double(-50.0, 50.0, 0.0, 2, QStringLiteral(" mm"));
    machine_form->addRow(QStringLiteral("落笔 Z"), pen_down_z_);

    panel_layout->addWidget(machine_group);

    // ---- 数据源 ----
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

    panel_layout->addWidget(source_group);
    panel_layout->addStretch();

    auto* scroll = new QScrollArea;
    scroll->setWidget(panel);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    // ---- 左侧停靠：参数面板（可滚动） ----
    auto* params_dock = new QDockWidget(QStringLiteral("参数"), this);
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
    auto* params_action = view_menu->addAction(QStringLiteral("参数面板(&P)"));
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

    auto* toolbar = addToolBar(QStringLiteral("主工具栏"));
    toolbar->setMovable(false);
    toolbar->addAction(export_action);
    toolbar->addSeparator();
    toolbar->addAction(fit_action);
    toolbar->addAction(refresh_action);
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
