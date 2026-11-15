#pragma once

// VrSpectatorWindow.h
// The headset's view on the desktop (issue #281): a window that shows what the
// user in the headset sees, live (one eye, cropped to the middle of the view,
// with the menu, the wrist cards and the ray), for teaching, demos and
// recording. Snapshot saves the current view as a PNG in the Workspace
// Exports.
//
// Opened from Headset Activity's Watch button. Watching starts when the window
// opens and stops when it closes (the headset then hides its "Desktop
// watching" badge). The frames come from VrSpectatorFeed.

#include "app/VrSpectator.h"

#include <QImage>
#include <QWidget>

class QLabel;
class QPushButton;
class VrSpectatorFeed;

/// The frame, scaled to fit with its aspect kept, centred on black.
class VrSpectatorView : public QWidget
{
    Q_OBJECT

public:
    explicit VrSpectatorView(QWidget *parent = nullptr);
    void setImage(const QImage &image);
    void setPlaceholder(const QString &text);
    const QImage &image() const { return m_image; }
    QSize sizeHint() const override { return { 640, 640 }; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QImage m_image;
    QString m_placeholder;
};

class VrSpectatorWindow : public QWidget
{
    Q_OBJECT

public:
    /// Watches @p client (a headset page of the session) on @p baseUrl.
    /// @p title names the headset ("Quest 3").
    VrSpectatorWindow(const QString &baseUrl, const QString &token, const QString &session,
                      const QString &client, const QString &title, QWidget *parent = nullptr);
    ~VrSpectatorWindow() override;

    QString client() const { return m_client; }
    VrSpectatorFeed *feed() const { return m_feed; }

signals:
    /// A snapshot was saved: its file name in the Workspace Exports.
    void snapshotSaved(const QString &filename);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void onFrame(const VrSpectator::FrameReply &frame);

    QString m_client;
    VrSpectatorFeed *m_feed{ nullptr };
    VrSpectatorView *m_view{ nullptr };
    QLabel *m_status{ nullptr };
    QPushButton *m_snapshot{ nullptr };
};
