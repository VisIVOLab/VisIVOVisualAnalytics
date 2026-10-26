#pragma once
// TestUpdateCheck.h
// QTest class for UpdateCheck — reading the GitHub "latest release" reply,
// comparing versions and pacing the automatic check. Core-only, no network.

#include <QObject>

class TestUpdateCheck : public QObject
{
    Q_OBJECT

private slots:
    void parse_readsTagAndReleasePage();
    void parse_refusesDraftsPrereleasesAndNonVersionTags();
    void parse_refusesBrokenJson();
    void parse_neverFollowsAForeignPageUrl();
    void isNewer_comparesNumericallyNotAlphabetically();
    void isNewer_staysQuietForBuildsItCannotPlace();
    void isCheckDue_runsAtMostOncePerInterval();
};
