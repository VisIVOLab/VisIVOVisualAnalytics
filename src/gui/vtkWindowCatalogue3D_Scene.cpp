#include "vtkWindowCatalogue3D.h"
#include "ui_vtkWindowCatalogue3D.h"

#include "LutScale.h"
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
#include <QDoubleSpinBox>
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

// ── libwcs coordinate conversion ──────────────────────────────────────────────
// wcscon() converts between WCS coordinate systems (FK5/J2000, Galactic, …).
// Only the declaration is needed here; the function is linked from libwcs.
extern "C" {
void wcscon(int sys1, int sys2, double eq1, double eq2,
            double *dtheta, double *dphi, double epoch);
}
static constexpr int kWCS_J2000    = 1;
static constexpr int kWCS_GALACTIC = 3;

using namespace Qt::StringLiterals;

// ── Scene ──
// Extracted verbatim from vtkWindowCatalogue3D.cpp.

namespace {
const std::array<std::array<double, 3>, 10> kPalette = { {
        { 0.58, 0.18, 0.92 }, // 0 – halo        – purple
        { 1.00, 0.50, 0.00 }, // 1 – relic        – orange
        { 0.00, 0.82, 0.90 }, // 2 – mini-halo    – cyan
        { 0.70, 0.90, 0.10 }, // 3 – double relic – lime
        { 0.90, 0.25, 0.25 }, // 4 – phoenix      – red
        { 0.20, 0.75, 0.35 }, // 5 – extended     – green
        { 0.95, 0.80, 0.00 }, // 6 – diffuse      – gold
        { 0.30, 0.55, 0.95 }, // 7 – candidate    – blue
        { 0.80, 0.45, 0.80 }, // 8 – unclear      – pink
        { 0.70, 0.70, 0.70 }, // 9 – other/fallback – gray
} };

const QMap<QString, int> kKnownMorphologies = {
        { QStringLiteral("halo"),         0 },
        { QStringLiteral("relic"),        1 },
        { QStringLiteral("mini-halo"),    2 },
        { QStringLiteral("minihalo"),     2 },
        { QStringLiteral("mini halo"),    2 },
        { QStringLiteral("double relic"), 3 },
        { QStringLiteral("doublerelic"),  3 },
        { QStringLiteral("phoenix"),      4 },
        { QStringLiteral("extended"),     5 },
        { QStringLiteral("diffuse"),      6 },
        { QStringLiteral("candidate"),    7 },
        { QStringLiteral("unclear"),      8 },
        { QStringLiteral("other"),        9 },
};
}  // namespace


std::array<double, 3> vtkWindowCatalogue3D::morphologyColor(const QString &morph,
                                                             int fallbackIndex)
{
    if (kKnownMorphologies.contains(morph))
        return kPalette[kKnownMorphologies[morph]];
    return kPalette[fallbackIndex % static_cast<int>(kPalette.size())];
}

void vtkWindowCatalogue3D::buildMorphologyLut()
{
    // Collect ordered unique morphologies preserving first-seen order.
    for (const auto &e : entries) {
        const QString morph = Catalogue3DParser::detail::entryMorphology(e, this->schema);
        if (!morphologyIndexOf.contains(morph)) {
            morphologyIndexOf[morph] = morphologyNames.size();
            morphologyNames.append(morph);
        }
    }

    const int n = morphologyNames.size();
    morphologyLut->SetNumberOfTableValues(n);
    // Range: use [-0.5, n-0.5] so each integer index falls in its own bin.
    morphologyLut->SetTableRange(-0.5, n - 0.5);

    for (int i = 0; i < n; ++i) {
        const auto [r, g, b] = morphologyColor(morphologyNames.at(i), i);
        morphologyLut->SetTableValue(i, r, g, b, 1.0);
    }
    morphologyLut->Build();

    this->valueLut->SetNumberOfTableValues(256);
    this->valueLut->SetHueRange(0.67, 0.0);
    this->valueLut->SetSaturationRange(0.85, 0.95);
    this->valueLut->SetValueRange(0.9, 1.0);
    this->valueLut->Build();
}

void vtkWindowCatalogue3D::buildScene()
{
    buildMorphologyLut();
    this->mappedPositions.resize(entries.size());

    // ── Populate point cloud ──────────────────────────────────────────────────
    sourcePoints->SetNumberOfPoints(static_cast<vtkIdType>(entries.size()));

    morphScalars->SetName("Morphology");
    morphScalars->SetNumberOfValues(static_cast<vtkIdType>(entries.size()));

    // Compute bounding box to derive a sensible default glyph radius.
    double xMin = std::numeric_limits<double>::max();
    double yMin = xMin, zMin = xMin;
    double xMax = std::numeric_limits<double>::lowest();
    double yMax = xMax, zMax = xMax;

    maxDistanceMpc = 1.0;
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        const auto &e = entries[i];
        morphScalars->SetValue(i, morphologyIndexOf.value(
                                          Catalogue3DParser::detail::entryMorphology(e, this->schema), 0));

        xMin = std::min(xMin, e.sceneX); xMax = std::max(xMax, e.sceneX);
        yMin = std::min(yMin, e.sceneY); yMax = std::max(yMax, e.sceneY);
        zMin = std::min(zMin, e.sceneZ); zMax = std::max(zMax, e.sceneZ);
        maxDistanceMpc = std::max(maxDistanceMpc,
                                  Catalogue3DParser::detail::entryDistanceMpc(e, this->schema));
    }

    const double diag = std::sqrt((xMax - xMin) * (xMax - xMin)
                                  + (yMax - yMin) * (yMax - yMin)
                                  + (zMax - zMin) * (zMax - zMin));
    // Default glyph radius ≈ 1.2 % of diagonal so fields remain readable.
    defaultGlyphRadius = std::max(0.5, diag * 0.012);
    sizeNormalizationValue = 1.0;

    // Vertices cell array so vtkGlyph3D recognises each point.
    vtkNew<vtkCellArray> verts;
    for (vtkIdType i = 0; i < static_cast<vtkIdType>(entries.size()); ++i)
        verts->InsertNextCell(1, &i);

    sourcesPolyData->SetPoints(sourcePoints);
    sourcesPolyData->SetVerts(verts);
    sourcesPolyData->GetPointData()->SetScalars(morphScalars);
    this->glyphScaleVectors->SetName("ScaleVector");
    this->glyphScaleVectors->SetNumberOfComponents(3);
    this->glyphScaleVectors->SetNumberOfTuples(static_cast<vtkIdType>(entries.size()));
    sourcesPolyData->GetPointData()->SetVectors(this->glyphScaleVectors);

    // ── Glyph pipeline ────────────────────────────────────────────────────────
    sphereSource->SetRadius(0.5); // unit sphere; scaled by SetScaleFactor
    sphereSource->SetPhiResolution(10);
    sphereSource->SetThetaResolution(10);
    pointCubeSource->SetXLength(1.0);
    pointCubeSource->SetYLength(1.0);
    pointCubeSource->SetZLength(1.0);
    vtkNew<vtkPoints> crossPoints;
    vtkNew<vtkCellArray> crossCells;
    const std::array<std::array<double, 3>, 6> crossVertices{ { { -0.5, 0.0, 0.0 },
                                                                 { 0.5, 0.0, 0.0 },
                                                                 { 0.0, -0.5, 0.0 },
                                                                 { 0.0, 0.5, 0.0 },
                                                                 { 0.0, 0.0, -0.5 },
                                                                 { 0.0, 0.0, 0.5 } } };
    for (const auto &v : crossVertices) {
        crossPoints->InsertNextPoint(v[0], v[1], v[2]);
    }
    const std::array<std::array<vtkIdType, 2>, 3> crossLines{ { { 0, 1 }, { 2, 3 }, { 4, 5 } } };
    for (const auto &line : crossLines) {
        crossCells->InsertNextCell(2);
        crossCells->InsertCellPoint(line[0]);
        crossCells->InsertCellPoint(line[1]);
    }
    vtkNew<vtkPolyData> crossData;
    crossData->SetPoints(crossPoints);
    crossData->SetLines(crossCells);
    this->crossSource->RemoveAllInputs();
    this->crossSource->AddInputData(crossData);

    // GPU-instanced glyphs: the mapper consumes the point cloud directly and
    // replicates the sphere per point on the GPU — no CPU geometry explosion,
    // so large catalogues stay interactive. (Picking is independent — it is a
    // CPU nearest-source search over the entry list, see pickNearestSource.)
    const int nMorph = morphologyNames.size();
    glyphMapper->SetInputData(sourcesPolyData);
    glyphMapper->SetSourceConnection(sphereSource->GetOutputPort());
    glyphMapper->SetScaleArray("ScaleVector");
    glyphMapper->SetScaleModeToScaleByVectorComponents();  // per-axis → ellipsoid
    glyphMapper->OrientOff();
    glyphMapper->SetScaleFactor(1.0);
    glyphMapper->SetLookupTable(morphologyLut);
    glyphMapper->SetScalarRange(-0.5, nMorph - 0.5);
    glyphMapper->SetColorModeToMapScalars();
    glyphMapper->SetScalarModeToUsePointData();   // colour by the active point scalars
    glyphMapper->ScalarVisibilityOn();

    glyphActor->SetMapper(glyphMapper);
    renderer->AddActor(glyphActor);
    this->pointsMapper->SetInputData(sourcesPolyData);
    this->pointsMapper->SetLookupTable(this->morphologyLut);
    this->pointsMapper->SetScalarRange(-0.5, nMorph - 0.5);
    this->pointsMapper->ScalarVisibilityOn();
    this->pointsActor->SetMapper(this->pointsMapper);
    this->pointsActor->GetProperty()->SetPointSize(4.0);
    this->pointsActor->GetProperty()->SetColor(0.78, 0.82, 0.95);
    this->pointsActor->VisibilityOff();
    renderer->AddActor(this->pointsActor);

    // ── Hover highlight (yellow wireframe) ────────────────────────────────────
    hoverSphere->SetRadius(defaultGlyphRadius * 1.35);
    hoverSphere->SetPhiResolution(12);
    hoverSphere->SetThetaResolution(12);

    hoverMapper->SetInputConnection(hoverSphere->GetOutputPort());
    hoverActor->SetMapper(hoverMapper);
    hoverActor->GetProperty()->SetRepresentationToWireframe();
    hoverActor->GetProperty()->SetColor(1.0, 0.95, 0.0); // yellow
    hoverActor->GetProperty()->SetLineWidth(1.8);
    hoverActor->VisibilityOff();
    renderer->AddActor(hoverActor);

    // ── Selection highlight (bright white wireframe) ───────────────────────────
    selectSphere->SetRadius(defaultGlyphRadius * 1.18);
    selectSphere->SetPhiResolution(12);
    selectSphere->SetThetaResolution(12);

    selectMapper->SetInputConnection(selectSphere->GetOutputPort());
    selectActor->SetMapper(selectMapper);
    selectActor->GetProperty()->SetRepresentationToWireframe();
    selectActor->GetProperty()->SetColor(1.0, 1.0, 1.0); // white
    selectActor->GetProperty()->SetLineWidth(2.2);
    selectActor->VisibilityOff();
    renderer->AddActor(selectActor);

    // ── Bounding-box coordinate axes ──────────────────────────────────────────
    const double bounds[6] = { xMin, xMax, yMin, yMax, zMin, zMax };
    cubeAxesActor->SetBounds(bounds);
    cubeAxesActor->SetCamera(renderer->GetActiveCamera());
    cubeAxesActor->SetXTitle("X (Mpc)");
    cubeAxesActor->SetYTitle("Y (Mpc)");
    cubeAxesActor->SetZTitle("Z (Mpc)");
    cubeAxesActor->SetXUnits("");
    cubeAxesActor->SetYUnits("");
    cubeAxesActor->SetZUnits("");
    // Axis colours: red / green / blue
    for (int axis = 0; axis < 3; ++axis) {
        cubeAxesActor->GetTitleTextProperty(axis)->SetColor(
                axis == 0 ? 1.0 : 0.5, axis == 1 ? 1.0 : 0.5, axis == 2 ? 1.0 : 0.5);
        cubeAxesActor->GetLabelTextProperty(axis)->SetColor(0.8, 0.8, 0.8);
    }
    cubeAxesActor->SetFlyModeToOuterEdges();
    cubeAxesActor->DrawXGridlinesOff();
    cubeAxesActor->DrawYGridlinesOff();
    cubeAxesActor->DrawZGridlinesOff();
    renderer->AddActor(cubeAxesActor);
    cubeAxesActor->SetVisibility(this->chkShowBoundingBox && this->chkShowBoundingBox->isChecked() ? 1 : 0);

    this->buildShells();
    this->updatePointPositions();
    this->updateColorMapping();
    this->updateGlyphPipeline();

    renderer->ResetCamera();
    renderWindow->Render();
}

void vtkWindowCatalogue3D::buildLabels()
{
    // Per-point billboard text actors do NOT scale: creating tens of thousands
    // of them (and adding each to the renderer) hangs the GUI thread for minutes,
    // and that many on-screen labels are visually useless anyway. Skip building
    // them above a cap — labels stay available for normal-sized catalogues, and
    // hover/selection still show the source name on demand.
    constexpr std::size_t kMaxLabelEntries = 2000;
    this->m_labelsSkippedDueToScale = (entries.size() > kMaxLabelEntries);
    if (this->m_labelsSkippedDueToScale) {
        qInfo() << "[catalogue3d] per-point labels skipped for" << entries.size()
                << "sources (cap" << kMaxLabelEntries << ") — not built at this scale";
        return;
    }
    for (const auto &e : entries) {
        auto actor = vtkSmartPointer<vtkBillboardTextActor3D>::New();
        actor->SetInput(Catalogue3DParser::detail::entryDisplayName(e, this->schema).toUtf8().constData());
        actor->SetPosition(e.sceneX, e.sceneY, e.sceneZ);
        actor->GetTextProperty()->SetFontSize(11);
        actor->GetTextProperty()->SetColor(1.0, 1.0, 1.0);
        actor->GetTextProperty()->ShadowOn();
        actor->GetTextProperty()->SetShadowOffset(1, -1);
        actor->SetVisibility(0); // hidden until user enables
        labelActors.push_back(actor);
        renderer->AddActor(actor);
    }
}

void vtkWindowCatalogue3D::buildShells()
{
    for (const auto &actor : this->shellActors) {
        this->renderer->RemoveActor(actor);
    }
    this->shellActors.clear();
    if (entries.empty()) {
        return;
    }

    const std::array<double, 3> shellFractions{ { 0.33, 0.66, 1.0 } };
    for (double fraction : shellFractions) {
        vtkNew<vtkSphereSource> shellSource;
        shellSource->SetRadius(std::max(1.0, this->maxDistanceMpc * fraction));
        shellSource->SetPhiResolution(48);
        shellSource->SetThetaResolution(48);
        vtkNew<vtkPolyDataMapper> shellMapper;
        shellMapper->SetInputConnection(shellSource->GetOutputPort());
        auto shellActor = vtkSmartPointer<vtkActor>::New();
        shellActor->SetMapper(shellMapper);
        shellActor->GetProperty()->SetRepresentationToWireframe();
        shellActor->GetProperty()->SetColor(0.42, 0.5, 0.66);
        shellActor->GetProperty()->SetOpacity(0.2);
        shellActor->VisibilityOff();
        this->renderer->AddActor(shellActor);
        this->shellActors.push_back(shellActor);
    }
}

QStringList vtkWindowCatalogue3D::numericRawFieldNames() const
{
    QStringList items;
    for (const int index : this->numericRawFieldIndices) {
        if (index >= 0 && index < this->rawHeaders.size()) {
            items.append(this->rawHeaders.at(index));
        }
    }
    return items;
}

bool vtkWindowCatalogue3D::rawFieldValue(const Catalogue3DEntry &entry, int fieldIndex,
                                         double &value) const
{
    if (fieldIndex < 0 || fieldIndex >= entry.rawFieldValues.size()) {
        return false;
    }
    return Catalogue3DParser::detail::maybeNumeric(entry.rawFieldValues.at(fieldIndex), value);
}

double vtkWindowCatalogue3D::mappedAxisValue(const Catalogue3DEntry &entry, const QString &mappingKey) const
{
    if (mappingKey == QStringLiteral("computed:x")) {
        return entry.sceneX;
    }
    if (mappingKey == QStringLiteral("computed:y")) {
        return entry.sceneY;
    }
    if (mappingKey == QStringLiteral("computed:z")) {
        return entry.sceneZ;
    }
    if (mappingKey.startsWith(QStringLiteral("raw:"))) {
        bool okIndex = false;
        const int index = mappingKey.mid(4).toInt(&okIndex);
        double value = 0.0;
        if (okIndex && this->rawFieldValue(entry, index, value)) {
            return value;
        }
    }
    return 0.0;
}

double vtkWindowCatalogue3D::mappedSizeValue(const Catalogue3DEntry &entry, bool &ok) const
{
    ok = true;
    if (this->mapSizeKey == QStringLiteral("builtin:fixed")) {
        return 1.0;
    }
    if (this->mapSizeKey == QStringLiteral("builtin:major")) {
        const double value = Catalogue3DParser::detail::entryMajorAxisArcmin(entry, this->schema);
        if (value > 0.0) {
            return value;
        }
        ok = false;
        return 1.0;
    }
    if (this->mapSizeKey == QStringLiteral("builtin:lls")) {
        const double value = Catalogue3DParser::detail::entryLlsMajorKpc(entry, this->schema);
        if (value > 0.0) {
            return value;
        }
        ok = false;
        return 1.0;
    }
    if (this->mapSizeKey == QStringLiteral("builtin:flux")) {
        const double value = Catalogue3DParser::detail::entryFluxMJy(entry, this->schema);
        if (value > 0.0) {
            return value;
        }
        ok = false;
        return 1.0;
    }
    if (this->mapSizeKey.startsWith(QStringLiteral("raw:"))) {
        bool okIndex = false;
        const int index = this->mapSizeKey.mid(4).toInt(&okIndex);
        double value = 0.0;
        if (okIndex && this->rawFieldValue(entry, index, value) && value > 0.0) {
            return value;
        }
        ok = false;
        return 1.0;
    }
    ok = false;
    return 1.0;
}

double vtkWindowCatalogue3D::mappedColorValue(const Catalogue3DEntry &entry, bool &ok) const
{
    ok = true;
    if (this->mapColorKey.startsWith(QStringLiteral("raw:"))) {
        bool okIndex = false;
        const int index = this->mapColorKey.mid(4).toInt(&okIndex);
        double value = 0.0;
        if (okIndex && this->rawFieldValue(entry, index, value)) {
            return value;
        }
        ok = false;
        return 0.0;
    }
    if (this->mapColorKey == QStringLiteral("builtin:speed")) {
        double vx = 0.0, vy = 0.0, vz = 0.0;
        if (this->m_velocityColIdx[0] >= 0 && this->m_velocityColIdx[1] >= 0
            && this->m_velocityColIdx[2] >= 0
            && this->rawFieldValue(entry, this->m_velocityColIdx[0], vx)
            && this->rawFieldValue(entry, this->m_velocityColIdx[1], vy)
            && this->rawFieldValue(entry, this->m_velocityColIdx[2], vz)) {
            return std::sqrt(vx * vx + vy * vy + vz * vz);
        }
        ok = false;
        return 0.0;
    }
    ok = false;
    return 0.0;
}

QString vtkWindowCatalogue3D::fallbackDetailsText(const Catalogue3DEntry &entry) const
{
    QString text;
    for (int i = 0; i < this->rawHeaders.size() && i < entry.rawFieldValues.size(); ++i) {
        const QString value = entry.rawFieldValues.at(i).trimmed();
        if (value.isEmpty()) {
            continue;
        }
        text += QStringLiteral("%1: %2\n").arg(this->rawHeaders.at(i), value);
    }
    return text.trimmed();
}

void vtkWindowCatalogue3D::updatePointPositions()
{
    if (this->entries.empty()) {
        return;
    }

    const auto validatedAxisKey = [this](const QString &requestedKey, const QString &fallbackKey) {
        if (!requestedKey.startsWith(QStringLiteral("raw:"))) {
            return requestedKey;
        }
        int validCount = 0;
        double minValue = std::numeric_limits<double>::max();
        double maxValue = std::numeric_limits<double>::lowest();
        for (const auto &entry : this->entries) {
            const double value = this->mappedAxisValue(entry, requestedKey);
            if (!std::isfinite(value)) {
                continue;
            }
            ++validCount;
            minValue = std::min(minValue, value);
            maxValue = std::max(maxValue, value);
        }
        if (validCount < 3 || !(minValue < maxValue)) {
            qWarning() << "[catalogue3d] invalid axis mapping" << requestedKey
                       << "-> fallback to" << fallbackKey;
            return fallbackKey;
        }
        return requestedKey;
    };

    const QString resolvedXKey = validatedAxisKey(this->mapXKey, QStringLiteral("computed:x"));
    const QString resolvedYKey = validatedAxisKey(this->mapYKey, QStringLiteral("computed:y"));
    const QString resolvedZKey = validatedAxisKey(this->mapZKey, QStringLiteral("computed:z"));

    double xMin = std::numeric_limits<double>::max();
    double yMin = xMin;
    double zMin = xMin;
    double xMax = std::numeric_limits<double>::lowest();
    double yMax = xMax;
    double zMax = xMax;

    for (vtkIdType i = 0; i < static_cast<vtkIdType>(this->entries.size()); ++i) {
        const auto &entry = this->entries[static_cast<std::size_t>(i)];
        const double x = this->mappedAxisValue(entry, resolvedXKey);
        const double y = this->mappedAxisValue(entry, resolvedYKey);
        const double z = this->mappedAxisValue(entry, resolvedZKey);
        this->mappedPositions[static_cast<std::size_t>(i)] = { x, y, z };
        this->sourcePoints->SetPoint(i, x, y, z);
        xMin = std::min(xMin, x); xMax = std::max(xMax, x);
        yMin = std::min(yMin, y); yMax = std::max(yMax, y);
        zMin = std::min(zMin, z); zMax = std::max(zMax, z);
        if (i < static_cast<vtkIdType>(this->labelActors.size())) {
            this->labelActors[static_cast<std::size_t>(i)]->SetPosition(x, y, z);
        }
    }
    this->sourcePoints->Modified();
    this->sourcesPolyData->Modified();
    const double bounds[6] = { xMin, xMax, yMin, yMax, zMin, zMax };
    this->cubeAxesActor->SetBounds(bounds);
    this->renderer->ResetCameraClippingRange();
    this->updateHighlightSphere(this->hoverSphere.Get(), this->hoverActor.Get(), this->hoveredIndex, 1.35);
    this->updateHighlightSphere(this->selectSphere.Get(), this->selectActor.Get(), this->selectedIndex, 1.18);
    qInfo() << "[catalogue3d] axis mapping updated"
            << "X=" << this->mapXKey << "resolved=" << resolvedXKey
            << "Y=" << this->mapYKey << "resolved=" << resolvedYKey
            << "Z=" << this->mapZKey << "resolved=" << resolvedZKey;
}

void vtkWindowCatalogue3D::updateColorMapping()
{
    if (this->entries.empty()) {
        return;
    }

    // Gamma only applies to the continuous Power stretch; keep the spinbox state
    // correct regardless of which branch (incl. the morphology early-return) runs.
    const LutScale::Mode colorScale = this->comboColorScale
            ? static_cast<LutScale::Mode>(std::clamp(this->comboColorScale->currentIndex(), 0, 4))
            : LutScale::Mode::Linear;
    const double colorGamma = this->spinColorGamma ? this->spinColorGamma->value() : 2.0;
    if (this->spinColorGamma) {
        this->spinColorGamma->setEnabled(colorScale == LutScale::Mode::Power);
    }

    if (this->mapColorKey == QStringLiteral("builtin:morphology")) {
        for (vtkIdType i = 0; i < static_cast<vtkIdType>(this->entries.size()); ++i) {
            const auto &entry = this->entries[static_cast<std::size_t>(i)];
            this->morphScalars->SetValue(
                    i, this->morphologyIndexOf.value(
                               Catalogue3DParser::detail::entryMorphology(entry, this->schema), 0));
        }
        this->morphScalars->Modified();
        this->sourcesPolyData->GetPointData()->SetScalars(this->morphScalars);
        this->glyphMapper->SetLookupTable(this->morphologyLut);
        this->glyphMapper->SetScalarRange(-0.5, this->morphologyNames.size() - 0.5);
        this->pointsMapper->SetLookupTable(this->morphologyLut);
        this->pointsMapper->SetScalarRange(-0.5, this->morphologyNames.size() - 0.5);
        qInfo() << "[catalogue3d] color mapping: morphology";
        return;
    }

    double minValue = std::numeric_limits<double>::max();
    double maxValue = std::numeric_limits<double>::lowest();
    bool anyValid = false;
    for (vtkIdType i = 0; i < static_cast<vtkIdType>(this->entries.size()); ++i) {
        bool ok = false;
        const double value = this->mappedColorValue(this->entries[static_cast<std::size_t>(i)], ok);
        if (!ok || !std::isfinite(value)) {
            this->morphScalars->SetValue(i, 0.0);
            continue;
        }
        this->morphScalars->SetValue(i, value);
        minValue = std::min(minValue, value);
        maxValue = std::max(maxValue, value);
        anyValid = true;
    }

    if (!anyValid || !(minValue < maxValue)) {
        qWarning() << "[catalogue3d] invalid color mapping" << this->mapColorKey
                   << "-> fallback to morphology";
        this->mapColorKey = QStringLiteral("builtin:morphology");
        if (this->comboMapColor) {
            QSignalBlocker blocker(this->comboMapColor);
            this->comboMapColor->setCurrentIndex(0);
        }
        this->updateColorMapping();
        return;
    }

    this->morphScalars->Modified();
    this->sourcesPolyData->GetPointData()->SetScalars(this->morphScalars);
    // Apply the chosen colour-scale stretch to the HSV value ramp. applyGenerated
    // uses ForceBuild(), so it stays idempotent across repeated mapping updates.
    LutScale::applyGenerated(this->valueLut, colorScale, minValue, maxValue, colorGamma);
    // Drive the mappers from the LUT's (possibly Log-floored) range — otherwise a
    // raw minValue<=0 would override the positive floor at render time.
    double mapperRange[2];
    this->valueLut->GetTableRange(mapperRange);
    this->glyphMapper->SetLookupTable(this->valueLut);
    this->glyphMapper->SetScalarRange(mapperRange[0], mapperRange[1]);
    this->pointsMapper->SetLookupTable(this->valueLut);
    this->pointsMapper->SetScalarRange(mapperRange[0], mapperRange[1]);
    qInfo() << "[catalogue3d] color mapping:" << this->mapColorKey
            << "range:" << mapperRange[0] << mapperRange[1];
}

void vtkWindowCatalogue3D::updateGlyphPipeline()
{
    if (entries.empty()) {
        return;
    }

    this->pointsActor->VisibilityOff();
    this->glyphActor->VisibilityOn();
    switch (this->geometryMode) {
    case GeometryMode::Ellipsoid:
    case GeometryMode::Sphere:
        this->glyphMapper->SetSourceConnection(this->sphereSource->GetOutputPort());
        break;
    case GeometryMode::Cross:
        this->glyphMapper->SetSourceConnection(this->crossSource->GetOutputPort());
        break;
    case GeometryMode::Point:
        this->glyphActor->VisibilityOff();
        this->pointsActor->VisibilityOn();
        break;
    }

    this->updateGlyphScales();
    this->renderWindow->Render();
}

void vtkWindowCatalogue3D::updateGlyphScales()
{
    if (entries.empty()) {
        return;
    }

    sizeNormalizationValue = 1.0;
    bool anyValid = false;
    for (const auto &entry : entries) {
        bool ok = false;
        const double value = this->mappedSizeValue(entry, ok);
        if (!ok || !std::isfinite(value) || value <= 0.0) {
            continue;
        }
        sizeNormalizationValue = std::max(sizeNormalizationValue, value);
        anyValid = true;
    }
    if (!anyValid) {
        qWarning() << "[catalogue3d] invalid size mapping" << this->mapSizeKey
                   << "-> fallback to fixed size";
        this->mapSizeKey = QStringLiteral("builtin:fixed");
        if (this->comboMapSize) {
            QSignalBlocker blocker(this->comboMapSize);
            this->comboMapSize->setCurrentIndex(0);
        }
        this->sizeMode = SizeMode::Fixed;
        sizeNormalizationValue = 1.0;
    }

    const double sliderScale = ui->sliderScale->value() / 100.0;
    double minVisual = std::numeric_limits<double>::max();
    double maxVisual = 0.0;
    for (vtkIdType i = 0; i < static_cast<vtkIdType>(entries.size()); ++i) {
        const Catalogue3DEntry &entry = entries[static_cast<std::size_t>(i)];
        bool ok = false;
        const double sourceValue = this->mappedSizeValue(entry, ok);
        const double normalized = ok && sourceValue > 0.0
                ? std::clamp(sourceValue / sizeNormalizationValue, 0.15, 1.6)
                : 0.35;
        double major = defaultGlyphRadius * sliderScale * normalized;
        double minor = major;
        double depth = major;

        if (this->geometryMode == GeometryMode::Ellipsoid) {
            const double majorAxisArcmin =
                    Catalogue3DParser::detail::entryMajorAxisArcmin(entry, this->schema);
            const double minorAxisArcmin =
                    Catalogue3DParser::detail::entryMinorAxisArcmin(entry, this->schema);
            const double llsMajorKpc =
                    Catalogue3DParser::detail::entryLlsMajorKpc(entry, this->schema);
            const double llsMinorKpc =
                    Catalogue3DParser::detail::entryLlsMinorKpc(entry, this->schema);
            if (this->mapSizeKey == QStringLiteral("builtin:major") && majorAxisArcmin > 0.0) {
                major = defaultGlyphRadius * sliderScale * std::max(0.15, majorAxisArcmin / sizeNormalizationValue);
                const double minorNorm = minorAxisArcmin > 0.0
                        ? std::max(0.12, minorAxisArcmin / sizeNormalizationValue)
                        : std::max(0.12, majorAxisArcmin / sizeNormalizationValue * 0.65);
                minor = defaultGlyphRadius * sliderScale * minorNorm;
                depth = minor;
            } else if (this->mapSizeKey == QStringLiteral("builtin:lls") && llsMajorKpc > 0.0) {
                major = defaultGlyphRadius * sliderScale * std::max(0.15, llsMajorKpc / sizeNormalizationValue);
                const double minorNorm = llsMinorKpc > 0.0
                        ? std::max(0.12, llsMinorKpc / sizeNormalizationValue)
                        : std::max(0.12, llsMajorKpc / sizeNormalizationValue * 0.7);
                minor = defaultGlyphRadius * sliderScale * minorNorm;
                depth = minor;
            }
        }

        if (this->geometryMode == GeometryMode::Sphere || this->geometryMode == GeometryMode::Point
            || this->geometryMode == GeometryMode::Cross) {
            major = minor = depth = defaultGlyphRadius * sliderScale * normalized;
        }

        this->glyphScaleVectors->SetTuple3(i, minor, major, depth);
        minVisual = std::min(minVisual, std::min({ minor, major, depth }));
        maxVisual = std::max(maxVisual, std::max({ minor, major, depth }));
    }
    this->glyphScaleVectors->Modified();
    this->sourcesPolyData->Modified();   // bump so the glyph mapper re-reads the scale vectors
    this->glyphMapper->Modified();
    this->pointsActor->GetProperty()->SetPointSize(std::clamp(4.0 * (ui->sliderScale->value() / 100.0), 3.0, 12.0));

    updateHighlightSphere(hoverSphere.Get(), hoverActor.Get(), hoveredIndex, 1.45);
    updateHighlightSphere(selectSphere.Get(), selectActor.Get(), selectedIndex, 1.28);
    qInfo() << "[catalogue3d] size mapping:" << this->mapSizeKey
            << "visual range:" << minVisual << maxVisual;
}
