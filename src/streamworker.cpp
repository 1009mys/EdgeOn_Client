#include "streamworker.h"
#include <opencv2/opencv.hpp>
#include <opencv2/cudacodec.hpp>
#include <opencv2/core/cuda.hpp>
#include <opencv2/cudaimgproc.hpp>
#include <QMutexLocker>
#include <QtGlobal>
#include <iostream>
#include <vector>

namespace {

void configureRtspTransportForFfmpeg() {
    static const bool configured = [] {
        qputenv("OPENCV_FFMPEG_CAPTURE_OPTIONS", "rtsp_transport;tcp");
        return true;
    }();
    (void)configured;
}

cv::Ptr<cv::cudacodec::VideoReader> openWithNvdec(const std::string& url, int timeoutMs) {
    configureRtspTransportForFfmpeg();

    cv::cudacodec::VideoReaderInitParams params;
    params.allowFrameDrop = true;

    const std::vector<std::vector<int>> sourceParamAttempts{
        {
            cv::CAP_PROP_OPEN_TIMEOUT_MSEC, timeoutMs
        },
        {}
    };

    cv::Exception lastException;
    bool hasException = false;

    const bool udpSourceAttempts[] = {true, false};
    for (const bool udpSource : udpSourceAttempts) {
        params.udpSource = udpSource;
        for (const auto& sourceParams : sourceParamAttempts) {
            try {
                auto reader = cv::cudacodec::createVideoReader(url, sourceParams, params);
                if (!reader.empty()) {
                    reader->set(cv::cudacodec::BGRA);
                    std::cerr << "NVDEC open success (udpSource=" << (udpSource ? "true" : "false")
                              << ")" << std::endl;
                    return reader;
                }
            } catch (const cv::Exception& exception) {
                lastException = exception;
                hasException = true;
                std::cerr << "NVDEC open attempt failed (udpSource=" << (udpSource ? "true" : "false")
                          << "): " << exception.what() << std::endl;
            }
        }
    }

    if (hasException)
        throw lastException;

    return {};
}

bool openWithFallback(cv::VideoCapture& cap, const std::string& url,
                      int openTimeoutMs, int readTimeoutMs) {
    configureRtspTransportForFfmpeg();

    cap.set(cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY);
    cap.set(cv::CAP_PROP_OPEN_TIMEOUT_MSEC, openTimeoutMs);
    cap.set(cv::CAP_PROP_READ_TIMEOUT_MSEC, readTimeoutMs);
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
    if (cap.open(url, cv::CAP_FFMPEG)) return true;

    return cap.open(url);
}

} // namespace

StreamWorker::StreamWorker(int cameraId, const QString& url, QObject* parent)
    : QThread(parent), m_cameraId(cameraId), m_url(url) {}

void StreamWorker::stop() {
    m_running = false;
    requestInterruption();
}

bool StreamWorker::takeLatestFrame(QImage& outFrame) {
    QMutexLocker lock(&m_frameMutex);
    if (m_latestFrame.isNull()) return false;
    outFrame = m_latestFrame;
    m_latestFrame = QImage();
    m_notifyPending.store(false, std::memory_order_release);
    return true;
}

bool StreamWorker::takeLatestGpuFrame(cv::cuda::GpuMat& outFrame) {
    QMutexLocker lock(&m_frameMutex);
    if (m_latestGpuFrame.empty()) return false;
    outFrame = m_latestGpuFrame;
    m_latestGpuFrame.release();
    m_notifyPending.store(false, std::memory_order_release);
    return true;
}

void StreamWorker::run() {
    constexpr int OPEN_TIMEOUT_MS    = 12000;
    constexpr int READ_TIMEOUT_MS    = 5000;
    constexpr int RECONNECT_DELAY_MS = 3000;

    const auto publishFrame = [this](const cv::Mat& frame) {
        cv::Mat bgraFrame;
        const cv::Mat* imageSource = &frame;

        if (frame.type() != CV_8UC4) {
            cv::cvtColor(frame, bgraFrame, cv::COLOR_BGR2BGRA);
            imageSource = &bgraFrame;
        }

        QImage img(imageSource->data, imageSource->cols, imageSource->rows,
                   static_cast<int>(imageSource->step),
                   QImage::Format_ARGB32);
        {
            QMutexLocker lock(&m_frameMutex);
            m_latestGpuFrame.release();
            m_latestFrame = img.copy();
        }

        // UI 이벤트 큐에는 셀당 프레임 알림 1개만 유지하고, 나머지는 최신 프레임으로 덮어쓴다.
        if (!m_notifyPending.exchange(true, std::memory_order_acq_rel))
            emit frameReady(m_cameraId);
    };

    const auto publishGpuFrame = [this](const cv::cuda::GpuMat& bgraFrame) {
        if (bgraFrame.empty() || bgraFrame.type() != CV_8UC4)
            return;

        {
            QMutexLocker lock(&m_frameMutex);
            m_latestFrame = QImage();
            // 최신 프레임만 유지하므로 deep copy 대신 헤더 공유로 복제 비용을 줄인다.
            m_latestGpuFrame = bgraFrame;
        }

        if (!m_notifyPending.exchange(true, std::memory_order_acq_rel))
            emit frameReady(m_cameraId);
    };

    while (!isInterruptionRequested()) {
        emit statusChanged(m_cameraId, "연결 중...");

        const std::string url = m_url.toStdString();
        bool usingNvdec = false;

        emit statusChanged(m_cameraId, "NVDEC 연결 시도...");
        try {
            auto gpuReader = openWithNvdec(url, OPEN_TIMEOUT_MS);
            if (!gpuReader.empty()) {
                usingNvdec = true;
                emit statusChanged(m_cameraId, m_url + " [NVDEC]");

                cv::cuda::GpuMat gpuFrame;
                cv::cuda::GpuMat gpuBgra;
                cv::cuda::Stream cudaStream;
                while (!isInterruptionRequested()) {
                    if (!gpuReader->nextFrame(gpuFrame) || gpuFrame.empty()) {
                        emit statusChanged(m_cameraId, "스트림 끊김 – 재연결...");
                        break;
                    }

                    cv::cuda::GpuMat* gpuOutput = &gpuFrame;
                    bool usedCudaCvtColor = false;
                    if (gpuFrame.type() == CV_8UC3) {
                        cv::cuda::cvtColor(gpuFrame, gpuBgra, cv::COLOR_BGR2BGRA, 0, cudaStream);
                        gpuOutput = &gpuBgra;
                        usedCudaCvtColor = true;
                    } else if (gpuFrame.type() != CV_8UC4) {
                        emit statusChanged(m_cameraId, "지원하지 않는 GPU 포맷 – 재연결...");
                        break;
                    }

                    if (usedCudaCvtColor)
                        cudaStream.waitForCompletion();
                    publishGpuFrame(*gpuOutput);
                }
            }
        } catch (const cv::Exception& exception) {
            std::cerr << "\nERROR: " << exception.what() << std::endl;
            usingNvdec = false;
        }

        if (!usingNvdec && !isInterruptionRequested()) {
            cv::VideoCapture cap;
            emit statusChanged(m_cameraId, "NVDEC 실패 - 일반 디코더 재시도...");
            if (!openWithFallback(cap, url, OPEN_TIMEOUT_MS, READ_TIMEOUT_MS)) {
                emit statusChanged(m_cameraId, "연결 실패 – 재시도 중...");
            } else {
                emit statusChanged(m_cameraId, m_url);

                cv::Mat frame;
                while (!isInterruptionRequested()) {
                    if (!cap.read(frame) || frame.empty()) {
                        emit statusChanged(m_cameraId, "스트림 끊김 – 재연결...");
                        break;
                    }

                    publishFrame(frame);
                }
            }

            cap.release();
        }

        // Wait before reconnect
        for (int i = 0; i < (RECONNECT_DELAY_MS / 100) && !isInterruptionRequested(); ++i)
            msleep(100);
    }
}

