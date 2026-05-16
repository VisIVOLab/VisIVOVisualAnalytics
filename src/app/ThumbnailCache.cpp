#include "ThumbnailCache.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QLinearGradient>
#include <QOpenGLWidget>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QStandardPaths>
#include <QWidget>

#include <random>

using namespace Qt::StringLiterals;

namespace {

QString cacheDir()
{
    const QString base = QStandardPaths::writableLocation(
            QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty()) return {};
    const QString dir = base + u"/thumbnails"_s;
    QDir().mkpath(dir);
    return dir;
}

// SHA1(id) → filename. Stable across runs, opaque to the user.
QString hashKey(const QString &id)
{
    return QString::fromLatin1(
            QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha1).toHex());
}

// Center-cropped scale: result fills the target rect with no letterboxing,
// preserving aspect ratio of the source. Matches the design's `xMidYMid slice`.
QPixmap scaleToFill(const QPixmap &src, int w, int h)
{
    if (src.isNull()) return {};
    const QPixmap scaled = src.scaled(QSize(w, h), Qt::KeepAspectRatioByExpanding,
                                       Qt::SmoothTransformation);
    const int dx = (scaled.width()  - w) / 2;
    const int dy = (scaled.height() - h) / 2;
    return scaled.copy(dx, dy, w, h);
}

} // anonymous namespace

namespace ThumbnailCache {

QString pathFor(const QString &id)
{
    if (id.isEmpty()) return {};
    const QString dir = cacheDir();
    if (dir.isEmpty()) return {};
    return dir + u"/"_s + hashKey(id) + u".png"_s;
}

bool save(const QString &id, const QPixmap &pixmap)
{
    if (pixmap.isNull()) return false;
    const QString out = pathFor(id);
    if (out.isEmpty()) return false;

    // Atomic save: write to .tmp and rename, so a crash mid-save can't leave
    // a corrupt PNG that the loader would later try to display.
    const QString tmp = out + u".tmp"_s;
    if (!pixmap.save(tmp, "PNG", /*quality=*/85)) return false;
    QFile::remove(out);                            // ignore errors (may not exist)
    return QFile::rename(tmp, out);
}

QPixmap load(const QString &id, int width, int height)
{
    const QString src = pathFor(id);
    if (src.isEmpty() || !QFile::exists(src)) return {};

    QImageReader r(src);
    r.setAutoTransform(true);
    const QImage img = r.read();
    if (img.isNull()) return {};

    QPixmap pm = QPixmap::fromImage(img);
    if (pm.width() == width && pm.height() == height) return pm;
    return scaleToFill(pm, width, height);
}

QString captureFromWidget(QWidget *widget, const QString &id)
{
    if (!widget || id.isEmpty()) return {};

    QPixmap grabbed;
    // VTK's QVTKOpenGLNativeWidget inherits from QOpenGLWidget. QWidget::grab()
    // captures Qt's CPU surface and returns a black or transparent pixmap for
    // GL-backed widgets on Qt 6 — grabFramebuffer() reads back the actual GL
    // backbuffer, which is what we want.
    if (auto *glw = qobject_cast<QOpenGLWidget *>(widget)) {
        const QImage fb = glw->grabFramebuffer();
        if (!fb.isNull()) {
            grabbed = QPixmap::fromImage(fb);
        }
    }
    if (grabbed.isNull()) {
        grabbed = widget->grab();
    }
    if (grabbed.isNull()) {
        qWarning() << "[ThumbnailCache] capture failed (null pixmap) for" << id;
        return {};
    }

    const QPixmap thumb = scaleToFill(grabbed, kDefaultWidth, kDefaultHeight);
    if (!save(id, thumb)) {
        qWarning() << "[ThumbnailCache] save failed for" << id;
        return {};
    }
    qDebug().noquote()
            << QStringLiteral("[ThumbnailCache] saved %1×%2 thumbnail for %3")
                       .arg(thumb.width()).arg(thumb.height()).arg(id);
    return pathFor(id);
}

// ── Placeholder generation ──────────────────────────────────────────────────
// The Mission Control design uses small typed SVG placeholders before any
// real thumbnail has been captured. We approximate the same look in a
// painted QPixmap so we don't need an SVG resource per type.

QPixmap placeholder(const QString &type, int width, int height)
{
    QPixmap pm(width, height);
    pm.fill(QColor(QStringLiteral("#04060a")));   // viewport-canvas dark per design

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QString k = type.trimmed().toLower();

    auto addStars = [&](int count, double maxR = 0.7) {
        std::mt19937 rng(0xC0FFEEu);  // deterministic so placeholders don't shimmer
        std::uniform_real_distribution<double> ux(0.0, width);
        std::uniform_real_distribution<double> uy(0.0, height);
        std::uniform_real_distribution<double> ur(0.2, maxR);
        std::uniform_real_distribution<double> ua(0.4, 0.95);
        for (int i = 0; i < count; ++i) {
            const QColor c(255, 255, 255, int(ua(rng) * 255));
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawEllipse(QPointF(ux(rng), uy(rng)), ur(rng), ur(rng));
        }
    };

    if (k == u"cube"_s || k == u"vbt-volume"_s) {
        // Inferno-ish radial blob + sparse stars.
        QRadialGradient g(width * 0.5, height * 0.5,
                          std::max(width, height) * 0.55);
        g.setColorAt(0.00, QColor(QStringLiteral("#fde725")));
        g.setColorAt(0.20, QColor(QStringLiteral("#6ece58")));
        g.setColorAt(0.50, QColor(QStringLiteral("#1f9e89")));
        g.setColorAt(0.80, QColor(QStringLiteral("#440154")));
        g.setColorAt(1.00, QColor(QStringLiteral("#04060a")));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        QTransform t;
        t.translate(width * 0.5, height * 0.5);
        t.rotate(-22);
        t.translate(-width * 0.5, -height * 0.5);
        p.setTransform(t);
        p.drawEllipse(QPointF(width * 0.5, height * 0.5),
                      width * 0.36, height * 0.18);
        p.resetTransform();
        addStars(8);
    } else if (k == u"image"_s) {
        // Soft elongated viridis-tinted glow + faint stars.
        QLinearGradient g(0, 0, width, 0);
        g.setColorAt(0.0, QColor(QStringLiteral("#1f9e89")));
        g.setColorAt(0.5, QColor(QStringLiteral("#fde725")));
        g.setColorAt(1.0, QColor(QStringLiteral("#1f9e89")));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 210, 80, int(0.5 * 255)));
        QTransform t;
        t.translate(width * 0.5, height * 0.5);
        t.rotate(-8);
        t.translate(-width * 0.5, -height * 0.5);
        p.setTransform(t);
        p.drawEllipse(QPointF(width * 0.5, height * 0.5),
                      width * 0.42, height * 0.10);
        p.setBrush(QColor(255, 255, 255, int(0.5 * 255)));
        p.drawEllipse(QPointF(width * 0.5, height * 0.5),
                      width * 0.24, height * 0.05);
        p.resetTransform();
        addStars(6);
    } else if (k == u"catalogue"_s) {
        // Scatter plot: cyan dots with subtle connections.
        const QColor dot(QStringLiteral("#4fc3f7"));
        std::mt19937 rng(0xCA7AB1EEu);
        std::uniform_real_distribution<double> ux(width * 0.08, width * 0.92);
        std::uniform_real_distribution<double> uy(height * 0.12, height * 0.88);
        std::uniform_real_distribution<double> ur(0.7, 1.4);
        QList<QPointF> pts;
        p.setPen(Qt::NoPen);
        p.setBrush(dot);
        for (int i = 0; i < 12; ++i) {
            const QPointF pt(ux(rng), uy(rng));
            pts.append(pt);
            p.drawEllipse(pt, ur(rng), ur(rng));
        }
        p.setPen(QPen(QColor(79, 195, 247, 60), 0.5));
        for (int i = 0; i + 1 < pts.size(); i += 2) {
            p.drawLine(pts[i], pts[i + 1]);
        }
    } else if (k == u"vbt"_s) {
        // Small grid of warm + cyan dots — point-cloud feel.
        std::mt19937 rng(0xD0C5DA7Au);
        std::uniform_real_distribution<double> uy(height * 0.18, height * 0.82);
        const QColor cols[3] = {
                QColor(QStringLiteral("#4fc3f7")),
                QColor(QStringLiteral("#a4d65e")),
                QColor(255, 255, 255),
        };
        p.setPen(Qt::NoPen);
        for (int x = 6; x < width - 6; x += 8) {
            const QColor c = cols[rng() % 3];
            p.setBrush(c);
            p.drawRect(QRectF(x, uy(rng), 2, 2));
        }
    } else if (k == u"hips"_s) {
        // All-sky radial vignette: hint at the spherical projection.
        QRadialGradient g(width * 0.5, height * 0.5,
                          std::max(width, height) * 0.6);
        g.setColorAt(0.0, QColor(QStringLiteral("#1a3550")));
        g.setColorAt(1.0, QColor(QStringLiteral("#04060a")));
        p.fillRect(0, 0, width, height, g);
        p.setPen(QPen(QColor(255, 255, 255, 40), 0.6));
        for (int i = 1; i < 4; ++i) {
            const double r = (width * 0.5) * (i / 4.0);
            p.drawEllipse(QPointF(width * 0.5, height * 0.5), r, r * 0.55);
        }
        addStars(14, 1.0);
    } else {
        // Fallback: subtle navy gradient with brand-blue arc.
        QLinearGradient g(0, 0, 0, height);
        g.setColorAt(0, QColor(QStringLiteral("#11181F")));
        g.setColorAt(1, QColor(QStringLiteral("#04060a")));
        p.fillRect(0, 0, width, height, g);
        p.setPen(QPen(QColor(QStringLiteral("#048ABF")), 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(width * 0.15, height * 0.2,
                          width * 0.7, height * 0.9),
                  20 * 16, 140 * 16);
    }
    p.end();

    return pm;
}

} // namespace ThumbnailCache
