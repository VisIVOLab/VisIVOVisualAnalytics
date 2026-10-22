#ifndef PRODUCTRECIPE_H
#define PRODUCTRECIPE_H

// ProductRecipe — the exported record of an analysis product (Inspector ›
// Provenance › Export recipe as JSON…), kept out of the panel so its shape is
// testable without a widget: the product, the dataset it came from, the
// parameters as the tab shows them, and the service calls that replay it, each
// with the request it was sent, typed, under the API's own field names.

#include <QJsonObject>
#include <QString>

struct AnalysisProduct;

namespace visivo {

/// `"schema": "visivo-recipe/1"`. @p application is the name and version that
/// recorded it, @p exported the UTC time of the export as ISO 8601.
QJsonObject productRecipe(const AnalysisProduct &product, const QString &application,
                          const QString &exported);

} // namespace visivo

#endif // PRODUCTRECIPE_H
