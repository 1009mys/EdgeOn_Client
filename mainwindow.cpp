#include "mainwindow.h"
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
#include <QAbstractItemView>
#include <QDrag>
#include <QMimeData>
#include <opencv2/core/cuda.hpp>

class StreamListWidget final : public QListWidget {
public:
    using QListWidget::QListWidget;

protected:
    void startDrag(Qt::DropActions supportedActions) override {
        auto* item = currentItem();
        if (!item) return;

        const QString text = item->text().trimmed();
        if (text.isEmpty()) return;

        auto* mime = new QMimeData();
        mime->setText(text);
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

    m_streamInput = new QLineEdit(panel);
    m_streamInput->setPlaceholderText("RTSP 주소 입력 (예: rtsp://...)");
    connect(m_streamInput, &QLineEdit::returnPressed, this, &MainWindow::addStreamAddress);

    auto* addBtn = new QPushButton("추가", panel);
    auto* delBtn = new QPushButton("삭제", panel);
    connect(addBtn, &QPushButton::clicked, this, &MainWindow::addStreamAddress);
    connect(delBtn, &QPushButton::clicked, this, &MainWindow::removeSelectedStreamAddress);

    auto* btnRow = new QHBoxLayout();
    btnRow->setContentsMargins(0, 0, 0, 0);
    btnRow->addWidget(addBtn);
    btnRow->addWidget(delBtn);

    m_streamList = new StreamListWidget(panel);
    m_streamList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_streamList->setDragEnabled(true);
    m_streamList->setDefaultDropAction(Qt::CopyAction);
    m_streamList->setToolTip("목록 항목을 비디오 셀로 드래그하면 해당 셀에서 스트림이 시작됩니다.");
    connect(m_streamList, &QListWidget::itemDoubleClicked,
            this, &MainWindow::onStreamListDoubleClicked);

    vbox->addWidget(m_streamInput);
    vbox->addLayout(btnRow);
    vbox->addWidget(m_streamList, 1);

    panel->setLayout(vbox);
    m_streamDock->setWidget(panel);
    addDockWidget(Qt::LeftDockWidgetArea, m_streamDock);
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
    if (!m_streamInput || !m_streamList) return;

    const QString url = m_streamInput->text().trimmed();
    if (url.isEmpty()) return;

    const auto found = m_streamList->findItems(url, Qt::MatchFixedString);
    if (!found.isEmpty()) {
        m_streamList->setCurrentItem(found.first());
        m_streamInput->clear();
        return;
    }

    m_streamList->addItem(url);
    m_streamInput->clear();
}

void MainWindow::removeSelectedStreamAddress() {
    if (!m_streamList) return;
    delete m_streamList->takeItem(m_streamList->currentRow());
}

void MainWindow::onStreamListDoubleClicked(QListWidgetItem* item) {
    if (!item) return;
    const QString url = item->text().trimmed();
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
        "텍스트 파일 (*.txt);;모든 파일 (*)");
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "오류", "파일을 열 수 없습니다:\n" + path);
        return;
    }

    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        if (m_streamList->findItems(line, Qt::MatchFixedString).isEmpty())
            m_streamList->addItem(line);
    }
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

