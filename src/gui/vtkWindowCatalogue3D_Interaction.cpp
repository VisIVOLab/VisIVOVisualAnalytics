#include "vtkWindowCatalogue3D.h"
#include "ui_vtkWindowCatalogue3D.h"

#include "app/DiagnosticsManager.h"
#include "DiagnosticsWindow.h"
#include "Catalogue3DTableModel.h"
#include "Catalogue3DParser.h"
#include "theme/VisivoTheme.h"

// ── VTK ──────────────────────────────────────────────────────────────────────
#include <QVTKOpenGLNativeWidget.h>
#include <vtkActor.h>
#include <vtkAppendPolyData.h>
#include <vtkAxesActor.h>
#include <vtkBillboardTextActor3D.h>
#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkCubeSource.h>
#include <vtkCubeAxesActor.h>
#include <vtkCoordinate.h>
#include <vtkFloatArray.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkGlyph3DMapper.h>
#include <vtkIntArray.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkLookupTable.h>
#include <vtkMatrix4x4.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSphereSource.h>
#include <vtkTextProperty.h>

// ── Qt ───────────────────────────────────────────────────────────────────────
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QKeySequence>
#include <QMetaObject>
#include <QComboBox>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTableView>
#include <QTextEdit>
#include <QVBoxLayout>
#include "SidebarPanel.h"
#include <QtConcurrent>

#include <optional>

/// Returns true when @p errorText looks like a backend session-expiry error.
// ── std ───────────────────────────────────────────────────────────────────────
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "CatalogueWcsUtil.h"

using namespace Qt::StringLiterals;

// ── Interaction ──
// Extracted verbatim from vtkWindowCatalogue3D.cpp.

void vtkWindowCatalogue3D::onMouseEvent(unsigned long eid)
{
    int x = 0, y = 0;
    ui->vtk->interactor()->GetEventPosition(x, y);

    if (eid == vtkCommand::LeftButtonDoubleClickEvent) {
        const int idx = entries.empty() ? -1 : pickNearestSource(x, y);
        qInfo() << "[catalogue3d] double-click observer fired, picked source:" << idx;
        if (idx >= 0) {
            const int newSelection = (idx == selectedIndex) ? -1 : idx;
            qInfo() << "[catalogue3d] double-click selection index:" << newSelection
                    << "(no zoom action executed)";
            setSelectedSource(newSelection);
        } else {
            qInfo() << "[catalogue3d] double-click miss; clearing selection"
                    << "(no zoom action executed)";
            setSelectedSource(-1);
        }
        this->leftClickCandidate = false;
        return;
    }

    if (eid == vtkCommand::LeftButtonPressEvent) {
        leftButtonDown = true;
        leftClickCandidate = true;
        pressX = x;
        pressY = y;
        pressPickIndex = entries.empty() ? -1 : pickNearestSource(x, y);
        qInfo() << "[catalogue3d] left press observed; camera remains owned by VTK style";
        qInfo() << "[catalogue3d] left press position:" << pressX << pressY
                << "press-pick-index:" << pressPickIndex;
        return;
    }

    if (eid == vtkCommand::LeftButtonReleaseEvent) {
        leftButtonDown = false;
        // Treat as a click only when the pointer hasn't moved significantly.
        const int dx = x - pressX;
        const int dy = y - pressY;
        qInfo() << "[catalogue3d] left release position:" << x << y
                << "click-candidate dx/dy:" << dx << dy;
        const bool isClick = this->leftClickCandidate
                && (dx * dx + dy * dy <= clickDragThresholdPx * clickDragThresholdPx);
        this->leftClickCandidate = false;
        if (isClick && !entries.empty()) {
            qInfo() << "[catalogue3d] click accepted with threshold:" << clickDragThresholdPx;
            int idx = this->pressPickIndex;
            if (idx < 0) {
                idx = pickNearestSource(x, y);
            }
            qInfo() << "[catalogue3d] click selection hit/miss index:" << idx;
            if (idx >= 0) {
                setSelectedSource(idx == selectedIndex ? -1 : idx);
            } else {
                qInfo() << "[catalogue3d] selection cleared by empty click";
                setSelectedSource(-1);
            }
        } else if (isClick) {
            qInfo() << "[catalogue3d] click accepted on empty scene; selection cleared";
            setSelectedSource(-1);
        } else {
            qInfo() << "[catalogue3d] drag rejected for selection";
        }
        return;
    }

    // MouseMoveEvent: hover-highlight the nearest source (yellow sphere). Camera
    // drag stays owned by VTK. Picking is now cheap (one-time matrix + inline
    // projection), so it can run on motion — but throttle it so a flood of move
    // events can't saturate the pick on a multi-million-point catalogue.
    if (leftButtonDown) {
        // Don't trail a stale highlight while rotating the camera.
        setHoveredSource(-1);
        return;
    }
    if (entries.empty())
        return;
    constexpr qint64 kHoverThrottleMs = 30;
    if (m_hoverThrottle.isValid() && m_hoverThrottle.elapsed() < kHoverThrottleMs)
        return;
    m_hoverThrottle.start();   // (re)start the throttle window
    setHoveredSource(pickNearestSource(x, y));
}

int vtkWindowCatalogue3D::pickNearestSource(int displayX, int displayY) const
{
    // Nearest source whose screen projection is within hoverThresholdPx of the
    // click. Rather than a vtkCoordinate world→display transform per entry
    // (millions of virtual calls per click on large catalogues), fetch the
    // camera's composite world→clip matrix ONCE and project inline — same result
    // (verified to ≤1 px, the vtkCoordinate int-rounding, against VTK 9.6.2),
    // ~10–50× faster. Picking still scans every entry, so it stays exact and
    // independent of the render-budget LOD (which only thins the GPU-fed cloud).
    vtkCamera *camera = this->renderer ? this->renderer->GetActiveCamera() : nullptr;
    if (!camera || this->mappedPositions.empty())
        return -1;

    const int *size = this->renderer->GetSize();
    const double w = size[0];
    const double h = size[1];
    if (w <= 0.0 || h <= 0.0)
        return -1;

    // world → clip (homogeneous: x/y ∈ [-cw, cw] before the perspective divide,
    // cw = clip-w below). Assumes the configured perspective/trackball camera —
    // for a parallel projection cw would not be a behind-camera test.
    vtkMatrix4x4 *cpt = camera->GetCompositeProjectionTransformMatrix(w / h, -1.0, 1.0);
    double m[16];
    for (int i = 0; i < 16; ++i)
        m[i] = cpt->GetElement(i / 4, i % 4);

    double bestDist2 = hoverThresholdPx * hoverThresholdPx;
    int bestIdx = -1;

    const std::size_t n = this->mappedPositions.size();
    for (std::size_t i = 0; i < n; ++i) {
        const auto &p = this->mappedPositions[i];
        const double x = p[0], y = p[1], z = p[2];
        const double cw = m[12] * x + m[13] * y + m[14] * z + m[15];
        if (cw <= 0.0)
            continue;   // at or behind the camera plane
        const double cx = m[0] * x + m[1] * y + m[2] * z + m[3];
        const double cy = m[4] * x + m[5] * y + m[6] * z + m[7];
        const double sx = (cx / cw * 0.5 + 0.5) * w;   // NDC → viewport pixels
        const double sy = (cy / cw * 0.5 + 0.5) * h;
        const double dx = sx - displayX;
        const double dy = sy - displayY;
        const double dist2 = dx * dx + dy * dy;
        if (dist2 < bestDist2) {
            bestDist2 = dist2;
            bestIdx = static_cast<int>(i);
        }
    }
    return bestIdx;
}

void vtkWindowCatalogue3D::updateHighlightSphere(vtkSphereSource *sphere, vtkActor *actor,
                                                  int idx, double radiusFactor)
{
    if (idx < 0) {
        actor->VisibilityOff();
        return;
    }
    const auto &position = this->mappedPositions[static_cast<std::size_t>(idx)];
    double tuple[3] = { defaultGlyphRadius, defaultGlyphRadius, defaultGlyphRadius };
    this->glyphScaleVectors->GetTuple(idx, tuple);
    const double r = std::max({ tuple[0], tuple[1], tuple[2] }) * radiusFactor;
    sphere->SetCenter(position[0], position[1], position[2]);
    sphere->SetRadius(r);
    sphere->Update();
    actor->VisibilityOn();
}

void vtkWindowCatalogue3D::setHoveredSource(int idx)
{
    if (idx == hoveredIndex)
        return;
    hoveredIndex = idx;
    updateHighlightSphere(hoverSphere.Get(), hoverActor.Get(), idx, 1.35);
    renderWindow->Render();
}

void vtkWindowCatalogue3D::setSelectedSource(int idx)
{
    selectedIndex = idx;
    if (hoveredIndex >= 0) {
        hoveredIndex = -1;
        hoverActor->VisibilityOff();
    }
    updateHighlightSphere(selectSphere.Get(), selectActor.Get(), idx, 1.18);
    updateInfoPanel();
    qInfo() << "[catalogue3d] selected source index:" << idx;
    emit this->sourceSelectionChanged(idx);
    renderWindow->Render();
}

void vtkWindowCatalogue3D::updateInfoPanel()
{
    if (selectedIndex < 0) {
        ui->labelInfo->setText(u"No source selected"_s);
        qInfo() << "[catalogue3d] details panel updated from selection: none";
        return;
    }

    const Catalogue3DEntry &e = entries[selectedIndex];
    const QString displayName = Catalogue3DParser::detail::entryDisplayName(e, this->schema);
    const QString morphology = Catalogue3DParser::detail::entryMorphology(e, this->schema);
    const double distanceMpc = Catalogue3DParser::detail::entryDistanceMpc(e, this->schema);
    const double redshift =
            Catalogue3DParser::detail::fieldDouble(e, this->schema, Catalogue3DParser::detail::detectedRedshiftField(this->schema));
    const double majorAxisArcmin =
            Catalogue3DParser::detail::entryMajorAxisArcmin(e, this->schema);
    const double minorAxisArcmin =
            Catalogue3DParser::detail::entryMinorAxisArcmin(e, this->schema);
    const double llsMajorKpc =
            Catalogue3DParser::detail::entryLlsMajorKpc(e, this->schema);
    const double llsMinorKpc =
            Catalogue3DParser::detail::entryLlsMinorKpc(e, this->schema);
    const double fluxMJy =
            Catalogue3DParser::detail::entryFluxMJy(e, this->schema);
    QString html;
    html += QStringLiteral("<b>%1</b>").arg(displayName.toHtmlEscaped());
    html += QStringLiteral("<br>RA: %1°").arg(e.raDeg, 0, 'f', 4);
    html += QStringLiteral("<br>Dec: %1°").arg(e.decDeg, 0, 'f', 4);
    if (redshift > 0.0)
        html += QStringLiteral("<br>z: %1").arg(redshift, 0, 'f', 4);
    if (distanceMpc > 0.0)
        html += QStringLiteral("<br>d: %1 Mpc").arg(distanceMpc, 0, 'f', 1);
    html += QStringLiteral("<br>Type: <i>%1</i>").arg(morphology.toHtmlEscaped());
    if (majorAxisArcmin > 0.0) {
        if (minorAxisArcmin > 0.0)
            html += QStringLiteral("<br>Size: %1′ × %2′")
                            .arg(majorAxisArcmin, 0, 'f', 1)
                            .arg(minorAxisArcmin, 0, 'f', 1);
        else
            html += QStringLiteral("<br>Size: %1′").arg(majorAxisArcmin, 0, 'f', 1);
    }
    if (llsMajorKpc > 0.0) {
        if (llsMinorKpc > 0.0)
            html += QStringLiteral("<br>LLS: %1 × %2 kpc")
                            .arg(llsMinorKpc, 0, 'f', 1)
                            .arg(llsMajorKpc, 0, 'f', 1);
        else
            html += QStringLiteral("<br>LLS: %1 kpc").arg(llsMajorKpc, 0, 'f', 1);
    }
    if (fluxMJy > 0.0)
        html += QStringLiteral("<br>Flux: %1 mJy").arg(fluxMJy, 0, 'f', 2);
    html += QStringLiteral(
            "<br><br><span style='color:#b0b8c8'>3D depth comes from RA/Dec + redshift or distance. Source shape is parametric.</span>");

    ui->labelInfo->setText(html);
    qInfo() << "[catalogue3d] details panel updated from selection:" << selectedIndex;
}

void vtkWindowCatalogue3D::toggleLabels(bool checked)
{
    if (checked && this->m_labelsSkippedDueToScale && labelActors.empty()) {
        // Labels were not built (too many sources). Tell the user instead of
        // silently doing nothing — the toggle isn't broken.
        const QString msg = u"Per-point labels are unavailable for catalogues over "
                            u"2000 sources (hover a source to see its name)."_s;
        ui->labelStatus->setText(msg);
        if (this->filterStatusLabel)
            this->filterStatusLabel->setText(msg);
        return;
    }
    for (auto &actor : labelActors)
        actor->SetVisibility(checked ? 1 : 0);
    if (!entries.empty())
        renderWindow->Render();
}

void vtkWindowCatalogue3D::toggleMorphColors(bool checked)
{
    glyphMapper->SetScalarVisibility(checked ? 1 : 0);
    pointsMapper->SetScalarVisibility(checked ? 1 : 0);
    if (!checked)
        glyphActor->GetProperty()->SetColor(0.75, 0.75, 0.85);
    if (!checked)
        pointsActor->GetProperty()->SetColor(0.75, 0.75, 0.85);
    if (checked) {
        this->updateColorMapping();
    }
    if (!entries.empty())
        renderWindow->Render();
}

void vtkWindowCatalogue3D::scaleChanged(int value)
{
    if (entries.empty())
        return;
    Q_UNUSED(value);
    this->updateGlyphScales();
    renderWindow->Render();
}

void vtkWindowCatalogue3D::resetCamera()
{
    renderer->ResetCamera();
    renderWindow->Render();
}

void vtkWindowCatalogue3D::updateGeometryMode()
{
    switch (this->comboGeometry ? this->comboGeometry->currentIndex() : 0) {
    case 1:
        this->geometryMode = GeometryMode::Sphere;
        break;
    case 2:
        this->geometryMode = GeometryMode::Point;
        break;
    case 3:
        this->geometryMode = GeometryMode::Cross;
        break;
    case 4:   // "Arrows (velocity)" — only present when vx/vy/vz exist
        this->geometryMode = GeometryMode::Velocity;
        break;
    default:
        this->geometryMode = GeometryMode::Ellipsoid;
        break;
    }
    this->updateGlyphPipeline();
}

void vtkWindowCatalogue3D::updateSizeMode()
{
    this->mapSizeKey = this->comboSizeMode ? this->comboSizeMode->currentData().toString()
                                           : QStringLiteral("builtin:fixed");
    if (this->mapSizeKey == QStringLiteral("builtin:major")) {
        this->sizeMode = SizeMode::MajorAxis;
    } else if (this->mapSizeKey == QStringLiteral("builtin:lls")) {
        this->sizeMode = SizeMode::Lls;
    } else if (this->mapSizeKey == QStringLiteral("builtin:flux")) {
        this->sizeMode = SizeMode::Flux;
    } else {
        this->sizeMode = SizeMode::Fixed;
    }
    this->updateGlyphScales();
    this->renderWindow->Render();
}

void vtkWindowCatalogue3D::toggleSceneAxes(bool checked)
{
    this->axesWidget->SetEnabled(checked ? 1 : 0);
    this->renderWindow->Render();
}

void vtkWindowCatalogue3D::toggleBoundingBox(bool checked)
{
    this->cubeAxesActor->SetVisibility(checked ? 1 : 0);
    this->renderWindow->Render();
}

void vtkWindowCatalogue3D::toggleShells(bool checked)
{
    for (const auto &actor : this->shellActors) {
        actor->SetVisibility(checked ? 1 : 0);
    }
    this->renderWindow->Render();
}

void vtkWindowCatalogue3D::updateDockSelectionDetails()
{
    if (!this->catalogueDetailsView) {
        return;
    }
    if (this->selectedIndex < 0 || this->selectedIndex >= static_cast<int>(this->entries.size())) {
        this->catalogueDetailsView->clear();
        qInfo() << "[catalogue3d] catalogue details view cleared";
        return;
    }
    const auto &entry = this->entries[static_cast<std::size_t>(this->selectedIndex)];
    QString details;
    details += QStringLiteral("Name: %1\n")
                       .arg(Catalogue3DParser::detail::entryDisplayName(entry, this->schema));
    details += QStringLiteral("RA: %1\n").arg(entry.raDeg, 0, 'f', 6);
    details += QStringLiteral("Dec: %1\n").arg(entry.decDeg, 0, 'f', 6);
    details += QStringLiteral("Distance: %1\n")
                       .arg(Catalogue3DParser::detail::entryDistanceMpc(entry, this->schema), 0, 'f', 3);
    details += QStringLiteral("Morphology: %1\n")
                       .arg(Catalogue3DParser::detail::entryMorphology(entry, this->schema));
    const QString rawDetails = this->fallbackDetailsText(entry);
    if (!rawDetails.isEmpty()) {
        details += QStringLiteral("\nRaw fields\n%1").arg(rawDetails);
    }
    this->catalogueDetailsView->setPlainText(details);
    qInfo() << "[catalogue3d] catalogue details view updated for selection:" << this->selectedIndex;
}

void vtkWindowCatalogue3D::syncTableSelection(int index)
{
    if (!this->catalogueTableView || !this->catalogueTableView->selectionModel() || !this->catalogueTableModel) {
        return;
    }
    this->syncingTableSelection = true;
    if (index < 0 || index >= this->catalogueTableModel->rowCount()) {
        this->catalogueTableView->clearSelection();
    } else {
        const QModelIndex modelIndex = this->catalogueTableModel->index(index, 0);
        this->catalogueTableView->selectionModel()->setCurrentIndex(
                modelIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        this->catalogueTableView->scrollTo(modelIndex, QAbstractItemView::PositionAtCenter);
    }
    this->syncingTableSelection = false;
}

void vtkWindowCatalogue3D::centerOnSource(int idx, double zoomFactor)
{
    if (idx < 0 || idx >= static_cast<int>(this->entries.size())) {
        return;
    }
    const auto &targetPosition = this->mappedPositions[static_cast<std::size_t>(idx)];
    auto *camera = this->renderer->GetActiveCamera();
    if (!camera) {
        return;
    }
    double focal[3];
    double cameraPosition[3];
    camera->GetFocalPoint(focal);
    camera->GetPosition(cameraPosition);
    const double dx = cameraPosition[0] - focal[0];
    const double dy = cameraPosition[1] - focal[1];
    const double dz = cameraPosition[2] - focal[2];
    camera->SetFocalPoint(targetPosition[0], targetPosition[1], targetPosition[2]);
    camera->SetPosition(targetPosition[0] + dx / zoomFactor,
                        targetPosition[1] + dy / zoomFactor,
                        targetPosition[2] + dz / zoomFactor);
    this->renderer->ResetCameraClippingRange();
    this->renderWindow->Render();
}
