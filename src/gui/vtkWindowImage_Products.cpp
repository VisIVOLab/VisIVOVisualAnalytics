// vtkWindowImage_Products.cpp
// Session Data for the 2-D image viewer: registering what its tools produce,
// and putting it back on screen when the user opens the row.
//
// The cube viewer has had this since Design 1d (see vtkWindowCube's
// registerAnalysisProduct and the "What a tool must leave behind" section of
// docs/technical/architecture-note.md). This window had the same dock and the same owner
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
#include "app/DatasetName.h"
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
#include "ProfileWidget.h"
#include "PublicationFigureDialog.h"
#include "LteCoDialog.h"
#include <vtkLookupTable.h>
#include <vtkNew.h>
#include "ColorMaps.h"
#include "RegionModel.h"
#include "app/LteCoRequest.h"

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
                                                const QVariantMap &origin,
                                                const QList<ReplayStep> &replay)
{
    AnalysisProduct p;
    p.productId = stableId;
    p.parentKey = this->filepath;
    p.parentLabel = visivo::datasetDisplayName(this->filepath);
    p.kind = kind;
    p.label = label;
    p.renderer = QString();   // single-view window: nothing mounts in a pane
    p.params = params;
    p.resultView = resultView;
    p.origin = origin;
    p.replay = replay;
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
    // A power spectrum or a skeleton: the plot, or the overlay, again.
    if (this->openMapTextureProduct(productId))
        return;
    // A SPECTRUM this window extracted: replot the curve. The record alone would
    // give the summary of a measurement whose point is its shape.
    if (const auto curve = m_extractedSpectra.constFind(productId);
        curve != m_extractedSpectra.cend()) {
        auto *plot = new ProfileWidget(this);
        plot->setAttribute(Qt::WA_DeleteOnClose);
        plot->setWindowTitle(u"Extracted Spectrum"_s);
        plot->setUsageMode(ProfileWidget::UsageMode::RegionStatic, p.label);
        plot->setupSpectrumPlot(this->remoteOverlayAxisTitle(1), u"Mean value"_s);
        plot->updateSpectrumPlot(curve->first, curve->second, p.label, /*live=*/false);
        plot->show();
        return;
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
    this->appendSessionRemoveAction(menu, productId);
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
    params.insert(u"Format"_s, u"%1 (matplotlib, rendered on the backend)"_s.arg(
            QFileInfo(outputPath).suffix().toUpper()));
    params.insert(u"Storage"_s, u"Workspace Exports"_s);
    this->registerAnalysisProduct(u"figure"_s,
                                  u"Publication figure: %1"_s.arg(QFileInfo(outputPath).fileName()),
                                  params, this->scopedProductId(u"pubfig"_s));
}

namespace {
/// The colours the window draws its map with, as @p n RGB rows in 0..1 (lowest value
/// first) for a figure to draw exactly. The map is read by its name from the registry,
/// freshly, not from the window's table: under a square-root, square or power stretch
/// the window resamples the table's entries through the stretch, and the figure applies
/// the stretch itself, which would be done twice. A table with no registry name is read
/// as it is when the window's scale does not bake a stretch into it (linear, log), and
/// not sent otherwise (the figure then offers what it can set by name).
QJsonArray windowColourTable(vtkLookupTable *lut, LutScale::Mode mode, std::size_t n = 256)
{
    vtkNew<vtkLookupTable> plain;
    const std::string name = lut->GetObjectName();
    if (!name.empty())
        ColorMaps::SetColorMap(plain, name);
    vtkLookupTable *source = nullptr;
    if (!plain->GetObjectName().empty())
        source = plain;
    else if (mode == LutScale::Mode::Linear || mode == LutScale::Mode::Log)
        source = lut;
    QJsonArray rows;
    const vtkIdType entries = source ? source->GetNumberOfTableValues() : 0;
    if (entries < 2)
        return rows;
    for (std::size_t i = 0; i < n; ++i) {
        const auto k = static_cast<vtkIdType>(std::llround(static_cast<double>(i)
                                                           * static_cast<double>(entries - 1)
                                                           / static_cast<double>(n - 1)));
        double rgba[4];
        source->GetTableValue(k, rgba);
        rows.append(QJsonArray{ rgba[0], rgba[1], rgba[2] });
    }
    return rows;
}
}   // namespace

visivo::figure::Context vtkWindowImage::publicationFigureContext()
{
    namespace vf = visivo::figure;
    vf::Context ctx;
    ctx.overlays = std::make_shared<vf::Overlays>();
    ctx.fileStem = QFileInfo(this->filepath).completeBaseName() + u"_figure"_s;
    vf::Options &o = ctx.defaults;

    // The colours as the window shows them: the map's name, and its table, which the
    // dialog sends when the backend draws that map only approximately by name.
    const int master = this->layers ? this->layers->getMasterIndex() : -1;
    if (master >= 0 && master < this->layers->rowCount()) {
        if (auto *lut = this->layers->getLookupTable(master)) {
            ctx.windowColourMap = QString::fromStdString(lut->GetObjectName());
            ctx.windowLut = windowColourTable(lut, this->layers->scaleMode(master));
            const double *r = lut->GetTableRange();
            if (r && std::isfinite(r[0]) && std::isfinite(r[1]) && r[0] < r[1]) {
                o.useRange = true;
                o.vmin = r[0];
                o.vmax = r[1];
            }
        }
        switch (this->layers->scaleMode(master)) {
        case LutScale::Mode::Log: o.stretch = u"log"_s; break;
        case LutScale::Mode::Sqrt: o.stretch = u"sqrt"_s; break;
        case LutScale::Mode::Square: o.stretch = u"squared"_s; break;
        case LutScale::Mode::Power:
            o.stretch = u"power"_s;
            o.gamma = this->layers->scaleGamma(master);
            break;
        case LutScale::Mode::Linear: break;
        }
    }
    o.frame = vf::frameForWindow(ui->actionGalactic->isChecked(), ui->actionEcliptic->isChecked());

    // Over the image. The lines of a contour layer are taken here, on the GUI
    // thread, as the export does; they are joined later, off it.
    constexpr std::size_t kMaxFigureSegments = 1'000'000;
    const auto identity = [](double x, double y) { return std::array<double, 2>{ x, y }; };
    bool tooMany = false;
    const auto take = [&](ContourLayer *layer) {
        if (!layer || !layer->hasInput() || !layer->visible())
            return;
        bool big = false;
        auto src = layer->lineSource(identity, kMaxFigureSegments, &big);
        if (big)
            tooMany = true;
        else if (!src.segments.empty())
            ctx.overlays->contourSources.push_back(std::move(src));
    };
    if (m_contoursVisible)
        take(m_contourLayer.get());
    for (const auto &ext : std::as_const(m_externalContourLayers))
        take(ext.layer.get());
    if (tooMany)
        statusBar()->showMessage(u"Some contours have too many segments for a figure and are "
                                 u"left out: raise the lowest level or smooth the image."_s, 8000);
    ctx.overlays->regions = vf::regionRegions(m_regions.regions());
    std::vector<vf::Note> notes;
    for (const auto &a : m_annotations)
        notes.push_back({ a.type == Annotation::Arrow, a.text, a.worldX, a.worldY, a.arrowEndX,
                          a.arrowEndY });
    ctx.overlays->annotations = vf::noteRegions(notes);
    o.contours = ctx.overlays->hasContours();
    o.regions = !ctx.overlays->regions.isEmpty();
    o.annotations = !ctx.overlays->annotations.isEmpty();
    return ctx;
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
            visivo::datasetDisplayName(this->filepath),
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
    // The measured region goes back into the region list (issue #341) and is
    // selected; a region of the list with the same geometry is selected rather
    // than duplicated, so opening a row twice does not stack two copies.
    std::optional<visivo::regions::Region> restored;
    const QVariantMap listRegion = visivo::originListRegion(origin);
    if (!listRegion.isEmpty()) {
        visivo::regions::Region r;
        if (!visivo::regions::fromCanonical(QJsonObject::fromVariantMap(listRegion), 0.0, &r))
            return false;
        restored = r;
    } else {
        // A product measured before the list: anchor/current (or vertices) in the
        // voxel grid displayed at the time. If the viewer has swapped LOD since
        // (preview -> full), that grid is not this one.
        const QString shape = visivo::originShape(origin);
        double scale[2] = { 1., 1. }, pos[2] = { 0., 0. };
        const bool haveFrame = this->currentVoxelFrame(scale, pos);
        const auto remap = [&](int v[2]) {
            if (haveFrame)
                visivo::remapOriginVoxel(origin, scale, pos, v);
        };
        std::array<int, 2> anchor{ 0, 0 }, current{ 0, 0 };
        std::vector<std::array<int, 2>> verts;
        double inner = 0.0;
        if (shape == u"polygon"_s) {
            verts = visivo::originPolygon(origin);
            if (verts.size() < 3)
                return false;
            for (auto &v : verts) {
                int p[2] = { v[0], v[1] };
                remap(p);
                v = { p[0], p[1] };
            }
        } else {
            int a[2] = { 0, 0 }, c[2] = { 0, 0 };
            if (!visivo::originPoint(origin, u"anchor"_s, a, 2)
                || !visivo::originPoint(origin, u"current"_s, c, 2))
                return false;   // half a geometry is worse than none
            double oldScale[2] = { 1., 1. }, oldPos[2] = { 0., 0. };
            const bool hadFrame = visivo::originFrame(origin, oldScale, oldPos);
            remap(a);
            remap(c);
            anchor = { a[0], a[1] };
            current = { c[0], c[1] };
            // The inner radius is a length in the same voxel grid, so it scales too.
            inner = visivo::originInnerRadius(origin);
            if (haveFrame && hadFrame && oldScale[0] != 0.0)
                inner *= oldScale[0] / scale[0];
        }
        restored = visivo::regionFromDisplayed(shape, this->displayGrid(), anchor, current, verts,
                                               inner);
        if (!restored)
            return false;
    }
    int id = -1;
    for (const auto &r : this->m_regions.regions()) {
        if (visivo::regions::sameGeometry(r, *restored)) {
            id = r.id;
            break;
        }
    }
    if (id < 0)
        id = this->m_regions.add(*restored);
    else if (!this->m_regions.find(id)->visible)
        this->m_regions.setVisible(id, true);   // hidden in Session Data: the row puts it back
    this->m_regions.select(id);
    this->onRegionListChanged();
    return true;
}

void vtkWindowImage::openLteCoDialog()
{
    if (this->remoteDatasetId.isEmpty())
        return;
    LteCoDialog::Context ctx;
    ctx.backendUrl = this->remoteBackendUrl;
    ctx.backendToken = this->remoteBackendToken;
    ctx.sessionId = this->remoteSessionId;
    ctx.hostDatasetId = this->remoteDatasetId;
    for (const visivo::regions::Region &r : this->m_regions.regions()) {
        QJsonObject canonical = visivo::regions::toCanonical(r, 0.0);
        if (!visivo::lte::isAreaRegion(canonical))
            continue;
        canonical.remove(u"props"_s);
        ctx.regions.append({ r.name.isEmpty() ? u"Region %1"_s.arg(r.id) : r.name, canonical });
    }

    if (auto *open = this->findChild<LteCoDialog *>(u"lteCoDialog"_s)) {
        open->refreshHostContext(ctx);
        open->show();
        open->raise();
        open->activateWindow();
        return;
    }

    auto *dlg = new LteCoDialog(ctx, this);
    dlg->setObjectName(u"lteCoDialog"_s);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    QPointer<vtkWindowImage> self(this);
    QObject::connect(dlg, &LteCoDialog::openMapRequested, this,
                     [self](const QString &path, const QString &label) {
                         if (self)
                             emit self->openBackendFitsRequested(path, label);
                     });
    QObject::connect(dlg, &LteCoDialog::runRecorded, this,
                     [self](const QString &label, const QVariantMap &params) {
                         if (!self)
                             return;
                         // Each run is its own row: two runs with other assumptions
                         // are two results to compare, not one to overwrite.
                         self->registerAnalysisProduct(
                                 u"lte"_s, label, params,
                                 self->scopedProductId(u"lte%1"_s.arg(++self->m_lteRuns)));
                     });
    dlg->show();
}
