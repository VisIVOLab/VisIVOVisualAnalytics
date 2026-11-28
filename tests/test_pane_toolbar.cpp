// test_pane_toolbar.cpp
// The strip above the pane grid (PaneToolbar), which the cube window and the particle
// window both build from: what it holds and in which order, its size, margins and spacing,
// the pane glyphs, what a click reports, and when the Linked button and the facet chips can
// be used. Both windows get the same widget, so what is checked here is true of both.
//
// A process of its own: it needs QtWidgets, which VisIVOTests does not link.

#include "PaneToolbar.h"

#include "theme/SegmentedToggle.h"

#include <QAbstractButton>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>

using namespace Qt::StringLiterals;

namespace {

/// The cube's chips, as the cube window adds them.
QList<QToolButton *> addCubeChips(PaneToolbar &bar)
{
    QList<QToolButton *> chips;
    for (const QString &name : { u"camera"_s, u"channel"_s, u"LUT"_s, u"render"_s, u"blend"_s })
        chips << bar.addChip({ name, u"tip"_s, true, {} });
    return chips;
}

QList<QAbstractButton *> segments(const PaneToolbar &bar)
{
    return bar.paneCountToggle()->findChildren<QAbstractButton *>();
}

} // namespace

class TestPaneToolbar : public QObject
{
    Q_OBJECT

private slots:
    void sizeMarginsAndSpacing_areTheCubesStrip()
    {
        PaneToolbar bar;
        QCOMPARE(bar.objectName(), u"paneToolbar"_s);
        QCOMPARE(bar.height(), 30);
        QCOMPARE(bar.minimumHeight(), 30);
        QCOMPARE(bar.maximumHeight(), 30);
        auto *lay = qobject_cast<QHBoxLayout *>(bar.layout());
        QVERIFY(lay);
        QCOMPARE(lay->contentsMargins(), QMargins(8, 0, 8, 0));
        QCOMPARE(lay->spacing(), 8);
        QCOMPARE(bar.paneCountToggle()->maximumWidth(), 108);
    }

    void items_toggleWindowWidgetLinkedChipsStretchReadout()
    {
        PaneToolbar bar;
        auto *lay = qobject_cast<QHBoxLayout *>(bar.layout());
        // Alone (the particle window with only the render chip adds one chip, no widget):
        // 1/2/4, Linked, stretch.
        QCOMPARE(bar.itemCount(), 3);

        auto *pick = new SegmentedToggle();
        bar.addWindowWidget(pick);
        const QList<QToolButton *> chips = addCubeChips(bar);
        auto *readout = new QLabel(u"x"_s);
        bar.setReadoutWidget(readout);
        // toggle, 3D/2D, Linked, five chips each followed by a 4 px gap, stretch, readout.
        QCOMPARE(bar.itemCount(), 1 + 1 + 1 + 5 * 2 + 1 + 1);

        QCOMPARE(lay->itemAt(0)->widget(), bar.paneCountToggle());
        QCOMPARE(lay->itemAt(1)->widget(), pick);
        QCOMPARE(lay->itemAt(2)->widget(), bar.linkedButton());
        for (int i = 0; i < chips.size(); ++i) {
            QCOMPARE(lay->itemAt(3 + 2 * i)->widget(), chips[i]);
            QVERIFY(lay->itemAt(4 + 2 * i)->spacerItem());          // the 4 px gap
            QCOMPARE(lay->itemAt(4 + 2 * i)->sizeHint().width(), 4);
        }
        QVERIFY(lay->itemAt(13)->spacerItem());                       // the stretch
        QCOMPARE(lay->itemAt(14)->widget(), readout);
        QCOMPARE(readout->parentWidget(), &bar);
    }

    void aWindowWithoutItsOwnWidgetKeepsTheSameOrder()
    {
        PaneToolbar bar;
        auto *lay = qobject_cast<QHBoxLayout *>(bar.layout());
        auto *chip = bar.addChip({ u"render"_s, u"tip"_s, true, {} });
        QCOMPARE(lay->itemAt(0)->widget(), bar.paneCountToggle());
        QCOMPARE(lay->itemAt(1)->widget(), bar.linkedButton());
        QCOMPARE(lay->itemAt(2)->widget(), chip);
        QVERIFY(lay->itemAt(3)->spacerItem());
        QVERIFY(lay->itemAt(4)->spacerItem());   // the stretch
        QCOMPARE(bar.itemCount(), 5);
    }

    void paneToggle_hasThreeGlyphSegmentsAndLinkedAnIcon()
    {
        PaneToolbar bar;
        QCOMPARE(bar.paneCountToggle()->count(), 3);
        const QList<QAbstractButton *> seg = segments(bar);
        QCOMPARE(seg.size(), 3);
        for (const QAbstractButton *b : seg) {
            QVERIFY(!b->icon().isNull());
            QVERIFY(b->text().isEmpty());       // glyphs only
            QVERIFY(!b->toolTip().isEmpty());   // a bare glyph says which layout it is
        }
        QCOMPARE(seg[0]->toolTip(), u"One pane"_s);
        QCOMPARE(seg[1]->toolTip(), u"Two panes"_s);
        QCOMPARE(seg[2]->toolTip(), u"Four panes"_s);
        QCOMPARE(bar.paneCountToggle()->currentIndex(), 0);   // opens on one pane
        QVERIFY(!bar.linkedButton()->icon().isNull());
        QCOMPARE(bar.linkedButton()->text(), u" Linked"_s);
        QVERIFY(bar.linkedButton()->isCheckable());
    }

    void chips_haveTheirTextTooltipAndInitialState()
    {
        PaneToolbar bar;
        auto *on = bar.addChip({ u"camera"_s, u"Linked: camera"_s, true, {} });
        auto *off = bar.addChip({ u"LUT"_s, u"Linked: LUT"_s, false, {} });
        QCOMPARE(on->text(), u"camera"_s);
        QCOMPARE(on->toolTip(), u"Linked: camera"_s);
        QVERIFY(on->isCheckable());
        QVERIFY(on->isChecked());
        QVERIFY(!off->isChecked());
    }

    void chipEnableRule_needsTargetsAndTheMaster()
    {
        PaneToolbar bar;
        const QList<QToolButton *> chips = addCubeChips(bar);
        chips[1]->setChecked(false);

        bar.setLinkAvailable(false, true);   // nothing to link: even a master left on does not help
        QVERIFY(!bar.linkedButton()->isEnabled());
        for (const QToolButton *c : chips)
            QVERIFY(!c->isEnabled());

        bar.setLinkAvailable(true, false);   // targets, master off: the button works, chips grey
        QVERIFY(bar.linkedButton()->isEnabled());
        for (const QToolButton *c : chips)
            QVERIFY(!c->isEnabled());

        bar.setLinkAvailable(true, true);
        QVERIFY(bar.linkedButton()->isEnabled());
        for (const QToolButton *c : chips)
            QVERIFY(c->isEnabled());
        QVERIFY(!chips[1]->isChecked());   // disabled chips keep their choice
    }

    void chipCallback_runsOnAUserClickNotOnASilentSet()
    {
        PaneToolbar bar;
        QList<bool> seen;
        auto *chip = bar.addChip({ u"render"_s, {}, true, [&seen](bool on) { seen << on; } });
        bar.setLinkAvailable(true, true);
        PaneToolbar::setChipChecked(chip, false);
        QVERIFY(seen.isEmpty());
        QVERIFY(!chip->isChecked());
        chip->click();
        QCOMPARE(seen, (QList<bool>{ true }));
    }

    void toggle_reportsTheChosenCount()
    {
        PaneToolbar bar;
        QSignalSpy spy(&bar, &PaneToolbar::paneCountRequested);
        segments(bar)[2]->click();
        segments(bar)[1]->click();
        segments(bar)[0]->click();
        QCOMPARE(spy.size(), 3);
        QCOMPARE(spy[0][0].toInt(), 4);
        QCOMPARE(spy[1][0].toInt(), 2);
        QCOMPARE(spy[2][0].toInt(), 1);
    }

    void setLayoutState_showsTheLayoutWithoutReportingIt()
    {
        PaneToolbar bar;
        QSignalSpy count(&bar, &PaneToolbar::paneCountRequested);
        bar.setLayoutState(4, false);
        QCOMPARE(bar.paneCountToggle()->currentIndex(), 2);
        bar.setLayoutState(2, true);
        QCOMPARE(bar.paneCountToggle()->currentIndex(), 1);
        QVERIFY(!segments(bar)[1]->icon().isNull());   // the rows glyph
        bar.setLayoutState(3, false);                  // anything but 1 / 2 / 4 reads as 1
        QCOMPARE(bar.paneCountToggle()->currentIndex(), 0);
        QVERIFY(count.isEmpty());
    }

    void secondClickOnTwo_flipsTheSplitOnlyAtTwoPanes()
    {
        PaneToolbar bar;
        QSignalSpy flip(&bar, &PaneToolbar::splitFlipRequested);
        bar.setLayoutState(1, false);
        segments(bar)[1]->click();   // choosing 2: a layout change, not a flip
        QVERIFY(flip.isEmpty());
        bar.setLayoutState(2, false);
        segments(bar)[1]->click();   // clicking the selected "2" again
        QCOMPARE(flip.size(), 1);
        bar.setLayoutState(4, false);
        segments(bar)[1]->click();
        QCOMPARE(flip.size(), 1);
    }
};

QTEST_MAIN(TestPaneToolbar)
#include "test_pane_toolbar.moc"
