#pragma once

#include "IsoLevels.h"

#include <QDialog>
#include <QPointer>
#include <QVector>

#include <functional>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTableWidget;

/// The editor of the 3-D isosurface's levels (issue #526): how many, between
/// which values, linear or log spaced, or a typed list; then each surface's
/// colour (by default the colour map's at that value) and opacity (by default
/// falling outward). "Apply" hands the levels to the viewer, which draws them
/// without closing the dialog; "One surface" goes back to the single surface at
/// the threshold.
class IsoLevelsDialog : public QDialog
{
    Q_OBJECT

public:
    struct Setup
    {
        QVector<visivo::isolevels::Level> current;   ///< the levels in use; empty: none yet
        visivo::isolevels::Range range;              ///< where a new set starts
        QString unit;                                ///< the cube's unit, for the labels
        std::function<QColor(double)> colourAt;      ///< the colour map at a value
    };

    explicit IsoLevelsDialog(const Setup &setup, QWidget *parent = nullptr);

    /// The levels as edited.
    QVector<visivo::isolevels::Level> levels() const { return m_levels; }

signals:
    /// "Apply": draw these levels now.
    void applied(const QVector<visivo::isolevels::Level> &levels);
    /// "One surface": leave the levels and draw one at the threshold.
    void singleSurfaceRequested();

private:
    void generate();
    void useTypedList();
    void fillTable();
    void setStatus(const QString &text, bool error);
    void adopt(const QVector<double> &values);

    Setup m_setup;
    QVector<visivo::isolevels::Level> m_levels;
    QPointer<QSpinBox> m_count;
    QPointer<QLineEdit> m_min;
    QPointer<QLineEdit> m_max;
    QPointer<QComboBox> m_spacing;
    QPointer<QLineEdit> m_list;
    QPointer<QTableWidget> m_table;
    QPointer<QLabel> m_status;
};
