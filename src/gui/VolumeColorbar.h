#pragma once

#include <QColor>
#include <QPair>
#include <QString>
#include <QVector>
#include <QWidget>

#include <optional>

class QFontMetrics;

/// The 3-D volume's colour bar (issue #83): the colour map over its Min..Max on
/// the volume's scale, its values, and the threshold below which the volume is
/// transparent (that part of the bar is dimmed; its value is written under the
/// bar, also when it lies outside Min..Max). Floats at the right edge of the
/// 3-D view it is attached to, local or remote, over 80 % of its height like
/// the bar of the former VBT viewer, and lets the mouse through.
///
/// Positions along the bar are fractions of the scale, 0 at the bottom: the
/// host computes them (vtkWindowCube::volumeScalePosition), so the bar reads
/// the same on a linear, log or power scale.
///
/// The ray-traced views draw their bar with the same look (paint), so the bar
/// does not change when the style does.
class VolumeColorbar : public QWidget
{
    Q_OBJECT

public:
    /// What a bar shows.
    struct Look
    {
        QVector<QColor> colours;                 ///< from the bottom of the bar to its top
        QVector<QPair<double, QString>> ticks;   ///< labelled values: (position, text)
        QString title;
        std::optional<double> threshold;
        QString thresholdCaption;
    };

    explicit VolumeColorbar(QWidget *parent = nullptr);

    /// Moves the bar onto `host` (the 3-D view now shown) and re-places it.
    void attachTo(QWidget *host);

    /// The colours from the bottom of the bar to its top.
    void setColours(const QVector<QColor> &colours);
    /// Labelled values: (position, text).
    void setTicks(const QVector<QPair<double, QString>> &ticks);
    /// Above the bar: the quantity (a field name, a unit); empty for none.
    void setTitle(const QString &title);
    /// Where the threshold sits (clamped to the bar) and its caption under the
    /// bar; none hides both.
    void setThreshold(std::optional<double> position, const QString &caption = {});

    /// The bar's font.
    static QFont barFont();
    /// Where a bar showing `look` sits in a view of `host` size.
    static QRect placement(const QSize &host, const QFontMetrics &fm, const Look &look);
    /// Draws `look` into `rect` (as placed by placement).
    static void paint(QPainter &painter, const QRect &rect, const QFontMetrics &fm,
                      const Look &look);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void reposition();

    Look m_look;
};
