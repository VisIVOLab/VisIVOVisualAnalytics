#ifndef ThemeManager_h
#define ThemeManager_h

#include <QObject>
#include <Qt>

class QApplication;

// ---------------------------------------------------------------------------
// EffectiveTheme — always resolved (never "unknown / system")
// ---------------------------------------------------------------------------
enum class EffectiveTheme { Dark, Light };

// ---------------------------------------------------------------------------
// ThemeManager — central runtime theme coordinator
//
// Usage:
//   // At startup, before any window is created:
//   ThemeManager::instance().apply(scheme, &app);
//
//   // To get the current effective theme:
//   ThemeManager::instance().effectiveTheme()   // Dark or Light
//
//   // For VTK text actor foreground color (readable on the renderer bg):
//   double r, g, b;
//   ThemeManager::instance().vtkTextColor(r, g, b);
//
//   // React to runtime theme changes (e.g. OS dark-mode toggle):
//   connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
//           this, [this](EffectiveTheme t) { /* update VTK actors */ });
// ---------------------------------------------------------------------------
class ThemeManager : public QObject
{
    Q_OBJECT

    explicit ThemeManager(QObject *parent = nullptr);

public:
    /// Returns the process-wide singleton.
    static ThemeManager &instance();

    // Apply QPalette + stylesheet for the given scheme.
    // Qt::ColorScheme::Unknown means "follow the OS".
    // Emits themeChanged() if the effective theme changes.
    void apply(Qt::ColorScheme scheme, QApplication *app);

    // Returns the currently active resolved theme.
    EffectiveTheme effectiveTheme() const { return m_effective; }

    // VTK text-actor foreground colour for the active theme.
    //   Dark  → (0.72, 0.84, 0.91)  #B8D6E8 — light blue on navy
    //   Light → (0.13, 0.13, 0.13)  #212121 — near-black on white
    void vtkTextColor(double &r, double &g, double &b) const;

signals:
    void themeChanged(EffectiveTheme theme);

private:
    // Resolve Unknown → Dark or Light by querying QGuiApplication::styleHints().
    static EffectiveTheme resolve(Qt::ColorScheme scheme);

    static void applyDark(QApplication *app);
    static void applyLight(QApplication *app);

    EffectiveTheme m_effective{ EffectiveTheme::Dark };
    Qt::ColorScheme m_userScheme{ Qt::ColorScheme::Unknown };
};

#endif // ThemeManager_h
