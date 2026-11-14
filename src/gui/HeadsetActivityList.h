#pragma once

// HeadsetActivityList.h
// The rows of Headset Activity (issue #238) in Inspector › Parameters: what
// was done in the headset, newest first, each product with its Open button.
//
// A plain scroll area of wrapped rows, not a QListWidget with item widgets:
// the theme pads list items (8 px 12 px) and an item widget is placed inside
// that padding, so a row sized to its own hint lost most of its height. The
// look, clip and cube rows showed no text at all and the Open buttons were
// cut in half (issue #249). Here each row is as tall as its wrapped text
// needs at the panel's width, and nothing scrolls sideways.
//
// Widgets only, no VTK: the geometry harness builds it on its own.

#include "app/VrHeadsetHub.h"

#include <QScrollArea>

class QVBoxLayout;

class HeadsetActivityList : public QScrollArea
{
    Q_OBJECT

public:
    explicit HeadsetActivityList(QWidget *parent = nullptr);

    /// Rebuilt from @p events (newest first); none: says so.
    void setEvents(const QList<VrHub::Event> &events);

    /// A row's text, with a break allowed after the separators of file names
    /// and paths, so a long name wraps inside the panel instead of widening it.
    static QString wrappable(const QString &text);

signals:
    /// Open pressed on a product's row.
    void openRequested(const VrHub::Event &event);

private:
    QWidget *m_rows{ nullptr };
    QVBoxLayout *m_rowsLayout{ nullptr };
};
