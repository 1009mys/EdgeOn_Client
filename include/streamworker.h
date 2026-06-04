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
    explicit StreamWorker(int cameraId,
                          const QString& url,
                          const QString& outputRoot,
                          int segmentSeconds,
                          QObject* parent = nullptr);
    void stop();
    void setAnalysisBusy(bool busy);
    void setRecordingEnabled(bool enabled);
    bool takeLatestFrame(QImage& outFrame,
                         qint64& outFrameUtcMs,
                         qint64& outFrameSeq,
                         qint64& outCaptureUtcMs,
                         qint64& outSourcePts,
                         int& outSourceTimeBaseNum,
                         int& outSourceTimeBaseDen,
                         cv::Size& outFrameSize);
    bool takeLatestGpuFrame(cv::cuda::GpuMat& outFrame,
                            qint64& outFrameUtcMs,
                            qint64& outFrameSeq,
                            qint64& outCaptureUtcMs,
                            qint64& outSourcePts,
                            int& outSourceTimeBaseNum,
                            int& outSourceTimeBaseDen,
                            cv::Size& outFrameSize);

    [[nodiscard]] int     cameraId() const { return m_cameraId; }
    [[nodiscard]] QString url()    const { return m_url;    }

signals:
    void frameReady   (int cameraId);
    void statusChanged(int cameraId, QString status);
    void segmentStarted(int cameraId,
                        qint64 startUtcMs,
                        int segmentSeconds,
                        QString tempPath,
                        qint64 startSourcePts,
                        int sourceTimeBaseNum,
                        int sourceTimeBaseDen);
    void segmentFinished(int cameraId, qint64 startUtcMs, qint64 endUtcMs, QString finalPath);

protected:
    void run() override;

private:
    int                  m_cameraId;
    QString              m_url;
    QString              m_outputRoot;
    int                  m_segmentSeconds{30};
    std::atomic<bool>    m_running{true};
    std::atomic<bool>    m_notifyPending{false};
    std::atomic<bool>    m_analysisBusy{false};
    std::atomic<bool>    m_recordingEnabled{false};
    std::atomic<qint64>   m_frameSequence{0};
    QMutex               m_frameMutex;
    QImage               m_latestFrame;
    cv::cuda::GpuMat     m_latestGpuFrame;
    qint64               m_latestFrameUtcMs{0};
    qint64               m_latestFrameSeq{0};
    qint64               m_latestCaptureUtcMs{0};
    qint64               m_latestSourcePts{0};
    int                  m_latestSourceTimeBaseNum{0};
    int                  m_latestSourceTimeBaseDen{1};
    cv::Size             m_latestFrameSize{};
};

