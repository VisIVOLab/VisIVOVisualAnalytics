#include "VrSpectatorWindow.h"

#include "app/VrSpectatorFeed.h"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

VrSpectatorView::VrSpectatorView(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(240, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void VrSpectatorView::setImage(const QImage &image)
{
    m_image = image;
    m_placeholder.clear();
    update();
}

void VrSpectatorView::setPlaceholder(const QString &text)
{
    m_placeholder = text;
    update();
}

void VrSpectatorView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (!m_image.isNull()) {
        const QSize fitted = m_image.size().scaled(size(), Qt::KeepAspectRatio);
        const QRect target(QPoint((width() - fitted.width()) / 2, (height() - fitted.height()) / 2), fitted);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(target, m_image);
    }
    if (!m_placeholder.isEmpty()) {
        p.setPen(Qt::lightGray);
        p.drawText(rect().adjusted(16, 16, -16, -16), Qt::AlignCenter | Qt::TextWordWrap, m_placeholder);
    }
}

VrSpectatorWindow::VrSpectatorWindow(const QString &baseUrl, const QString &token, const QString &session,
                                     const QString &client, const QString &title, QWidget *parent)
    : QWidget(parent, Qt::Window), m_client(client)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(u"Headset view — %1"_s.arg(title.isEmpty() ? u"headset"_s : title));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    m_view = new VrSpectatorView(this);
    m_view->setPlaceholder(u"Waiting for the headset's view…"_s);
    layout->addWidget(m_view, 1);
    auto *row = new QHBoxLayout;
    m_status = new QLabel(u"Asking the headset…"_s, this);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setToolTip(u"Frames a second seen here, the frame's size, how old it was when it "
                         u"came, and what capturing it costs the headset per frame."_s);
    row->addWidget(m_status, 1);
    m_snapshot = new QPushButton(u"Snapshot"_s, this);
    m_snapshot->setEnabled(false);
    m_snapshot->setToolTip(u"Save the headset's current view as a PNG in the Workspace Exports."_s);
    row->addWidget(m_snapshot);
    layout->addLayout(row);
    resize(720, 760);

    m_feed = new VrSpectatorFeed(baseUrl, token, session, client, this);
    connect(m_feed, &VrSpectatorFeed::frameReady, this, &VrSpectatorWindow::onFrame);
    connect(m_feed, &VrSpectatorFeed::watching, this, [this](int fps, int width) {
        m_status->setText(u"Watching (%1 fps, %2 px wide asked); waiting for the first frame…"_s.arg(fps).arg(width));
    });
    connect(m_feed, &VrSpectatorFeed::ended, this, [this](const QString &why) {
        m_status->setText(u"Stopped: %1"_s.arg(why));
        if (m_view->image().isNull())
            m_view->setPlaceholder(why);
    });
    connect(m_feed, &VrSpectatorFeed::trouble, this, [this](const QString &why) {
        m_status->setText(u"%1 (trying again)"_s.arg(why));
    });
    connect(m_snapshot, &QPushButton::clicked, this, [this]() {
        m_snapshot->setEnabled(false);
        m_feed->snapshot();
    });
    connect(m_feed, &VrSpectatorFeed::snapshotSaved, this, [this](const VrSpectator::SnapshotResult &r) {
        m_snapshot->setEnabled(true);
        m_status->setText(u"Saved %1 in the Workspace Exports."_s.arg(r.filename));
        emit snapshotSaved(r.filename);
    });
    connect(m_feed, &VrSpectatorFeed::snapshotFailed, this, [this](const QString &why) {
        m_snapshot->setEnabled(!m_view->image().isNull());
        m_status->setText(u"Snapshot not saved: %1"_s.arg(why));
    });
    m_feed->start();
}

VrSpectatorWindow::~VrSpectatorWindow() = default;

void VrSpectatorWindow::onFrame(const VrSpectator::FrameReply &frame)
{
    QImage image;
    if (!image.loadFromData(frame.jpeg, "JPEG")) {
        m_status->setText(u"A frame could not be decoded."_s);
        return;
    }
    m_view->setImage(image);
    m_snapshot->setEnabled(true);
    m_status->setText(VrSpectator::statusLine(m_feed->fps(), image.width(), image.height(), frame.ageMs, frame.costMs));
}

void VrSpectatorWindow::closeEvent(QCloseEvent *event)
{
    m_feed->stop();
    QWidget::closeEvent(event);
}
