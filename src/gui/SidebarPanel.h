#ifndef SidebarPanel_h
#define SidebarPanel_h

// ---------------------------------------------------------------------------
// SidebarPanel
//
// A self-contained collapsible sidebar widget used by all main visualization
// windows.  It owns:
//   • a 48-px icon rail on the left
//   • a QStackedWidget with one page per section
//   • a smooth collapse/expand animation (200 ms InOutCubic)
//
// Usage
// ─────
//   auto *sidebar = new SidebarPanel(272, this);
//   sidebar->addPage(":/icons/rail_display.svg",  "Display",  displayPageWidget);
//   sidebar->addPage(":/icons/rail_analysis.svg", "Analysis", analysisPageWidget);
//   sidebar->finalize();
//   mainLayout->addWidget(sidebar);
//   QObject::connect(sidebar, &SidebarPanel::pageChanged, this, &MyWindow::onPage);
//
// Each content widget passed to addPage() is automatically wrapped in a
// per-page QScrollArea (scrollbar on the right, no horizontal bar).
// This prevents any content from being clipped on short windows.
//
// The toggle action returned by createToggleAction() can be added to a
// View menu so users can collapse the sidebar via keyboard shortcut.
// ---------------------------------------------------------------------------

#include <QList>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QWidget>

class QAction;
class QParallelAnimationGroup;
class QScrollArea;
class QStackedWidget;
class QToolButton;
class QVBoxLayout;

class SidebarPanel : public QWidget
{
    Q_OBJECT

public:
    struct PageDef
    {
        QString iconResource; // Qt resource path, e.g. ":/icons/rail_display.svg"
        QString tooltip;
        QWidget *content;     // ownership transferred to SidebarPanel
    };

    // expandedWidth: sidebar width when open (not counting the 48-px rail).
    // Total open width = railWidth (48) + expandedWidth.
    explicit SidebarPanel(int expandedWidth = 272, QWidget *parent = nullptr);

    // Add a page.  Must be called before finalize().
    // Returns the zero-based page index.
    int addPage(const QString &iconResource, const QString &tooltip, QWidget *content);

    // Convenience overload using PageDef.
    int addPage(const PageDef &def);

    // Wire all signals, apply styles, show page 0.  Call once after all pages are added.
    void finalize(int initialPage = 0);

    // Returns a checkable QAction (Ctrl+\) suitable for a View menu.
    // Caller takes ownership of the action.
    QAction *createToggleAction(const QString &label = QStringLiteral("Controls Sidebar"),
                                QObject *parent = nullptr);

    bool isExpanded() const;
    int currentPage() const;

public slots:
    // animated=false used during window construction / restore from settings.
    void setExpanded(bool expanded, bool animated = true);
    void toggleExpanded();
    void switchToPage(int index);

signals:
    void pageChanged(int index);
    void expandedChanged(bool expanded);

private:
    void buildRail();
    void applyStyles();
    void setActiveButton(QToolButton *btn);

    static constexpr int kRailWidth = 48;
    static constexpr int kAnimDurationMs = 200;

    const int m_expandedWidth;
    bool m_expanded{ true };
    int m_pendingPage{ -1 };
    bool m_finalized{ false };

    // Rail
    QWidget *m_rail{ nullptr };
    QVBoxLayout *m_railLayout{ nullptr };
    QToolButton *m_toggleBtn{ nullptr };
    QList<QToolButton *> m_railBtns;

    // Content
    QScrollArea *m_outerArea{ nullptr }; // contains the QStackedWidget
    QStackedWidget *m_stack{ nullptr };

    QPointer<QParallelAnimationGroup> m_animGroup;
    QPointer<QAction> m_toggleAction;

    // Data accumulated before finalize()
    struct PendingPage
    {
        QString icon;
        QString tooltip;
        QWidget *content{ nullptr };
    };
    QList<PendingPage> m_pending;
};

#endif // SidebarPanel_h
