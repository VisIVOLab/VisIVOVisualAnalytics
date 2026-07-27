#ifndef PRODUCTREGISTRY_H
#define PRODUCTREGISTRY_H

// ProductRegistry — the one new model behind Design 1d (tappa 2). Every analysis
// tool that derives something from a dataset (moment map, PV diagram, noise
// estimate, line-width map, baseline cube, stack, channel maps, …) registers the
// result here, tagged with the *stable* parent dataset key (the FITS filepath,
// matching AnalysisParamStore / ThumbnailCache) plus the volatile backend ids and
// the parameter recipe. The session-data tree observes this registry to show each
// product nested under the dataset it came from instead of as an orphan window.
//
// In-memory and session-scoped (products live for the app run); nothing is
// persisted here — parameter recipes are already persisted by AnalysisParamStore.

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <QList>

class QWidget;

struct AnalysisProduct
{
    QString productId;       ///< unique; assigned by the registry when empty
    QString parentKey;       ///< stable dataset key (FITS filepath) it derives from
    QString parentLabel;     ///< display name of the parent dataset
    QString kind;            ///< "moment" | "pv" | "noise" | "linewidth" | …
    QString label;           ///< e.g. "Moment 0 — integrated"
    QString status{ QStringLiteral("ready") };  ///< "ready" | "running" | "failed"
    double  progress{ 1.0 }; ///< 0..1 (for running tasks)
    QVariantMap params;      ///< recipe (mirrors the AnalysisParamStore group)
    QString taskId;          ///< backend task id, if async
    QString sourceDatasetId; ///< volatile backend dataset id
    QString sessionId;       ///< backend session id
    QPointer<QWidget> window;///< result window to raise() on activation (optional)
};

class ProductRegistry : public QObject
{
    Q_OBJECT
public:
    static ProductRegistry &instance();

    /// Register a product. If @p p.productId is empty a unique one is assigned
    /// (returned). Emits productAdded.
    QString registerProduct(AnalysisProduct p);

    /// Update status/progress of a registered product (emits productUpdated).
    void setStatus(const QString &productId, const QString &status, double progress = 1.0);

    /// Associate a result window with a product (for focus-on-activate).
    void attachWindow(const QString &productId, QWidget *window);

    QList<AnalysisProduct> productsFor(const QString &parentKey) const;
    QList<AnalysisProduct> all() const { return m_products; }
    AnalysisProduct product(const QString &productId) const;

signals:
    void productAdded(const AnalysisProduct &product);
    void productUpdated(const AnalysisProduct &product);

private:
    ProductRegistry() = default;
    int indexOf(const QString &productId) const;

    QList<AnalysisProduct> m_products;
    int m_counter = 0;
};

#endif // PRODUCTREGISTRY_H
