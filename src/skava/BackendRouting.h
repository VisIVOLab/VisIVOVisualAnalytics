#ifndef BackendRouting_h
#define BackendRouting_h

#include <QList>
#include <QString>
#include <QStringList>

#include <limits>

#include "../Settings.h"    // BackendNode
#include "SkavaClient.h"    // SkavaDatalinkResponse

class Settings;

/// Where to open a SKAVA dataset: the registered backend to use and the
/// address of the dataset's copy ON THAT backend's node. The two travel
/// together because SKAVA can list a VisIVO backend on every node that holds
/// a replica, and each node's file lives at its own path.
struct SkavaRoute
{
    BackendNode node;          ///< empty id = no usable registered backend
    QString     accessUrl;     ///< the replica on node.srcCode / the descriptor's node
    QString     nodeCode;      ///< SKAVA node code of the chosen descriptor
    QString     endpoint;      ///< the descriptor's backend URL
    /// SKAVA's reason for ranking this node where it is, and its cost under
    /// the policy (NaN when SKAVA gave none): shown with the choice.
    QString     rankReason;
    double      score{ std::numeric_limits<double>::quiet_NaN() };
    /// One line per descriptor that was passed over and why, in SKAVA's
    /// ranking order — shown to the user when nothing matched.
    QStringList skipped;
    bool valid() const { return !node.id.isEmpty() && !accessUrl.isEmpty(); }
};

/// Every usable route for a dataset, in the order to try them: SKAVA's
/// ranking, with the node the user asked for (if any, and usable) first. A
/// backend that refuses the open is followed by the next candidate; the
/// download is the last resort.
struct SkavaRouting
{
    QList<SkavaRoute> candidates;
    /// The descriptors no candidate came from and why, in ranking order.
    QStringList       skipped;
    bool valid() const { return !candidates.isEmpty(); }
};

/// The usable routes for a SKAVA dataset (docs/distributed-backend-
/// design.md §4.2). Descriptors are tried in SKAVA's ranking order; one is
/// usable when a registered node matches it (same URL, or same SKAVA node
/// code), it serves this kind of dataset (``supports_kinds``), the node holds
/// a replica, and — when the backend requires authentication — the client
/// will send a token: the node's own, or, when the node is the local backend
/// (BackendClient::isLocalBackendUrl) registered without one, the automatic
/// one BackendClient falls back to (VISIVO_TOKEN / ~/.visivo_token), which
/// ``autoToken`` says exists. ``preferredNode`` (a
/// SKAVA node code) moves that node's route to the front. No Settings, no
/// network, no file access (it reads only the local backend URL BackendClient holds).
SkavaRouting routeSkavaCandidates(const QList<BackendNode> &registry,
                                  const SkavaDatalinkResponse &datalink,
                                  bool autoToken = false,
                                  const QString &preferredNode = {});

/// The first candidate of routeSkavaCandidates(), with every skip reason.
SkavaRoute routeSkavaDataset(const QList<BackendNode> &registry,
                             const SkavaDatalinkResponse &datalink,
                             bool autoToken = false);

/// The same, reading the registry from Settings.
SkavaRoute routeSkavaDataset(Settings *settings, const SkavaDatalinkResponse &datalink);
SkavaRouting routeSkavaCandidates(Settings *settings, const SkavaDatalinkResponse &datalink,
                                  const QString &preferredNode = {});

/// Backwards-compatible wrapper: the routed node only.
BackendNode pickBackendForSkavaDataset(Settings *settings,
                                       const SkavaDatalinkResponse &datalink);

/// The registered node a descriptor refers to: same URL (trailing slash and
/// loopback spellings folded), else same SKAVA node code. Index into
/// ``registry``, or -1. The one matching rule, shared by routing and by the
/// "register / give a token to this backend" prompt.
int findRegisteredBackend(const QList<BackendNode> &registry,
                          const SkavaVisivoBackendDescriptor &desc);

/// Whether a VisIVO backend advertising ``supportsKinds`` can open an ObsCore
/// ``dataproduct_type``. An empty list or an empty type is no constraint.
bool skavaBackendSupportsKind(const QStringList &supportsKinds, const QString &dataproductType);

#endif  // BackendRouting_h
