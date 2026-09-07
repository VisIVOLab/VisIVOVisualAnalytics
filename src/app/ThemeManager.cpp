#include "ThemeManager.h"

#include "gui/theme/VisivoTheme.h"

#include <QApplication>
#include <QGuiApplication>
#include <QStyleHints>

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

ThemeManager &ThemeManager::instance()
{
    static ThemeManager s_instance;
    return s_instance;
}

ThemeManager::ThemeManager(QObject *parent) : QObject(parent)
{
    // When the user scheme is "System", re-apply whenever the OS dark-mode
    // toggle fires so the effective theme tracks it live.
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                     this, [this](Qt::ColorScheme) {
                         if (m_userScheme == Qt::ColorScheme::Unknown) {
                             if (auto *a = qobject_cast<QApplication *>(
                                         QCoreApplication::instance()))
                                 apply(Qt::ColorScheme::Unknown, a);
                         }
                     });
}

// ---------------------------------------------------------------------------
// Theme resolution
// ---------------------------------------------------------------------------

EffectiveTheme ThemeManager::resolve(Qt::ColorScheme scheme)
{
    if (scheme == Qt::ColorScheme::Light)
        return EffectiveTheme::Light;
    if (scheme == Qt::ColorScheme::Dark)
        return EffectiveTheme::Dark;

    // Unknown → ask the OS; still Unknown → default to Dark (VisIVO native).
    const Qt::ColorScheme osScheme = QGuiApplication::styleHints()->colorScheme();
    return osScheme == Qt::ColorScheme::Light ? EffectiveTheme::Light : EffectiveTheme::Dark;
}

// ---------------------------------------------------------------------------
// Public apply()
// ---------------------------------------------------------------------------

void ThemeManager::apply(Qt::ColorScheme scheme, QApplication *app)
{
    // Restart-only, deliberately. Re-applying mid-session restyles the global
    // QSS and notifies the handful of widgets that subscribe to themeChanged(),
    // but every widget that bakes a theme token into an inline stylesheet at
    // construction — most of the chrome — keeps the palette it was built with.
    // The window then sits half in each theme, which is worse than not
    // switching at all. The preference is still persisted by Settings and is
    // picked up by the next launch; the second apply here would only ever be
    // the Settings dialog or an OS dark-mode flip in "follow the system" mode.
    if (m_applied)
        return;
    m_applied = true;

    m_userScheme = scheme;
    const EffectiveTheme next = resolve(scheme);
    const bool changed = (next != m_effective);
    m_effective = next;

    if (next == EffectiveTheme::Light)
        VisivoTheme::applyLightTo(app);
    else
        VisivoTheme::applyDarkTo(app);

    if (changed)
        emit themeChanged(m_effective);
}

// ---------------------------------------------------------------------------
// VTK text colour
// ---------------------------------------------------------------------------

void ThemeManager::vtkTextColor(double &r, double &g, double &b) const
{
    if (m_effective == EffectiveTheme::Light) {
        r = 0.13; g = 0.13; b = 0.13;   // #212121 — near-black
    } else {
        r = 0.72; g = 0.84; b = 0.91;   // #B8D6E8 — light blue on navy
    }
}
