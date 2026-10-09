#ifndef BackendRouting_h
#define BackendRouting_h

#include "../Settings.h"    // BackendNode
#include "SkavaClient.h"    // SkavaDatalinkResponse

class Settings;

/// Pick the registered backend that SKAVA's DataLink descriptors point at for a
/// dataset. Matching is by (1) exact base-URL match (trailing slashes
/// normalised), then (2) SKAVA ``node_code`` == registry ``srcCode``. Returns a
/// default-constructed (empty ``id``) BackendNode when there is no SKAVA hint or
/// no match — the caller then falls back to the local download path.
///
/// Extracted from MainWindow so the routing decision is unit-testable without
/// pulling in the GUI/VTK stack. See docs/distributed-backend-design.md
/// §4.2 (routing decision) / §6.5 (MainWindow routing).
BackendNode pickBackendForSkavaDataset(Settings *settings,
                                       const SkavaDatalinkResponse &datalink);

#endif  // BackendRouting_h
