#include "streamworker.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QVector>

#include <opencv2/core/cuda.hpp>
#include <opencv2/cudaimgproc.hpp>
#include <opencv2/imgproc.hpp>

#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavutil/timestamp.h>
#include <libswscale/swscale.h>
}

namespace {

constexpr int kOpenTimeoutUs = 12 * 1000 * 1000;
constexpr int kReadTimeoutUs = 5 * 1000 * 1000;
constexpr int kReconnectDelayMs = 3000;

QString ffErr(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}

QString makeRecordingTempPath(const QString& outputRoot, int cameraId, const QString& stamp)
{
    const QString root = outputRoot.trimmed().isEmpty() ? QStringLiteral("E:/EdgeOn") : outputRoot.trimmed();
    const QString cameraDirPath = QDir::cleanPath(root + QString("/camera_%1").arg(cameraId));
    QDir cameraDir(cameraDirPath);
    if (!cameraDir.exists()) {
        cameraDir.mkpath(".");
    }
    return cameraDir.filePath(QString("camera_%1_%2_RECORDING.mkv").arg(cameraId).arg(stamp));
}

QString finalizeRecordingPath(const QString& tempPath, int cameraId, const QString& startStamp)
{
    const QFileInfo info(tempPath);
    const QString endStamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    const QString finalName = QString("camera_%1_%2_%3.mkv").arg(cameraId).arg(startStamp).arg(endStamp);
    const QString finalPath = QDir(info.absolutePath()).filePath(finalName);
    if (!QFileInfo::exists(tempPath)) {
        return finalPath;
    }
    if (QFile::rename(tempPath, finalPath)) {
        return finalPath;
    }
    return tempPath;
}

} // namespace

StreamWorker::StreamWorker(int cameraId,
                           const QString& url,
                           const QString& outputRoot,
                           int segmentSeconds,
                           QObject* parent)
    : QThread(parent),
      m_cameraId(cameraId),
      m_url(url.trimmed()),
      m_outputRoot(outputRoot.trimmed()),
      m_segmentSeconds(qMax(1, segmentSeconds))
{
}

void StreamWorker::stop()
{
    m_running = false;
    requestInterruption();
}

void StreamWorker::setAnalysisBusy(bool busy)
{
    m_analysisBusy.store(busy, std::memory_order_release);
}

void StreamWorker::setRecordingEnabled(bool enabled)
{
    m_recordingEnabled.store(enabled, std::memory_order_release);
}

bool StreamWorker::takeLatestFrame(QImage& outFrame,
                                   qint64& outFrameUtcMs,
                                   qint64& outFrameSeq,
                                   qint64& outCaptureUtcMs,
                                   qint64& outSourcePts,
                                   int& outSourceTimeBaseNum,
                                   int& outSourceTimeBaseDen,
                                   cv::Size& outFrameSize)
{
    QMutexLocker lock(&m_frameMutex);
    if (m_latestFrame.isNull()) return false;
    outFrame = m_latestFrame;
    outFrameUtcMs = m_latestFrameUtcMs;
    outFrameSeq = m_latestFrameSeq;
    outCaptureUtcMs = m_latestCaptureUtcMs;
    outSourcePts = m_latestSourcePts;
    outSourceTimeBaseNum = m_latestSourceTimeBaseNum;
    outSourceTimeBaseDen = m_latestSourceTimeBaseDen;
    outFrameSize = m_latestFrameSize;
    m_latestFrame = QImage();
    m_latestFrameUtcMs = 0;
    m_latestFrameSeq = 0;
    m_latestCaptureUtcMs = 0;
    m_latestSourcePts = 0;
    m_latestSourceTimeBaseNum = 0;
    m_latestSourceTimeBaseDen = 1;
    m_latestFrameSize = {};
    m_notifyPending.store(false, std::memory_order_release);
    return true;
}

bool StreamWorker::takeLatestGpuFrame(cv::cuda::GpuMat& outFrame,
                                      qint64& outFrameUtcMs,
                                      qint64& outFrameSeq,
                                      qint64& outCaptureUtcMs,
                                      qint64& outSourcePts,
                                      int& outSourceTimeBaseNum,
                                      int& outSourceTimeBaseDen,
                                      cv::Size& outFrameSize)
{
    QMutexLocker lock(&m_frameMutex);
    if (m_latestGpuFrame.empty()) return false;
    outFrame = m_latestGpuFrame;
    outFrameUtcMs = m_latestFrameUtcMs;
    outFrameSeq = m_latestFrameSeq;
    outCaptureUtcMs = m_latestCaptureUtcMs;
    outSourcePts = m_latestSourcePts;
    outSourceTimeBaseNum = m_latestSourceTimeBaseNum;
    outSourceTimeBaseDen = m_latestSourceTimeBaseDen;
    outFrameSize = m_latestFrameSize;
    m_latestGpuFrame.release();
    m_latestFrameUtcMs = 0;
    m_latestFrameSeq = 0;
    m_latestCaptureUtcMs = 0;
    m_latestSourcePts = 0;
    m_latestSourceTimeBaseNum = 0;
    m_latestSourceTimeBaseDen = 1;
    m_latestFrameSize = {};
    m_notifyPending.store(false, std::memory_order_release);
    return true;
}

void StreamWorker::run()
{
    avformat_network_init();

    while (m_running.load(std::memory_order_acquire) && !isInterruptionRequested()) {
        emit statusChanged(m_cameraId, "FFmpeg RTSP 연결 중...");

        AVFormatContext* inFmt = nullptr;
        AVDictionary* options = nullptr;
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        av_dict_set(&options, "stimeout", QByteArray::number(kOpenTimeoutUs).constData(), 0);
        av_dict_set(&options, "rw_timeout", QByteArray::number(kReadTimeoutUs).constData(), 0);

        const std::string urlStd = m_url.toStdString();
        int err = avformat_open_input(&inFmt, urlStd.c_str(), nullptr, &options);
        av_dict_free(&options);
        if (err < 0) {
            emit statusChanged(m_cameraId, "RTSP 연결 실패: " + ffErr(err));
            for (int i = 0; i < (kReconnectDelayMs / 100) && !isInterruptionRequested(); ++i) {
                msleep(100);
            }
            continue;
        }

        err = avformat_find_stream_info(inFmt, nullptr);
        if (err < 0) {
            emit statusChanged(m_cameraId, "스트림 정보 조회 실패: " + ffErr(err));
            avformat_close_input(&inFmt);
            for (int i = 0; i < (kReconnectDelayMs / 100) && !isInterruptionRequested(); ++i) {
                msleep(100);
            }
            continue;
        }

        int videoStreamIndex = -1;
        for (unsigned int i = 0; i < inFmt->nb_streams; ++i) {
            if (inFmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                videoStreamIndex = static_cast<int>(i);
                break;
            }
        }
        if (videoStreamIndex < 0) {
            emit statusChanged(m_cameraId, "비디오 스트림이 없습니다.");
            avformat_close_input(&inFmt);
            for (int i = 0; i < (kReconnectDelayMs / 100) && !isInterruptionRequested(); ++i) {
                msleep(100);
            }
            continue;
        }

        AVStream* videoStream = inFmt->streams[videoStreamIndex];
        const AVCodec* decoder = avcodec_find_decoder(videoStream->codecpar->codec_id);
        if (!decoder) {
            emit statusChanged(m_cameraId, "비디오 디코더를 찾지 못했습니다.");
            avformat_close_input(&inFmt);
            for (int i = 0; i < (kReconnectDelayMs / 100) && !isInterruptionRequested(); ++i) {
                msleep(100);
            }
            continue;
        }

        AVCodecContext* decCtx = avcodec_alloc_context3(decoder);
        if (!decCtx) {
            emit statusChanged(m_cameraId, "디코더 컨텍스트 할당 실패");
            avformat_close_input(&inFmt);
            for (int i = 0; i < (kReconnectDelayMs / 100) && !isInterruptionRequested(); ++i) {
                msleep(100);
            }
            continue;
        }
        avcodec_parameters_to_context(decCtx, videoStream->codecpar);

        // NVDEC 우선, 실패 시 SW decode.
        av_opt_set(decCtx->priv_data, "hwaccel", "cuda", 0);
        err = avcodec_open2(decCtx, decoder, nullptr);
        if (err < 0) {
            avcodec_free_context(&decCtx);
            decCtx = avcodec_alloc_context3(decoder);
            if (!decCtx) {
                emit statusChanged(m_cameraId, "디코더 컨텍스트 재할당 실패");
                avformat_close_input(&inFmt);
                for (int i = 0; i < (kReconnectDelayMs / 100) && !isInterruptionRequested(); ++i) {
                    msleep(100);
                }
                continue;
            }
            avcodec_parameters_to_context(decCtx, videoStream->codecpar);
            err = avcodec_open2(decCtx, decoder, nullptr);
            if (err < 0) {
                emit statusChanged(m_cameraId, "디코더 오픈 실패: " + ffErr(err));
                avcodec_free_context(&decCtx);
                avformat_close_input(&inFmt);
                for (int i = 0; i < (kReconnectDelayMs / 100) && !isInterruptionRequested(); ++i) {
                    msleep(100);
                }
                continue;
            }
            emit statusChanged(m_cameraId, m_url + " [FFmpeg SW decode]");
        } else {
            emit statusChanged(m_cameraId, m_url + " [FFmpeg NVDEC]");
        }

        AVFrame* frame = av_frame_alloc();
        AVPacket* packet = av_packet_alloc();
        AVFormatContext* outFmt = nullptr;
        QVector<int> outStreamMap;
        qint64 segmentStartPts = AV_NOPTS_VALUE;
        qint64 segmentStartUtcMs = 0;
        QString segmentStartStamp;
        QString segmentTempPath;
        bool segmentStartNotified = false;

        qint64 captureAnchorPts = AV_NOPTS_VALUE;
        qint64 captureAnchorUtcMs = 0;

        auto closeSegment = [&](bool emitFinished) {
            if (!outFmt) {
                return;
            }
            av_write_trailer(outFmt);
            if (!(outFmt->oformat->flags & AVFMT_NOFILE) && outFmt->pb) {
                avio_closep(&outFmt->pb);
            }
            avformat_free_context(outFmt);
            outFmt = nullptr;
            if (emitFinished && !segmentTempPath.isEmpty()) {
                const QString finalPath = finalizeRecordingPath(segmentTempPath, m_cameraId, segmentStartStamp);
                const qint64 endUtcMs = QDateTime::currentMSecsSinceEpoch();
                emit segmentFinished(m_cameraId, segmentStartUtcMs, endUtcMs, finalPath);
            }
            segmentStartPts = AV_NOPTS_VALUE;
            segmentStartUtcMs = 0;
            segmentStartStamp.clear();
            segmentTempPath.clear();
            segmentStartNotified = false;
            outStreamMap.clear();
        };

        auto openSegment = [&]() -> bool {
            const QDateTime startDt = QDateTime::currentDateTime();
            segmentStartUtcMs = startDt.toMSecsSinceEpoch();
            segmentStartStamp = startDt.toString("yyyyMMdd_HHmmss");
            segmentTempPath = makeRecordingTempPath(m_outputRoot, m_cameraId, segmentStartStamp);

            int localErr = avformat_alloc_output_context2(&outFmt, nullptr, "matroska", segmentTempPath.toStdString().c_str());
            if (localErr < 0 || !outFmt) {
                emit statusChanged(m_cameraId, "세그먼트 생성 실패: " + ffErr(localErr));
                outFmt = nullptr;
                return false;
            }

            outStreamMap = QVector<int>(static_cast<int>(inFmt->nb_streams), -1);
            for (unsigned int i = 0; i < inFmt->nb_streams; ++i) {
                AVStream* inStream = inFmt->streams[i];
                AVStream* outStream = avformat_new_stream(outFmt, nullptr);
                if (!outStream) {
                    emit statusChanged(m_cameraId, "출력 스트림 생성 실패");
                    closeSegment(false);
                    return false;
                }
                outStreamMap[static_cast<int>(i)] = outStream->index;
                avcodec_parameters_copy(outStream->codecpar, inStream->codecpar);
                outStream->codecpar->codec_tag = 0;
                outStream->time_base = inStream->time_base;
            }

            if (!(outFmt->oformat->flags & AVFMT_NOFILE)) {
                localErr = avio_open(&outFmt->pb, segmentTempPath.toStdString().c_str(), AVIO_FLAG_WRITE);
                if (localErr < 0) {
                    emit statusChanged(m_cameraId, "세그먼트 파일 오픈 실패: " + ffErr(localErr));
                    closeSegment(false);
                    return false;
                }
            }

            localErr = avformat_write_header(outFmt, nullptr);
            if (localErr < 0) {
                emit statusChanged(m_cameraId, "세그먼트 헤더 쓰기 실패: " + ffErr(localErr));
                closeSegment(false);
                return false;
            }

            emit statusChanged(m_cameraId, QString("녹화 중 (FFmpeg remux, %1초 분할)").arg(m_segmentSeconds));
            return true;
        };

        auto remuxPacket = [&](const AVPacket* srcPacket) {
            if (!m_recordingEnabled.load(std::memory_order_acquire)) {
                if (outFmt) {
                    closeSegment(true);
                }
                return;
            }

            if (!outFmt) {
                if (!openSegment()) {
                    return;
                }
            }

            const AVPacket* packetToCheck = srcPacket;
            if (packetToCheck->stream_index == videoStreamIndex && packetToCheck->pts != AV_NOPTS_VALUE) {
                if (segmentStartPts == AV_NOPTS_VALUE) {
                    segmentStartPts = packetToCheck->pts;
                    if (!segmentStartNotified) {
                        emit segmentStarted(m_cameraId,
                                            segmentStartUtcMs,
                                            m_segmentSeconds,
                                            segmentTempPath,
                                            segmentStartPts,
                                            videoStream->time_base.num,
                                            videoStream->time_base.den);
                        segmentStartNotified = true;
                    }
                } else {
                    const qint64 elapsedMs = av_rescale_q(packetToCheck->pts - segmentStartPts,
                                                          videoStream->time_base,
                                                          AVRational{1, 1000});
                    if (elapsedMs >= static_cast<qint64>(m_segmentSeconds) * 1000LL &&
                        (packetToCheck->flags & AV_PKT_FLAG_KEY) != 0) {
                        closeSegment(true);
                        if (!openSegment()) {
                            return;
                        }
                        segmentStartPts = packetToCheck->pts;
                        if (!segmentStartNotified) {
                            emit segmentStarted(m_cameraId,
                                                segmentStartUtcMs,
                                                m_segmentSeconds,
                                                segmentTempPath,
                                                segmentStartPts,
                                                videoStream->time_base.num,
                                                videoStream->time_base.den);
                            segmentStartNotified = true;
                        }
                    }
                }
            }

            AVPacket writePacket;
            av_init_packet(&writePacket);
            av_packet_ref(&writePacket, srcPacket);
            const int outIndex = (writePacket.stream_index >= 0 && writePacket.stream_index < outStreamMap.size())
                                     ? outStreamMap[writePacket.stream_index]
                                     : -1;
            if (outIndex < 0 || !outFmt) {
                av_packet_unref(&writePacket);
                return;
            }

            AVStream* inStream = inFmt->streams[writePacket.stream_index];
            AVStream* outStream = outFmt->streams[outIndex];
            writePacket.stream_index = outIndex;
            writePacket.pts = av_rescale_q_rnd(writePacket.pts, inStream->time_base, outStream->time_base,
                                               static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
            writePacket.dts = av_rescale_q_rnd(writePacket.dts, inStream->time_base, outStream->time_base,
                                               static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
            writePacket.duration = static_cast<int>(av_rescale_q(writePacket.duration, inStream->time_base, outStream->time_base));
            writePacket.pos = -1;

            const int writeErr = av_interleaved_write_frame(outFmt, &writePacket);
            av_packet_unref(&writePacket);
            if (writeErr < 0) {
                emit statusChanged(m_cameraId, "세그먼트 write 실패: " + ffErr(writeErr));
                closeSegment(true);
            }
        };

        auto publishDecodedFrame = [&](AVFrame* decoded) {
            if (m_analysisBusy.load(std::memory_order_acquire)) {
                return;
            }

            qint64 sourcePts = decoded->best_effort_timestamp;
            qint64 captureUtcMs = QDateTime::currentMSecsSinceEpoch();
            if (sourcePts != AV_NOPTS_VALUE) {
                if (captureAnchorPts == AV_NOPTS_VALUE) {
                    captureAnchorPts = sourcePts;
                    captureAnchorUtcMs = captureUtcMs;
                }
                const qint64 deltaMs = av_rescale_q(sourcePts - captureAnchorPts, videoStream->time_base, AVRational{1, 1000});
                captureUtcMs = captureAnchorUtcMs + deltaMs;
            }

            const qint64 frameSeq = m_frameSequence.fetch_add(1, std::memory_order_acq_rel) + 1;
            const qint64 frameUtcMs = captureUtcMs;

            SwsContext* sws = sws_getContext(decoded->width,
                                             decoded->height,
                                             static_cast<AVPixelFormat>(decoded->format),
                                             decoded->width,
                                             decoded->height,
                                             AV_PIX_FMT_BGRA,
                                             SWS_BILINEAR,
                                             nullptr,
                                             nullptr,
                                             nullptr);
            if (!sws) {
                return;
            }

            uint8_t* dstData[4] = {nullptr, nullptr, nullptr, nullptr};
            int dstLinesize[4] = {0, 0, 0, 0};
            if (av_image_alloc(dstData, dstLinesize, decoded->width, decoded->height, AV_PIX_FMT_BGRA, 1) < 0) {
                sws_freeContext(sws);
                return;
            }

            sws_scale(sws,
                      decoded->data,
                      decoded->linesize,
                      0,
                      decoded->height,
                      dstData,
                      dstLinesize);

            cv::Mat bgra(decoded->height, decoded->width, CV_8UC4, dstData[0], dstLinesize[0]);
            QImage image(bgra.data, bgra.cols, bgra.rows, static_cast<int>(bgra.step), QImage::Format_ARGB32);
            cv::cuda::GpuMat gpu;
            try {
                gpu.upload(bgra);
            } catch (...) {
                gpu.release();
            }

            {
                QMutexLocker lock(&m_frameMutex);
                m_latestFrame = image.copy();
                m_latestGpuFrame = gpu;
                m_latestFrameUtcMs = frameUtcMs;
                m_latestFrameSeq = frameSeq;
                m_latestCaptureUtcMs = captureUtcMs;
                m_latestSourcePts = (sourcePts == AV_NOPTS_VALUE ? 0 : sourcePts);
                m_latestSourceTimeBaseNum = videoStream->time_base.num;
                m_latestSourceTimeBaseDen = videoStream->time_base.den;
                m_latestFrameSize = cv::Size{decoded->width, decoded->height};
            }

            av_freep(&dstData[0]);
            sws_freeContext(sws);

            if (!m_notifyPending.exchange(true, std::memory_order_acq_rel)) {
                emit frameReady(m_cameraId);
            }
        };

        while (m_running.load(std::memory_order_acquire) && !isInterruptionRequested()) {
            err = av_read_frame(inFmt, packet);
            if (err < 0) {
                emit statusChanged(m_cameraId, "스트림 끊김 - 재연결...");
                break;
            }

            remuxPacket(packet);

            if (packet->stream_index == videoStreamIndex) {
                if (avcodec_send_packet(decCtx, packet) >= 0) {
                    while (avcodec_receive_frame(decCtx, frame) == 0) {
                        publishDecodedFrame(frame);
                        av_frame_unref(frame);
                    }
                }
            }

            av_packet_unref(packet);
        }

        if (outFmt) {
            closeSegment(true);
        }

        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&decCtx);
        avformat_close_input(&inFmt);

        for (int i = 0; i < (kReconnectDelayMs / 100) && !isInterruptionRequested(); ++i) {
            msleep(100);
        }
    }

    avformat_network_deinit();
}

