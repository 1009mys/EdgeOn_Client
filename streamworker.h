#pragma once
#include <QThread>
#include <QImage>
#include <QMutex>
#include <opencv2/core/cuda.hpp>
#include <atomic>

class StreamWorker : public QThread {
    Q_OBJECT
public:
    explicit StreamWorker(int cellId, const QString& url, QObject* parent = nullptr);
    void stop();
    bool takeLatestFrame(QImage& outFrame);
    bool takeLatestGpuFrame(cv::cuda::GpuMat& outFrame);

    [[nodiscard]] int     cellId() const { return m_cellId; }
    [[nodiscard]] QString url()    const { return m_url;    }

signals:
    void frameReady   (int cellId);
    void statusChanged(int cellId, QString status);

protected:
    void run() override;

private:
    int                  m_cellId;
    QString              m_url;
    std::atomic<bool>    m_running{true};
    std::atomic<bool>    m_notifyPending{false};
    QMutex               m_frameMutex;
    QImage               m_latestFrame;
    cv::cuda::GpuMat     m_latestGpuFrame;
};

