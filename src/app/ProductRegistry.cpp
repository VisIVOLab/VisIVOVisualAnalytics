#include "ProductRegistry.h"

#include <QWidget>

ProductRegistry &ProductRegistry::instance()
{
    // Intentionally leaked heap singleton (never deleted): avoids static-QObject
    // teardown ordering issues after QApplication, and stays valid across tests
    // that recreate QApplication.
    static ProductRegistry *s_instance = new ProductRegistry();
    return *s_instance;
}

int ProductRegistry::indexOf(const QString &productId) const
{
    for (int i = 0; i < m_products.size(); ++i)
        if (m_products[i].productId == productId)
            return i;
    return -1;
}

QString ProductRegistry::registerProduct(AnalysisProduct p)
{
    if (p.productId.isEmpty()) {
        const QString kind = p.kind.isEmpty() ? QStringLiteral("product") : p.kind;
        p.productId = QStringLiteral("%1-%2").arg(kind).arg(++m_counter);
    }
    // Replace an existing entry with the same id rather than duplicating.
    const int existing = indexOf(p.productId);
    if (existing >= 0) {
        m_products[existing] = p;
        emit productUpdated(p);
        return p.productId;
    }
    m_products.append(p);
    emit productAdded(p);
    return p.productId;
}

void ProductRegistry::setStatus(const QString &productId, const QString &status,
                                double progress)
{
    const int i = indexOf(productId);
    if (i < 0)
        return;
    m_products[i].status = status;
    m_products[i].progress = progress;
    emit productUpdated(m_products[i]);
}

void ProductRegistry::setParams(const QString &productId, const QVariantMap &params)
{
    for (AnalysisProduct &p : m_products) {
        if (p.productId != productId)
            continue;
        p.params = params;
        emit productUpdated(p);
        return;
    }
}

void ProductRegistry::removeProduct(const QString &productId)
{
    const int i = indexOf(productId);
    if (i < 0)
        return;
    m_products.removeAt(i);
    emit productRemoved(productId);
}

void ProductRegistry::attachWindow(const QString &productId, QWidget *window)
{
    const int i = indexOf(productId);
    if (i < 0)
        return;
    m_products[i].window = window;
    emit productUpdated(m_products[i]);
}

QList<AnalysisProduct> ProductRegistry::productsFor(const QString &parentKey) const
{
    QList<AnalysisProduct> out;
    for (const AnalysisProduct &p : m_products)
        if (p.parentKey == parentKey)
            out.append(p);
    return out;
}

AnalysisProduct ProductRegistry::product(const QString &productId) const
{
    const int i = indexOf(productId);
    return i >= 0 ? m_products[i] : AnalysisProduct{};
}

QString ProductRegistry::newOwnerScope()
{
    return QStringLiteral("win%1").arg(++m_ownerCounter);
}
