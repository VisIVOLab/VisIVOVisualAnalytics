#ifndef CatalogueTableModel_h
#define CatalogueTableModel_h

#include "CatalogueOverlayUtils.h"

#include <QAbstractTableModel>
#include <QString>
#include <QStringList>

#include <vector>

class CatalogueTableModel : public QAbstractTableModel
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

    const std::vector<CatalogueOverlayEntry> *entries{ nullptr };
    QString m_frameLabel;
    QStringList m_extraFields;
};

#endif
