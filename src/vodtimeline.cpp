#include "vodtimeline.h"

#include <QPainter>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QDateTime>

// ── 상수 ──────────────────────────────────────────────────────
namespace {
constexpr qint64 kChunkMs   = 10'000LL;   ///< 버퍼 표시 단위 (10 초)
constexpr qint64 kMinViewMs = 30'000LL;   ///< 최소 줌 (30 초)

// 줌 레벨별 눈금 간격 계산
qint64 pickInterval(qint64 viewMs) {
    if (viewMs < 90'000LL)          return 10'000LL;
    if (viewMs < 450'000LL)         return 30'000LL;
    if (viewMs < 2'700'000LL)       return 120'000LL;
    if (viewMs < 10'800'000LL)      return 600'000LL;
    if (viewMs < 54'000'000LL)      return 3'600'000LL;
    return                                 21'600'000LL;
}

QString formatLabel(qint64 epochMs, qint64 intervalMs) {
    const QDateTime dt = QDateTime::fromMSecsSinceEpoch(epochMs);
    if (intervalMs < 60'000LL)          return dt.toString("HH:mm:ss");
    if (intervalMs < 3'600'000LL)       return dt.toString("HH:mm");
    // 3600000 ~ 21600000 : 시간 단위 (하루 미만) → 날짜+시간
    return dt.toString("MM/dd HH:mm");
}
} // namespace

// ── 생성자 ─────────────────────────────────────────────────────
VodTimeline::VodTimeline(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setMinimumHeight(70);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setCursor(Qt::PointingHandCursor);
}

QSize VodTimeline::sizeHint()        const { return {800, 90}; }
QSize VodTimeline::minimumSizeHint() const { return {200, 70}; }

// ── 데이터 업데이트 ────────────────────────────────────────────
void VodTimeline::setSegments(const QVector<VodSegment>& segments) {
    m_segments = segments;
    if (!m_segments.isEmpty()) {
        m_totalStartMs = m_segments.first().startMs;
        m_totalEndMs   = m_segments.last().endMs;
        const qint64 totalDur = m_totalEndMs - m_totalStartMs;
        m_viewDurationMs = qMax(kMinViewMs, totalDur);
        m_viewStartMs    = m_totalStartMs;
        m_posMs          = m_totalStartMs;
    } else {
        m_totalStartMs = m_totalEndMs = 0;
    }
    clampView();
    update();
}

void VodTimeline::setPosition(qint64 posMs) {
    m_posMs = posMs;
    // 재생 헤드가 뷰 밖으로 나가면 자동 스크롤
    const qint64 viewEnd = m_viewStartMs + m_viewDurationMs;
    if (posMs < m_viewStartMs || posMs > viewEnd) {
        m_viewStartMs = posMs - m_viewDurationMs / 4;
        clampView();
    }
    update();
}

void VodTimeline::clampView() {
    if (m_totalEndMs <= m_totalStartMs) return;
    const qint64 totalDur = m_totalEndMs - m_totalStartMs;
    m_viewDurationMs = qBound(kMinViewMs, m_viewDurationMs, totalDur);
    m_viewStartMs    = qBound(m_totalStartMs,
                              m_viewStartMs,
                              m_totalEndMs - m_viewDurationMs);
}

qint64 VodTimeline::xToMs(int x) const {
    if (width() <= 0) return m_viewStartMs;
    return m_viewStartMs + static_cast<qint64>(x) * m_viewDurationMs / width();
}

int VodTimeline::msToX(qint64 ms) const {
    if (m_viewDurationMs <= 0) return 0;
    return static_cast<int>((ms - m_viewStartMs) * width() / m_viewDurationMs);
}

// ── 그리기 ─────────────────────────────────────────────────────
void VodTimeline::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    const int W        = width();
    const int H        = height();
    const int barBot   = H * 60 / 100;     // 세그먼트 바 하단 경계
    const int trackY   = 6;
    const int trackH   = barBot - trackY - 4;

    // 배경
    p.fillRect(0, 0,      W, barBot,     QColor(28, 28, 42));
    p.fillRect(0, barBot, W, H - barBot, QColor(18, 18, 30));

    if (m_totalStartMs >= m_totalEndMs) {
        p.setPen(QColor(160, 160, 180));
        p.drawText(rect(), Qt::AlignCenter, "저장된 영상이 없습니다");
        return;
    }

    // 트랙 배경
    p.fillRect(0, trackY, W, trackH, QColor(45, 45, 65));

    // 세그먼트 + 10 초 버퍼 구분선
    for (const auto& seg : m_segments) {
        int x1 = msToX(seg.startMs);
        int x2 = msToX(seg.endMs);
        if (x2 < 0 || x1 > W) continue;
        x1 = qMax(0, x1);
        x2 = qMin(W, x2);
        if (x2 <= x1) continue;

        // 세그먼트 채우기
        p.fillRect(x1, trackY, x2 - x1, trackH, QColor(45, 110, 185));

        // 10 초 단위 구분선
        qint64 t = ((seg.startMs + kChunkMs) / kChunkMs) * kChunkMs;
        while (t < seg.endMs) {
            const int cx = msToX(t);
            if (cx > x1 && cx < x2)
                p.fillRect(cx, trackY, 1, trackH, QColor(70, 145, 220));
            t += kChunkMs;
        }
    }

    // 재생 헤드
    const int px = msToX(m_posMs);
    if (px >= 0 && px <= W) {
        p.setPen(QPen(QColor(255, 80, 80), 2));
        p.drawLine(px, 0, px, barBot + 2);

        // 현재 시각 말풍선
        QFont bf = font();
        bf.setPointSizeF(8.0);
        bf.setBold(true);
        p.setFont(bf);
        const QFontMetrics fm(bf);
        const QString posStr =
            QDateTime::fromMSecsSinceEpoch(m_posMs).toString("yyyy/MM/dd  HH:mm:ss");
        const int tw = fm.horizontalAdvance(posStr) + 10;
        int tx = px + 5;
        if (tx + tw > W) tx = px - tw - 5;
        tx = qMax(0, tx);
        p.fillRect(tx - 2, 1, tw + 4, fm.height() + 4, QColor(0, 0, 0, 170));
        p.setPen(Qt::white);
        p.drawText(tx, 2 + fm.ascent(), posStr);
    }

    // 시간 눈금 & 레이블
    const qint64 intervalMs = pickInterval(m_viewDurationMs);
    QFont lf = font();
    lf.setPointSizeF(7.5);
    p.setFont(lf);

    const qint64 viewEnd   = m_viewStartMs + m_viewDurationMs;
    const qint64 firstTick = (m_viewStartMs / intervalMs) * intervalMs;

    for (qint64 t = firstTick; t <= viewEnd + intervalMs; t += intervalMs) {
        const int x = msToX(t);
        if (x < -2 || x > W + 2) continue;

        p.setPen(QColor(100, 100, 135));
        p.drawLine(x, barBot, x, barBot + 5);

        p.setPen(QColor(165, 165, 195));
        const QString label = formatLabel(t, intervalMs);
        p.drawText(x + 2, barBot + 6, W - x - 2, H - barBot - 6,
                   Qt::AlignLeft | Qt::AlignTop, label);
    }
}

// ── 마우스 ─────────────────────────────────────────────────────
void VodTimeline::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        emit seekRequested(xToMs(event->pos().x()));
    }
}

void VodTimeline::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragging && (event->buttons() & Qt::LeftButton))
        emit seekRequested(xToMs(event->pos().x()));
}

void VodTimeline::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton)
        m_dragging = false;
}

// ── 휠 (Ctrl + 확대/축소) ──────────────────────────────────────
void VodTimeline::wheelEvent(QWheelEvent* event) {
    if (!(event->modifiers() & Qt::ControlModifier)) {
        event->ignore();
        return;
    }
    const double factor   = (event->angleDelta().y() > 0) ? 0.65 : 1.0 / 0.65;
    const qint64 cursorMs = xToMs(static_cast<int>(event->position().x()));
    const double ratio    = static_cast<double>(cursorMs - m_viewStartMs) / m_viewDurationMs;
    m_viewDurationMs      = static_cast<qint64>(m_viewDurationMs * factor);
    m_viewStartMs         = cursorMs - static_cast<qint64>(ratio * m_viewDurationMs);
    clampView();
    update();
    event->accept();
}


