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

    Context m_ctx;
    std::unique_ptr<Widgets> w;
    QTimer *m_debounce{ nullptr };
    QLabel *m_preview{ nullptr };
    QLabel *m_status{ nullptr };
    int m_seq{ 0 };           ///< the newest preview asked for
    bool m_inFlight{ false };
    bool m_dirty{ false };
};

}   // namespace visivo::figure
