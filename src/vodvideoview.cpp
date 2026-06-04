#include "vodvideoview.h"

#include <QPainter>
#include <QPen>
#include <QVideoFrame>
#include <QVideoSink>

namespace {

constexpr QColor kPalette[] = {
    QColor(0x4F, 0xC3, 0xF7),
    QColor(0xFF, 0xB3, 0x00),
    QColor(0x81, 0xC7, 0x84),
    QColor(0xE5, 0x73, 0x73),
    QColor(0xBA, 0x68, 0xC8),
    QColor(0xFF, 0x8A, 0x65),
    QColor(0x64, 0xB5, 0xF6),
    QColor(0xFF, 0xEE, 0x58)
};

} // namespace

VodVideoView::VodVideoView(QWidget* parent)
    : QWidget(parent)
{
    setStyleSheet("background: black;");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    m_sink = new QVideoSink(this);
    // QueuedConnection → 메인 스레드에서 안전하게 프레임 수신
    connect(m_sink, &QVideoSink::videoFrameChanged,
            this,   &VodVideoView::onVideoFrameChanged, Qt::QueuedConnection);
}

QVideoSink* VodVideoView::videoSink() const
{
    return m_sink;
}

void VodVideoView::setDetections(int frameW, int frameH,
                                  const std::vector<detection_result>& dets)
{
    m_detFrameW  = frameW;
    m_detFrameH  = frameH;
    m_detections = dets;
    update();
}

void VodVideoView::clearDetections()
{
    m_detections.clear();
    update();
}

void VodVideoView::onVideoFrameChanged(const QVideoFrame& frame)
{
    m_latestFrame = frame;

    qint64 timestampUs = frame.startTime();
    if (timestampUs < 0) {
        timestampUs = frame.endTime();
    }

    if (timestampUs != m_latestFrameTimestampUs) {
        m_latestFrameTimestampUs = timestampUs;
        emit videoFrameTimestampChanged(m_latestFrameTimestampUs);
    }

    update();
}

// ── 내부 유틸 ──────────────────────────────────────────────────
QColor VodVideoView::colorForClassId(int classId)
{
    constexpr int kSize = static_cast<int>(sizeof(kPalette) / sizeof(kPalette[0]));
    const int idx = classId >= 0 ? (classId % kSize) : 0;
    return kPalette[idx];
}

QRectF VodVideoView::computeAspectFitRect(const QSize& src, const QSize& dst)
{
    if (src.isEmpty() || dst.isEmpty()) return {};
    const qreal scale = qMin(
        static_cast<qreal>(dst.width())  / src.width(),
        static_cast<qreal>(dst.height()) / src.height());
    const qreal w = src.width()  * scale;
    const qreal h = src.height() * scale;
    return QRectF((dst.width()  - w) * 0.5,
                  (dst.height() - h) * 0.5,
                  w, h);
}

// ── paintEvent ─────────────────────────────────────────────────
void VodVideoView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);

    QRectF videoRect;

    if (m_latestFrame.isValid()) {
        // QVideoFrame → QImage (GPU→CPU readback 포함)
        const QImage img = m_latestFrame.toImage();
        if (!img.isNull()) {
            videoRect = computeAspectFitRect(img.size(), size());
            p.drawImage(videoRect, img);
        }
    }

    // BBox 렌더링 — 비디오 aspect-fit 영역 기준
    if (!m_detections.empty() && m_detFrameW > 0 && m_detFrameH > 0) {
        // 비디오 프레임이 아직 없으면 위젯 전체를 videoRect로 사용
        if (videoRect.isEmpty()) {
            videoRect = computeAspectFitRect(
                QSize(m_detFrameW, m_detFrameH), size());
        }
        if (!videoRect.isEmpty()) {
            const qreal scaleX = videoRect.width()  / static_cast<qreal>(m_detFrameW);
            const qreal scaleY = videoRect.height() / static_cast<qreal>(m_detFrameH);

            p.setRenderHint(QPainter::Antialiasing, false);
            for (const auto& det : m_detections) {
                QRectF box(videoRect.left() + det.box_x * scaleX,
                           videoRect.top()  + det.box_y * scaleY,
                           det.box_width  * scaleX,
                           det.box_height * scaleY);
                box = box.intersected(videoRect);
                if (box.width() <= 1.0 || box.height() <= 1.0) continue;
                p.setPen(QPen(colorForClassId(det.class_id), 2));
                p.drawRect(box);
            }
        }
    }
}

