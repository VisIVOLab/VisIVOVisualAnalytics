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
    QString label;           ///< e.g. "Moment 0, integrated"
    QString renderer;        ///< non-empty ⇒ showable in a pane (§3.2); "" ⇒ Inspector-only
                             ///< (e.g. "moment2d"). Statistics/exports leave this empty.
    QString status{ QStringLiteral("ready") };  ///< "ready" | "running" | "failed"
    double  progress{ 1.0 }; ///< 0..1 (for running tasks)
    QVariantMap params;      ///< recipe (mirrors the AnalysisParamStore group)
    QString taskId;          ///< backend task id, if async
    QString sourceDatasetId; ///< volatile backend dataset id
    QString sessionId;       ///< backend session id
    /// The service route that performed the transition and replays it from
    /// `params` (e.g. "/v1/cube/mask_apply_bin"). Empty for a product that is a
    /// client-side measurement rather than a service call.
    QString endpoint;
    QString ownerId;         ///< the VIEWER WINDOW that produced+renders it (scope token). Two
                             ///< windows on the same dataset share parentKey but not ownerId, so
                             ///< product ids never collide and each window's tree shows only its
                             ///< own products (their render caches/widgets are per-window).
    QPointer<QWidget> window;///< result window to raise() on activation (optional)
    /// The exact recipe of the result window this product came from
    /// (ToolResultDialog::makeRecord), so the row can put it back on screen
    /// unchanged. Empty for products whose result is a pane or a file.
    QVariantMap resultView;
    /// WHERE the tool measured: the drawn region, or the probed voxel
    /// (ProductOrigin.h). Opening the row redraws it, so a number can be traced
    /// back to the geometry that produced it instead of only being described.
    /// Empty when the product has no geometry (a whole-image metric, a file).
    QVariantMap origin;
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

    /// Remove a product (emits productRemoved). Used for a TRANSITORY live entry that a tool
    /// promotes-or-drops (e.g. the Extract-Spectrum live preview). No-op if not present.
    void removeProduct(const QString &productId);

    /// Replace a product's params (emits productUpdated). For a tool that
    /// registers its row when it STARTS — so the user sees it running — and only
    /// learns the recipe / output location when the result lands.
    void setParams(const QString &productId, const QVariantMap &params);

    /// Attach the result-window recipe (emits productUpdated).
    void setResultView(const QString &productId, const QVariantMap &record);

    /// Associate a result window with a product (for focus-on-activate).
    void attachWindow(const QString &productId, QWidget *window);

    QList<AnalysisProduct> productsFor(const QString &parentKey) const;
    QList<AnalysisProduct> all() const { return m_products; }
    AnalysisProduct product(const QString &productId) const;

    /// Mint a fresh, process-unique owner scope token for a viewer window. Each vtkWindowCube /
    /// vtkWindowImage calls this once so its products are tagged (and their ids salted) with a
    /// value distinct from every other window — even a second window on the same dataset.
    QString newOwnerScope();

signals:
    void productAdded(const AnalysisProduct &product);
    void productUpdated(const AnalysisProduct &product);
    void productRemoved(const QString &productId);

private:
    ProductRegistry() = default;
    int indexOf(const QString &productId) const;

    QList<AnalysisProduct> m_products;
    int m_counter = 0;
    int m_ownerCounter = 0;   ///< monotonic source for newOwnerScope()
};

#endif // PRODUCTREGISTRY_H
