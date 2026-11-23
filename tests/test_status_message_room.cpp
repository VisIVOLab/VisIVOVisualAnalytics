// test_status_message_room.cpp
// Temporary status-bar messages must be readable in the image and cube
// windows (issue #508). A QStatusBar draws showMessage() in the gap left of
// its permanent widgets; the image window's cursor readout is added with a
// stretch, and the cube window's labels can add up to more than the bar, so
// the gap was zero wide and no message ever showed. The bars here are set up
// the way those windows set theirs up, and what is measured is the width the
// message is drawn in: from the bar's left edge to its first visible
// permanent widget (what QStatusBar paints into), and the pixels there.
//
// A process of its own: it needs QtWidgets, which VisIVOTests does not link.

#include "StatusMessageRoom.h"

#include <QApplication>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QMainWindow>
#include <QStatusBar>
#include <QTest>

using namespace Qt::StringLiterals;

namespace {

QLabel *addLabel(QMainWindow &w, const QString &text, int stretch = 0, int minWidth = 0)
{
    auto *l = new QLabel(text, &w);
    l->setStyleSheet(u"QLabel { padding-left: 8px; }"_s);
    if (minWidth > 0)
        l->setMinimumWidth(minWidth);
    w.statusBar()->addPermanentWidget(l, stretch);
    return l;
}

/// Where QStatusBar paints a message: left edge to the first visible
/// permanent widget.
int messageWidth(QStatusBar *bar)
{
    int firstLeft = bar->width();
    for (QLabel *l : bar->findChildren<QLabel *>())
        if (l->isVisible())
            firstLeft = qMin(firstLeft, l->geometry().left());
    return firstLeft;
}

void settle(QMainWindow &w)
{
    QCoreApplication::sendPostedEvents(nullptr, 0);
    QTest::qWait(20);
    w.statusBar()->layout()->activate();
}

/// The image window's bar: a stretched readout, then the labels and a rail.
QLabel *buildImageBar(QMainWindow &w)
{
    w.resize(1400, 300);
    addLabel(w, u"state"_s);   // a leftmost label, as the window has none left of it
    QLabel *hover = addLabel(w, u"X=120  Y=88  Value=1.2e-3  RA=12:30:00  Dec=+12:00:00"_s, 1);
    for (const char *t : { "Sanity ok", "WCS ok", "Source: none", "health  queue  cache" })
        addLabel(w, QString::fromLatin1(t));
    return hover;
}

} // namespace

class TestStatusMessageRoom : public QObject
{
    Q_OBJECT

private slots:
    void aStretchedReadoutLeavesTheMessageNoRoom()
    {
        // The cause, without the fix: the message gap is (almost) zero.
        QMainWindow w;
        buildImageBar(w);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.statusBar()->showMessage(u"Add Image Layer: waiting for the first layer"_s);
        settle(w);
        QVERIFY2(messageWidth(w.statusBar()) < 20, "the readout's stretch should take the gap");
    }

    void theImageBarShowsAMessage()
    {
        QMainWindow w;
        QLabel *hover = buildImageBar(w);
        visivo::StatusMessageRoom::attach(w.statusBar())->yieldToMessages(hover);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        const QString msg = u"Add Image Layer: waiting for the first layer"_s;
        const int need = w.statusBar()->fontMetrics().horizontalAdvance(msg);
        w.statusBar()->showMessage(msg);
        settle(w);
        QVERIFY2(messageWidth(w.statusBar()) >= need,
                 qPrintable(u"message room %1 px, text %2 px"_s
                                    .arg(messageWidth(w.statusBar())).arg(need)));

        // It is drawn: the message area differs from the bar without a message.
        const int room = messageWidth(w.statusBar());
        const QImage with = w.statusBar()->grab(QRect(0, 0, room, w.statusBar()->height())).toImage();
        w.statusBar()->clearMessage();
        settle(w);
        const QImage without =
                w.statusBar()->grab(QRect(0, 0, room, w.statusBar()->height())).toImage();
        QVERIFY(with != without);
    }

    void theReadoutComesBackWhenTheMessageGoes()
    {
        QMainWindow w;
        QLabel *hover = buildImageBar(w);
        visivo::StatusMessageRoom::attach(w.statusBar())->yieldToMessages(hover);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        w.statusBar()->showMessage(u"first"_s);
        QVERIFY(hover->isHidden());
        w.statusBar()->showMessage(u"second replaces it"_s);   // restore, then fold again
        QVERIFY(hover->isHidden());
        w.statusBar()->clearMessage();
        QVERIFY(hover->isVisible());
        // And the timed form clears itself.
        w.statusBar()->showMessage(u"brief"_s, 30);
        QVERIFY(hover->isHidden());
        QTRY_VERIFY(hover->isVisible());
    }

    void aRowOfLabelsWiderThanTheBarGivesWayFromTheLeft()
    {
        // The cube window: no stretch, but the permanent labels fill the bar.
        QMainWindow w;
        w.resize(900, 300);
        QList<QLabel *> labels;
        for (int i = 0; i < 6; ++i)
            labels << addLabel(w, u"a permanent label with a long text %1"_s.arg(i));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        visivo::StatusMessageRoom::attach(w.statusBar());

        const QString msg = u"R1: spectrum recomputed at full resolution over 4096 pixels."_s;
        w.statusBar()->showMessage(msg);
        settle(w);
        const int need = qMin(w.statusBar()->fontMetrics().horizontalAdvance(msg) + 24,
                              w.statusBar()->width() * 3 / 5);
        QVERIFY2(messageWidth(w.statusBar()) >= need,
                 qPrintable(u"message room %1 px, wanted %2"_s
                                    .arg(messageWidth(w.statusBar())).arg(need)));
        QVERIFY(labels.first()->isHidden());   // leftmost goes first
        QVERIFY(labels.last()->isVisible());   // the right-hand end stays

        w.statusBar()->clearMessage();
        for (QLabel *l : std::as_const(labels))
            QVERIFY(l->isVisible());
    }

    void aWidgetTheWindowTakesBackStaysTheWindows()
    {
        // While folded, the window empties and hides a label (the cube's
        // product summary does): it must not come back at the message's end.
        QMainWindow w;
        w.resize(900, 300);
        QList<QLabel *> labels;
        for (int i = 0; i < 6; ++i)
            labels << addLabel(w, u"a permanent label with a long text %1"_s.arg(i));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        visivo::StatusMessageRoom::attach(w.statusBar());

        w.statusBar()->showMessage(u"R1: spectrum recomputed at full resolution over 4096 pixels."_s);
        QVERIFY(labels[0]->isHidden());
        QVERIFY(labels[1]->isHidden());
        labels[0]->clear();
        labels[0]->hide();              // already hidden by us: no event, empty text
        labels[1]->show();              // taken back, then hidden by the window
        labels[1]->hide();
        w.statusBar()->clearMessage();
        QVERIFY(labels[0]->isHidden());
        QVERIFY(labels[1]->isHidden());
        QVERIFY(labels[2]->isVisible());
    }

    void aSecondMessageReplacesTheFirstWithoutLosingWidgets()
    {
        QMainWindow w;
        w.resize(900, 300);
        QList<QLabel *> labels;
        for (int i = 0; i < 6; ++i)
            labels << addLabel(w, u"a permanent label with a long text %1"_s.arg(i));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        visivo::StatusMessageRoom::attach(w.statusBar());
        w.statusBar()->showMessage(u"first message, quite a long sentence for this bar"_s);
        w.statusBar()->showMessage(u"x"_s);   // this bar is over-full: some stay folded
        QVERIFY(labels.last()->isVisible());
        w.statusBar()->clearMessage();
        for (QLabel *l : std::as_const(labels))
            QVERIFY(l->isVisible());
    }

    void aBarWithRoomKeepsEverything()
    {
        QMainWindow w;
        w.resize(1400, 300);
        QLabel *a = addLabel(w, u"one"_s);
        QLabel *b = addLabel(w, u"two"_s);
        visivo::StatusMessageRoom::attach(w.statusBar());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.statusBar()->showMessage(u"short"_s);
        QVERIFY(a->isVisible());
        QVERIFY(b->isVisible());
    }

    void attachIsOnePerBar()
    {
        QMainWindow w;
        QCOMPARE(visivo::StatusMessageRoom::attach(w.statusBar()),
                 visivo::StatusMessageRoom::attach(w.statusBar()));
        QCOMPARE(visivo::StatusMessageRoom::attach(nullptr), nullptr);
    }

    void aLongSentenceDoesNotClearTheWholeBar()
    {
        QCOMPARE(visivo::StatusMessageRoom::roomWanted(5000, 1000), 600);
        QCOMPARE(visivo::StatusMessageRoom::roomWanted(100, 1000), 124);
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    TestStatusMessageRoom t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_status_message_room.moc"
