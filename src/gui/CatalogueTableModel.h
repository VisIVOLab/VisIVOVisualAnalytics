#ifndef CatalogueTableModel_h
#define CatalogueTableModel_h

#include "CatalogueOverlayUtils.h"

#include <QAbstractItemModel>
#include <QVector>
#include <QString>
#include <QStringList>

#include <vector>

class CatalogueTableModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    explicit CatalogueTableModel(QObject *parent = nullptr);

    void setEntries(const std::vector<CatalogueOverlayEntry> *entries);
    /// The sky frame the entries' positions are in ("Galactic", "FK5", …). The
    /// two sky columns are captioned from it.
    void setFrameLabel(const QString &frameLabel);
    /// The catalogue's own fields, in file order: one column each, after the
    /// ones every catalogue has. A catalogue is not eight numbers, and the
    /// fluxes and flags beside them are usually why it was opened.
    void setExtraFields(const QStringList &fields);
    /// True when the catalogue actually has parent/child relations. A plain
    /// catalogue has none, and the view must then show no expander column.
    bool isGrouped() const { return m_grouped; }

    /// Entry index behind a model index, or -1. The view speaks model indices,
    /// the overlay speaks entry indices; these two translate between them.
    int entryIndexFor(const QModelIndex &index) const;
    QModelIndex indexForEntry(int entryIndex) const;

    QModelIndex index(int row, int column,
                      const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    static constexpr int kFixedColumns = 8;   ///< the columns every catalogue has

    enum class Column
    {
        Name = 0,
        Type,
        RA,
        Dec,
        X,
        Y,
        Size,
        Angle,
    };

    /// One physical source: the row shown at top level, and the other bands of
    /// the same source under it. Built only when the catalogue says which
    /// entries belong together (CatalogueOverlayEntry::groupKey); otherwise the
    /// model stays flat and behaves exactly as the table always did.
    struct Group
    {
        int representative{ -1 };
        QVector<int> children;
    };
    void rebuildGroups();

    const std::vector<CatalogueOverlayEntry> *entries{ nullptr };
    QVector<Group> m_groups;
    bool m_grouped{ false };
    QString m_frameLabel;
    QStringList m_extraFields;
};

#endif
