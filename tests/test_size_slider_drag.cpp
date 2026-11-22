// test_size_slider_drag.cpp
// Dragging the Splat size slider of a 3-D particle window (issue #456). The
// report said the splats did not change while the handle was dragged, only
// on a click in the groove. The slider here is the real one: the window's
// form (ui/vtkWindowCatalogue3D.ui) is built by uic, its VTK view stubbed.
// The window itself needs VTK and is not built here, so what is checked is
// the slider's side: every step of a drag emits valueChanged (the signal
// scaleChanged is connected to), and each step's value gives the local
// splats and the Remote view's set_point_style command a new size through
// the same functions the window calls. The ray-traced view does not take the
// slider; it is disabled while that view is shown (enablePointOnlyControls).
//
// A process of its own: it needs QtWidgets, which VisIVOTests does not link.

#include "ParticleSplatDefaults.h"
#include "RemotePointStyle.h"
#include "ui_vtkWindowCatalogue3D.h"

#include <QMainWindow>
#include <QSignalSpy>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTest>

#include <vector>

class TestSizeSliderDrag : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void theFormsSliderTracksItsHandle();
    void draggingTheHandleResizesEveryStep();
    void aClickInTheGrooveResizesToo();

private:
    struct Received
    {
        int value;
        bool handleHeld;
        double localRadius;
        double remoteSize;
    };

    QPoint handleCentre() const;
    void follow();

    QMainWindow *m_window = nullptr;
    Ui::vtkWindowCatalogue3D m_ui;
    std::vector<Received> m_received;
};

void TestSizeSliderDrag::init()
{
    m_window = new QMainWindow;
    m_ui.setupUi(m_window);
    m_window->resize(1280, 800);
    m_window->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_window));
    QSlider *slider = m_ui.sliderScale;
    QVERIFY(slider->isVisible());
    QVERIFY(slider->width() > 100);
    slider->setValue(ParticleSplatDefaults::initialSliderValue());
    m_received.clear();
    follow();
}

void TestSizeSliderDrag::cleanup()
{
    delete m_window;
    m_window = nullptr;
}

void TestSizeSliderDrag::follow()
{
    QSlider *slider = m_ui.sliderScale;
    // The spacing is any: only whether the radius changes is read.
    QObject::connect(slider, &QSlider::valueChanged, m_window, [this, slider](int value) {
        m_received.push_back(
                { value, slider->isSliderDown(),
                  ParticleSplatDefaults::splatRadiusForSlider(1.0, value),
                  visivo::remote::pointStyleCommand(true, value, 2.0)
                          .value(QStringLiteral("size"))
                          .toDouble() });
    });
}

QPoint TestSizeSliderDrag::handleCentre() const
{
    QSlider *slider = m_ui.sliderScale;
    QStyleOptionSlider option;
    option.initFrom(slider);
    option.subControls = QStyle::SC_All;
    option.orientation = slider->orientation();
    option.maximum = slider->maximum();
    option.minimum = slider->minimum();
    option.tickPosition = slider->tickPosition();
    option.tickInterval = slider->tickInterval();
    option.upsideDown = slider->invertedAppearance();
    option.direction = slider->layoutDirection();
    option.sliderPosition = slider->sliderPosition();
    option.sliderValue = slider->value();
    option.singleStep = slider->singleStep();
    option.pageStep = slider->pageStep();
    return slider->style()
            ->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, slider)
            .center();
}

void TestSizeSliderDrag::theFormsSliderTracksItsHandle()
{
    // Without tracking, valueChanged would wait for the release.
    QVERIFY(m_ui.sliderScale->hasTracking());
    QCOMPARE(m_ui.sliderScale->orientation(), Qt::Horizontal);
}

void TestSizeSliderDrag::draggingTheHandleResizesEveryStep()
{
    QSlider *slider = m_ui.sliderScale;
    QSignalSpy released(slider, &QSlider::sliderReleased);
    const int before = slider->value();
    const QPoint start = handleCentre();
    QTest::mousePress(slider, Qt::LeftButton, Qt::NoModifier, start);
    QVERIFY(slider->isSliderDown());
    // Otherwise the press would be a click in the groove, the case that
    // already worked, and the drag below would prove nothing.
    QCOMPARE(slider->value(), before);
    QVERIFY(m_received.empty());

    const int steps = 8;
    const int stride = (slider->width() - start.x()) / (steps + 2);
    QVERIFY(stride > 0);
    for (int i = 1; i <= steps; ++i)
        QTest::mouseMove(slider, start + QPoint(i * stride, 0));
    const std::size_t whileHeld = m_received.size();
    QTest::mouseRelease(slider, Qt::LeftButton, Qt::NoModifier,
                        start + QPoint(steps * stride, 0));

    QCOMPARE(released.count(), 1);
    QVERIFY2(whileHeld >= std::size_t(steps / 2),
             qPrintable(QStringLiteral("only %1 size changes during the drag").arg(whileHeld)));
    QCOMPARE(m_received.size(), whileHeld);
    QVERIFY(slider->value() > before);
    QCOMPARE(m_received.back().value, slider->value());
    for (std::size_t i = 0; i < m_received.size(); ++i) {
        QVERIFY(m_received[i].handleHeld);
        if (i == 0)
            continue;
        QVERIFY(m_received[i].value > m_received[i - 1].value);
        QVERIFY(m_received[i].localRadius > m_received[i - 1].localRadius);
        QVERIFY(m_received[i].remoteSize > m_received[i - 1].remoteSize);
    }
}

void TestSizeSliderDrag::aClickInTheGrooveResizesToo()
{
    QSlider *slider = m_ui.sliderScale;
    const int before = slider->value();
    const QPoint groove(slider->width() - 4, handleCentre().y());
    QTest::mouseClick(slider, Qt::LeftButton, Qt::NoModifier, groove);
    QVERIFY(slider->value() > before);
    QVERIFY(!m_received.empty());
    QCOMPARE(m_received.back().value, slider->value());
    QVERIFY(m_received.back().remoteSize > visivo::remote::pointStyleCommand(true, before, 2.0)
                                                   .value(QStringLiteral("size"))
                                                   .toDouble());
}

QTEST_MAIN(TestSizeSliderDrag)
#include "test_size_slider_drag.moc"
