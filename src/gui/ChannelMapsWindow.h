#pragma once

#include "ChannelMapsDialog.h"
#include "app/BackendClient.h"

#include <QMainWindow>
#include <QPointer>
#include <QString>
#include <QVector>

class QCPColorGradient;
class QCustomPlot;
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

private slots:
    void saveMosaicPng();

private:
    void buildUi();
    void fetchAndRender();
    void populateGrid(const BackendCubeSubvolumeResult &vol);
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
};
