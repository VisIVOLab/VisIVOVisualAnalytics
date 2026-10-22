#include "ProductRecipe.h"

#include "ProductRegistry.h"

#include <QJsonArray>

using namespace Qt::StringLiterals;

namespace visivo {

QJsonObject productRecipe(const AnalysisProduct &p, const QString &application,
                          const QString &exported)
{
    // The exported recipe used to be the label→value map of the tab and nothing
    // else: enough to read, not enough to replay. A record that claims to
    // reproduce a product has to say which dataset it came from, which route
    // performs each transition and with what request, and which version of the
    // software recorded it.
    QJsonObject recipe;
    recipe.insert(u"schema"_s, u"visivo-recipe/1"_s);
    recipe.insert(u"application"_s, application);
    recipe.insert(u"exported"_s, exported);

    QJsonObject product;
    product.insert(u"id"_s, p.productId);
    product.insert(u"kind"_s, p.kind);
    product.insert(u"label"_s, p.label);
    if (!p.replay.isEmpty()) {
        // One object per call, in order. `request` holds the values the client
        // sent under the API's field names — numbers as numbers, seeds as
        // [z, y, x] — so a script posts them back instead of parsing the display
        // strings of `parameters`; `body` names what travelled as bytes, which
        // the record describes but does not carry.
        QJsonArray steps;
        for (const ReplayStep &step : p.replay) {
            QJsonObject s;
            s.insert(u"route"_s, step.route);
            if (!step.request.isEmpty())
                s.insert(u"request"_s, QJsonObject::fromVariantMap(step.request));
            if (!step.body.isEmpty())
                s.insert(u"body"_s, step.body);
            steps.append(s);
        }
        product.insert(u"replay"_s, steps);
    }
    if (!p.replayCaveat.isEmpty())
        product.insert(u"replay_caveat"_s, p.replayCaveat);
    recipe.insert(u"product"_s, product);

    QJsonObject dataset;
    dataset.insert(u"label"_s, p.parentLabel);
    dataset.insert(u"path"_s, p.parentKey);
    // Volatile ids: valid for the session that made the product, kept because
    // they tie the record to the service's own job log. A replay opens `path`
    // again and uses the dataset id that open returns.
    if (!p.sourceDatasetId.isEmpty())
        dataset.insert(u"backend_dataset_id"_s, p.sourceDatasetId);
    if (!p.sessionId.isEmpty())
        dataset.insert(u"backend_session_id"_s, p.sessionId);
    recipe.insert(u"dataset"_s, dataset);

    recipe.insert(u"parameters"_s, QJsonObject::fromVariantMap(p.params));
    if (!p.origin.isEmpty())
        recipe.insert(u"origin"_s, QJsonObject::fromVariantMap(p.origin));
    return recipe;
}

} // namespace visivo
