#include "VLKBInventoryTree.h"
#include "vtkWindowImage.h"

#include <functional>
#include <limits>

#include <QFont>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QModelIndex>
#include <QPushButton>
#include <QStandardItem>
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
    resize(520, 620);

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
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setUniformRowHeights(true);
    vbox->addWidget(m_tree, 1);

    QObject::connect(m_tree, &QAbstractItemView::doubleClicked,
                     this,   &VLKBInventoryTree::onDoubleClicked);

    // Legend for the full-sky-overlap highlight.
    auto *legend = new QLabel(
            u"<span style='background:#3ca03c40;'>&nbsp;green&nbsp;</span> "
            u"= dataset fully covers the region"_s, this);
    legend->setStyleSheet(u"color: #8090a0; font-size: 10px;"_s);
    vbox->addWidget(legend);

    // Action row: explicit Open button (double-click still works).
    auto *actions = new QHBoxLayout;
    auto *hint = new QLabel(u"Double-click a dataset, or:"_s, this);
    hint->setStyleSheet(u"color: #8090a0; font-size: 10px; font-style: italic;"_s);
    m_openButton = new QPushButton(u"Download && Open"_s, this);
    m_openButton->setEnabled(false);
    m_openButton->setDefault(true);
    actions->addWidget(hint, 1);
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
    m_groups << WavelengthGroup { u"Submm"_s,            5.e-4,  3.e-3  };
    m_groups << WavelengthGroup { u"Radio"_s,            3.e-3,  kMax   };
}

void VLKBInventoryTree::setupTopLevels()
{
    /*
     * Continuum
     *   ├─ Visible
     *   ├─ Near-IR / Mid-IR
     *   ├─ Far-IR
     *   ├─ Submm
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
    m_model->appendRow(images);
    m_model->appendRow(cubes);
    m_tree->setModel(m_model);

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
            if (c.overlap == 3)
                row->setBackground(QColor(60, 160, 60, 80));
            parent->appendRow(row);
        }
    }

    if (xml.hasError()) {
        qCritical() << Q_FUNC_INFO << xml.errorString();
        QMessageBox::critical(this, u"VLKB Parse Error"_s, xml.errorString());
    }
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
    const QModelIndex idx = m_tree->currentIndex();
    bool ok = false;
    if (idx.isValid() && !m_tree->isRowHidden(idx.row(), idx.parent())) {
        auto *item = m_model->itemFromIndex(idx);
        ok = item && m_cutouts.contains(item->data(Qt::UserRole).toUuid());
    }
    if (m_openButton) m_openButton->setEnabled(ok);
}

void VLKBInventoryTree::downloadSelected()
{
    requestCutout(m_tree->currentIndex());
}

void VLKBInventoryTree::requestCutout(const QModelIndex &idx)
{
    if (!idx.isValid())
        return;

    const QUuid id = m_model->itemFromIndex(idx)->data(Qt::UserRole).toUuid();

    if (!m_cutouts.contains(id))
        return;  // branch node — ignore

    const Cutout &c = m_cutouts.value(id);
    if (c.ivoID.isEmpty())
        return;

    // Build a human-readable label: "Survey Transition" (e.g. "MAGPIS 20cm").
    // Fall back to the ivoID tail if both fields are empty.
    QString displayName;
    if (!c.survey.isEmpty() || !c.transition.isEmpty()) {
        displayName = c.survey;
        if (!c.survey.isEmpty() && !c.transition.isEmpty())
            displayName += u' ';
        displayName += c.transition;
    } else {
        displayName = c.ivoID.section(u'/', -1);
    }

    emit cutoutRequested(c.ivoID, m_posCutout, displayName.simplified(), c.type);
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
    // A leaf hidden by the filter must not remain the actionable current item.
    const QModelIndex cur = m_tree->currentIndex();
    if (cur.isValid() && m_tree->isRowHidden(cur.row(), cur.parent()))
        m_tree->selectionModel()->clear();
    onSelectionChanged();
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
