#ifndef TestBackendContract_h
#define TestBackendContract_h

#include <QObject>

/// Contract test for the C++ side of the client<->backend header agreement.
/// Asserts that the canonical header names the desktop client reads
/// (src/app/BackendHeaders.h) exactly match the shared manifest the backend
/// generates (backend/tests/contract/visivo_headers.json). If the backend
/// renames/adds/drops an X-* header and regenerates the manifest without the
/// C++ mirror being regenerated too, this fails — catching the drift that would
/// otherwise make the client silently read metadata back as 0/"".
class TestBackendContract : public QObject
{
    Q_OBJECT
private slots:
    void constantsAreWellFormed();
    void constantsMatchManifest();
};

#endif  // TestBackendContract_h
