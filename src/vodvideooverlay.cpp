#include "vodvideooverlay.h"

#include <QEvent>
#include <QPainter>
#include <QPen>
#include <QResizeEvent>

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
}

VodVideoOverlay::VodVideoOverlay(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAutoFillBackground(false);

    // 부모(QVideoWidget) resize/show 이벤트를 감시해 오버레이 크기를 동기화한다.
    if (parent) {
        parent->installEventFilter(this);
        setGeometry(parent->rect());
    }
}

bool VodVideoOverlay::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == parentWidget()) {
        if (event->type() == QEvent::Resize) {
            setGeometry(parentWidget()->rect());
            raise();
        } else if (event->type() == QEvent::Show) {
            raise();
        }
    }
    return QWidget::eventFilter(obj, event);
}

void VodVideoOverlay::setFrameDetections(int frameWidth,
                                         int frameHeight,
                                         const std::vector<detection_result>& detections)
{
    m_frameWidth = frameWidth;
    m_frameHeight = frameHeight;
    m_detections = detections;
    update();
}

void VodVideoOverlay::clear()
{
    m_frameWidth = 0;
    m_frameHeight = 0;
    m_detections.clear();
    update();
}

QColor VodVideoOverlay::colorForClassId(int classId) const
{
    constexpr int kPaletteSize = static_cast<int>(sizeof(kPalette) / sizeof(kPalette[0]));
    const int index = classId >= 0 ? (classId % kPaletteSize) : 0;
    return kPalette[index];
}

QRectF VodVideoOverlay::computeAspectFitRect() const
{
    if (m_frameWidth <= 0 || m_frameHeight <= 0 || width() <= 0 || height() <= 0) {
        return {};
    }

    const qreal srcW = static_cast<qreal>(m_frameWidth);
    const qreal srcH = static_cast<qreal>(m_frameHeight);
    const qreal dstW = static_cast<qreal>(width());
    const qreal dstH = static_cast<qreal>(height());
    const qreal scale = qMin(dstW / srcW, dstH / srcH);

    const qreal w = srcW * scale;
    const qreal h = srcH * scale;
    const qreal x = (dstW - w) * 0.5;
    const qreal y = (dstH - h) * 0.5;
    return QRectF(x, y, w, h);
}

void VodVideoOverlay::paintEvent(QPaintEvent*)
{
    if (m_frameWidth <= 0 || m_frameHeight <= 0 || m_detections.empty()) {
        return;
    }

    const QRectF videoRect = computeAspectFitRect();
    if (videoRect.isEmpty()) {
        return;
    }

    const qreal scaleX = videoRect.width() / static_cast<qreal>(m_frameWidth);
    const qreal scaleY = videoRect.height() / static_cast<qreal>(m_frameHeight);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    for (const auto& det : m_detections) {
        QRectF box(videoRect.left() + static_cast<qreal>(det.box_x) * scaleX,
                   videoRect.top() + static_cast<qreal>(det.box_y) * scaleY,
                   static_cast<qreal>(det.box_width) * scaleX,
                   static_cast<qreal>(det.box_height) * scaleY);

        box = box.intersected(videoRect);
        if (box.width() <= 1.0 || box.height() <= 1.0) {
            continue;
        }

        painter.setPen(QPen(colorForClassId(det.class_id), 2));
        painter.drawRect(box);
    }
}

