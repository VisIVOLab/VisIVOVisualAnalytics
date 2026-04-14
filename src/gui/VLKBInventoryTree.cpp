#include "VLKBInventoryTree.h"
#include "vtkWindowImage.h"

#include <limits>

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QModelIndex>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTreeView>
#include <QUuid>
#include <QVBoxLayout>
#include <QXmlStreamReader>

using namespace Qt::StringLiterals;

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

    auto *model = qobject_cast<QStandardItemModel *>(m_tree->model());
    applyStyleHints(model->index(0, 0));
    applyStyleHints(model->index(1, 0));
}

// ── UI builder ────────────────────────────────────────────────────────────────

void VLKBInventoryTree::buildUi(const QString &posCutout)
{
    setWindowTitle(u"VLKB Inventory"_s);
    resize(480, 560);

    auto *vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(10, 8, 10, 8);
    vbox->setSpacing(6);

    // Position summary label
    auto *posLbl = new QLabel(u"Region: "_s + posCutout, this);
    posLbl->setWordWrap(true);
    posLbl->setStyleSheet(u"color: #607080; font-size: 10px;"_s);
    vbox->addWidget(posLbl);

    // Hint
    auto *hint = new QLabel(
            u"Double-click a dataset to download and open it."_s, this);
    hint->setStyleSheet(u"color: #8090a0; font-size: 10px; font-style: italic;"_s);
    vbox->addWidget(hint);

    m_tree = new QTreeView(this);
    m_tree->setHeaderHidden(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    vbox->addWidget(m_tree, 1);

    QObject::connect(m_tree, &QAbstractItemView::doubleClicked,
                     this,   &VLKBInventoryTree::onDoubleClicked);
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

    auto *model = new QStandardItemModel(this);
    model->appendRow(images);
    model->appendRow(cubes);
    m_tree->setModel(model);
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

            // Insert into hash and tree
            const QUuid id = QUuid::createUuid();
            m_cutouts.insert(id, c);

            const int grpIdx = groupIndexOf(c.spectrum_min);
            if (grpIdx < 0)
                continue;

            const QString label = c.survey + u' ' + c.transition;
            auto *row = new QStandardItem(label);
            row->setToolTip(c.pubDID);
            row->setData(id, Qt::UserRole);
            if (c.overlap == 3)
                row->setBackground(QColor(60, 160, 60, 80));

            auto *model = qobject_cast<QStandardItemModel *>(m_tree->model());

            if (c.type == u"image"_s) {
                model->item(0)->child(grpIdx)->appendRow(row);
            } else if (c.type == u"cube"_s) {
                auto *bandItem = model->item(1)->child(grpIdx);
                QStandardItem *speciesItem = nullptr;
                for (int r = 0; r < bandItem->rowCount() && !speciesItem; ++r) {
                    if (bandItem->child(r)->text() == c.species)
                        speciesItem = bandItem->child(r);
                }
                if (!speciesItem) {
                    speciesItem = new QStandardItem(c.species);
                    bandItem->appendRow(speciesItem);
                }
                speciesItem->appendRow(row);
            }
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
    if (!idx.isValid())
        return;

    auto *model = qobject_cast<QStandardItemModel *>(m_tree->model());
    const QUuid id = model->itemFromIndex(idx)->data(Qt::UserRole).toUuid();

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
    auto *model = qobject_cast<QStandardItemModel *>(m_tree->model());
    for (int i = 0; i < m_groups.size(); ++i) {
        const QModelIndex idx = model->index(i, 0, parent);
        auto *item = model->itemFromIndex(idx);
        if (!item) continue;
        if (model->hasChildren(idx)) {
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
        } else {
            item->setForeground(QColor(Qt::gray));
        }
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
