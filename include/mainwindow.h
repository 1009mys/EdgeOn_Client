#pragma once
#include <QMainWindow>
#include <QVector>
#include <QMap>
#include <QSet>
#include <QGridLayout>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include "videocell.h"
#include "streamworker.h"
#include "recorderworker.h"
#include "yolov8inference.h"

class QLabel;
class QMenu;
class QDockWidget;
class QListWidget;
class QListWidgetItem;
class QLineEdit;
class QComboBox;
class QCheckBox;
class QPushButton;
class QSpinBox;
class QSystemTrayIcon;
class QAction;
class QCloseEvent;
class QStackedWidget;
class VodPanel;
struct camera_info;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    static constexpr int MAX_COLS = 4;
    static constexpr int MAX_ROWS = 4;
    static constexpr int TOTAL    = MAX_COLS * MAX_ROWS;

    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onAddRequested   (int cellId, const QString& url);
    void onAddRequestedByCamera(int cellId, int cameraId, const QString& url);
    void onMoveRequested  (int fromCellId, int toCellId);
    void onRemoveRequested(int cellId);
    void onFrameReady     (int cameraId);
    void onStatusChanged  (int cameraId, const QString& status);
    void onRecorderStatusChanged(int cameraId, const QString& status);
    void loadUrlsFromFile ();
    void removeAllStreams ();
    void addStreamAddress();
    void updateSelectedStreamAddress();
    void removeSelectedStreamAddress();
    void onStreamListDoubleClicked(QListWidgetItem* item);
    void showFromTray();
    void quitFromTray();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    struct StreamSession {
        int cameraId{0};
        QString url;
        QString lastStatus;
        bool recordRequested{false};
        bool analysisEnabled{false};
        bool analysisBusy{false};
        std::vector<Yolov8Detection> lastDetections;
        cv::Size lastDetectionFrameSize{};
        StreamWorker* worker{nullptr};
        RecorderWorker* recorder{nullptr};
        QSet<int> attachedCells;
    };

    struct InferenceTask {
        int cameraId{0};
        cv::cuda::GpuMat frame;
    };

    void buildUi();
    void clearGridItems();
    void applyGridLayout(int rows, int cols);
    void applyFocusRingLayout();
    void applyTriplePlusQuadLayout();
    void addLayoutPresetAction(QMenu* menu, const QString& text, int rows, int cols);
    void buildStreamListPanel();
    void addStream   (int cellId, const QString& url);
    void addStreamByCamera(int cellId, int cameraId, const QString& url);
    void removeStream(int cellId);
    void updateStatusBar();

    int resolveOrCreateCameraId(const QString& url);
    void bindCellToCamera(int cellId, int cameraId, const QString& url);
    void unbindCell(int cellId);
    void setCameraRecordingRequested(int cameraId, bool requested);
    void setCameraAnalysisEnabled(int cameraId, bool enabled);
    bool loadCameraAnalysisEnabled(int cameraId, bool defaultValue) const;
    void saveCameraAnalysisEnabled(int cameraId, bool enabled) const;
    void syncSessionLifetime(int cameraId);
    void stopAndDeleteSession(StreamSession& session);
    StreamSession* findSession(int cameraId);
    const StreamSession* findSession(int cameraId) const;
    void ensureInferenceWorkerStarted();
    void stopInferenceWorker();
    void enqueueInferenceTask(int cameraId, const cv::cuda::GpuMat& frame);

    void loadCameraListFromDB();
    bool saveCameraToDB(const camera_info& camera);
    void addCameraListItem(const camera_info& camera);
    void updateCameraItemVisualState(QListWidgetItem* item);
    void resetInlineCameraForm(bool clearSelection = false);
    void setupSystemTray();
    void shutdownAllSessions();

    QWidget*                 m_central{nullptr};
    QGridLayout*             m_grid{nullptr};
    int                      m_visibleRows{4};
    int                      m_visibleCols{4};
    int                      m_visibleSlotCount{TOTAL};
    QString                  m_layoutName{"4x4"};
    QDockWidget*             m_streamDock{nullptr};
    QListWidget*             m_streamList{nullptr};
    QSpinBox*                m_cameraIdInput{nullptr};
    QLineEdit*               m_nameInput{nullptr};
    QComboBox*               m_streamTypeInput{nullptr};
    QLineEdit*               m_streamInput{nullptr};
    QCheckBox*               m_enabledInput{nullptr};
    QPushButton*             m_recordEnabledButton{nullptr};
    QCheckBox*               m_recordAutoDecodeInput{nullptr};
    bool                     m_recordAutoDecodeEnabled{true};
    bool                     m_streamListItemSyncing{false};
    QVector<VideoCell*>      m_cells;
    QMap<int, StreamSession> m_sessions;
    QMap<int, int>           m_cellToCamera;
    QMap<QString, int>       m_ephemeralCameraIds;
    QSystemTrayIcon*         m_trayIcon{nullptr};
    QAction*                 m_showAction{nullptr};
    QAction*                 m_quitAction{nullptr};
    bool                     m_allowClose{false};
    bool                     m_isShuttingDown{false};
    bool                     m_trayEnabled{false};
    bool                     m_trayNoticeShown{false};
    QLabel*                  m_statusLabel{nullptr};
    int                      m_nextCameraId{1};
    int                      m_nextEphemeralCameraId{-1};
    QStackedWidget*          m_stackedMain{nullptr};
    VodPanel*                m_vodPanel{nullptr};

    std::mutex               m_inferenceQueueMutex;
    std::condition_variable  m_inferenceQueueCv;
    std::queue<InferenceTask> m_inferenceQueue;
    std::thread              m_inferenceThread;
    std::atomic<bool>        m_inferenceStopRequested{false};
    std::atomic<bool>        m_inferenceThreadStarted{false};
};
