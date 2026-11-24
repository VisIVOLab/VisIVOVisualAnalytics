#ifndef ContourSettingsPanel_h
#define ContourSettingsPanel_h

// ContourSettingsPanel.h
// The controls of a set of contours (ContourSettings.h), the same for the image
// viewer's own contours, those drawn from another FITS file and the cube's slice
// contours: how the levels are chosen, an optional smoothing, the colours, the
// line width and the dashing of negative levels. It edits a Settings value and
// says when the user changed it; what is drawn is the window's business.

#include "ContourSettings.h"

#include <QPointer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

class ContourSettingsPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ContourSettingsPanel(QWidget *parent = nullptr);

    /// Show @p settings; does not emit changed().
    void setSettings(const visivo::contours::Settings &settings);
    visivo::contours::Settings settings() const;

    /// Why the contours cannot be drawn as asked ("" clears it).
    void setProblem(const QString &text);

signals:
    /// The user changed something; settings() has the new values.
    void changed();
    /// Estimate was pressed: measure the noise and give it back with setSettings().
    void estimateRequested();

private:
    void syncRows();
    void pickColour();
    void emitChanged();

    visivo::contours::Settings m_state;   ///< what the colour button holds besides the widgets
    QPointer<QFormLayout> m_form;
    QPointer<QComboBox> m_levelMode;
    QPointer<QSpinBox> m_count;
    QPointer<QLineEdit> m_lower;
    QPointer<QLineEdit> m_upper;
    QPointer<QWidget> m_sigmaRow;
    QPointer<QLineEdit> m_sigma;
    QPointer<QLineEdit> m_multiples;
    QPointer<QCheckBox> m_negative;
    QPointer<QLineEdit> m_list;
    QPointer<QWidget> m_smoothRow;
    QPointer<QDoubleSpinBox> m_smoothing;
    QPointer<QComboBox> m_smoothingUnit;
    QPointer<QComboBox> m_colourMode;
    QPointer<QPushButton> m_colourButton;
    QPointer<QComboBox> m_colourMap;
    QPointer<QLineEdit> m_perLevel;
    QPointer<QDoubleSpinBox> m_width;
    QPointer<QCheckBox> m_dash;
    QPointer<QLabel> m_problem;
};

#endif   // ContourSettingsPanel_h
