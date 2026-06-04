#include "mainwindow.h"

#include "DBManager.h"
#include "vodpanel.h"

#include <QWidget>
#include <QGridLayout>
#include <QStackedWidget>
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
#include <QBrush>
#include <QColor>
#include <QCloseEvent>
#include <QSystemTrayIcon>
#include <QApplication>
#include <QStyle>
#include <QTimer>
#include <QMetaObject>
#include <opencv2/core/cuda.hpp>

#include <chrono>
#include <utility>

namespace {

constexpr int kCameraIdRole = Qt::UserRole + 1;
constexpr int kCameraNameRole = Qt::UserRole + 2;
constexpr int kCameraStreamTypeRole = Qt::UserRole + 3;
constexpr int kCameraEnabledRole = Qt::UserRole + 4;
constexpr int kCameraUrlRole = Qt::UserRole + 5;
constexpr int kCameraRecordEnabledRole = Qt::UserRole + 6;
constexpr int kCameraAnalysisEnabledRole = Qt::UserRole + 7;
constexpr const char* kCameraFormExpandedSetting = "ui/streamList/cameraFormExpanded";
constexpr const char* kRecordAutoDecodeSetting = "stream/recordAutoDecodeEnabled";
constexpr size_t kMaxInferenceQueueDepth = 16;
constexpr const char* kCameraIdMime = "application/x-edgeon-camera-id";
constexpr const char* kRecordOutputRoot = "E:/EdgeOn";
constexpr int kRecordSegmentSeconds = 30;
constexpr const char* kRecordSegmentSecondsSetting = "record/segmentSeconds";
constexpr const char* kYoloModelPath = "models/yolov8s.onnx";
constexpr float kInferenceConfidenceThreshold = 0.25f;
constexpr float kInferenceIouThreshold = 0.45f;
constexpr int kYoloModelInputWidth = 640;
constexpr int kYoloModelInputHeight = 640;

int effectiveRecordSegmentSeconds() {
    QSettings settings;
    return qMax(1, settings.value(kRecordSegmentSecondsSetting, kRecordSegmentSeconds).toInt());
}

QString cameraAnalysisSettingKey(const int cameraId) {
    return QString("%1/%2").arg(kRecordAutoDecodeSetting).arg(cameraId);
}

void syncRecordButtonText(QPushButton* button, bool enabled) {
    if (!button) return;
    button->setText(enabled ? "REC ON" : "REC OFF");
}

QString yoloModelName()
{
    return QFileInfo(QString::fromUtf8(kYoloModelPath)).baseName();
}

std::vector<detection_result> makeDetectionRows(const detection_frame_info& frameInfo,
                                                const std::vector<Yolov8Detection>& detections)
{
    std::vector<detection_result> rows;
    rows.reserve(detections.size());

    const qint64 storedUtcMs = QDateTime::currentMSecsSinceEpoch();
    for (size_t index = 0; index < detections.size(); ++index) {
        const auto& det = detections[index];
        detection_result row;
        row.camera_id = frameInfo.camera_id;
        row.frame_utc_ms = frameInfo.frame_utc_ms;
        row.frame_seq = frameInfo.frame_seq;
        row.det_index = static_cast<int>(index);
        row.stored_utc_ms = storedUtcMs;
        row.capture_utc_ms = frameInfo.capture_utc_ms > 0 ? frameInfo.capture_utc_ms : frameInfo.frame_utc_ms;
        row.source_pts = frameInfo.source_pts;
        row.source_time_base_num = frameInfo.source_time_base_num;
        row.source_time_base_den = frameInfo.source_time_base_den;
        row.frame_width = frameInfo.frame_width;
        row.frame_height = frameInfo.frame_height;
        row.stream_url = frameInfo.stream_url;
        row.model_name = frameInfo.model_name;
        row.model_provider = frameInfo.model_provider;
        row.model_input_width = frameInfo.model_input_width;
        row.model_input_height = frameInfo.model_input_height;
        row.confidence_threshold = frameInfo.confidence_threshold;
        row.iou_threshold = frameInfo.iou_threshold;
        row.inference_ms = frameInfo.inference_ms;
        row.class_id = det.classId;
        row.score = det.score;
        row.box_x = det.box.x;
        row.box_y = det.box.y;
        row.box_width = det.box.width;
        row.box_height = det.box.height;
        row.segment_relative_ms = frameInfo.segment_relative_ms;
        row.record_segment_start_utc_ms = frameInfo.record_segment_start_utc_ms;
        row.record_segment_end_utc_ms = frameInfo.record_segment_end_utc_ms;
        row.record_segment_start_source_pts = frameInfo.record_segment_start_source_pts;
        row.record_segment_source_time_base_num = frameInfo.record_segment_source_time_base_num;
        row.record_segment_source_time_base_den = frameInfo.record_segment_source_time_base_den;
        row.record_segment_file_path = frameInfo.record_segment_file_path;
        rows.push_back(std::move(row));
    }

    return rows;
}

QString streamTypeToText(StreamType type) {
    switch (type) {
    case StreamType::RTSP: return "RTSP";
    case StreamType::ONVIF: return "ONVIF";
    }
    return "Unknown";
}

QString makeCameraTooltip(const camera_info& camera) {
    return QString("camera_id: %1\nname: %2\nstream_type: %3\nenabled: %4\nrecord_enabled: %5\nurl: %6")
        .arg(camera.camera_id)
        .arg(QString::fromStdString(camera.name))
        .arg(streamTypeToText(camera.stream_type))
        .arg(camera.enabled ? "true" : "false")
        .arg(camera.record_enabled ? "true" : "false")
        .arg(QString::fromStdString(camera.url));
}

QString makeCameraDisplayText(const camera_info& camera) {
    const QString name = QString::fromStdString(camera.name).trimmed();
    const QString url = QString::fromStdString(camera.url).trimmed();
    const QString recBadge = camera.record_enabled ? " [REC]" : "";
    if (name.isEmpty() || name == url) {
        return url + recBadge;
    }
    return QString("%1 (%2)%3").arg(name, url, recBadge);
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

    bool recordEnabled = false;
    if (cols.size() >= 6 && !cols[5].trimmed().isEmpty()) {
        if (!parseEnabledValue(cols[5], recordEnabled)) {
            return false;
        }
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
    camera.record_enabled = recordEnabled;
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
        const int cameraId = item->data(kCameraIdRole).toInt();
        if (cameraId > 0) {
            mime->setData(kCameraIdMime, QByteArray::number(cameraId));
        }
        auto* drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->exec(supportedActions, Qt::CopyAction);
    }
};

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    buildUi();
    setWindowTitle("RTSP 멀티채널 뷰어");
    resize(1600, 900);
    setupSystemTray();
}

MainWindow::~MainWindow() {
    shutdownAllSessions();
    stopInferenceWorker();
    if (m_trayIcon) {
        m_trayIcon->hide();
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (m_allowClose || !m_trayEnabled || !m_trayIcon || !m_trayIcon->isVisible()) {
        QMainWindow::closeEvent(event);
        return;
    }

    event->ignore();
    hide();

    if (!m_trayNoticeShown) {
        m_trayNoticeShown = true;
        m_trayIcon->showMessage(
            "RTSP 멀티채널 뷰어",
            "프로그램이 시스템 트레이로 최소화되었습니다. 종료는 트레이 메뉴의 '종료'를 사용하세요.",
            QSystemTrayIcon::Information,
            3000);
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
        connect(cell, &VideoCell::addRequestedByCamera, this, &MainWindow::onAddRequestedByCamera);
        connect(cell, &VideoCell::moveRequested,   this, &MainWindow::onMoveRequested);
        connect(cell, &VideoCell::removeRequested, this, &MainWindow::onRemoveRequested);
        m_cells[i] = cell;
    }
    setCentralWidget(m_central);

    // ── VOD 패널 + 스택 위젯 ────────────────────────────────
    m_vodPanel    = new VodPanel(QString::fromUtf8(kRecordOutputRoot), this);
    m_stackedMain = new QStackedWidget(this);
    m_stackedMain->addWidget(m_central);   // index 0 : 지능형 관제
    m_stackedMain->addWidget(m_vodPanel);  // index 1 : 저장영상 확인
    setCentralWidget(m_stackedMain);

    // ── 툴바 ─────────────────────────────────────────────────
    auto* tb = addToolBar("도구");
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    // ── 모드 전환 버튼 ────────────────────────────────────────
    const QString modeButtonStyle = R"(
        QPushButton {
            font-size: 14px;
            font-weight: bold;
            padding: 6px 18px;
            border: 2px solid #555;
            border-radius: 6px;
            background-color: #3c3c3c;
            color: #cccccc;
        }
        QPushButton:checked {
            background-color: #0078d4;
            color: white;
            border-color: #005fa3;
        }
        QPushButton:hover:!checked {
            background-color: #505050;
        }
    )";

    auto* btnLive = new QPushButton("🎥  지능형 관제", tb);
    auto* btnAI   = new QPushButton("🤖  이벤트 관제", tb);
    auto* btnVod  = new QPushButton("🗂  저장영상 확인", tb);

    for (auto* btn : {btnLive, btnAI, btnVod}) {
        btn->setCheckable(true);
        btn->setStyleSheet(modeButtonStyle);
        btn->setMinimumHeight(36);
    }
    btnLive->setChecked(true);   // 기본 모드: 지능형 관제

    auto switchMode = [btnLive, btnAI, btnVod](QPushButton* active) {
        for (auto* btn : {btnLive, btnAI, btnVod}) {
            btn->setChecked(btn == active);
        }
    };
    connect(btnLive, &QPushButton::clicked, this, [this, switchMode, btnLive]() {
        switchMode(btnLive);
        if (m_stackedMain) m_stackedMain->setCurrentIndex(0);
    });
    connect(btnAI,   &QPushButton::clicked, this, [switchMode, btnAI]() {
        switchMode(btnAI);
    });
    connect(btnVod,  &QPushButton::clicked, this, [this, switchMode, btnVod]() {
        switchMode(btnVod);
        if (m_stackedMain) {
            m_stackedMain->setCurrentIndex(1);
            if (m_vodPanel) m_vodPanel->setFocus();
        }
    });

    tb->addWidget(btnLive);
    tb->addWidget(btnAI);
    tb->addWidget(btnVod);

    tb->addSeparator();

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

    // ── 스트림 목록 → VOD 연결 (VOD 모드일 때 클릭 시 해당 카메라 로드) ──
    connect(m_streamList, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* current, QListWidgetItem*) {
                if (!m_stackedMain || m_stackedMain->currentIndex() != 1) return;
                if (!current || !m_vodPanel) return;
                const int cameraId = current->data(kCameraIdRole).toInt();
                if (cameraId > 0) {
                    const int segSecs = effectiveRecordSegmentSeconds();
                    m_vodPanel->loadCamera(cameraId, segSecs);
                }
            });

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

    m_recordEnabledButton = new QPushButton(panel);
    m_recordEnabledButton->setCheckable(true);
    m_recordEnabledButton->setChecked(false);
    syncRecordButtonText(m_recordEnabledButton, false);

    m_recordAutoDecodeInput = new QCheckBox("영상분석 활성화 (선택 카메라)", panel);
    {
        QSettings settings;
        m_recordAutoDecodeEnabled = settings.value(kRecordAutoDecodeSetting, true).toBool();
    }
    m_recordAutoDecodeInput->setChecked(m_recordAutoDecodeEnabled);

    auto* form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(4);
    form->addRow("camera_id", m_cameraIdInput);
    form->addRow("이름", m_nameInput);
    form->addRow("스트림 타입", m_streamTypeInput);
    form->addRow("URL", m_streamInput);
    form->addRow("상태", m_enabledInput);
    form->addRow("녹화", m_recordEnabledButton);
    form->addRow("동작", m_recordAutoDecodeInput);

    connect(m_recordEnabledButton, &QPushButton::toggled, this, [this](bool checked) {
        syncRecordButtonText(m_recordEnabledButton, checked);
        if (!m_streamList) return;

        auto* item = m_streamList->currentItem();
        if (!item) return;

        const int cameraId = item->data(kCameraIdRole).toInt();
        if (cameraId <= 0) return;

        const auto cameraResult = DBManager::instance().getCameraByCameraId(cameraId);
        if (!cameraResult) {
            QMessageBox::warning(this, "DB 오류", "카메라 조회 실패:\n" + cameraResult.error());
            return;
        }

        camera_info camera = cameraResult.value();
        camera.record_enabled = checked;
        const auto updateResult = DBManager::instance().updateCameraByCameraId(camera);
        if (!updateResult) {
            QMessageBox::warning(this, "DB 오류", "녹화 상태 저장 실패:\n" + updateResult.error());
            return;
        }

        item->setText(makeCameraDisplayText(camera));
        item->setData(kCameraRecordEnabledRole, checked);
        item->setToolTip(makeCameraTooltip(camera));
        updateCameraItemVisualState(item);
        setCameraRecordingRequested(cameraId, checked);
    });

    connect(m_recordAutoDecodeInput, &QCheckBox::toggled, this, [this](bool checked) {
        if (!m_streamList || m_streamListItemSyncing) {
            return;
        }

        auto* item = m_streamList->currentItem();
        if (!item) {
            return;
        }

        const int cameraId = item->data(kCameraIdRole).toInt();
        if (cameraId <= 0) {
            return;
        }

        m_streamListItemSyncing = true;
        item->setData(kCameraAnalysisEnabledRole, checked);
        item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        m_streamListItemSyncing = false;

        saveCameraAnalysisEnabled(cameraId, checked);
        setCameraAnalysisEnabled(cameraId, checked);
    });

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
    connect(m_streamList, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (!item || m_streamListItemSyncing) {
            return;
        }

        const int cameraId = item->data(kCameraIdRole).toInt();
        if (cameraId <= 0) {
            return;
        }

        const bool checked = item->checkState() == Qt::Checked;
        item->setData(kCameraAnalysisEnabledRole, checked);
        saveCameraAnalysisEnabled(cameraId, checked);
        setCameraAnalysisEnabled(cameraId, checked);

        if (item == m_streamList->currentItem() && m_recordAutoDecodeInput) {
            const QSignalBlocker blocker(m_recordAutoDecodeInput);
            m_recordAutoDecodeInput->setChecked(checked);
        }
    });

    // 선택한 항목을 인라인 입력 폼으로 가져와 수정 흐름을 단순화
    connect(m_streamList, &QListWidget::currentItemChanged,
            this, [this](QListWidgetItem* current, QListWidgetItem*) {
                if (!m_cameraIdInput || !m_nameInput || !m_streamTypeInput || !m_streamInput || !m_enabledInput || !m_recordEnabledButton)
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
                const bool recordEnabled = current->data(kCameraRecordEnabledRole).toBool();
                const bool analysisEnabled = current->data(kCameraAnalysisEnabledRole).toBool();
                {
                    const QSignalBlocker blocker(m_recordEnabledButton);
                    m_recordEnabledButton->setChecked(recordEnabled);
                }
                syncRecordButtonText(m_recordEnabledButton, recordEnabled);
                if (m_recordAutoDecodeInput) {
                    const QSignalBlocker blocker(m_recordAutoDecodeInput);
                    m_recordAutoDecodeInput->setChecked(analysisEnabled);
                }
            });

    vbox->addWidget(foldButton);
    vbox->addWidget(formContainer);
    vbox->addWidget(m_streamList, 1);

    panel->setLayout(vbox);
    m_streamDock->setWidget(panel);
    addDockWidget(Qt::LeftDockWidgetArea, m_streamDock);

    loadCameraListFromDB();
}

void MainWindow::setupSystemTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        m_trayEnabled = false;
        QMessageBox::warning(
            this,
            "시스템 트레이 미지원",
            "현재 환경에서 시스템 트레이를 사용할 수 없습니다.\n"
            "창 닫기(X)는 프로그램 종료로 동작합니다.");
        return;
    }

    m_trayIcon = new QSystemTrayIcon(this);
    QIcon icon = QApplication::windowIcon();
    if (icon.isNull()) {
        icon = windowIcon();
    }
    if (icon.isNull()) {
        icon = style()->standardIcon(QStyle::SP_ComputerIcon);
    }
    if (!icon.isNull()) {
        setWindowIcon(icon);
    }
    m_trayIcon->setIcon(icon);
    m_trayIcon->setToolTip("RTSP 멀티채널 뷰어");

    auto* trayMenu = new QMenu(this);
    m_showAction = trayMenu->addAction("열기");
    m_quitAction = trayMenu->addAction("종료");

    connect(m_showAction, &QAction::triggered, this, &MainWindow::showFromTray);
    connect(m_quitAction, &QAction::triggered, this, &MainWindow::quitFromTray);
    connect(m_trayIcon, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger ||
                    reason == QSystemTrayIcon::DoubleClick) {
                    showFromTray();
                }
            });

    m_trayIcon->setContextMenu(trayMenu);
    m_trayIcon->show();
    m_trayEnabled = m_trayIcon->isVisible();

    if (!m_trayEnabled) {
        QTimer::singleShot(300, this, [this]() {
            if (!m_trayIcon || m_trayIcon->isVisible()) {
                m_trayEnabled = m_trayIcon && m_trayIcon->isVisible();
                return;
            }

            m_trayIcon->show();
            m_trayEnabled = m_trayIcon->isVisible();
            if (!m_trayEnabled) {
                QMessageBox::warning(
                    this,
                    "시스템 트레이 표시 실패",
                    "시스템 트레이 아이콘을 표시하지 못했습니다.\n"
                    "창 닫기(X)는 프로그램 종료로 동작합니다.");
            }
        });
    }
}

void MainWindow::showFromTray()
{
    showNormal();
    raise();
    activateWindow();
}

void MainWindow::quitFromTray()
{
    if (m_isShuttingDown) {
        return;
    }

    m_isShuttingDown = true;
    m_allowClose = true;
    if (m_trayIcon) {
        m_trayIcon->hide();
    }
    m_trayEnabled = false;

    shutdownAllSessions();
    QCoreApplication::quit();
}

void MainWindow::shutdownAllSessions()
{
    // 종료 경로에서는 모든 워커가 완전히 끝날 때까지 기다려 QThread 파괴 경고를 방지한다.
    const auto cameraIds = m_sessions.keys();
    for (const int cameraId : cameraIds) {
        if (auto* session = findSession(cameraId)) {
            stopAndDeleteSession(*session);
        }
    }
    m_sessions.clear();
    m_cellToCamera.clear();
    stopInferenceWorker();
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

void MainWindow::onAddRequestedByCamera(int cellId, int cameraId, const QString& url) {
    addStreamByCamera(cellId, cameraId, url);
}

void MainWindow::onMoveRequested(int fromCellId, int toCellId) {
    if (fromCellId < 0 || fromCellId >= TOTAL || toCellId < 0 || toCellId >= TOTAL)
        return;
    if (fromCellId == toCellId)
        return;

    const int sourceCameraId = m_cellToCamera.value(fromCellId, 0);
    QString url = m_cells[fromCellId]->streamUrl();
    if (sourceCameraId != 0) {
        if (const auto* session = findSession(sourceCameraId)) {
            if (!session->url.trimmed().isEmpty()) {
                url = session->url;
            }
        }
    }

    if (url.trimmed().isEmpty())
        return;

    if (m_cellToCamera.contains(toCellId)) removeStream(toCellId);
    if (m_cellToCamera.contains(fromCellId)) removeStream(fromCellId);

    addStreamByCamera(toCellId, sourceCameraId, url);
}

void MainWindow::onRemoveRequested(int cellId) {
    removeStream(cellId);
}

void MainWindow::onFrameReady(int cameraId) {
    auto* session = findSession(cameraId);
    if (!session || !session->worker) return;
    if (sender() != session->worker) return;

    cv::cuda::GpuMat gpuFrame;
    qint64 frameUtcMs = 0;
    qint64 frameSeq = 0;
    qint64 captureUtcMs = 0;
    qint64 sourcePts = 0;
    int sourceTimeBaseNum = 0;
    int sourceTimeBaseDen = 1;
    cv::Size frameSize;
    if (session->worker->takeLatestGpuFrame(gpuFrame,
                                            frameUtcMs,
                                            frameSeq,
                                            captureUtcMs,
                                            sourcePts,
                                            sourceTimeBaseNum,
                                            sourceTimeBaseDen,
                                            frameSize)) {
        detection_frame_info frameInfo;
        frameInfo.camera_id = cameraId;
        frameInfo.frame_utc_ms = frameUtcMs;
        frameInfo.frame_seq = frameSeq;
        frameInfo.capture_utc_ms = captureUtcMs > 0 ? captureUtcMs : frameUtcMs;
        frameInfo.source_pts = sourcePts;
        frameInfo.source_time_base_num = sourceTimeBaseNum;
        frameInfo.source_time_base_den = sourceTimeBaseDen > 0 ? sourceTimeBaseDen : 1;
        frameInfo.frame_width = frameSize.width > 0 ? frameSize.width : gpuFrame.cols;
        frameInfo.frame_height = frameSize.height > 0 ? frameSize.height : gpuFrame.rows;
        frameInfo.stream_url = session->url;
        frameInfo.model_name = yoloModelName();
        frameInfo.model_provider = "CUDA";
        frameInfo.model_input_width = kYoloModelInputWidth;
        frameInfo.model_input_height = kYoloModelInputHeight;
        frameInfo.confidence_threshold = kInferenceConfidenceThreshold;
        frameInfo.iou_threshold = kInferenceIouThreshold;
        frameInfo.record_requested = session->recordRequested;
        frameInfo.analysis_enabled = session->analysisEnabled;
        frameInfo.segment_relative_ms = 0;
        frameInfo.record_segment_start_utc_ms = session->recordSegmentStartUtcMs;
        frameInfo.record_segment_end_utc_ms = session->recordSegmentEndUtcMs;
        frameInfo.record_segment_start_source_pts = session->recordSegmentStartSourcePts;
        frameInfo.record_segment_source_time_base_num = session->recordSegmentSourceTimeBaseNum;
        frameInfo.record_segment_source_time_base_den = session->recordSegmentSourceTimeBaseDen;
        frameInfo.record_segment_file_path = session->recordSegmentFilePath;

        if (frameInfo.record_segment_source_time_base_num != 0 &&
            frameInfo.record_segment_source_time_base_den > 0) {
            const qint64 deltaPts = frameInfo.source_pts - frameInfo.record_segment_start_source_pts;
            const long double scaledMs =
                (static_cast<long double>(deltaPts) *
                 static_cast<long double>(frameInfo.record_segment_source_time_base_num) *
                 1000.0L) /
                static_cast<long double>(frameInfo.record_segment_source_time_base_den);
            frameInfo.segment_relative_ms = static_cast<qint64>(scaledMs);
        }

        for (const int cellId : session->attachedCells) {
            if (cellId >= 0 && cellId < TOTAL) {
                m_cells[cellId]->updateGpuFrame(gpuFrame);
                m_cells[cellId]->updateDetections(session->lastDetections, session->lastDetectionFrameSize);
            }
        }

        const bool shouldAnalyze = session->recordRequested && session->analysisEnabled;
        if (shouldAnalyze && !gpuFrame.empty() && !session->analysisBusy) {
            session->analysisBusy = true;
            session->worker->setAnalysisBusy(true);
            enqueueInferenceTask(cameraId, gpuFrame, frameInfo);
        }
        return;
    }

    QImage frame;
    if (session->worker->takeLatestFrame(frame,
                                         frameUtcMs,
                                         frameSeq,
                                         captureUtcMs,
                                         sourcePts,
                                         sourceTimeBaseNum,
                                         sourceTimeBaseDen,
                                         frameSize)) {
        for (const int cellId : session->attachedCells) {
            if (cellId >= 0 && cellId < TOTAL) {
                m_cells[cellId]->updateFrame(frame);
                m_cells[cellId]->updateDetections({}, cv::Size{});
            }
        }
    }
}

void MainWindow::onStatusChanged(int cameraId, const QString& status) {
    auto* session = findSession(cameraId);
    if (!session || !session->worker) return;
    if (sender() != session->worker) return;

    session->lastStatus = status;
    for (const int cellId : session->attachedCells) {
        if (cellId >= 0 && cellId < TOTAL) {
            m_cells[cellId]->setStatus(status);
        }
    }
}

void MainWindow::onRecorderSegmentStarted(int cameraId,
                                          qint64 startUtcMs,
                                          int segmentSeconds,
                                          const QString& tempPath,
                                          qint64 startSourcePts,
                                          int sourceTimeBaseNum,
                                          int sourceTimeBaseDen)
{
    auto* session = findSession(cameraId);
    if (!session) return;

    session->recordSegmentStartUtcMs = startUtcMs;
    session->recordSegmentEndUtcMs = startUtcMs + static_cast<qint64>(qMax(1, segmentSeconds)) * 1000LL;
    session->recordSegmentStartSourcePts = startSourcePts;
    session->recordSegmentSourceTimeBaseNum = sourceTimeBaseNum;
    session->recordSegmentSourceTimeBaseDen = sourceTimeBaseDen > 0 ? sourceTimeBaseDen : 1;
    session->recordSegmentFilePath = tempPath;
}

void MainWindow::onRecorderSegmentFinished(int cameraId, qint64 startUtcMs, qint64 endUtcMs, const QString& finalPath)
{
    auto* session = findSession(cameraId);
    if (!session) return;

    if (session->recordSegmentStartUtcMs == 0 || session->recordSegmentStartUtcMs == startUtcMs) {
        session->recordSegmentStartUtcMs = startUtcMs;
        session->recordSegmentEndUtcMs = endUtcMs;
        session->recordSegmentFilePath = finalPath;
    }
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
    camera.record_enabled = m_recordEnabledButton && m_recordEnabledButton->isChecked();

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
    if (!m_streamList || !m_cameraIdInput || !m_nameInput || !m_streamTypeInput || !m_streamInput || !m_enabledInput || !m_recordEnabledButton)
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
    camera.record_enabled = m_recordEnabledButton->isChecked();

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
    item->setData(kCameraRecordEnabledRole, camera.record_enabled);
    item->setData(kCameraUrlRole, QString::fromStdString(camera.url));
    const bool analysisEnabledFromSettings = loadCameraAnalysisEnabled(camera.camera_id, m_recordAutoDecodeEnabled);
    item->setData(kCameraAnalysisEnabledRole, analysisEnabledFromSettings);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(analysisEnabledFromSettings ? Qt::Checked : Qt::Unchecked);
    item->setToolTip(makeCameraTooltip(camera));
    updateCameraItemVisualState(item);

    if (camera.camera_id != oldCameraId) {
        setCameraRecordingRequested(oldCameraId, false);
        setCameraAnalysisEnabled(oldCameraId, false);
    }
    setCameraRecordingRequested(camera.camera_id, camera.record_enabled);

    const bool analysisEnabled = item->data(kCameraAnalysisEnabledRole).toBool();
    saveCameraAnalysisEnabled(camera.camera_id, analysisEnabled);
    setCameraAnalysisEnabled(camera.camera_id, analysisEnabled);

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
        setCameraRecordingRequested(cameraId, false);
        setCameraAnalysisEnabled(cameraId, false);
        QSettings settings;
        settings.remove(cameraAnalysisSettingKey(cameraId));
    }

    delete m_streamList->takeItem(row);
    resetInlineCameraForm(true);
}

void MainWindow::onStreamListDoubleClicked(QListWidgetItem* item) {
    if (!item) return;
    const int cameraId = item->data(kCameraIdRole).toInt();

    // 저장영상 확인 모드에서는 더블클릭을 VOD 카메라 로드 동작으로 사용한다.
    if (m_stackedMain && m_stackedMain->currentIndex() == 1) {
        if (cameraId > 0 && m_vodPanel) {
            m_vodPanel->loadCamera(cameraId, effectiveRecordSegmentSeconds());
            m_vodPanel->setFocus();
        }
        return;
    }

    const QString url = item->data(kCameraUrlRole).toString().trimmed().isEmpty()
                            ? item->text().trimmed()
                            : item->data(kCameraUrlRole).toString().trimmed();
    if (url.isEmpty()) return;

    // 표시 중인 슬롯 중 첫 번째 빈 칸에 스트림을 시작한다.
    for (int i = 0; i < m_visibleSlotCount; ++i) {
        if (!m_cells[i]->isActive()) {
            addStreamByCamera(i, cameraId, url);
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
            camera.record_enabled = false;
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
    const auto cellIds = m_cellToCamera.keys();
    for (const int cellId : cellIds) removeStream(cellId);
}

// ── 스트림 추가/제거 ─────────────────────────────────────────

void MainWindow::addStream(int cellId, const QString& url) {
    if (cellId < 0 || cellId >= TOTAL) return;
    const QString normalizedUrl = url.trimmed();
    if (normalizedUrl.isEmpty()) return;

    const int cameraId = resolveOrCreateCameraId(normalizedUrl);
    if (cameraId == 0) return;

    addStreamByCamera(cellId, cameraId, normalizedUrl);
}

void MainWindow::addStreamByCamera(int cellId, int cameraId, const QString& url)
{
    if (cellId < 0 || cellId >= TOTAL) return;
    if (cameraId == 0) return;
    const QString normalizedUrl = url.trimmed();
    if (normalizedUrl.isEmpty()) return;

    bindCellToCamera(cellId, cameraId, normalizedUrl);
    updateStatusBar();
}

void MainWindow::removeStream(int cellId) {
    if (cellId < 0 || cellId >= TOTAL) return;
    unbindCell(cellId);
    updateStatusBar();
}

void MainWindow::updateStatusBar() {
    const int active = static_cast<int>(m_sessions.size());
    setWindowTitle(QString("RTSP 멀티채널 뷰어  ─  %1  |  활성 %2 / %3")
                       .arg(m_layoutName)
                       .arg(active).arg(TOTAL));
    if (m_statusLabel) {
        m_statusLabel->setText(
            QString("레이아웃: %1 (표시 %2칸)  |  활성 세션: %3 / %4")
                .arg(m_layoutName)
                .arg(m_visibleSlotCount)
                           .arg(active).arg(TOTAL));
    }
}

int MainWindow::resolveOrCreateCameraId(const QString& url)
{
    const QString normalizedUrl = url.trimmed();
    if (normalizedUrl.isEmpty()) return 0;

    if (m_streamList) {
        for (int i = 0; i < m_streamList->count(); ++i) {
            const auto* item = m_streamList->item(i);
            if (!item) continue;

            const QString itemUrl = item->data(kCameraUrlRole).toString().trimmed().isEmpty()
                                        ? item->text().trimmed()
                                        : item->data(kCameraUrlRole).toString().trimmed();
            if (itemUrl != normalizedUrl) continue;

            const int cameraId = item->data(kCameraIdRole).toInt();
            if (cameraId > 0) {
                return cameraId;
            }
        }
    }

    if (m_ephemeralCameraIds.contains(normalizedUrl)) {
        return m_ephemeralCameraIds.value(normalizedUrl);
    }

    const int ephemeralCameraId = m_nextEphemeralCameraId--;
    m_ephemeralCameraIds.insert(normalizedUrl, ephemeralCameraId);
    return ephemeralCameraId;
}

void MainWindow::bindCellToCamera(int cellId, int cameraId, const QString& url)
{
    if (cellId < 0 || cellId >= TOTAL || cameraId == 0) return;

    // 기존 바인딩은 먼저 해제해 셀당 세션 1개 규칙을 유지한다.
    unbindCell(cellId);

    auto sessionIt = m_sessions.find(cameraId);
    if (sessionIt == m_sessions.end()) {
        StreamSession session;
        session.cameraId = cameraId;
        session.url = url;
        if (cameraId > 0) {
            const auto cameraResult = DBManager::instance().getCameraByCameraId(cameraId);
            if (cameraResult) {
                const auto& camera = cameraResult.value();
                session.url = QString::fromStdString(camera.url).trimmed();
                session.recordRequested = camera.record_enabled;
            }
            session.analysisEnabled = loadCameraAnalysisEnabled(cameraId, m_recordAutoDecodeEnabled);
        }
        m_sessions.insert(cameraId, session);
        sessionIt = m_sessions.find(cameraId);
    }

    auto& session = sessionIt.value();
    if (session.url.isEmpty()) {
        session.url = url;
    }

    m_cellToCamera.insert(cellId, cameraId);
    session.attachedCells.insert(cellId);

    m_cells[cellId]->activate(session.url);
    m_cells[cellId]->updateDetections(session.lastDetections, session.lastDetectionFrameSize);
    if (!session.lastStatus.trimmed().isEmpty()) {
        m_cells[cellId]->setStatus(session.lastStatus);
    }
    syncSessionLifetime(cameraId);

    // 이미 디코딩 중인 세션이라면 bind 직후 최신 프레임을 한 번 가져와 즉시 표시를 시도한다.
    if (session.worker) {
        cv::cuda::GpuMat gpuFrame;
        qint64 frameUtcMs = 0;
        qint64 frameSeq = 0;
        cv::Size frameSize;
        qint64 captureUtcMs = 0;
        qint64 sourcePts = 0;
        int sourceTimeBaseNum = 0;
        int sourceTimeBaseDen = 1;
        if (session.worker->takeLatestGpuFrame(gpuFrame,
                                               frameUtcMs,
                                               frameSeq,
                                               captureUtcMs,
                                               sourcePts,
                                               sourceTimeBaseNum,
                                               sourceTimeBaseDen,
                                               frameSize)) {
            m_cells[cellId]->updateGpuFrame(gpuFrame);
        } else {
            QImage frame;
            if (session.worker->takeLatestFrame(frame,
                                                frameUtcMs,
                                                frameSeq,
                                                captureUtcMs,
                                                sourcePts,
                                                sourceTimeBaseNum,
                                                sourceTimeBaseDen,
                                                frameSize)) {
                m_cells[cellId]->updateFrame(frame);
            }
        }
    }
}

void MainWindow::unbindCell(int cellId)
{
    const auto bindingIt = m_cellToCamera.find(cellId);
    if (bindingIt == m_cellToCamera.end()) {
        if (cellId >= 0 && cellId < TOTAL) {
            m_cells[cellId]->deactivate();
        }
        return;
    }

    const int cameraId = bindingIt.value();
    m_cellToCamera.erase(bindingIt);

    auto sessionIt = m_sessions.find(cameraId);
    if (sessionIt != m_sessions.end()) {
        sessionIt->attachedCells.remove(cellId);
    }

    if (cellId >= 0 && cellId < TOTAL) {
        m_cells[cellId]->deactivate();
    }

    syncSessionLifetime(cameraId);
}

void MainWindow::setCameraRecordingRequested(int cameraId, bool requested)
{
    if (cameraId == 0) return;

    auto sessionIt = m_sessions.find(cameraId);
    if (sessionIt == m_sessions.end()) {
        if (!requested) return;

        StreamSession session;
        session.cameraId = cameraId;

        const auto cameraResult = DBManager::instance().getCameraByCameraId(cameraId);
        if (cameraResult) {
            session.url = QString::fromStdString(cameraResult.value().url).trimmed();
            session.analysisEnabled = loadCameraAnalysisEnabled(cameraId, m_recordAutoDecodeEnabled);
        }
        session.recordRequested = true;
        m_sessions.insert(cameraId, session);
    } else {
        sessionIt->recordRequested = requested;
        if (!requested) {
            sessionIt->recordSegmentStartUtcMs = 0;
            sessionIt->recordSegmentEndUtcMs = 0;
            sessionIt->recordSegmentStartSourcePts = 0;
            sessionIt->recordSegmentSourceTimeBaseNum = 0;
            sessionIt->recordSegmentSourceTimeBaseDen = 1;
            sessionIt->recordSegmentFilePath.clear();
        }
    }

    syncSessionLifetime(cameraId);
    updateStatusBar();
}

void MainWindow::setCameraAnalysisEnabled(int cameraId, bool enabled)
{
    if (cameraId == 0) return;

    auto sessionIt = m_sessions.find(cameraId);
    if (sessionIt == m_sessions.end()) {
        if (!enabled) {
            return;
        }

        StreamSession session;
        session.cameraId = cameraId;
        session.analysisEnabled = enabled;

        const auto cameraResult = DBManager::instance().getCameraByCameraId(cameraId);
        if (cameraResult) {
            const auto& camera = cameraResult.value();
            session.url = QString::fromStdString(camera.url).trimmed();
            session.recordRequested = camera.record_enabled;
        }

        m_sessions.insert(cameraId, session);
    } else {
        sessionIt->analysisEnabled = enabled;
        if (!enabled) {
            sessionIt->analysisBusy = false;
            sessionIt->lastDetections.clear();
            sessionIt->lastDetectionFrameSize = cv::Size{};
            if (sessionIt->worker) {
                sessionIt->worker->setAnalysisBusy(false);
            }
            for (const int cellId : sessionIt->attachedCells) {
                if (cellId >= 0 && cellId < TOTAL) {
                    m_cells[cellId]->updateDetections({}, cv::Size{});
                }
            }
        }
    }

    syncSessionLifetime(cameraId);
    updateStatusBar();
}

bool MainWindow::loadCameraAnalysisEnabled(int cameraId, bool defaultValue) const
{
    QSettings settings;
    const QString key = cameraAnalysisSettingKey(cameraId);
    if (settings.contains(key)) {
        return settings.value(key).toBool();
    }
    return defaultValue;
}

void MainWindow::saveCameraAnalysisEnabled(int cameraId, bool enabled) const
{
    if (cameraId <= 0) {
        return;
    }

    QSettings settings;
    settings.setValue(cameraAnalysisSettingKey(cameraId), enabled);
    settings.sync();
}

void MainWindow::ensureInferenceWorkerStarted()
{
    if (m_inferenceThreadStarted.load(std::memory_order_acquire)) {
        return;
    }

    m_inferenceStopRequested.store(false, std::memory_order_release);
    m_inferenceThread = std::thread([this]() {
        Yolov8Inference inference;
        bool inferenceLoaded = false;
        try {
            inference.load(kYoloModelPath);
            inferenceLoaded = inference.isLoaded() && inference.getExecutionProviderName() == "CUDA";
        } catch (...) {
            inferenceLoaded = false;
        }

        while (!m_inferenceStopRequested.load(std::memory_order_acquire)) {
            InferenceTask task;
            {
                std::unique_lock<std::mutex> lock(m_inferenceQueueMutex);
                m_inferenceQueueCv.wait(lock, [this]() {
                    return m_inferenceStopRequested.load(std::memory_order_acquire) || !m_inferenceQueue.empty();
                });

                if (m_inferenceStopRequested.load(std::memory_order_acquire)) {
                    break;
                }

                if (m_inferenceQueue.empty()) {
                    continue;
                }

                task = std::move(m_inferenceQueue.front());
                m_inferenceQueue.pop();
            }

            std::vector<Yolov8Detection> detections;
            const cv::Size frameSize = task.frame.size();
            double inferenceMs = 0.0;
            if (inferenceLoaded && !task.frame.empty()) {
                try {
                    const auto inferenceStart = std::chrono::steady_clock::now();
                    detections = inference.inference_cuda(task.frame, 0.25f, 0.45f, true);
                    const auto inferenceEnd = std::chrono::steady_clock::now();
                    inferenceMs = std::chrono::duration<double, std::milli>(inferenceEnd - inferenceStart).count();
                } catch (...) {
                    detections.clear();
                }
            }

            task.frameInfo.detection_count = static_cast<int>(detections.size());
            task.frameInfo.inference_ms = inferenceMs;

            QMetaObject::invokeMethod(this, [this,
                                             cameraId = task.cameraId,
                                             detections = std::move(detections),
                                             frameSize,
                                             frameInfo = task.frameInfo]() mutable {
                auto* session = findSession(cameraId);
                if (!session) {
                    return;
                }

                session->analysisBusy = false;
                if (session->recordRequested && session->analysisEnabled) {
                    session->lastDetections = detections;
                    session->lastDetectionFrameSize = frameSize;
                } else {
                    session->lastDetections.clear();
                    session->lastDetectionFrameSize = cv::Size{};
                }

                if (session->recordRequested && session->analysisEnabled && !detections.empty()) {
                    const auto dbResult = DBManager::instance().saveDetectionResults(
                        frameInfo,
                        makeDetectionRows(frameInfo, detections));
                    if (!dbResult) {
                        qWarning() << "Detection 저장 실패:" << dbResult.error();
                    }
                } else if (session->recordRequested && session->analysisEnabled) {
                    const std::vector<detection_result> emptyRows;
                    const auto dbResult = DBManager::instance().saveDetectionResults(frameInfo, emptyRows);
                    if (!dbResult) {
                        qWarning() << "빈 Detection 프레임 저장 실패:" << dbResult.error();
                    }
                }

                if (session->worker) {
                    session->worker->setAnalysisBusy(false);
                }

                for (const int cellId : session->attachedCells) {
                    if (cellId >= 0 && cellId < TOTAL) {
                        m_cells[cellId]->updateDetections(session->lastDetections, session->lastDetectionFrameSize);
                    }
                }
            }, Qt::QueuedConnection);
        }
    });

    m_inferenceThreadStarted.store(true, std::memory_order_release);
}

void MainWindow::stopInferenceWorker()
{
    if (!m_inferenceThreadStarted.load(std::memory_order_acquire)) {
        return;
    }

    m_inferenceStopRequested.store(true, std::memory_order_release);
    m_inferenceQueueCv.notify_all();

    if (m_inferenceThread.joinable()) {
        m_inferenceThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_inferenceQueueMutex);
        std::queue<InferenceTask> empty;
        m_inferenceQueue.swap(empty);
    }

    m_inferenceThreadStarted.store(false, std::memory_order_release);
}

void MainWindow::enqueueInferenceTask(int cameraId, const cv::cuda::GpuMat& frame, const detection_frame_info& frameInfo)
{
    if (cameraId == 0 || frame.empty()) {
        return;
    }

    ensureInferenceWorkerStarted();

    {
        std::lock_guard<std::mutex> lock(m_inferenceQueueMutex);
        if (m_inferenceQueue.size() >= kMaxInferenceQueueDepth) {
            m_inferenceQueue.pop();
        }
        m_inferenceQueue.push(InferenceTask{cameraId, frame, frameInfo});
    }

    m_inferenceQueueCv.notify_one();
}

void MainWindow::syncSessionLifetime(int cameraId)
{
    auto* session = findSession(cameraId);
    if (!session) return;

    const QString streamUrl = session->url.trimmed();
    const bool shouldRecord = session->recordRequested;
    const bool shouldAnalyze = shouldRecord && session->analysisEnabled;
    const bool shouldDecode = !session->attachedCells.isEmpty() || shouldAnalyze || shouldRecord;

    if (!shouldDecode && session->worker) {
        auto* worker = session->worker;
        session->worker = nullptr;
        disconnect(worker, nullptr, this, nullptr);
        worker->stop();
        if (worker->isRunning()) {
            worker->wait(5000);
        }
        if (worker->isRunning()) {
            worker->wait();
        }
        delete worker;
        session->analysisBusy = false;
    }

    if (!shouldAnalyze) {
        session->analysisBusy = false;
        session->lastDetections.clear();
        session->lastDetectionFrameSize = cv::Size{};
        if (session->worker) {
            session->worker->setAnalysisBusy(false);
        }
        for (const int cellId : session->attachedCells) {
            if (cellId >= 0 && cellId < TOTAL) {
                m_cells[cellId]->updateDetections({}, cv::Size{});
            }
        }
    }

    if (streamUrl.isEmpty()) {
        if (!shouldRecord && !shouldDecode && session->attachedCells.isEmpty()) {
            m_sessions.remove(cameraId);
        }
        return;
    }

    if (shouldDecode && !session->worker) {
        const int segmentSeconds = effectiveRecordSegmentSeconds();
        session->worker = new StreamWorker(cameraId,
                                           streamUrl,
                                           QString::fromUtf8(kRecordOutputRoot),
                                           segmentSeconds,
                                           this);
        connect(session->worker, &StreamWorker::frameReady,
                this, &MainWindow::onFrameReady, Qt::QueuedConnection);
        connect(session->worker, &StreamWorker::statusChanged,
                this, &MainWindow::onStatusChanged, Qt::QueuedConnection);
        connect(session->worker, &StreamWorker::segmentStarted,
                this, &MainWindow::onRecorderSegmentStarted, Qt::QueuedConnection);
        connect(session->worker, &StreamWorker::segmentFinished,
                this, &MainWindow::onRecorderSegmentFinished, Qt::QueuedConnection);
        session->worker->start();
    }

    if (session->worker) {
        session->worker->setAnalysisBusy(session->analysisBusy);
        session->worker->setRecordingEnabled(shouldRecord);
        if (!shouldRecord) {
            session->recordSegmentStartUtcMs = 0;
            session->recordSegmentEndUtcMs = 0;
            session->recordSegmentStartSourcePts = 0;
            session->recordSegmentSourceTimeBaseNum = 0;
            session->recordSegmentSourceTimeBaseDen = 1;
            session->recordSegmentFilePath.clear();
        }
    }

    if (!shouldRecord && !shouldDecode && session->attachedCells.isEmpty()) {
        m_sessions.remove(cameraId);
    }
}

void MainWindow::stopAndDeleteSession(StreamSession& session)
{
    session.analysisBusy = false;
    session.lastDetections.clear();
    session.lastDetectionFrameSize = cv::Size{};
    session.recordSegmentStartUtcMs = 0;
    session.recordSegmentEndUtcMs = 0;
    session.recordSegmentStartSourcePts = 0;
    session.recordSegmentSourceTimeBaseNum = 0;
    session.recordSegmentSourceTimeBaseDen = 1;
    session.recordSegmentFilePath.clear();

    if (session.worker) {
        auto* worker = session.worker;
        session.worker = nullptr;

        disconnect(worker, nullptr, this, nullptr);
        worker->stop();
        if (worker->isRunning()) {
            worker->wait(5000);
        }
        if (worker->isRunning()) {
            worker->wait();
        }
        delete worker;
    }

}

MainWindow::StreamSession* MainWindow::findSession(int cameraId)
{
    const auto it = m_sessions.find(cameraId);
    if (it == m_sessions.end()) return nullptr;
    return &it.value();
}

const MainWindow::StreamSession* MainWindow::findSession(int cameraId) const
{
    const auto it = m_sessions.constFind(cameraId);
    if (it == m_sessions.cend()) return nullptr;
    return &it.value();
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
    m_streamListItemSyncing = true;
    item->setData(kCameraIdRole, camera.camera_id);
    item->setData(kCameraNameRole, QString::fromStdString(camera.name));
    item->setData(kCameraStreamTypeRole, static_cast<int>(camera.stream_type));
    item->setData(kCameraEnabledRole, camera.enabled);
    item->setData(kCameraRecordEnabledRole, camera.record_enabled);
    const bool analysisEnabled = loadCameraAnalysisEnabled(camera.camera_id, m_recordAutoDecodeEnabled);
    item->setData(kCameraAnalysisEnabledRole, analysisEnabled);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(analysisEnabled ? Qt::Checked : Qt::Unchecked);
    item->setData(kCameraUrlRole, url);
    item->setToolTip(makeCameraTooltip(camera));
    updateCameraItemVisualState(item);
    m_streamListItemSyncing = false;

    // 셀이 없어도 녹화가 켜진 카메라는 세션 수명주기에서 유지될 수 있게 즉시 반영한다.
    if (camera.record_enabled) {
        setCameraRecordingRequested(camera.camera_id, true);
    }
    setCameraAnalysisEnabled(camera.camera_id, analysisEnabled);
}

void MainWindow::updateCameraItemVisualState(QListWidgetItem* item)
{
    if (!item) return;

    const bool recordEnabled = item->data(kCameraRecordEnabledRole).toBool();
    if (recordEnabled) {
        item->setBackground(QColor(255, 230, 230));
        item->setForeground(QColor(180, 20, 20));
        return;
    }

    item->setBackground(QBrush());
    item->setForeground(QBrush());
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
    if (m_recordEnabledButton) {
        const QSignalBlocker blocker(m_recordEnabledButton);
        m_recordEnabledButton->setChecked(false);
        syncRecordButtonText(m_recordEnabledButton, false);
    }
    if (m_recordAutoDecodeInput) {
        const QSignalBlocker blocker(m_recordAutoDecodeInput);
        m_recordAutoDecodeInput->setChecked(m_recordAutoDecodeEnabled);
    }
}

