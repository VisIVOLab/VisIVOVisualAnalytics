// Contours of the cube's 2-D slice (and of the simulation slices) — partial
// class of vtkWindowCube.
//
// The slice pipeline feeds a ContourLayer (ContourLayer.h), so the contours
// follow the channel on screen, and the same ContourSettingsPanel as the image
// viewer's styles them: levels by count, sigma, logarithm or list, an optional
// smoothing, colours, line width and dashed negative levels (issue #519). They
// can be saved as a ds9 region file or a table of vertices (issue #49).

#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"

#include "ContourExport.h"
#include "ContourSettingsPanel.h"
#include "RegionPanePlacement.h"

#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkTrivialProducer.h>

#include <QGridLayout>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStatusBar>

#include <array>
#include <cmath>

using namespace Qt::StringLiterals;
namespace vc = visivo::contours;

namespace {

/// The slice on screen.
vtkImageData *sliceImageOf(vtkTrivialProducer *source)
{
    return source ? vtkImageData::SafeDownCast(source->GetOutputDataObject(0)) : nullptr;
}

/// Two bounds that are the same number as far as typing is concerned.
bool sameBound(double a, double b)
{
    return std::abs(a - b) <= 1e-6 * std::max({ std::abs(a), std::abs(b), 1e-30 });
}

}   // namespace

void vtkWindowCube::setupContours(vtkRenderer *renderer)
{
    // The levels start as the placeholder range of the constructor; they move
    // onto the data when the backend reports it (refreshContourBoundsFromData).
    vc::Settings s;
    s.count = this->level;
    s.lower = this->lowerBound;
    s.upper = this->upperBound;

    m_contourLayer = std::make_unique<ContourLayer>();
    m_contourLayer->attach(renderer);
    m_contourLayer->setInputConnection(this->remoteSliceDisplaySource->GetOutputPort());
    m_contourLayer->setSettings(s);

    // The panel goes where the old Level / Lower / Upper fields were, in the
    // frame the Parameters tab mounts while the tool is on.
    m_contourPanel = new ContourSettingsPanel(ui->frameContours);
    m_contourPanel->setSettings(s);
    ui->gridContours->addWidget(m_contourPanel, 2, 0, 1, 2);
    auto *exportButton = new QPushButton(u"Export Contours…"_s, ui->frameContours);
    exportButton->setToolTip(u"Save the contours of this slice as a ds9 region file (a polygon "
                             u"per line, with its colour and width) or as a table of vertices."_s);
    ui->gridContours->addWidget(exportButton, 3, 0, 1, 2);
    QObject::connect(exportButton, &QPushButton::clicked, this, &vtkWindowCube::exportContours);
    QObject::connect(m_contourPanel, &ContourSettingsPanel::changed, this,
                     &vtkWindowCube::onContourPanelChanged);
    QObject::connect(m_contourPanel, &ContourSettingsPanel::estimateRequested, this,
                     &vtkWindowCube::estimateContourNoise);
}

void vtkWindowCube::applyContourPixelScale()
{
    if (!m_contourLayer)
        return;
    // World units per cube pixel along the slice: 1 when the slice is drawn on
    // the cube's own pixels, more when it is a strided view of a large one.
    double worldPerPixel = 1.0;
    if (const auto placement = this->regionPlacement(this->sliceWin.GetPointer())) {
        const double perWorld = placement->cubePerWorld();
        if (std::isfinite(perWorld) && perWorld > 0.0)
            worldPerPixel = 1.0 / perWorld;
    }
    const double deg = std::abs(this->remoteDatasetCdelt[1]);
    m_contourLayer->setPixelScale(worldPerPixel, std::isfinite(deg) ? deg * 3600.0 : 0.0);
}

void vtkWindowCube::onContourPanelChanged()
{
    if (!m_contourLayer || !m_contourPanel)
        return;
    vc::Settings s = m_contourPanel->settings();
    const vc::Settings before = m_contourLayer->settings();
    // Typing a bound claims it: the data-driven initialisation must not
    // overwrite the user's own numbers if the preview lands afterwards.
    if (!sameBound(s.lower, before.lower) || !sameBound(s.upper, before.upper))
        m_contourBoundsInitialised = true;
    // Levels in sigma need a noise: start from the slice's own.
    if (s.levelMode == vc::LevelMode::Sigma && !(s.sigma > 0.0)) {
        if (vtkImageData *image = sliceImageOf(this->remoteSliceDisplaySource.GetPointer())) {
            s.sigma = ContourLayer::noiseOf(image);
            m_contourPanel->setSettings(s);
        }
    }
    this->applyContourPixelScale();
    m_contourPanel->setProblem(m_contourLayer->setSettings(s));
    this->sliceWin->Render();
}

void vtkWindowCube::estimateContourNoise()
{
    if (!m_contourLayer || !m_contourPanel)
        return;
    vtkImageData *image = sliceImageOf(this->remoteSliceDisplaySource.GetPointer());
    const double sigma = image ? ContourLayer::noiseOf(image) : 0.0;
    if (!(sigma > 0.0)) {
        this->statusBar()->showMessage(
                u"The noise of this slice could not be measured: type it."_s, 6000);
        return;
    }
    vc::Settings s = m_contourPanel->settings();   // edits not applied yet are kept
    s.sigma = sigma;
    m_contourPanel->setSettings(s);
    this->applyContourPixelScale();
    m_contourPanel->setProblem(m_contourLayer->setSettings(s));
    this->statusBar()->showMessage(
            u"Noise σ = %1 (1.4826 × MAD of the pixels of this channel)"_s.arg(sigma, 0, 'g', 6),
            8000);
    this->sliceWin->Render();
}

void vtkWindowCube::refreshContourBoundsFromData()
{
    // Only the first time: after that the panel holds the user's own numbers and
    // a later data update must not overwrite them.
    if (m_contourBoundsInitialised || !m_contourLayer || !m_contourPanel)
        return;
    if (!(this->lowerBound < this->upperBound))
        return;
    // The panel is the source of truth for the rest of the settings.
    vc::Settings s = m_contourPanel->settings();
    s.lower = this->lowerBound;
    s.upper = this->upperBound;
    // The panel shows the numbers with nine digits: they must still be two
    // different numbers when they come back from the text.
    if (!(QString::number(s.lower, 'g', 9).toDouble() < QString::number(s.upper, 'g', 9).toDouble()))
        return;
    m_contourBoundsInitialised = true;
    m_contourPanel->setSettings(s);
    this->applyContourPixelScale();
    m_contourPanel->setProblem(m_contourLayer->setSettings(s));
    this->sliceWin->Render();
}

void vtkWindowCube::exportContours()
{
    const auto placement = this->regionPlacement(this->sliceWin.GetPointer());
    if (!m_contourLayer || !placement) {
        this->statusBar()->showMessage(
                u"There is no slice on screen to take contours from."_s, 5000);
        return;
    }
    // The slice's world to the cube's pixels (0-based), as the regions are.
    const auto lines = m_contourLayer->lines([placement](double x, double y) {
        const visivo::regions::Vec2 p = placement->toCube(x, y);
        return std::array<double, 2>{ p[0], p[1] };
    });
    const QString frame = ui->actionGalactic->isChecked()
            ? u"galactic"_s
            : (ui->actionEcliptic->isChecked() ? u"ecliptic"_s : u"fk5"_s);
    vc::exportContours(this, lines, m_contourLayer->settings().lineWidth,
                       { this->remoteBackendUrl, this->remoteBackendToken, this->remoteSessionId,
                         this->remoteDatasetId, this->filepath },
                       frame);
}
