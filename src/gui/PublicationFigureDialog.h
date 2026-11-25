#pragma once

// PublicationFigureDialog.h
// Tools > Products > Publication Figure… (UC 1.3, issue #41): the options of the
// figure on the left, a live preview on the right. The preview is the backend's
// own figure at low resolution (the same route with `preview`), asked for a moment
// after the last edit, one at a time, so what is shown is what Render writes.
// The options are in PublicationFigureSpec.h; this is only the widgets.

#include "PublicationFigureSpec.h"

#include <QDialog>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTimer;

namespace visivo::figure {

/// What the window that asks knows: the backend and dataset the figure is of, the
/// options the window shows (colour map, stretch, range, frame, title), and what
/// it has over the image.
struct Context
{
    QString backendUrl;
    QString token;
    QString sessionId;
    QString datasetId;
    QString fileStem;                    ///< output name without extension
    Options defaults;
    /// The viewer's colour-map registry (ColorMaps::GetColorMapNames); empty offers
    /// only the default's. The dialog opens with all of it and narrows it to what the
    /// backend can set when that answers (colourMapChoices).
    QStringList colourMaps;
    /// The name of the window's colour map; empty when it has none. The dialog
    /// starts from it.
    QString windowColourMap;
    /// The window's colours as 256 RGB rows in 0..1 (lowest value first, the map
    /// without the stretch the window applies on top), or empty when there is no
    /// table to send. With it the figure draws the window's map exactly, whatever it is.
    QJsonArray windowLut;
    std::shared_ptr<Overlays> overlays;  ///< a null one is taken as no overlays
};

class PublicationFigureDialog : public QDialog
{
public:
    PublicationFigureDialog(const Context &context, QWidget *parent);
    ~PublicationFigureDialog() override;

    /// The options as they stand.
    Options options() const;
    /// The request body of the final figure or of the preview. Joins the contour
    /// lines on first use: call it off the GUI thread.
    std::function<QJsonObject(bool preview)> bodyMaker() const;

private:
    struct Widgets;

    void build();
    void schedulePreview();
    void runPreview();
    void showPreview(const QPixmap &pm, const QString &message);
    /// Asks the backend, off the GUI thread and once per backend and session, which
    /// colour maps a figure can set; the answer narrows the list.
    void requestColourMaps();
    /// Rebuilds the colour-map list from @p answer, keeping the choice made.
    void fillColourMaps(const BackendFigureColourMaps &answer);
    void updateColourMapNote();

    Context m_ctx;
    std::unique_ptr<Widgets> w;
    QTimer *m_debounce{ nullptr };
    QLabel *m_preview{ nullptr };
    QLabel *m_status{ nullptr };
    QLabel *m_cmapNote{ nullptr };
    QList<ColourMapChoice> m_choices;   ///< what the combo lists, in order
    int m_seq{ 0 };           ///< the newest preview asked for
    bool m_inFlight{ false };
    bool m_dirty{ false };
};

}   // namespace visivo::figure
