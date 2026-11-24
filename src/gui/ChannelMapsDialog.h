#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QComboBox;
class QLabel;
class QSpinBox;

struct ChannelMapsConfig
{
    int channelStart{ 0 };
    int channelEnd{ 63 };
    int stride{ 1 };
    int columns{ 8 };
    QString lutName;
    bool sharedRange{ true };
    /// The spectral value of channel channelStart + i, with unit and frame
    /// ("32.1534 km/s LSRK"), as the cube window shows its axis; empty when
    /// the axis is the channel number (issue #496).
    QStringList channelCaptions;
};

class ChannelMapsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ChannelMapsDialog(int cubeDepth, int currentSlice,
                               const QString &currentLut,
                               QWidget *parent = nullptr);

    ChannelMapsConfig config() const;

private:
    void updateSummary();

    int m_cubeDepth;
    QSpinBox  *m_startSpin  { nullptr };
    QSpinBox  *m_endSpin    { nullptr };
    QSpinBox  *m_strideSpin { nullptr };
    QSpinBox  *m_colsSpin   { nullptr };
    QComboBox *m_lutCombo   { nullptr };
    QLabel    *m_summaryLabel { nullptr };
};
