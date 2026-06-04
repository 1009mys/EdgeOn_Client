#pragma once

#include <QWidget>

#include <vector>

#include "Protocol.h"

class VodVideoOverlay : public QWidget {
    Q_OBJECT
public:
    explicit VodVideoOverlay(QWidget* parent = nullptr);

    void setFrameDetections(int frameWidth,
                            int frameHeight,
                            const std::vector<detection_result>& detections);
    void clear();

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    QColor colorForClassId(int classId) const;
    QRectF computeAspectFitRect() const;

    int m_frameWidth{0};
    int m_frameHeight{0};
    std::vector<detection_result> m_detections;
};

