#pragma once

#include "IsoLevels.h"

#include <QString>
#include <QVector>
#include <QWidget>

/// The legend of the 3-D view's isosurfaces (issue #526): one row per level,
/// highest first, with its colour, its value and its opacity. Floats at the
/// right edge of the 3-D view it is attached to, where the volume's colour bar
/// is when the volume is drawn, and lets the mouse through. Drawn like that
/// bar: light text with a dark halo, no box, so it reads on any background.
class IsoLegend : public QWidget
{
    Q_OBJECT

public:
    explicit IsoLegend(QWidget *parent = nullptr);

    /// Moves the legend onto `host` (the 3-D view now shown) and re-places it.
    void attachTo(QWidget *host);
    /// What it lists; `unit` follows each value. An empty list shows nothing.
    void setLevels(const QVector<visivo::isolevels::Level> &levels, const QString &unit);

    /// The text of a row: the level's value, then its opacity in percent.
    static QString rowText(const visivo::isolevels::Level &level, const QString &unit);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void reposition();

    QVector<visivo::isolevels::Level> m_levels;
    QString m_unit;
};
