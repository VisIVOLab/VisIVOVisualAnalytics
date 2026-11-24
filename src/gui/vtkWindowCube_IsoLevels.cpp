// Isosurfaces at several levels (issue #526).
//
// The 3-D view's isosurface was one surface at the threshold. With levels it is
// one surface per level, each with its own colour and opacity, nested: the
// backend extracts them in one request (POST /v1/products/isosurface), the
// viewer draws them with depth peeling so the inner ones show through the outer
// ones, and a legend names each level.
//
// The first (lowest) level's actor is currentIsosurfaceActor, so every place
// that knows "the isosurface" still finds one; the others are in
// m_isoExtraActors. The editor is IsoLevelsDialog, the numbers IsoLevels.h.

#include "vtkWindowCube.h"

#include "IsoLegend.h"
#include "IsoLevelsDialog.h"
#include "ui_vtkWindowCube.h"

#include <QMessageBox>
#include <QStatusBar>

#include <vtkActor.h>
#include <vtkColorTransferFunction.h>
#include <vtkProperty.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;
using namespace visivo::isolevels;

namespace {
/// The level whose value is `value` (they are the same numbers, the backend
/// having returned what was asked), else the nearest: the legend and the
/// surfaces cannot disagree about which is which.
const Level *levelAt(const QVector<Level> &levels, double value)
{
    const Level *best = nullptr;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (const Level &l : levels) {
        const double d = std::abs(l.value - value);
        if (d < bestDistance) {
            bestDistance = d;
            best = &l;
        }
    }
    return best;
}
}

QVector<vtkActor *> vtkWindowCube::isoSurfaceActors() const
{
    QVector<vtkActor *> out;
    if (this->currentIsosurfaceActor)
        out << this->currentIsosurfaceActor.GetPointer();
    for (const auto &extra : this->m_isoExtraActors)
        out << extra.GetPointer();
    return out;
}

void vtkWindowCube::removeIsoExtraActors()
{
    auto *renderer = (ui->vtkCube && ui->vtkCube->renderWindow())
            ? ui->vtkCube->renderWindow()->GetRenderers()->GetFirstRenderer()
            : nullptr;
    for (const auto &extra : this->m_isoExtraActors) {
        if (renderer && renderer->HasViewProp(extra))
            renderer->RemoveActor(extra);
    }
    this->m_isoExtraActors.clear();
}

QColor vtkWindowCube::isoColourAt(double value) const
{
    double rgb[3] = { 1.0, 0.5, 1.0 };   // the single surface's colour, with no map yet
    if (this->volumeColorTransferFunction)
        this->volumeColorTransferFunction->GetColor(value, rgb);
    return QColor::fromRgbF(std::clamp(rgb[0], 0.0, 1.0), std::clamp(rgb[1], 0.0, 1.0),
                            std::clamp(rgb[2], 0.0, 1.0));
}

void vtkWindowCube::applyIsoLevelStyle()
{
    auto *window = ui->vtkCube ? ui->vtkCube->renderWindow() : nullptr;
    auto *renderer = window ? window->GetRenderers()->GetFirstRenderer() : nullptr;
    bool translucent = false;
    if (this->isoLevelsActive()) {
        const QVector<vtkActor *> actors = this->isoSurfaceActors();
        for (int i = 0; i < actors.size() && i < this->m_isoDrawnValues.size(); ++i) {
            const Level *level = levelAt(this->m_isoLevels, this->m_isoDrawnValues[i]);
            if (!level)
                continue;
            actors[i]->GetProperty()->SetColor(level->colour.redF(), level->colour.greenF(),
                                               level->colour.blueF());
            actors[i]->GetProperty()->SetOpacity(level->opacity);
            translucent = translucent || level->opacity < 1.0;
        }
    }
    // Translucent surfaces drawn in the order of the scene would hide the inner
    // ones behind the outer ones' blending: depth peeling sorts them per pixel.
    // Only while there are some, as it costs a pass per peel; the peels are the
    // levels (at most kMaxLevels), the occlusion ratio zero for an exact result.
    if (renderer) {
        const bool peel = translucent && this->m_isosurfaceDrawn;
        const bool wasPeeling = renderer->GetUseDepthPeeling() != 0;
        renderer->SetUseDepthPeeling(peel ? 1 : 0);
        if (peel && !wasPeeling) {
            m_isoSavedMultiSamples = window->GetMultiSamples();
            window->SetAlphaBitPlanes(1);
            window->SetMultiSamples(0);
        } else if (!peel && wasPeeling) {
            window->SetAlphaBitPlanes(0);
            window->SetMultiSamples(m_isoSavedMultiSamples);
        }
        if (peel) {
            renderer->SetMaximumNumberOfPeels(2 * kMaxLevels);
            renderer->SetOcclusionRatio(0.0);
        }
    }
    this->refreshIsoLegend();
}

void vtkWindowCube::refreshIsoLegend()
{
    if (!this->m_isoLegend)
        return;
    const bool show = this->isoLevelsActive() && this->m_isosurfaceDrawn && this->m_show3dRendering
            && !this->m_isoDrawnValues.isEmpty();
    if (!show) {
        this->m_isoLegend->hide();
        return;
    }
    // The levels that have a surface; one that cut no voxels is not listed.
    QVector<Level> listed;
    for (const double v : this->m_isoDrawnValues) {
        if (const Level *level = levelAt(this->m_isoLevels, v))
            listed << *level;
    }
    this->m_isoLegend->setLevels(listed, this->volumeBarTitle());
    this->m_isoLegend->show();
}

void vtkWindowCube::setIsoLevels(const QVector<Level> &levels)
{
    const bool valuesChanged = visivo::isolevels::values(levels)
            != visivo::isolevels::values(this->m_isoLevels);
    if (!this->isoLevelsActive() && this->currentIsosurfaceActor) {
        // The single surface's colour, for when the levels are given up.
        double rgb[3];
        this->currentIsosurfaceActor->GetProperty()->GetColor(rgb);
        this->m_isoSingleColour = QColor::fromRgbF(rgb[0], rgb[1], rgb[2]);
    }
    this->m_isoLevels = levels;
    if (levels.isEmpty() || valuesChanged || !this->m_isosurfaceDrawn) {
        // New values: the backend extracts them; the surfaces and the legend
        // follow when it answers. Switching to the isosurface does the same.
        if (ui->actionIsosurface && !ui->actionIsosurface->isChecked())
            ui->actionIsosurface->setChecked(true);
        this->startAsyncIsosurface(ui->lineThreshold->text().toDouble());
        return;
    }
    // The same surfaces, other colours or opacities: nothing to extract.
    this->applyIsoLevelStyle();
    if (ui->vtkCube && ui->vtkCube->renderWindow())
        ui->vtkCube->renderWindow()->Render();
}

void vtkWindowCube::clearIsoLevels()
{
    if (!this->isoLevelsActive())
        return;
    this->m_isoLevels.clear();
    this->m_isoDrawnValues.clear();
    this->removeIsoExtraActors();
    // The one surface at the threshold again, in the colour the user chose.
    if (this->currentIsosurfaceActor) {
        this->currentIsosurfaceActor->GetProperty()->SetOpacity(1.0);
        this->currentIsosurfaceActor->GetProperty()->SetColor(this->m_isoSingleColour.redF(),
                                                              this->m_isoSingleColour.greenF(),
                                                              this->m_isoSingleColour.blueF());
    }
    this->applyIsoLevelStyle();
    if (ui->actionIsosurface && ui->actionIsosurface->isChecked())
        this->startAsyncIsosurface(ui->lineThreshold->text().toDouble());
}

void vtkWindowCube::openIsoLevelsDialog()
{
    if (this->m_effectiveRenderMode == RenderMode::Remote) {
        // The server draws this view with the one surface at the threshold.
        QMessageBox::information(
                this, tr("Isosurface levels"),
                tr("Surfaces at several levels are drawn by this window's own 3-D view. "
                   "Switch the 3-D view to local rendering to use them."));
        return;
    }
    const auto range = this->volumeDataRange();
    const double threshold = ui->lineThreshold->text().toDouble();
    IsoLevelsDialog::Setup setup;
    setup.current = this->m_isoLevels;
    setup.range = startingRange(threshold, range ? range->second : std::nan(""));
    setup.unit = this->m_brightnessUnit;
    setup.colourAt = [this](double v) { return this->isoColourAt(v); };
    auto *dialog = new IsoLevelsDialog(setup, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    QObject::connect(dialog, &IsoLevelsDialog::applied, this,
                     [this](const QVector<Level> &levels) { this->setIsoLevels(levels); });
    QObject::connect(dialog, &IsoLevelsDialog::singleSurfaceRequested, this,
                     [this]() { this->clearIsoLevels(); });
    dialog->show();
}
