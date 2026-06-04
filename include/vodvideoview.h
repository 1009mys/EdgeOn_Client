#pragma once
#include <QVideoFrame>
#include <QWidget>

#include <vector>

#include "Protocol.h"

class QVideoSink;

/**
 * VOD 비디오 + BBox 오버레이 합성 위젯.
 *
 * QVideoWidget 대신 QVideoSink 로 프레임을 수신하고,
 * paintEvent 에서 비디오 + BBox 를 하나의 QPainter 패스로 렌더링한다.
 * Windows D3D11 스왑체인이 Qt 자식 위젯을 덮어쓰는 문제를 우회한다.
 */
class VodVideoView : public QWidget {
    Q_OBJECT
public:
    explicit VodVideoView(QWidget* parent = nullptr);

    [[nodiscard]] QVideoSink* videoSink() const;

    void setDetections(int frameW, int frameH,
                       const std::vector<detection_result>& dets);
    void clearDetections();
    [[nodiscard]] qint64 latestFrameTimestampUs() const { return m_latestFrameTimestampUs; }

signals:
    void videoFrameTimestampChanged(qint64 timestampUs);

protected:
    void paintEvent(QPaintEvent* event) override;

private slots:
    void onVideoFrameChanged(const QVideoFrame& frame);

private:
    static QColor    colorForClassId(int classId);
    static QRectF    computeAspectFitRect(const QSize& src, const QSize& dst);

    QVideoSink*                    m_sink{nullptr};
    QVideoFrame                    m_latestFrame;
    qint64                         m_latestFrameTimestampUs{-1};
    int                            m_detFrameW{0};
    int                            m_detFrameH{0};
    std::vector<detection_result>  m_detections;
};


