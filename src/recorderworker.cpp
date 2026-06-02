#include "recorderworker.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
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

} // namespace

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

		// ── ffmpeg 경로 확인 ─────────────────────────────────
		const QString ffmpegProgram = resolveFfmpegProgram();
		if (ffmpegProgram.isEmpty()) {
			emit statusChanged(m_cameraId,
							   "녹화 시작 실패: ffmpeg를 찾을 수 없습니다 "
							   "(EDGEON_FFMPEG_PATH 또는 record/ffmpegPath 설정 필요)");
			break;
		}

		// ── 저장 폴더 확인 ────────────────────────────────────
		const QString safeRoot      = m_outputRoot.isEmpty() ? QStringLiteral("E:/EdgeOn") : m_outputRoot;
		const QString cameraDirPath = QDir::cleanPath(safeRoot + QString("/camera_%1").arg(m_cameraId));
		QDir cameraDir(cameraDirPath);
		if (!cameraDir.exists() && !cameraDir.mkpath(".")) {
			emit statusChanged(m_cameraId, "녹화 시작 실패: 저장 폴더 생성 실패");
			break;
		}

		// ── 세그먼트 파일명 결정 ──────────────────────────────
		// 녹화 시작 시각 기록, 종료 시각은 실제 ffmpeg 종료 후 결정
		const QDateTime startDt    = QDateTime::currentDateTime();
		const QString   startStamp = startDt.toString("yyyyMMdd_HHmmss");

		// 녹화 중에는 임시 이름 사용
		const QString tempFileName = QString("camera_%1_%2_RECORDING.mkv")
										 .arg(m_cameraId).arg(startStamp);
		const QString tempFilePath = cameraDir.filePath(tempFileName);
		emit segmentStarted(m_cameraId, startDt.toMSecsSinceEpoch(), m_segmentSeconds, tempFilePath);

		// ── ffmpeg 인수 구성 ──────────────────────────────────
		// 세그먼트 1개 = -t segmentSeconds 로 정확히 잘라냄
		QStringList args;
		args << "-hide_banner"
			 << "-loglevel" << "warning"
			 << "-rtsp_transport" << "tcp"
			 << "-i" << m_url
			 << "-t" << QString::number(m_segmentSeconds)
			 << "-map" << "0"
			 << "-c" << "copy"
			 << "-y"
			 << tempFilePath;

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

		// ── ffmpeg 완료 대기 (중단 요청 감시) ────────────────
		while (m_running.load(std::memory_order_acquire) && !isInterruptionRequested()) {
			if (process.waitForFinished(500))
				break;
		}

		// ── 프로세스 강제 종료 (중단 요청 수신) ──────────────
		if (process.state() != QProcess::NotRunning) {
			process.terminate();
			if (!process.waitForFinished(3000)) {
				process.kill();
				process.waitForFinished();
			}
			// 실제 종료 시각으로 파일 rename
			const QString finalPath = renameWithEndTime(tempFilePath, cameraDirPath, startStamp);
			emit segmentFinished(m_cameraId,
								 startDt.toMSecsSinceEpoch(),
								 QDateTime::currentMSecsSinceEpoch(),
								 finalPath.isEmpty() ? tempFilePath : finalPath);
			emit statusChanged(m_cameraId, "녹화 중지");
			break;
		}

		// ── 정상 종료: 파일 rename ────────────────────────────
		const QString finalPath = renameWithEndTime(tempFilePath, cameraDirPath, startStamp);
		emit segmentFinished(m_cameraId,
							 startDt.toMSecsSinceEpoch(),
							 QDateTime::currentMSecsSinceEpoch(),
							 finalPath.isEmpty() ? tempFilePath : finalPath);

		if (!m_running.load(std::memory_order_acquire) || isInterruptionRequested()) {
			emit statusChanged(m_cameraId, "녹화 중지");
			break;
		}

		const int exitCode = process.exitCode();
		if (exitCode == 0) {
			// 세그먼트 정상 완료 → 다음 세그먼트 바로 시작
			continue;
		}

		// ── 오류 종료: 재연결 대기 ────────────────────────────
		const QString brief = QString::fromLocal8Bit(process.readAll()).trimmed().left(180);
		emit statusChanged(m_cameraId,
						   QString("녹화 재연결 대기: %1").arg(brief.isEmpty()
								? "ffmpeg 비정상 종료" : brief));

		for (int i = 0; i < (kReconnectDelayMs / 100); ++i) {
			if (!m_running.load(std::memory_order_acquire) || isInterruptionRequested())
				break;
			msleep(100);
		}
	}
}

QString RecorderWorker::renameWithEndTime(const QString& tempPath,
									   const QString& dirPath,
									   const QString& startStamp)
{
	const QString endStamp   = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
	const QString finalName  = QString("camera_%1_%2_%3.mkv")
								   .arg(m_cameraId).arg(startStamp).arg(endStamp);
	const QString finalPath  = QDir(dirPath).filePath(finalName);

	if (QFileInfo::exists(tempPath)) {
		if (!QFile::rename(tempPath, finalPath)) {
			// rename 실패 시 원래 이름 유지 (로그는 상태 신호로 전달)
			emit statusChanged(m_cameraId,
							   QString("파일 이름 변경 실패: %1").arg(tempPath));
			return {};
		}
	}

	return finalPath;
}




