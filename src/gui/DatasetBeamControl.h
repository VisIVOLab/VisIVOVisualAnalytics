#ifndef DatasetBeamControl_h
#define DatasetBeamControl_h

// DatasetBeamControl.h
// The Inspector's "Beam" value: the beam and where it came from, with
// "Set beam…" when the header has none and "Clear" for a typed one
// (POST /v1/datasets/beam, issue #416). The rules live in app/DatasetBeam.h;
// this is the widget around them.

#include <QPointer>
#include <QString>
#include <QWidget>

#include <limits>

class QLabel;
class QToolButton;

class DatasetBeamControl : public QWidget
{
    Q_OBJECT

public:
    explicit DatasetBeamControl(QWidget *parent = nullptr);

    void setDataset(const QString &backendUrl, const QString &token, const QString &datasetId);
    void setBeam(double majorDeg, double minorDeg, double paDeg, const QString &source);

signals:
    /// The backend accepted a typed beam, or forgot it. Degrees; NaN when
    /// the dataset has no beam any more.
    void beamChanged(double majorDeg, double minorDeg, double paDeg, const QString &source);

private:
    void refresh();
    void askForBeam();
    void clearBeam();
    void send(double majorArcsec, double minorArcsec, double paDeg, bool clear);

    QString m_backendUrl;
    QString m_token;
    QString m_datasetId;
    double m_majorDeg{ std::numeric_limits<double>::quiet_NaN() };
    double m_minorDeg{ std::numeric_limits<double>::quiet_NaN() };
    double m_paDeg{ 0.0 };
    QString m_source;
    bool m_busy{ false };

    QPointer<QLabel> m_label;
    QPointer<QToolButton> m_setButton;
    QPointer<QToolButton> m_clearButton;
};

#endif   // DatasetBeamControl_h
