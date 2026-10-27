#include "capturetraindialog.h"

#include "capturecanvas.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTextCursor>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

CaptureTrainDialog::CaptureTrainDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("采集与训练"));
    resize(920, 780);

    auto* layout = new QVBoxLayout(this);
    tabs_ = new QTabWidget(this);
    buildCaptureTab(tabs_);
    buildTrainingTab(tabs_);
    layout->addWidget(tabs_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    if (auto* close = buttons->button(QDialogButtonBox::Close)) {
        close->setText(QStringLiteral("关闭"));
    }
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::hide);
    layout->addWidget(buttons);
}

void CaptureTrainDialog::buildCaptureTab(QTabWidget* tabs) {
    auto* tab = new QWidget;
    auto* layout = new QVBoxLayout(tab);

    auto* top = new QHBoxLayout;
    mode_combo_ = new QComboBox;
    mode_combo_->addItem(QStringLiteral("单字"));
    mode_combo_->addItem(QStringLiteral("整行"));
    guide_edit_ = new QLineEdit(QStringLiteral("永"));
    guide_edit_->setMaxLength(1);
    top->addWidget(new QLabel(QStringLiteral("模式")));
    top->addWidget(mode_combo_);
    top->addWidget(new QLabel(QStringLiteral("内容")));
    top->addWidget(guide_edit_, 1);
    layout->addLayout(top);

    canvas_ = new CaptureCanvas(tab);
    canvas_->setMinimumSize(640, 400);
    canvas_->setGuide(QStringLiteral("永"), false);
    layout->addWidget(canvas_, 1);

    auto* stats_row = new QHBoxLayout;
    capture_stats_ = new QLabel;
    stats_row->addWidget(capture_stats_);
    stats_row->addStretch();
    auto* undo = new QPushButton(QStringLiteral("撤销一笔"));
    auto* clear = new QPushButton(QStringLiteral("清空画布"));
    stats_row->addWidget(undo);
    stats_row->addWidget(clear);
    layout->addLayout(stats_row);

    auto* path_row = new QHBoxLayout;
    capture_path_ = new QLineEdit(defaultCapturePath());
    auto* browse = new QPushButton(QStringLiteral("保存到…"));
    path_row->addWidget(new QLabel(QStringLiteral("导出路径")));
    path_row->addWidget(capture_path_, 1);
    path_row->addWidget(browse);
    layout->addLayout(path_row);

    auto* actions = new QHBoxLayout;
    auto* add = new QPushButton(QStringLiteral("添加当前样本"));
    auto* export_button = new QPushButton(QStringLiteral("导出 JSON"));
    actions->addWidget(add);
    actions->addWidget(export_button);
    actions->addStretch();
    layout->addLayout(actions);

    capture_status_ = new QLabel;
    capture_status_->setStyleSheet(QStringLiteral("color:#2e7d32;"));
    capture_status_->setWordWrap(true);
    layout->addWidget(capture_status_);

    tabs->addTab(tab, QStringLiteral("采集"));

    connect(mode_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                const bool line = index == 1;
                guide_edit_->setMaxLength(line ? 200 : 1);
                if (!line && guide_edit_->text().size() > 1) {
                    guide_edit_->setText(guide_edit_->text().left(1));
                }
                canvas_->setGuide(guide_edit_->text(), line);
                updateCaptureStats();
            });
    connect(guide_edit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        canvas_->setGuide(text, mode_combo_->currentIndex() == 1);
    });
    connect(canvas_, &CaptureCanvas::strokesChanged, this,
            &CaptureTrainDialog::updateCaptureStats);
    connect(undo, &QPushButton::clicked, canvas_, &CaptureCanvas::undoStroke);
    connect(clear, &QPushButton::clicked, canvas_, &CaptureCanvas::clear);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("导出采集 JSON"), capture_path_->text(),
            QStringLiteral("JSON (*.json)"));
        if (!path.isEmpty()) {
            capture_path_->setText(path);
        }
    });
    connect(add, &QPushButton::clicked, this, &CaptureTrainDialog::addSample);
    connect(export_button, &QPushButton::clicked, this, &CaptureTrainDialog::exportJson);
    updateCaptureStats();
}

void CaptureTrainDialog::buildTrainingTab(QTabWidget* tabs) {
    auto* tab = new QWidget;
    auto* layout = new QVBoxLayout(tab);

    auto* data_row = new QHBoxLayout;
    data_path_ = new QLineEdit;
    data_path_->setPlaceholderText(QStringLiteral("先在上方采集并导出 JSON"));
    auto* browse_data = new QPushButton(QStringLiteral("选择…"));
    data_row->addWidget(new QLabel(QStringLiteral("数据")));
    data_row->addWidget(data_path_, 1);
    data_row->addWidget(browse_data);
    layout->addLayout(data_row);

    auto* form = new QFormLayout;
    python_path_ = new QLineEdit(pythonExecutable());
    form->addRow(QStringLiteral("Python"), python_path_);
    model_out_ = new QLineEdit(repoRoot() + QStringLiteral("/data/exports/style-model-real.pt"));
    form->addRow(QStringLiteral("输出模型"), model_out_);
    epochs_ = new QSpinBox;
    epochs_->setRange(1, 5000);
    epochs_->setValue(200);
    form->addRow(QStringLiteral("训练轮数"), epochs_);
    n_points_ = new QSpinBox;
    n_points_->setRange(8, 128);
    n_points_->setValue(48);
    form->addRow(QStringLiteral("每笔点数"), n_points_);
    align_check_ = new QCheckBox(QStringLiteral("对齐到标准骨架（--align）"));
    align_check_->setChecked(true);
    form->addRow(QString(), align_check_);
    offline_check_ = new QCheckBox(QStringLiteral("离线（用本地字形缓存）"));
    offline_check_->setChecked(true);
    form->addRow(QString(), offline_check_);
    auto_export_ = new QCheckBox(QStringLiteral("训练完自动导出 weights 并启用"));
    auto_export_->setChecked(true);
    form->addRow(QString(), auto_export_);
    layout->addLayout(form);

    auto* buttons = new QHBoxLayout;
    inspect_button_ = new QPushButton(QStringLiteral("体检数据"));
    train_button_ = new QPushButton(QStringLiteral("开始训练"));
    cancel_button_ = new QPushButton(QStringLiteral("取消"));
    cancel_button_->setEnabled(false);
    buttons->addWidget(inspect_button_);
    buttons->addWidget(train_button_);
    buttons->addWidget(cancel_button_);
    buttons->addStretch();
    layout->addLayout(buttons);

    progress_ = new QProgressBar;
    progress_->setRange(0, 100);
    progress_->setValue(0);
    layout->addWidget(progress_);

    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setPlaceholderText(QStringLiteral("训练日志会显示在这里"));
    layout->addWidget(log_, 1);

    tabs->addTab(tab, QStringLiteral("训练"));

    connect(browse_data, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择采集 JSON"), data_path_->text(),
            QStringLiteral("JSON (*.json)"));
        if (!path.isEmpty()) {
            data_path_->setText(path);
        }
    });
    connect(inspect_button_, &QPushButton::clicked, this,
            &CaptureTrainDialog::inspectData);
    connect(train_button_, &QPushButton::clicked, this,
            &CaptureTrainDialog::startTraining);
    connect(cancel_button_, &QPushButton::clicked, this,
            &CaptureTrainDialog::cancelTraining);
}

void CaptureTrainDialog::updateCaptureStats() {
    if (capture_stats_ == nullptr) {
        return;
    }
    const int current = static_cast<int>(canvas_->strokes().size());
    const int total = mode_combo_->currentIndex() == 1
                          ? static_cast<int>(lines_.size())
                          : static_cast<int>(samples_.size());
    capture_stats_->setText(
        QStringLiteral("已采集 %1 条 · 当前 %2 笔 · %3 Hz · 压感 %4")
            .arg(total)
            .arg(current)
            .arg(canvas_->samplingRate())
            .arg(canvas_->pressureSupported() ? QStringLiteral("有") : QStringLiteral("无")));
}

void CaptureTrainDialog::addSample() {
    const bool line = mode_combo_->currentIndex() == 1;
    QString text = guide_edit_->text().trimmed();
    if (text.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("缺少内容"),
                             QStringLiteral("请先填写要写的字或整行文字。"));
        return;
    }
    if (!line) {
        text = text.left(1);
    }
    if (canvas_->strokes().empty()) {
        QMessageBox::warning(this, QStringLiteral("没有笔画"),
                             QStringLiteral("请先在纸面上写一遍。"));
        return;
    }
    if (line) {
        lines_.append(buildLineObject(text));
    } else {
        samples_.append(buildSampleObject(text));
    }
    canvas_->clear();
    updateCaptureStats();
    capture_status_->setText(
        QStringLiteral("已添加「%1」，共 %2 条。")
            .arg(text)
            .arg(line ? lines_.size() : samples_.size()));
}

void CaptureTrainDialog::exportJson() {
    const bool line = mode_combo_->currentIndex() == 1;
    const auto& items = line ? lines_ : samples_;
    if (items.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("没有样本"),
                             QStringLiteral("请先添加至少一个样本。"));
        return;
    }

    QJsonObject root;
    root[QStringLiteral("format")] =
        line ? QStringLiteral("handwriting-capture/2.0")
             : QStringLiteral("handwriting-capture/1.0");
    QJsonObject meta;
    meta[QStringLiteral("device")] =
        canvas_->pressureSupported() ? QStringLiteral("qt-tablet") : QStringLiteral("qt-mouse");
    meta[QStringLiteral("pressureSupported")] = canvas_->pressureSupported();
    meta[QStringLiteral("samplingHz")] = canvas_->samplingRate();
    meta[QStringLiteral("createdAt")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    meta[QStringLiteral("coordSystem")] =
        QStringLiteral("归一化到书写区，原点左上，x右 y下，0~1");
    if (line) {
        meta[QStringLiteral("lineCount")] = static_cast<int>(items.size());
    } else {
        meta[QStringLiteral("charCount")] = static_cast<int>(items.size());
    }
    root[QStringLiteral("meta")] = meta;

    QJsonArray array;
    for (const auto& item : items) {
        array.append(item);
    }
    root[line ? QStringLiteral("lines") : QStringLiteral("samples")] = array;

    QString path = capture_path_->text().trimmed();
    if (path.isEmpty()) {
        path = defaultCapturePath();
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"),
                             QStringLiteral("无法写入：%1").arg(path));
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();

    last_export_path_ = path;
    data_path_->setText(path);
    capture_status_->setText(
        QStringLiteral("已导出 %1 条 → %2").arg(items.size()).arg(path));
    appendLog(QStringLiteral("已导出采集：%1（%2 条）").arg(path).arg(items.size()));
    capture_path_->setText(defaultCapturePath());
}

QJsonObject CaptureTrainDialog::buildSampleObject(const QString& text) const {
    QJsonObject sample;
    sample[QStringLiteral("index")] = static_cast<int>(samples_.size());
    sample[QStringLiteral("char")] = text;
    sample[QStringLiteral("strokeCount")] = static_cast<int>(canvas_->strokes().size());
    sample[QStringLiteral("rate")] = canvas_->samplingRate();
    sample[QStringLiteral("pressure")] = canvas_->pressureSupported();
    sample[QStringLiteral("strokes")] = strokesToJson();
    sample[QStringLiteral("strokeStartT")] = startTimesToJson();
    return sample;
}

QJsonObject CaptureTrainDialog::buildLineObject(const QString& text) const {
    QJsonObject line;
    line[QStringLiteral("index")] = static_cast<int>(lines_.size());
    line[QStringLiteral("text")] = text;
    line[QStringLiteral("kind")] = QStringLiteral("textline");
    line[QStringLiteral("strokeCount")] = static_cast<int>(canvas_->strokes().size());
    line[QStringLiteral("rate")] = canvas_->samplingRate();
    line[QStringLiteral("pressure")] = canvas_->pressureSupported();
    line[QStringLiteral("strokes")] = strokesToJson();
    line[QStringLiteral("strokeStartT")] = startTimesToJson();
    line[QStringLiteral("charRanges")] = charRangesToJson(text);
    return line;
}

QJsonArray CaptureTrainDialog::strokesToJson() const {
    QJsonArray out;
    for (const auto& stroke : canvas_->strokes()) {
        QJsonArray points;
        for (const auto& point : stroke.points()) {
            QJsonArray values;
            values.append(std::round(point.x * 100000.0) / 100000.0);
            values.append(std::round(point.y * 100000.0) / 100000.0);
            values.append(std::round(point.pressure * 10000.0) / 10000.0);
            values.append(point.t);
            points.append(values);
        }
        out.append(points);
    }
    return out;
}

QJsonArray CaptureTrainDialog::startTimesToJson() const {
    QJsonArray out;
    for (const auto& stroke : canvas_->strokes()) {
        out.append(stroke.points().front().t);
    }
    return out;
}

QJsonArray CaptureTrainDialog::charRangesToJson(const QString& text) const {
    QJsonArray out;
    for (const auto& range : segmentRanges(text)) {
        QJsonArray pair;
        pair.append(range.first);
        pair.append(range.second);
        out.append(pair);
    }
    return out;
}

QVector<QPair<int, int>> CaptureTrainDialog::segmentRanges(const QString& text) const {
    const auto& strokes = canvas_->strokes();
    const int n = static_cast<int>(strokes.size());
    const int chars = static_cast<int>(text.size());
    QVector<QPair<int, int>> ranges(chars, qMakePair(0, 0));

    QVector<int> positions;
    for (int i = 0; i < chars; ++i) {
        if (!text.at(i).isSpace()) {
            positions.push_back(i);
        }
    }
    if (positions.isEmpty() || n == 0) {
        return ranges;
    }

    if (n <= positions.size()) {
        for (int i = 0; i < n; ++i) {
            ranges[positions[i]] = qMakePair(i, i + 1);
        }
        for (int i = n; i < positions.size(); ++i) {
            ranges[positions[i]] = qMakePair(n, n);
        }
        return ranges;
    }

    QVector<QPair<double, int>> gaps;
    gaps.reserve(n - 1);
    for (int i = 1; i < n; ++i) {
        const int prev_end = strokes[i - 1].points().back().t;
        const int next_start = strokes[i].points().front().t;
        gaps.push_back(qMakePair(static_cast<double>(next_start - prev_end), i));
    }
    std::sort(gaps.begin(), gaps.end(),
              [](const QPair<double, int>& a, const QPair<double, int>& b) {
                  return a.first > b.first;
              });

    QVector<int> boundaries;
    const int need = static_cast<int>(positions.size()) - 1;
    for (int i = 0; i < need && i < gaps.size(); ++i) {
        boundaries.push_back(gaps[i].second);
    }
    std::sort(boundaries.begin(), boundaries.end());

    int start = 0;
    for (int i = 0; i < positions.size(); ++i) {
        const int end = (i < boundaries.size()) ? boundaries[i] : n;
        ranges[positions[i]] = qMakePair(start, end);
        start = end;
    }
    return ranges;
}

void CaptureTrainDialog::showTrainingTab() {
    if (tabs_ != nullptr) {
        tabs_->setCurrentIndex(1);
    }
}

void CaptureTrainDialog::saveDemoCapture() {
    mode_combo_->setCurrentIndex(0);
    guide_edit_->setText(QStringLiteral("永"));
    canvas_->setDemoStroke();
    addSample();
    exportJson();
}

void CaptureTrainDialog::inspectData() {
    const QString data = data_path_->text().trimmed();
    if (data.isEmpty() || !QFileInfo::exists(data)) {
        QMessageBox::warning(this, QStringLiteral("缺少数据"),
                             QStringLiteral("请先选择采集 JSON。"));
        return;
    }

    auto* process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setWorkingDirectory(repoRoot());
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString src = repoRoot() + QStringLiteral("/src");
    const QString existing = env.value(QStringLiteral("PYTHONPATH"));
    env.insert(QStringLiteral("PYTHONPATH"),
               existing.isEmpty() ? src : src + QDir::listSeparator() + existing);
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    process->setProcessEnvironment(env);

    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        appendLog(QString::fromUtf8(process->readAllStandardOutput()));
    });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, process](int, QProcess::ExitStatus) {
                process->deleteLater();
                appendLog(QStringLiteral(
                    "体检结束。有警告也可以训练，但建议先补压感/减少缺字。"));
            });

    appendLog(QStringLiteral("开始体检：%1").arg(data));
    process->start(python_path_->text().trimmed(),
                   {QStringLiteral("-m"), QStringLiteral("handwriting.cli"),
                    QStringLiteral("inspect"), data});
    if (!process->waitForStarted(3000)) {
        appendLog(QStringLiteral("无法启动 Python：%1").arg(python_path_->text()));
        process->deleteLater();
    }
}

void CaptureTrainDialog::startTraining() {
    if (process_ != nullptr) {
        appendLog(QStringLiteral("已有任务在运行。"));
        return;
    }
    const QString data = data_path_->text().trimmed();
    if (data.isEmpty() || !QFileInfo::exists(data)) {
        QMessageBox::warning(this, QStringLiteral("缺少数据"),
                             QStringLiteral("请先选择采集 JSON。"));
        return;
    }
    const QString python = python_path_->text().trimmed();
    if (python.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("缺少 Python"),
                             QStringLiteral("请填写 Python 可执行文件路径。"));
        return;
    }
    const QString out = model_out_->text().trimmed();
    if (out.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("缺少输出路径"),
                             QStringLiteral("请填写模型输出路径。"));
        return;
    }
    QDir().mkpath(QFileInfo(out).absolutePath());

    QStringList args = {QStringLiteral("-m"), QStringLiteral("handwriting.cli"),
                        QStringLiteral("train"), QStringLiteral("--data"), data,
                        QStringLiteral("--epochs"), QString::number(epochs_->value()),
                        QStringLiteral("--n-points"), QString::number(n_points_->value()),
                        QStringLiteral("--out"), out};
    if (align_check_->isChecked()) {
        args << QStringLiteral("--align");
    }
    if (offline_check_->isChecked()) {
        args << QStringLiteral("--offline");
    }
    if (!cache_dir_.isEmpty()) {
        args << QStringLiteral("--cache-dir") << cache_dir_;
    }

    last_model_path_ = out;
    log_->clear();
    appendLog(QStringLiteral("开始训练：%1").arg(data));
    progress_->setValue(0);
    setTrainingBusy(true);
    startProcess(args, false);
}

void CaptureTrainDialog::startProcess(const QStringList& args, bool exportStage) {
    exporting_ = exportStage;
    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::MergedChannels);
    process_->setWorkingDirectory(repoRoot());

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString src = repoRoot() + QStringLiteral("/src");
    const QString existing = env.value(QStringLiteral("PYTHONPATH"));
    env.insert(QStringLiteral("PYTHONPATH"),
               existing.isEmpty() ? src : src + QDir::listSeparator() + existing);
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    process_->setProcessEnvironment(env);

    connect(process_, &QProcess::readyReadStandardOutput, this,
            &CaptureTrainDialog::onProcessOutput);
    connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            &CaptureTrainDialog::onProcessFinished);

    process_->start(python_path_->text().trimmed(), args);
    if (!process_->waitForStarted(3000)) {
        appendLog(QStringLiteral("无法启动 Python：%1").arg(python_path_->text()));
        process_->deleteLater();
        process_ = nullptr;
        setTrainingBusy(false);
    }
}

void CaptureTrainDialog::onProcessOutput() {
    if (process_ == nullptr) {
        return;
    }
    const QString text = QString::fromUtf8(process_->readAllStandardOutput());
    appendLog(text);

    static const QRegularExpression re(QStringLiteral("epoch\\s+(\\d+)\\s*/\\s*(\\d+)"));
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        const int current = match.captured(1).toInt();
        const int total = match.captured(2).toInt();
        if (total > 0) {
            progress_->setValue(current * 100 / total);
        }
    }
}

void CaptureTrainDialog::onProcessFinished(int exitCode, QProcess::ExitStatus /*status*/) {
    const bool was_export = exporting_;
    if (process_ != nullptr) {
        process_->deleteLater();
        process_ = nullptr;
    }
    if (exitCode != 0) {
        appendLog(was_export ? QStringLiteral("导出失败。") : QStringLiteral("训练失败。"));
        setTrainingBusy(false);
        exporting_ = false;
        return;
    }
    if (was_export) {
        appendLog(QStringLiteral("weights 已导出：%1").arg(export_path_));
        progress_->setValue(100);
        setTrainingBusy(false);
        exporting_ = false;
        emit modelReady(export_path_);
        return;
    }
    if (auto_export_->isChecked()) {
        export_path_ = QFileInfo(last_model_path_).absolutePath() + QStringLiteral("/") +
                       QFileInfo(last_model_path_).completeBaseName() +
                       QStringLiteral(".json");
        appendLog(QStringLiteral("训练完成，导出 weights → %1").arg(export_path_));
        const QStringList args = {QStringLiteral("-m"), QStringLiteral("handwriting.cli"),
                                  QStringLiteral("export-weights"), QStringLiteral("--model"),
                                  last_model_path_, QStringLiteral("-o"), export_path_};
        startProcess(args, true);
        return;
    }
    appendLog(QStringLiteral("训练完成：%1").arg(last_model_path_));
    progress_->setValue(100);
    setTrainingBusy(false);
}

void CaptureTrainDialog::cancelTraining() {
    if (process_ != nullptr) {
        process_->kill();
        appendLog(QStringLiteral("已取消。"));
    }
}

void CaptureTrainDialog::setTrainingBusy(bool busy) {
    train_button_->setEnabled(!busy);
    cancel_button_->setEnabled(busy);
    if (busy) {
        progress_->setValue(0);
    }
}

void CaptureTrainDialog::appendLog(const QString& text) {
    if (log_ == nullptr) {
        return;
    }
    log_->moveCursor(QTextCursor::End);
    log_->insertPlainText(text);
    if (!text.endsWith(QLatin1Char('\n'))) {
        log_->insertPlainText(QStringLiteral("\n"));
    }
}

QString CaptureTrainDialog::repoRoot() const {
    QDir dir(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 6; ++i) {
        if (QFileInfo::exists(dir.filePath(QStringLiteral("src/handwriting/cli.py")))) {
            return dir.absolutePath();
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    return QDir::currentPath();
}

QString CaptureTrainDialog::pythonExecutable() const {
    const QString anaconda = QStringLiteral("C:/ProgramData/anaconda3/python.exe");
    if (QFileInfo::exists(anaconda)) {
        return anaconda;
    }
    const QString found = QStandardPaths::findExecutable(QStringLiteral("python"));
    return found.isEmpty() ? QStringLiteral("python") : found;
}

QString CaptureTrainDialog::defaultCapturePath() const {
    const QString dir = repoRoot() + QStringLiteral("/data/captures");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/capture-") +
           QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")) +
           QStringLiteral(".json");
}
