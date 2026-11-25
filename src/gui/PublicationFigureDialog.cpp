#include "PublicationFigureDialog.h"

#include "app/BackendClient.h"

#include <QCheckBox>
#include <QAbstractItemView>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

using namespace Qt::StringLiterals;

namespace visivo::figure {

namespace {
const QStringList kCorners{ u"bottom-left"_s, u"bottom-right"_s, u"top-left"_s, u"top-right"_s };
const QStringList kStretches{ u"linear"_s, u"sqrt"_s, u"squared"_s, u"log"_s,
                              u"asinh"_s,  u"sinh"_s, u"power"_s };

QComboBox *combo(const QStringList &items, const QString &current, QWidget *parent,
                 bool editable = false)
{
    auto *c = new QComboBox(parent);
    c->addItems(items);
    c->setEditable(editable);
    const int i = c->findText(current, Qt::MatchFixedString);
    if (i >= 0)
        c->setCurrentIndex(i);
    else if (editable)
        c->setCurrentText(current);
    return c;
}

QDoubleSpinBox *spin(double lo, double hi, double value, int decimals, QWidget *parent,
                     const QString &suffix = QString())
{
    auto *s = new QDoubleSpinBox(parent);
    s->setRange(lo, hi);
    s->setDecimals(decimals);
    s->setValue(value);
    s->setSuffix(suffix);
    // Keyboard tracking stays on: a size typed in the box reaches the preview as it
    // is typed (through the dialog's one debounce), not only after Tab.
    return s;
}
}   // namespace

struct PublicationFigureDialog::Widgets
{
    QComboBox *cmap, *stretch, *fontFamily, *frame, *tickFormat, *colorbarPosition, *beamPosition,
            *scaleBarUnit, *scaleBarPosition, *compassPosition, *format;
    QDoubleSpinBox *gamma, *percent, *vmin, *vmax, *titleSize, *labelSize, *tickSize,
            *colorbarLabelSize, *scaleBarLength, *widthIn, *heightIn;
    QSpinBox *dpi;
    QLineEdit *title, *xlabel, *ylabel, *colorbarLabel;
    QCheckBox *useRange, *grid, *colorbar, *beam, *scaleBar, *compass, *contours, *regions,
            *annotations, *crop;
};

PublicationFigureDialog::PublicationFigureDialog(const Context &context, QWidget *parent)
    : QDialog(parent), m_ctx(context), w(std::make_unique<Widgets>())
{
    if (!m_ctx.overlays)
        m_ctx.overlays = std::make_shared<Overlays>();
    if (m_ctx.colourMaps.isEmpty())
        m_ctx.colourMaps = QStringList{ m_ctx.defaults.cmap };
    setWindowTitle(u"Publication Figure"_s);
    build();
}

PublicationFigureDialog::~PublicationFigureDialog() = default;

void PublicationFigureDialog::build()
{
    const Options &d = m_ctx.defaults;
    auto *tabs = new QTabWidget(this);

    // ── Image ────────────────────────────────────────────────────────────────
    auto *image = new QWidget(tabs);
    auto *fi = new QFormLayout(image);
    w->cmap = new QComboBox(image);
    w->stretch = combo(kStretches, d.stretch, image);
    w->gamma = spin(0.1, 10.0, d.gamma, 2, image);
    w->gamma->setToolTip(u"Exponent of the power stretch."_s);
    w->useRange = new QCheckBox(u"Use this range (as in the window)"_s, image);
    w->useRange->setChecked(d.useRange);
    // Radio data are 1e-5 to 1e-3 of a unit: ten decimals, and a step that follows
    // the magnitude, so the window's range is not rounded away.
    w->vmin = spin(-1e12, 1e12, d.vmin, 10, image);
    w->vmax = spin(-1e12, 1e12, d.vmax, 10, image);
    for (QDoubleSpinBox *s : { w->vmin, w->vmax })
        s->setStepType(QAbstractSpinBox::AdaptiveDecimalStepType);
    w->percent = spin(50.0, 100.0, d.percent, 1, image, u" %"_s);
    w->percent->setToolTip(u"Without a range, the colours span this percentile of the pixels."_s);
    fi->addRow(u"Colour map"_s, w->cmap);
    // Under the list: that the choice is only close to the window's map, or that the
    // window's is not available. Said, not replaced silently.
    m_cmapNote = new QLabel(image);
    m_cmapNote->setWordWrap(true);
    m_cmapNote->hide();
    fi->addRow(QString(), m_cmapNote);
    fi->addRow(u"Stretch"_s, w->stretch);
    fi->addRow(u"Power exponent"_s, w->gamma);
    fi->addRow(QString(), w->useRange);
    fi->addRow(u"Minimum"_s, w->vmin);
    fi->addRow(u"Maximum"_s, w->vmax);
    fi->addRow(u"Clip at"_s, w->percent);
    tabs->addTab(image, u"Image"_s);

    // ── Text ─────────────────────────────────────────────────────────────────
    auto *text = new QWidget(tabs);
    auto *ft = new QFormLayout(text);
    w->title = new QLineEdit(d.title, text);
    w->xlabel = new QLineEdit(d.xlabel, text);
    w->ylabel = new QLineEdit(d.ylabel, text);
    w->xlabel->setPlaceholderText(u"Right Ascension"_s);
    w->ylabel->setPlaceholderText(u"Declination"_s);
    w->fontFamily = combo({ u"sans-serif"_s, u"serif"_s, u"monospace"_s }, d.fontFamily, text, true);
    w->fontFamily->setToolTip(u"A generic family, or one installed on the backend."_s);
    w->titleSize = spin(4, 72, d.titleSize, 0, text, u" pt"_s);
    w->labelSize = spin(4, 72, d.labelSize, 0, text, u" pt"_s);
    w->tickSize = spin(4, 72, d.tickSize, 0, text, u" pt"_s);
    w->colorbarLabelSize = spin(4, 72, d.colorbarLabelSize, 0, text, u" pt"_s);
    ft->addRow(u"Title"_s, w->title);
    ft->addRow(u"X axis title"_s, w->xlabel);
    ft->addRow(u"Y axis title"_s, w->ylabel);
    ft->addRow(u"Font"_s, w->fontFamily);
    ft->addRow(u"Title size"_s, w->titleSize);
    ft->addRow(u"Axis title size"_s, w->labelSize);
    ft->addRow(u"Tick label size"_s, w->tickSize);
    ft->addRow(u"Colour bar label size"_s, w->colorbarLabelSize);
    tabs->addTab(text, u"Text"_s);

    // ── Axes and colour bar ──────────────────────────────────────────────────
    auto *axes = new QWidget(tabs);
    auto *fa = new QFormLayout(axes);
    w->grid = new QCheckBox(u"WCS sky grid"_s, axes);
    w->grid->setChecked(d.grid);
    w->frame = combo({ u"native"_s, u"fk5"_s, u"icrs"_s, u"galactic"_s, u"ecliptic"_s }, d.frame,
                     axes);
    w->frame->setToolTip(u"The sky frame of the ticks and the grid; native is the image's own."_s);
    w->tickFormat = combo({ u"auto"_s, u"sexagesimal"_s, u"degrees"_s }, d.tickFormat, axes);
    w->colorbar = new QCheckBox(u"Colour bar"_s, axes);
    w->colorbar->setChecked(d.colorbar);
    w->colorbarPosition = combo({ u"right"_s, u"left"_s, u"top"_s, u"bottom"_s },
                                d.colorbarPosition, axes);
    w->colorbarLabel = new QLineEdit(d.colorbarLabel, axes);
    w->colorbarLabel->setPlaceholderText(u"BUNIT"_s);
    fa->addRow(QString(), w->grid);
    fa->addRow(u"Coordinate frame"_s, w->frame);
    fa->addRow(u"Tick format"_s, w->tickFormat);
    fa->addRow(QString(), w->colorbar);
    fa->addRow(u"Colour bar position"_s, w->colorbarPosition);
    fa->addRow(u"Colour bar label"_s, w->colorbarLabel);
    tabs->addTab(axes, u"Axes"_s);

    // ── Overlays ─────────────────────────────────────────────────────────────
    auto *over = new QWidget(tabs);
    auto *fo = new QFormLayout(over);
    w->beam = new QCheckBox(u"Beam (from the header)"_s, over);
    w->beam->setChecked(d.beam);
    w->beamPosition = combo(kCorners, d.beamPosition, over);
    w->scaleBar = new QCheckBox(u"Scale bar (angular)"_s, over);
    w->scaleBar->setChecked(d.scaleBar);
    w->scaleBarLength = spin(0.0, 1e6, d.scaleBarLength, 2, over);
    w->scaleBarLength->setSpecialValueText(u"round length"_s);
    w->scaleBarLength->setToolTip(u"Pick a unit to type a length; with auto it is a round one."_s);
    w->scaleBarUnit = combo({ u"auto"_s, u"arcsec"_s, u"arcmin"_s, u"deg"_s }, d.scaleBarUnit, over);
    w->scaleBarUnit->setToolTip(u"auto: a round length, in arcsec below 60\", arcmin below 60', "
                                u"else degrees. Pick a unit to type a length."_s);
    w->scaleBarPosition = combo(kCorners, d.scaleBarPosition, over);
    w->compass = new QCheckBox(u"Compass (north and east)"_s, over);
    w->compass->setChecked(d.compass);
    w->compassPosition = combo(kCorners, d.compassPosition, over);
    const auto &ov = *m_ctx.overlays;
    // The image's own contours and every external set the window shows are drawn
    // together; the "Contours of" box in the window only picks which set its panel edits.
    const auto nSets = static_cast<int>(ov.contourSources.size());
    w->contours = new QCheckBox(nSets > 1 ? u"Contours of the window (%1 sets)"_s.arg(nSets)
                                          : u"Contours of the window"_s,
                                over);
    w->contours->setChecked(d.contours && ov.hasContours());
    w->contours->setEnabled(ov.hasContours());
    w->contours->setToolTip(ov.hasContours()
                                    ? u"The image's own and the external contours, all together, as drawn."_s
                                    : u"Show contours in the window first."_s);
    w->regions = new QCheckBox(u"Regions (%1)"_s.arg(ov.regions.size()), over);
    w->regions->setChecked(d.regions && !ov.regions.isEmpty());
    w->regions->setEnabled(!ov.regions.isEmpty());
    w->annotations = new QCheckBox(u"Text and arrow annotations (%1)"_s.arg(ov.annotations.size()),
                                   over);
    w->annotations->setChecked(d.annotations && !ov.annotations.isEmpty());
    w->annotations->setEnabled(!ov.annotations.isEmpty());
    fo->addRow(w->beam, w->beamPosition);
    fo->addRow(w->scaleBar, w->scaleBarPosition);
    fo->addRow(u"   length"_s, w->scaleBarLength);
    fo->addRow(u"   unit"_s, w->scaleBarUnit);
    fo->addRow(w->compass, w->compassPosition);
    fo->addRow(w->contours);
    fo->addRow(w->regions);
    fo->addRow(w->annotations);
    tabs->addTab(over, u"Overlays"_s);

    // ── Output ───────────────────────────────────────────────────────────────
    auto *out = new QWidget(tabs);
    auto *fx = new QFormLayout(out);
    QStringList fmts;
    for (const FormatInfo &f : formats())
        fmts << QString::fromLatin1(f.key);
    w->format = combo(fmts, d.format, out);
    w->widthIn = spin(1, 40, d.widthIn, 1, out, u" in"_s);
    w->heightIn = spin(1, 40, d.heightIn, 1, out, u" in"_s);
    w->dpi = new QSpinBox(out);
    w->dpi->setRange(30, 1200);
    w->dpi->setValue(d.dpi);
    w->crop = new QCheckBox(u"Trim the white margin"_s, out);
    w->crop->setChecked(d.crop);
    fx->addRow(u"Format"_s, w->format);
    fx->addRow(u"Width"_s, w->widthIn);
    fx->addRow(u"Height"_s, w->heightIn);
    fx->addRow(u"Resolution"_s, w->dpi);
    fx->addRow(QString(), w->crop);
    tabs->addTab(out, u"Output"_s);

    // ── Preview ──────────────────────────────────────────────────────────────
    m_preview = new QLabel(this);
    m_preview->setMinimumSize(520, 440);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setFrameShape(QFrame::StyledPanel);
    m_preview->setText(u"Preview…"_s);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setMaximumWidth(560);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(u"Render"_s);
    // Return must not be a shortcut for Render: it exports a file, and a Return
    // meant for the colour-map list (or any editor) reached the button too. Both
    // buttons are only clicked; QDialog would otherwise make the first one default.
    for (auto b : { QDialogButtonBox::Ok, QDialogButtonBox::Cancel }) {
        buttons->button(b)->setAutoDefault(false);
        buttons->button(b)->setDefault(false);
    }
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *right = new QVBoxLayout;
    right->addWidget(m_preview, 1);
    right->addWidget(m_status);
    auto *top = new QHBoxLayout;
    top->addWidget(tabs, 0);
    top->addLayout(right, 1);
    auto *root = new QVBoxLayout(this);
    root->addLayout(top);
    root->addWidget(buttons);

    // One debounce for every editor: a preview a moment after the last edit.
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(450);
    connect(m_debounce, &QTimer::timeout, this, &PublicationFigureDialog::runPreview);
    const auto edited = [this] { schedulePreview(); };
    for (QComboBox *c : { w->stretch, w->fontFamily, w->frame, w->tickFormat,
                          w->colorbarPosition, w->beamPosition, w->scaleBarUnit,
                          w->scaleBarPosition, w->compassPosition, w->format })
        connect(c, &QComboBox::currentTextChanged, this, edited);
    for (QDoubleSpinBox *s : { w->gamma, w->percent, w->vmin, w->vmax, w->titleSize, w->labelSize,
                               w->tickSize, w->colorbarLabelSize, w->scaleBarLength, w->widthIn,
                               w->heightIn })
        connect(s, &QDoubleSpinBox::valueChanged, this, edited);
    connect(w->dpi, &QSpinBox::valueChanged, this, edited);
    connect(w->cmap, &QComboBox::currentIndexChanged, this, [this] {
        updateColourMapNote();
        schedulePreview();
    });
    // A length is typed in a unit that is picked: with auto it would be read in a unit
    // the user cannot see, so it is for an explicit unit only (Spec.h).
    const auto lengthRule = [this] {
        const bool typed = scaleBarLengthEnabled(w->scaleBarUnit->currentText());
        w->scaleBarLength->setEnabled(typed);
        if (!typed)
            w->scaleBarLength->setValue(0.0);
    };
    connect(w->scaleBarUnit, &QComboBox::currentTextChanged, this, lengthRule);
    lengthRule();
    // The whole registry now, narrowed to what the backend can set when it answers.
    fillColourMaps(BackendFigureColourMaps{});
    requestColourMaps();
    for (QLineEdit *e : { w->title, w->xlabel, w->ylabel, w->colorbarLabel })
        connect(e, &QLineEdit::textChanged, this, edited);
    for (QCheckBox *c : { w->useRange, w->grid, w->colorbar, w->beam, w->scaleBar, w->compass,
                          w->contours, w->regions, w->annotations, w->crop })
        connect(c, &QCheckBox::toggled, this, edited);
    // The range only matters when it is used.
    const auto range = [this] {
        w->vmin->setEnabled(w->useRange->isChecked());
        w->vmax->setEnabled(w->useRange->isChecked());
        w->percent->setEnabled(!w->useRange->isChecked());
    };
    connect(w->useRange, &QCheckBox::toggled, this, range);
    range();
    schedulePreview();
}

Options PublicationFigureDialog::options() const
{
    Options o;
    const int cm = w->cmap->currentIndex() >= 0 ? w->cmap->currentData().toInt() : -1;
    if (cm >= 0 && cm < m_choices.size()) {
        o.cmap = m_choices[cm].name;
        // The window's own table is sent as colours: exact whatever the map is.
        if (m_choices[cm].kind == ColourMapChoice::Kind::Window)
            o.lut = m_ctx.windowLut;
    } else {
        o.cmap = m_ctx.defaults.cmap;
    }
    o.stretch = w->stretch->currentText();
    o.gamma = w->gamma->value();
    o.percent = w->percent->value();
    o.useRange = w->useRange->isChecked();
    o.vmin = w->vmin->value();
    o.vmax = w->vmax->value();
    o.title = w->title->text();
    o.xlabel = w->xlabel->text();
    o.ylabel = w->ylabel->text();
    o.fontFamily = w->fontFamily->currentText().trimmed().isEmpty()
            ? u"sans-serif"_s
            : w->fontFamily->currentText().trimmed();
    o.titleSize = w->titleSize->value();
    o.labelSize = w->labelSize->value();
    o.tickSize = w->tickSize->value();
    o.colorbarLabelSize = w->colorbarLabelSize->value();
    o.grid = w->grid->isChecked();
    o.frame = w->frame->currentText();
    o.tickFormat = w->tickFormat->currentText();
    o.colorbar = w->colorbar->isChecked();
    o.colorbarPosition = w->colorbarPosition->currentText();
    o.colorbarLabel = w->colorbarLabel->text();
    o.beam = w->beam->isChecked();
    o.beamPosition = w->beamPosition->currentText();
    o.scaleBar = w->scaleBar->isChecked();
    o.scaleBarLength = w->scaleBarLength->value();
    o.scaleBarUnit = w->scaleBarUnit->currentText();
    o.scaleBarPosition = w->scaleBarPosition->currentText();
    o.compass = w->compass->isChecked();
    o.compassPosition = w->compassPosition->currentText();
    o.contours = w->contours->isChecked();
    o.regions = w->regions->isChecked();
    o.annotations = w->annotations->isChecked();
    o.widthIn = w->widthIn->value();
    o.heightIn = w->heightIn->value();
    o.dpi = w->dpi->value();
    o.crop = w->crop->isChecked();
    o.format = w->format->currentText();
    return o;
}

std::function<QJsonObject(bool)> PublicationFigureDialog::bodyMaker() const
{
    // The options as they are now, and the overlays by pointer: the lines are
    // joined by whichever thread asks first.
    const Options o = options();
    const auto overlays = m_ctx.overlays;
    const QString dataset = m_ctx.datasetId, stem = m_ctx.fileStem;
    return [o, overlays, dataset, stem](bool preview) {
        return requestBody(dataset, o, *overlays, preview, stem);
    };
}

void PublicationFigureDialog::schedulePreview()
{
    m_debounce->start();
}

void PublicationFigureDialog::runPreview()
{
    // One request at a time; an edit made meanwhile asks again when it is back,
    // so the picture shown is always of the newest options.
    // A preview of a dialog that has closed would be a request nobody reads.
    if (!isVisible())
        return;
    // While a range is being typed the minimum may pass the maximum for a moment
    // (the preview follows the keys): that is not asked of the backend, the last
    // picture stays and the status says what is waited for.
    if (w->useRange->isChecked() && w->vmin->value() >= w->vmax->value()) {
        m_status->setText(u"The minimum must be below the maximum."_s);
        // A request still out on the old options must not repaint over this: it
        // finishes as stale (dirty), and the retry it triggers comes back here.
        if (m_inFlight)
            m_dirty = true;
        return;
    }
    if (m_inFlight) {
        m_dirty = true;
        return;
    }
    m_inFlight = true;
    m_dirty = false;
    const int seq = ++m_seq;
    m_status->setText(u"Rendering the preview…"_s);
    const auto make = bodyMaker();
    const QString url = m_ctx.backendUrl, token = m_ctx.token, session = m_ctx.sessionId;
    auto *watcher = new QFutureWatcher<BackendPublicationFigureResult>(this);
    connect(watcher, &QFutureWatcher<BackendPublicationFigureResult>::finished, this,
            [this, watcher, seq] {
                watcher->deleteLater();
                m_inFlight = false;
                const auto r = watcher->result();
                if (seq == m_seq && !m_dirty) {
                    QPixmap pm;
                    if (r.valid && pm.loadFromData(r.png, "PNG")) {
                        showPreview(pm, r.notes.join(u'\n'));
                    } else {
                        m_preview->setPixmap(QPixmap());
                        m_preview->setText(u"No preview"_s);
                        m_status->setText(r.error.isEmpty() ? u"The backend gave no picture."_s
                                                            : r.error);
                    }
                }
                if (m_dirty)
                    runPreview();
            });
    watcher->setFuture(QtConcurrent::run([make, url, token, session]() {
        BackendClient bc(url, token);
        bc.setSessionId(session);
        return bc.publicationFigure(make(true));
    }));
}

namespace {
/// One answer per backend and session. Read and written on the GUI thread only.
QHash<QString, BackendFigureColourMaps> &colourMapAnswers()
{
    static QHash<QString, BackendFigureColourMaps> answers;
    return answers;
}
}   // namespace

void PublicationFigureDialog::requestColourMaps()
{
    // Per backend and session: a backend restarted at the same address starts a
    // new session and is asked again.
    const QString key = m_ctx.backendUrl + u'|' + m_ctx.sessionId;
    const auto cached = colourMapAnswers().constFind(key);
    if (cached != colourMapAnswers().constEnd()) {
        fillColourMaps(*cached);
        return;
    }
    // A hung or remote backend must not hold the dialog: the registry is offered at
    // once and the answer, when it comes, narrows it (the choice is kept).
    const QString url = m_ctx.backendUrl, token = m_ctx.token, session = m_ctx.sessionId;
    const QStringList registry = m_ctx.colourMaps;
    auto *watcher = new QFutureWatcher<BackendFigureColourMaps>(this);
    connect(watcher, &QFutureWatcher<BackendFigureColourMaps>::finished, this, [this, watcher, key] {
        watcher->deleteLater();
        const auto r = watcher->result();
        if (!r.valid) {
            // An older backend, or none: the whole registry stays, but the window's
            // table is not offered, since such a backend would drop it silently.
            m_colourMapsFailed = true;
            fillColourMaps(r);
            return;
        }
        colourMapAnswers().insert(key, r);
        fillColourMaps(r);
    });
    watcher->setFuture(QtConcurrent::run([url, token, session, registry]() {
        BackendClient bc(url, token);
        bc.setSessionId(session);
        return bc.publicationFigureColourMaps(registry);
    }));
}

void PublicationFigureDialog::fillColourMaps(const BackendFigureColourMaps &answer)
{
    using Kind = ColourMapChoice::Kind;
    // The backend's answer lands about when the user reaches for the list. Clearing
    // and refilling a combo under its open popup closes it and loses the click (the
    // first click had to be repeated), so the refill waits for the popup to close.
    if (w->cmap->view() && w->cmap->view()->isVisible()) {
        m_pendingColourMaps = answer;
        QTimer::singleShot(150, this, [this] {
            if (!m_pendingColourMaps)
                return;
            const BackendFigureColourMaps later = *m_pendingColourMaps;
            m_pendingColourMaps.reset();
            fillColourMaps(later);
        });
        return;
    }
    m_pendingColourMaps.reset();
    // What is chosen now, to keep it across the new list; the first time, the window's.
    const int before = w->cmap->currentData().toInt();
    const bool first = m_choices.isEmpty();
    const ColourMapChoice kept = (w->cmap->currentIndex() >= 0 && before >= 0 && before < m_choices.size()) ? m_choices[before]
                                                                            : ColourMapChoice{};
    const QList<ColourMapChoice> choices = colourMapChoices(
            m_ctx.colourMaps, answer, m_ctx.windowColourMap,
            !m_ctx.windowLut.isEmpty() && !m_colourMapsFailed);
    // The answer says what the list says already: leave the combo as it is.
    if (!first && sameColourMapChoices(choices, m_choices)) {
        updateColourMapNote();
        return;
    }
    m_choices = choices;
    const auto find = [this](const std::function<bool(const ColourMapChoice &)> &want) {
        for (int i = 0; i < m_choices.size(); ++i)
            if (want(m_choices[i]))
                return i;
        return -1;
    };
    int pick = -1;
    if (!first) {
        pick = find([&](const ColourMapChoice &c) { return c.kind == kept.kind && c.name == kept.name; });
        // The window's table dropped because the backend draws that map exactly by
        // name: the registry's entry of the same name stands in for it.
        if (pick < 0 && kept.kind == Kind::Window)
            pick = find([&](const ColourMapChoice &c) {
                return c.name.compare(kept.name, Qt::CaseInsensitive) == 0;
            });
    } else if (!m_ctx.windowColourMap.isEmpty()) {
        pick = find([](const ColourMapChoice &c) { return c.kind == Kind::Window; });
        if (pick < 0)
            pick = find([&](const ColourMapChoice &c) {
                return c.name.compare(m_ctx.windowColourMap, Qt::CaseInsensitive) == 0;
            });
    } else {
        pick = find([&](const ColourMapChoice &c) {
            return c.name.compare(m_ctx.defaults.cmap, Qt::CaseInsensitive) == 0;
        });
    }
    if (pick < 0)
        pick = find([](const ColourMapChoice &c) { return c.name == QLatin1String("Inferno"); });
    if (pick < 0 && !m_choices.isEmpty())
        pick = 0;

    const QSignalBlocker block(w->cmap);
    w->cmap->clear();
    bool separated = false;
    for (int i = 0; i < m_choices.size(); ++i) {
        if (m_choices[i].kind == Kind::Extra && !separated) {
            w->cmap->insertSeparator(w->cmap->count());
            separated = true;
        }
        w->cmap->addItem(m_choices[i].label, i);   // the data is the index into m_choices
    }
    w->cmap->setCurrentIndex(w->cmap->findData(pick));
    updateColourMapNote();
    // The list changed under a choice that is no longer there: the preview follows.
    if (!first && (pick < 0 || kept.kind != m_choices[pick].kind || kept.name != m_choices[pick].name))
        schedulePreview();
}

void PublicationFigureDialog::updateColourMapNote()
{
    QString text;
    const int idx = w->cmap->currentIndex() >= 0 ? w->cmap->currentData().toInt() : -1;
    if (idx >= 0 && idx < m_choices.size())
        text = approximationNote(m_choices[idx]);
    // The window's map when the figure cannot draw it by name or by colours.
    const bool windowShown = idx >= 0 && idx < m_choices.size() && (m_choices[idx].kind == ColourMapChoice::Kind::Window
                                          || m_choices[idx].name.compare(
                                                     m_ctx.windowColourMap, Qt::CaseInsensitive) == 0);
    if (text.isEmpty() && !m_ctx.windowColourMap.isEmpty() && !windowShown && !m_choices.isEmpty()
        && m_ctx.windowLut.isEmpty()) {
        bool listed = false;
        for (const auto &c : std::as_const(m_choices))
            listed = listed || c.name.compare(m_ctx.windowColourMap, Qt::CaseInsensitive) == 0;
        if (!listed)
            text = u"The window's colour map (%1) is not available in figures."_s.arg(
                    m_ctx.windowColourMap);
    }
    m_cmapNote->setText(text);
    m_cmapNote->setVisible(!text.isEmpty());
}

void PublicationFigureDialog::showPreview(const QPixmap &pm, const QString &message)
{
    m_preview->setText(QString());
    m_preview->setPixmap(pm.scaled(m_preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    m_status->setText(message);
}

}   // namespace visivo::figure
