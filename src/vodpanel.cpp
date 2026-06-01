#include "vodpanel.h"

#include <QMediaPlayer>
#include <QAudioOutput>
#include <QVideoWidget>
#include <QStackedLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QTimer>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QRegularExpression>
#include <QSizePolicy>
#include <QUrl>
#include <algorithm>

namespace {
constexpr qint64 kPrebufferLeadMs = 1500;  // 파일 경계 1.5초 전부터 다음 파일 선버퍼링
constexpr qint64 kSeekSettleToleranceMs = 250;
constexpr qint64 kReliableDurationThresholdMs = 1000;
constexpr float kActiveVolume = 0.8f;
constexpr float kMuteVolume = 0.0f;
}

// ── 생성자 ─────────────────────────────────────────────────────
VodPanel::VodPanel(const QString& outputRoot, QWidget* parent)
    : QWidget(parent), m_outputRoot(outputRoot)
{
    setFocusPolicy(Qt::StrongFocus);

    // ── 미디어 플레이어 (활성/대기 이중화) ─────────────────────
    auto* videoHost = new QWidget(this);
    m_videoStack = new QStackedLayout(videoHost);
    m_videoStack->setContentsMargins(0, 0, 0, 0);

    for (int i = 0; i < kPlayerCount; ++i) {
        m_players[i] = new QMediaPlayer(this);
        m_audioOutputs[i] = new QAudioOutput(this);
        m_players[i]->setAudioOutput(m_audioOutputs[i]);
        m_audioOutputs[i]->setVolume(i == m_activeSlot ? kActiveVolume : kMuteVolume);

        m_videoWidgets[i] = new QVideoWidget(videoHost);
        m_videoWidgets[i]->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        m_videoWidgets[i]->setStyleSheet("background: black;");
        m_players[i]->setVideoOutput(m_videoWidgets[i]);
        m_videoStack->addWidget(m_videoWidgets[i]);

        connect(m_players[i], &QMediaPlayer::positionChanged,
                this, [this, i](qint64 posMs) { onPlayerPositionChanged(i, posMs); });
        connect(m_players[i], &QMediaPlayer::mediaStatusChanged,
                this, [this, i](QMediaPlayer::MediaStatus status) { onMediaStatusChanged(i, status); });
        connect(m_players[i], &QMediaPlayer::durationChanged,
                this, [this, i](qint64 duration) { onDurationChanged(i, duration); });
        connect(m_players[i], &QMediaPlayer::playbackStateChanged,
                this, [this, i](QMediaPlayer::PlaybackState state) { onPlaybackStateChanged(i, state); });
    }
    m_videoStack->setCurrentIndex(m_activeSlot);

    // ── 타임라인 ─────────────────────────────────────────────
    m_timeline = new VodTimeline(this);
    m_timeline->setFixedHeight(90);

    // ── 컨트롤 행 ────────────────────────────────────────────
    m_playBtn = new QPushButton("▶  재생", this);
    m_playBtn->setFixedWidth(110);
    m_playBtn->setStyleSheet(
        "QPushButton { background:#1e5080; color:white; border:none;"
        "  border-radius:5px; padding:5px 12px; font-size:13px; }"
        "QPushButton:hover { background:#2a70b0; }"
        "QPushButton:pressed { background:#155070; }");
    connect(m_playBtn, &QPushButton::clicked, this, &VodPanel::togglePlayPause);

    m_timeLabel = new QLabel("----/--/--  --:--:--", this);
    m_timeLabel->setStyleSheet(
        "color:#dddddd; font-size:13px; font-family:monospace;");
    m_timeLabel->setMinimumWidth(220);

    m_infoLabel = new QLabel("좌측 목록에서 카메라를 선택하세요", this);
    m_infoLabel->setStyleSheet("color:#888888; font-size:11px;");
    m_infoLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    auto* ctrlRow = new QHBoxLayout();
    ctrlRow->setContentsMargins(8, 4, 8, 4);
    ctrlRow->setSpacing(10);
    ctrlRow->addWidget(m_playBtn);
    ctrlRow->addWidget(m_timeLabel);
    ctrlRow->addStretch();
    ctrlRow->addWidget(m_infoLabel);

    // ── 힌트 레이블 ──────────────────────────────────────────
    auto* hintLabel = new QLabel(
        "Space: 재생/일시정지   ←→: ±5초   Ctrl+휠: 줌", this);
    hintLabel->setStyleSheet("color:#555555; font-size:10px;");
    hintLabel->setAlignment(Qt::AlignCenter);

    // ── 전체 레이아웃 ─────────────────────────────────────────
    auto* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);
    vbox->addWidget(videoHost, 1);
    vbox->addLayout(ctrlRow);
    vbox->addWidget(m_timeline);
    vbox->addWidget(hintLabel);

    // ── 시그널 연결 ───────────────────────────────────────────
    connect(m_timeline, &VodTimeline::seekRequested,
            this, &VodPanel::onSeekRequested);

}

VodPanel::~VodPanel() = default;

// ── 카메라 로드 ────────────────────────────────────────────────
void VodPanel::loadCamera(int cameraId, int segmentSeconds) {
    resetPlayers();
    m_segments.clear();
    m_activeSegIdx = kInvalidSegIdx;
    m_prebufferSegIdx = kInvalidSegIdx;

    const QString cameraDir = QDir::cleanPath(
        m_outputRoot + QString("/camera_%1").arg(cameraId));
    QDir dir(cameraDir);

    if (!dir.exists()) {
        m_infoLabel->setText(
            QString("저장 폴더 없음: camera_%1").arg(cameraId));
        m_timeline->setSegments({});
        updateControls();
        return;
    }

    // ---- 파일 스캔 ----
    // 파일명 패턴: camera_{id}_{yyyyMMdd_HHmmss}_{yyyyMMdd_HHmmss}.mkv
    // 녹화 중인 임시 파일(RECORDING.mkv)은 제외
    const QRegularExpression re(
        QString(R"(^camera_%1_(\d{8}_\d{6})_(\d{8}_\d{6})\.mkv$)").arg(cameraId));

    const QStringList files =
        dir.entryList({"*.mkv"}, QDir::Files, QDir::Name);

    QVector<QPair<qint64, QString>> entries;
    for (const QString& file : files) {
        const auto match = re.match(file);
        if (!match.hasMatch()) continue;   // RECORDING.mkv 등 제외

        QDateTime dtStart = QDateTime::fromString(match.captured(1), "yyyyMMdd_HHmmss");
        QDateTime dtEnd   = QDateTime::fromString(match.captured(2), "yyyyMMdd_HHmmss");
        if (!dtStart.isValid() || !dtEnd.isValid()) continue;

        entries.append({dtStart.toMSecsSinceEpoch(), dir.filePath(file)});
    }
    std::sort(entries.begin(), entries.end());

    // ---- 세그먼트 구성 (종료 시각은 파일명에서 직접 파싱) ----
    const QRegularExpression reEnd(
        QString(R"(^camera_%1_\d{8}_\d{6}_(\d{8}_\d{6})\.mkv$)").arg(cameraId));

    for (const auto& [startMs, filePath] : entries) {
        VodSegment seg;
        seg.startMs  = startMs;
        seg.filePath = filePath;

        const QString fileName = QFileInfo(filePath).fileName();
        const auto mEnd = reEnd.match(fileName);
        if (mEnd.hasMatch()) {
            QDateTime dtEnd = QDateTime::fromString(mEnd.captured(1), "yyyyMMdd_HHmmss");
            seg.endMs = dtEnd.isValid() ? dtEnd.toMSecsSinceEpoch()
                                        : startMs + static_cast<qint64>(segmentSeconds) * 1000;
        } else {
            seg.endMs = startMs + static_cast<qint64>(segmentSeconds) * 1000;
        }
        m_segments.append(seg);
    }

    m_timeline->setSegments(m_segments);

    if (m_segments.isEmpty()) {
        m_infoLabel->setText(
            QString("카메라 %1: 저장된 영상 없음").arg(cameraId));
    } else {
        m_infoLabel->setText(
            QString("카메라 %1  |  영상 %2 개").arg(cameraId).arg(m_segments.size()));
        m_timeline->setPosition(m_segments.first().startMs);
        m_timeLabel->setText(
            QDateTime::fromMSecsSinceEpoch(m_segments.first().startMs)
                .toString("yyyy/MM/dd  HH:mm:ss"));
    }
    updateControls();
}

// ── 키보드 단축키 ──────────────────────────────────────────────
void VodPanel::keyPressEvent(QKeyEvent* event) {
    const qint64 pos = m_timeline->positionMs();
    switch (event->key()) {
    case Qt::Key_Space:
        togglePlayPause();
        event->accept();
        return;
    case Qt::Key_Left:
        onSeekRequested(pos - 5000);
        event->accept();
        return;
    case Qt::Key_Right:
        onSeekRequested(pos + 5000);
        event->accept();
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

// ── 탐색 ──────────────────────────────────────────────────────
void VodPanel::onSeekRequested(qint64 absMs) {
    if (m_segments.isEmpty()) return;
    absMs = qBound(m_segments.first().startMs,
                   absMs,
                   m_segments.last().endMs);
    m_timeline->setPosition(absMs);
    const bool wasPlaying =
        m_players[m_activeSlot]->playbackState() == QMediaPlayer::PlayingState;
    playAt(absMs, wasPlaying);
}

void VodPanel::playAt(qint64 absMs, bool startPlaying) {
    const int idx = findSegmentIndex(absMs);
    if (idx < 0) return;

    const qint64 offset = absMs - m_segments[idx].startMs;

    if (m_activeSegIdx == idx) {
        // 같은 파일 내 탐색.
        // setPosition() 직후 Qt가 positionChanged(0)을 먼저 발생시키므로,
        // pending을 유지한 채 seek해야 타임라인이 파일 시작으로 튀지 않는다.
        const bool needPlay = startPlaying &&
            m_players[m_activeSlot]->playbackState() != QMediaPlayer::PlayingState;
        m_pendingSeekMs[m_activeSlot]         = offset;
        m_startPlayingOnReady[m_activeSlot]   = needPlay;
        m_players[m_activeSlot]->setPosition(offset);
        prebufferNextSegment();
        return;
    }

    // 이미 다음 파일을 prebuffer해 둔 경우 즉시 스왑
    if (m_prebufferSegIdx == idx) {
        const int oldActive = m_activeSlot;
        const int nextActive = standbySlot();

        // warmup 타이머가 남아 있어도 새 active 슬롯에는 적용되지 않게 무효화한다.
        m_isPrebufferWarmup[nextActive] = false;
        m_isProgrammaticPause[nextActive] = false;
        ++m_warmupGeneration[nextActive];

        switchToSlot(nextActive);
        m_activeSegIdx = idx;
        m_prebufferSegIdx = kInvalidSegIdx;

        m_players[oldActive]->stop();
        m_players[oldActive]->setSource({});

        // duration/decoder 타이밍 이슈로 seek가 무시되는 경우가 있어 pending을 항상 유지한다.
        m_pendingSeekMs[nextActive] = offset;
        m_startPlayingOnReady[nextActive] = startPlaying;
        tryApplyPendingSeek(nextActive);

        prebufferNextSegment();
        return;
    }

    clearPrebuffer();
    loadSegmentToSlot(m_activeSlot, idx, offset, startPlaying);
    prebufferNextSegment();
}

int VodPanel::findSegmentIndex(qint64 absMs) const {
    if (m_segments.isEmpty()) return -1;
    for (int i = 0; i < m_segments.size(); ++i) {
        if (absMs >= m_segments[i].startMs && absMs < m_segments[i].endMs)
            return i;
    }
    if (absMs < m_segments.first().startMs) return 0;
    return m_segments.size() - 1;
}

int VodPanel::standbySlot() const {
    return (m_activeSlot + 1) % kPlayerCount;
}

void VodPanel::resetPlayers() {
    for (int i = 0; i < kPlayerCount; ++i) {
        m_players[i]->stop();
        m_players[i]->setSource({});
        m_pendingSeekMs[i] = -1;
        m_startPlayingOnReady[i] = false;
        m_isPrebufferWarmup[i] = false;
        m_isProgrammaticPause[i] = false;
        ++m_warmupGeneration[i];
        m_audioOutputs[i]->setVolume(i == m_activeSlot ? kActiveVolume : kMuteVolume);
    }
}

void VodPanel::clearPrebuffer() {
    const int standby = standbySlot();
    m_players[standby]->stop();
    m_players[standby]->setSource({});
    m_pendingSeekMs[standby] = -1;
    m_startPlayingOnReady[standby] = false;
    m_isPrebufferWarmup[standby] = false;
    m_isProgrammaticPause[standby] = false;
    ++m_warmupGeneration[standby];
    m_prebufferSegIdx = kInvalidSegIdx;
}

void VodPanel::switchToSlot(int slot) {
    m_activeSlot = slot;
    if (m_videoStack) {
        m_videoStack->setCurrentIndex(slot);
    }
    for (int i = 0; i < kPlayerCount; ++i) {
        m_audioOutputs[i]->setVolume(i == m_activeSlot ? kActiveVolume : kMuteVolume);
    }
}

void VodPanel::loadSegmentToSlot(int slot, int segIdx, qint64 offsetMs, bool startPlaying) {
    if (segIdx < 0 || segIdx >= m_segments.size()) return;

    m_pendingSeekMs[slot] = qMax<qint64>(0, offsetMs);
    m_startPlayingOnReady[slot] = startPlaying;

    // active 슬롯은 warmup을 수행하면 offset이 0으로 되돌아갈 수 있으므로 금지한다.
    const bool isStandbySlot = (slot != m_activeSlot);
    m_isPrebufferWarmup[slot] = isStandbySlot && !startPlaying;
    m_isProgrammaticPause[slot] = false;
    ++m_warmupGeneration[slot];

    m_players[slot]->stop();
    m_players[slot]->setSource(QUrl::fromLocalFile(m_segments[segIdx].filePath));

    if (slot == m_activeSlot) {
        m_activeSegIdx = segIdx;
    } else {
        m_prebufferSegIdx = segIdx;
    }
}

void VodPanel::prebufferNextSegment() {
    if (m_activeSegIdx < 0 || m_activeSegIdx >= m_segments.size()) {
        clearPrebuffer();
        return;
    }

    const int nextSeg = m_activeSegIdx + 1;
    if (nextSeg >= m_segments.size()) {
        clearPrebuffer();
        return;
    }

    if (m_prebufferSegIdx == nextSeg) {
        return;
    }

    const int standby = standbySlot();
    loadSegmentToSlot(standby, nextSeg, 0, false);
}

// ── 플레이어 시그널 처리 ───────────────────────────────────────
void VodPanel::onPlayerPositionChanged(int slot, qint64 posMs) {
    // positionChanged는 seek 확정 여부를 판단할 수 있는 가장 신뢰도 높은 신호다.
    if (m_pendingSeekMs[slot] >= 0) {
        tryApplyPendingSeek(slot);

        // pending seek가 남아있으면(주로 첫 0ms 이벤트) active 타임라인 갱신을 보류한다.
        if (slot == m_activeSlot && m_pendingSeekMs[slot] >= 0) {
            return;
        }
    }

    if (slot != m_activeSlot) return;
    if (m_activeSegIdx < 0 || m_activeSegIdx >= m_segments.size()) return;
    const qint64 absMs = m_segments[m_activeSegIdx].startMs + posMs;
    m_timeline->setPosition(absMs);
    m_timeLabel->setText(
        QDateTime::fromMSecsSinceEpoch(absMs).toString("yyyy/MM/dd  HH:mm:ss"));

    const qint64 remainingMs = m_segments[m_activeSegIdx].endMs - absMs;
    if (remainingMs <= kPrebufferLeadMs) {
        prebufferNextSegment();
    }
}

void VodPanel::onMediaStatusChanged(int slot, QMediaPlayer::MediaStatus status) {
    if (status == QMediaPlayer::LoadedMedia ||
        status == QMediaPlayer::BufferedMedia ||
        status == QMediaPlayer::BufferingMedia) {
        tryApplyPendingSeek(slot);
    }

    if (slot != m_activeSlot) {
        return;
    }

    if (status == QMediaPlayer::EndOfMedia) {
        const int expectedNextSeg = m_activeSegIdx + 1;
        const int nextSlot = standbySlot();

        // 이미 prebuffer 된 다음 세그먼트가 있으면 화면/오디오만 즉시 스왑
        if (m_prebufferSegIdx == expectedNextSeg &&
            expectedNextSeg >= 0 && expectedNextSeg < m_segments.size()) {
            const int oldActive = m_activeSlot;
            switchToSlot(nextSlot);
            m_activeSegIdx = expectedNextSeg;
            m_prebufferSegIdx = kInvalidSegIdx;

            m_players[oldActive]->stop();
            m_players[oldActive]->setSource({});

            if (m_players[m_activeSlot]->playbackState() != QMediaPlayer::PlayingState) {
                m_players[m_activeSlot]->play();
            }

            prebufferNextSegment();
            updateControls();
            return;
        }

        // prebuffer 실패/미준비 시 기존 방식 폴백
        if (expectedNextSeg >= 0 && expectedNextSeg < m_segments.size()) {
            loadSegmentToSlot(m_activeSlot, expectedNextSeg, 0, true);
            prebufferNextSegment();
        }
    }
}

void VodPanel::onDurationChanged(int slot, qint64 duration) {
    if (duration <= 0) return;

    tryApplyPendingSeek(slot);

    // standby prebuffer 워밍업: 짧게 재생 후 0초로 되돌려 첫 프레임 준비
    if (m_isPrebufferWarmup[slot]) {
        m_isPrebufferWarmup[slot] = false;
        m_isProgrammaticPause[slot] = true;
        const int generation = m_warmupGeneration[slot];
        m_players[slot]->play();
        QTimer::singleShot(120, this, [this, slot, generation]() {
            // slot이 active로 승격됐거나, 새 파일 로드로 generation이 바뀌면 stale 타이머로 간주한다.
            if (slot == m_activeSlot) return;
            if (generation != m_warmupGeneration[slot]) return;
            if (!m_isProgrammaticPause[slot]) return;
            m_players[slot]->pause();
            m_players[slot]->setPosition(0);
            m_isProgrammaticPause[slot] = false;
        });
    }
}

void VodPanel::onPlaybackStateChanged(int slot, QMediaPlayer::PlaybackState) {
    if (m_pendingSeekMs[slot] >= 0) {
        tryApplyPendingSeek(slot);
    }

    if (slot == m_activeSlot) {
        updateControls();
    }
}

void VodPanel::tryApplyPendingSeek(int slot)
{
    if (slot < 0 || slot >= kPlayerCount) return;
    if (m_pendingSeekMs[slot] < 0) return;

    auto* player = m_players[slot];
    if (!player) return;

    const qint64 requested = qMax<qint64>(0, m_pendingSeekMs[slot]);
    const qint64 duration = player->duration();
    if (duration <= 0) return;

    const bool durationReliable = (duration >= kReliableDurationThresholdMs);
    const qint64 maxPos = qMax<qint64>(0, duration - 50);
    const qint64 target = durationReliable
                              ? qBound<qint64>(0, requested, maxPos)
                              : requested;
    const qint64 current = player->position();

    // duration이 충분히 신뢰 가능한 시점에 목표 위치에 도달하면 pending을 해제한다.
    // (메타데이터 초기 단계 duration 불안정 시 0ms로 오판 확정되는 문제를 방지)
    if (qAbs(current - target) <= kSeekSettleToleranceMs) {
        if (!durationReliable && requested > kSeekSettleToleranceMs) {
            player->setPosition(target);
            return;
        }

        m_pendingSeekMs[slot] = -1;
        if (m_startPlayingOnReady[slot]) {
            m_startPlayingOnReady[slot] = false;
            player->play();
        }
        return;
    }

    player->setPosition(target);
}

// ── 재생 / 일시정지 ────────────────────────────────────────────
void VodPanel::togglePlayPause() {
    if (m_segments.isEmpty()) return;
    auto* activePlayer = m_players[m_activeSlot];
    if (activePlayer->playbackState() == QMediaPlayer::PlayingState) {
        activePlayer->pause();
    } else {
        if (activePlayer->source().isEmpty() || m_activeSegIdx < 0) {
            playAt(m_timeline->positionMs(), true);
        } else {
            activePlayer->play();
        }
    }
}

void VodPanel::updateControls() {
    const bool playing =
        m_players[m_activeSlot]->playbackState() == QMediaPlayer::PlayingState;
    m_playBtn->setText(playing ? "⏸  일시정지" : "▶  재생");
}


