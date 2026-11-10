#include "vtkWindowCatalogue3D.h"
#include "RemotePointView.h"
#include <vtkPointGaussianMapper.h>
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
#include <QStatusBar>
#include <QTableView>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <optional>

/// Returns true when @p errorText looks like a backend session-expiry error.
// ── std ───────────────────────────────────────────────────────────────────────
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "CatalogueWcsUtil.h"
#include "SelectionMarkerSize.h"

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
            // The gesture's first click has already selected (or deselected)
            // it; the double-click selects it, not toggle it a second time.
            qInfo() << "[catalogue3d] double-click selection index:" << idx
                    << "(no zoom action executed)";
            if (idx != selectedIndex)
                setSelectedSource(idx);
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
        // A drag past the threshold is a camera move, even if it comes back
        // to where it started: the release must not count as a click.
        const int dx = x - pressX;
        const int dy = y - pressY;
        if (dx * dx + dy * dy > clickDragThresholdPx * clickDragThresholdPx)
            this->leftClickCandidate = false;
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

double vtkWindowCatalogue3D::highlightRadius(int idx, double radiusFactor)
{
    // Ellipsoids, spheres and crosses are wrapped by the marker; points,
    // splats and arrows have no body of their own to wrap.
    const bool wrapsGlyph = this->geometryMode == GeometryMode::Ellipsoid
            || this->geometryMode == GeometryMode::Sphere
            || this->geometryMode == GeometryMode::Cross;
    const double inData = this->highlightRadiusInData(idx, radiusFactor, wrapsGlyph);

    // Then kept to a few pixels on screen, at the source's depth: sized by the
    // data alone it could cover half the view (a few particles in a large box,
    // a bright source's glyph) or shrink to nothing.
    vtkCamera *camera = this->renderer ? this->renderer->GetActiveCamera() : nullptr;
    const int *size = this->renderer ? this->renderer->GetSize() : nullptr;
    if (!camera || !size || idx < 0 || static_cast<std::size_t>(idx) >= this->mappedPositions.size())
        return inData;
    const auto &p = this->mappedPositions[static_cast<std::size_t>(idx)];
    double eye[3], dir[3];
    camera->GetPosition(eye);
    camera->GetDirectionOfProjection(dir);
    const double depth = (p[0] - eye[0]) * dir[0] + (p[1] - eye[1]) * dir[1]
            + (p[2] - eye[2]) * dir[2];
    const double perPixel = visivo::worldPerPixel(camera->GetParallelProjection() != 0,
                                                  camera->GetViewAngle(),
                                                  camera->GetParallelScale(), depth, size[1]);
    // The hover ring a little larger than the selection's, as in data units.
    visivo::MarkerPixelRange range = visivo::markerPixelRange(wrapsGlyph);
    const double ringScale = radiusFactor / 1.18;
    range.minPx *= ringScale;
    range.maxPx *= ringScale;
    return visivo::clampMarkerRadius(inData, perPixel, range);
}

void vtkWindowCatalogue3D::refreshHighlightSizes()
{
    // The camera moved: the markers keep their size on screen.
    if (this->selectedIndex >= 0)
        this->updateHighlightSphere(this->selectSphere.Get(), this->selectActor.Get(),
                                    this->selectedIndex, 1.18);
    if (this->hoveredIndex >= 0)
        this->updateHighlightSphere(this->hoverSphere.Get(), this->hoverActor.Get(),
                                    this->hoveredIndex, 1.35);
}

double vtkWindowCatalogue3D::highlightRadiusInData(int idx, double radiusFactor,
                                                   bool wrapsGlyph) const
{
    if (!wrapsGlyph) {
        // Points and splats have no glyph of their own: a marker sized by the
        // particles' mean spacing (∛(volume / N), as the splats are sized), not
        // the catalogue glyph's size, which round a particle is a large sphere.
        double b[6];
        this->sourcePoints->GetBounds(b);
        const double dims[3] = { b[1] - b[0], b[3] - b[2], b[5] - b[4] };
        const double longest = std::max({ dims[0], dims[1], dims[2], 1.0e-6 });
        double volume = 1.0;
        for (const double d : dims)
            volume *= std::max(d, longest * 1.0e-3);
        const double n = std::max<double>(1.0, static_cast<double>(this->entries.size()));
        // Under one spacing: it marks the particle, it does not cover its
        // neighbours.
        return 0.6 * std::cbrt(volume / n) * radiusFactor;
    }
    double tuple[3] = { defaultGlyphRadius, defaultGlyphRadius, defaultGlyphRadius };
    if (idx >= 0 && idx < this->glyphScaleVectors->GetNumberOfTuples())
        this->glyphScaleVectors->GetTuple(idx, tuple);
    return std::max({ tuple[0], tuple[1], tuple[2] }) * radiusFactor;
}

void vtkWindowCatalogue3D::updateHighlightSphere(vtkSphereSource *sphere, vtkActor *actor,
                                                  int idx, double radiusFactor)
{
    if (idx < 0 || static_cast<std::size_t>(idx) >= this->mappedPositions.size()) {
        actor->VisibilityOff();
        return;
    }
    const auto &position = this->mappedPositions[static_cast<std::size_t>(idx)];
    sphere->SetCenter(position[0], position[1], position[2]);
    sphere->SetRadius(this->highlightRadius(idx, radiusFactor));
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
    // A selection the user made is told to the other SAMP tools by the row it
    // has in the original table; one a SAMP message made is not sent back.
    if (idx >= 0 && !this->m_applyingSampSelection) {
        const QString key = this->sampTableKey();
        this->ensureSampRowMap();
        const qint64 row = this->m_sampRowMap.rowFor(idx);
        if (!key.isEmpty() && row >= 0)
            emit this->sampRowActivated(key, row);
        // A line about an earlier selection ("TOPCAT highlighted row 12.")
        // would now describe a source that is no longer the selected one.
        if (!this->m_selectionStatus.isEmpty()
            && ui->labelStatus->text() == this->m_selectionStatus) {
            namespace cp = Catalogue3DParser::detail;
            const Catalogue3DEntry &e = this->entries[static_cast<std::size_t>(idx)];
            const bool named =
                    !cp::fieldString(e, this->schema, cp::detectedNameField(this->schema)).isEmpty();
            const QString name = cp::entryDisplayName(e, this->schema);   // "Row N" unnamed
            this->showSelectionStatus(named && row >= 0
                                              ? u"Selected %1 (row %2)."_s.arg(name).arg(row + 1)
                                              : u"Selected %1."_s.arg(name));
        }
    } else if (idx < 0 && !this->m_selectionStatus.isEmpty()
               && ui->labelStatus->text() == this->m_selectionStatus) {
        // Deselected: the line must not keep naming the old source.
        this->showSelectionStatus(u"No source selected."_s);
    }
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
    // Which row of the table it is, as the status line and TOPCAT number it
    // (an unnamed source is already called by it).
    if (e.sourceRow >= 0
        && !Catalogue3DParser::detail::fieldString(
                    e, this->schema, Catalogue3DParser::detail::detectedNameField(this->schema))
                    .isEmpty())
        html += QStringLiteral("<br>Row: %1").arg(e.sourceRow + 1);
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
    gaussianMapper->SetScalarVisibility(checked ? 1 : 0);
    if (!checked) {
        glyphActor->GetProperty()->SetColor(0.75, 0.75, 0.85);
        pointsActor->GetProperty()->SetColor(0.75, 0.75, 0.85);
        gaussianActor->GetProperty()->SetColor(0.75, 0.75, 0.85);
    }
    if (checked) {
        this->updateColorMapping();
    }
    this->refreshColorbar();
    if (!entries.empty())
        renderWindow->Render();
    this->pushRemoteState();
}

void vtkWindowCatalogue3D::scaleChanged(int value)
{
    if (entries.empty())
        return;
    Q_UNUSED(value);
    this->updateGlyphScales();
    if (this->geometryMode == GeometryMode::Gaussian)
        this->updateGaussianSplats();
    renderWindow->Render();
    this->pushRemoteState();
}

void vtkWindowCatalogue3D::resetCamera()
{
    renderer->ResetCamera();
    renderWindow->Render();
    if (this->m_remoteView)
        this->m_remoteView->send({ { u"type"_s, u"reset"_s } });
}

// Match a copilot field name against a comboMapColor item. Its item texts are
// "raw:<HEADER>" for data columns and friendly labels ("Morphology color",
// "Speed |v|") for the built-ins; accept the header with or without the "raw:"
// prefix, case-insensitively.
static int catalogueColorFieldIndex(QComboBox *combo, const QString &field)
{
    if (!combo)
        return -1;
    // Prefer a real data column ("raw:<HEADER>") so a column named like a built-in
    // label (e.g. a column literally called "Speed |v|") wins over the sentinel.
    for (int i = 0; i < combo->count(); ++i) {
        const QString text = combo->itemText(i);
        if (text.startsWith(u"raw:"_s, Qt::CaseInsensitive)
            && text.mid(4).compare(field, Qt::CaseInsensitive) == 0)
            return i;
    }
    // Fall back to a friendly built-in label (or an unprefixed exact match).
    for (int i = 0; i < combo->count(); ++i) {
        if (combo->itemText(i).compare(field, Qt::CaseInsensitive) == 0)
            return i;
    }
    return -1;
}

void vtkWindowCatalogue3D::applyCopilotActions(const QList<QJsonObject> &actions)
{
    // A point catalogue supports colour scale (Linear/Log/…), colour-by-field and
    // camera control; cube/image-only directives are ignored (harmless no-op).
    bool batchSetsScale = false;
    bool batchSetsColorField = false;
    bool batchResetsCamera = false;
    for (const QJsonObject &a : actions) {
        const QString t = a.value(u"type"_s).toString();
        if (t == u"set_scaling"_s)
            batchSetsScale = true;
        else if (t == u"set_color_field"_s)
            batchSetsColorField = true;
        else if (t == u"reset_camera"_s)
            batchResetsCamera = true;
    }
    if (!batchSetsScale && !batchSetsColorField && !batchResetsCamera)
        return;   // nothing applicable — leave any prior Undo point untouched

    // Fresh snapshot of only what this batch touches (per-batch "Undo latest").
    vtkCamera *camera = this->renderer ? this->renderer->GetActiveCamera() : nullptr;
    this->m_copilotUndo = CopilotUndo{};
    if (batchSetsScale && this->comboColorScale) {
        this->m_copilotUndo.scaleText = this->comboColorScale->currentText();
        this->m_copilotUndo.scaleCaptured = true;
    }
    if (batchSetsColorField && this->comboMapColor) {
        this->m_copilotUndo.colorFieldText = this->comboMapColor->currentText();
        this->m_copilotUndo.colorFieldCaptured = true;
    }
    if (batchResetsCamera && camera) {
        camera->GetPosition(this->m_copilotUndo.position);
        camera->GetFocalPoint(this->m_copilotUndo.focalPoint);
        camera->GetViewUp(this->m_copilotUndo.viewUp);
        this->m_copilotUndo.parallelScale = camera->GetParallelScale();
        this->m_copilotUndo.cameraCaptured = true;
    }

    for (const QJsonObject &a : actions) {
        const QString type = a.value(u"type"_s).toString();
        if (type == u"set_scaling"_s && this->comboColorScale) {
            const QString scale = a.value(u"scale"_s).toString().trimmed();
            const int idx = this->comboColorScale->findText(scale, Qt::MatchFixedString);
            if (idx >= 0)
                this->comboColorScale->setCurrentIndex(idx);   // triggers recolour
        } else if (type == u"set_color_field"_s && this->comboMapColor) {
            const int idx = catalogueColorFieldIndex(
                    this->comboMapColor, a.value(u"field"_s).toString().trimmed());
            if (idx >= 0)
                this->comboMapColor->setCurrentIndex(idx);     // triggers recolour
        } else if (type == u"reset_camera"_s && camera) {
            this->resetCamera();
        }
    }
}

void vtkWindowCatalogue3D::undoCopilotActions()
{
    if (this->m_copilotUndo.scaleCaptured && this->comboColorScale) {
        const int idx = this->comboColorScale->findText(this->m_copilotUndo.scaleText,
                                                        Qt::MatchFixedString);
        if (idx >= 0)
            this->comboColorScale->setCurrentIndex(idx);
        this->m_copilotUndo.scaleCaptured = false;
    }
    if (this->m_copilotUndo.colorFieldCaptured && this->comboMapColor) {
        const int idx = this->comboMapColor->findText(this->m_copilotUndo.colorFieldText);
        if (idx >= 0)
            this->comboMapColor->setCurrentIndex(idx);
        this->m_copilotUndo.colorFieldCaptured = false;
    }
    vtkCamera *camera = this->renderer ? this->renderer->GetActiveCamera() : nullptr;
    if (camera && this->m_copilotUndo.cameraCaptured) {
        camera->SetPosition(this->m_copilotUndo.position);
        camera->SetFocalPoint(this->m_copilotUndo.focalPoint);
        camera->SetViewUp(this->m_copilotUndo.viewUp);
        camera->SetParallelScale(this->m_copilotUndo.parallelScale);
        this->m_copilotUndo.cameraCaptured = false;
        if (this->renderer)
            this->renderer->ResetCameraClippingRange();
        if (this->renderWindow)
            this->renderWindow->Render();
    }
}

void vtkWindowCatalogue3D::updateGeometryMode()
{
    // By the item's text, not its index: "Arrows (velocity)" comes and goes
    // with the velocity columns.
    const QString shape = this->comboGeometry ? this->comboGeometry->currentText() : QString();
    if (shape == u"Sphere"_s)
        this->geometryMode = GeometryMode::Sphere;
    else if (shape == u"Point"_s)
        this->geometryMode = GeometryMode::Point;
    else if (shape == u"Cross"_s)
        this->geometryMode = GeometryMode::Cross;
    else if (shape == u"Gaussian splats"_s)
        this->geometryMode = GeometryMode::Gaussian;
    else if (shape == u"Arrows (velocity)"_s)
        this->geometryMode = GeometryMode::Velocity;
    else
        this->geometryMode = GeometryMode::Ellipsoid;
    // Ray-traced light overrides the shape while it is on; the combo keeps the
    // shape to return to. A table not open on the backend cannot have it: the
    // check goes back off here, before anything is changed for it (a custom
    // cosmology dropped for a view that will not come).
    if (this->m_rtLightCheck && this->m_rtLightCheck->isChecked()) {
        if (this->remoteDatasetId.isEmpty() || !this->m_renderStack) {
            const QSignalBlocker block(this->m_rtLightCheck);
            this->m_rtLightCheck->setChecked(false);
            this->statusBar()->showMessage(
                    u"The ray-traced view is prepared by the backend, and this table is not "
                    u"open there."_s, 6000);
        } else {
            this->geometryMode = GeometryMode::Raytraced;
        }
    }
    // Ray-traced: the view is drawn on the GPU, the shapes stay as they were.
    if (this->geometryMode != GeometryMode::Raytraced)
        this->updateGlyphPipeline();
    else
        this->dropCustomCosmologyForServerView(u"The ray-traced view"_s);
    // Points and splats are all one size, the slider's: no size column, and
    // the slider called what it sets. By the shape chosen, ray-traced or not:
    // while it is, these rows are only disabled (enablePointOnlyControls).
    const bool points = shape == u"Point"_s;
    const bool splats = shape == u"Gaussian splats"_s;
    if (this->m_encForm && this->comboSizeMode) {
        this->m_encForm->setRowVisible(this->comboSizeMode, !points && !splats);
        if (auto *label = qobject_cast<QLabel *>(this->m_encForm->labelForField(ui->sliderScale)))
            label->setText(points ? u"Point size"_s : splats ? u"Splat size"_s : u"Scale"_s);
    }
    this->setRaytraced(this->geometryMode == GeometryMode::Raytraced);
    this->pushRemoteState();
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
    this->pushRemoteState();
}

void vtkWindowCatalogue3D::toggleBoundingBox(bool checked)
{
    this->cubeAxesActor->SetVisibility(checked ? 1 : 0);
    this->renderWindow->Render();
    this->pushRemoteState();
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
        // A column the view shows: QTableView::scrollTo() does nothing for an
        // index in a hidden column, and column 0 (Name) is hidden for a table
        // without names (applyPointTableLayout), so the table never scrolled.
        int column = 0;
        const int columns = this->catalogueTableModel->columnCount();
        while (column < columns && this->catalogueTableView->isColumnHidden(column))
            ++column;
        if (column >= columns)
            column = 0;
        const QModelIndex modelIndex = this->catalogueTableModel->index(index, column);
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
