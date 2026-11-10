#pragma once
// TestFilePreview.h
// QTest class for the file browser's preview of any file (issue #159): the
// reply of /v1/files/preview, the older-backend fallback decision and the
// caption. Header-only logic plus BackendClient against a loopback server.

#include <QObject>

class TestFilePreview : public QObject
{
    Q_OBJECT

private slots:
    void parsesATextPreview();
    void aReplyWithoutKindIsAnError();
    void notJsonIsAnError();
    void onlyFastApisOwn404MeansAnOldBackend();
    void fallbackOnAnOldBackend();
    void captionNamesTheFormatAndTruncation();
    void bodyExplainsFilesWithNoText();
    void headerCardsBecomeAFitsPreview();
    void clientReadsThePreviewRoute();
    void clientTellsAMissingRouteFromAMissingFile();
};
