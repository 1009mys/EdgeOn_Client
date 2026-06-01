#include "recorderworker.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QDir>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>

namespace {

constexpr int kReconnectDelayMs = 3000;

QString resolveFfmpegProgram()
{
	// 1) 환경 변수로 강제 지정
	const QString envPath = qEnvironmentVariable("EDGEON_FFMPEG_PATH").trimmed();
	if (!envPath.isEmpty() && QFileInfo::exists(envPath)) {
		return envPath;
	}

	// 2) 앱 설정값으로 지정
	QSettings settings;
	const QString configuredPath = settings.value("record/ffmpegPath").toString().trimmed();
	if (!configuredPath.isEmpty() && QFileInfo::exists(configuredPath)) {
		return configuredPath;
	}

	// 3) 배포 번들 경로 우선 탐색
	const QString appDir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
	const QString localFfmpeg = QDir(appDir).filePath("ffmpeg.exe");
	if (QFileInfo::exists(localFfmpeg)) {
		return localFfmpeg;
	}
	const QString toolsFfmpeg = QDir(appDir).filePath("tools/ffmpeg.exe");
	if (QFileInfo::exists(toolsFfmpeg)) {
		return toolsFfmpeg;
	}
#endif

	// 4) 시스템 PATH 탐색
	const QString systemPath = QStandardPaths::findExecutable("ffmpeg");
	if (!systemPath.isEmpty()) {
		return systemPath;
	}

	return {};
}

}

RecorderWorker::RecorderWorker(int cameraId,
							   const QString& url,
							   const QString& outputRoot,
							   int segmentSeconds,
							   QObject* parent)
	: QThread(parent),
	  m_cameraId(cameraId),
	  m_url(url.trimmed()),
	  m_outputRoot(outputRoot.trimmed()),
	  m_segmentSeconds(segmentSeconds > 0 ? segmentSeconds : 30) {}

void RecorderWorker::stop()
{
	m_running = false;
	requestInterruption();
}

void RecorderWorker::run()
{
	if (m_url.isEmpty()) {
		emit statusChanged(m_cameraId, "녹화 시작 실패: URL이 비어 있습니다.");
		return;
	}

	while (m_running.load(std::memory_order_acquire) && !isInterruptionRequested()) {
		const QString ffmpegProgram = resolveFfmpegProgram();
		if (ffmpegProgram.isEmpty()) {
			emit statusChanged(m_cameraId,
							   "녹화 시작 실패: ffmpeg를 찾을 수 없습니다 (EDGEON_FFMPEG_PATH 또는 record/ffmpegPath 설정 필요)");
			break;
		}

		const QString safeRoot = m_outputRoot.isEmpty() ? QStringLiteral("E:/EdgeOn") : m_outputRoot;
		const QString cameraDirPath = QDir::cleanPath(safeRoot + QString("/camera_%1").arg(m_cameraId));
		QDir cameraDir(cameraDirPath);
		if (!cameraDir.exists() && !cameraDir.mkpath(".")) {
			emit statusChanged(m_cameraId, "녹화 시작 실패: 저장 폴더 생성 실패");
			break;
		}

		const QString startStamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
		const QString filePattern = cameraDir.filePath(
			QString("camera_%1_%2_%05d.mkv").arg(m_cameraId).arg(startStamp));

		QStringList args;
		args << "-hide_banner"
			 << "-loglevel" << "warning"
			 << "-rtsp_transport" << "tcp"
			 << "-i" << m_url
			 << "-map" << "0"
			 << "-c" << "copy"
			 << "-f" << "segment"
			 << "-segment_time" << QString::number(m_segmentSeconds)
			 << "-segment_format" << "matroska"
			 << "-reset_timestamps" << "1"
			 << "-y"
			 << filePattern;

		QProcess process;
		process.setProcessChannelMode(QProcess::MergedChannels);
		process.start(ffmpegProgram, args);
		if (!process.waitForStarted(5000)) {
			emit statusChanged(m_cameraId,
							   QString("녹화 시작 실패: ffmpeg 실행 불가 (%1)").arg(ffmpegProgram));
			break;
		}

		emit statusChanged(m_cameraId,
						   QString("녹화 중 (원본 copy, %1초 분할)").arg(m_segmentSeconds));

		while (m_running.load(std::memory_order_acquire) && !isInterruptionRequested()) {
			if (process.waitForFinished(500)) {
				break;
			}
		}

		if (process.state() != QProcess::NotRunning) {
			process.terminate();
			if (!process.waitForFinished(3000)) {
				process.kill();
				process.waitForFinished();
			}
			emit statusChanged(m_cameraId, "녹화 중지");
			break;
		}

		const int exitCode = process.exitCode();
		if (!m_running.load(std::memory_order_acquire) || isInterruptionRequested()) {
			emit statusChanged(m_cameraId, "녹화 중지");
			break;
		}

		const QString processLog = QString::fromLocal8Bit(process.readAll()).trimmed();
		if (exitCode == 0) {
			emit statusChanged(m_cameraId, "녹화 완료");
			break;
		}

		const QString brief = processLog.isEmpty()
								  ? QStringLiteral("ffmpeg 비정상 종료")
								  : processLog.left(180);
		emit statusChanged(m_cameraId, QString("녹화 재연결 대기: %1").arg(brief));

		for (int i = 0; i < (kReconnectDelayMs / 100); ++i) {
			if (!m_running.load(std::memory_order_acquire) || isInterruptionRequested()) {
				break;
			}
			msleep(100);
		}
	}
}



