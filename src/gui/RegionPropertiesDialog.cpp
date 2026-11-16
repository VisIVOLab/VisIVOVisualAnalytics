#include "RegionPropertiesDialog.h"

#include "app/BackendClient.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrentRun>

using namespace Qt::StringLiterals;
using namespace visivo::regions;

RegionPropertiesDialog::RegionPropertiesDialog(const Backend &backend, QWidget *parent)
    : QDialog(parent), m_backend(backend)
{
    setWindowTitle(u"Region Properties"_s);
    setAttribute(Qt::WA_DeleteOnClose);
    setModal(false);
    // A tool window floats over the viewer. As a plain dialog it went behind
    // the image the moment the user dragged a region there, and macOS does not
    // repaint a covered window: the numbers that follow the drag were hidden
    // exactly when they mattered (seen in the app).
    setWindowFlag(Qt::Tool, true);

    auto *layout = new QVBoxLayout(this);
    auto *head = new QFormLayout;
    m_name = new QLineEdit(this);
    m_shapeLabel = new QLabel(this);
    m_frame = new QComboBox(this);
    m_frame->setToolTip(u"The frame the numbers below are read and typed in. Positions are "
                        u"converted with the dataset's WCS by the backend."_s);
    head->addRow(u"Name"_s, m_name);
    head->addRow(u"Shape"_s, m_shapeLabel);
    head->addRow(u"Frame"_s, m_frame);
    layout->addLayout(head);

    m_fieldsHost = new QWidget(this);
    m_fieldsForm = new QFormLayout(m_fieldsHost);
    m_fieldsForm->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_fieldsHost);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, this);
    m_applyButton = buttons->button(QDialogButtonBox::Apply);
    m_applyButton->setDefault(true);
    m_applyButton->setAutoDefault(true);
    layout->addWidget(buttons);
    connect(m_applyButton, &QPushButton::clicked, this, &RegionPropertiesDialog::apply);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(m_name, &QLineEdit::textEdited, this, &RegionPropertiesDialog::markEdited);

    // A drag on screen sends a region per mouse move; only the last one needs
    // converting.
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(60);
    connect(m_refreshTimer, &QTimer::timeout, this, &RegionPropertiesDialog::showInCurrentFrame);

    connect(m_frame, &QComboBox::currentIndexChanged, this, [this]() {
        if (m_filling)
            return;
        // A new frame shows the region as it is; half-typed numbers in the old
        // frame would not mean anything in this one.
        m_dirty = false;
        rebuildFields();
        showInCurrentFrame();
    });

    populateFrames(false);
    if (m_backend.datasetId.isEmpty()) {
        m_framesKnown = true;
        setStatus(u"No dataset on the backend: positions in image pixels only."_s, false);
    } else {
        setStatus(u"Reading the dataset's coordinate frame…"_s, false);
        // An empty conversion only reports the dataset's sky frame.
        convert({}, u"image"_s, {}, [this](const BackendRegionConvertResult &r) {
            m_framesKnown = true;
            if (!r.valid) {
                setStatus(u"Positions in image pixels only (%1)."_s.arg(r.error), true);
                return;
            }
            populateFrames(!r.nativeFrame.isEmpty());
            if (r.nativeFrame.isEmpty())
                setStatus(u"The dataset has no celestial WCS: positions in image pixels only."_s, false);
            else
                setStatus(QString(), false);
            // Numbers typed in pixels while the frame was being read stay: the
            // frame changes only when nothing is being edited.
            if (m_dirty)
                return;
            if (m_preferredFrame.isEmpty() && !r.nativeFrame.isEmpty())
                m_preferredFrame = r.nativeFrame;
            const int i = m_frame->findData(static_cast<int>(frameChoiceFor(m_preferredFrame)));
            if (i >= 0) {
                const QSignalBlocker block(m_frame);
                m_frame->setCurrentIndex(i);
            }
            rebuildFields();
            showInCurrentFrame();
        });
    }
}

void RegionPropertiesDialog::convert(const QJsonArray &regions, const QString &toFrame,
                                     const QString &unit, Done done)
{
    QPointer<RegionPropertiesDialog> self(this);
    auto *watcher = new QFutureWatcher<BackendRegionConvertResult>(this);
    connect(watcher, &QFutureWatcher<BackendRegionConvertResult>::finished, this,
            [self, watcher, done]() {
                const BackendRegionConvertResult r = watcher->result();
                watcher->deleteLater();
                if (self)
                    done(r);
            });
    const Backend b = m_backend;
    watcher->setFuture(QtConcurrent::run([b, regions, toFrame, unit]() {
        BackendClient client(b.url, b.token);
        client.setSessionId(b.sessionId);
        return client.convertRegions(regions, toFrame, b.datasetId, /*origin=*/0, unit);
    }));
}

void RegionPropertiesDialog::populateFrames(bool hasCelestialWcs)
{
    const QSignalBlocker block(m_frame);
    const int was = m_frame->currentData().isValid() ? m_frame->currentData().toInt()
                                                     : static_cast<int>(FrameChoice::Pixels);
    m_frame->clear();
    for (FrameChoice c : availableFrames(hasCelestialWcs))
        m_frame->addItem(frameInfo(c).label, static_cast<int>(c));
    const int keep = m_frame->findData(was);
    m_frame->setCurrentIndex(keep >= 0 ? keep : 0);
}

FrameChoice RegionPropertiesDialog::currentFrame() const
{
    return m_frame->currentData().isValid() ? static_cast<FrameChoice>(m_frame->currentData().toInt())
                                            : FrameChoice::Pixels;
}

void RegionPropertiesDialog::setRegion(const Region &region)
{
    const bool other = region.id != m_region.id;
    m_region = region;
    setWindowTitle(u"Region Properties: %1"_s.arg(region.name));
    if (other) {
        m_dirty = false;
        if (!region.sourceFrame.isEmpty() && region.sourceFrame != u"image"_s)
            m_preferredFrame = region.sourceFrame;
        {
            const QSignalBlocker block(m_name);
            m_name->setText(region.name);
        }
        m_shapeLabel->setText(shapeLabel(region.shape));
        // Open in the frame the region was typed or read in, when offered.
        if (m_framesKnown && !m_preferredFrame.isEmpty()) {
            const int i = m_frame->findData(static_cast<int>(frameChoiceFor(m_preferredFrame)));
            if (i >= 0) {
                const QSignalBlocker block(m_frame);
                m_frame->setCurrentIndex(i);
            }
        }
        rebuildFields();
        showInCurrentFrame();
        return;
    }
    if (!m_dirty)
        scheduleRefresh();
}

void RegionPropertiesDialog::scheduleRefresh()
{
    m_refreshTimer->start();
}

void RegionPropertiesDialog::rebuildFields()
{
    while (m_fieldsForm->rowCount() > 0)
        m_fieldsForm->removeRow(0);
    m_lon = m_lat = nullptr;
    m_vertices = nullptr;
    m_angle = nullptr;
    m_sizes.clear();
    m_fieldsShape = static_cast<int>(m_region.shape);

    const FrameChoice frame = currentFrame();
    const bool sky = isSky(frame);
    const bool equatorial = frame != FrameChoice::GalacticDegrees;
    const QString lonName = !sky ? u"X"_s : equatorial ? u"RA"_s : u"l"_s;
    const QString latName = !sky ? u"Y"_s : equatorial ? u"Dec"_s : u"b"_s;
    const QString hint = !sky ? u"pixels, the first pixel is 1"_s
                       : frameInfo(frame).sexagesimal
                               ? (equatorial ? u"hh:mm:ss.s and ±dd:mm:ss, or degrees"_s
                                             : u"dd:mm:ss or degrees"_s)
                               : u"degrees"_s;

    if (isPointList(m_region.shape)) {
        m_vertices = new QTableWidget(0, 2, m_fieldsHost);
        m_vertices->setHorizontalHeaderLabels({ lonName, latName });
        m_vertices->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        m_vertices->setToolTip(u"Vertices (%1). Edit a cell, or add and remove rows."_s.arg(hint));
        m_vertices->setMinimumHeight(140);
        connect(m_vertices, &QTableWidget::itemChanged, this, [this]() {
            if (!m_filling)
                markEdited();
        });
        auto *rowButtons = new QWidget(m_fieldsHost);
        auto *rb = new QHBoxLayout(rowButtons);
        rb->setContentsMargins(0, 0, 0, 0);
        auto *add = new QPushButton(u"Add vertex"_s, rowButtons);
        auto *remove = new QPushButton(u"Remove vertex"_s, rowButtons);
        rb->addWidget(add);
        rb->addWidget(remove);
        rb->addStretch(1);
        connect(add, &QPushButton::clicked, this, [this]() {
            const int row = m_vertices->currentRow() >= 0 ? m_vertices->currentRow() + 1
                                                          : m_vertices->rowCount();
            m_vertices->insertRow(row);
            // Start from the vertex before, so the new one is somewhere sensible.
            for (int c = 0; c < 2; ++c) {
                const auto *src = m_vertices->item(std::max(0, row - 1), c);
                m_vertices->setItem(row, c, new QTableWidgetItem(src ? src->text() : QString()));
            }
            markEdited();
        });
        connect(remove, &QPushButton::clicked, this, [this]() {
            if (m_vertices->rowCount() <= minVertices(m_region.shape)) {
                setStatus(m_region.shape == Shape::Line ? u"A line keeps at least two vertices."_s
                                                        : u"A polygon keeps at least three vertices."_s,
                          true);
                return;
            }
            m_vertices->removeRow(m_vertices->currentRow() >= 0 ? m_vertices->currentRow()
                                                                : m_vertices->rowCount() - 1);
            markEdited();
        });
        m_fieldsForm->addRow(u"Vertices"_s, m_vertices);
        m_fieldsForm->addRow(QString(), rowButtons);
    } else {
        m_lon = new QLineEdit(m_fieldsHost);
        m_lat = new QLineEdit(m_fieldsHost);
        for (QLineEdit *e : { m_lon, m_lat }) {
            e->setToolTip(hint);
            e->setMinimumWidth(180);   // a sexagesimal position must fit whole
            connect(e, &QLineEdit::textEdited, this, &RegionPropertiesDialog::markEdited);
        }
        m_fieldsForm->addRow(u"Centre %1"_s.arg(lonName), m_lon);
        m_fieldsForm->addRow(u"Centre %1"_s.arg(latName), m_lat);
    }

    const QString unit = sky ? u" arcsec"_s : u" px"_s;
    for (const QString &label : sizeLabels(m_region.shape)) {
        auto *spin = new QDoubleSpinBox(m_fieldsHost);
        spin->setRange(0.0, 1e9);
        spin->setDecimals(3);
        spin->setSuffix(unit);
        spin->setKeyboardTracking(false);
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this]() {
            if (!m_filling)
                markEdited();
        });
        m_fieldsForm->addRow(label, spin);
        m_sizes.push_back(spin);
    }
    if (hasAngle(m_region.shape)) {
        m_angle = new QDoubleSpinBox(m_fieldsHost);
        m_angle->setRange(-360.0, 360.0);
        m_angle->setDecimals(3);
        m_angle->setSuffix(u"°"_s);
        m_angle->setKeyboardTracking(false);
        m_angle->setToolTip(sky ? u"Counter-clockwise from the longitude axis, as ds9 writes it."_s
                                : u"Counter-clockwise from the pixel x axis."_s);
        connect(m_angle, &QDoubleSpinBox::valueChanged, this, [this]() {
            if (!m_filling)
                markEdited();
        });
        m_fieldsForm->addRow(u"Angle"_s, m_angle);
    }
}

void RegionPropertiesDialog::showInCurrentFrame()
{
    if (m_region.id <= 0)
        return;
    if (m_fieldsShape != static_cast<int>(m_region.shape))
        rebuildFields();
    const FrameChoice frame = currentFrame();
    if (!isSky(frame)) {
        fillFields(fieldsFromCanonical(toCanonical(m_region, 1.0), frame));
        return;
    }
    const int generation = ++m_showGeneration;
    const int shownId = m_region.id;
    convert(QJsonArray{ toCanonical(m_region, 0.0) }, frameInfo(frame).backendFrame, u"arcsec"_s,
            [this, generation, frame, shownId](const BackendRegionConvertResult &r) {
                // Only the latest request, for the region and frame still shown,
                // and never over numbers the user is typing.
                if (generation != m_showGeneration || shownId != m_region.id
                    || frame != currentFrame() || m_dirty)
                    return;
                if (!r.valid || r.regions.isEmpty()) {
                    setStatus(u"Cannot show the region in this frame: %1"_s.arg(r.error), true);
                    return;
                }
                fillFields(fieldsFromCanonical(r.regions.first().toObject(), frame));
            });
}

void RegionPropertiesDialog::fillFields(const RegionFields &f)
{
    m_filling = true;
    if (m_lon && m_lat) {
        m_lon->setText(f.center[0]);
        m_lat->setText(f.center[1]);
    }
    if (m_vertices) {
        m_vertices->setRowCount(static_cast<int>(f.vertices.size()));
        for (int i = 0; i < static_cast<int>(f.vertices.size()); ++i) {
            for (int c = 0; c < 2; ++c)
                m_vertices->setItem(i, c, new QTableWidgetItem(f.vertices[static_cast<std::size_t>(i)][static_cast<std::size_t>(c)]));
        }
    }
    for (std::size_t i = 0; i < m_sizes.size() && i < f.sizes.size(); ++i)
        m_sizes[i]->setValue(f.sizes[i]);
    if (m_angle)
        m_angle->setValue(f.angleDeg);
    m_filling = false;
    // A good conversion clears the error of an earlier one; "Applied." stays.
    if (m_status->property("error").toBool())
        setStatus(QString(), false);
}

RegionFields RegionPropertiesDialog::readFields() const
{
    RegionFields f;
    f.shape = m_region.shape;
    if (m_lon && m_lat)
        f.center = { m_lon->text(), m_lat->text() };
    if (m_vertices) {
        for (int i = 0; i < m_vertices->rowCount(); ++i) {
            const auto *x = m_vertices->item(i, 0);
            const auto *y = m_vertices->item(i, 1);
            f.vertices.push_back({ x ? x->text() : QString(), y ? y->text() : QString() });
        }
    }
    for (const QDoubleSpinBox *s : m_sizes)
        f.sizes.push_back(s->value());
    if (m_angle)
        f.angleDeg = m_angle->value();
    return f;
}

void RegionPropertiesDialog::markEdited()
{
    m_dirty = true;
    m_refreshTimer->stop();
}

void RegionPropertiesDialog::setStatus(const QString &text, bool error)
{
    m_status->setText(text);
    m_status->setProperty("error", error);
    m_status->setStyleSheet(error ? u"color: #e5534b;"_s : QString());
}

void RegionPropertiesDialog::apply()
{
    if (m_region.id <= 0)
        return;
    // Spin boxes commit a typed value on focus loss; Apply must not race that.
    for (QDoubleSpinBox *s : m_sizes)
        s->interpretText();
    if (m_angle)
        m_angle->interpretText();

    const FrameChoice frame = currentFrame();
    QJsonObject typed;
    QString why;
    if (!canonicalFromFields(readFields(), frame, &typed, &why)) {
        setStatus(why, true);
        return;
    }
    if (!isSky(frame)) {
        Region edited;
        if (!fromCanonical(typed, 1.0, &edited, &why)) {
            setStatus(why, true);
            return;
        }
        edited.sourceFrame = u"image"_s;
        edited.sourceUnit = u"pix"_s;
        finishApply(edited);
        return;
    }
    const int generation = ++m_applyGeneration;
    const int editedId = m_region.id;
    m_applyButton->setEnabled(false);
    setStatus(u"Converting with the dataset's WCS…"_s, false);
    const QString frameName = frameInfo(frame).backendFrame;
    convert(QJsonArray{ typed }, u"image"_s, {},
            [this, generation, editedId, frameName](const BackendRegionConvertResult &r) {
                if (generation != m_applyGeneration)
                    return;
                m_applyButton->setEnabled(true);
                if (editedId != m_region.id)
                    return;   // another region was selected meanwhile
                Region edited;
                QString why;
                if (!r.valid || r.regions.isEmpty()) {
                    setStatus(u"Not applied: %1"_s.arg(r.error), true);
                    return;
                }
                if (!fromCanonical(r.regions.first().toObject(), 0.0, &edited, &why)) {
                    setStatus(u"Not applied: %1"_s.arg(why), true);
                    return;
                }
                edited.sourceFrame = frameName;
                edited.sourceUnit = u"arcsec"_s;
                finishApply(edited);
            });
}

void RegionPropertiesDialog::finishApply(Region edited)
{
    edited.id = m_region.id;
    edited.color = m_region.color;
    // What the numbers do not set: the file's properties, a text's string,
    // the line of the file it came from.
    edited.props = m_region.props;
    edited.text = m_region.text;
    edited.sourceLine = m_region.sourceLine;
    edited.batch = m_region.batch;
    edited.name = m_name->text().trimmed().isEmpty() ? m_region.name : m_name->text().trimmed();
    if (edited.name != m_region.name) {
        // The name is what is drawn: a text region's string, or the label a
        // region file gave (`text={…}`), which an export writes back.
        if (edited.shape == Shape::Text)
            edited.text = edited.name;
        if (edited.props.contains(u"text"_s))
            edited.props[u"text"_s] = edited.name;
    }
    m_dirty = false;
    setStatus(u"Applied."_s, false);
    emit regionEdited(edited);
}
