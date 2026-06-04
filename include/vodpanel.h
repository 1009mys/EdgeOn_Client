#pragma once
#include <QWidget>
#include <QVector>
#include <QMediaPlayer>

#include <unordered_map>

#include "Protocol.h"
#include "vodtimeline.h"

class QAudioOutput;
class QStackedLayout;
class QLabel;
class QPushButton;
class QComboBox;
class VodVideoView;

/**
 * 저장영상 확인 패널.
 *
 * - loadCamera(cameraId) : 녹화 폴더를 스캔하여 타임라인을 구성
 * - QMediaPlayer 로 로컬 .mkv 파일 재생
 * - 스페이스 : 재생/일시정지
 * - ←/→ 방향키 : ±5 초 탐색
 * - Ctrl + 휠 : 타임라인 줌 (VodTimeline 위임)
 */
enum class FrameMatchMode {
    FRAME_UTC_MS,      // frame_utc_ms 기준 우선
    CAPTURE_UTC_MS,    // capture_utc_ms 기준 우선
    AUTO               // segment_relative_ms 우선, 실패 시 capture_utc_ms/frame_utc_ms 선택
};

class VodPanel : public QWidget {
    Q_OBJECT
public:
    explicit VodPanel(const QString& outputRoot, QWidget* parent = nullptr);
    ~VodPanel() override;

    void loadCamera(int cameraId, int segmentSeconds = 30);
    void setFrameMatchMode(FrameMatchMode mode) { m_frameMatchMode = mode; }

protected:
    void keyPressEvent(QKeyEvent* event) override;
    bool focusNextPrevChild(bool) override { return false; }

private slots:
    void onSeekRequested(qint64 absMs);
    void onPlayerPositionChanged(int slot, qint64 posMs);
    void onMediaStatusChanged(int slot, QMediaPlayer::MediaStatus status);
    void onDurationChanged(int slot, qint64 duration);
    void onPlaybackStateChanged(int slot, QMediaPlayer::PlaybackState state);
    void togglePlayPause();

private:
    static constexpr int kPlayerCount = 2;
    static constexpr int kInvalidSegIdx = -1;

    void playAt(qint64 absMs, bool startPlaying);
    int  findSegmentIndex(qint64 absMs) const;
    int  standbySlot() const;
    void resetPlayers();
    void clearPrebuffer();
    void prebufferNextSegment();
    void switchToSlot(int slot);
    void loadSegmentToSlot(int slot, int segIdx, qint64 offsetMs, bool startPlaying);
    void tryApplyPendingSeek(int slot);
    void updateControls();
    void prefetchDetectionsForSegment(int segIdx);
    void syncDetectionPrefetch(int segIdx);
    void updateOverlayForAbsMs(qint64 absMs);
    void clearAllOverlays();
    int  segmentIndexForSlot(int slot) const;
    const detection_frame_group* findBestFrameForAbsMs(const std::vector<detection_frame_group>& frames,
                                                       qint64 absMs) const;

    QString         m_outputRoot;
    int             m_currentCameraId{-1};

    QMediaPlayer*   m_players[kPlayerCount]{nullptr, nullptr};
    QAudioOutput*   m_audioOutputs[kPlayerCount]{nullptr, nullptr};
    VodVideoView*   m_views[kPlayerCount]{nullptr, nullptr};   // QVideoWidget+overlay 대체
    QStackedLayout* m_videoStack{nullptr};

    VodTimeline*    m_timeline{nullptr};
    QLabel*         m_timeLabel{nullptr};
    QPushButton*    m_playBtn{nullptr};
    QLabel*         m_infoLabel{nullptr};
    QComboBox*      m_frameMatchModeBox{nullptr};

    QVector<VodSegment> m_segments;
    int             m_activeSlot{0};
    int             m_activeSegIdx{kInvalidSegIdx};
    int             m_prebufferSegIdx{kInvalidSegIdx};
    qint64          m_pendingSeekMs[kPlayerCount]{-1, -1};
    bool            m_startPlayingOnReady[kPlayerCount]{false, false};
    bool            m_isPrebufferWarmup[kPlayerCount]{false, false};
    bool            m_isProgrammaticPause[kPlayerCount]{false, false};
    int             m_warmupGeneration[kPlayerCount]{0, 0};
    qint64          m_latestVideoAbsMs[kPlayerCount]{-1, -1};
    qint64          m_latestVideoPosMs[kPlayerCount]{-1, -1};
    qint64          m_lastOverlayMatchAbsMs[kPlayerCount]{-1, -1};
    qint64          m_lastOverlayFrameUtcMs[kPlayerCount]{-1, -1};
    qint64          m_lastOverlayFrameSeq[kPlayerCount]{-1, -1};
    qint64          m_lastOverlayCommitWallMs[kPlayerCount]{0, 0};
    bool            m_overlayVisible[kPlayerCount]{false, false};

    std::unordered_map<int, std::vector<detection_frame_group>> m_detectionCacheBySegIdx;
    FrameMatchMode  m_frameMatchMode{FrameMatchMode::AUTO};

    static constexpr qint64 kDetectionMatchToleranceMs = 80;
    static constexpr qint64 kSegmentBoundaryPreferNextMs = 100;
    static constexpr qint64 kOverlayHoldMs = 220;
};

