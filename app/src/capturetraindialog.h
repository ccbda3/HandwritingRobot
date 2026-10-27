// 原生「采集与训练」窗口：替代网页采集器。
//
// 采集页：单字 / 整行，数位板或鼠标书写，多条样本后一次导出 1.0 / 2.0 JSON。
// 训练页：调用 Python CLI（训练仍按 ADR-0007 放在 Python），完成后自动导出
// weights 并通知主窗口启用。
#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QPair>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTabWidget;
class CaptureCanvas;

class CaptureTrainDialog : public QDialog {
    Q_OBJECT

public:
    explicit CaptureTrainDialog(QWidget* parent = nullptr);
    void setCacheDir(const QString& path) { cache_dir_ = path; }

    // 仅供快照/自测：注入一条演示笔画并导出一份 JSON
    void saveDemoCapture();
    [[nodiscard]] QString lastExportPath() const { return last_export_path_; }
    void showTrainingTab();

signals:
    void modelReady(const QString& weightsPath);

private slots:
    void addSample();
    void exportJson();
    void inspectData();
    void startTraining();
    void cancelTraining();
    void onProcessOutput();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);

private:
    void buildCaptureTab(QTabWidget* tabs);
    void buildTrainingTab(QTabWidget* tabs);
    void updateCaptureStats();
    void appendLog(const QString& text);
    void setTrainingBusy(bool busy);
    void startProcess(const QStringList& args, bool exportStage);
    [[nodiscard]] QString repoRoot() const;
    [[nodiscard]] QString pythonExecutable() const;
    [[nodiscard]] QString defaultCapturePath() const;
    [[nodiscard]] QJsonObject buildSampleObject(const QString& text) const;
    [[nodiscard]] QJsonObject buildLineObject(const QString& text) const;
    [[nodiscard]] QJsonArray strokesToJson() const;
    [[nodiscard]] QJsonArray startTimesToJson() const;
    [[nodiscard]] QJsonArray charRangesToJson(const QString& text) const;
    [[nodiscard]] QVector<QPair<int, int>> segmentRanges(const QString& text) const;

    QTabWidget* tabs_{nullptr};
    QComboBox* mode_combo_{nullptr};
    QLineEdit* guide_edit_{nullptr};
    CaptureCanvas* canvas_{nullptr};
    QLabel* capture_stats_{nullptr};
    QLabel* capture_status_{nullptr};
    QLineEdit* capture_path_{nullptr};
    QVector<QJsonObject> samples_;
    QVector<QJsonObject> lines_;

    QLineEdit* data_path_{nullptr};
    QLineEdit* python_path_{nullptr};
    QLineEdit* model_out_{nullptr};
    QCheckBox* align_check_{nullptr};
    QCheckBox* offline_check_{nullptr};
    QCheckBox* auto_export_{nullptr};
    QSpinBox* epochs_{nullptr};
    QSpinBox* n_points_{nullptr};
    QPushButton* inspect_button_{nullptr};
    QPushButton* train_button_{nullptr};
    QPushButton* cancel_button_{nullptr};
    QProgressBar* progress_{nullptr};
    QPlainTextEdit* log_{nullptr};
    QProcess* process_{nullptr};
    QString cache_dir_;
    QString last_model_path_;
    QString export_path_;
    QString last_export_path_;
    bool exporting_{false};
};
