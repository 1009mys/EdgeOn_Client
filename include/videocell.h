#pragma once
#include <QWidget>
#include <QLabel>
#include <QImage>
#include <QMutex>
#include <QPoint>
#include <QElapsedTimer>
#include <opencv2/core/cuda.hpp>
#include <memory>

class QDragEnterEvent;
class QDragMoveEvent;
class QDragLeaveEvent;
class QDropEvent;
class QPaintEngine;

class VideoCell : public QWidget {
    Q_OBJECT
public:
    explicit VideoCell(int cellId, QWidget* parent = nullptr);
    ~VideoCell() override;

    [[nodiscard]] int     cellId()   const { return m_cellId; }
    [[nodiscard]] bool    isActive() const { return m_active;  }
    [[nodiscard]] QString streamUrl() const { return m_url;   }

    void activate  (const QString& url);
    void deactivate();
    void updateFrame(const QImage& frame);
    void updateGpuFrame(const cv::cuda::GpuMat& frame);
    void setStatus  (const QString& status);
    QPaintEngine* paintEngine() const override;

signals:
    void addRequested   (int cellId, QString url);
    void moveRequested  (int fromCellId, int toCellId);
    void removeRequested(int cellId);

protected:
    void dragEnterEvent      (QDragEnterEvent*) override;
    void dragMoveEvent       (QDragMoveEvent*) override;
    void dragLeaveEvent      (QDragLeaveEvent*) override;
    void dropEvent           (QDropEvent*) override;
    void mousePressEvent      (QMouseEvent*)   override;
    void mouseMoveEvent       (QMouseEvent*)   override;
    void mouseDoubleClickEvent(QMouseEvent*)   override;
    void contextMenuEvent     (QContextMenuEvent*) override;
    void paintEvent           (QPaintEvent*)   override;
    void resizeEvent          (QResizeEvent*)  override;

private:
    struct RenderState;

    void showAddDialog();
    void onFramePresented();
    void refreshStatusLabel();
    void renderDirectX();
    void updateRenderBitmap();
    void discardRenderResources();

    int     m_cellId;
    bool    m_active{false};
    QString m_url;
    QImage  m_frame;
    QString m_baseStatus;
    QString m_fpsText;
    QElapsedTimer m_fpsTimer;
    int m_fpsFrameCount{0};
    cv::cuda::GpuMat m_gpuFrame;
    QMutex m_gpuFrameMutex;
    bool    m_bitmapDirty{false};
    QLabel* m_statusLabel;
    bool    m_dragHover{false};
    QPoint  m_dragStartPos;
    std::unique_ptr<RenderState> m_renderState;
};

