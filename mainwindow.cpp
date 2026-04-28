#include "mainwindow.h"

#include "DBManager.h"

#include <QWidget>
#include <QGridLayout>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
#include <QToolBar>
#include <QAction>
#include <QStatusBar>
#include <QLabel>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QMessageBox>
#include <QThread>
#include <QMenu>
#include <QToolButton>
#include <QDockWidget>
#include <QListWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QFormLayout>
#include <QComboBox>
#include <QSpinBox>
#include <QAbstractItemView>
#include <QDrag>
#include <QMimeData>
#include <QSignalBlocker>
#include <QSettings>
#include <opencv2/core/cuda.hpp>

namespace {

constexpr int kCameraIdRole = Qt::UserRole + 1;
constexpr int kCameraNameRole = Qt::UserRole + 2;
constexpr int kCameraStreamTypeRole = Qt::UserRole + 3;
constexpr int kCameraEnabledRole = Qt::UserRole + 4;
constexpr int kCameraUrlRole = Qt::UserRole + 5;
constexpr const char* kCameraFormExpandedSetting = "ui/streamList/cameraFormExpanded";

QString streamTypeToText(StreamType type) {
    switch (type) {
    case StreamType::RTSP: return "RTSP";
    case StreamType::ONVIF: return "ONVIF";
    }
    return "Unknown";
}

QString makeCameraTooltip(const camera_info& camera) {
    return QString("camera_id: %1\nname: %2\nstream_type: %3\nenabled: %4\nurl: %5")
        .arg(camera.camera_id)
        .arg(QString::fromStdString(camera.name))
        .arg(streamTypeToText(camera.stream_type))
        .arg(camera.enabled ? "true" : "false")
        .arg(QString::fromStdString(camera.url));
}

QString makeCameraDisplayText(const camera_info& camera) {
    const QString name = QString::fromStdString(camera.name).trimmed();
    const QString url = QString::fromStdString(camera.url).trimmed();
    if (name.isEmpty() || name == url) {
        return url;
    }
    return QString("%1 (%2)").arg(name, url);
}

bool listHasUrl(const QListWidget* list, const QString& url) {
    if (!list) return false;
    const QString target = url.trimmed();
    if (target.isEmpty()) return false;

    for (int i = 0; i < list->count(); ++i) {
        const auto* item = list->item(i);
        if (!item) continue;

        const QString itemUrl = item->data(kCameraUrlRole).toString().trimmed().isEmpty()
                                    ? item->text().trimmed()
                                    : item->data(kCameraUrlRole).toString().trimmed();
        if (itemUrl == target) {
            return true;
        }
    }

    return false;
}

bool listHasCameraId(const QListWidget* list, int cameraId) {
    if (!list || cameraId <= 0) return false;

    for (int i = 0; i < list->count(); ++i) {
        const auto* item = list->item(i);
        if (!item) continue;
        if (item->data(kCameraIdRole).toInt() == cameraId) {
            return true;
        }
    }

    return false;
}

bool parseStreamType(const QString& text, StreamType& outType) {
    const QString normalized = text.trimmed().toUpper();
    if (normalized == "RTSP") {
        outType = StreamType::RTSP;
        return true;
    }
    if (normalized == "ONVIF") {
        outType = StreamType::ONVIF;
        return true;
    }
    return false;
}

bool parseEnabledValue(const QString& text, bool& outEnabled) {
    const QString normalized = text.trimmed().toLower();
    if (normalized == "1" || normalized == "true" || normalized == "y" || normalized == "yes") {
        outEnabled = true;
        return true;
    }
    if (normalized == "0" || normalized == "false" || normalized == "n" || normalized == "no") {
        outEnabled = false;
        return true;
    }
    return false;
}

bool isCameraCsvHeader(const QStringList& cols) {
    if (cols.size() < 5) return false;
    const QString c0 = cols[0].trimmed().toLower();
    const QString c1 = cols[1].trimmed().toLower();
    const QString c2 = cols[2].trimmed().toLower();
    const QString c3 = cols[3].trimmed().toLower();
    const QString c4 = cols[4].trimmed().toLower();
    return c0 == "id" && c1 == "name" && c2 == "url" && c3 == "type" && c4 == "enabled";
}

bool parseCameraCsvLine(const QString& line, camera_info& camera) {
    const QStringList cols = line.split(',', Qt::KeepEmptyParts);
    if (cols.size() < 5) {
        return false;
    }

    bool idOk = false;
    const int cameraId = cols[0].trimmed().toInt(&idOk);
    if (!idOk || cameraId <= 0) {
        return false;
    }

    StreamType type = StreamType::RTSP;
    if (!parseStreamType(cols[3], type)) {
        return false;
    }

    bool enabled = true;
    if (!parseEnabledValue(cols[4], enabled)) {
        return false;
    }

    const QString url = cols[2].trimmed();
    if (url.isEmpty()) {
        return false;
    }

    QString name = cols[1].trimmed();
    if (name.isEmpty()) {
        name = url;
    }

    camera = {};
    camera.camera_id = cameraId;
    camera.name = name.toStdString();
    camera.stream_type = type;
    camera.url = url.toStdString();
    camera.enabled = enabled;
    return true;
}

}

class StreamListWidget final : public QListWidget {
public:
    using QListWidget::QListWidget;

protected:
    void startDrag(Qt::DropActions supportedActions) override {
        auto* item = currentItem();
        if (!item) return;

        const QString url = item->data(kCameraUrlRole).toString().trimmed().isEmpty()
                                ? item->text().trimmed()
                                : item->data(kCameraUrlRole).toString().trimmed();
        if (url.isEmpty()) return;

        auto* mime = new QMimeData();
        mime->setText(url);
        auto* drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->exec(supportedActions, Qt::CopyAction);
    }
};

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    buildUi();
    setWindowTitle("RTSP 멀티채널 뷰어");
    resize(1600, 900);
}

MainWindow::~MainWindow() {
    // MainWindow 종료 시점에는 워커를 동기 정리해 QThread 파괴 경쟁을 피한다.
    const auto workers = m_workers;
    m_workers.clear();
    for (auto* w : workers) {
        disconnect(w, nullptr, this, nullptr);
        w->stop();
        w->wait(5000);
        delete w;
    }
}

// ── UI 빌드 ───────────────────────────────────────────────────

void MainWindow::buildUi() {
    buildStreamListPanel();

    m_central = new QWidget(this);
#ifdef Q_OS_WIN
    // Direct2D로 렌더링되는 VideoCell(자식 HWND)들이 Qt의 배경 페인팅에
    // 가려지지 않도록 중앙 컨테이너를 네이티브 창으로 격리한다.
    m_central->setAttribute(Qt::WA_NativeWindow);
    m_central->setAttribute(Qt::WA_NoSystemBackground);
    m_central->setAttribute(Qt::WA_OpaquePaintEvent);
#endif
    m_grid    = new QGridLayout(m_central);
    m_grid->setSpacing(4);
    m_grid->setContentsMargins(4, 4, 4, 4);

    m_cells.resize(TOTAL);
    for (int i = 0; i < TOTAL; ++i) {
        auto* cell = new VideoCell(i, m_central);
        connect(cell, &VideoCell::addRequested,    this, &MainWindow::onAddRequested);
        connect(cell, &VideoCell::moveRequested,   this, &MainWindow::onMoveRequested);
        connect(cell, &VideoCell::removeRequested, this, &MainWindow::onRemoveRequested);
        m_cells[i] = cell;
    }
    setCentralWidget(m_central);

    // ── 툴바 ─────────────────────────────────────────────────
    auto* tb = addToolBar("도구");
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    auto* actLoad = tb->addAction("📂  URL 목록 불러오기");
    actLoad->setToolTip("한 줄에 URL 하나씩 적힌 .txt 파일을 선택합니다.\n'#'으로 시작하는 줄은 주석으로 무시됩니다.");
    connect(actLoad, &QAction::triggered, this, &MainWindow::loadUrlsFromFile);

    tb->addSeparator();

    auto* actClear = tb->addAction("🗑  모두 제거");
    connect(actClear, &QAction::triggered, this, &MainWindow::removeAllStreams);

    tb->addSeparator();

    auto* layoutMenu = new QMenu(this);
    addLayoutPresetAction(layoutMenu, "1 x 1", 1, 1);
    addLayoutPresetAction(layoutMenu, "2 x 2", 2, 2);
    addLayoutPresetAction(layoutMenu, "3 x 3", 3, 3);
    addLayoutPresetAction(layoutMenu, "4 x 4", 4, 4);
    layoutMenu->addSeparator();
    addLayoutPresetAction(layoutMenu, "2 x 3", 2, 3);
    addLayoutPresetAction(layoutMenu, "3 x 2", 3, 2);
    layoutMenu->addSeparator();
    auto* actFocusRing = layoutMenu->addAction("큰 화면 1 + 주변(링)");
    connect(actFocusRing, &QAction::triggered, this, [this]() {
        applyFocusRingLayout();
        updateStatusBar();
    });
    auto* actTripleQuad = layoutMenu->addAction("큰 화면 3 + 소형 4");
    connect(actTripleQuad, &QAction::triggered, this, [this]() {
        applyTriplePlusQuadLayout();
        updateStatusBar();
    });

    auto* layoutButton = new QToolButton(tb);
    layoutButton->setText("레이아웃");
    layoutButton->setPopupMode(QToolButton::InstantPopup);
    layoutButton->setMenu(layoutMenu);
    tb->addWidget(layoutButton);

    // ── 상태바 ────────────────────────────────────────────────
    m_statusLabel = new QLabel(this);
    statusBar()->addWidget(m_statusLabel);
    applyGridLayout(4, 4);
    updateStatusBar();
}

void MainWindow::buildStreamListPanel() {
    m_streamDock = new QDockWidget("스트림 목록", this);
    m_streamDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    auto* panel = new QWidget(m_streamDock);
    auto* vbox = new QVBoxLayout(panel);
    vbox->setContentsMargins(6, 6, 6, 6);
    vbox->setSpacing(6);

    m_cameraIdInput = new QSpinBox(panel);
    m_cameraIdInput->setRange(1, 2147483647);
    m_cameraIdInput->setValue(m_nextCameraId);

    m_nameInput = new QLineEdit(panel);
    m_nameInput->setPlaceholderText("카메라 이름");

    m_streamTypeInput = new QComboBox(panel);
    m_streamTypeInput->addItem("RTSP", static_cast<int>(StreamType::RTSP));
    m_streamTypeInput->addItem("ONVIF", static_cast<int>(StreamType::ONVIF));

    m_streamInput = new QLineEdit(panel);
    m_streamInput->setPlaceholderText("스트림 주소 (예: rtsp://...)");
    connect(m_streamInput, &QLineEdit::returnPressed, this, &MainWindow::addStreamAddress);

    m_enabledInput = new QCheckBox("활성", panel);
    m_enabledInput->setChecked(true);

    auto* form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(4);
    form->addRow("camera_id", m_cameraIdInput);
    form->addRow("이름", m_nameInput);
    form->addRow("스트림 타입", m_streamTypeInput);
    form->addRow("URL", m_streamInput);
    form->addRow("상태", m_enabledInput);

    auto* addBtn = new QPushButton("추가", panel);
    auto* editBtn = new QPushButton("수정", panel);
    auto* delBtn = new QPushButton("삭제", panel);
    connect(addBtn, &QPushButton::clicked, this, &MainWindow::addStreamAddress);
    connect(editBtn, &QPushButton::clicked, this, &MainWindow::updateSelectedStreamAddress);
    connect(delBtn, &QPushButton::clicked, this, &MainWindow::removeSelectedStreamAddress);

    auto* btnRow = new QHBoxLayout();
    btnRow->setContentsMargins(0, 0, 0, 0);
    btnRow->addWidget(addBtn);
    btnRow->addWidget(editBtn);
    btnRow->addWidget(delBtn);

    auto* formContainer = new QWidget(panel);
    auto* formContainerLayout = new QVBoxLayout(formContainer);
    formContainerLayout->setContentsMargins(0, 0, 0, 0);
    formContainerLayout->setSpacing(4);
    formContainerLayout->addLayout(form);
    formContainerLayout->addLayout(btnRow);

    auto* foldButton = new QToolButton(panel);
    foldButton->setCheckable(true);
    foldButton->setChecked(true);
    foldButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    foldButton->setArrowType(Qt::DownArrow);
    foldButton->setText("카메라 정보");

    connect(foldButton, &QToolButton::toggled, this,
            [formContainer, foldButton](bool expanded) {
                formContainer->setVisible(expanded);
                foldButton->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
                QSettings settings;
                settings.setValue(kCameraFormExpandedSetting, expanded);
                settings.sync();
            });

    {
        QSettings settings;
        const bool expanded = settings.value(kCameraFormExpandedSetting, true).toBool();
        foldButton->setChecked(expanded);
    }

    m_streamList = new StreamListWidget(panel);
    m_streamList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_streamList->setDragEnabled(true);
    m_streamList->setDefaultDropAction(Qt::CopyAction);
    m_streamList->setToolTip("목록 항목을 비디오 셀로 드래그하면 해당 셀에서 스트림이 시작됩니다.");
    connect(m_streamList, &QListWidget::itemDoubleClicked,
            this, &MainWindow::onStreamListDoubleClicked);

    // 선택한 항목을 인라인 입력 폼으로 가져와 수정 흐름을 단순화
    connect(m_streamList, &QListWidget::currentItemChanged,
            this, [this](QListWidgetItem* current, QListWidgetItem*) {
                if (!m_cameraIdInput || !m_nameInput || !m_streamTypeInput || !m_streamInput || !m_enabledInput)
                    return;
                if (!current) {
                    resetInlineCameraForm(false);
                    return;
                }

                const int cameraId = current->data(kCameraIdRole).toInt();
                if (cameraId > 0) m_cameraIdInput->setValue(cameraId);
                m_nameInput->setText(current->data(kCameraNameRole).toString());

                const int streamType = current->data(kCameraStreamTypeRole).toInt();
                const int idx = m_streamTypeInput->findData(streamType);
                m_streamTypeInput->setCurrentIndex(idx >= 0 ? idx : 0);

                const QString url = current->data(kCameraUrlRole).toString().trimmed().isEmpty()
                                        ? current->text().trimmed()
                                        : current->data(kCameraUrlRole).toString().trimmed();
                m_streamInput->setText(url);
                m_enabledInput->setChecked(current->data(kCameraEnabledRole).toBool());
            });

    vbox->addWidget(foldButton);
    vbox->addWidget(formContainer);
    vbox->addWidget(m_streamList, 1);

    panel->setLayout(vbox);
    m_streamDock->setWidget(panel);
    addDockWidget(Qt::LeftDockWidgetArea, m_streamDock);

    loadCameraListFromDB();
}

void MainWindow::addLayoutPresetAction(QMenu* menu, const QString& text, int rows, int cols) {
    auto* act = menu->addAction(text);
    connect(act, &QAction::triggered, this, [this, rows, cols]() {
        applyGridLayout(rows, cols);
        updateStatusBar();
    });
}

void MainWindow::clearGridItems() {
    while (QLayoutItem* item = m_grid->takeAt(0)) {
        // 레이아웃 아이템만 분리하고 위젯 소유권은 유지한다.
        delete item;
    }
}

void MainWindow::applyGridLayout(int rows, int cols) {
    m_visibleRows = qBound(1, rows, MAX_ROWS);
    m_visibleCols = qBound(1, cols, MAX_COLS);
    m_layoutName = QString("%1x%2").arg(m_visibleRows).arg(m_visibleCols);
    m_visibleSlotCount = m_visibleRows * m_visibleCols;

    clearGridItems();

    for (int i = 0; i < TOTAL; ++i) {
        auto* cell = m_cells[i];
        if (i < m_visibleSlotCount) {
            cell->show();
            m_grid->addWidget(cell, i / m_visibleCols, i % m_visibleCols);
        } else {
            cell->hide();
        }
    }

    for (int r = 0; r < MAX_ROWS; ++r)
        m_grid->setRowStretch(r, r < m_visibleRows ? 1 : 0);
    for (int c = 0; c < MAX_COLS; ++c)
        m_grid->setColumnStretch(c, c < m_visibleCols ? 1 : 0);
}

void MainWindow::applyFocusRingLayout() {
    // 4x4에서 중앙 2x2를 메인으로 쓰고, 주변 12칸에 소형 영상을 배치한다.
    m_visibleRows = 4;
    m_visibleCols = 4;
    m_visibleSlotCount = 13;
    m_layoutName = "메인+주변";

    clearGridItems();
    for (auto* cell : m_cells) cell->hide();

    for (int r = 0; r < MAX_ROWS; ++r) m_grid->setRowStretch(r, 1);
    for (int c = 0; c < MAX_COLS; ++c) m_grid->setColumnStretch(c, 1);

    m_cells[0]->show();
    m_grid->addWidget(m_cells[0], 1, 1, 2, 2);

    int id = 1;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (r >= 1 && r <= 2 && c >= 1 && c <= 2) continue;
            m_cells[id]->show();
            m_grid->addWidget(m_cells[id], r, c);
            ++id;
        }
    }

    // span 경계의 1px 겹침 상황에서도 중앙 셀 외곽선이 가려지지 않게 상단으로 올린다.
    m_cells[0]->raise();
}

void MainWindow::applyTriplePlusQuadLayout() {
    // 4x4를 2x2 사분면으로 나눠 3개는 대형, 우하단은 2x2 소형 4개를 배치한다.
    m_visibleRows = 4;
    m_visibleCols = 4;
    m_visibleSlotCount = 7;
    m_layoutName = "대형3+소형4";

    clearGridItems();
    for (auto* cell : m_cells) cell->hide();

    for (int r = 0; r < MAX_ROWS; ++r) m_grid->setRowStretch(r, 1);
    for (int c = 0; c < MAX_COLS; ++c) m_grid->setColumnStretch(c, 1);

    m_cells[0]->show();
    m_grid->addWidget(m_cells[0], 0, 0, 2, 2);
    m_cells[1]->show();
    m_grid->addWidget(m_cells[1], 0, 2, 2, 2);
    m_cells[2]->show();
    m_grid->addWidget(m_cells[2], 2, 0, 2, 2);

    m_cells[3]->show();
    m_grid->addWidget(m_cells[3], 2, 2);
    m_cells[4]->show();
    m_grid->addWidget(m_cells[4], 2, 3);
    m_cells[5]->show();
    m_grid->addWidget(m_cells[5], 3, 2);
    m_cells[6]->show();
    m_grid->addWidget(m_cells[6], 3, 3);
}

// ── 슬롯 ─────────────────────────────────────────────────────

void MainWindow::onAddRequested(int cellId, const QString& url) {
    addStream(cellId, url);
}

void MainWindow::onMoveRequested(int fromCellId, int toCellId) {
    if (fromCellId < 0 || fromCellId >= TOTAL || toCellId < 0 || toCellId >= TOTAL)
        return;
    if (fromCellId == toCellId)
        return;

    QString url;
    if (m_workers.contains(fromCellId)) {
        url = m_workers[fromCellId]->url();
    } else {
        url = m_cells[fromCellId]->streamUrl();
    }

    if (url.trimmed().isEmpty())
        return;

    if (m_workers.contains(toCellId))
        removeStream(toCellId);
    if (m_workers.contains(fromCellId))
        removeStream(fromCellId);

    addStream(toCellId, url);
}

void MainWindow::onRemoveRequested(int cellId) {
    removeStream(cellId);
}

void MainWindow::onFrameReady(int cellId) {
    if (cellId < 0 || cellId >= TOTAL) return;
    if (!m_workers.contains(cellId)) return;
    if (sender() != m_workers.value(cellId)) return;

    cv::cuda::GpuMat gpuFrame;
    if (m_workers[cellId]->takeLatestGpuFrame(gpuFrame)) {
        m_cells[cellId]->updateGpuFrame(gpuFrame);
        return;
    }

    QImage frame;
    if (m_workers[cellId]->takeLatestFrame(frame))
        m_cells[cellId]->updateFrame(frame);
}

void MainWindow::onStatusChanged(int cellId, const QString& status) {
    if (cellId < 0 || cellId >= TOTAL) return;
    if (!m_workers.contains(cellId)) return;
    if (sender() != m_workers.value(cellId)) return;
    m_cells[cellId]->setStatus(status);
}

void MainWindow::addStreamAddress() {
    if (!m_cameraIdInput || !m_nameInput || !m_streamTypeInput || !m_streamInput || !m_enabledInput || !m_streamList)
        return;

    camera_info camera{};
    camera.camera_id = m_cameraIdInput->value();
    camera.name = m_nameInput->text().trimmed().toStdString();
    camera.stream_type = static_cast<StreamType>(m_streamTypeInput->currentData().toInt());
    camera.url = m_streamInput->text().trimmed().toStdString();
    camera.enabled = m_enabledInput->isChecked();

    if (QString::fromStdString(camera.url).trimmed().isEmpty()) {
        QMessageBox::warning(this, "입력 오류", "URL은 비어 있을 수 없습니다.");
        return;
    }
    if (QString::fromStdString(camera.name).trimmed().isEmpty()) {
        camera.name = camera.url;
    }

    if (!saveCameraToDB(camera)) {
        return;
    }

    addCameraListItem(camera);
    m_nextCameraId = qMax(m_nextCameraId, camera.camera_id + 1);
    resetInlineCameraForm(false);
}

void MainWindow::updateSelectedStreamAddress()
{
    if (!m_streamList || !m_cameraIdInput || !m_nameInput || !m_streamTypeInput || !m_streamInput || !m_enabledInput)
        return;

    auto* item = m_streamList->currentItem();
    if (!item) return;

    const int oldCameraId = item->data(kCameraIdRole).toInt();
    if (oldCameraId <= 0) {
        QMessageBox::warning(this, "오류", "선택 항목의 camera_id가 올바르지 않습니다.");
        return;
    }

    camera_info camera{};
    camera.camera_id = m_cameraIdInput->value();
    camera.name = m_nameInput->text().trimmed().toStdString();
    camera.stream_type = static_cast<StreamType>(m_streamTypeInput->currentData().toInt());
    camera.url = m_streamInput->text().trimmed().toStdString();
    camera.enabled = m_enabledInput->isChecked();

    if (QString::fromStdString(camera.url).trimmed().isEmpty()) {
        QMessageBox::warning(this, "입력 오류", "URL은 비어 있을 수 없습니다.");
        return;
    }
    if (QString::fromStdString(camera.name).trimmed().isEmpty()) {
        camera.name = camera.url;
    }

    if (camera.camera_id == oldCameraId) {
        auto updateResult = DBManager::instance().updateCameraByCameraId(camera);
        if (!updateResult) {
            QMessageBox::warning(this, "DB 오류", "카메라 수정 실패:\n" + updateResult.error());
            return;
        }
    } else {
        const auto existing = DBManager::instance().getCameraByCameraId(camera.camera_id);
        if (existing) {
            QMessageBox::warning(this, "중복", "변경하려는 camera_id가 이미 존재합니다.");
            return;
        }

        auto addResult = DBManager::instance().addCamera(camera);
        if (!addResult) {
            QMessageBox::warning(this, "DB 오류", "camera_id 변경용 신규 저장 실패:\n" + addResult.error());
            return;
        }

        auto deleteResult = DBManager::instance().deleteCameraByCameraId(oldCameraId);
        if (!deleteResult) {
            static_cast<void>(DBManager::instance().deleteCameraByCameraId(camera.camera_id));
            QMessageBox::warning(this, "DB 오류", "기존 camera_id 삭제 실패로 변경을 취소했습니다:\n" + deleteResult.error());
            return;
        }
    }

    item->setText(makeCameraDisplayText(camera));
    item->setData(kCameraIdRole, camera.camera_id);
    item->setData(kCameraNameRole, QString::fromStdString(camera.name));
    item->setData(kCameraStreamTypeRole, static_cast<int>(camera.stream_type));
    item->setData(kCameraEnabledRole, camera.enabled);
    item->setData(kCameraUrlRole, QString::fromStdString(camera.url));
    item->setToolTip(makeCameraTooltip(camera));
    m_nextCameraId = qMax(m_nextCameraId, camera.camera_id + 1);
    m_streamInput->setText(QString::fromStdString(camera.url));
}

void MainWindow::removeSelectedStreamAddress() {
    if (!m_streamList) return;

    const int row = m_streamList->currentRow();
    if (row < 0) return;

    auto* item = m_streamList->item(row);
    const int cameraId = item ? item->data(kCameraIdRole).toInt() : 0;
    if (cameraId > 0) {
        auto deleteResult = DBManager::instance().deleteCameraByCameraId(cameraId);
        if (!deleteResult) {
            QMessageBox::warning(this, "DB 오류", "카메라 삭제 실패:\n" + deleteResult.error());
            return;
        }
    }

    delete m_streamList->takeItem(row);
    resetInlineCameraForm(true);
}

void MainWindow::onStreamListDoubleClicked(QListWidgetItem* item) {
    if (!item) return;
    const QString url = item->data(kCameraUrlRole).toString().trimmed().isEmpty()
                            ? item->text().trimmed()
                            : item->data(kCameraUrlRole).toString().trimmed();
    if (url.isEmpty()) return;

    // 표시 중인 슬롯 중 첫 번째 빈 칸에 스트림을 시작한다.
    for (int i = 0; i < m_visibleSlotCount; ++i) {
        if (!m_cells[i]->isActive()) {
            addStream(i, url);
            return;
        }
    }

    // 빈 칸이 없으면 아무것도 하지 않는다.
}

void MainWindow::loadUrlsFromFile() {
    const QString path = QFileDialog::getOpenFileName(
        this, "URL 목록 파일 선택", {},
        "가져오기 파일 (*.txt *.csv);;텍스트 파일 (*.txt);;CSV 파일 (*.csv);;모든 파일 (*)");
    if (path.isEmpty()) return;

    const bool isCsvFile = QFileInfo(path).suffix().compare("csv", Qt::CaseInsensitive) == 0;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "오류", "파일을 열 수 없습니다:\n" + path);
        return;
    }

    // 옵션 다이얼로그: 체크박스만 추가하면 쉽게 확장할 수 있다.
    QDialog optionDialog(this);
    optionDialog.setWindowTitle("불러오기 옵션");
    optionDialog.setModal(true);

    auto* optionLayout = new QVBoxLayout(&optionDialog);
    optionLayout->setContentsMargins(12, 12, 12, 12);
    optionLayout->setSpacing(8);

    auto* removeDuplicatesCheck = new QCheckBox("기존 목록과 중복되는 URL 제거", &optionDialog);
    removeDuplicatesCheck->setChecked(false);
    optionLayout->addWidget(removeDuplicatesCheck);

    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &optionDialog);
    connect(buttonBox, &QDialogButtonBox::accepted, &optionDialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &optionDialog, &QDialog::reject);
    optionLayout->addWidget(buttonBox);

    if (optionDialog.exec() != QDialog::Accepted) {
        return;
    }

    const bool removeDuplicates = removeDuplicatesCheck->isChecked();

    QTextStream in(&f);
    int lineNo = 0;
    int addedCount = 0;
    int skippedCount = 0;
    int invalidCount = 0;

    while (!in.atEnd()) {
        ++lineNo;
        QString line = in.readLine();
        if (lineNo == 1 && !line.isEmpty() && line[0] == QChar::ByteOrderMark) {
            line.remove(0, 1);
        }
        line = line.trimmed();

        if (line.isEmpty() || line.startsWith('#'))
            continue;

        camera_info camera{};
        if (isCsvFile) {
            const QStringList cols = line.split(',', Qt::KeepEmptyParts);
            if (isCameraCsvHeader(cols)) {
                continue;
            }
            if (!parseCameraCsvLine(line, camera)) {
                ++invalidCount;
                continue;
            }
        } else {
            camera.camera_id = m_nextCameraId;
            camera.name = line.toStdString();
            camera.stream_type = StreamType::RTSP;
            camera.url = line.toStdString();
            camera.enabled = true;
        }

        const QString cameraUrl = QString::fromStdString(camera.url);
        if (removeDuplicates && listHasUrl(m_streamList, cameraUrl)) {
            ++skippedCount;
            continue;
        }

        if (listHasCameraId(m_streamList, camera.camera_id)) {
            ++skippedCount;
            continue;
        }

        if (!saveCameraToDB(camera)) {
            ++invalidCount;
            continue;
        }

        addCameraListItem(camera);
        m_nextCameraId = qMax(m_nextCameraId, camera.camera_id + 1);
        ++addedCount;
    }

    if (m_cameraIdInput && !m_streamList->currentItem()) {
        m_cameraIdInput->setValue(m_nextCameraId);
    }

    QMessageBox::information(
        this,
        "가져오기 결과",
        QString("추가: %1건\n중복/충돌로 건너뜀: %2건\n형식 오류/저장 실패: %3건")
            .arg(addedCount)
            .arg(skippedCount)
            .arg(invalidCount));
}

void MainWindow::removeAllStreams() {
    const auto ids = m_workers.keys();
    for (int id : ids) removeStream(id);
}

// ── 스트림 추가/제거 ─────────────────────────────────────────

void MainWindow::addStream(int cellId, const QString& url) {
    // 기존 스트림이 있으면 먼저 제거
    if (m_workers.contains(cellId))
        removeStream(cellId);

    m_cells[cellId]->activate(url);

    auto* worker = new StreamWorker(cellId, url, this);
    connect(worker, &StreamWorker::frameReady,
            this,   &MainWindow::onFrameReady,    Qt::QueuedConnection);
    connect(worker, &StreamWorker::statusChanged,
            this,   &MainWindow::onStatusChanged, Qt::QueuedConnection);

    m_workers[cellId] = worker;
    worker->start();
    updateStatusBar();
}

void MainWindow::removeStream(int cellId) {
    if (!m_workers.contains(cellId)) return;

    auto* worker = m_workers.take(cellId);
    disconnect(worker, nullptr, this, nullptr);
    worker->stop();

    // 실행 중이면 종료 후 GUI 스레드에서 안전하게 삭제한다.
    if (worker->isRunning()) {
        connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    } else {
        delete worker;
    }

    m_cells[cellId]->deactivate();
    updateStatusBar();
}

void MainWindow::updateStatusBar() {
    const int active = static_cast<int>(m_workers.size());
    setWindowTitle(QString("RTSP 멀티채널 뷰어  ─  %1  |  활성 %2 / %3")
                       .arg(m_layoutName)
                       .arg(active).arg(TOTAL));
    m_statusLabel->setText(
        QString("레이아웃: %1 (표시 %2칸)  |  활성 스트림: %3 / %4")
            .arg(m_layoutName)
            .arg(m_visibleSlotCount)
                       .arg(active).arg(TOTAL));
}

void MainWindow::loadCameraListFromDB()
{
    if (!m_streamList) return;

    auto listResult = DBManager::instance().listCameras();
    if (!listResult) {
        QMessageBox::warning(this, "DB 오류", "카메라 목록 로드 실패:\n" + listResult.error());
        return;
    }

    int maxCameraId = 0;
    for (const auto& camera : listResult.value()) {
        addCameraListItem(camera);
        if (camera.camera_id > maxCameraId) {
            maxCameraId = camera.camera_id;
        }
    }

    m_nextCameraId = maxCameraId + 1;
    if (m_nextCameraId <= 0) {
        m_nextCameraId = 1;
    }

    if (m_cameraIdInput && !m_streamList->currentItem()) {
        m_cameraIdInput->setValue(m_nextCameraId);
    }
}

bool MainWindow::saveCameraToDB(const camera_info& camera)
{
    auto addResult = DBManager::instance().addCamera(camera);
    if (!addResult) {
        QMessageBox::warning(this, "DB 오류", "카메라 저장 실패:\n" + addResult.error());
        return false;
    }

    return true;
}

void MainWindow::addCameraListItem(const camera_info& camera)
{
    if (!m_streamList) return;

    const QString url = QString::fromStdString(camera.url);
    auto* item = new QListWidgetItem(makeCameraDisplayText(camera), m_streamList);
    item->setData(kCameraIdRole, camera.camera_id);
    item->setData(kCameraNameRole, QString::fromStdString(camera.name));
    item->setData(kCameraStreamTypeRole, static_cast<int>(camera.stream_type));
    item->setData(kCameraEnabledRole, camera.enabled);
    item->setData(kCameraUrlRole, url);
    item->setToolTip(makeCameraTooltip(camera));
}

void MainWindow::resetInlineCameraForm(bool clearSelection)
{
    if (clearSelection && m_streamList) {
        const QSignalBlocker blocker(m_streamList);
        m_streamList->setCurrentItem(nullptr);
    }

    if (m_cameraIdInput) {
        m_cameraIdInput->setValue(m_nextCameraId);
    }
    if (m_nameInput) {
        m_nameInput->clear();
    }
    if (m_streamTypeInput) {
        m_streamTypeInput->setCurrentIndex(0);
    }
    if (m_streamInput) {
        m_streamInput->clear();
    }
    if (m_enabledInput) {
        m_enabledInput->setChecked(true);
    }
}

