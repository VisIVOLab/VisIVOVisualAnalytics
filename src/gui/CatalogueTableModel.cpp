#include "CatalogueTableModel.h"

#include <QHash>
#include <QString>

#include <limits>

namespace {
QString catalogueShapeName(CatalogueOverlayEntry::Shape shape)
{
    switch (shape) {
    case CatalogueOverlayEntry::Shape::Ellipse:  return QStringLiteral("Ellipse");
    case CatalogueOverlayEntry::Shape::Polyline: return QStringLiteral("Polyline");
    default:                                     return QStringLiteral("Point");
    }
}
}

namespace {
// Top-level rows carry this as their internal id; a child carries its group.
constexpr quintptr kTopLevel = std::numeric_limits<quintptr>::max();
}   // namespace

CatalogueTableModel::CatalogueTableModel(QObject *parent) : QAbstractItemModel(parent) {}

void CatalogueTableModel::rebuildGroups()
{
    m_groups.clear();
    m_grouped = false;
    if (!entries)
        return;
    // Grouped only if the catalogue says so. A local file has no such relation
    // and must keep behaving like a flat table.
    QHash<QString, int> groupOfKey;
    for (std::size_t i = 0; i < entries->size(); ++i) {
        const CatalogueOverlayEntry &e = entries->at(i);
        const int idx = static_cast<int>(i);
        if (e.groupKey.isEmpty()) {
            Group g;
            g.representative = idx;
            m_groups.append(g);
            continue;
        }
        m_grouped = true;
        auto it = groupOfKey.constFind(e.groupKey);
        if (it == groupOfKey.cend()) {
            Group g;
            g.representative = idx;
            m_groups.append(g);
            groupOfKey.insert(e.groupKey, m_groups.size() - 1);
            continue;
        }
        Group &g = m_groups[it.value()];
        // The band-merged detection is the one that represents the source; any
        // other band goes underneath it. Without one, the first seen leads.
        const bool thisIsMerged = e.band == QStringLiteral("ft");
        if (thisIsMerged && entries->at(static_cast<std::size_t>(g.representative)).band
                    != QStringLiteral("ft")) {
            g.children.append(g.representative);
            g.representative = idx;
        } else {
            g.children.append(idx);
        }
    }
}

int CatalogueTableModel::entryIndexFor(const QModelIndex &index) const
{
    if (!index.isValid() || !entries)
        return -1;
    if (index.internalId() == kTopLevel) {
        return index.row() >= 0 && index.row() < m_groups.size()
                ? m_groups.at(index.row()).representative
                : -1;
    }
    const auto g = static_cast<int>(index.internalId());
    if (g < 0 || g >= m_groups.size())
        return -1;
    const QVector<int> &kids = m_groups.at(g).children;
    return index.row() >= 0 && index.row() < kids.size() ? kids.at(index.row()) : -1;
}

QModelIndex CatalogueTableModel::indexForEntry(int entryIndex) const
{
    for (int g = 0; g < m_groups.size(); ++g) {
        if (m_groups.at(g).representative == entryIndex)
            return createIndex(g, 0, kTopLevel);
        const int child = m_groups.at(g).children.indexOf(entryIndex);
        if (child >= 0)
            return createIndex(child, 0, static_cast<quintptr>(g));
    }
    return {};
}

QModelIndex CatalogueTableModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent))
        return {};
    if (!parent.isValid())
        return createIndex(row, column, kTopLevel);
    if (parent.internalId() != kTopLevel)
        return {};   // only two levels
    return createIndex(row, column, static_cast<quintptr>(parent.row()));
}

QModelIndex CatalogueTableModel::parent(const QModelIndex &child) const
{
    if (!child.isValid() || child.internalId() == kTopLevel)
        return {};
    return createIndex(static_cast<int>(child.internalId()), 0, kTopLevel);
}

void CatalogueTableModel::setFrameLabel(const QString &frameLabel)
{
    if (m_frameLabel == frameLabel)
        return;
    m_frameLabel = frameLabel;
    emit headerDataChanged(Qt::Horizontal, static_cast<int>(Column::RA),
                           static_cast<int>(Column::Dec));
}

void CatalogueTableModel::setEntries(const std::vector<CatalogueOverlayEntry> *newEntries)
{
    beginResetModel();
    this->entries = newEntries;
    rebuildGroups();
    endResetModel();
}

int CatalogueTableModel::rowCount(const QModelIndex &parent) const
{
    if (!this->entries)
        return 0;
    if (!parent.isValid())
        return m_groups.size();
    if (parent.internalId() != kTopLevel || parent.column() > 0)
        return 0;
    return parent.row() >= 0 && parent.row() < m_groups.size()
            ? m_groups.at(parent.row()).children.size()
            : 0;
}

int CatalogueTableModel::columnCount(const QModelIndex &) const
{
    // The SAME columns at every level. Returning 0 for a valid parent made
    // hasIndex() refuse every child index, so a band under its band-merged
    // source could not be built by the view at all - while indexForEntry()
    // created one directly, leaving navigation and selection disagreeing.
    return kFixedColumns + static_cast<int>(m_extraFields.size());
}

void CatalogueTableModel::setExtraFields(const QStringList &fields)
{
    if (m_extraFields == fields)
        return;
    beginResetModel();
    m_extraFields = fields;
    endResetModel();
}

QVariant CatalogueTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || !this->entries) {
        return {};
    }

    const int entryIndex = entryIndexFor(index);
    if (entryIndex < 0 || entryIndex >= static_cast<int>(this->entries->size()))
        return {};
    const CatalogueOverlayEntry &entry = this->entries->at(static_cast<std::size_t>(entryIndex));
    if (index.column() >= kFixedColumns) {
        // A field of the catalogue itself: looked up by name, because not every
        // row carries every field.
        if (role != Qt::DisplayRole && role != Qt::TextAlignmentRole)
            return {};
        const QString key = m_extraFields.value(index.column() - kFixedColumns);
        for (const auto &kv : entry.extra) {
            if (kv.first == key) {
                if (role == Qt::TextAlignmentRole) {
                    bool numeric = false;
                    kv.second.toDouble(&numeric);
                    return static_cast<int>((numeric ? Qt::AlignRight : Qt::AlignLeft)
                                            | Qt::AlignVCenter);
                }
                return kv.second;
            }
        }
        return {};
    }
    const auto column = static_cast<Column>(index.column());

    if (role == Qt::TextAlignmentRole) {
        switch (column) {
        case Column::Name:
        case Column::Type:
            return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
        default:
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        }
    }

    if (role != Qt::DisplayRole) {
        return {};
    }

    switch (column) {
    case Column::Name: {
        const QString name = entry.label.trimmed().isEmpty()
                ? QStringLiteral("Source %1").arg(entryIndex + 1)
                : entry.label.trimmed();
        // A parent says how many more bands are folded under it, so a collapsed
        // row does not hide that there is anything to open.
        if (index.internalId() == kTopLevel && index.row() < m_groups.size()) {
            const int extra = m_groups.at(index.row()).children.size();
            if (extra > 0)
                return QStringLiteral("%1  + %2 band%3").arg(name).arg(extra)
                        .arg(extra == 1 ? QString() : QStringLiteral("s"));
        }
        return name;
    }
    case Column::Type:
        return catalogueShapeName(entry.shape);
    // A sky position, when the entry has one - which is not the same question as
    // "is the SHAPE in sky coordinates". A VLKB ellipse is drawn in pixels and
    // still knows its Galactic centre; the two columns used to be empty for
    // every such source.
    case Column::RA:
        return entry.frame == CatalogueOverlayEntry::Frame::Sky || entry.hasSkyPosition
                ? QString::number(entry.raDeg, 'f', 5) : QVariant{};
    case Column::Dec:
        return entry.frame == CatalogueOverlayEntry::Frame::Sky || entry.hasSkyPosition
                ? QString::number(entry.decDeg, 'f', 5) : QVariant{};
    case Column::X:
        return QString::number(entry.pixelX, 'f', 2);
    case Column::Y:
        return QString::number(entry.pixelY, 'f', 2);
    // Whenever the source HAS a size, not only when its shape is drawn as an
    // ellipse: a VLKB detection is sampled into a polyline and still has FWHM
    // and PA, and both columns were empty for every one of them.
    case Column::Size:
        if (entry.radiusX > 0.0 || entry.radiusY > 0.0) {
            return QStringLiteral("%1, %2")
                    .arg(QString::number(entry.radiusX, 'f', 2),
                         QString::number(entry.radiusY, 'f', 2));
        }
        return {};
    case Column::Angle:
        if (entry.radiusX > 0.0 || entry.radiusY > 0.0) {
            return QString::number(entry.angleDeg, 'f', 1);
        }
        return {};
    }

    return {};
}

QVariant CatalogueTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QAbstractItemModel::headerData(section, orientation, role);
    }
    if (section >= kFixedColumns) {
        return m_extraFields.value(section - kFixedColumns);
    }

    switch (static_cast<Column>(section)) {
    case Column::Name:
        return QStringLiteral("Name");
    case Column::Type:
        return QStringLiteral("Type");
    case Column::RA:
        // Naming the columns after the frame the numbers are in: a Galactic
        // catalogue under an "RA (deg)" header is simply mislabelled data.
        return m_frameLabel.compare(QStringLiteral("Galactic"), Qt::CaseInsensitive) == 0
                ? QStringLiteral("GLON (deg)") : QStringLiteral("RA (deg)");
    case Column::Dec:
        return m_frameLabel.compare(QStringLiteral("Galactic"), Qt::CaseInsensitive) == 0
                ? QStringLiteral("GLAT (deg)") : QStringLiteral("Dec (deg)");
    case Column::X:
        return QStringLiteral("X");
    case Column::Y:
        return QStringLiteral("Y");
    case Column::Size:
        return QStringLiteral("Size");
    case Column::Angle:
        return QStringLiteral("Angle");
    }

    return {};
}
