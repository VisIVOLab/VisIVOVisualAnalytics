#pragma once

#include <QDialog>
#include <QString>

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
