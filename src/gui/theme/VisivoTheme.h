#ifndef VisivoTheme_h
#define VisivoTheme_h

#include <QPixmap>
#include <QString>

class QApplication;
class QColor;
class QFrame;
class QLabel;
class QPushButton;
class QToolButton;
class QWidget;

// ---------------------------------------------------------------------------
// VisivoTheme — VisIVO "Observatory console" design system (Layout B).
//
// The palette derives from the OKLCH ramp shipped in the Claude-Design bundle
// (`visivo/project/tokens.js`) — cool-neutral surfaces with very low chroma
// and the brand blue (#048ABF) reserved for primary actions. Hex values are
// the sRGB conversions of those OKLCH coordinates and live as inline string
// literals so widgets stay theme-aware via the accessor functions below.
//
// Surface ramp (dark / light):
//   kBackground / kSurface         deepest bg / canvas      #090F14 / #F8FAFD
//   kSurfaceContainerLow           card / panel             #11181F / #F1F4F7
//   kSurfaceContainerHigh          hover / raised           #192229 / #E7ECF0
//   kSurfaceContainerHighest       input / top-of-stack     #242E37 / #DCE2E7
//
// Brand & semantic colors:
//   kPrimary       #048ABF   brand blue (theme-invariant)
//   kSuccess       green     completed operations
//   kWarning       amber     near-threshold / sanity
//   kError         red       failures / disconnects
//   kRunning       teal      live activity (distinct hue from primary)
//   kCache         lavender  cache hits / cold-storage retrievals
//
// Usage:
//   VisivoTheme::applyDarkTo(qApp);   ← call after QApplication is created
// ---------------------------------------------------------------------------
namespace VisivoTheme {

// ── Theme mode tracking ───────────────────────────────────────────────────────
// Set by applyDarkTo() / applyLightTo(); read by the color functions below.
// All colors that vary between dark and light are returned by inline accessors
// so widgets built via the factory functions or composing inline stylesheets
// automatically pick up the right palette at construction time.
//
// NOTE: changing the mode after widgets are constructed only updates the
//       global QSS — already-constructed widgets keep their inline styles.
//       Recreate them (or restart) to fully refresh.
enum class Mode { Dark, Light };
void setMode(Mode m);
Mode currentMode();
inline bool isDark() { return currentMode() == Mode::Dark; }

// ── Brand palette ────────────────────────────────────────────────────────────
// The brand blue is theme-invariant. kNavy/kBlue are kept as legacy aliases
// for the few SAMP/branding paths that hard-code the historical navy/blue.
inline constexpr auto kNavy       = "#0E2940";
inline constexpr auto kBlue       = "#0367A6";
inline constexpr auto kBlueBright = "#048ABF";

// ── Surface ramp (cool-neutral) ──────────────────────────────────────────────
// IMPORTANT — Layout B Light principle:
//   Data viewport surfaces (VTK render windows, LUT-preview thumbnails, the
//   colorbar strips) intentionally stay DARK in the light theme. Scientific
//   colormaps (Inferno / Viridis / Magma) are designed against black; on a
//   white background they wash out. The constants below describe the *chrome*
//   palette only — viewport backgrounds are set independently in the VTK
//   render-window initializers and must NOT switch with the theme.
//
// Dark values were converted from the bundle's OKLCH ramp; light values are
// the hand-tuned design tokens from `VisIVO Layout B Light.html`.
//
// surface0 ─ deepest canvas / viewport-adjacent gutter
inline const char *kBackground() {
    return isDark() ? "#090F14" : "#FFFFFF";
}
inline const char *kSurface() {
    return isDark() ? "#090F14" : "#FFFFFF";
}
// surface1 ─ cards / panels
inline const char *kSurfaceContainerLow() {
    return isDark() ? "#11181F" : "#FAFBFD";
}
inline const char *kSurfaceContainer() {
    return isDark() ? "#11181F" : "#FAFBFD";
}
// surface2 ─ raised / hover (toolbars, status bar, vb-rail)
inline const char *kSurfaceContainerHigh() {
    return isDark() ? "#192229" : "#F1F4F9";
}
// surface3 ─ input fields / top-of-stack (path-pill, chips background)
inline const char *kSurfaceContainerHighest() {
    return isDark() ? "#242E37" : "#F4F6FA";
}
inline const char *kSurfaceBright() {
    return isDark() ? "#242E37" : "#F4F6FA";
}
inline const char *kSurfaceVariant() {
    return isDark() ? "#242E37" : "#F4F6FA";
}

// ── Primary accent (theme-invariant brand blues) ─────────────────────────────
inline const char *kPrimary()         { return "#048ABF"; }
inline const char *kPrimaryDim() {
    return isDark() ? "#005574" : "#007CA5";
}
inline const char *kOnPrimary()       {
    return isDark() ? "#F6F9FC" : "#FAFCFE";
}
inline const char *kPrimaryFixed()    { return "#5BB8D8"; }
inline const char *kPrimaryFixedDim() { return "#3AAECF"; }

// ── Text ─────────────────────────────────────────────────────────────────────
inline const char *kOnSurface() {
    return isDark() ? "#F1F4F6" : "#1C2734";
}
inline const char *kOnSurfaceVariant() {
    return isDark() ? "#9FA6AC" : "#5B6A7E";
}
inline const char *kSecondary() {
    return isDark() ? "#6B737A" : "#8A99AE";
}

// ── Borders ──────────────────────────────────────────────────────────────────
inline const char *kOutline() {
    return isDark() ? "#2B343C" : "#D8DEE7";
}
inline const char *kOutlineVariant() {
    return isDark() ? "#2B343C" : "#E6EBF2";
}

// ── Semantic states ──────────────────────────────────────────────────────────
// Foreground hues from the Layout B token ramp. Container variants are
// derived as low-alpha tints of the same hue so badges read on either theme.
inline const char *kSuccess() { return isDark() ? "#57BC80" : "#14874E"; }
inline const char *kWarning() { return isDark() ? "#E8AA4E" : "#B77610"; }
inline const char *kError()   { return isDark() ? "#D67069" : "#BD413F"; }
inline const char *kRunning() { return isDark() ? "#17D0D8" : "#0081A5"; }
inline const char *kCache()   { return isDark() ? "#A39FC8" : "#716B98"; }

// Container shades for badges (low-luminance fill on dark, low-saturation
// tint on light). Hex values picked to keep the foreground readable on top.
inline const char *kTertiaryContainer()  {
    return isDark() ? "#1A2D24" : "#DFEEE5";
}
inline const char *kOnTertiaryFixed()    {
    return isDark() ? "#57BC80" : "#14874E";
}
inline const char *kErrorContainer()     {
    return isDark() ? "#2D1614" : "#F4DAD8";
}
inline const char *kOnErrorContainer()   {
    return isDark() ? "#D67069" : "#BD413F";
}
inline const char *kWarningContainer()   {
    return isDark() ? "#2C2210" : "#F4E5C9";
}
inline const char *kOnWarningContainer() {
    return isDark() ? "#E8AA4E" : "#B77610";
}

// ── Typography — Lato as the primary brand typeface ───────────────────────────
// Lato is bundled via Qt resources. The fallback chain ensures graceful
// degradation on platforms where resource loading fails.
inline constexpr auto kBrandFamily =
    "\"Lato\", \"Helvetica Neue\", \"Arial\", sans-serif";

// Data/body alias — still Lato; Inter listed as system fallback only.
inline constexpr auto kBodyFamily =
    "\"Lato\", \"Inter\", \"Helvetica Neue\", \"Segoe UI\", sans-serif";

// ── Font loading ──────────────────────────────────────────────────────────────
// Registers the bundled Lato TTF files with QFontDatabase.
// Returns true if at least Lato-Regular loaded successfully.
bool loadBundledFonts();

// ── Logo helper ───────────────────────────────────────────────────────────────
// Returns a QLabel containing the VisIVO SVG logo rendered as a QPixmap.
// The logo is scaled to 'height' pixels (preserving aspect ratio).
// Uses the bundled :/branding/visivo_logo.svg resource.
// Falls back to :/icons/VisIVO_128.png if SVG is unavailable.
// Returns nullptr on failure (caller must guard).
QLabel *createVisivoLogoLabel(int height, QWidget *parent = nullptr);

// ── Component factories ───────────────────────────────────────────────────────
QLabel *makeSectionLabel(const QString &text, QWidget *parent = nullptr);

QFrame *makeCard(QWidget *parent = nullptr, bool elevated = false);

enum class BadgeKind { Success, Error, Warning, Neutral };
QLabel *makeBadge(const QString &text, BadgeKind kind = BadgeKind::Neutral,
                  QWidget *parent = nullptr);

QPushButton *makePrimaryButton(const QString &text, QWidget *parent = nullptr);
QPushButton *makeSecondaryButton(const QString &text, QWidget *parent = nullptr);

// ── Stylesheet helpers shared across visualization windows ───────────────────
// These return fresh QStrings keyed off the current theme so call sites get
// the right palette regardless of when they're invoked.

// QLabel — small uppercase section header inside a sidebar card.
QString sidebarHeaderStyleSheet();

// QLabel — secondary smaller header (no bottom border).
QString sidebarSubHeaderStyleSheet();

// QFrame / QGroupBox — outlined card frame around grouped sidebar widgets.
QString sidebarCardStyleSheet();

// QLabel — muted key text in info grids ("RA", "Dec", …).
QString infoKeyStyleSheet();

// QLabel — primary monospace value text in info grids.
QString infoValueStyleSheet();

// QToolButton — sidebar action button with brand-blue hover / checked tint.
QString sidebarToolButtonStyleSheet();

// ── Layout B viewer toolbar ──────────────────────────────────────────────────
// Slim horizontal bar that anchors a viewer screen: a path-pill on the left
// (icon + selectable file path), one or more tag chips, and an action zone on
// the right (Find / Command / Export). Used at the top of the cube and image
// viewers so they share the same Layout B "screen" header.

struct ViewerToolbarSpec
{
    QString filePath;       // "/Users/foo/cubehi-clean-m31.fits"
    QString primaryTag;     // "CUBE · 1024×1024×112" (cyan accent)
    QString secondaryTag;   // "remote · full-res" (neutral)
};

// Handles to the interactive widgets the toolbar builds, so the caller (or the
// shared WorkspaceChrome helper) can wire behaviour. All are owned by the
// returned toolbar widget; any may be null if not built.
struct ViewerToolbarWidgets
{
    QToolButton *find = nullptr;
    QToolButton *command = nullptr;        // "Command ⌘K"
    QToolButton *exportBtn = nullptr;
    QToolButton *primaryTag = nullptr;     // "CUBE · …" clickable (FITS header)
    QToolButton *datasetSwitch = nullptr;  // "▼" recent-dataset switcher
    QToolButton *theme = nullptr;          // "◐/◑" dark/light toggle
    QLabel      *remoteChip = nullptr;     // live "host · N running" health chip
};

// Returns a fully built QWidget for placement at the top of a viewer's central
// area. `out` (optional) receives handles to the action widgets so the caller
// can wire them — Find / Command / Export, the clickable "CUBE · …" primary
// tag (e.g. opens the FITS header dialog), the dataset switcher, the theme
// toggle, and the live remote/health chip.
QWidget *makeViewerToolbar(const ViewerToolbarSpec &spec,
                           ViewerToolbarWidgets *out = nullptr,
                           QWidget *parent = nullptr);

// Small "Tools → Foo…" breadcrumb chip used in the footer of tool dialogs to
// echo the menu path the user came from. Read-only label, mono-spaced, dim.
QLabel *makeBreadcrumbTag(const QString &text, QWidget *parent = nullptr);

// Renders a Qt-resource SVG icon at the given pixel size, substituting any
// `currentColor` references in the SVG with the active theme's muted-text
// colour (kOnSurfaceVariant). Used by the viewer rail buttons so the icons
// follow the theme without needing per-theme SVG variants.
QPixmap renderThemeIcon(const QString &resourcePath, int size,
                        const QColor *colorOverride = nullptr);

// ── Global theme application ──────────────────────────────────────────────────

// Dark theme (VisIVO navy — canonical baseline).
QString darkStyleSheet();
void applyDarkTo(QApplication *app);

// Light theme (VisIVO brand blues on light backgrounds).
QString lightStyleSheet();
void applyLightTo(QApplication *app);

// Legacy alias — maps to applyDarkTo().
inline void applyTo(QApplication *app) { applyDarkTo(app); }
// Legacy alias — maps to darkStyleSheet().
inline QString globalStyleSheet() { return darkStyleSheet(); }

} // namespace VisivoTheme

#endif // VisivoTheme_h
