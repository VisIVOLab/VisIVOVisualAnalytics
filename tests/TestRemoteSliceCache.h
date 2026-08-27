#pragma once
// TestRemoteSliceCache.h
// QTest class for the RemoteSliceCache<Slice> LRU (E-04 god-object step 1).
// Header-only, GUI/VTK-free: instantiated with a fake slice type.

#include <QObject>

class TestRemoteSliceCache : public QObject
{
    Q_OBJECT

private slots:
    void find_missReturnsNull();
    void insertThenFind_hit();
    void insert_invalidIgnored();
    void insert_noImageDataIgnored();
    void lru_evictsLeastRecentlyUsed();
    void find_refreshesRecency_savesFromEviction();
    void touch_refreshesRecency();
    void touch_absentKeyIsNoOp_noOverEviction();
    void reinsertSameKey_updatesValue_noGrowth();
};
