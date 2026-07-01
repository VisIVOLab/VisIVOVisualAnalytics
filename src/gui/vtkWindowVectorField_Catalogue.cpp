#include "vtkWindowVectorField.h"

#include "Catalogue3DParser.h"
#include "RemoteFileBrowserDialog.h"
#include "app/BackendClient.h"

#include <vtkActor.h>
#include <vtkCellArray.h>
#include <vtkCoordinate.h>
#include <vtkDataArray.h>
#include <vtkFloatArray.h>
#include <vtkLookupTable.h>
#include <vtkNew.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkScalarBarActor.h>

#include <QComboBox>

#include <cmath>

#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStatusBar>

using namespace Qt::StringLiterals;

// ── CosmicFlows combo: overlay a galaxy catalogue in the velocity-field frame ──
// The velocity viewer already holds a live backend session; this reuses the
// catalogue open/query/parse path (same as vtkWindowCatalogue3D) to pull the
// XYZ positions and draws them as a plain GPU point cloud in the SAME renderer,
// so galaxies appear inside the flow. Only Cartesian (X/Y/Z Mpc) catalogues
// share this frame; sky catalogues are parsed to positions too but won't align.

namespace {
QString catalogueFormatForPath(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().trimmed().toLower();
    if (suffix == u"csv"_s)
        return u"csv"_s;
    if (suffix == u"vot"_s || suffix == u"votable"_s || suffix == u"xml"_s)
        return u"votable"_s;
    if (suffix == u"fits"_s || suffix == u"fit"_s)
        return u"fits"_s;
    if (suffix == u"speck"_s)
        return u"speck"_s;
    if (suffix == u"tbl"_s)
        return u"ipac"_s;
    return u"csv"_s;
}
}  // namespace

void vtkWindowVectorField::overlayCatalogue()
{
    // The field is only in physical Mpc once a box size is set; until then it is
    // in grid units and catalogue Mpc positions won't align. Warn up front.
    if (this->m_boxMpc <= 0.0) {
        const auto btn = QMessageBox::warning(
                this, u"Overlay Catalogue"_s,
                u"This velocity field is shown in grid units (Box size (Mpc) = 0), but a "
                u"galaxy catalogue is in physical Mpc — the two will not align until you set "
                u"“Box size (Mpc)” to the field's physical box.\n\nOverlay anyway?"_s,
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (btn != QMessageBox::Yes)
            return;
    }

    BackendClient client(this->m_backendUrl, this->m_backendToken);
    if (!this->m_sessionId.isEmpty())
        client.setSessionId(this->m_sessionId);

    RemoteFileBrowserDialog dialog(client.baseUrl(), client.token(), this);
    dialog.setExtensionFilter(
            { u".csv"_s, u".vot"_s, u".votable"_s, u".xml"_s, u".fits"_s, u".fit"_s,
              u".speck"_s, u".tbl"_s },
            u"Galaxy catalogues"_s);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QString path = dialog.selectedFilePath();
    if (path.isEmpty())
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    this->statusBar()->showMessage(u"Overlaying catalogue…"_s);

    const auto opened = client.openCatalogue(path, catalogueFormatForPath(path));
    if (!opened.valid) {
        QApplication::restoreOverrideCursor();
        QMessageBox::warning(this, u"Overlay Catalogue"_s,
                             u"Open failed: %1"_s.arg(opened.error.isEmpty()
                                                              ? u"unknown error"_s
                                                              : opened.error));
        return;
    }

    // Build the parser schema as the 3-D catalogue viewer does.
    Catalogue3DSchema schema;
    schema.raFieldName = opened.raField;
    schema.decFieldName = opened.decField;
    schema.xFieldName = opened.xField;
    schema.yFieldName = opened.yField;
    schema.zFieldName = opened.zField;
    schema.fields.reserve(opened.fields.size());
    for (const auto &f : opened.fields) {
        schema.fields.push_back({ f.name, f.type });
        schema.headers.append(f.name);
    }

    // For the flow overlay we want the physical Cartesian positions. Whenever
    // X/Y/Z are present (backend "cartesian" always implies them), force the
    // parser's Cartesian branch — otherwise a catalogue that ALSO carries RA/Dec
    // would be treated as sky and land on an RA/Dec-derived shell. Guarded on
    // haveXYZ so we never strip RA/Dec with no Cartesian columns to fall back to.
    const bool haveXYZ = !schema.xFieldName.isEmpty() && !schema.yFieldName.isEmpty()
            && !schema.zFieldName.isEmpty();
    if (haveXYZ) {
        schema.raFieldName.clear();
        schema.decFieldName.clear();
    }

    BackendTabularQueryRequest qreq;
    qreq.datasetId = opened.datasetId;   // limit defaults to 100k — plenty for context
    const auto subset = client.queryTabularCatalogue(qreq);
    const auto parsed = Catalogue3DParser::parseBackendSubset(schema, subset.rows);
    if (!subset.valid || !parsed.valid) {
        QApplication::restoreOverrideCursor();
        QMessageBox::warning(this, u"Overlay Catalogue"_s,
                             u"Load failed: %1"_s.arg(!subset.valid ? subset.error
                                                                    : parsed.errorMessage));
        return;
    }

    // Point cloud from the parsed scene positions (Cartesian = the X/Y/Z fields).
    vtkNew<vtkPoints> points;
    points->SetNumberOfPoints(static_cast<vtkIdType>(parsed.entries.size()));
    vtkNew<vtkCellArray> verts;
    verts->AllocateEstimate(static_cast<vtkIdType>(parsed.entries.size()), 1);
    for (vtkIdType i = 0; i < static_cast<vtkIdType>(parsed.entries.size()); ++i) {
        const auto &e = parsed.entries[static_cast<std::size_t>(i)];
        points->SetPoint(i, e.sceneX, e.sceneY, e.sceneZ);
        verts->InsertNextCell(1, &i);
    }
    vtkNew<vtkPolyData> poly;
    poly->SetPoints(points);
    poly->SetVerts(verts);

    // Attach a named scalar array per numeric column so the galaxies can be
    // coloured by any attribute without re-fetching. Field lookups reuse the
    // parser accessor (name → index via schema), so precision matches the viewer.
    const vtkIdType nPts = static_cast<vtkIdType>(parsed.entries.size());
    QStringList colourKeys;
    for (const auto &field : schema.fields) {
        const QString type = field.type.toLower();
        const bool numeric = type.contains(u"float"_s) || type.contains(u"double"_s)
                || type.contains(u"int"_s) || type.contains(u"real"_s);
        if (!numeric)
            continue;
        vtkNew<vtkFloatArray> arr;
        arr->SetName(field.name.toUtf8().constData());
        arr->SetNumberOfTuples(nPts);
        for (vtkIdType i = 0; i < nPts; ++i) {
            const double v = Catalogue3DParser::detail::fieldDouble(
                    parsed.entries[static_cast<std::size_t>(i)], schema, field.name);
            arr->SetValue(i, static_cast<float>(v));
        }
        poly->GetPointData()->AddArray(arr);
        colourKeys << field.name;
    }

    // Speed |v| from vx/vy/vz when present — the cosmic-flow attribute.
    int vxi = -1, vyi = -1, vzi = -1;
    for (int c = 0; c < schema.headers.size(); ++c) {
        const QString h = schema.headers.at(c).trimmed().toLower();
        if (h == u"vx"_s) vxi = c;
        else if (h == u"vy"_s) vyi = c;
        else if (h == u"vz"_s) vzi = c;
    }
    // Skip the derived key if a real column already claims that name (rare).
    if (vxi >= 0 && vyi >= 0 && vzi >= 0 && !colourKeys.contains(u"Speed |v|"_s)) {
        // Read a component defensively — a null/NaN entry must not poison the
        // array range (unlike fieldDouble, raw toDouble() doesn't finite-check).
        const auto comp = [](const Catalogue3DEntry &e, int idx) -> double {
            if (idx < 0 || idx >= e.fields.size())
                return 0.0;
            bool ok = false;
            const double v = e.fields.at(idx).toDouble(&ok);
            return (ok && std::isfinite(v)) ? v : 0.0;
        };
        vtkNew<vtkFloatArray> speed;
        speed->SetName("Speed |v|");
        speed->SetNumberOfTuples(nPts);
        for (vtkIdType i = 0; i < nPts; ++i) {
            const auto &e = parsed.entries[static_cast<std::size_t>(i)];
            const double vx = comp(e, vxi), vy = comp(e, vyi), vz = comp(e, vzi);
            speed->SetValue(i, static_cast<float>(std::sqrt(vx * vx + vy * vy + vz * vz)));
        }
        poly->GetPointData()->AddArray(speed);
        colourKeys << u"Speed |v|"_s;
    }

    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputData(poly);
    mapper->ScalarVisibilityOff();   // uniform colour — the arrows carry the speed LUT

    const bool firstOverlay = !this->catalogueActor;
    if (this->catalogueActor)
        this->renderer->RemoveActor(this->catalogueActor);
    this->catalogueActor = vtkSmartPointer<vtkActor>::New();
    this->catalogueActor->SetMapper(mapper);
    this->catalogueActor->GetProperty()->SetColor(0.95, 0.85, 0.30);  // warm gold
    this->catalogueActor->GetProperty()->SetPointSize(
            this->spinCataloguePointSize ? this->spinCataloguePointSize->value() : 3.0);
    this->renderer->AddActor(this->catalogueActor);

    this->m_catalogueName = QFileInfo(path).fileName();
    this->m_catalogueCount = static_cast<int>(parsed.entries.size());

    if (this->chkShowCatalogue) {
        QSignalBlocker blk(this->chkShowCatalogue);
        this->chkShowCatalogue->setChecked(true);
        this->chkShowCatalogue->setEnabled(true);
    }
    if (this->spinCataloguePointSize)
        this->spinCataloguePointSize->setEnabled(true);
    if (this->comboGalaxyColor) {
        // Repopulate to Uniform + this catalogue's numeric attributes. Blocked so
        // it doesn't fire recolour — the actor already starts Uniform.
        QSignalBlocker blk(this->comboGalaxyColor);
        this->comboGalaxyColor->clear();
        this->comboGalaxyColor->addItem(u"Uniform"_s);
        this->comboGalaxyColor->addItems(colourKeys);
        this->comboGalaxyColor->setCurrentIndex(0);
        this->comboGalaxyColor->setEnabled(true);
    }
    // New actor starts Uniform — drop any legend left from a previous overlay.
    if (this->catalogueScalarBar)
        this->catalogueScalarBar->VisibilityOff();

    // Reframe on the FIRST overlay so the galaxies are visible — if the catalogue
    // shares the field's frame they sit inside it; if not, the zoom-out makes the
    // mismatch obvious. Later re-overlays keep the user's current view.
    if (firstOverlay)
        this->renderer->ResetCamera();
    this->renderWindow->Render();
    QApplication::restoreOverrideCursor();

    const bool capped = subset.totalRows > this->m_catalogueCount;
    QString msg = u"Overlaid %1 galaxies from %2"_s.arg(this->m_catalogueCount)
                          .arg(this->m_catalogueName);
    if (capped)
        msg += u" (of %1; capped)"_s.arg(subset.totalRows);
    if (!schema.isCartesian())
        msg += u" — note: not a Cartesian catalogue, positions may not match the field frame"_s;
    this->statusBar()->showMessage(msg, 8000);
    if (this->summaryLabel) {
        if (this->m_summaryBase.isEmpty())
            this->m_summaryBase = this->summaryLabel->text();
        this->summaryLabel->setText(this->m_summaryBase
                                    + u" · %1 galaxies"_s.arg(this->m_catalogueCount));
    }
}

void vtkWindowVectorField::setCatalogueVisible(bool on)
{
    if (!this->catalogueActor)
        return;
    this->catalogueActor->SetVisibility(on);
    // The attribute legend only makes sense while the (attribute-coloured)
    // galaxies are shown.
    if (this->catalogueScalarBar) {
        auto *mapper = vtkPolyDataMapper::SafeDownCast(this->catalogueActor->GetMapper());
        const bool coloured = mapper && mapper->GetScalarVisibility();
        this->catalogueScalarBar->SetVisibility(on && coloured);
    }
    this->renderWindow->Render();
}

void vtkWindowVectorField::recolourCatalogue(const QString &key)
{
    if (!this->catalogueActor)
        return;
    auto *mapper = vtkPolyDataMapper::SafeDownCast(this->catalogueActor->GetMapper());
    if (!mapper)
        return;

    if (key.isEmpty() || key == u"Uniform"_s) {
        mapper->ScalarVisibilityOff();   // back to the flat gold colour
        if (this->catalogueScalarBar)
            this->catalogueScalarBar->VisibilityOff();
        this->renderWindow->Render();
        return;
    }

    auto *poly = vtkPolyData::SafeDownCast(mapper->GetInput());
    vtkDataArray *arr = poly ? poly->GetPointData()->GetArray(key.toUtf8().constData()) : nullptr;
    if (!arr) {
        mapper->ScalarVisibilityOff();
        if (this->catalogueScalarBar)
            this->catalogueScalarBar->VisibilityOff();
        this->renderWindow->Render();
        return;
    }

    if (!this->catalogueLut) {
        // Blue → red ramp, distinct from the field's speed colormap.
        this->catalogueLut = vtkSmartPointer<vtkLookupTable>::New();
        this->catalogueLut->SetHueRange(0.667, 0.0);
        this->catalogueLut->SetNumberOfTableValues(256);
        this->catalogueLut->Build();
    }
    double range[2];
    arr->GetRange(range);
    if (!(range[0] < range[1]))
        range[1] = range[0] + 1.0;   // degenerate range → keep the LUT valid
    this->catalogueLut->SetTableRange(range);

    mapper->SetLookupTable(this->catalogueLut);
    mapper->SetScalarModeToUsePointFieldData();
    mapper->SelectColorArray(key.toUtf8().constData());
    mapper->SetScalarRange(range);
    mapper->ScalarVisibilityOn();

    // Legend for the galaxy attribute, on the LEFT so it doesn't collide with the
    // field's speed bar (right). Built once, then just retitled/reshown.
    if (!this->catalogueScalarBar) {
        this->catalogueScalarBar = vtkSmartPointer<vtkScalarBarActor>::New();
        this->catalogueScalarBar->SetLookupTable(this->catalogueLut);
        this->catalogueScalarBar->SetMaximumWidthInPixels(90);
        this->catalogueScalarBar->GetPositionCoordinate()
                ->SetCoordinateSystemToNormalizedViewport();
        // y = 0.20 keeps it clear of the orientation-axes marker (0,0)–(0.14,0.14).
        this->catalogueScalarBar->GetPositionCoordinate()->SetValue(0.02, 0.20);
        this->catalogueScalarBar->SetWidth(0.10);
        this->catalogueScalarBar->SetHeight(0.4);
        this->renderer->AddActor2D(this->catalogueScalarBar);
    }
    this->catalogueScalarBar->SetTitle(key.toUtf8().constData());
    this->catalogueScalarBar->SetVisibility(
            this->catalogueActor->GetVisibility() ? 1 : 0);
    this->renderWindow->Render();
}
