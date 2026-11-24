#include "SaveViewDialog.h"

#include <vtkRenderWindow.h>
#include <vtkRenderer.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>

using namespace Qt::StringLiterals;
using namespace visivo::renderexport;

namespace visivo {

SaveViewDialog::SaveViewDialog(const QSize &view, bool streamed, const QString &title,
                               QWidget *parent)
    : QDialog(parent), m_view(view), m_streamed(streamed)
{
    setWindowTitle(u"Save View As"_s);
    auto *form = new QFormLayout(this);

    m_format = new QComboBox(this);
    m_format->addItem(u"PNG image"_s, int(Format::Png));
    m_format->addItem(u"PDF, vector annotations"_s, int(Format::Pdf));
    m_format->addItem(u"SVG, vector annotations"_s, int(Format::Svg));
    m_format->setToolTip(u"A PDF or an SVG keeps the axes, the colour bar and the title as "
                         u"vectors; the render itself is an image inside it."_s);
    form->addRow(u"Format"_s, m_format);

    m_size = new QComboBox(this);
    m_size->addItem(u"Current size of the view"_s, int(SizeChoice::Current));
    m_size->addItem(u"2 x the view"_s, int(SizeChoice::Twice));
    m_size->addItem(u"4 x the view"_s, int(SizeChoice::Fourfold));
    m_size->addItem(u"Width in pixels"_s, int(SizeChoice::Custom));
    m_size->setToolTip(u"The camera and the shape of the view are kept; the render is made "
                       u"again at this size, not enlarged."_s);
    m_width = new QSpinBox(this);
    m_width->setRange(16, kMaxSide);
    m_width->setSuffix(u" px"_s);
    m_width->setValue(std::max(16, view.width()));
    m_sizeNote = new QLabel(this);
    m_sizeNote->setWordWrap(true);
    form->addRow(u"Size"_s, m_size);
    form->addRow(u"Width"_s, m_width);
    form->addRow(QString(), m_sizeNote);

    m_transparent = new QCheckBox(u"Transparent background"_s, this);
    m_transparent->setToolTip(u"PNG with an alpha channel: nothing is drawn where the scene is "
                              u"empty, and the volume fades into it."_s);
    form->addRow(m_transparent);

    m_annotations = new QCheckBox(u"Axes, colour bar and title"_s, this);
    m_annotations->setChecked(true);
    form->addRow(m_annotations);

    m_title = new QLineEdit(title, this);
    m_title->setPlaceholderText(u"Optional, drawn at the top left"_s);
    form->addRow(u"Title"_s, m_title);

    if (streamed) {
        auto *note = new QLabel(
                u"This view is drawn on the server. The picture is the frame now on screen, "
                u"at the size it was streamed in, with its own background."_s,
                this);
        note->setWordWrap(true);
        form->addRow(note);
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_ok = buttons->button(QDialogButtonBox::Ok);
    m_ok->setText(u"Save…"_s);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(buttons);

    connect(m_format, &QComboBox::currentIndexChanged, this, [this](int) { refresh(); });
    connect(m_size, &QComboBox::currentIndexChanged, this, [this](int) { refresh(); });
    connect(m_width, &QSpinBox::valueChanged, this, [this](int) { refresh(); });
    connect(m_annotations, &QCheckBox::toggled, this, [this](bool) { refresh(); });
    refresh();
}

Request SaveViewDialog::request() const
{
    Request r;
    r.format = static_cast<Format>(m_format->currentData().toInt());
    r.size = static_cast<SizeChoice>(m_size->currentData().toInt());
    r.customWidth = m_width->value();
    r.transparent = m_transparent->isChecked() && !m_streamed;
    r.annotations = m_annotations->isChecked();
    return r;
}

QString SaveViewDialog::title() const
{
    return m_annotations->isChecked() ? m_title->text().trimmed() : QString();
}

void SaveViewDialog::refresh()
{
    const Request r = request();
    const bool custom = r.size == SizeChoice::Custom;
    m_size->setEnabled(!m_streamed);
    m_width->setEnabled(!m_streamed && custom);
    m_transparent->setEnabled(!m_streamed && r.format == Format::Png);
    if (!m_transparent->isEnabled())
        m_transparent->setChecked(false);
    m_title->setEnabled(m_annotations->isChecked());
    if (m_streamed) {
        m_sizeNote->setText(u"%1 x %2 px"_s.arg(m_view.width()).arg(m_view.height()));
        m_ok->setEnabled(true);
        return;
    }
    const QSize out = outputSize(m_view, r);
    const QString problem = sizeProblem(out);
    m_sizeNote->setText(problem.isEmpty()
                                ? u"%1 x %2 px"_s.arg(out.width()).arg(out.height())
                                : problem);
    m_ok->setEnabled(problem.isEmpty());
    // A vector file's page is the view; the render inside it is at this size.
}

QString saveViewAs(const SaveViewSource &source)
{
    QWidget *parent = source.view ? source.view->window() : nullptr;
    const bool streamed = source.window == nullptr;
    if (streamed && source.frame.isNull()) {
        QMessageBox::information(parent, u"Save View As"_s,
                                 u"The server has not sent a frame of this view yet."_s);
        return {};
    }
    if (!streamed && !source.renderer) {
        QMessageBox::warning(parent, u"Save View As"_s, u"There is no 3-D view to save."_s);
        return {};
    }

    QSize deviceSize;
    if (streamed) {
        deviceSize = source.frame.size();
    } else {
        const int *s = source.window->GetSize();
        deviceSize = QSize(s[0], s[1]);
    }
    QSize logicalSize = source.view ? source.view->size() : deviceSize;
    if (streamed)
        logicalSize = deviceSize;
    if (deviceSize.isEmpty() || logicalSize.isEmpty()) {
        QMessageBox::information(parent, u"Save View As"_s, u"The 3-D view has no size yet."_s);
        return {};
    }

    SaveViewDialog dialog(deviceSize, streamed, QString(), parent);
    if (dialog.exec() != QDialog::Accepted)
        return {};
    Request request = dialog.request();

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    QString path = QFileDialog::getSaveFileName(
            parent, u"Save View As"_s,
            dir + u"/"_s + suggestedName(source.name, request.format), filterFor(request.format));
    if (path.isEmpty())
        return {};
    // A name typed with another suffix asks for that format.
    request.format = formatOfName(path, request.format);
    const QString typed = path;
    path = withSuffix(path, request.format);
    // The file dialog asked about overwriting the name as typed, not this one.
    if (path != typed && QFileInfo::exists(path)
        && QMessageBox::question(parent, u"Save View As"_s,
                                 u"%1 already exists. Replace it?"_s.arg(QFileInfo(path).fileName()))
                != QMessageBox::Yes)
        return {};
    const bool vector = request.format != Format::Png;
    if (vector)
        request.transparent = false;   // the page has its own background

    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    QImage scene;
    Annotations notes;
    notes.logicalSize = logicalSize;
    notes.deviceSize = deviceSize;
    notes.title = dialog.title();
    if (request.annotations) {
        // A PNG keeps what the scene draws itself (it is part of the pixels);
        // the colour bar is added when the scene has none. A vector file has
        // all of them redrawn as vectors.
        if (!source.colourbarInScene || vector)
            notes.colourbar = source.colourbar;
        if (vector && !streamed) {
            notes.box = axesBoxOf(source.axes);
            notes.renderer = source.renderer;
        }
    }
    if (streamed) {
        scene = source.frame;
    } else {
        // The scene's own annotations stay in a PNG as they are on screen; a
        // vector file (or no annotations) gets the bare render.
        const bool hide = vector || !request.annotations;
        scene = renderScene(source.window, source.renderer, outputSize(deviceSize, request),
                            request.transparent,
                            hide ? source.hideInScene : std::function<void(bool)>(), &error);
    }
    const bool ok = !scene.isNull() && writePicture(path, request.format, scene, notes, &error);
    QGuiApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::warning(parent, u"Save View As"_s,
                             error.isEmpty() ? u"The picture could not be saved."_s : error);
        return {};
    }
    return path;
}

} // namespace visivo
