// vtkWindowImage_Products.cpp
// Session Data for the 2-D image viewer: registering what its tools produce,
// and putting it back on screen when the user opens the row.
//
// The cube viewer has had this since Design 1d (see vtkWindowCube's
// registerAnalysisProduct and the "What a tool must leave behind" section of
// docs/architecture-note.md). This window had the same dock and the same owner
// scope, and registered nothing at all, so its tools behaved like the cube's
// tools did before that work: the result lived in a floating window, and closing
// it was the end of it.
//
// The three kinds of product here mirror the cube's, minus the pane-mounted one
// (this window has a single view, so a derived MAP is a layer rather than a
// pane):
//
//   a LAYER      — a derived map added to this window (polarisation, spectral
//                  index, Faraday RM). Activation selects that layer.
//   a FILE       — an artefact on disk (publication figure, workspace export).
//                  Activation opens it; the row menu copies its path.
//   a MEASUREMENT— numbers (region statistics, image quality, cross-match).
//                  Activation replays the result window from its record.

#include "vtkWindowImage.h"
#include "ui_vtkWindowImage.h"

#include "LayerListModel.h"   // src/, not src/gui/
#include <QDockWidget>
#include "ProductOrigin.h"
#include "ProductRegistry.h"
#include "ToolResultDialog.h"
#include "WorkspaceChrome.h"

#include <QAbstractItemModel>
#include <QListView>
#include <vtkRenderWindow.h>

#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMenu>
#include <QUrl>
#include <vtkImageData.h>

using namespace Qt::StringLiterals;


namespace {
/// Select a layer row in the layer list, which is this window's way of saying
/// "look at this map". Bounds-checked: a product outlives the layer it made if
/// the user removes that layer.
void selectLayer(QListView *list, QAbstractItemModel *model, int index)
{
    if (!list || !model || index < 0 || index >= model->rowCount())
        return;
    list->setCurrentIndex(model->index(index, 0));
}
}   // namespace

QString vtkWindowImage::scopedProductId(const QString &stem) const
{
    return u"%1@%2#%3"_s.arg(stem, this->filepath, this->m_windowScope);
}

QString vtkWindowImage::registerAnalysisProduct(const QString &kind, const QString &label,
                                                const QVariantMap &params,
                                                const QString &stableId,
                                                const QVariantMap &resultView,
                                                int layerIndex,
                                                const QVariantMap &origin)
{
    AnalysisProduct p;
    p.productId = stableId;
    p.parentKey = this->filepath;
    p.parentLabel = QFileInfo(this->filepath).fileName();
    p.kind = kind;
    p.label = label;
    p.renderer = QString();   // single-view window: nothing mounts in a pane
    p.params = params;
    p.resultView = resultView;
    p.origin = origin;
    p.sourceDatasetId = this->remoteDatasetId;
    p.sessionId = this->remoteSessionId;
    p.ownerId = this->m_windowScope;
    const QString id = ProductRegistry::instance().registerProduct(std::move(p));
    if (layerIndex >= 0) {
        m_productLayer.insert(id, layerIndex);
        // The row number is not a layer identity: the list is drag-reorderable
        // (LayerListModel::dropMimeData), so remember the NAME as well and
        // resolve through it first when the product is opened.
        if (this->layers && layerIndex < this->layers->rowCount()) {
            const QString name =
                    this->layers->data(this->layers->index(layerIndex, 0), Qt::DisplayRole)
                            .toString();
            if (!name.isEmpty())
                m_productLayerName.insert(id, name);
        }
    }

    if (this->m_workspaceChrome) {
        this->m_workspaceChrome->selectSessionProduct(id);
        // Count only THIS window's products: the registry is session-wide and a
        // sibling window on the same file shares parentKey but not ownerId.
        int mine = 0;
        for (const AnalysisProduct &ap :
             ProductRegistry::instance().productsFor(this->filepath))
            if (ap.ownerId == this->m_windowScope)
                ++mine;
        this->m_workspaceChrome->setSessionRailCount(mine);
    }
    return id;
}

int vtkWindowImage::productLayerRow(const QString &productId) const
{
    const auto byIndex = m_productLayer.constFind(productId);
    if (byIndex == m_productLayer.cend())
        return -1;
    // Names first: a reorder moves the layer, not its name. Fall back to the
    // recorded row when the layer was never named or the name is gone.
    const QString name = m_productLayerName.value(productId);
    if (!name.isEmpty() && this->layers) {
        for (int r = 0; r < this->layers->rowCount(); ++r) {
            if (this->layers->data(this->layers->index(r, 0), Qt::DisplayRole).toString() == name)
                return r;
        }
    }
    const int row = byIndex.value();
    return (this->layers && row < this->layers->rowCount()) ? row : -1;
}

void vtkWindowImage::openProduct(const QString &productId)
{
    const AnalysisProduct p = ProductRegistry::instance().product(productId);

    // A LAYER: select it, which is what "open" means for a map in a window that
    // shows one view.
    const int layerRow = this->productLayerRow(productId);
    if (layerRow >= 0) {
        selectLayer(ui->listLayer, this->layers, layerRow);
        return;
    }
    // A FILE: hand it to the desktop.
    const QString path = p.params.value(u"Output path"_s).toString();
    if (!path.isEmpty() && QFileInfo::exists(path)) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        return;
    }
    // A MEASUREMENT: put the geometry back on screen first — the numbers say
    // what was measured, the overlay says where, and a row that only reopens a
    // table leaves the second half unanswered (user report).
    if (this->restoreProductOrigin(p.origin)) {
        const QString where = visivo::describeOrigin(p.origin);
        if (!where.isEmpty())
            this->statusBar()->showMessage(u"Showing %1"_s.arg(where), 6000);
    }
    if (!p.resultView.isEmpty()) {
        const QString slot = u"recorded-result:"_s + productId;
        if (auto *existing = this->findChild<ToolResultDialog *>(slot)) {
            // Replace, don't raise. A second run of the same tool overwrites the
            // registry entry under the same id, so the open window can be
            // showing the previous numbers - and applyRecord() APPENDS sections,
            // so refreshing it in place would stack the two results instead.
            existing->setObjectName(QString());
            existing->close();
            existing->deleteLater();
        }
        auto *dlg = new ToolResultDialog(this);
        dlg->setObjectName(slot);
        dlg->applyRecord(p.resultView);
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
        return;
    }
    this->statusBar()->showMessage(
            p.label.isEmpty() ? u"This product recorded nothing to reopen."_s
                              : u"“%1” recorded nothing to reopen."_s.arg(p.label),
            5000);
}

void vtkWindowImage::showSessionRowContextMenu(const QString &viewTag, const QString &productId,
                                               const QPoint &globalPos)
{
    Q_UNUSED(viewTag);
    if (productId.isEmpty())
        return;
    const AnalysisProduct p = ProductRegistry::instance().product(productId);
    QMenu menu(this);
    menu.setToolTipsVisible(true);

    const int layerRow = this->productLayerRow(productId);
    const QString path = p.params.value(u"Output path"_s).toString();
    if (layerRow >= 0) {
        QObject::connect(menu.addAction(u"Select This Layer"_s), &QAction::triggered, this,
                         [this, productId]() {
                             // Resolved again on click: the list may have been
                             // reordered since the menu was built.
                             const int row = this->productLayerRow(productId);
                             if (row >= 0)
                                 selectLayer(ui->listLayer, this->layers, row);
                         });
    } else if (!path.isEmpty()) {
        QAction *open = menu.addAction(u"Open the File"_s);
        open->setEnabled(QFileInfo::exists(path));
        QObject::connect(open, &QAction::triggered, this, [path]() {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        });
        QObject::connect(menu.addAction(u"Copy File Path"_s), &QAction::triggered, this,
                         [path]() { QGuiApplication::clipboard()->setText(path); });
    } else if (!p.resultView.isEmpty()) {
        QObject::connect(menu.addAction(u"Show the Numbers Again"_s), &QAction::triggered, this,
                         [this, productId]() { this->openProduct(productId); });
    }
    if (menu.isEmpty())
        return;
    menu.exec(globalPos);
}

void vtkWindowImage::recordPublicationFigure(const QString &outputPath)
{
    // A FILE product: the row opens the PNG, and its menu copies the path. The
    // figure used to be announced in a message box and nowhere else.
    QVariantMap params;
    params.insert(u"Output path"_s, outputPath);
    params.insert(u"Format"_s, u"PNG (matplotlib, rendered on the backend)"_s);
    params.insert(u"Storage"_s, u"Workspace Exports"_s);
    this->registerAnalysisProduct(u"figure"_s,
                                  u"Publication figure: %1"_s.arg(QFileInfo(outputPath).fileName()),
                                  params, this->scopedProductId(u"pubfig"_s));
}

void vtkWindowImage::recordImageQuality(const QVariantMap &metrics)
{
    // A MEASUREMENT: the numbers go to Provenance, and the row replays the
    // window they were shown in. The order is the one the tool reports them in,
    // not the map's alphabetical one.
    static const QStringList kOrder{
        u"total_pixels"_s, u"blanked_fraction"_s, u"zero_fraction"_s,
        u"negative_fraction"_s, u"min"_s, u"max"_s, u"mean"_s, u"median"_s,
        u"robust_std"_s, u"dynamic_range"_s, u"striping_metric"_s, u"negative_bowl"_s };
    QList<QPair<QString, QString>> rows;
    QVariantMap params;
    for (const QString &k : kOrder) {
        if (!metrics.contains(k))
            continue;
        const QString v = metrics.value(k).toString();
        rows.append({ k, v });
        params.insert(k, v);
    }
    const QStringList flags = metrics.value(u"flags"_s).toStringList();
    const QString flagText = flags.isEmpty() ? u"(none, looks clean)"_s : flags.join(u", "_s);
    rows.append({ u"flags"_s, flagText });
    params.insert(u"flags"_s, flagText);

    const QVariantMap record = ToolResultDialog::makeRecord(
            u"Image Quality / Artifacts"_s,
            QFileInfo(this->filepath).fileName(),
            { { u"Metrics"_s, rows } });
    this->registerAnalysisProduct(u"quality"_s, u"Image quality"_s, params,
                                  this->scopedProductId(u"imagequality"_s), record);
}

bool vtkWindowImage::currentVoxelFrame(double scale[2], double pos[2]) const
{
    auto *imageData =
            this->layers ? this->layers->getImageData(this->layers->getMasterIndex()) : nullptr;
    if (!imageData)
        return false;
    double ro[3], rs[3];
    int rext[6];
    imageData->GetOrigin(ro);
    imageData->GetSpacing(rs);
    imageData->GetExtent(rext);
    // Same mapping refreshRegionOverlay() gives the overlay actors, kept in one
    // place so a recorded region and a live one cannot drift apart.
    scale[0] = rs[0];
    scale[1] = rs[1];
    pos[0]   = ro[0] - rext[0] * rs[0];
    pos[1]   = ro[1] - rext[2] * rs[1];   // ext[2], not ext[1]: that Y index has bitten before
    return scale[0] != 0.0 && scale[1] != 0.0;
}

bool vtkWindowImage::restoreProductOrigin(const QVariantMap &origin)
{
    if (visivo::originKind(origin) != u"region"_s)
        return false;
    const QString shape = visivo::originShape(origin);
    // The geometry was recorded in the voxel grid displayed at the time. If the
    // viewer has swapped LOD since (preview -> full), that grid is not this one.
    double scale[2] = { 1., 1. }, pos[2] = { 0., 0. };
    const bool haveFrame = this->currentVoxelFrame(scale, pos);
    const auto remap = [&](int v[2]) {
        if (haveFrame)
            visivo::remapOriginVoxel(origin, scale, pos, v);
    };
    if (shape == u"polygon"_s) {
        auto verts = visivo::originPolygon(origin);
        if (verts.size() < 3)
            return false;
        for (auto &v : verts) {
            int p[2] = { v[0], v[1] };
            remap(p);
            v = { p[0], p[1] };
        }
        this->regionPolygonVertices = verts;
        this->regionMode = RegionMode::Polygon;
    } else {
        int a[2] = { 0, 0 }, c[2] = { 0, 0 };
        if (!visivo::originPoint(origin, u"anchor"_s, a, 2)
            || !visivo::originPoint(origin, u"current"_s, c, 2))
            return false;   // half a geometry is worse than none
        double oldScale[2] = { 1., 1. }, oldPos[2] = { 0., 0. };
        const bool hadFrame = visivo::originFrame(origin, oldScale, oldPos);
        remap(a);
        remap(c);
        this->regionAnchorVoxel = { a[0], a[1] };
        this->regionCurrentVoxel = { c[0], c[1] };
        // The inner radius is a length in the same voxel grid, so it scales too.
        this->regionAnnulusInnerRadius = visivo::originInnerRadius(origin);
        if (haveFrame && hadFrame && oldScale[0] != 0.0)
            this->regionAnnulusInnerRadius *= oldScale[0] / scale[0];
        this->regionMode = shape == u"box"_s      ? RegionMode::Box
                         : shape == u"circle"_s   ? RegionMode::Circle
                         : shape == u"annulus"_s  ? RegionMode::Annulus
                                                  : RegionMode::None;
        if (this->regionMode == RegionMode::None)
            return false;
    }
    this->regionDragging = false;
    this->regionValid = true;
    this->refreshRegionOverlay();
    if (ui->vtk && ui->vtk->renderWindow())
        ui->vtk->renderWindow()->Render();
    return true;
}
