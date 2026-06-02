#pragma once

#include <QThread>
#include <QString>

#include <atomic>

class RecorderWorker : public QThread {
	Q_OBJECT
public:
	explicit RecorderWorker(int cameraId,
							const QString& url,
							const QString& outputRoot,
							int segmentSeconds,
							QObject* parent = nullptr);

	void stop();

	[[nodiscard]] int cameraId() const { return m_cameraId; }
	[[nodiscard]] QString url() const { return m_url; }

signals:
	void statusChanged(int cameraId, QString status);
	void segmentStarted(int cameraId, qint64 startUtcMs, int segmentSeconds, QString tempPath);
	void segmentFinished(int cameraId, qint64 startUtcMs, qint64 endUtcMs, QString finalPath);

protected:
	void run() override;

private:
	QString renameWithEndTime(const QString& tempPath,
							   const QString& dirPath,
							   const QString& startStamp);
	int m_cameraId;
	QString m_url;
	QString m_outputRoot;
	int m_segmentSeconds;
	std::atomic<bool> m_running{true};
};

