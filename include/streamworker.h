#pragma once
#include <QThread>
#include <QImage>
#include <QMutex>
#include <QString>
#include <QtGlobal>
#include <opencv2/core/cuda.hpp>
#include <atomic>

class StreamWorker : public QThread {
    Q_OBJECT
public:
    explicit StreamWorker(int cameraId, const QString& url, QObject* parent = nullptr);
    void stop();
    void setAnalysisBusy(bool busy);
    bool takeLatestFrame(QImage& outFrame, qint64& outFrameUtcMs, qint64& outFrameSeq, cv::Size& outFrameSize);
    bool takeLatestGpuFrame(cv::cuda::GpuMat& outFrame, qint64& outFrameUtcMs, qint64& outFrameSeq, cv::Size& outFrameSize);

    [[nodiscard]] int     cameraId() const { return m_cameraId; }
    [[nodiscard]] QString url()    const { return m_url;    }

signals:
    void frameReady   (int cameraId);
    void statusChanged(int cameraId, QString status);

protected:
    void run() override;

private:
    int                  m_cameraId;
    QString              m_url;
    std::atomic<bool>    m_running{true};
    std::atomic<bool>    m_notifyPending{false};
    std::atomic<bool>    m_analysisBusy{false};
    std::atomic<qint64>   m_frameSequence{0};
    QMutex               m_frameMutex;
    QImage               m_latestFrame;
    cv::cuda::GpuMat     m_latestGpuFrame;
    qint64               m_latestFrameUtcMs{0};
    qint64               m_latestFrameSeq{0};
    cv::Size             m_latestFrameSize{};
};

