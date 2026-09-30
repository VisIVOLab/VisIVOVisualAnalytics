#include "VLKBInventoryTree.h"
#include "vtkWindowImage.h"

#include <functional>
#include <utility>
#include <limits>

#include <QFont>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QModelIndex>
#include <QPushButton>
#include <QStandardItem>
#include <QApplication>
#include <QHeaderView>
#include <QStandardItemModel>
#include <QStyle>
#include <QTreeView>
#include <QUuid>
#include <QVBoxLayout>
#include <QXmlStreamReader>

using namespace Qt::StringLiterals;

// Role holding a branch node's untagged base label, so the "(N)" count can be
// (re)applied idempotently without string-stripping (which would mangle a species
// name that legitimately contains parentheses, e.g. "NH3 (1,1)").
static constexpr int kBaseNameRole = Qt::UserRole + 1;

// ── Linked window ─────────────────────────────────────────────────────────────

vtkWindowImage *VLKBInventoryTree::linkedImageWindow() const
{
    return qobject_cast<vtkWindowImage *>(m_linkedImageWindow.data());
}

void VLKBInventoryTree::setLinkedImageWindow(vtkWindowImage *win)
{
    m_linkedImageWindow = win;
    // The claim is over the moment the window exists: anything arriving from
    // now on can be added straight to it.
    if (win)
        m_imageWindowPending = false;
}

bool VLKBInventoryTree::claimImageWindow()
{
    if (m_imageWindowPending || linkedImageWindow())
        return false;
    m_imageWindowPending = true;
    return true;
}

void VLKBInventoryTree::queuePendingLayer(const QString &backendPath, const QString &displayName)
{
    m_pendingLayers.append({ backendPath, displayName });
}

QList<QPair<QString, QString>> VLKBInventoryTree::takePendingLayers()
{
    m_imageWindowPending = false;
    return std::exchange(m_pendingLayers, {});
}

// ── Constructor ───────────────────────────────────────────────────────────────

VLKBInventoryTree::VLKBInventoryTree(const QByteArray &votable,
                                      const QString    &posCutout,
                                      QWidget          *parent)
    : QWidget(parent), m_posCutout(posCutout)
{
    buildUi(posCutout);
    setupWavelengthGroups();
    setupTopLevels();
    parseVOTable(votable);

    applyStyleHints(m_model->index(0, 0));
    applyStyleHints(m_model->index(1, 0));
    annotateCounts();

    // Reveal the data straight away: expand every populated branch.
    m_tree->expandAll();
    if (m_totalLabel) {
        const int n = m_cutouts.size();
        m_totalLabel->setText(n == 1 ? u"1 dataset found"_s
                                     : u"%1 datasets found"_s.arg(n));
    }
}

// ── UI builder ────────────────────────────────────────────────────────────────

void VLKBInventoryTree::buildUi(const QString &posCutout)
{
    setWindowTitle(u"VLKB Inventory"_s);
    // Three columns of real content — the dataset name, its coverage and the
    // archive filename — do not fit in 520 px: something is always elided, and
    // for a while it was the category headings. Wider by default; the columns
    // below then give every spare pixel to the names.
    resize(760, 680);

    auto *vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(10, 8, 10, 8);
    vbox->setSpacing(6);

    // Region + result count on one row.
    auto *top = new QHBoxLayout;
    auto *posLbl = new QLabel(u"Region: "_s + posCutout, this);
    posLbl->setWordWrap(true);
    posLbl->setStyleSheet(u"color: #607080; font-size: 10px;"_s);
    m_totalLabel = new QLabel(this);
    m_totalLabel->setStyleSheet(u"color: #305070; font-size: 11px; font-weight: 600;"_s);
    m_totalLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    top->addWidget(posLbl, 1);
    top->addWidget(m_totalLabel, 0);
    vbox->addLayout(top);

    // Filter box.
    m_filter = new QLineEdit(this);
    m_filter->setClearButtonEnabled(true);
    m_filter->setPlaceholderText(u"Filter datasets (survey, band, line…)"_s);
    vbox->addWidget(m_filter);
    QObject::connect(m_filter, &QLineEdit::textChanged,
                     this, &VLKBInventoryTree::applyFilter);

    m_tree = new QTreeView(this);
    m_tree->setHeaderHidden(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    // Several datasets over the same region is the normal case — six THOR
    // pointings, four WISE bands — and downloading them one double-click at a
    // time was the only way to get them.
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setUniformRowHeights(true);
    vbox->addWidget(m_tree, 1);

    QObject::connect(m_tree, &QAbstractItemView::doubleClicked,
                     this,   &VLKBInventoryTree::onDoubleClicked);

    // Right-click: the two actions that are not the default one — a viewer of
    // its own, and downloading without opening.
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(m_tree, &QTreeView::customContextMenuRequested,
                     this,   &VLKBInventoryTree::showRowContextMenu);

    // "Green = fully covers the region" was information you could only act on
    // by eye, over a list of 151 rows. Now it filters.
    m_coverageOnly = new QCheckBox(u"Only datasets that fully cover the region"_s, this);
    QObject::connect(m_coverageOnly, &QCheckBox::toggled,
                     this, &VLKBInventoryTree::reapplyFilter);
    vbox->addWidget(m_coverageOnly);

    // Legend for the full-sky-overlap highlight.
    auto *legend = new QLabel(
            u"<span style='background:#3ca03c40;'>&nbsp;green&nbsp;</span> "
            u"= dataset fully covers the region"_s, this);
    legend->setStyleSheet(u"color: #8090a0; font-size: 10px;"_s);
    vbox->addWidget(legend);

    // What is happening right now. A cutout is fetched asynchronously — the
    // window does not freeze — but until it arrives nothing on screen changed,
    // so a double-click read as "it did nothing" and people clicked again. This
    // line is the answer, and it is here rather than in a modal dialog because
    // several requests legitimately run at once.
    m_status = new QLabel(this);
    m_status->setStyleSheet(u"color: #d8b24a; font-size: 11px;"_s);
    m_status->setWordWrap(true);
    m_status->setVisible(false);
    vbox->addWidget(m_status);

    // Action row: explicit Open button (double-click still works).
    auto *actions = new QHBoxLayout;
    // Not "Double-click a dataset, or:" — that reads as a sentence someone cut
    // off, and it does not say that the thing to click is the button beside it.
    auto *hint = new QLabel(
            u"Double-click a dataset to open it, or select several and use the buttons. "
            u"Right-click for more."_s, this);
    hint->setWordWrap(true);
    hint->setStyleSheet(u"color: #8090a0; font-size: 10px; font-style: italic;"_s);
    m_openButton = new QPushButton(u"Download && Open"_s, this);
    m_openButton->setEnabled(false);
    m_openButton->setDefault(true);
    // The archive's own batch API: one job for the whole selection instead of
    // one request per dataset. Worth it from a handful upwards, and it survives
    // the app being closed — so it is offered, not imposed.
    m_batchButton = new QPushButton(u"Request as batch…"_s, this);
    m_batchButton->setEnabled(false);
    m_batchButton->setToolTip(
            u"Ask the archive for the whole selection as one asynchronous job. "
            u"Slower to start, but it is a single request and it keeps running if "
            u"you close this window."_s);
    QObject::connect(m_batchButton, &QPushButton::clicked, this,
                     &VLKBInventoryTree::requestBatch);
    actions->addWidget(hint, 1);
    actions->addWidget(m_batchButton, 0);
    actions->addWidget(m_openButton, 0);
    vbox->addLayout(actions);
    QObject::connect(m_openButton, &QPushButton::clicked,
                     this, &VLKBInventoryTree::downloadSelected);
}

// ── Tree setup ────────────────────────────────────────────────────────────────

void VLKBInventoryTree::setupWavelengthGroups()
{
    constexpr double kMax = std::numeric_limits<double>::max();
    m_groups << WavelengthGroup { u"Visible"_s,          0.,     1.e-6  };
    m_groups << WavelengthGroup { u"Near-IR / Mid-IR"_s, 1.e-6,  2.5e-5 };
    m_groups << WavelengthGroup { u"Far-IR"_s,           2.5e-5, 5.e-4  };
    // Legacy 1.7.4: the Submm/Radio split moved 3 mm → 5 mm and the band was relabelled
    // "Submm / mm" so mm-wave surveys group with sub-mm instead of Radio.
    m_groups << WavelengthGroup { u"Submm / mm"_s,       5.e-4,  5.e-3  };
    m_groups << WavelengthGroup { u"Radio"_s,            5.e-3,  kMax   };
}

void VLKBInventoryTree::setupTopLevels()
{
    /*
     * Continuum
     *   ├─ Visible
     *   ├─ Near-IR / Mid-IR
     *   ├─ Far-IR
     *   ├─ Submm / mm
     *   └─ Radio
     * Spectroscopy
     *   └─ (same bands)
     *       └─ Species
     *           └─ Dataset items
     */
    auto *images = new QStandardItem(u"Continuum"_s);
    auto *cubes  = new QStandardItem(u"Spectroscopy"_s);

    for (const auto &g : m_groups) {
        images->appendRow(new QStandardItem(g.name));
        cubes->appendRow(new QStandardItem(g.name));
    }

    m_model = new QStandardItemModel(this);
    // Three columns, because one was not enough to tell two rows apart: the
    // inventory routinely lists six datasets all called "THOR 1.44 GHz", and
    // what distinguishes them — their coverage of the region and their
    // identifier — was only in a tooltip.
    m_model->setHorizontalHeaderLabels({ u"Dataset"_s, u"Coverage"_s, u"Identifier"_s });
    m_model->appendRow(images);
    m_model->appendRow(cubes);
    m_tree->setModel(m_model);
    // Coverage is one short word and Identifier a filename: sized to content
    // they stop squeezing the first column, which is what carries the tree.
    if (auto *hdr = m_tree->header()) {
        hdr->setSectionResizeMode(0, QHeaderView::Stretch);
        hdr->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        hdr->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    }

    // selectionChanged, not currentChanged: with multiple selection a ⌘-click
    // can add or remove a row without moving the current index, and the button
    // then showed a count for a selection that no longer existed.
    QObject::connect(m_tree->selectionModel(), &QItemSelectionModel::selectionChanged,
                     this, &VLKBInventoryTree::onSelectionChanged);
    QObject::connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged,
                     this, &VLKBInventoryTree::onSelectionChanged);
}

// ── VOTable parser ────────────────────────────────────────────────────────────
// Ported from legacy VLKBInventoryTree::parseVOTable(), logic unchanged.

void VLKBInventoryTree::parseVOTable(const QByteArray &votable)
{
    int idx         = 0;
    int idxType     = -1;
    int idxSurvey   = -1;
    int idxSubSurvey= -1;
    int idxPubDID   = -1;
    int idxIvoID    = -1;
    int idxEmMin    = -1;
    int idxEmMax    = -1;
    int idxOverlap  = -1;
    int idxRegion   = -1;

    QXmlStreamReader xml(votable);
    while (!xml.atEnd() && !xml.hasError()) {
        xml.readNext();

        // Collect FIELD column indexes by name
        if (xml.isStartElement() && xml.name() == u"FIELD"_s) {
            const QString name = xml.attributes().value(u"name"_s).toString();
            if      (name == u"dataproduct_type"_s)   idxType      = idx;
            else if (name == u"obs_collection"_s)      idxSurvey    = idx;
            else if (name == u"obs_title"_s)           idxSubSurvey = idx;
            else if (name == u"bib_reference"_s)       idxPubDID    = idx;
            else if (name == u"obs_publisher_did"_s)   idxIvoID     = idx;
            else if (name == u"em_min"_s)              idxEmMin     = idx;
            else if (name == u"em_max"_s)              idxEmMax     = idx;
            else if (name == u"overlapSky"_s)          idxOverlap   = idx;
            else if (name == u"s_region_galactic"_s)   idxRegion    = idx;
            ++idx;
        }

        // Parse each data row
        if (xml.isStartElement() && xml.name() == u"TR"_s) {
            Cutout c;
            c.pos_cutout = m_posCutout;
            int col = 0;

            while (!(xml.isEndElement() && xml.name() == u"TR"_s)) {
                xml.readNext();
                if (xml.isStartElement() && xml.name() == u"TD"_s) {
                    const QString val = xml.readElementText();
                    if      (col == idxType)       c.type        = val;
                    else if (col == idxSurvey)     c.survey      = val;
                    else if (col == idxSubSurvey)  {
                        const auto parts = val.split(u'|');
                        c.species    = parts.first().simplified();
                        c.transition = parts.last().simplified();
                    }
                    else if (col == idxPubDID)     c.pubDID      = val.simplified();
                    else if (col == idxIvoID)      c.ivoID       = val.simplified();
                    else if (col == idxEmMin)      c.spectrum_min = val.toDouble();
                    else if (col == idxEmMax)      c.spectrum_max = val.toDouble();
                    else if (col == idxOverlap)    c.overlap      = val.toInt();
                    else if (col == idxRegion)     c.region_gal  = val;
                    ++col;
                }
            }

            // A dataset with no obs_publisher_did can't be fetched — don't show or
            // count it. Resolve the target parent FIRST — a row that can't be placed
            // (out of the known bands, or an unrecognised dataproduct_type) is neither
            // allocated nor counted (no leak; the header total stays accurate).
            if (c.ivoID.isEmpty())
                continue;
            const int grpIdx = groupIndexOf(c.spectrum_min);
            if (grpIdx < 0)
                continue;

            QStandardItem *parent = nullptr;
            if (c.type == u"image"_s) {
                parent = m_model->item(0)->child(grpIdx);
            } else if (c.type == u"cube"_s) {
                auto *bandItem = m_model->item(1)->child(grpIdx);
                for (int r = 0; r < bandItem->rowCount() && !parent; ++r) {
                    if (bandItem->child(r)->text() == c.species)
                        parent = bandItem->child(r);
                }
                if (!parent) {
                    parent = new QStandardItem(c.species);
                    bandItem->appendRow(parent);
                }
            }
            if (!parent)
                continue;

            const QUuid id = QUuid::createUuid();
            m_cutouts.insert(id, c);

            const QString label = (c.survey + u' ' + c.transition).simplified();
            const QString shown = label.isEmpty() ? c.ivoID.section(u'/', -1) : label;
            auto *row = new QStandardItem(shown);
            auto *coverageCell = new QStandardItem(
                    c.overlap == 3 ? u"full"_s : u"partial"_s);
            coverageCell->setTextAlignment(Qt::AlignCenter);
            // The identifier's tail is what actually differs between two
            // datasets with the same survey and band.
            auto *identifierCell = new QStandardItem(c.ivoID.section(u'/', -1));
            identifierCell->setForeground(QColor(154, 174, 196));
            // Rich tooltip: survey, band/line, type, wavelength, identifier.
            QString tip = u"<b>%1</b>"_s.arg(shown.toHtmlEscaped());
            if (!c.survey.isEmpty())     tip += u"<br>Survey: %1"_s.arg(c.survey.toHtmlEscaped());
            if (!c.transition.isEmpty()) tip += u"<br>Band/line: %1"_s.arg(c.transition.toHtmlEscaped());
            tip += u"<br>Type: %1"_s.arg(c.type == u"cube"_s ? u"spectral cube"_s : u"image"_s);
            if (c.spectrum_min > 0.0)
                tip += u"<br>λ: %1 m"_s.arg(c.spectrum_min, 0, 'g', 3);
            if (c.overlap == 3) tip += u"<br><i>fully covers the region</i>"_s;
            if (!c.pubDID.isEmpty()) tip += u"<br>%1"_s.arg(c.pubDID.toHtmlEscaped());
            row->setToolTip(tip);
            row->setData(id, Qt::UserRole);
            row->setIcon(style()->standardIcon(
                    c.type == u"cube"_s ? QStyle::SP_FileDialogContentsView
                                        : QStyle::SP_FileIcon));
            if (c.overlap == 3) {
                for (QStandardItem *cell : { row, coverageCell, identifierCell })
                    cell->setBackground(QColor(60, 160, 60, 80));
            }
            for (QStandardItem *cell : { coverageCell, identifierCell }) {
                cell->setData(id, Qt::UserRole);
                cell->setToolTip(tip);
            }
            parent->appendRow({ row, coverageCell, identifierCell });
        }
    }

    if (xml.hasError()) {
        qCritical() << Q_FUNC_INFO << xml.errorString();
        QMessageBox::critical(this, u"VLKB Parse Error"_s, xml.errorString());
    }

    spanGroupRows();
}

void VLKBInventoryTree::spanGroupRows()
{
    // Category rows ("Continuum", "Near-IR", a species name) carry nothing in
    // the Coverage and Identifier columns, but they were still confined to the
    // first column — whose width is set by the leaf rows' icons — so every one
    // of them was shown elided: "Continuu...", "Near-I...", "Far-IR ...". They
    // are headings, so let them use the whole width, which is what
    // setFirstColumnSpanned is for. Nothing is widened and no leaf row moves.
    std::function<void(const QModelIndex &)> walk = [&](const QModelIndex &parent) {
        const int rows = m_model->rowCount(parent);
        for (int r = 0; r < rows; ++r) {
            const QModelIndex idx = m_model->index(r, 0, parent);
            if (m_model->rowCount(idx) > 0) {   // has children => a category
                m_tree->setFirstColumnSpanned(r, parent, true);
                walk(idx);
            }
        }
    };
    walk(QModelIndex());
}

// ── Interaction ───────────────────────────────────────────────────────────────

void VLKBInventoryTree::onDoubleClicked(const QModelIndex &idx)
{
    requestCutout(idx);
}

void VLKBInventoryTree::onSelectionChanged()
{
    // Enable Open only when a real dataset (leaf) that is currently VISIBLE is
    // selected — a leaf hidden by the filter must not be actionable.
    int datasets = 0;
    const QModelIndexList selected = m_tree->selectionModel()
            ? m_tree->selectionModel()->selectedRows()
            : QModelIndexList{};
    for (const QModelIndex &idx : selected) {
        if (!idx.isValid() || m_tree->isRowHidden(idx.row(), idx.parent()))
            continue;
        auto *item = m_model->itemFromIndex(idx);
        if (item && m_cutouts.contains(item->data(Qt::UserRole).toUuid()))
            ++datasets;
    }
    if (m_openButton) {
        m_openButton->setEnabled(datasets > 0);
        m_openButton->setText(datasets > 1 ? u"Download && Open (%1)"_s.arg(datasets)
                                           : u"Download && Open"_s);
    }
    if (m_batchButton) {
        // One dataset is not a batch; the direct request is simpler and faster.
        m_batchButton->setEnabled(datasets > 1);
        m_batchButton->setText(datasets > 1 ? u"Request as batch (%1)…"_s.arg(datasets)
                                            : u"Request as batch…"_s);
    }
    showFootprintFor(selected.size() == 1 ? selected.first() : QModelIndex{});
}

// Outline the clicked dataset's sky coverage on the linked image, so its
// position is visible before anything is downloaded. Only for a single
// selection: several outlines at once say less than one.
void VLKBInventoryTree::showFootprintFor(const QModelIndex &idx)
{
    auto *win = linkedImageWindow();
    if (!win)
        return;                 // nothing to draw on yet
    if (!idx.isValid()) {
        win->clearInventoryFootprint();
        return;
    }
    auto *item = m_model->itemFromIndex(idx);
    const QUuid id = item ? item->data(Qt::UserRole).toUuid() : QUuid{};
    if (!m_cutouts.contains(id)) {
        win->clearInventoryFootprint();   // a group row
        return;
    }

    // s_region_galactic is "<shape> <frame> l1 b1 l2 b2 …": the first two tokens
    // name the shape and the coordinate system, the rest are the corners.
    const QStringList tok = m_cutouts.value(id).region_gal.split(u' ', Qt::SkipEmptyParts);
    QVector<QPointF> corners;
    for (int i = 2; i + 1 < tok.size(); i += 2) {
        bool okL = false, okB = false;
        const double l = tok.at(i).toDouble(&okL);
        const double b = tok.at(i + 1).toDouble(&okB);
        if (!okL || !okB) {
            win->clearInventoryFootprint();
            return;
        }
        corners.append(QPointF(l, b));
    }
    if (corners.size() < 3) {
        win->clearInventoryFootprint();
        return;
    }
    win->showInventoryFootprint(corners);
}

void VLKBInventoryTree::downloadSelected()
{
    // Every selected dataset, in the order the tree shows them. Asking for six
    // pointings used to mean six double-clicks and six trips through the
    // dialog.
    // Always the selection, never the current index: a row can be current
    // without being selected, and downloading that one instead is not what the
    // highlighted rows promised.
    const QModelIndexList selected = m_tree->selectionModel()
            ? m_tree->selectionModel()->selectedRows()
            : QModelIndexList{};
    for (const QModelIndex &idx : selected) {
        if (idx.isValid() && !m_tree->isRowHidden(idx.row(), idx.parent()))
            requestCutout(idx);
    }
}

void VLKBInventoryTree::requestBatch()
{
    QStringList ids;
    QStringList names;
    const QModelIndexList selected = m_tree->selectionModel()
            ? m_tree->selectionModel()->selectedRows()
            : QModelIndexList{};
    for (const QModelIndex &idx : selected) {
        if (!idx.isValid() || m_tree->isRowHidden(idx.row(), idx.parent()))
            continue;
        auto *item = m_model->itemFromIndex(idx);
        if (!item)
            continue;
        const QUuid id = item->data(Qt::UserRole).toUuid();
        if (!m_cutouts.contains(id))
            continue;
        const Cutout &c = m_cutouts.value(id);
        ids.append(c.ivoID);
        names.append((c.survey + u' ' + c.transition).simplified());
    }
    if (ids.size() < 2)
        return;
    emit batchRequested(ids, m_posCutout, names);
}

VLKBInventoryTree::RowRequest VLKBInventoryTree::rowRequest(const QModelIndex &idx) const
{
    RowRequest r;
    if (!idx.isValid())
        return r;
    const QUuid id = m_model->itemFromIndex(idx)->data(Qt::UserRole).toUuid();
    if (!m_cutouts.contains(id))
        return r;            // branch node — no dataset behind it
    const Cutout &c = m_cutouts.value(id);
    if (c.ivoID.isEmpty())
        return r;

    // A human-readable label: "Survey Transition" (e.g. "MAGPIS 20cm"), falling
    // back to the ivoID tail when both fields are empty.
    QString displayName;
    if (!c.survey.isEmpty() || !c.transition.isEmpty()) {
        displayName = c.survey;
        if (!c.survey.isEmpty() && !c.transition.isEmpty())
            displayName += u' ';
        displayName += c.transition;
    } else {
        displayName = c.ivoID.section(u'/', -1);
    }
    r.ivoID = c.ivoID;
    r.pos = m_posCutout;
    r.displayName = displayName.simplified();
    r.type = c.type;
    return r;
}

void VLKBInventoryTree::requestCutout(const QModelIndex &idx)
{
    const RowRequest r = rowRequest(idx);
    if (r.ivoID.isEmpty())
        return;
    noteCutoutStarted(r.displayName);
    emit cutoutRequested(r.ivoID, r.pos, r.displayName, r.type);
}

void VLKBInventoryTree::showRowContextMenu(const QPoint &pos)
{
    const QModelIndex idx = m_tree->indexAt(pos);
    const RowRequest r = rowRequest(idx);
    if (r.ivoID.isEmpty())
        return;              // a group row has nothing to act on

    QMenu menu(this);
    // Named after what they do, and ordered with the default first so the menu
    // reads as "the usual thing, or these two".
    QAction *asLayer = menu.addAction(
            r.type == u"image"_s ? u"Open as a Layer"_s : u"Open"_s);
    asLayer->setToolTip(u"Add to the shared viewer for this inventory — its datasets "
                        u"all cover the same sky."_s);
    QAction *ownWindow = menu.addAction(u"Open in a New Window"_s);
    ownWindow->setEnabled(r.type == u"image"_s);
    ownWindow->setToolTip(r.type == u"image"_s
            ? u"A viewer of its own, for comparing two datasets side by side."_s
            : u"Cubes always open in their own viewer."_s);
    menu.addSeparator();
    QAction *saveOnly = menu.addAction(u"Save File…"_s);
    saveOnly->setToolTip(u"Download the cutout and keep the file, without opening it."_s);

    QAction *chosen = menu.exec(m_tree->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;
    if (chosen == asLayer) {
        noteCutoutStarted(r.displayName);
        emit cutoutRequested(r.ivoID, r.pos, r.displayName, r.type);
    } else if (chosen == ownWindow) {
        noteCutoutStarted(r.displayName);
        emit openInNewWindowRequested(r.ivoID, r.pos, r.displayName, r.type);
    } else if (chosen == saveOnly) {
        noteCutoutStarted(r.displayName);
        emit saveFileRequested(r.ivoID, r.pos, r.displayName, r.type);
    }
}

void VLKBInventoryTree::noteCutoutStarted(const QString &displayName)
{
    ++m_pending;
    m_lastRequested = displayName;
    updateStatus();
}

void VLKBInventoryTree::noteCutoutFinished(const QString &displayName, bool ok)
{
    if (m_pending > 0)
        --m_pending;
    if (!ok)
        m_lastFailed = displayName;
    updateStatus();
}

void VLKBInventoryTree::updateStatus()
{
    if (!m_status)
        return;
    if (m_pending > 0) {
        m_status->setText(m_pending == 1
                ? u"Requesting %1 from the archive — the viewer opens when it arrives…"_s
                          .arg(m_lastRequested)
                : u"Requesting %1 datasets from the archive…"_s.arg(m_pending));
        m_status->setVisible(true);
        // The cursor says the same thing for the hand that is still on the
        // mouse, which is where the attention is right after a double-click.
        if (!m_busyCursorSet) {
            m_busyCursorSet = true;
            QApplication::setOverrideCursor(Qt::BusyCursor);
        }
    } else {
        m_status->setVisible(false);
        if (m_busyCursorSet) {
            m_busyCursorSet = false;
            QApplication::restoreOverrideCursor();
        }
    }
    // Requests keep running if the window closes, but the buttons should not
    // invite a second identical one while the first is out.
    if (m_openButton && m_pending > 0)
        m_openButton->setEnabled(false);
    else
        onSelectionChanged();
}

// ── Style helpers ─────────────────────────────────────────────────────────────

void VLKBInventoryTree::applyStyleHints(const QModelIndex &parent)
{
    m_tree->expand(parent);
    for (int i = 0; i < m_groups.size(); ++i) {
        const QModelIndex idx = m_model->index(i, 0, parent);
        auto *item = m_model->itemFromIndex(idx);
        if (!item) continue;
        if (m_model->hasChildren(idx)) {
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
        } else {
            item->setForeground(QColor(Qt::gray));
        }
    }
}

// ── Counts + filtering ────────────────────────────────────────────────────────

int VLKBInventoryTree::countAndTag(QStandardItem *node)
{
    if (!node)
        return 0;
    // Leaf (a real dataset) → count 1, don't retag.
    if (m_cutouts.contains(node->data(Qt::UserRole).toUuid()))
        return 1;
    int total = 0;
    for (int r = 0; r < node->rowCount(); ++r)
        total += countAndTag(node->child(r));
    // Tag from the stored base label (idempotent; never strips real parentheses).
    QVariant baseV = node->data(kBaseNameRole);
    if (!baseV.isValid()) {
        baseV = node->text();
        node->setData(baseV, kBaseNameRole);
    }
    const QString base = baseV.toString();
    node->setText(total > 0 ? u"%1 (%2)"_s.arg(base).arg(total) : base);
    return total;
}

void VLKBInventoryTree::annotateCounts()
{
    for (int r = 0; r < m_model->rowCount(); ++r)
        countAndTag(m_model->item(r));
}

bool VLKBInventoryTree::filterNode(QStandardItem *node, const QString &needle)
{
    if (!node)
        return false;
    const QModelIndex idx = node->index();
    // Leaf: visible iff its label matches (empty needle = everything).
    if (m_cutouts.contains(node->data(Qt::UserRole).toUuid())) {
        const bool match = needle.isEmpty()
                || node->text().contains(needle, Qt::CaseInsensitive);
        m_tree->setRowHidden(idx.row(), idx.parent(), !match);
        return match;
    }
    // Branch: if the band/species name itself matches, reveal its whole subtree
    // (so "Radio" shows every radio dataset). Otherwise, visible iff a descendant is.
    const QString branchBase = node->data(kBaseNameRole).toString();
    if (!branchBase.isEmpty() && branchBase.contains(needle, Qt::CaseInsensitive)) {
        std::function<void(QStandardItem *)> unhide = [&](QStandardItem *n) {
            for (int r = 0; r < n->rowCount(); ++r) {
                m_tree->setRowHidden(r, n->index(), false);
                unhide(n->child(r));
            }
        };
        unhide(node);
        m_tree->setRowHidden(idx.row(), idx.parent(), false);
        m_tree->expand(idx);
        return true;
    }
    bool anyVisible = false;
    for (int r = 0; r < node->rowCount(); ++r)
        anyVisible |= filterNode(node->child(r), needle);
    m_tree->setRowHidden(idx.row(), idx.parent(), !anyVisible);
    if (anyVisible)
        m_tree->expand(idx);
    return anyVisible;
}

void VLKBInventoryTree::reapplyFilter()
{
    applyFilter(m_filter ? m_filter->text() : QString());
}

void VLKBInventoryTree::applyFilter(const QString &text)
{
    const QString needle = text.trimmed();
    if (needle.isEmpty()) {
        // Restore the initial tree: unhide every node (incl. empty bands) + expand.
        std::function<void(QStandardItem *)> unhide = [&](QStandardItem *n) {
            for (int r = 0; r < n->rowCount(); ++r) {
                m_tree->setRowHidden(r, n->index(), false);
                unhide(n->child(r));
            }
        };
        for (int r = 0; r < m_model->rowCount(); ++r) {
            m_tree->setRowHidden(r, QModelIndex(), false);
            unhide(m_model->item(r));
        }
        m_tree->expandAll();
    } else {
        for (int r = 0; r < m_model->rowCount(); ++r)
            filterNode(m_model->item(r), needle);
    }
    // …then the coverage filter on top of the text one: a dataset that only
    // clips the corner of the region is rarely what anyone wanted.
    if (m_coverageOnly && m_coverageOnly->isChecked())
        hidePartialCoverage();

    // A leaf hidden by the filter must not remain the actionable current item.
    const QModelIndex cur = m_tree->currentIndex();
    if (cur.isValid() && m_tree->isRowHidden(cur.row(), cur.parent()))
        m_tree->selectionModel()->clear();
    onSelectionChanged();
}

void VLKBInventoryTree::hidePartialCoverage()
{
    // Hide every leaf whose dataset does not fully cover the region, then hide
    // the branches that are left with nothing visible under them.
    std::function<bool(QStandardItem *)> prune = [&](QStandardItem *node) {
        bool anyVisible = false;
        for (int r = 0; r < node->rowCount(); ++r) {
            QStandardItem *child = node->child(r);
            if (m_tree->isRowHidden(r, node->index()))
                continue;
            const QUuid id = child->data(Qt::UserRole).toUuid();
            bool visible = false;
            if (m_cutouts.contains(id)) {
                visible = m_cutouts.value(id).overlap == 3;
            } else {
                visible = prune(child);
            }
            m_tree->setRowHidden(r, node->index(), !visible);
            anyVisible = anyVisible || visible;
        }
        return anyVisible;
    };
    for (int r = 0; r < m_model->rowCount(); ++r) {
        if (m_tree->isRowHidden(r, QModelIndex()))
            continue;
        m_tree->setRowHidden(r, QModelIndex(), !prune(m_model->item(r)));
    }
}

int VLKBInventoryTree::groupIndexOf(double lambda) const
{
    for (int i = 0; i < m_groups.size(); ++i) {
        const auto &g = m_groups.at(i);
        if (lambda > g.lambdaMin && lambda <= g.lambdaMax)
            return i;
    }
    return -1;
}
