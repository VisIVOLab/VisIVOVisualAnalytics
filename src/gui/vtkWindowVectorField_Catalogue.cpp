#include "vtkWindowVectorField.h"

#include "Catalogue3DParser.h"
#include "RemoteFileBrowserDialog.h"
#include "app/BackendClient.h"

#include <vtkActor.h>
#include <vtkCellArray.h>
#include <vtkNew.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>

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
    this->renderWindow->Render();
}
