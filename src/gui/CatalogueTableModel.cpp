#include "CatalogueTableModel.h"

#include <QString>

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

CatalogueTableModel::CatalogueTableModel(QObject *parent) : QAbstractTableModel(parent) {}

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
    endResetModel();
}

int CatalogueTableModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() || !this->entries) {
        return 0;
    }
    return static_cast<int>(this->entries->size());
}

int CatalogueTableModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
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
    if (!index.isValid() || !this->entries || index.row() < 0
        || index.row() >= static_cast<int>(this->entries->size())) {
        return {};
    }

    const CatalogueOverlayEntry &entry = this->entries->at(static_cast<std::size_t>(index.row()));
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
    case Column::Name:
        return entry.label.trimmed().isEmpty() ? QStringLiteral("Source %1").arg(index.row() + 1)
                                               : entry.label.trimmed();
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
    case Column::Size:
        if (entry.shape == CatalogueOverlayEntry::Shape::Ellipse) {
            return QStringLiteral("%1, %2")
                    .arg(QString::number(entry.radiusX, 'f', 2),
                         QString::number(entry.radiusY, 'f', 2));
        }
        return {};
    case Column::Angle:
        if (entry.shape == CatalogueOverlayEntry::Shape::Ellipse) {
            return QString::number(entry.angleDeg, 'f', 1);
        }
        return {};
    }

    return {};
}

QVariant CatalogueTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QAbstractTableModel::headerData(section, orientation, role);
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
