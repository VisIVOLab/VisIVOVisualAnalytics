#pragma once
// TestVrSpectator.h
// The headset's view on the desktop (issue #281): what the spectator routes
// answer and what the desktop makes of it (app/VrSpectator.h), the feed
// against a fake backend (frames, a lapsed watch, the page gone, the
// snapshot), and the request bodies against the backend's request models.

#include <QObject>

class TestVrSpectator : public QObject
{
    Q_OBJECT

private slots:
    void watchBodyIsBoundedAndCarriesNoToken();
    void watchAnswersAreUnderstood();
    void frameRepliesAreParsed();
    void frameUrlAndRetryDelays();
    void snapshotAnswersAreUnderstood();
    void rateAndStatusLine();
    void feedShowsFramesAndSaysGoodbye();
    void feedStartsAgainWhenTheWatchLapsedAndEndsWhenThePageGoes();
    void feedSavesASnapshot();
    void feedSendsTheWatchAgainWhenNothingComes();
    void bodiesMatchTheBackendsRequestModels();
};
