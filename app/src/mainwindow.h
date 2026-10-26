// Qt 桌面前端主窗口。
//
// 架构约定（见 docs/adr/0003-cpp-core-python-for-training.md）：
//   本进程直接链接 C++ 核心库完成字形加载、渲染与 G 代码生成，
//   **不启动 Python 子进程、不做序列化**。
//   唯一需要 Python 的地方是预取字形数据（handwriting preload），
//   因为核心库刻意不含 HTTP 客户端。
//
// 预览与导出共用同一份数据（同一个 Page + 同一批 StrokeSet），
// 因此「看到的」与「写出来的」必然一致。
#pragma once

#include <QMainWindow>
#include <QString>

#include <string>
#include <vector>

#include "handwriting/bounds.hpp"
#include "handwriting/gcode.hpp"
#include "handwriting/layout.hpp"
#include "handwriting/render.hpp"
#include "handwriting/trajectory.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QDialog;
class QLineEdit;
class QPlainTextEdit;
class NotebookEditor;
class QSpinBox;
class QTimer;
class QDockWidget;
class MachinePanel;
class PreviewCanvas;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    /// 仅供快照/演示：注入已标定边界与机头位置。
    void setDemoWorkspace(const handwriting::bounds::Workspace& ws);
    void setDemoHead(double x, double y);
    void setDemoText(const QString& text);

    /// 仅供快照：拿到选项弹窗指针。
    [[nodiscard]] QDialog* settingsDialog() const noexcept { return settings_dialog_; }
    /// 仅供快照：拿到文本编辑弹窗指针（首次打开后才有）。
    [[nodiscard]] QDialog* textDialog() const noexcept { return text_dialog_; }

private slots:
    void scheduleRefresh();
    void refresh();
    void beginCellEdit(int index);
    void commitCellEdit();
    void showSettings();
    void editText();
    void updateTextPreview();
    void resetLayoutDefaults();
    void exportGcode();
    void exportPreviewImage();
    void browseGlyphCache();
    void openGlyphCacheFolder();

private:
    struct Rendered {
        handwriting::layout::Page page;
        std::vector<handwriting::StrokeSet> glyphs;
        std::vector<std::string> missing;
        std::vector<std::size_t> source_offsets;
        std::string missing_detail;
        bool ok{false};
        QString error;
    };

    void buildUi();
    void buildMenus();
    void buildStatusBar();
    void buildTextDialog();
    void applyTextDialog();
    void updateTextDialogStats();

    [[nodiscard]] handwriting::render::Settings renderSettings() const;
    [[nodiscard]] handwriting::layout::Settings layoutSettings() const;
    [[nodiscard]] handwriting::gcode::PlotSettings plotSettings() const;
    [[nodiscard]] Rendered renderNow() const;
    void updateStatus(const Rendered& rendered);
    /// 供机器面板取用当前 G 代码。只返回缓存，不重新生成 ——
    /// 面板每 20ms 轮询一次，这里若真去生成会把界面卡死。
    [[nodiscard]] const std::string& currentProgram() const noexcept {
        return current_program_;
    }

    void updateModeEnabled();

    // ---- 文本与画布 ----
    QPlainTextEdit* text_edit_{nullptr};  // 隐藏的文本模型，真正渲染用它
    QLabel* text_preview_{nullptr};
    QLabel* text_stats_{nullptr};
    QDialog* text_dialog_{nullptr};
    NotebookEditor* text_dialog_editor_{nullptr};
    QLabel* text_dialog_stats_{nullptr};
    PreviewCanvas* canvas_{nullptr};
    QDialog* settings_dialog_{nullptr};
    QLineEdit* cell_editor_{nullptr};
    int edit_index_{-1};
    Rendered last_rendered_;

    // ---- 版面 ----
    QComboBox* mode_combo_{nullptr};
    QDoubleSpinBox* page_width_{nullptr};
    QDoubleSpinBox* page_height_{nullptr};
    QSpinBox* line_count_{nullptr};
    QSpinBox* first_line_{nullptr};
    QDoubleSpinBox* char_size_{nullptr};
    QSpinBox* chars_per_line_{nullptr};
    QCheckBox* show_cells_{nullptr};
    QCheckBox* show_rules_{nullptr};

    // ---- 字形 ----
    QDoubleSpinBox* slant_{nullptr};
    QDoubleSpinBox* jitter_{nullptr};
    QDoubleSpinBox* pressure_amp_{nullptr};

    // ---- 机器 ----
    QComboBox* pen_combo_{nullptr};
    QDoubleSpinBox* feed_draw_{nullptr};
    QDoubleSpinBox* pen_up_z_{nullptr};
    QDoubleSpinBox* pen_down_z_{nullptr};
    QCheckBox* invert_y_{nullptr};

    // ---- 数据源 ----
    QLineEdit* cache_edit_{nullptr};

    // ---- 状态栏 ----
    QLabel* status_chars_{nullptr};
    QLabel* status_page_{nullptr};
    QLabel* status_machine_{nullptr};
    QLabel* status_zoom_{nullptr};

    QTimer* refresh_timer_{nullptr};

    MachinePanel* machine_panel_{nullptr};
    QDockWidget* dock_{nullptr};
    QDockWidget* params_dock_{nullptr};
    std::string current_program_;
};
