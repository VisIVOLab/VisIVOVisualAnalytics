#ifndef RegionPropertiesDialog_h
#define RegionPropertiesDialog_h

#include "RegionFrameText.h"
#include "RegionModel.h"

#include <QDialog>
#include <QJsonArray>
#include <QPointer>
#include <QString>

#include <functional>
#include <vector>

struct BackendRegionConvertResult;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTimer;
class QWidget;

// ---------------------------------------------------------------------------
// RegionPropertiesDialog (issue #341)
//
// Sets one region by numbers: its centre (or polygon vertices), sizes and
// angle, typed in a frame the user picks: image pixels counted from 1 as ds9
// does, ICRS or FK5 in degrees or sexagesimal, or galactic degrees. Sky frames
// are offered only when the dataset has a celestial WCS.
//
// Every move between pixels and the sky goes through the backend
// (POST /v1/regions/convert, astropy on the dataset's full WCS), never the
// viewer's linear approximation: a position typed from a catalogue has to land
// where ds9 would put it.
//
// Non-modal. The viewer feeds it the region again whenever it changes on
// screen (setRegion), and the numbers follow — unless the user has started
// typing, which is never overwritten. Apply sends the edited region back
// (regionEdited); the viewer puts it in its list, which is one undo step.
// ---------------------------------------------------------------------------
class RegionPropertiesDialog : public QDialog
{
    Q_OBJECT

public:
    struct Backend
    {
        QString url;
        QString token;
        QString sessionId;
        QString datasetId;   ///< empty: pixels only
    };

    explicit RegionPropertiesDialog(const Backend &backend, QWidget *parent = nullptr);

    /// Show `region`. A different region discards unapplied edits; the same
    /// one (moved on screen) refreshes the numbers unless the user is editing.
    void setRegion(const visivo::regions::Region &region);
    int regionId() const { return m_region.id; }

signals:
    void regionEdited(const visivo::regions::Region &region);

private:
    using Done = std::function<void(const BackendRegionConvertResult &)>;
    void convert(const QJsonArray &regions, const QString &toFrame, const QString &unit, Done done);
    void populateFrames(bool hasCelestialWcs);
    visivo::regions::FrameChoice currentFrame() const;
    void rebuildFields();
    void scheduleRefresh();
    void showInCurrentFrame();
    void fillFields(const visivo::regions::RegionFields &fields);
    visivo::regions::RegionFields readFields() const;
    void apply();
    void finishApply(visivo::regions::Region edited);
    void markEdited();
    void setStatus(const QString &text, bool error);

    Backend m_backend;
    visivo::regions::Region m_region;
    int m_fieldsShape{ -1 };   ///< the shape the field widgets are built for
    bool m_dirty{ false };     ///< the user typed something not yet applied
    bool m_filling{ false };   ///< the dialog itself is writing the fields
    bool m_framesKnown{ false };
    int m_showGeneration{ 0 };
    int m_applyGeneration{ 0 };
    QString m_preferredFrame;  ///< the frame to open in once the WCS is known

    QLineEdit *m_name{ nullptr };
    QLabel *m_shapeLabel{ nullptr };
    QComboBox *m_frame{ nullptr };
    QWidget *m_fieldsHost{ nullptr };
    QFormLayout *m_fieldsForm{ nullptr };
    QLineEdit *m_lon{ nullptr };
    QLineEdit *m_lat{ nullptr };
    QTableWidget *m_vertices{ nullptr };
    std::vector<QDoubleSpinBox *> m_sizes;
    QDoubleSpinBox *m_angle{ nullptr };
    QLabel *m_status{ nullptr };
    QPushButton *m_applyButton{ nullptr };
    QTimer *m_refreshTimer{ nullptr };
};

#endif // RegionPropertiesDialog_h
