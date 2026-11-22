#include "vtkWindowCatalogue3D.h"

#include "CatalogueSelectionKey.h"
#include "LinkedWindowRegistry.h"
#include "RaytraceViewport.h"
#include "RemotePointView.h"
#include "SnapshotTimelineSteps.h"
#include "app/DatasetName.h"
#include "ui_vtkWindowCatalogue3D.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QJsonArray>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSet>
#include <QSignalBlocker>
#include <QSlider>
#include <QStatusBar>
#include <QTimer>
#include <QUuid>
#include <QtConcurrent>

#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCommand.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;

namespace {

LinkedWindowRegistry<vtkWindowCatalogue3D> &comparisonWindows()
{
    static LinkedWindowRegistry<vtkWindowCatalogue3D> windows;
    return windows;
}

} // namespace

void vtkWindowCatalogue3D::openQuantityComparison()
{
    if (!this->m_pointTable || this->remoteDatasetId.isEmpty())
        return;
    if (this->filterBusy || this->m_sceneBuilding) {
        this->statusBar()->showMessage(
                u"Wait for the current particle load to finish before opening a comparison."_s,
                6000);
        return;
    }

    if (this->m_comparisonGroup.isEmpty())
        this->m_comparisonGroup = QUuid::createUuid().toString(QUuid::WithoutBraces);
    this->m_comparisonLinked = true;
    this->m_comparisonCameraTracker.refit(this->comparisonCameraPose());
    this->updateWindowTitle();

    auto *peer = new vtkWindowCatalogue3D(
            this->filepath, this->backendUrl, this->backendToken, this->m_cataloguePaths,
            this->m_label, this->parentWidget(), this);
    peer->setAttribute(Qt::WA_DeleteOnClose);
    peer->setWindowFlag(Qt::Window, true);
    peer->resize(this->size());
    peer->move(this->pos() + QPoint(36, 36));
    peer->show();
    peer->raise();

    // Prefer the three quantities proposed for the MVP, then other physical
    // scalars. Repeating the action therefore opens density, temperature and
    // entropy when they exist instead of producing several rho views.
    QSet<QString> used;
    const auto group = comparisonWindows().snapshot();
    for (const QPointer<vtkWindowCatalogue3D> &window : group) {
        if (window && window != peer && window->m_comparisonGroup == this->m_comparisonGroup
            && window->comboMapColor)
            used.insert(window->comboMapColor->currentData().toString());
    }
    const QStringList preferred{ u"rho"_s, u"density"_s, u"temperature"_s, u"temp"_s,
                                 u"entropy"_s, u"k"_s, u"u"_s, u"pressure"_s,
                                 u"metallicity"_s, u"mass"_s, u"sfr"_s, u"age"_s };
    int quantity = -1;
    const auto usable = [peer, &used](int item) {
        const QString key = peer->comboMapColor->itemData(item).toString();
        if (!key.startsWith(u"raw:"_s) || used.contains(key))
            return false;
        const QString name = peer->rawHeaders.value(key.mid(4).toInt());
        const QStringList coordinates{ peer->schema.xFieldName, peer->schema.yFieldName,
                                       peer->schema.zFieldName, u"vx"_s, u"vy"_s, u"vz"_s };
        return !coordinates.contains(name, Qt::CaseInsensitive);
    };
    for (const QString &wanted : preferred) {
        for (int i = 0; i < peer->comboMapColor->count(); ++i) {
            if (!usable(i))
                continue;
            const QString key = peer->comboMapColor->itemData(i).toString();
            if (peer->rawHeaders.value(key.mid(4).toInt()).compare(
                        wanted, Qt::CaseInsensitive) == 0) {
                quantity = i;
                break;
            }
        }
        if (quantity >= 0)
            break;
    }
    for (int i = 0; quantity < 0 && i < peer->comboMapColor->count(); ++i)
        if (usable(i))
            quantity = i;
    if (quantity >= 0)
        peer->comboMapColor->setCurrentIndex(quantity);
    if (this->m_comparisonRepresentationLinked)
        peer->applyComparisonRepresentation(this->comparisonRepresentation());

    // The count and quantity are persistent in every member of the group. This
    // is deliberately stronger than the transient status-bar message below.
    const auto updatedGroup = comparisonWindows().snapshot();
    for (const QPointer<vtkWindowCatalogue3D> &window : updatedGroup) {
        if (window && window->m_comparisonGroup == this->m_comparisonGroup)
            window->updateComparisonUi();
    }

    // The constructor builds the scene before it can join the group. Give the
    // new view the already-framed camera now, so it opens on precisely the
    // same region rather than briefly asking the user to align it by hand.
    if (this->m_rtView && !this->m_rtView->camera().isEmpty())
        peer->applyComparisonRaytraceCamera(this->m_rtView->camera());
    else
        peer->applyComparisonCamera(this->comparisonCameraPose());
    if (this->selectedIndex >= 0)
        this->broadcastComparisonSelection(this->selectedIndex);

    this->statusBar()->showMessage(
            u"Comparison opened: camera and source selection are linked; colour, range and "
            u"scale remain independent."_s,
            8000);
}

void vtkWindowCatalogue3D::setupComparisonLink(vtkWindowCatalogue3D *source)
{
    comparisonWindows().add(this);
    if (source) {
        this->m_comparisonGroup = source->m_comparisonGroup;
        this->m_comparisonLinked = true;
        this->m_comparisonRepresentationLinked = source->m_comparisonRepresentationLinked;
    }
    this->updateComparisonUi();

    vtkNew<vtkCallbackCommand> callback;
    callback->SetClientData(this);
    callback->SetCallback([](vtkObject *, unsigned long, void *data, void *) {
        static_cast<vtkWindowCatalogue3D *>(data)->onComparisonCameraFrame();
    });
    this->m_comparisonCameraObserverTag =
            this->renderWindow->AddObserver(vtkCommand::EndEvent, callback);
    this->m_comparisonCameraTracker.refit(this->comparisonCameraPose());

    QObject::connect(this, &vtkWindowCatalogue3D::sourceSelectionChanged, this,
                     &vtkWindowCatalogue3D::broadcastComparisonSelection);
    if (this->comboGeometry)
        QObject::connect(this->comboGeometry, &QComboBox::currentIndexChanged, this,
                         [this]() { this->broadcastComparisonRepresentation(); });
    if (ui->sliderScale)
        QObject::connect(ui->sliderScale, &QSlider::valueChanged, this,
                         [this]() { this->broadcastComparisonRepresentation(); });
    if (this->comboRenderBudget)
        QObject::connect(this->comboRenderBudget, &QComboBox::currentIndexChanged, this,
                         [this]() { this->broadcastComparisonRepresentation(); });
    if (this->m_rtLightCheck)
        QObject::connect(this->m_rtLightCheck, &QCheckBox::toggled, this,
                         [this]() { this->broadcastComparisonRepresentation(); });
}

void vtkWindowCatalogue3D::teardownComparisonLink()
{
    const QString group = this->m_comparisonGroup;
    if (this->m_comparisonCameraObserverTag && this->renderWindow) {
        this->renderWindow->RemoveObserver(this->m_comparisonCameraObserverTag);
        this->m_comparisonCameraObserverTag = 0;
    }
    comparisonWindows().remove(this);
    if (!group.isEmpty()) {
        const auto remaining = comparisonWindows().snapshot();
        int groupSize = 0;
        vtkWindowCatalogue3D *last = nullptr;
        for (const QPointer<vtkWindowCatalogue3D> &window : remaining) {
            if (window && window->m_comparisonGroup == group) {
                ++groupSize;
                last = window;
            }
        }
        // A comparison needs at least two views. Do not leave the last window
        // claiming to be linked to a peer that no longer exists.
        if (groupSize == 1 && last) {
            last->m_comparisonGroup.clear();
            last->m_comparisonLinked = false;
        }
        for (const QPointer<vtkWindowCatalogue3D> &window : remaining) {
            if (window && (window->m_comparisonGroup == group || window == last))
                window->updateComparisonUi();
        }
    }
}

void vtkWindowCatalogue3D::updateComparisonUi()
{
    const bool inGroup = !this->m_comparisonGroup.isEmpty();
    const bool linked = this->m_comparisonLinked && inGroup;
    if (this->m_comparisonCard)
        this->m_comparisonCard->setVisible(inGroup);
    if (this->m_comparisonLinkToggle) {
        const QSignalBlocker block(this->m_comparisonLinkToggle);
        this->m_comparisonLinkToggle->setChecked(linked);
        this->m_comparisonLinkToggle->setText(linked ? u"Linked"_s : u"Unlinked"_s);
    }
    if (this->m_comparisonRepresentationToggle) {
        const QSignalBlocker block(this->m_comparisonRepresentationToggle);
        this->m_comparisonRepresentationToggle->setChecked(
                this->m_comparisonRepresentationLinked);
        this->m_comparisonRepresentationToggle->setEnabled(linked);
    }
    if (!inGroup) {
        this->updateWindowTitle();
        return;
    }

    int views = 0;
    const auto windows = comparisonWindows().snapshot();
    for (const QPointer<vtkWindowCatalogue3D> &window : windows) {
        if (window && window->m_comparisonGroup == this->m_comparisonGroup)
            ++views;
    }
    const QString quantity = this->comboMapColor
            ? this->comboMapColor->currentText()
            : u"Not selected"_s;
    if (this->m_comparisonSummary) {
        this->m_comparisonSummary->setText(
                u"<b>%1 · %2 VIEWS</b><br>"
                 "Quantity in this view: <b>%3</b><br>"
                 "<small>Linked: camera and selection%4<br>"
                 "Independent: quantity, colormap, colour range and stretch</small>"_s
                        .arg(linked ? u"LINKED COMPARISON"_s : u"UNLINKED VIEW"_s)
                        .arg(views)
                        .arg(quantity.toHtmlEscaped())
                        .arg(this->m_comparisonRepresentationLinked
                                     ? u", representation"_s
                                     : u"; rendering is autonomous"_s)
                + (linked && this->drawnOnBackend()
                           // The backend turns this view: its moves are not
                           // seen here, so they cannot be sent (issue #446).
                           ? u"<br><small><b>Drawn on the backend:</b> this view follows the "
                              "group's camera, but turning it here does not move the "
                              "others. Use View ▸ Rendering ▸ Local to lead the group."
                              "</small>"_s
                           : QString()));
    }
    this->updateWindowTitle();
}

vtkWindowCatalogue3D::ComparisonRepresentation
vtkWindowCatalogue3D::comparisonRepresentation() const
{
    ComparisonRepresentation state;
    if (this->comboGeometry)
        state.geometry = this->comboGeometry->currentText();
    if (ui->sliderScale)
        state.sizeSlider = ui->sliderScale->value();
    if (this->comboRenderBudget)
        state.renderBudget = this->comboRenderBudget->currentData().toInt();
    state.raytraced = this->m_rtLightCheck && this->m_rtLightCheck->isChecked();
    return state;
}

void vtkWindowCatalogue3D::applyComparisonRepresentation(
        const ComparisonRepresentation &state)
{
    const QScopedValueRollback<bool> applying(this->m_applyingComparisonUpdate, true);
    if (this->comboGeometry && !state.geometry.isEmpty()) {
        const int item = this->comboGeometry->findText(state.geometry);
        if (item >= 0)
            this->comboGeometry->setCurrentIndex(item);
    }
    if (ui->sliderScale)
        ui->sliderScale->setValue(state.sizeSlider);
    if (this->comboRenderBudget) {
        const int item = this->comboRenderBudget->findData(state.renderBudget);
        if (item >= 0)
            this->comboRenderBudget->setCurrentIndex(item);
    }
    if (this->m_rtLightCheck)
        this->m_rtLightCheck->setChecked(state.raytraced);
}

void vtkWindowCatalogue3D::broadcastComparisonRepresentation()
{
    const auto flags = [](const vtkWindowCatalogue3D *window) {
        return SnapshotTimelineSteps::LinkFlags{ !window->m_comparisonGroup.isEmpty(),
                                                 window->m_comparisonLinked,
                                                 window->m_comparisonRepresentationLinked,
                                                 window->m_applyingComparisonUpdate };
    };
    if (!SnapshotTimelineSteps::representationSends(flags(this)))
        return;
    const ComparisonRepresentation state = this->comparisonRepresentation();
    const auto windows = comparisonWindows().snapshot();
    for (const QPointer<vtkWindowCatalogue3D> &window : windows) {
        if (window && window != this && window->m_comparisonGroup == this->m_comparisonGroup
            && SnapshotTimelineSteps::representationReceives(flags(window)))
            window->applyComparisonRepresentation(state);
    }
}

void vtkWindowCatalogue3D::setComparisonLinked(bool linked)
{
    if (this->m_comparisonGroup.isEmpty())
        return;
    if (!linked) {
        this->m_comparisonLinked = false;
        this->m_comparisonCameraTracker.refit(this->comparisonCameraPose());
        this->updateComparisonUi();
        this->statusBar()->showMessage(
                u"View unlinked: camera, selection and representation are now autonomous."_s,
                6000);
        return;
    }

    vtkWindowCatalogue3D *reference = nullptr;
    const auto windows = comparisonWindows().snapshot();
    for (const QPointer<vtkWindowCatalogue3D> &window : windows) {
        if (window && window != this && window->m_comparisonGroup == this->m_comparisonGroup
            && window->m_comparisonLinked) {
            reference = window;
            break;
        }
    }
    // If every member was unlinked, the first peer becomes the reference so a
    // single click reconnects the comparison instead of waiting for two clicks.
    if (!reference) {
        for (const QPointer<vtkWindowCatalogue3D> &window : windows) {
            if (window && window != this
                && window->m_comparisonGroup == this->m_comparisonGroup) {
                reference = window;
                reference->m_comparisonLinked = true;
                reference->updateComparisonUi();
                break;
            }
        }
    }
    this->m_comparisonLinked = true;
    if (reference) {
        if (this->m_comparisonRepresentationLinked)
            this->applyComparisonRepresentation(reference->comparisonRepresentation());
        if (reference->m_rtView && !reference->m_rtView->camera().isEmpty())
            this->applyComparisonRaytraceCamera(reference->m_rtView->camera());
        else
            this->applyComparisonCamera(reference->comparisonCameraPose());
        const CatalogueSelection::Key selected = CatalogueSelection::keyOf(
                reference->entries, reference->schema, reference->selectedIndex);
        const int index = CatalogueSelection::indexOf(this->entries, this->schema, selected);
        const QScopedValueRollback<bool> applying(this->m_applyingComparisonUpdate, true);
        // The group's selection, not the user's: not sent over SAMP again.
        const QScopedValueRollback<bool> samp(this->m_applyingSampSelection, true);
        this->setSelectedSource(index);
    }
    this->m_comparisonCameraTracker.adopt(this->comparisonCameraPose());
    this->updateComparisonUi();
    this->statusBar()->showMessage(
            this->m_comparisonRepresentationLinked
                    ? u"View linked: camera, selection and representation synchronized. "
                       "Quantity and colour mapping remain independent."_s
                    : u"View linked: camera and selection synchronized; rendering remains autonomous."_s,
            8000);
}

void vtkWindowCatalogue3D::setComparisonRepresentationLinked(bool linked)
{
    this->m_comparisonRepresentationLinked = linked;
    if (linked && this->m_comparisonLinked && !this->m_comparisonGroup.isEmpty()) {
        const auto windows = comparisonWindows().snapshot();
        for (const QPointer<vtkWindowCatalogue3D> &window : windows) {
            if (window && window != this && window->m_comparisonLinked
                && window->m_comparisonRepresentationLinked
                && window->m_comparisonGroup == this->m_comparisonGroup) {
                this->applyComparisonRepresentation(window->comparisonRepresentation());
                break;
            }
        }
    }
    this->updateComparisonUi();
    this->statusBar()->showMessage(
            linked
                    ? u"Rendering matched: shape, size, budget and ray-traced light follow the group."_s
                    : u"Rendering autonomous: camera and selection remain linked."_s,
            7000);
}

void vtkWindowCatalogue3D::setupSnapshotTimeline(vtkWindowCatalogue3D *source)
{
    // A catalogue opened from several files is pieces of one output, which a
    // step would have read as a run's outputs.
    if (!this->m_pointTable || this->remoteDatasetId.isEmpty()
        || !SnapshotTimelineSteps::timelineApplies(this->m_cataloguePaths.size()))
        return;
    if (source) {
        this->m_snapshotSeries = source->m_snapshotSeries;
        this->updateSnapshotTimelineUi();
        return;
    }
    // A step's window is handed the series it steps along: listing (and
    // reading every output's header) again per step is not needed.
    if (this->m_openedByStep)
        return;

    QObject::connect(&this->m_snapshotSeriesWatcher,
                     &QFutureWatcher<BackendPointSeriesResult>::finished, this, [this]() {
        const BackendPointSeriesResult result = this->m_snapshotSeriesWatcher.result();
        const auto windows = comparisonWindows().snapshot();
        for (const QPointer<vtkWindowCatalogue3D> &window : windows) {
            if (!window)
                continue;
            if (window == this
                || (!this->m_comparisonGroup.isEmpty()
                    && window->m_comparisonGroup == this->m_comparisonGroup)) {
                // A failed reply (the window opened by a step asks again)
                // never hides a timeline that works, in the middle of Play.
                if (!SnapshotTimelineSteps::acceptsSeriesReply(window->m_snapshotSeries.valid,
                                                               result.valid))
                    continue;
                BackendPointSeriesResult merged = result;
                if (merged.valid && window->m_snapshotSeries.valid)
                    merged.available = SnapshotTimelineSteps::mergedAvailability(
                            window->m_snapshotSeries.paths, window->m_snapshotSeries.available,
                            merged.paths, merged.available);
                window->m_snapshotSeries = merged;
                window->updateSnapshotTimelineUi();
            }
        }
    });
    const QString url = this->backendUrl;
    const QString token = this->backendToken;
    const QString session = this->remoteSessionId;
    const QString dataset = this->remoteDatasetId;
    this->m_snapshotSeriesWatcher.setFuture(QtConcurrent::run(
            [url, token, session, dataset]() {
                BackendClient client(url, token);
                client.setSessionId(session);
                return client.requestPointSeries(dataset);
            }));
}

void vtkWindowCatalogue3D::updateSnapshotTimelineUi()
{
    const BackendPointSeriesResult &series = this->m_snapshotSeries;
    const bool available = SnapshotTimelineSteps::timelineApplies(this->m_cataloguePaths.size())
            && series.valid && series.paths.size() > 1 && series.index >= 0;
    if (this->m_timelineCard)
        this->m_timelineCard->setVisible(available);
    if (!available)
        return;

    const int index = series.index;
    const int count = static_cast<int>(series.paths.size());
    const int number = series.numbers.value(index);
    QString details = u"Output %1 · %2 of %3"_s
            .arg(number).arg(index + 1).arg(series.count);
    const double time = this->m_snapshotMetadata.value(u"time"_s).toDouble(
            std::numeric_limits<double>::quiet_NaN());
    const double redshift = this->m_snapshotMetadata.value(u"redshift"_s).toDouble(
            std::numeric_limits<double>::quiet_NaN());
    if (std::isfinite(time))
        details += u" · time %1"_s.arg(time, 0, 'g', 5);
    if (std::isfinite(redshift) && redshift >= 0.0)
        details += u" · z %1"_s.arg(redshift, 0, 'g', 5);
    const auto skipped = static_cast<int>(series.available.count(false));
    if (skipped > 0) {
        details += u"\n%1 output(s) without this particle type, or that do not open, are "
                   "skipped."_s.arg(skipped);
    }
    details += u"\nChanging output clears source selection; it does not track particles."_s;
    if (this->m_timelineSummary)
        this->m_timelineSummary->setText(details);
    if (this->m_timelineSlider) {
        const QSignalBlocker block(this->m_timelineSlider);
        this->m_timelineSlider->setRange(0, count - 1);
        this->m_timelineSlider->setValue(index);
        this->m_timelineSlider->setEnabled(!this->m_snapshotSwitching);
    }
    if (this->m_timelinePrevious)
        this->m_timelinePrevious->setEnabled(
                !this->m_snapshotSwitching
                && SnapshotTimelineSteps::stepTarget(series.available, count, index, -1) >= 0);
    if (this->m_timelineNext)
        this->m_timelineNext->setEnabled(
                !this->m_snapshotSwitching
                && SnapshotTimelineSteps::stepTarget(series.available, count, index, 1) >= 0);
    if (this->m_timelinePlay) {
        this->m_timelinePlay->setText(this->m_snapshotPlaying ? u"Pause"_s : u"Play"_s);
        this->m_timelinePlay->setEnabled(!this->m_snapshotSwitching);
    }
}

void vtkWindowCatalogue3D::stepSnapshot(int delta)
{
    if (!this->m_snapshotSeries.valid || this->m_snapshotSwitching)
        return;
    const int target = SnapshotTimelineSteps::stepTarget(
            this->m_snapshotSeries.available, static_cast<int>(this->m_snapshotSeries.paths.size()),
            this->m_snapshotSeries.index, delta);
    if (target < 0) {
        if (this->m_snapshotPlaying)
            this->stopSnapshotPlayback();
        return;
    }
    this->switchSnapshotIndex(target);
}

void vtkWindowCatalogue3D::toggleSnapshotPlayback()
{
    const bool playing = !this->m_snapshotPlaying;
    const auto windows = comparisonWindows().snapshot();
    for (const QPointer<vtkWindowCatalogue3D> &window : windows) {
        if (!window)
            continue;
        if (window == this
            || (!this->m_comparisonGroup.isEmpty()
                && window->m_comparisonGroup == this->m_comparisonGroup)) {
            window->m_snapshotPlaying = playing;
            if (window->m_timelineTimer)
                window->m_timelineTimer->stop();
            window->updateSnapshotTimelineUi();
        }
    }
    if (playing && this->m_timelineTimer)
        this->m_timelineTimer->start();
}

void vtkWindowCatalogue3D::stopSnapshotPlayback()
{
    if (this->m_snapshotPlaying)
        this->toggleSnapshotPlayback();
}

bool vtkWindowCatalogue3D::linkedWithPeers() const
{
    if (!this->m_comparisonLinked || this->m_comparisonGroup.isEmpty())
        return false;
    // Closed windows leave the registry in closeEvent: the last one left is
    // no longer in a comparison, whatever its flags still say.
    const auto windows = comparisonWindows().snapshot();
    return std::any_of(windows.cbegin(), windows.cend(),
                       [this](const QPointer<vtkWindowCatalogue3D> &window) {
                           return window && window != this
                                   && window->m_comparisonGroup == this->m_comparisonGroup;
                       });
}

QString vtkWindowCatalogue3D::timelineBusyReason() const
{
    const auto windows = comparisonWindows().snapshot();
    for (const QPointer<vtkWindowCatalogue3D> &window : windows) {
        if (!window)
            continue;
        if (window != this
            && (this->m_comparisonGroup.isEmpty()
                || window->m_comparisonGroup != this->m_comparisonGroup))
            continue;
        // A window building its scene refuses to close, and would be left
        // behind by the step; the others would lose what they are loading.
        if (window->m_snapshotSwitching)
            return u"the output being opened is ready"_s;
        if (window->m_sceneBuilding)
            return u"the scene is built"_s;
        if (window->m_awaitingHydration)
            return u"the particles' quantities are loaded"_s;
        if (window->filterBusy || window->m_loadingMore)
            return u"the particles are loaded"_s;
    }
    return {};
}

void vtkWindowCatalogue3D::startAttributeHydration()
{
    if (!this->m_awaitingHydration || this->m_hydratingAttributes)
        return;
    if (this->remoteDatasetId.isEmpty() || this->m_openFailedAtMs >= 0) {
        this->m_awaitingHydration = false;
        this->shareHydratedRows(HydrationShare::StopWaiting);
        return;
    }
    // Another query is answering with every column: its answer ends the wait.
    if (this->filterBusy)
        return;
    if (this->m_sceneBuilding) {
        QTimer::singleShot(200, this, [this]() { this->startAttributeHydration(); });
        return;
    }
    // Ordinary asynchronous filtering with no new filters: the fixed RNG
    // returns the same row indices and the watcher atomically replaces the
    // partial rows once all columns have arrived.
    // FOLLOW-UP (#450): applyFilter() queries the filter list as edited;
    // once m_appliedFilters exists, the columns must be asked for with the
    // filters the loaded rows were selected by.
    this->m_hydratingAttributes = true;
    this->applyFilter();
    if (!this->filterBusy) {
        this->m_hydratingAttributes = false;
        this->m_awaitingHydration = false;
        this->shareHydratedRows(HydrationShare::StopWaiting);
        return;
    }
    if (this->filterStatusLabel)
        this->filterStatusLabel->setText(u"Loading particle attributes in background…"_s);
}

void vtkWindowCatalogue3D::shareHydratedRows(HydrationShare how)
{
    if (this->remoteDatasetId.isEmpty())
        return;
    // Comparison views opened before the columns arrived hold the same rows
    // without them: their quantity drew as one flat colour (issue #446).
    const auto windows = comparisonWindows().snapshot();
    for (const QPointer<vtkWindowCatalogue3D> &window : windows) {
        if (!window || window == this || !window->m_awaitingHydration
            || window->m_hydratingAttributes
            || window->remoteDatasetId != this->remoteDatasetId)
            continue;
        switch (how) {
        case HydrationShare::Rows:
            window->adoptHydratedRows(*this);
            break;
        case HydrationShare::StopWaiting:
            window->m_awaitingHydration = false;
            break;
        case HydrationShare::LoadOwn: {
            QPointer<vtkWindowCatalogue3D> peer = window;
            QTimer::singleShot(0, peer, [peer]() {
                if (peer)
                    peer->startAttributeHydration();
            });
            break;
        }
        }
    }
}

void vtkWindowCatalogue3D::adoptHydratedRows(const vtkWindowCatalogue3D &source)
{
    // Busy with its own query, whose answer brings every column too.
    if (this->filterBusy)
        return;
    // Its rows are not the source's any more (a page loaded or loading
    // here), or cannot be replaced mid-build: it asks for the columns itself.
    const auto rows = [](const vtkWindowCatalogue3D &w) {
        return w.entries.size() + w.unplacedEntries.size();
    };
    if (this->m_sceneBuilding || this->m_loadingMore || rows(*this) != rows(source)) {
        QTimer::singleShot(0, this, [this]() { this->startAttributeHydration(); });
        return;
    }
    const CatalogueSelection::Key selected =
            CatalogueSelection::keyOf(this->entries, this->schema, this->selectedIndex);
    this->entries = source.entries;
    this->unplacedEntries = source.unplacedEntries;
    this->m_currentOffset = source.m_currentOffset;
    this->m_sampleStep = source.m_sampleStep;
    this->m_samplePhase = source.m_samplePhase;
    this->m_sampleLimit = source.m_sampleLimit;
    this->m_totalCount = source.m_totalCount;
    this->m_awaitingHydration = false;
    // Placed in this view's frame and cosmology, not the source's.
    this->applyFrameToEntries();
    this->reapplyCosmologyAfterReload();

    this->renderer->RemoveAllViewProps();
    this->morphologyNames.clear();
    this->morphologyIndexOf.clear();
    this->labelActors.clear();
    this->shellActors.clear();
    this->mappedPositions.clear();
    this->hoveredIndex = -1;
    this->selectedIndex = CatalogueSelection::indexOf(this->entries, this->schema, selected);
    if (!this->entries.empty()) {
        // The same rows: the camera stays where the step or the user put it.
        this->buildScene(SnapshotTimelineSteps::Rebuild::SameRows);
        this->buildLabels();
    }
    this->refreshCatalogueTable();
    this->refreshViewerChrome();
    this->refreshSelectionAfterReplacement();
    this->updateComparisonUi();
    this->renderWindow->Render();
}

void vtkWindowCatalogue3D::switchSnapshotIndex(int requested)
{
    if (!this->m_snapshotSeries.valid || this->m_snapshotSwitching)
        return;
    const int count = static_cast<int>(this->m_snapshotSeries.paths.size());
    // The slider can land on an output that cannot be opened: the nearest
    // that can, in the direction it moved.
    const int index = SnapshotTimelineSteps::sliderTarget(
            this->m_snapshotSeries.available, count, this->m_snapshotSeries.index, requested);
    if (index < 0) {
        this->updateSnapshotTimelineUi();
        return;
    }
    const QString busy = this->timelineBusyReason();
    if (!busy.isEmpty()) {
        // Play tries again on its next tick; a click is told why.
        if (!this->m_snapshotPlaying)
            this->statusBar()->showMessage(
                    u"Wait until %1 before changing output."_s.arg(busy), 5000);
        this->updateSnapshotTimelineUi();
        return;
    }

    struct ViewState {
        QPointer<vtkWindowCatalogue3D> old;
        QString colourBy;
        QString colourName;   ///< the column colourBy names, matched by name
        QString colormap;
        int colourScale{ 0 };
        double gamma{ 2.0 };
        int geometry{ 0 };
        int sizeSlider{ 0 };
        int renderBudget{ 1000000 };
        bool raytraced{ false };
        bool linked{ true };
        bool representationLinked{ true };
        LinkedCameraSync::Pose camera;
        QJsonObject rayCamera;
        QRect windowGeometry;
    };
    QList<ViewState> states;
    const auto registered = comparisonWindows().snapshot();
    for (const QPointer<vtkWindowCatalogue3D> &window : registered) {
        if (!window)
            continue;
        const bool same = window == this
                || (!this->m_comparisonGroup.isEmpty()
                    && window->m_comparisonGroup == this->m_comparisonGroup);
        if (!same)
            continue;
        ViewState state;
        state.old = window;
        state.colourBy = window->comboMapColor
                ? window->comboMapColor->currentData().toString() : QString();
        if (state.colourBy.startsWith(u"raw:"_s))
            state.colourName = window->rawHeaders.value(state.colourBy.mid(4).toInt());
        state.colormap = window->comboColormap
                ? window->comboColormap->currentData().toString() : QString();
        state.colourScale = window->comboColorScale
                ? window->comboColorScale->currentIndex() : 0;
        state.gamma = window->spinColorGamma ? window->spinColorGamma->value() : 2.0;
        state.geometry = window->comboGeometry ? window->comboGeometry->currentIndex() : 0;
        state.sizeSlider = window->ui->sliderScale ? window->ui->sliderScale->value() : 0;
        state.renderBudget = window->comboRenderBudget
                ? window->comboRenderBudget->currentData().toInt() : 1000000;
        state.raytraced = window->m_rtLightCheck && window->m_rtLightCheck->isChecked();
        state.linked = window->m_comparisonLinked;
        state.representationLinked = window->m_comparisonRepresentationLinked;
        state.camera = window->comparisonCameraPose();
        state.rayCamera = window->m_rtView ? window->m_rtView->camera() : QJsonObject{};
        state.windowGeometry = window->geometry();
        states.append(state);
    }
    if (states.isEmpty())
        return;

    const bool resumePlayback = std::any_of(
            states.cbegin(), states.cend(), [](const ViewState &state) {
                return state.old && state.old->m_snapshotPlaying;
            });
    for (ViewState &state : states) {
        state.old->m_snapshotSwitching = true;
        state.old->m_snapshotPlaying = false;
        if (state.old->m_timelineTimer)
            state.old->m_timelineTimer->stop();
        state.old->updateSnapshotTimelineUi();
    }

    const QString targetPath = this->m_snapshotSeries.paths.at(index);
    BackendPointSeriesResult targetSeries = this->m_snapshotSeries;
    targetSeries.index = index;
    const QString particle = this->m_componentMetadata.value(u"name"_s).toString();
    if (!particle.isEmpty())
        visivo::rememberFieldNames({ { particle, targetPath } });
    const Catalogue3DLabel label{ visivo::datasetDisplayName(targetPath), false };

    vtkWindowCatalogue3D *primary = nullptr;
    {
        // One message for the step below, not a modal box per output.
        const QScopedValueRollback<bool> quiet(s_openingStep, true);
        primary = new vtkWindowCatalogue3D(
                targetPath, this->backendUrl, this->backendToken, {}, label,
                this->parentWidget(), nullptr);
    }
    primary->setAttribute(Qt::WA_DeleteOnClose);
    if (!primary->catalogueOpened()) {
        const QString why = primary->m_openError.isEmpty()
                ? u"it could not be opened"_s
                : primary->m_openError;
        // Opened on the backend and then failed to load: not left held there.
        primary->releaseDataset();
        primary->deleteLater();
        for (ViewState &state : states) {
            if (!state.old)
                continue;
            BackendPointSeriesResult &series = state.old->m_snapshotSeries;
            // Next, Previous and Play step over it from now on.
            if (series.available.size() != series.paths.size())
                series.available = QVector<bool>(series.paths.size(), true);
            if (index < series.available.size())
                series.available[index] = false;
            state.old->m_snapshotSwitching = false;
            state.old->m_snapshotPlaying = false;
            if (state.old->m_timelineTimer)
                state.old->m_timelineTimer->stop();
            state.old->updateSnapshotTimelineUi();
        }
        // Play stops: retrying every tick put up one error box after another.
        this->statusBar()->showMessage(
                u"Output %1 not opened: %2.%3 It is skipped from now on."_s
                        .arg(targetSeries.numbers.value(index))
                        .arg(why)
                        .arg(resumePlayback ? u" Playback stopped."_s : QString()),
                15000);
        return;
    }

    QStringList missingQuantities;
    const auto restore = [&missingQuantities](vtkWindowCatalogue3D *window,
                                              const ViewState &state) {
        if (window->comboMapColor) {
            // By the column's name: another output may list its columns in
            // another order, and an index would show another quantity.
            const QString key = SnapshotTimelineSteps::quantityKey(
                    state.colourBy, state.colourName, window->rawHeaders);
            const int item = key.isEmpty() ? -1 : window->comboMapColor->findData(key);
            if (item >= 0)
                window->comboMapColor->setCurrentIndex(item);
            else if (!state.colourName.isEmpty() && !missingQuantities.contains(state.colourName))
                missingQuantities.append(state.colourName);
        }
        if (window->comboColormap) {
            const int item = window->comboColormap->findData(state.colormap);
            if (item >= 0)
                window->comboColormap->setCurrentIndex(item);
        }
        if (window->comboColorScale)
            window->comboColorScale->setCurrentIndex(state.colourScale);
        if (window->spinColorGamma)
            window->spinColorGamma->setValue(state.gamma);
        if (window->comboGeometry)
            window->comboGeometry->setCurrentIndex(state.geometry);
        // FOLLOW-UP (#453): once m_sliderAutomatic exists, save it in ViewState
        // and put it back after this setValue, which marks the size as the
        // user's (a later switch to Points would keep the splats' size).
        if (window->ui->sliderScale)
            window->ui->sliderScale->setValue(state.sizeSlider);
        if (window->comboRenderBudget) {
            const int item = window->comboRenderBudget->findData(state.renderBudget);
            if (item >= 0)
                window->comboRenderBudget->setCurrentIndex(item);
        }
        if (window->m_rtLightCheck && state.raytraced)
            window->m_rtLightCheck->setChecked(true);
        window->setGeometry(state.windowGeometry);
    };
    // Each window takes its own link flags before its controls, under the
    // applying guard: a new window starts with its source's flags, and an
    // unlinked view's restored shape reached the linked ones (issue #446).
    const auto restoreView = [&restore](vtkWindowCatalogue3D *window, const ViewState &state) {
        SnapshotTimelineSteps::restoreWithOwnFlags(
                window->m_comparisonLinked, window->m_comparisonRepresentationLinked,
                window->m_applyingComparisonUpdate, state.linked, state.representationLinked,
                [&]() { restore(window, state); });
    };

    primary->m_snapshotSeries = targetSeries;
    primary->updateSnapshotTimelineUi();
    restoreView(primary, states.first());
    QList<QPointer<vtkWindowCatalogue3D>> replacements{ primary };
    if (states.size() > 1) {
        primary->m_comparisonGroup = QUuid::createUuid().toString(QUuid::WithoutBraces);
        for (int i = 1; i < states.size(); ++i) {
            // Made from the primary: they share its dataset and take its rows'
            // other columns when its background query brings them.
            auto *peer = new vtkWindowCatalogue3D(
                    targetPath, this->backendUrl, this->backendToken, {}, label,
                    states.at(i).old ? states.at(i).old->parentWidget() : this->parentWidget(),
                    primary);
            peer->setAttribute(Qt::WA_DeleteOnClose);
            peer->m_snapshotSeries = targetSeries;
            restoreView(peer, states.at(i));
            replacements.append(peer);
        }
    }
    for (int i = 0; i < replacements.size(); ++i) {
        const QPointer<vtkWindowCatalogue3D> &window = replacements.at(i);
        if (!window)
            continue;
        window->m_snapshotSwitching = false;
        const ViewState &state = states.at(i);
        // The camera the view showed: the ray-traced one when it had it (it
        // sets the VTK camera too), so the view is not drawn twice.
        if (!state.rayCamera.isEmpty())
            window->applyComparisonRaytraceCamera(state.rayCamera);
        else
            window->applyComparisonCamera(state.camera);
        window->updateComparisonUi();
        window->updateSnapshotTimelineUi();
        window->show();
        window->raise();
    }
    QString message = u"Snapshot changed for every linked view. Source selection was cleared; "
                       "no cross-snapshot particle identity was assumed."_s;
    if (!missingQuantities.isEmpty())
        message += u" Not in this output: %1."_s.arg(missingQuantities.join(u", "_s));
    primary->statusBar()->showMessage(message, 10000);

    for (ViewState &state : states) {
        if (state.old)
            state.old->close();
    }
    if (resumePlayback
        && SnapshotTimelineSteps::stepTarget(targetSeries.available,
                                             static_cast<int>(targetSeries.paths.size()), index, 1)
                >= 0) {
        primary->m_snapshotPlaying = true;
        primary->updateSnapshotTimelineUi();
        if (primary->m_timelineTimer)
            primary->m_timelineTimer->start();
    }
}

LinkedCameraSync::Pose vtkWindowCatalogue3D::comparisonCameraPose() const
{
    LinkedCameraSync::Pose pose;
    vtkCamera *camera = this->renderer->GetActiveCamera();
    const double *position = camera->GetPosition();
    const double *focal = camera->GetFocalPoint();
    const double *up = camera->GetViewUp();
    for (int i = 0; i < 3; ++i) {
        pose.position[i] = position[i];
        pose.focalPoint[i] = focal[i];
        pose.viewUp[i] = up[i];
    }
    pose.viewAngle = camera->GetViewAngle();
    pose.parallelScale = camera->GetParallelScale();
    return pose;
}

void vtkWindowCatalogue3D::onComparisonCameraFrame()
{
    const LinkedCameraSync::Pose pose = this->comparisonCameraPose();
    if (!this->m_comparisonLinked || this->m_comparisonGroup.isEmpty()) {
        this->m_comparisonCameraTracker.refit(pose);
        return;
    }
    if (!this->m_comparisonCameraTracker.frame(pose, this->m_applyingComparisonUpdate))
        return;

    // Coalesce the burst of EndEvents produced by a drag. The tracker makes
    // this one send carry the latest pose and prevents peer echo.
    QTimer::singleShot(0, this, [this]() {
        if (!this->m_comparisonCameraTracker.takePending() || !this->m_comparisonLinked)
            return;
        const LinkedCameraSync::Pose latest = this->comparisonCameraPose();
        this->m_comparisonCameraTracker.adopt(latest);
        const auto peers = comparisonWindows().snapshot();
        for (const QPointer<vtkWindowCatalogue3D> &peer : peers) {
            if (peer && peer != this && peer->m_comparisonLinked
                && peer->m_comparisonGroup == this->m_comparisonGroup)
                peer->applyComparisonCamera(latest);
        }
    });
}

void vtkWindowCatalogue3D::applyComparisonCamera(const LinkedCameraSync::Pose &pose)
{
    if (!pose.finite())
        return;
    const QScopedValueRollback<bool> applying(this->m_applyingComparisonUpdate, true);
    vtkCamera *camera = this->renderer->GetActiveCamera();
    camera->SetPosition(pose.position.data());
    camera->SetFocalPoint(pose.focalPoint.data());
    camera->SetViewUp(pose.viewUp.data());
    camera->SetViewAngle(pose.viewAngle);
    if (pose.parallelScale > 0.0)
        camera->SetParallelScale(pose.parallelScale);
    this->renderer->ResetCameraClippingRange();
    this->m_comparisonCameraTracker.adopt(pose);
    this->renderWindow->Render();
    // A ray-traced or backend-drawn view shows its own camera, not this one.
    this->followCameraInShownView(this->localCameraJson());
}

void vtkWindowCatalogue3D::broadcastComparisonSelection(int index)
{
    if (!this->m_comparisonLinked || this->m_comparisonGroup.isEmpty()
        || this->m_applyingComparisonUpdate)
        return;
    const CatalogueSelection::Key key =
            CatalogueSelection::keyOf(this->entries, this->schema, index);
    const auto peers = comparisonWindows().snapshot();
    for (const QPointer<vtkWindowCatalogue3D> &peer : peers) {
        if (!peer || peer == this || !peer->m_comparisonLinked
            || peer->m_comparisonGroup != this->m_comparisonGroup)
            continue;
        const int peerIndex = index < 0
                ? -1
                : CatalogueSelection::indexOf(peer->entries, peer->schema, key);
        const QScopedValueRollback<bool> applying(peer->m_applyingComparisonUpdate, true);
        const QScopedValueRollback<bool> samp(peer->m_applyingSampSelection, true);
        peer->setSelectedSource(peerIndex);
    }
}

void vtkWindowCatalogue3D::broadcastComparisonRaytraceCamera(const QJsonObject &camera)
{
    if (!this->m_comparisonLinked || this->m_comparisonGroup.isEmpty()
        || this->m_applyingComparisonUpdate || camera.isEmpty())
        return;
    const auto peers = comparisonWindows().snapshot();
    for (const QPointer<vtkWindowCatalogue3D> &peer : peers) {
        if (peer && peer != this && peer->m_comparisonLinked
            && peer->m_comparisonGroup == this->m_comparisonGroup)
            peer->applyComparisonRaytraceCamera(camera);
    }
}

void vtkWindowCatalogue3D::applyComparisonRaytraceCamera(const QJsonObject &camera)
{
    if (camera.isEmpty())
        return;
    const auto value = [&camera](const QString &name, int component) {
        return camera.value(name).toArray().at(component).toDouble();
    };
    const QScopedValueRollback<bool> applying(this->m_applyingComparisonUpdate, true);
    vtkCamera *vtkCamera = this->renderer->GetActiveCamera();
    vtkCamera->SetPosition(value(u"position"_s, 0), value(u"position"_s, 1),
                           value(u"position"_s, 2));
    vtkCamera->SetFocalPoint(value(u"focal_point"_s, 0), value(u"focal_point"_s, 1),
                            value(u"focal_point"_s, 2));
    vtkCamera->SetViewUp(value(u"view_up"_s, 0), value(u"view_up"_s, 1),
                        value(u"view_up"_s, 2));
    if (camera.contains(u"view_angle"_s))
        vtkCamera->SetViewAngle(camera.value(u"view_angle"_s).toDouble(30.0));
    if (camera.contains(u"parallel"_s))
        vtkCamera->SetParallelProjection(camera.value(u"parallel"_s).toBool() ? 1 : 0);
    if (camera.value(u"parallel_scale"_s).toDouble() > 0.0)
        vtkCamera->SetParallelScale(camera.value(u"parallel_scale"_s).toDouble());
    this->renderer->ResetCameraClippingRange();
    this->m_comparisonCameraTracker.adopt(this->comparisonCameraPose());
    if (this->m_rtView)
        this->m_rtView->setCamera(camera);
    this->renderWindow->Render();
    // A peer drawn on the backend turns there (the ray-traced view, when it
    // is the one shown, already has the camera).
    if (this->m_remoteView && !(this->m_renderStack
                                && this->m_renderStack->currentWidget() == this->m_rtView))
        this->followCameraInShownView(camera);
}
