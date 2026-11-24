#pragma once

#include "ChannelMapsDialog.h"
#include "app/BackendClient.h"

#include <QMainWindow>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

class QCPColorGradient;
class QCustomPlot;
class QEvent;
class QGridLayout;
class QLabel;
class QPushButton;
class QScrollArea;

class ChannelMapsWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit ChannelMapsWindow(const ChannelMapsConfig &config,
                               const QString &backendUrl,
                               const QString &token,
                               const QString &sessionId,
                               const QString &datasetId,
                               int cubeWidth, int cubeHeight, int cubeDepth,
                               const QString &filepath,
                               QWidget *parent = nullptr);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void saveMosaicPng();

private:
    void buildUi();
    void fetchAndRender();
    void populateGrid(const BackendCubeSubvolumeResult &vol);
    QString panelTitle(int absChannel) const;
    /// The panel title as one or two lines: the channel, then its spectral
    /// value with unit and frame when the cube has them.
    QStringList panelTitleLines(int absChannel) const;
    /// Sizes every panel from the room the viewport has (ChannelMapsLayout.h).
    void relayoutCells();
    void showEnlargedCell(int cellIdx, int absChannel,
                          double rangeMin, double rangeMax,
                          const QCPColorGradient &gradient);

    ChannelMapsConfig m_config;
    QString m_backendUrl;
    QString m_token;
    QString m_sessionId;
    QString m_datasetId;
    int m_cubeWidth;
    int m_cubeHeight;
    int m_cubeDepth;
    QString m_filepath;

    QPointer<QGridLayout> m_grid;
    QPointer<QScrollArea> m_scroll;
    QPointer<QLabel>      m_statusLabel;
    QVector<QCustomPlot*> m_cells;

    // Raw per-channel data stored for reliable PNG export (bypasses
    // QCustomPlot's toPixmap which has layout issues at non-widget sizes).
    struct ChannelSlice {
        int channel;      // absolute channel index
        int width, height;
        QVector<float> data; // row-major, size = width * height
    };
    QVector<ChannelSlice> m_slices;
    int m_cellTitleHeight{ 28 };   ///< height of the title row of a panel, px
    double m_sharedMin{ 0. };
    double m_sharedMax{ 1. };
};
