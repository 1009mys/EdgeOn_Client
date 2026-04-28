#pragma once
#include <QMainWindow>
#include <QVector>
#include <QMap>
#include <QGridLayout>
#include <QString>
#include "videocell.h"
#include "streamworker.h"

class QLabel;
class QMenu;
class QDockWidget;
class QListWidget;
class QListWidgetItem;
class QLineEdit;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    static constexpr int MAX_COLS = 4;
    static constexpr int MAX_ROWS = 4;
    static constexpr int TOTAL    = MAX_COLS * MAX_ROWS;

    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onAddRequested   (int cellId, const QString& url);
    void onMoveRequested  (int fromCellId, int toCellId);
    void onRemoveRequested(int cellId);
    void onFrameReady     (int cellId);
    void onStatusChanged  (int cellId, const QString& status);
    void loadUrlsFromFile ();
    void removeAllStreams ();
    void addStreamAddress();
    void removeSelectedStreamAddress();
    void onStreamListDoubleClicked(QListWidgetItem* item);

private:
    void buildUi();
    void clearGridItems();
    void applyGridLayout(int rows, int cols);
    void applyFocusRingLayout();
    void applyTriplePlusQuadLayout();
    void addLayoutPresetAction(QMenu* menu, const QString& text, int rows, int cols);
    void buildStreamListPanel();
    void addStream   (int cellId, const QString& url);
    void removeStream(int cellId);
    void updateStatusBar();

    QWidget*                 m_central{nullptr};
    QGridLayout*             m_grid{nullptr};
    int                      m_visibleRows{4};
    int                      m_visibleCols{4};
    int                      m_visibleSlotCount{TOTAL};
    QString                  m_layoutName{"4x4"};
    QDockWidget*             m_streamDock{nullptr};
    QListWidget*             m_streamList{nullptr};
    QLineEdit*               m_streamInput{nullptr};
    QVector<VideoCell*>      m_cells;
    QMap<int, StreamWorker*> m_workers;
    QLabel*                  m_statusLabel{nullptr};
};

