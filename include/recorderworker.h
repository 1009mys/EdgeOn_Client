#pragma once

#include <QThread>

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

protected:
	void run() override;

private:
	int m_cameraId;
	QString m_url;
	QString m_outputRoot;
	int m_segmentSeconds;
	std::atomic<bool> m_running{true};
};

