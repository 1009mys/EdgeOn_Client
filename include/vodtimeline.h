#pragma once
#include <QWidget>
#include <QVector>
#include <QString>

struct VodSegment {
    qint64  startMs{0};   ///< ms since epoch (local)
    qint64  endMs{0};
    QString filePath;
};

/**
 * 저장영상 타임라인 바.
 * - 녹화 세그먼트를 파란 블록으로 표시
 * - 10 초 단위 버퍼 구분선
 * - 날짜/시간/분/초 레이블 (줌 레벨에 따라 자동 조정)
 * - Ctrl + 휠로 표시 범위 확대/축소
 * - 좌클릭 / 드래그로 탐색
 */
class VodTimeline : public QWidget {
    Q_OBJECT
public:
    explicit VodTimeline(QWidget* parent = nullptr);

    void setSegments(const QVector<VodSegment>& segments);
    void setPosition(qint64 posMs);

    [[nodiscard]] qint64 positionMs()   const { return m_posMs; }
    [[nodiscard]] qint64 totalStartMs() const { return m_totalStartMs; }
    [[nodiscard]] qint64 totalEndMs()   const { return m_totalEndMs; }

    QSize sizeHint()        const override;
    QSize minimumSizeHint() const override;

signals:
    void seekRequested(qint64 posMs);

protected:
    void paintEvent(QPaintEvent* event)       override;
    void mousePressEvent(QMouseEvent* event)  override;
    void mouseMoveEvent(QMouseEvent* event)   override;
    void mouseReleaseEvent(QMouseEvent* event)override;
    void wheelEvent(QWheelEvent* event)       override;

private:
    qint64 xToMs(int x)    const;
    int    msToX(qint64 ms)const;
    void   clampView();

    QVector<VodSegment> m_segments;
    qint64 m_posMs{0};
    qint64 m_viewStartMs{0};
    qint64 m_viewDurationMs{3'600'000LL};   ///< visible time window (ms)
    qint64 m_totalStartMs{0};
    qint64 m_totalEndMs{0};
    bool   m_dragging{false};
};

