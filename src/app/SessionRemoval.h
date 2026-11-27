#ifndef SESSIONREMOVAL_H
#define SESSIONREMOVAL_H

// Removing items from Session Data (issue #580), the part that does not depend
// on any widget: what a removal covers, what it has to ask, and which file is a
// temporary one to delete. SessionDataTree asks the user and calls the window's
// handler; the handler (one per viewer window) decides what removing an item
// means for what that window holds (a pane, a layer, a cache entry).

#include "ProductRegistry.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

namespace visivo::removal {

/// What a viewer window tells the Session Data tree about removal. The tree
/// owns the gesture (Delete key, context menu, multi-selection) and the
/// confirmation; the window owns the meaning.
struct Handler
{
    /// Why this window cannot remove the product (the master image of a
    /// layered window, say); empty when it can. Optional.
    std::function<QString(const AnalysisProduct &)> refuse;
    /// Remove these products: free what the window holds for them, then drop
    /// them from the registry. Called after the user confirmed.
    std::function<void(const QStringList &)> remove;
};

/// What a request to remove some products comes to.
struct Plan
{
    QStringList ids;         ///< to be removed, in the order asked, without repeats
    QStringList refused;     ///< "label: why" for each one that cannot go now
    QStringList dependants;  ///< other products made from them, not in @c ids
    QStringList unsaved;     ///< the ones of @c ids that are not saved permanently
    /// Asked first when something would be lost, or others were made from it.
    bool needsConfirmation() const { return !unsaved.isEmpty() || !dependants.isEmpty(); }
};

inline QString displayName(const AnalysisProduct &p)
{
    return p.label.isEmpty() ? p.kind : p.label;
}

/// True when the product has a permanent home on disk (a Workspace Export): a
/// removal then costs nothing. A product held only in memory, or in a
/// temporary file, is not.
inline bool savedPermanently(const AnalysisProduct &p)
{
    const QString storage = p.params.value(QStringLiteral("Storage")).toString();
    return !storage.isEmpty() && !storage.contains(QStringLiteral("temporary"), Qt::CaseInsensitive);
}

/// Why a product cannot be removed right now, regardless of the window: a
/// running tool owns its row until it finishes, a live preview until it ends.
inline QString busyReason(const AnalysisProduct &p)
{
    if (p.status == QStringLiteral("running"))
        return QStringLiteral("it is still running");
    // A tool that registers its row when it starts says so in its parameters
    // and only sets the status when it is done (the baseline).
    if (p.params.value(QStringLiteral("Status")).toString().startsWith(QStringLiteral("running")))
        return QStringLiteral("it is still running");
    if (p.status == QStringLiteral("live"))
        return QStringLiteral("it is a live preview; end the tool that shows it");
    return {};
}

/// The products to remove for @p requested, from the registry's @p all.
/// Ids that are not there, and repeats, are dropped. @p windowRefusal is the
/// window's own refusal (Handler::refuse). Products made from the ones asked
/// for, directly or through others, are listed as dependants unless they
/// cannot be removed either.
inline Plan planRemoval(const QList<AnalysisProduct> &all, const QStringList &requested,
                        const std::function<QString(const AnalysisProduct &)> &windowRefusal = {})
{
    Plan plan;
    QHash<QString, const AnalysisProduct *> byId;
    for (const AnalysisProduct &p : all)
        byId.insert(p.productId, &p);

    const auto refusal = [&](const AnalysisProduct &p) {
        QString why = busyReason(p);
        if (why.isEmpty() && windowRefusal)
            why = windowRefusal(p);
        return why;
    };

    QSet<QString> asked;
    for (const QString &id : requested) {
        const auto it = byId.constFind(id);
        if (it == byId.constEnd() || asked.contains(id))
            continue;
        asked.insert(id);
        const QString why = refusal(**it);
        if (!why.isEmpty()) {
            plan.refused << QStringLiteral("%1: %2").arg(displayName(**it), why);
            continue;
        }
        plan.ids << id;
        if (!savedPermanently(**it))
            plan.unsaved << id;
    }

    // Everything made from what goes, however far down. A product asked for
    // but refused stays: what was made from it is not touched either.
    QSet<QString> going(plan.ids.begin(), plan.ids.end());
    bool grew = true;
    while (grew) {
        grew = false;
        for (const AnalysisProduct &p : all) {
            if (going.contains(p.productId) || asked.contains(p.productId))
                continue;
            for (const QString &source : p.dependsOn) {
                if (!going.contains(source))
                    continue;
                if (!refusal(p).isEmpty())
                    break;
                going.insert(p.productId);
                plan.dependants << p.productId;
                grew = true;
                break;
            }
        }
    }
    return plan;
}

/// The question for the user, or empty when the plan needs none.
inline QString confirmationText(const QList<AnalysisProduct> &all, const Plan &plan)
{
    if (!plan.needsConfirmation())
        return {};
    QHash<QString, QString> names;
    for (const AnalysisProduct &p : all)
        names.insert(p.productId, displayName(p));
    const auto list = [&names](const QStringList &ids) {
        QStringList out;
        for (const QString &id : ids)
            out << QStringLiteral("“%1”").arg(names.value(id, id));
        return out.join(QStringLiteral(", "));
    };

    QStringList lines;
    if (!plan.unsaved.isEmpty()) {
        lines << (plan.ids.size() == 1
                          ? QStringLiteral("Remove %1 from this session?").arg(list(plan.ids))
                          : QStringLiteral("Remove %1 items from this session?")
                                    .arg(QString::number(plan.ids.size())));
        lines << QStringLiteral("%1 not saved permanently, so it cannot be brought back. "
                                "A permanent copy saved on disk is never touched.")
                         .arg(plan.unsaved.size() == 1 ? QStringLiteral("It is")
                                                       : QStringLiteral("%1 of them are")
                                                                 .arg(QString::number(
                                                                         plan.unsaved.size())));
    } else {
        lines << QStringLiteral("Remove %1 from this session?").arg(list(plan.ids));
    }
    if (!plan.dependants.isEmpty())
        lines << QStringLiteral("Made from %1: %2.")
                         .arg(plan.ids.size() == 1 ? QStringLiteral("it")
                                                   : QStringLiteral("these"),
                              list(plan.dependants));
    return lines.join(QStringLiteral("\n\n"));
}

/// The temporary file behind @p p that removing it deletes, or empty. Only a
/// file the product itself calls temporary, that exists, and that lies under
/// @p tempRoot (the system's temporary directory): never a file the user saved
/// or opened, whatever the product's parameters say.
inline QString temporaryFileToDelete(const AnalysisProduct &p, const QString &tempRoot)
{
    const QString storage = p.params.value(QStringLiteral("Storage")).toString();
    if (!storage.contains(QStringLiteral("temporary"), Qt::CaseInsensitive))
        return {};
    const QString path = p.params.value(QStringLiteral("Output path")).toString();
    if (path.isEmpty() || tempRoot.isEmpty())
        return {};
    const QString real = QFileInfo(path).canonicalFilePath();   // empty when it is gone
    const QString root = QDir(tempRoot).canonicalPath();
    if (real.isEmpty() || root.isEmpty())
        return {};
    return real.startsWith(root + QLatin1Char('/')) ? real : QString();
}

} // namespace visivo::removal

#endif // SESSIONREMOVAL_H
