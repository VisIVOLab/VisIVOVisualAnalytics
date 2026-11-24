#ifndef PhysicalUnitsControl_h
#define PhysicalUnitsControl_h

// PhysicalUnitsControl.h
// The Inspector's "Units" value for a simulation volume (issue #524): what the
// values and coordinates are shown in (physical units from the run's
// definitions.h, or what the user typed, or the file's code units), a switch
// between physical and code units, and "Units…" to type UNIT_DENSITY,
// UNIT_LENGTH, UNIT_VELOCITY or a factor and unit per variable
// (POST /v1/datasets/units). The rules live in app/SimulationVolume.h; this is
// the widget around them.

#include "app/SimulationVolume.h"

#include <QPointer>
#include <QString>
#include <QWidget>

class QCheckBox;
class QLabel;
class QToolButton;

class PhysicalUnitsControl : public QWidget
{
    Q_OBJECT

public:
    explicit PhysicalUnitsControl(QWidget *parent = nullptr);

    void setDataset(const QString &backendUrl, const QString &token, const QString &sessionId,
                    const QString &datasetId);
    void setUnits(const visivo::simvol::PhysicalUnits &units, bool physical);

    const visivo::simvol::PhysicalUnits &units() const { return m_units; }
    bool physical() const { return m_physical; }

signals:
    /// The user turned physical units on or off.
    void displayChanged(bool physical);
    /// The backend accepted typed units (or forgot them).
    void unitsChanged(const visivo::simvol::PhysicalUnits &units);

private:
    void refresh();
    void askForUnits();
    void send(const visivo::simvol::UnitsRequest &request);

    QString m_backendUrl;
    QString m_token;
    QString m_sessionId;
    QString m_datasetId;
    visivo::simvol::PhysicalUnits m_units;
    bool m_physical{ true };
    bool m_busy{ false };

    QPointer<QLabel> m_label;
    QPointer<QCheckBox> m_physicalBox;
    QPointer<QToolButton> m_editButton;
};

#endif   // PhysicalUnitsControl_h
