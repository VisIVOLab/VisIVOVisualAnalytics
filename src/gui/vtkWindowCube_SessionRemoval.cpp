// "Remove from Session" in the cube viewer (issue #580) — partial class of
// vtkWindowCube.
//
// The gesture (Delete key, context menu, multi-selection) and the question
// belong to SessionDataTree; what a removal means for a cube window is here:
// the panes showing the product, a pane that was popped out to its own window,
// the render caches, the PV diagram, a tool window, and the temporary file a
// baseline cube left on this machine. A permanent copy the user saved is never
// touched, whatever the product's parameters say (app/SessionRemoval.h).

#include "vtkWindowCube.h"
#include "vtkWindowImage.h"

#include <QDockWidget>
#include <QStatusBar>
#include <vtkLookupTable.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>

#include "Pane2DView.h"
#include "ProfileWidget.h"
#include "PvDiagramWidget.h"
#include "SessionDataTree.h"
#include "ToolResultDialog.h"
#include "WorkspaceChrome.h"
#include "app/ProductRegistry.h"
#include "app/SessionRemoval.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMenu>

using namespace Qt::StringLiterals;

void vtkWindowCube::dropSessionProduct(const QString &id)
{
    if (id.isEmpty())
        return;
    // The slice may be wearing this map as its background: give the slice its
    // channels back first (that also re-assigns the panes it shuffled), or it
    // keeps showing a product that no longer exists.
    if (id == m_sliceBackdropProductId)
        this->setSliceBackdrop(QString());

    releaseBorrowedPane(id);   // hands back the view a borrowing product displaced

    // Every pane showing it, a popped-out one included: the pane is brought
    // back into the grid first, so no window is left open on a product that is
    // gone, and its widget is emptied so the pixels are not kept alive by a
    // parked view. A pane currently showing another view still caches what it
    // last displayed (its widget and product id are kept when the slot is
    // reassigned), so the cache is cleared for every slot: a product computed
    // again under the same id must not inherit that stale display state.
    for (int i = 0; i < 4; ++i) {
        const bool showing = m_slots[i].view.kind == ViewId::Product
                && m_slots[i].view.productId == id;
        if (showing && paneDetached(i))
            bringBackPane(i);
        if (showing)
            m_slots[i].view = {};
        if (m_paneProductId[i] == id) {
            if (m_paneViews[i])
                m_paneViews[i]->setImageData(nullptr, 0.0, 1.0);
            if (m_paneSpectra[i])
                m_paneSpectra[i]->updateSpectrumPlot({}, {}, QString(), /*live=*/false);
            m_paneProductId[i].clear();
        }
        // m_paneProductId names only the product mounted last, whichever
        // renderer it used: an image parked behind a spectrum (or the reverse)
        // is not named there any more. Empty every renderer the slot is not
        // showing right now; it only held a stale cache, and the next mount
        // of anything fills it again (its id no longer matches).
        if (!showing) {
            const QString shown = m_slots[i].view.kind == ViewId::Product
                    ? m_slots[i].view.productId : QString();
            if (m_paneViews[i] && !m_products2d.contains(shown))
                m_paneViews[i]->setImageData(nullptr, 0.0, 1.0);
            if (m_paneSpectra[i] && !m_products1d.contains(shown))
                m_paneSpectra[i]->updateSpectrumPlot({}, {}, QString(), /*live=*/false);
            // The renderer that held m_paneProductId was just emptied: forget
            // it, or mounting that product again would keep the placeholder
            // range instead of restoring its own.
            if (m_paneProductId[i] != shown)
                m_paneProductId[i].clear();
        }
        if (showing)
            mountView(i);
    }

    // The PV diagram is one widget hosted in a pane, not a cache entry: it
    // goes, and with it the path drawn on the maps. Its own tidy-up on
    // destruction is done here, now, so a PV started a moment later is not
    // cleared by the old widget's destroyed signal.
    QWidget *pvWidget = nullptr;
    if (!m_pvProductId.isEmpty() && id == m_pvProductId) {
        if (PvDiagramWidget *pv = this->pvDiagramWidget.data()) {
            pvWidget = pv;
            pv->disconnect(this);
            this->pvDiagramWidget = nullptr;
            this->m_pvProductId.clear();
            this->clearPv();
            if (this->active2dWin())
                this->active2dWin()->Render();
            pv->hide();
            pv->deleteLater();
        }
    }

    m_products2d.remove(id);
    m_products1d.remove(id);
    // The generation counter is NOT reset: a moment computed again under this id
    // must not look like the pixels the cached dataset below was written for.
    m_momentDatasets.remove(id);
    m_momentPowerSpectra.remove(id);
    m_momentPowerFits.remove(id);
    m_momentPowerLabels.remove(id);
    if (m_lastComputedMomentProductId == id)
        m_lastComputedMomentProductId.clear();
    if (m_momentPipelineProductId == id)
        m_momentPipelineProductId.clear();
    if (m_lastStatisticsProductId == id)
        m_lastStatisticsProductId.clear();

    // What was open on it: the tool's own window (a channel-map grid), and the
    // recorded-numbers window a row's double-click opens. Only a tool or result
    // dialog goes: a product may also name a whole viewer as its window (a
    // baseline opened as a cube of its own, this window for pane products), and
    // a viewer holds more than this product. A dialog another product still
    // shows stays too.
    const AnalysisProduct p = ProductRegistry::instance().product(id);
    if (QWidget *win = p.window.data()) {
        const bool isViewer = qobject_cast<vtkWindowCube *>(win) || qobject_cast<vtkWindowImage *>(win);
        bool usedByOther = false;
        for (const AnalysisProduct &other : ProductRegistry::instance().all())
            if (other.productId != id && other.window.data() == win)
                usedByOther = true;
        if (win != this && win != pvWidget && !isViewer && !usedByOther)
            win->close();
    }
    if (auto *recorded = this->findChild<ToolResultDialog *>(u"recorded-result:"_s + id))
        recorded->close();

    ProductRegistry::instance().removeProduct(id);
}

void vtkWindowCube::removeImageProduct(const QString &stableKey)
{
    const QString id = this->scopedProductId(stableKey);
    if (!m_products2d.contains(id) && !m_products1d.contains(id))
        return;
    this->dropSessionProduct(id);
    refreshPaneChrome();
}

void vtkWindowCube::removeSessionProducts(const QStringList &productIds)
{
    ProductRegistry &reg = ProductRegistry::instance();
    // The temporary files to delete, read BEFORE the rows go: what a product
    // says about its storage lives in the registry entry.
    QStringList files;
    if (this->backendIsLocal()) {
        for (const QString &id : productIds) {
            const QString file =
                    visivo::removal::temporaryFileToDelete(reg.product(id), QDir::tempPath());
            if (!file.isEmpty())
                files << file;
        }
    }

    for (const QString &id : productIds)
        this->dropSessionProduct(id);
    refreshPaneChrome();
    saveLayoutSettings();   // the saved layout names panes by product: forget these

    // A baseline cube opened as a cube of its own reads that file for as long
    // as its window lives: leave the file when any other window is on it.
    // A window on its way out (hidden, waiting for its work to end) still reads
    // the file until it is destroyed, so the delete waits for the last one.
    for (const QString &file : std::as_const(files))
        removeTemporaryFileWhenUnused(file, this);
    this->statusBar()->showMessage(productIds.size() == 1
                                           ? u"Removed from this session."_s
                                           : u"Removed %1 items from this session."_s.arg(
                                                     QString::number(productIds.size())),
                                   4000);
}

void vtkWindowCube::removeTemporaryFileWhenUnused(const QString &file, const QObject *except)
{
    QList<vtkWindowCube *> holders;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        auto *other = qobject_cast<vtkWindowCube *>(w);
        if (other && other != except
            && QFileInfo(other->datasetPersistKey()).canonicalFilePath() == file)
            holders << other;
    }
    if (holders.isEmpty()) {
        QFile::remove(file);
        return;
    }
    // A live viewer keeps the file for good: it is that window's data now.
    for (vtkWindowCube *holder : std::as_const(holders))
        if (holder->isVisible() && !holder->m_closeWhenIdle)
            return;
    // Only closing viewers hold it: watch one, and look again when it is
    // destroyed (one connection at a time, so the checks do not multiply).
    QObject::connect(holders.first(), &QObject::destroyed, qApp, [file](QObject *gone) {
        removeTemporaryFileWhenUnused(file, gone);
    });
}

void vtkWindowCube::setupSessionRemoval()
{
    SessionDataTree *tree = this->m_workspaceChrome ? this->m_workspaceChrome->sessionTree()
                                                    : nullptr;
    if (!tree)
        return;
    visivo::removal::Handler handler;
    handler.remove = [this](const QStringList &ids) { this->removeSessionProducts(ids); };
    tree->setRemovalHandler(std::move(handler));
}

void vtkWindowCube::appendSessionRemoveAction(QMenu &menu, const QString &productId)
{
    if (SessionDataTree *tree = this->m_workspaceChrome ? this->m_workspaceChrome->sessionTree()
                                                        : nullptr)
        tree->appendRemoveAction(&menu, productId);
}
