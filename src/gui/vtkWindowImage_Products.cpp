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
#include "ProductRegistry.h"
#include "ToolResultDialog.h"
#include "WorkspaceChrome.h"

#include <QAbstractItemModel>
#include <QListView>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMenu>
#include <QUrl>

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
                                                int layerIndex)
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
    p.sourceDatasetId = this->remoteDatasetId;
    p.sessionId = this->remoteSessionId;
    p.ownerId = this->m_windowScope;
    const QString id = ProductRegistry::instance().registerProduct(std::move(p));
    if (layerIndex >= 0)
        m_productLayer.insert(id, layerIndex);

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

void vtkWindowImage::openProduct(const QString &productId)
{
    const AnalysisProduct p = ProductRegistry::instance().product(productId);

    // A LAYER: select it, which is what "open" means for a map in a window that
    // shows one view.
    const auto layer = m_productLayer.constFind(productId);
    if (layer != m_productLayer.cend()) {
        selectLayer(ui->listLayer, this->layers, layer.value());
        return;
    }
    // A FILE: hand it to the desktop.
    const QString path = p.params.value(u"Output path"_s).toString();
    if (!path.isEmpty() && QFileInfo::exists(path)) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        return;
    }
    // A MEASUREMENT: replay the window it came from.
    if (!p.resultView.isEmpty()) {
        const QString slot = u"recorded-result:"_s + productId;
        if (auto *existing = this->findChild<ToolResultDialog *>(slot)) {
            existing->show();
            existing->raise();
            existing->activateWindow();
            return;
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

    const auto layer = m_productLayer.constFind(productId);
    const QString path = p.params.value(u"Output path"_s).toString();
    if (layer != m_productLayer.cend()) {
        QObject::connect(menu.addAction(u"Select This Layer"_s), &QAction::triggered, this,
                         [this, idx = layer.value()]() {
                             selectLayer(ui->listLayer, this->layers, idx);
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
