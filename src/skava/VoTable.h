#ifndef VoTable_h
#define VoTable_h

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

/// A minimal reader for the VOTable documents VO services return: the first
/// results TABLE serialised as TABLEDATA (FIELD names and row cells), its
/// PARAMs, the INFO QUERY_STATUS, and the ``adhoc:service`` RESOURCEs a
/// DataLink document uses to describe a service such as a SODA cutout.
///
/// BINARY / BINARY2 serialisations are reported, not decoded: VisIVO asks
/// services for TABLEDATA (``RESPONSEFORMAT=votable/td``), which every TAP
/// and DataLink implementation offers.
struct VoTableService
{
    QString id;           ///< RESOURCE ID, what a row's service_def refers to
    QString standardId;   ///< e.g. ivo://ivoa.net/std/SODA#sync-1.0
    QString accessUrl;
    QStringList inputParams;
    /// The inputs the service fixes for this dataset (a PARAM with a value in
    /// the inputParams GROUP): the dataset's ID on the node, an OPERATION.
    QHash<QString, QString> inputValues;
};

struct VoTable
{
    bool        valid{ false };
    QString     error;              ///< parse error, or the service's QUERY_STATUS=ERROR text
    bool        binary{ false };    ///< the table was not TABLEDATA
    QStringList fields;
    QVector<QStringList> rows;
    QHash<QString, QString> params; ///< TABLE-level PARAM name → value
    QVector<VoTableService> services;

    /// Cell @p name of row @p row; empty when the column is absent.
    QString cell(int row, const QString &name) const;
    const VoTableService *service(const QString &id) const;

    static VoTable parse(const QByteArray &xml);
};

#endif  // VoTable_h
