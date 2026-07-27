#include "VisivoTheme.h"

#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QStyleFactory>
#include <QSvgRenderer>
#include <QPainter>
#include <QToolButton>
#include <QWidget>

using namespace Qt::StringLiterals;

namespace VisivoTheme {

// ── Theme mode (set by applyDarkTo / applyLightTo, read by inline accessors) ─
namespace {
Mode g_mode = Mode::Dark;
}

void setMode(Mode m) { g_mode = m; }
Mode currentMode()   { return g_mode; }

// ── Font loading ──────────────────────────────────────────────────────────────

bool loadBundledFonts()
{
    bool ok = true;

    const struct { const char *path; const char *label; } kFonts[] = {
        { ":/fonts/Lato-Light.ttf",   "Lato Light"   },
        { ":/fonts/Lato-Regular.ttf", "Lato Regular" },
        { ":/fonts/Lato-Bold.ttf",    "Lato Bold"    },
        { ":/fonts/Lato-Black.ttf",   "Lato Black"   },
    };

    for (const auto &f : kFonts) {
        const int id = QFontDatabase::addApplicationFont(QLatin1StringView(f.path));
        if (id < 0) {
            qWarning("[VisivoTheme] failed to load bundled font: %s", f.label);
            ok = false;
        }
    }

    return ok;
}

// ── Component factories ───────────────────────────────────────────────────────

QLabel *makeSectionLabel(const QString &text, QWidget *parent)
{
    auto *lbl = new QLabel(text.toUpper(), parent);
    lbl->setStyleSheet(
            QStringLiteral(
                    "QLabel {"
                    "  color: %1;"
                    "  font-family: %2;"
                    "  font-size: 10px;"
                    "  font-weight: 700;"
                    "  letter-spacing: 0.18em;"
                    "  background-color: transparent;"
                    "}")
                    .arg(QLatin1StringView(kOnSurfaceVariant()),
                         QLatin1StringView(kBrandFamily)));
    return lbl;
}

QFrame *makeCard(QWidget *parent, bool elevated)
{
    auto *card = new QFrame(parent);
    // Scope the border + background via object-name selector so the rule
    // applies ONLY to this QFrame and not to any QFrame-derived child
    // (QLabel inherits QFrame; without the # selector every label inside
    // the card would inherit the border, producing the boxed-in look the
    // user spotted in the Data Hub).
    const QString name = QStringLiteral("themeCard%1").arg(quintptr(card), 0, 16);
    card->setObjectName(name);
    const QLatin1StringView bg(elevated ? kSurfaceContainerHighest() : kSurfaceContainerHigh());
    card->setStyleSheet(
            QStringLiteral(
                    "QFrame#%1 {"
                    "  background-color: %2;"
                    "  border-radius: 8px;"
                    "  border: 1px solid %3;"
                    "}")
                    .arg(name, bg, QLatin1StringView(kOutline())));
    return card;
}

QLabel *makeBadge(const QString &text, BadgeKind kind, QWidget *parent)
{
    auto *lbl = new QLabel(text, parent);
    QLatin1StringView bg, fg;
    switch (kind) {
    case BadgeKind::Success:
        bg = QLatin1StringView(kTertiaryContainer());
        fg = QLatin1StringView(kOnTertiaryFixed());
        break;
    case BadgeKind::Error:
        bg = QLatin1StringView(kErrorContainer());
        fg = QLatin1StringView(kOnErrorContainer());
        break;
    case BadgeKind::Warning:
        bg = QLatin1StringView(kWarningContainer());
        fg = QLatin1StringView(kOnWarningContainer());
        break;
    default:
        bg = QLatin1StringView(kSurfaceContainerHighest());
        fg = QLatin1StringView(kOnSurfaceVariant());
        break;
    }
    lbl->setStyleSheet(
            QStringLiteral(
                    "QLabel {"
                    "  background-color: %1;"
                    "  color: %2;"
                    "  font-family: %3;"
                    "  font-size: 10px;"
                    "  font-weight: 700;"
                    "  letter-spacing: 0.12em;"
                    "  padding: 3px 10px;"
                    "  border-radius: 10px;"
                    "  border: none;"
                    "}")
                    .arg(bg, fg, QLatin1StringView(kBrandFamily)));
    return lbl;
}

QPushButton *makePrimaryButton(const QString &text, QWidget *parent)
{
    auto *btn = new QPushButton(text, parent);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setStyleSheet(
            QStringLiteral(
                    "QPushButton {"
                    "  background-color: %1;"
                    "  color: %2;"
                    "  font-family: %3;"
                    "  font-size: 13px;"
                    "  font-weight: 700;"
                    "  padding: 10px 24px;"
                    "  border-radius: 6px;"
                    "  border: none;"
                    "}"
                    "QPushButton:hover {"
                    "  background-color: %4;"
                    "}"
                    "QPushButton:pressed {"
                    "  background-color: %5;"
                    "}"
                    "QPushButton:disabled {"
                    "  background-color: rgba(4, 138, 191, 0.25);"
                    "  color: rgba(255,255,255, 0.45);"
                    "}")
                    .arg(QLatin1StringView(kPrimary()),
                         QLatin1StringView(kOnPrimary()),
                         QLatin1StringView(kBrandFamily),
                         QLatin1StringView(kPrimaryDim()),
                         QLatin1StringView(kPrimaryFixed())));
    return btn;
}

QPushButton *makeSecondaryButton(const QString &text, QWidget *parent)
{
    auto *btn = new QPushButton(text, parent);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setStyleSheet(
            QStringLiteral(
                    "QPushButton {"
                    "  background-color: %1;"
                    "  color: %2;"
                    "  font-family: %3;"
                    "  font-size: 12px;"
                    "  font-weight: 600;"
                    "  padding: 8px 18px;"
                    "  border-radius: 6px;"
                    "  border: 1px solid rgba(4, 138, 191, 0.38);"
                    "}"
                    "QPushButton:hover {"
                    "  background-color: %4;"
                    "  border-color: %5;"
                    "  color: %2;"
                    "}"
                    "QPushButton:pressed {"
                    "  background-color: %6;"
                    "}")
                    .arg(QLatin1StringView(kSurfaceContainerHigh()),
                         QLatin1StringView(kPrimary()),
                         QLatin1StringView(kBrandFamily),
                         QLatin1StringView(kSurfaceContainerHighest()),
                         QLatin1StringView(kPrimary()),
                         QLatin1StringView(kSurfaceBright())));
    return btn;
}

// ── Stylesheet helpers (theme-aware) ──────────────────────────────────────────

QString sidebarHeaderStyleSheet()
{
    return QStringLiteral(
                   "QLabel {"
                   "  color: %1;"
                   "  font-size: 10px;"
                   "  font-weight: 600;"
                   "  padding-bottom: 4px;"
                   "  border-bottom: 1px solid rgba(4, 138, 191, 0.20);"
                   "}")
            .arg(QLatin1StringView(kOnSurfaceVariant()));
}

QString sidebarSubHeaderStyleSheet()
{
    return QStringLiteral("QLabel { color: %1; font-size: 9px; font-weight: 600; }")
            .arg(QLatin1StringView(kOnSurfaceVariant()));
}

QString sidebarCardStyleSheet()
{
    // Idle card tint must read on the surrounding surface in either theme.
    const QLatin1StringView tint = isDark()
            ? "rgba(255, 255, 255, 0.03)"_L1
            : "rgba(0, 0, 0, 0.03)"_L1;
    // The QFrame/QGroupBox rule applies to the styled widget AND every
    // QFrame-derived descendant (QLabel inherits QFrame). Without an
    // explicit counter-rule, every child label inherits the 1-px border —
    // visible as boxed-in section headers ("RENDERING MODE", "COLOR MAP", …)
    // in viewer sidebars. Neutralise borders + tinted backgrounds on
    // labels and tool buttons so only the outer card frame keeps the
    // border and the inner controls render flat.
    return QStringLiteral(
                   "QFrame, QGroupBox {"
                   "  background-color: %1;"
                   "  border: 1px solid rgba(4, 138, 191, 0.14);"
                   "  border-radius: 6px;"
                   "}"
                   "QLabel {"
                   "  background: transparent;"
                   "  border: none;"
                   "  border-radius: 0;"
                   "}"
                   "QToolButton {"
                   "  background: transparent;"
                   "  border: 1px solid transparent;"
                   "}")
            .arg(tint);
}

QString infoKeyStyleSheet()
{
    return QStringLiteral("QLabel { color: %1; font-size: 10px; }")
            .arg(QLatin1StringView(kOnSurfaceVariant()));
}

QString infoValueStyleSheet()
{
    return QStringLiteral(
                   "QLabel { color: %1; font-size: 10px; font-family: monospace; }")
            .arg(QLatin1StringView(kOnSurface()));
}

QString sidebarToolButtonStyleSheet()
{
    // Brand-blue alpha overlays for hover/checked — readable on either theme.
    return QStringLiteral(
            "QToolButton { text-align: left; padding: 4px 8px; border-radius: 4px;"
            "              border: 1px solid transparent; }"
            "QToolButton:hover { background-color: rgba(4,138,191,0.12);"
            "                    border: 1px solid rgba(4,138,191,0.25); }"
            "QToolButton:checked { background-color: rgba(4,138,191,0.20);"
            "                      border: 1px solid rgba(4,138,191,0.40); }");
}

namespace {

// Helpers used by makeViewerToolbar — kept local to this TU.

QToolButton *makeToolbarButton(const QString &text, bool primary, QWidget *parent)
{
    auto *btn = new QToolButton(parent);
    btn->setText(text);
    btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    btn->setCursor(Qt::PointingHandCursor);
    const QLatin1StringView bg(primary ? kSurfaceContainerHighest() : kSurfaceContainerHigh());
    const QLatin1StringView fg(primary ? kPrimary() : kOnSurface());
    btn->setStyleSheet(
            QStringLiteral(
                    "QToolButton {"
                    "  background-color: %1;"
                    "  color: %2;"
                    "  border: 1px solid %3;"
                    "  border-radius: 6px;"
                    "  padding: 4px 12px;"
                    "  font-size: 11px;"
                    "  font-weight: 600;"
                    "}"
                    "QToolButton:hover {"
                    "  border-color: %4;"
                    "}")
                    .arg(bg, fg,
                         QLatin1StringView(kOutline()),
                         QLatin1StringView(kPrimary())));
    return btn;
}

QLabel *makeTagChip(const QString &text, bool accent, QWidget *parent)
{
    auto *lbl = new QLabel(text, parent);
    const QLatin1StringView bg = accent
            ? QLatin1StringView(kSurfaceContainerHigh())
            : QLatin1StringView(kSurfaceContainerLow());
    const QLatin1StringView fg = accent
            ? QLatin1StringView(kPrimary())
            : QLatin1StringView(kOnSurfaceVariant());
    lbl->setStyleSheet(
            QStringLiteral(
                    "QLabel {"
                    "  background-color: %1; color: %2;"
                    "  font-family: 'JetBrains Mono', 'SF Mono', Menlo, monospace;"
                    "  font-size: 10px; font-weight: 600;"
                    "  padding: 3px 8px; border-radius: 4px;"
                    "  border: 1px solid %3;"
                    "}")
                    .arg(bg, fg, QLatin1StringView(kOutline())));
    return lbl;
}

// Same visual as makeTagChip but as a clickable QToolButton so callers can
// connect QAbstractButton::clicked. Used for the primary "CUBE · …" tag in
// the viewer toolbar that opens the FITS header dialog on click.
QToolButton *makeTagButton(const QString &text, bool accent, QWidget *parent)
{
    auto *btn = new QToolButton(parent);
    btn->setText(text);
    btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    btn->setCursor(Qt::PointingHandCursor);
    const QLatin1StringView bg = accent
            ? QLatin1StringView(kSurfaceContainerHigh())
            : QLatin1StringView(kSurfaceContainerLow());
    const QLatin1StringView fg = accent
            ? QLatin1StringView(kPrimary())
            : QLatin1StringView(kOnSurfaceVariant());
    btn->setStyleSheet(
            QStringLiteral(
                    "QToolButton {"
                    "  background-color: %1; color: %2;"
                    "  font-family: 'JetBrains Mono', 'SF Mono', Menlo, monospace;"
                    "  font-size: 10px; font-weight: 600;"
                    "  padding: 3px 8px; border-radius: 4px;"
                    "  border: 1px solid %3;"
                    "}"
                    "QToolButton:hover {"
                    "  border-color: %2;"
                    "}")
                    .arg(bg, fg, QLatin1StringView(kOutline())));
    return btn;
}

} // namespace

QPixmap renderThemeIcon(const QString &resourcePath, int size,
                        const QColor *colorOverride)
{
    // Load the SVG text from the Qt resource bundle, swap `currentColor`
    // tokens for the active theme's hex, then render via QSvgRenderer. We
    // do the substitution at the byte-array level — QSvgRenderer doesn't
    // resolve `currentColor` itself, and adding a separate stylesheet to
    // the SVG's CSS scope is brittle across versions.
    QFile f(resourcePath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QByteArray svg = f.readAll();

    const QColor color = colorOverride && colorOverride->isValid()
            ? *colorOverride
            : QColor(QString::fromLatin1(kOnSurfaceVariant()));
    const QByteArray hex = color.name(QColor::HexRgb).toLatin1();
    svg.replace("currentColor", hex);

    QSvgRenderer renderer(svg);
    if (!renderer.isValid()) return {};

    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    renderer.render(&p, QRectF(0, 0, size, size));
    return pm;
}

QLabel *makeBreadcrumbTag(const QString &text, QWidget *parent)
{
    auto *lbl = new QLabel(text, parent);
    lbl->setStyleSheet(
            QStringLiteral(
                    "QLabel {"
                    "  background-color: %1;"
                    "  color: %2;"
                    "  font-family: 'JetBrains Mono', 'SF Mono', Menlo, monospace;"
                    "  font-size: 10px; font-weight: 600;"
                    "  padding: 3px 8px; border-radius: 4px;"
                    "  border: 1px solid %3;"
                    "}")
                    .arg(QLatin1StringView(kSurfaceContainerLow()),
                         QLatin1StringView(kOnSurfaceVariant()),
                         QLatin1StringView(kOutline())));
    return lbl;
}

namespace {

// A compact square glyph button (▼ switcher, ◐ theme toggle) matching the
// toolbar's neutral chrome.
QToolButton *makeGlyphButton(const QString &glyph, QWidget *parent)
{
    auto *btn = new QToolButton(parent);
    btn->setText(glyph);
    btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setStyleSheet(
            QStringLiteral(
                    "QToolButton {"
                    "  background-color: %1; color: %2;"
                    "  border: 1px solid %3; border-radius: 6px;"
                    "  padding: 3px 7px; font-size: 11px; font-weight: 600;"
                    "}"
                    "QToolButton:hover { border-color: %4; color: %4; }"
                    "QToolButton::menu-indicator { image: none; width: 0; }")
                    .arg(QLatin1StringView(kSurfaceContainerHigh()),
                         QLatin1StringView(kOnSurfaceVariant()),
                         QLatin1StringView(kOutline()),
                         QLatin1StringView(kPrimary())));
    return btn;
}

} // namespace

QWidget *makeViewerToolbar(const ViewerToolbarSpec &spec,
                           ViewerToolbarWidgets *out,
                           QWidget *parent)
{
    auto *bar = new QFrame(parent);
    bar->setObjectName(QStringLiteral("viewerToolbar"));
    bar->setStyleSheet(
            QStringLiteral(
                    "QFrame#viewerToolbar {"
                    "  background-color: %1;"
                    "  border: 1px solid %2;"
                    "  border-radius: 8px;"
                    "}"
                    "QFrame#viewerToolbar QLabel { background: transparent; }")
                    .arg(QLatin1StringView(kSurfaceContainerLow()),
                         QLatin1StringView(kOutline())));

    auto *hbox = new QHBoxLayout(bar);
    hbox->setContentsMargins(10, 6, 10, 6);
    hbox->setSpacing(8);

    // ── Path-pill: home glyph + dim parent path + bright file name ───────────
    auto *pill = new QFrame(bar);
    pill->setObjectName(QStringLiteral("viewerPathPill"));
    pill->setStyleSheet(
            QStringLiteral(
                    "QFrame#viewerPathPill {"
                    "  background-color: %1;"
                    "  border: 1px solid %2;"
                    "  border-radius: 6px;"
                    "}"
                    "QFrame#viewerPathPill QLabel { background: transparent; }")
                    .arg(QLatin1StringView(kSurfaceContainerHigh()),
                         QLatin1StringView(kOutline())));
    auto *pillLayout = new QHBoxLayout(pill);
    pillLayout->setContentsMargins(10, 4, 10, 4);
    pillLayout->setSpacing(8);

    auto *icon = new QLabel(QStringLiteral("⌂"), pill);
    icon->setStyleSheet(
            QStringLiteral("color: %1; font-size: 12px;")
                    .arg(QLatin1StringView(kPrimary())));
    pillLayout->addWidget(icon);

    // Split file path: dim directory + bright filename (Layout B aesthetic).
    const QFileInfo fi(spec.filePath);
    const QString dirText = spec.filePath.isEmpty()
            ? QString()
            : (fi.absolutePath().endsWith(QLatin1Char('/'))
                       ? fi.absolutePath()
                       : fi.absolutePath() + QLatin1Char('/'));
    const QString fileText = fi.fileName();

    if (!dirText.isEmpty()) {
        auto *dir = new QLabel(dirText, pill);
        dir->setStyleSheet(
                QStringLiteral(
                        "color: %1; font-family: 'JetBrains Mono', 'SF Mono', monospace; "
                        "font-size: 11px;")
                        .arg(QLatin1StringView(kOnSurfaceVariant())));
        dir->setTextInteractionFlags(Qt::TextSelectableByMouse);
        pillLayout->addWidget(dir);
    }

    auto *file = new QLabel(fileText.isEmpty() ? spec.filePath : fileText, pill);
    file->setStyleSheet(
            QStringLiteral(
                    "color: %1; font-family: 'JetBrains Mono', 'SF Mono', monospace; "
                    "font-size: 11px; font-weight: 600;")
                    .arg(QLatin1StringView(kOnSurface())));
    file->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pillLayout->addWidget(file);

    hbox->addWidget(pill);

    // ── Dataset switcher (▼): opens a recent-datasets menu ───────────────────
    auto *switchBtn = makeGlyphButton(QStringLiteral("▼"), bar);
    switchBtn->setToolTip(QStringLiteral("Switch dataset"));
    switchBtn->setPopupMode(QToolButton::InstantPopup);
    hbox->addWidget(switchBtn);
    if (out) out->datasetSwitch = switchBtn;

    // ── Tag chips (primary cyan-tinted clickable, secondary neutral) ─────────
    if (!spec.primaryTag.isEmpty()) {
        auto *primaryBtn = makeTagButton(spec.primaryTag, /*accent=*/true, bar);
        primaryBtn->setToolTip(QStringLiteral("Show FITS header"));
        hbox->addWidget(primaryBtn);
        if (out) out->primaryTag = primaryBtn;
    }
    if (!spec.secondaryTag.isEmpty())
        hbox->addWidget(makeTagChip(spec.secondaryTag, /*accent=*/false, bar));

    hbox->addStretch(1);

    // ── Live remote/health chip ("host · N running") ─────────────────────────
    auto *remoteChip = makeTagChip(QStringLiteral("remote"), /*accent=*/false, bar);
    remoteChip->setToolTip(QStringLiteral("Backend host and running-task count"));
    hbox->addWidget(remoteChip);
    if (out) out->remoteChip = remoteChip;

    // ── Action buttons (Find / Command / Export) ────────────────────────────
    auto *findBtn = makeToolbarButton(QStringLiteral("Find"), false, bar);
    hbox->addWidget(findBtn);
    if (out) out->find = findBtn;

    auto *cmdBtn = makeToolbarButton(QStringLiteral("Command  ⌘K"), true, bar);
    hbox->addWidget(cmdBtn);
    if (out) out->command = cmdBtn;

    // Subtle vertical pipe separator before destructive/export action.
    auto *pipe = new QLabel(QStringLiteral("·"), bar);
    pipe->setStyleSheet(
            QStringLiteral("color: %1; font-size: 14px;")
                    .arg(QLatin1StringView(kSecondary())));
    hbox->addWidget(pipe);

    auto *expBtn = makeToolbarButton(QStringLiteral("Export"), false, bar);
    hbox->addWidget(expBtn);
    if (out) out->exportBtn = expBtn;

    // ── Theme toggle (◐) ─────────────────────────────────────────────────────
    auto *themeBtn = makeGlyphButton(QStringLiteral("◐"), bar);
    themeBtn->setToolTip(QStringLiteral("Toggle dark / light theme"));
    hbox->addWidget(themeBtn);
    if (out) out->theme = themeBtn;

    return bar;
}

// ── Dark QSS ──────────────────────────────────────────────────────────────────

QString darkStyleSheet()
{
    // Lato is the VisIVO brand typeface. It is bundled and registered in
    // loadBundledFonts() before this stylesheet is applied, so the font-family
    // reference here will resolve to the bundled copy on all platforms.
    return QStringLiteral(
            // ── Base ──────────────────────────────────────────────────────
            "QWidget {"
            "  background-color: #090F14;"
            "  color: #F1F4F6;"
            "  font-family: \"Lato\", \"Helvetica Neue\", \"Arial\", sans-serif;"
            "  font-size: 13px;"
            "  selection-background-color: rgba(4, 138, 191, 0.30);"
            "  selection-color: #F1F4F6;"
            "}"

            // ── Main window / dialog shells ────────────────────────────────
            "QMainWindow, QDialog {"
            "  background-color: #090F14;"
            "}"
            "QMainWindow::separator {"
            "  width: 1px; height: 1px;"
            "  background-color: rgba(4, 138, 191, 0.20);"
            "}"

            // ── Status bar ─────────────────────────────────────────────────
            "QStatusBar {"
            "  background-color: #11181F;"
            "  color: #9FA6AC;"
            "  font-size: 11px;"
            "  border-top: 1px solid rgba(4, 138, 191, 0.25);"
            "}"

            // ── Menu bar ───────────────────────────────────────────────────
            // Uses the VisIVO corporate navy — consistent with brand imagery.
            "QMenuBar {"
            "  background-color: #11181F;"
            "  color: #F1F4F6;"
            "  font-family: \"Lato\", \"Helvetica Neue\", \"Arial\", sans-serif;"
            "  font-size: 13px;"
            "  font-weight: 500;"
            "  padding: 2px 4px;"
            "  border-bottom: 1px solid rgba(4, 138, 191, 0.22);"
            "  spacing: 2px;"
            "}"
            "QMenuBar::item {"
            "  padding: 6px 12px;"
            "  border-radius: 4px;"
            "  background-color: transparent;"
            "}"
            "QMenuBar::item:selected {"
            "  background-color: #11181F;"
            "  color: #048ABF;"
            "}"
            "QMenuBar::item:pressed {"
            "  background-color: #192229;"
            "  color: #048ABF;"
            "}"

            // ── Drop-down menus ────────────────────────────────────────────
            "QMenu {"
            "  background-color: #11181F;"
            "  color: #F1F4F6;"
            "  border: 1px solid rgba(4, 138, 191, 0.28);"
            "  border-radius: 6px;"
            "  padding: 4px;"
            "}"
            "QMenu::item {"
            "  padding: 8px 28px 8px 16px;"
            "  border-radius: 4px;"
            "}"
            "QMenu::item:selected {"
            "  background-color: #192229;"
            "  color: #048ABF;"
            "}"
            "QMenu::item:disabled {"
            "  color: rgba(138, 184, 204, 0.4);"
            "}"
            "QMenu::separator {"
            "  height: 1px;"
            "  background-color: rgba(4, 138, 191, 0.20);"
            "  margin: 4px 8px;"
            "}"

            // ── Toolbar ────────────────────────────────────────────────────
            "QToolBar {"
            "  background-color: #11181F;"
            "  border-bottom: 1px solid rgba(4, 138, 191, 0.22);"
            "  spacing: 4px;"
            "  padding: 3px 6px;"
            "}"
            "QToolBar::separator {"
            "  width: 1px;"
            "  background-color: rgba(4, 138, 191, 0.22);"
            "  margin: 4px 2px;"
            "}"
            "QToolButton {"
            "  background-color: transparent;"
            "  color: #F1F4F6;"
            "  padding: 5px 8px;"
            "  border-radius: 4px;"
            "  border: none;"
            "}"
            "QToolButton:hover {"
            "  background-color: #11181F;"
            "  color: #048ABF;"
            "}"
            "QToolButton:pressed, QToolButton:checked {"
            "  background-color: #192229;"
            "  color: #048ABF;"
            "}"

            // ── Tab widget ─────────────────────────────────────────────────
            "QTabWidget::pane {"
            "  border: none;"
            "  background-color: #090F14;"
            "}"
            "QTabBar {"
            "  background-color: #11181F;"
            "  qproperty-drawBase: 0;"
            "}"
            "QTabBar::tab {"
            "  background-color: transparent;"
            "  color: #9FA6AC;"
            "  padding: 9px 22px;"
            "  font-family: \"Lato\", \"Helvetica Neue\", \"Arial\", sans-serif;"
            "  font-size: 13px;"
            "  font-weight: 500;"
            "  border: none;"
            "  border-bottom: 2px solid transparent;"
            "  margin-right: 2px;"
            "}"
            "QTabBar::tab:selected {"
            "  color: #048ABF;"
            "  border-bottom: 2px solid #048ABF;"
            "}"
            "QTabBar::tab:hover:!selected {"
            "  color: #F1F4F6;"
            "  background-color: rgba(18, 51, 79, 0.6);"
            "}"

            // ── Dock widget ────────────────────────────────────────────────
            // Title uses Lato uppercase tracked — consistent with brand section labels.
            "QDockWidget {"
            "  background-color: #11181F;"
            "  color: #F1F4F6;"
            "  border: none;"
            "  titlebar-close-icon: url(none);"
            "  titlebar-normal-icon: url(none);"
            "}"
            "QDockWidget::title {"
            "  background-color: #11181F;"
            "  color: #9FA6AC;"
            "  padding: 8px 14px;"
            "  font-family: \"Lato\", \"Helvetica Neue\", \"Arial\", sans-serif;"
            "  font-size: 10px;"
            "  font-weight: 700;"
            "  letter-spacing: 0.18em;"
            "  border-bottom: 1px solid rgba(4, 138, 191, 0.18);"
            "}"
            "QDockWidget::close-button, QDockWidget::float-button {"
            "  background-color: transparent;"
            "  border: none;"
            "  border-radius: 3px;"
            "  padding: 2px;"
            "}"
            "QDockWidget::close-button:hover, QDockWidget::float-button:hover {"
            "  background-color: #192229;"
            "}"

            // ── Push button (default ghost style) ─────────────────────────
            "QPushButton {"
            "  background-color: #11181F;"
            "  color: #F1F4F6;"
            "  font-family: \"Lato\", \"Helvetica Neue\", \"Arial\", sans-serif;"
            "  font-size: 13px;"
            "  font-weight: 500;"
            "  padding: 8px 16px;"
            "  border-radius: 6px;"
            "  border: 1px solid rgba(4, 138, 191, 0.28);"
            "  outline: none;"
            "}"
            "QPushButton:hover {"
            "  background-color: #192229;"
            "  color: #048ABF;"
            "  border-color: rgba(4, 138, 191, 0.55);"
            "}"
            "QPushButton:pressed {"
            "  background-color: #242E37;"
            "  color: #048ABF;"
            "}"
            "QPushButton:disabled {"
            "  background-color: #11181F;"
            "  color: rgba(230, 244, 249, 0.30);"
            "  border-color: rgba(4, 138, 191, 0.12);"
            "}"
            "QPushButton:flat {"
            "  background-color: transparent;"
            "  border: none;"
            "  color: #048ABF;"
            "}"
            "QPushButton:flat:hover {"
            "  background-color: rgba(18, 51, 79, 0.8);"
            "}"

            // ── Group box ──────────────────────────────────────────────────
            "QGroupBox {"
            "  background-color: #11181F;"
            "  border: 1px solid rgba(4, 138, 191, 0.18);"
            "  border-radius: 8px;"
            "  margin-top: 18px;"
            "  padding-top: 6px;"
            "  color: #9FA6AC;"
            "  font-family: \"Lato\", \"Helvetica Neue\", \"Arial\", sans-serif;"
            "  font-size: 10px;"
            "  font-weight: 700;"
            "  letter-spacing: 0.18em;"
            "}"
            "QGroupBox::title {"
            "  subcontrol-origin: margin;"
            "  subcontrol-position: top left;"
            "  padding: 0 8px;"
            "  left: 12px;"
            "  color: #9FA6AC;"
            "  font-size: 10px;"
            "  font-weight: 700;"
            "  letter-spacing: 0.18em;"
            "}"

            // ── Line edit / spin box ───────────────────────────────────────
            "QLineEdit, QDoubleSpinBox, QSpinBox, QTextEdit {"
            "  background-color: #11181F;"
            "  color: #F1F4F6;"
            "  border: 1px solid rgba(4, 138, 191, 0.32);"
            "  border-radius: 4px;"
            "  padding: 6px 10px;"
            "  font-size: 13px;"
            "  selection-background-color: rgba(4, 138, 191, 0.28);"
            "}"
            "QLineEdit:focus, QDoubleSpinBox:focus, QSpinBox:focus, QTextEdit:focus {"
            "  background-color: #11181F;"
            "  border-color: rgba(4, 138, 191, 0.55);"
            "}"
            "QDoubleSpinBox::up-button, QDoubleSpinBox::down-button,"
            "QSpinBox::up-button, QSpinBox::down-button {"
            "  background-color: #11181F;"
            "  border: none;"
            "  width: 18px;"
            "}"
            "QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover,"
            "QSpinBox::up-button:hover, QSpinBox::down-button:hover {"
            "  background-color: #192229;"
            "}"

            // ── Combo box ──────────────────────────────────────────────────
            "QComboBox {"
            "  background-color: #11181F;"
            "  color: #F1F4F6;"
            "  border: 1px solid rgba(4, 138, 191, 0.32);"
            "  border-radius: 4px;"
            "  padding: 6px 10px;"
            "  font-size: 13px;"
            "}"
            "QComboBox:hover { border-color: rgba(4, 138, 191, 0.55); }"
            "QComboBox:focus { border-color: rgba(4, 138, 191, 0.70); }"
            "QComboBox::drop-down {"
            "  border: none;"
            "  width: 24px;"
            "  subcontrol-origin: padding;"
            "  subcontrol-position: center right;"
            "}"
            "QComboBox QAbstractItemView {"
            "  background-color: #11181F;"
            "  color: #F1F4F6;"
            "  border: 1px solid rgba(4, 138, 191, 0.32);"
            "  border-radius: 4px;"
            "  selection-background-color: #192229;"
            "  selection-color: #048ABF;"
            "  outline: none;"
            "  padding: 4px;"
            "}"

            // ── List / tree widget ─────────────────────────────────────────
            "QListWidget, QTreeWidget, QTreeView {"
            "  background-color: #11181F;"
            "  color: #F1F4F6;"
            "  border: none;"
            "  outline: none;"
            "}"
            "QListWidget::item, QTreeWidget::item {"
            "  padding: 8px 12px;"
            "  border-radius: 4px;"
            "}"
            "QListWidget::item:selected, QTreeWidget::item:selected {"
            "  background-color: #11181F;"
            "  color: #048ABF;"
            "}"
            "QListWidget::item:hover:!selected, QTreeWidget::item:hover:!selected {"
            "  background-color: rgba(18, 51, 79, 0.65);"
            "}"

            // ── Table view ─────────────────────────────────────────────────
            "QTableView {"
            "  background-color: #11181F;"
            "  color: #F1F4F6;"
            "  gridline-color: rgba(4, 138, 191, 0.14);"
            "  border: none;"
            "  outline: none;"
            "  alternate-background-color: #090F14;"
            "}"
            "QTableView::item { padding: 6px 8px; }"
            "QTableView::item:selected { background-color: #11181F; color: #048ABF; }"
            "QHeaderView { background-color: #11181F; border: none; }"
            "QHeaderView::section {"
            "  background-color: #11181F;"
            "  color: #9FA6AC;"
            "  padding: 7px 10px;"
            "  font-family: \"Lato\", \"Helvetica Neue\", \"Arial\", sans-serif;"
            "  font-size: 10px;"
            "  font-weight: 700;"
            "  letter-spacing: 0.10em;"
            "  border: none;"
            "  border-bottom: 1px solid rgba(4, 138, 191, 0.20);"
            "  border-right: 1px solid rgba(4, 138, 191, 0.10);"
            "}"
            "QHeaderView::section:hover { background-color: #192229; color: #048ABF; }"

            // ── Scroll bars ────────────────────────────────────────────────
            "QScrollBar:vertical {"
            "  background-color: transparent;"
            "  width: 8px; border: none; margin: 0;"
            "}"
            "QScrollBar::handle:vertical {"
            "  background-color: rgba(4, 138, 191, 0.40);"
            "  border-radius: 4px; min-height: 24px;"
            "}"
            "QScrollBar::handle:vertical:hover { background-color: #048ABF; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical,"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
            "  background: none; border: none; height: 0;"
            "}"
            "QScrollBar:horizontal {"
            "  background-color: transparent;"
            "  height: 8px; border: none; margin: 0;"
            "}"
            "QScrollBar::handle:horizontal {"
            "  background-color: rgba(4, 138, 191, 0.40);"
            "  border-radius: 4px; min-width: 24px;"
            "}"
            "QScrollBar::handle:horizontal:hover { background-color: #048ABF; }"
            "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal,"
            "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {"
            "  background: none; border: none; width: 0;"
            "}"

            // ── Scroll area ────────────────────────────────────────────────
            "QScrollArea { background-color: #090F14; border: none; }"
            "QScrollArea > QWidget { background-color: transparent; }"

            // ── Slider ─────────────────────────────────────────────────────
            "QSlider::groove:horizontal {"
            "  background-color: #192229;"
            "  height: 4px; border-radius: 2px;"
            "}"
            "QSlider::handle:horizontal {"
            "  background-color: #048ABF;"
            "  width: 14px; height: 14px;"
            "  margin: -5px 0; border-radius: 7px;"
            "  border: 2px solid #11181F;"
            "}"
            "QSlider::handle:horizontal:hover { background-color: #5BB8D8; }"
            "QSlider::sub-page:horizontal {"
            "  background-color: #005574;"
            "  height: 4px; border-radius: 2px;"
            "}"

            // ── Splitter ───────────────────────────────────────────────────
            "QSplitter::handle { background-color: rgba(4, 138, 191, 0.20); }"
            "QSplitter::handle:horizontal { width: 1px; }"
            "QSplitter::handle:vertical   { height: 1px; }"

            // ── Radio / check box ──────────────────────────────────────────
            "QRadioButton, QCheckBox {"
            "  color: #F1F4F6; spacing: 8px; background-color: transparent;"
            "}"
            "QRadioButton::indicator {"
            "  width: 16px; height: 16px; border-radius: 8px;"
            "  border: 2px solid #2B343C; background-color: #11181F;"
            "}"
            "QCheckBox::indicator {"
            "  width: 16px; height: 16px; border-radius: 3px;"
            "  border: 2px solid #2B343C; background-color: #11181F;"
            "}"
            "QRadioButton::indicator:checked, QCheckBox::indicator:checked {"
            "  background-color: #048ABF; border-color: #048ABF;"
            "}"
            "QRadioButton::indicator:hover, QCheckBox::indicator:hover {"
            "  border-color: #048ABF;"
            "}"

            // ── Labels ─────────────────────────────────────────────────────
            "QLabel { background-color: transparent; color: #F1F4F6; }"

            // ── Tooltip ────────────────────────────────────────────────────
            "QToolTip {"
            "  background-color: #11181F;"
            "  color: #F1F4F6;"
            "  border: 1px solid rgba(4, 138, 191, 0.35);"
            "  border-radius: 4px;"
            "  padding: 6px 10px;"
            "  font-size: 12px;"
            "}"

            // ── Web engine ─────────────────────────────────────────────────
            "QWebEngineView { background-color: #090F14; }");
}

QLabel *createVisivoLogoLabel(int height, QWidget *parent)
{
    // Pick the logo variant that reads against the active surface palette:
    //   • Dark theme → white logo (high contrast on navy)
    //   • Light theme → colored brand logo (the canonical VisIVO mark)
    const QString logoPath = isDark()
            ? QStringLiteral(":/icons/visivo_logo_white.svg")
            : QStringLiteral(":/icons/VisIVO.svg");

    // Try the SVG resource first — crisp at any DPI.
    QSvgRenderer renderer(logoPath);
    if (renderer.isValid()) {
        const QSize svgSize = renderer.defaultSize();
        // Scale width to match requested height while preserving aspect ratio.
        const int w = (svgSize.height() > 0)
                ? qRound(svgSize.width() * static_cast<double>(height) / svgSize.height())
                : height * 2;
        QPixmap pm(w, height);
        pm.fill(Qt::transparent);
        QPainter painter(&pm);
        renderer.render(&painter);
        painter.end();

        auto *lbl = new QLabel(parent);
        lbl->setPixmap(pm);
        lbl->setFixedSize(w, height);
        lbl->setStyleSheet(u"background: transparent; border: none;"_s);
        lbl->setAttribute(Qt::WA_TransparentForMouseEvents);
        return lbl;
    }

    // SVG failed — fall back to the bundled 128-px PNG.
    QPixmap fallback(QStringLiteral(":/icons/VisIVO_128.png"));
    if (!fallback.isNull()) {
        const QPixmap scaled = fallback.scaledToHeight(height, Qt::SmoothTransformation);
        auto *lbl = new QLabel(parent);
        lbl->setPixmap(scaled);
        lbl->setFixedSize(scaled.width(), scaled.height());
        lbl->setStyleSheet(u"background: transparent; border: none;"_s);
        lbl->setAttribute(Qt::WA_TransparentForMouseEvents);
        return lbl;
    }

    qWarning("[VisivoTheme] createVisivoLogoLabel: both SVG and PNG resources failed to load");
    return nullptr;
}

void applyDarkTo(QApplication *app)
{
    // Mark the active theme so factory functions / inline accessors return the
    // dark palette. Must be set BEFORE building the QPalette below, because the
    // accessors are conditional on currentMode().
    setMode(Mode::Dark);

    // ── 1. Register bundled fonts first ───────────────────────────────────────
    // Must happen before any QFont or QSS that references "Lato".
    loadBundledFonts();

    // ── 2. Set Lato as the application default font ───────────────────────────
    // This covers widgets that bypass QSS font-family (native delegates, VTK
    // overlays, platform renderers). Lato Regular at 13px matches the brand body
    // size and is a safe, readable default.
    QFont appFont(QStringLiteral("Lato"));
    appFont.setPointSize(13);
    appFont.setWeight(QFont::Normal);
    app->setFont(appFont);

    // ── 3. QPalette ───────────────────────────────────────────────────────────
    // Ensures widgets that skip QSS get coherent VisIVO-navy colours.
    QPalette pal;
    pal.setColor(QPalette::Window,          QColor(kBackground()));
    pal.setColor(QPalette::WindowText,      QColor(kOnSurface()));
    pal.setColor(QPalette::Base,            QColor(kSurfaceContainerLow()));
    pal.setColor(QPalette::AlternateBase,   QColor(kSurface()));
    pal.setColor(QPalette::Text,            QColor(kOnSurface()));
    pal.setColor(QPalette::BrightText,      QColor(kPrimary()));
    pal.setColor(QPalette::Button,          QColor(kSurfaceContainerHigh()));
    pal.setColor(QPalette::ButtonText,      QColor(kOnSurface()));
    pal.setColor(QPalette::Highlight,       QColor(kSurfaceBright()));
    pal.setColor(QPalette::HighlightedText, QColor(kPrimary()));
    pal.setColor(QPalette::Link,            QColor(kPrimary()));
    pal.setColor(QPalette::LinkVisited,     QColor(kPrimaryDim()));
    pal.setColor(QPalette::ToolTipBase,     QColor(kSurfaceContainerHigh()));
    pal.setColor(QPalette::ToolTipText,     QColor(kOnSurface()));
    pal.setColor(QPalette::PlaceholderText, QColor(kOnSurfaceVariant()));
    pal.setColor(QPalette::Mid,             QColor(kOutlineVariant()));
    pal.setColor(QPalette::Midlight,        QColor(kSurfaceContainer()));
    pal.setColor(QPalette::Dark,            QColor(kSurfaceContainerLow()));
    pal.setColor(QPalette::Shadow,          QColor(kBackground()));
    app->setPalette(pal);

    // ── 4. QSS ────────────────────────────────────────────────────────────────
    app->setStyleSheet(darkStyleSheet());
}

// ── Light theme ───────────────────────────────────────────────────────────────
//
// Design intent:
//   • Retain the VisIVO blue accent (#005574, #048ABF) as the interactive
//     colour — brand-consistent on both themes.
//   • Background hierarchy mirrors the dark theme but in light grays with
//     a subtle blue tint, preserving the VisIVO visual identity.
//   • Text is near-black navy (#1C2734) rather than pure black for warmth.
//
// Light palette tokens (named in parallel with the dark namespace above):
inline constexpr auto kLightBg              = "#FFFFFF";   // deepest bg (blue-tinted near-white)
inline constexpr auto kLightSurface         = "#FFFFFF";   // standard panel
inline constexpr auto kLightSurfaceLow      = "#FAFBFD";   // slightly tinted widget base
inline constexpr auto kLightSurfaceHigh     = "#F1F4F9";   // elevated widget
inline constexpr auto kLightHover           = "#F4F6FA";   // hover/selected
inline constexpr auto kLightText            = "#1C2734";   // primary text (dark navy)
inline constexpr auto kLightTextMuted       = "#5B6A7E";   // secondary/muted
inline constexpr auto kLightOutline         = "#D8DEE7";   // border

QString lightStyleSheet()
{
    return QStringLiteral(
            // ── Base ──────────────────────────────────────────────────────────
            "QWidget {"
            "  background-color: #FFFFFF;"
            "  color: #1C2734;"
            "  font-family: \"Lato\", \"Helvetica Neue\", \"Arial\", sans-serif;"
            "  font-size: 13px;"
            "  selection-background-color: rgba(4, 138, 191, 0.20);"
            "  selection-color: #1C2734;"
            "}"

            // ── Main window / dialog shells ────────────────────────────────────
            "QMainWindow, QDialog {"
            "  background-color: #FFFFFF;"
            "}"
            "QMainWindow::separator {"
            "  width: 1px; height: 1px;"
            "  background-color: rgba(3, 103, 166, 0.15);"
            "}"

            // ── Status bar ─────────────────────────────────────────────────────
            "QStatusBar {"
            "  background-color: #FAFBFD;"
            "  color: #5B6A7E;"
            "  font-size: 11px;"
            "  border-top: 1px solid rgba(3, 103, 166, 0.18);"
            "}"

            // ── Menu bar ───────────────────────────────────────────────────────
            "QMenuBar {"
            "  background-color: #FAFBFD;"
            "  color: #1C2734;"
            "  font-family: \"Lato\", \"Helvetica Neue\", \"Arial\", sans-serif;"
            "  font-size: 13px;"
            "  font-weight: 500;"
            "  padding: 2px 4px;"
            "  border-bottom: 1px solid rgba(3, 103, 166, 0.18);"
            "  spacing: 2px;"
            "}"
            "QMenuBar::item {"
            "  padding: 6px 12px;"
            "  border-radius: 4px;"
            "  background-color: transparent;"
            "}"
            "QMenuBar::item:selected {"
            "  background-color: #F4F6FA;"
            "  color: #005574;"
            "}"
            "QMenuBar::item:pressed {"
            "  background-color: #C8DCF0;"
            "  color: #005574;"
            "}"

            // ── Drop-down menus ────────────────────────────────────────────────
            "QMenu {"
            "  background-color: #FFFFFF;"
            "  color: #1C2734;"
            "  border: 1px solid rgba(3, 103, 166, 0.22);"
            "  border-radius: 6px;"
            "  padding: 4px;"
            "}"
            "QMenu::item {"
            "  padding: 8px 28px 8px 16px;"
            "  border-radius: 4px;"
            "}"
            "QMenu::item:selected {"
            "  background-color: #F4F6FA;"
            "  color: #005574;"
            "}"
            "QMenu::item:disabled {"
            "  color: rgba(90, 122, 143, 0.5);"
            "}"
            "QMenu::separator {"
            "  height: 1px;"
            "  background-color: rgba(3, 103, 166, 0.15);"
            "  margin: 4px 8px;"
            "}"

            // ── Toolbar ────────────────────────────────────────────────────────
            "QToolBar {"
            "  background-color: #FAFBFD;"
            "  border-bottom: 1px solid rgba(3, 103, 166, 0.18);"
            "  spacing: 4px;"
            "  padding: 3px 6px;"
            "}"
            "QToolBar::separator {"
            "  width: 1px;"
            "  background-color: rgba(3, 103, 166, 0.18);"
            "  margin: 4px 2px;"
            "}"
            "QToolButton {"
            "  background-color: transparent;"
            "  color: #1C2734;"
            "  padding: 5px 8px;"
            "  border-radius: 4px;"
            "  border: none;"
            "}"
            "QToolButton:hover {"
            "  background-color: #F4F6FA;"
            "  color: #005574;"
            "}"
            "QToolButton:pressed, QToolButton:checked {"
            "  background-color: #C8DCF0;"
            "  color: #005574;"
            "}"

            // ── Tab widget ─────────────────────────────────────────────────────
            "QTabWidget::pane {"
            "  border: none;"
            "  background-color: #FFFFFF;"
            "}"
            "QTabBar {"
            "  background-color: #FAFBFD;"
            "  qproperty-drawBase: 0;"
            "}"
            "QTabBar::tab {"
            "  background-color: transparent;"
            "  color: #5B6A7E;"
            "  padding: 9px 22px;"
            "  font-family: \"Lato\", \"Helvetica Neue\", \"Arial\", sans-serif;"
            "  font-size: 13px;"
            "  font-weight: 500;"
            "  border: none;"
            "  border-bottom: 2px solid transparent;"
            "  margin-right: 2px;"
            "}"
            "QTabBar::tab:selected {"
            "  color: #005574;"
            "  border-bottom: 2px solid #005574;"
            "}"
            "QTabBar::tab:hover:!selected {"
            "  color: #1C2734;"
            "  background-color: rgba(213, 230, 242, 0.6);"
            "}"

            // ── Dock widget ────────────────────────────────────────────────────
            "QDockWidget {"
            "  background-color: #FAFBFD;"
            "  color: #1C2734;"
            "  border: none;"
            "}"
            "QDockWidget::title {"
            "  background-color: #F1F4F9;"
            "  color: #5B6A7E;"
            "  padding: 8px 14px;"
            "  font-family: \"Lato\", \"Helvetica Neue\", \"Arial\", sans-serif;"
            "  font-size: 10px;"
            "  font-weight: 700;"
            "  letter-spacing: 0.18em;"
            "  border-bottom: 1px solid rgba(3, 103, 166, 0.15);"
            "}"
            "QDockWidget::close-button, QDockWidget::float-button {"
            "  background-color: transparent;"
            "  border: none;"
            "  border-radius: 3px;"
            "  padding: 2px;"
            "}"
            "QDockWidget::close-button:hover, QDockWidget::float-button:hover {"
            "  background-color: #F4F6FA;"
            "}"

            // ── Push button ────────────────────────────────────────────────────
            "QPushButton {"
            "  background-color: #FFFFFF;"
            "  color: #1C2734;"
            "  font-family: \"Lato\", \"Helvetica Neue\", \"Arial\", sans-serif;"
            "  font-size: 13px;"
            "  font-weight: 500;"
            "  padding: 8px 16px;"
            "  border-radius: 6px;"
            "  border: 1px solid rgba(3, 103, 166, 0.30);"
            "  outline: none;"
            "}"
            "QPushButton:hover {"
            "  background-color: #FAFBFD;"
            "  color: #005574;"
            "  border-color: rgba(3, 103, 166, 0.55);"
            "}"
            "QPushButton:pressed {"
            "  background-color: #F4F6FA;"
            "  color: #005574;"
            "}"
            "QPushButton:disabled {"
            "  background-color: #FFFFFF;"
            "  color: rgba(26, 43, 60, 0.35);"
            "  border-color: rgba(3, 103, 166, 0.12);"
            "}"
            "QPushButton:flat {"
            "  background-color: transparent;"
            "  border: none;"
            "  color: #005574;"
            "}"
            "QPushButton:flat:hover {"
            "  background-color: rgba(213, 230, 242, 0.8);"
            "}"

            // ── Group box ──────────────────────────────────────────────────────
            "QGroupBox {"
            "  background-color: #FAFBFD;"
            "  border: 1px solid rgba(3, 103, 166, 0.18);"
            "  border-radius: 8px;"
            "  margin-top: 18px;"
            "  padding-top: 6px;"
            "  color: #5B6A7E;"
            "  font-family: \"Lato\", \"Helvetica Neue\", \"Arial\", sans-serif;"
            "  font-size: 10px;"
            "  font-weight: 700;"
            "  letter-spacing: 0.18em;"
            "}"
            "QGroupBox::title {"
            "  subcontrol-origin: margin;"
            "  subcontrol-position: top left;"
            "  padding: 0 8px;"
            "  left: 12px;"
            "  color: #5B6A7E;"
            "  font-size: 10px;"
            "  font-weight: 700;"
            "  letter-spacing: 0.18em;"
            "}"

            // ── Line edit / spin box ───────────────────────────────────────────
            "QLineEdit, QDoubleSpinBox, QSpinBox, QTextEdit {"
            "  background-color: #FFFFFF;"
            "  color: #1C2734;"
            "  border: 1px solid rgba(3, 103, 166, 0.30);"
            "  border-radius: 4px;"
            "  padding: 6px 10px;"
            "  font-size: 13px;"
            "  selection-background-color: rgba(4, 138, 191, 0.20);"
            "}"
            "QLineEdit:focus, QDoubleSpinBox:focus, QSpinBox:focus, QTextEdit:focus {"
            "  background-color: #FFFFFF;"
            "  border-color: rgba(3, 103, 166, 0.60);"
            "}"
            "QDoubleSpinBox::up-button, QDoubleSpinBox::down-button,"
            "QSpinBox::up-button, QSpinBox::down-button {"
            "  background-color: #FAFBFD;"
            "  border: none;"
            "  width: 18px;"
            "}"
            "QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover,"
            "QSpinBox::up-button:hover, QSpinBox::down-button:hover {"
            "  background-color: #F4F6FA;"
            "}"

            // ── Combo box ──────────────────────────────────────────────────────
            "QComboBox {"
            "  background-color: #FFFFFF;"
            "  color: #1C2734;"
            "  border: 1px solid rgba(3, 103, 166, 0.30);"
            "  border-radius: 4px;"
            "  padding: 6px 10px;"
            "  font-size: 13px;"
            "}"
            "QComboBox:hover { border-color: rgba(3, 103, 166, 0.55); }"
            "QComboBox:focus { border-color: rgba(3, 103, 166, 0.70); }"
            "QComboBox::drop-down {"
            "  border: none;"
            "  width: 24px;"
            "  subcontrol-origin: padding;"
            "  subcontrol-position: center right;"
            "}"
            "QComboBox QAbstractItemView {"
            "  background-color: #FFFFFF;"
            "  color: #1C2734;"
            "  border: 1px solid rgba(3, 103, 166, 0.25);"
            "  border-radius: 4px;"
            "  selection-background-color: #F4F6FA;"
            "  selection-color: #005574;"
            "  outline: none;"
            "  padding: 4px;"
            "}"

            // ── List / tree widget ─────────────────────────────────────────────
            "QListWidget, QTreeWidget, QTreeView {"
            "  background-color: #FFFFFF;"
            "  color: #1C2734;"
            "  border: none;"
            "  outline: none;"
            "}"
            "QListWidget::item, QTreeWidget::item {"
            "  padding: 8px 12px;"
            "  border-radius: 4px;"
            "}"
            "QListWidget::item:selected, QTreeWidget::item:selected {"
            "  background-color: #F4F6FA;"
            "  color: #005574;"
            "}"
            "QListWidget::item:hover:!selected, QTreeWidget::item:hover:!selected {"
            "  background-color: rgba(213, 230, 242, 0.65);"
            "}"

            // ── Table view ─────────────────────────────────────────────────────
            "QTableView {"
            "  background-color: #FFFFFF;"
            "  color: #1C2734;"
            "  gridline-color: rgba(3, 103, 166, 0.12);"
            "  border: none;"
            "  outline: none;"
            "  alternate-background-color: #FFFFFF;"
            "}"
            "QTableView::item { padding: 6px 8px; }"
            "QTableView::item:selected { background-color: #F4F6FA; color: #005574; }"
            "QHeaderView { background-color: #FAFBFD; border: none; }"
            "QHeaderView::section {"
            "  background-color: #FAFBFD;"
            "  color: #5B6A7E;"
            "  padding: 7px 10px;"
            "  font-family: \"Lato\", \"Helvetica Neue\", \"Arial\", sans-serif;"
            "  font-size: 10px;"
            "  font-weight: 700;"
            "  letter-spacing: 0.10em;"
            "  border: none;"
            "  border-bottom: 1px solid rgba(3, 103, 166, 0.18);"
            "  border-right: 1px solid rgba(3, 103, 166, 0.10);"
            "}"
            "QHeaderView::section:hover { background-color: #F4F6FA; color: #005574; }"

            // ── Scroll bars ────────────────────────────────────────────────────
            "QScrollBar:vertical {"
            "  background-color: transparent;"
            "  width: 8px; border: none; margin: 0;"
            "}"
            "QScrollBar::handle:vertical {"
            "  background-color: rgba(3, 103, 166, 0.35);"
            "  border-radius: 4px; min-height: 24px;"
            "}"
            "QScrollBar::handle:vertical:hover { background-color: #005574; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical,"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
            "  background: none; border: none; height: 0;"
            "}"
            "QScrollBar:horizontal {"
            "  background-color: transparent;"
            "  height: 8px; border: none; margin: 0;"
            "}"
            "QScrollBar::handle:horizontal {"
            "  background-color: rgba(3, 103, 166, 0.35);"
            "  border-radius: 4px; min-width: 24px;"
            "}"
            "QScrollBar::handle:horizontal:hover { background-color: #005574; }"
            "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal,"
            "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {"
            "  background: none; border: none; width: 0;"
            "}"

            // ── Scroll area ────────────────────────────────────────────────────
            "QScrollArea { background-color: #FFFFFF; border: none; }"
            "QScrollArea > QWidget { background-color: transparent; }"

            // ── Slider ─────────────────────────────────────────────────────────
            "QSlider::groove:horizontal {"
            "  background-color: #C8DCF0;"
            "  height: 4px; border-radius: 2px;"
            "}"
            "QSlider::handle:horizontal {"
            "  background-color: #005574;"
            "  width: 14px; height: 14px;"
            "  margin: -5px 0; border-radius: 7px;"
            "  border: 2px solid #FFFFFF;"
            "}"
            "QSlider::handle:horizontal:hover { background-color: #048ABF; }"
            "QSlider::sub-page:horizontal {"
            "  background-color: #048ABF;"
            "  height: 4px; border-radius: 2px;"
            "}"

            // ── Splitter ───────────────────────────────────────────────────────
            "QSplitter::handle { background-color: rgba(3, 103, 166, 0.15); }"
            "QSplitter::handle:horizontal { width: 1px; }"
            "QSplitter::handle:vertical   { height: 1px; }"

            // ── Radio / check box ──────────────────────────────────────────────
            "QRadioButton, QCheckBox {"
            "  color: #1C2734; spacing: 8px; background-color: transparent;"
            "}"
            "QRadioButton::indicator {"
            "  width: 16px; height: 16px; border-radius: 8px;"
            "  border: 2px solid #D8DEE7; background-color: #FFFFFF;"
            "}"
            "QCheckBox::indicator {"
            "  width: 16px; height: 16px; border-radius: 3px;"
            "  border: 2px solid #D8DEE7; background-color: #FFFFFF;"
            "}"
            "QRadioButton::indicator:checked, QCheckBox::indicator:checked {"
            "  background-color: #005574; border-color: #005574;"
            "}"
            "QRadioButton::indicator:hover, QCheckBox::indicator:hover {"
            "  border-color: #005574;"
            "}"

            // ── Labels ─────────────────────────────────────────────────────────
            "QLabel { background-color: transparent; color: #1C2734; }"

            // ── Tooltip ────────────────────────────────────────────────────────
            "QToolTip {"
            "  background-color: #FFFFFF;"
            "  color: #1C2734;"
            "  border: 1px solid rgba(3, 103, 166, 0.30);"
            "  border-radius: 4px;"
            "  padding: 6px 10px;"
            "  font-size: 12px;"
            "}"

            // ── Web engine ─────────────────────────────────────────────────────
            "QWebEngineView { background-color: #FFFFFF; }"
    );
}

void applyLightTo(QApplication *app)
{
    // Mark the active theme so factory functions / inline accessors return the
    // light palette. Must be set BEFORE building widgets through any factory.
    setMode(Mode::Light);

    // ── 1. Register bundled fonts (idempotent) ────────────────────────────────
    loadBundledFonts();

    // ── 2. Default font ───────────────────────────────────────────────────────
    QFont appFont(QStringLiteral("Lato"));
    appFont.setPointSize(13);
    appFont.setWeight(QFont::Normal);
    app->setFont(appFont);

    // ── 3. QPalette ───────────────────────────────────────────────────────────
    app->setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QPalette pal;
    pal.setColor(QPalette::Window,          QColor(kLightBg));
    pal.setColor(QPalette::WindowText,      QColor(kLightText));
    pal.setColor(QPalette::Base,            QColor(kLightSurface));
    pal.setColor(QPalette::AlternateBase,   QColor(kLightSurfaceLow));
    pal.setColor(QPalette::Text,            QColor(kLightText));
    pal.setColor(QPalette::BrightText,      QColor(kBlueBright));
    pal.setColor(QPalette::Button,          QColor(kLightSurfaceHigh));
    pal.setColor(QPalette::ButtonText,      QColor(kLightText));
    pal.setColor(QPalette::Highlight,       QColor(kBlueBright));
    pal.setColor(QPalette::HighlightedText, QColor(kLightSurface));
    pal.setColor(QPalette::Link,            QColor(kBlue));
    pal.setColor(QPalette::LinkVisited,     QColor(kLightTextMuted));
    pal.setColor(QPalette::ToolTipBase,     QColor(kLightSurfaceHigh));
    pal.setColor(QPalette::ToolTipText,     QColor(kLightText));
    pal.setColor(QPalette::PlaceholderText, QColor(kLightTextMuted));
    pal.setColor(QPalette::Mid,             QColor(kLightOutline));
    pal.setColor(QPalette::Midlight,        QColor(kLightSurfaceLow));
    pal.setColor(QPalette::Dark,            QColor(u"#C0D0DB"_s));
    pal.setColor(QPalette::Shadow,          QColor(u"#8AABBF"_s));
    app->setPalette(pal);

    // ── 4. QSS ────────────────────────────────────────────────────────────────
    app->setStyleSheet(lightStyleSheet());
}

} // namespace VisivoTheme
