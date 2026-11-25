#include "vtkWindowImage.h"
#include "ImageLayerContext.h"
#include "ui_vtkWindowImage.h"

#include "ImageLayerController.h"
#include "LayerBlend.h"
#include "LayerBlendLut.h"
#include "LayerListModel.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <vtkCommand.h>
#include <vtkImageData.h>
#include <vtkImageProperty.h>
#include <vtkImageSlice.h>
#include <vtkImageSliceMapper.h>
#include <vtkImageStack.h>
#include <vtkLookupTable.h>
#include <vtkMatrix4x4.h>
#include <vtkRenderWindow.h>

#include <algorithm>
#include <limits>
#include <cmath>
#include <cstring>

using namespace Qt::Literals::StringLiterals;
using namespace visivo::blend;

// Additive blend and the RGB composite (issue #45).
//
// Each layer already has a colour map, a range and a stretch; what the stack
// lacked was a way to COMBINE layers other than painting one over another. In
// Additive mode the visible layers are summed in colour, per pixel, on the base
// image's grid, and the sum is drawn as one more image on top of the stack. The
// layers themselves are untouched: their cards keep controlling each "channel",
// and switching back to Normal just hides the sum. "Make RGB Composite…" is a
// shortcut that gives the chosen layers black-to-red/green/blue maps and turns
// the mode on.

namespace {

/// The picture on screen is built on at most this many pixels of the base grid;
/// a larger base is shown reduced (the saved PNG is always full size).
constexpr long long kDisplayPixels = 2'500'000;
/// Rows composed at a time, which bounds the scratch memory of a full-size save.
constexpr int kBandRows = 128;
/// The blend image sits above every layer (layer numbers are 0..n-1).
constexpr int kBlendLayerNumber = 1000;
/// A full-size PNG is one RGBA buffer; past this it would not fit.
constexpr long long kMaxSavePixels = 400'000'000;

/// A layer's pixel indices -> the plane the window draws in: the image's own
/// origin and spacing, then the actor's transform (the layer's rotation).
Affine2D dataToWorld(vtkImageData *data, vtkImageSlice *actor)
{
    int ext[6];
    double origin[3], spacing[3];
    data->GetExtent(ext);
    data->GetOrigin(origin);
    data->GetSpacing(spacing);
    Affine2D d;
    d.a = spacing[0];
    d.d = spacing[1];
    d.e = origin[0] + spacing[0] * ext[0];
    d.f = origin[1] + spacing[1] * ext[2];

    vtkMatrix4x4 *m = actor->GetMatrix();
    Affine2D w;
    w.a = m->GetElement(0, 0);
    w.b = m->GetElement(0, 1);
    w.c = m->GetElement(1, 0);
    w.d = m->GetElement(1, 1);
    w.e = m->GetElement(0, 3);
    w.f = m->GetElement(1, 3);
    return w.after(d);
}

/// The pixels of \a data as floats: the image's own buffer when it is a
/// single-component float image (what every FITS layer is), else a converted
/// copy held in \a owned.
FloatImage floatView(vtkImageData *data, std::vector<float> &owned)
{
    int ext[6];
    data->GetExtent(ext);
    FloatImage view;
    view.width = ext[1] - ext[0] + 1;
    view.height = ext[3] - ext[2] + 1;
    if (view.width < 1 || view.height < 1)
        return {};
    if (data->GetScalarType() == VTK_FLOAT && data->GetNumberOfScalarComponents() == 1) {
        view.data = static_cast<const float *>(data->GetScalarPointer(ext[0], ext[2], ext[4]));
        return view;
    }
    owned.resize(static_cast<std::size_t>(view.width) * static_cast<std::size_t>(view.height));
    std::size_t i = 0;
    for (int y = 0; y < view.height; ++y)
        for (int x = 0; x < view.width; ++x)
            owned[i++] = static_cast<float>(
                    data->GetScalarComponentAsDouble(ext[0] + x, ext[2] + y, ext[4], 0));
    view.data = owned.data();
    return view;
}

/// One visible layer in the sum.
struct Part
{
    vtkSmartPointer<vtkImageData> data;
    vtkSmartPointer<vtkLookupTable> lut;
    Affine2D toBase;        ///< its pixel indices -> the base image's
    double weight{ 1.0 };   ///< the layer's opacity
};

/// The base image's grid and the layers that go into the sum, in stack order.
struct Parts
{
    vtkSmartPointer<vtkImageData> baseData;
    vtkSmartPointer<vtkImageSlice> baseActor;
    Affine2D baseWorld;     ///< the base image's pixel indices -> the plane
    int width{ 0 };
    int height{ 0 };
    std::vector<Part> parts;
};

/// What the window's layers contribute right now; false when nothing can be
/// blended (no base image, no visible layer).
bool collectParts(const LayerListModel &layers, Parts &out)
{
    const int master = layers.getMasterIndex();
    out.baseData = layers.getImageData(master);
    out.baseActor = layers.getLayerActor(master);
    if (!out.baseData || !out.baseActor)
        return false;
    int ext[6];
    out.baseData->GetExtent(ext);
    out.width = ext[1] - ext[0] + 1;
    out.height = ext[3] - ext[2] + 1;
    if (out.width < 1 || out.height < 1)
        return false;
    out.baseWorld = dataToWorld(out.baseData, out.baseActor);
    bool ok = false;
    const Affine2D worldToBase = out.baseWorld.inverse(&ok);
    if (!ok)
        return false;

    for (int row = 0; row < layers.rowCount(); ++row) {
        vtkImageSlice *actor = layers.getLayerActor(row);
        vtkImageData *data = layers.getImageData(row);
        vtkLookupTable *lut = layers.getLookupTable(row);
        if (!actor || !data || !lut || !actor->GetVisibility())
            continue;
        int e[6];
        data->GetExtent(e);
        if (e[1] < e[0] || e[3] < e[2])
            continue;
        Part p;
        p.data = data;
        p.lut = lut;
        p.toBase = worldToBase.after(dataToWorld(data, actor));
        p.weight = layers.getLayerOpacity(row);
        out.parts.push_back(std::move(p));
    }
    return !out.parts.empty();
}

/// The sum of the layers on one band: \a values[k] holds layer k's resampled
/// pixels of the band (NaN = no data), \a out receives the band's RGBA pixels.
void blendBand(const std::vector<const float *> &values, const std::vector<vtkLookupTable *> &luts,
               const std::vector<double> &weights, std::size_t pixels, Rgba8 *out,
               std::vector<Accum> &acc, std::vector<unsigned char> &rgba)
{
    acc.assign(pixels, Accum{});
    rgba.resize(pixels * 4);
    std::vector<float> clamped;
    for (std::size_t k = 0; k < values.size(); ++k) {
        // The table maps with the layer's range, stretch and colour map, so the
        // blend is made of exactly the colours the layer shows on its own.
        // mapThroughTable clamps the values to the range first: a saturated
        // pixel takes the top colour whatever the table's out-of-range
        // settings, which a copy made for the worker did not keep (saved PNGs
        // showed 0 where the screen showed the brightest colour).
        mapThroughTable(luts[k], values[k], pixels, rgba.data(), clamped);
        for (std::size_t p = 0; p < pixels; ++p) {
            if (!std::isnan(values[k][p]))
                accumulate(acc[p], &rgba[p * 4], weights[k]);
        }
    }
    for (std::size_t p = 0; p < pixels; ++p)
        out[p] = finish(acc[p]);
}

/// The full-size composite, written to \a path. Runs on a worker thread with
/// its own copies of the lookup tables; returns an error text, empty on success.
QString writeBlendPng(std::vector<Part> parts, int width, int height, QString path)
{
    if (static_cast<long long>(width) * height > kMaxSavePixels)
        return u"The image is too large to save in one piece (%1 x %2 pixels)."_s.arg(width).arg(
                height);
    QImage image(width, height, QImage::Format_RGBA8888);
    if (image.isNull())
        return u"There is not enough memory for a %1 x %2 image."_s.arg(width).arg(height);

    std::vector<std::vector<float>> owned(parts.size());
    std::vector<FloatImage> views;
    std::vector<vtkLookupTable *> luts;
    std::vector<double> weights;
    for (std::size_t k = 0; k < parts.size(); ++k) {
        views.push_back(floatView(parts[k].data, owned[k]));
        luts.push_back(parts[k].lut);
        weights.push_back(parts[k].weight);
    }

    std::vector<std::vector<float>> band(parts.size());
    std::vector<Rgba8> out;
    std::vector<Accum> acc;
    std::vector<unsigned char> rgba;
    for (int row0 = 0; row0 < height; row0 += kBandRows) {
        const int row1 = std::min(height, row0 + kBandRows);
        const std::size_t pixels = static_cast<std::size_t>(row1 - row0) * static_cast<std::size_t>(width);
        std::vector<const float *> ptrs;
        for (std::size_t k = 0; k < parts.size(); ++k) {
            band[k].resize(pixels);
            resampleRows(views[k], parts[k].toBase, 1, width, row0, row1, band[k].data());
            ptrs.push_back(band[k].data());
        }
        out.resize(pixels);
        blendBand(ptrs, luts, weights, pixels, out.data(), acc, rgba);
        // The data's row 0 is the bottom of the sky; a picture starts at the top.
        for (int y = row0; y < row1; ++y)
            std::memcpy(image.scanLine(height - 1 - y),
                        out.data() + static_cast<std::size_t>(y - row0) * static_cast<std::size_t>(width),
                        static_cast<std::size_t>(width) * sizeof(Rgba8));
    }
    if (!image.save(path, "PNG"))
        return u"Could not write %1."_s.arg(path);
    return {};
}

}   // namespace

/// The blend image and what it was built from, so a frame that changes nothing
/// the sum depends on costs a few comparisons instead of a recomputation.
struct vtkWindowImage::BlendState
{
    vtkSmartPointer<vtkImageSlice> actor;
    vtkSmartPointer<vtkImageData> image;
    vtkSmartPointer<vtkMatrix4x4> matrix;
    bool inStack{ false };

    /// One layer's pixels on the (reduced) base grid. Keyed by the image it
    /// came from and where that image sits, since resampling is the costly part
    /// and a change of colour range or stretch does not touch it.
    struct Resampled
    {
        vtkSmartPointer<vtkImageData> data;
        unsigned long mtime{ 0 };
        Affine2D toBase;
        std::vector<float> values;
    };
    std::vector<Resampled> cache;

    vtkSmartPointer<vtkImageData> baseData;
    unsigned long baseMtime{ 0 };
    Affine2D baseWorld;
    int step{ 0 };
    std::vector<unsigned long long> colourSignature;
};

namespace {
bool sameAffine(const Affine2D &p, const Affine2D &q)
{
    return p.a == q.a && p.b == q.b && p.c == q.c && p.d == q.d && p.e == q.e && p.f == q.f;
}
}   // namespace

void vtkWindowImage::setupLayerBlend()
{
    this->m_blend = std::make_shared<BlendState>();

    this->m_actBlendAdditive = new QAction(u"Additive Blend"_s, this);
    this->m_actBlendAdditive->setCheckable(true);
    this->m_actBlendAdditive->setToolTip(
            u"Add the visible layers' colours together instead of painting each over the one "
            u"below. A layer's opacity is its weight. Red, green and blue layers sum to white "
            u"where all three are bright."_s);
    this->m_actMakeRgb = new QAction(u"Make RGB Composite…"_s, this);
    this->m_actMakeRgb->setToolTip(
            u"Pick three or more layers, give each a black-to-colour map (red, green, blue, ...) "
            u"and blend them additively. Each layer's range and stretch stay on its own card."_s);
    this->m_actSaveBlend = new QAction(u"Save Blended Image as PNG…"_s, this);
    this->m_actSaveBlend->setToolTip(
            u"Write the additive blend at the full resolution of the base image: the pixels "
            u"only, without axes, grid or colour bar (the publication figure draws one image)."_s);
    this->m_actSaveBlend->setEnabled(false);

    ui->menuView->addAction(this->m_actBlendAdditive);
    ui->menuView->addAction(this->m_actMakeRgb);
    QObject::connect(this->m_actBlendAdditive, &QAction::toggled, this,
                     &vtkWindowImage::setBlendAdditive);
    QObject::connect(this->m_actMakeRgb, &QAction::triggered, this,
                     &vtkWindowImage::makeRgbComposite);
    QObject::connect(this->m_actSaveBlend, &QAction::triggered, this,
                     &vtkWindowImage::saveBlendPng);

    // The same switch beside the layer list, where the layers are.
    if (ui->hboxLayerListHdr) {
        this->m_blendCheck = new QCheckBox(u"Additive"_s);
        this->m_blendCheck->setToolTip(this->m_actBlendAdditive->toolTip());
        auto *rgbButton = new QPushButton(u"RGB…"_s);
        rgbButton->setFlat(true);
        rgbButton->setCursor(Qt::PointingHandCursor);
        rgbButton->setToolTip(this->m_actMakeRgb->toolTip());
        ui->hboxLayerListHdr->addStretch();
        ui->hboxLayerListHdr->addWidget(this->m_blendCheck);
        ui->hboxLayerListHdr->addWidget(rgbButton);
        QObject::connect(this->m_blendCheck, &QCheckBox::toggled, this->m_actBlendAdditive,
                         &QAction::setChecked);
        QObject::connect(rgbButton, &QPushButton::clicked, this->m_actMakeRgb, &QAction::trigger);
    }

    if (auto *win = ui->vtk->renderWindow())
        win->AddObserver(vtkCommand::StartEvent, this, &vtkWindowImage::onBlendRenderStart);
}

void vtkWindowImage::setBlendAdditive(bool on)
{
    // The action and the box are two views of one switch: setting one fires the
    // other's toggled(), which comes back here. Only the outermost call acts.
    if (this->m_blendSyncing)
        return;
    this->m_blendSyncing = true;
    this->m_blendAdditive = on;
    if (this->m_actBlendAdditive)
        this->m_actBlendAdditive->setChecked(on);
    if (this->m_blendCheck)
        this->m_blendCheck->setChecked(on);
    this->m_blendSyncing = false;
    if (this->m_actSaveBlend)
        this->m_actSaveBlend->setEnabled(on);
    // Built now, outside a render, so the image joins the stack before a frame.
    if (on)
        this->onBlendRenderStart();
    this->vtkRender();
    this->statusBar()->showMessage(
            on ? u"Additive blend: the visible layers' colours are summed."_s
               : u"Normal blend: each layer is drawn over the one below."_s,
            4000);
}

void vtkWindowImage::onBlendRenderStart()
{
    if (!this->m_blend || !this->layers)
        return;
    BlendState &b = *this->m_blend;
    const auto hide = [&b]() {
        if (b.actor)
            b.actor->SetVisibility(false);
    };
    if (!this->m_blendAdditive) {
        hide();
        return;
    }
    Parts src;
    if (!collectParts(*this->layers, src)) {
        hide();
        return;
    }

    const int step = displayStep(src.width, src.height, kDisplayPixels);
    const int outW = (src.width + step - 1) / step;
    const int outH = (src.height + step - 1) / step;
    const std::size_t outPixels = static_cast<std::size_t>(outW) * static_cast<std::size_t>(outH);

    // What depends on where the images are: redone only when they moved.
    bool geometryChanged = b.step != step || b.baseData.Get() != src.baseData.Get()
            || b.baseMtime != src.baseData->GetMTime() || !sameAffine(b.baseWorld, src.baseWorld)
            || b.cache.size() != src.parts.size() || !b.image;
    b.cache.resize(src.parts.size());
    for (std::size_t k = 0; k < src.parts.size(); ++k) {
        const Part &p = src.parts[k];
        BlendState::Resampled &c = b.cache[k];
        if (c.data.Get() == p.data.Get() && c.mtime == p.data->GetMTime()
            && sameAffine(c.toBase, p.toBase) && c.values.size() == outPixels
            && !geometryChanged)
            continue;
        std::vector<float> owned;
        const FloatImage view = floatView(p.data, owned);
        c.values.resize(outPixels);
        resampleRows(view, p.toBase, step, outW, 0, outH, c.values.data());
        c.data = p.data;
        c.mtime = p.data->GetMTime();
        c.toBase = p.toBase;
        geometryChanged = true;
    }

    // What depends on colour: each layer's table (range, stretch, map) and weight.
    std::vector<unsigned long long> signature;
    for (const Part &p : src.parts) {
        signature.push_back(p.lut->GetMTime());
        unsigned long long bits = 0;
        std::memcpy(&bits, &p.weight, sizeof(bits));
        signature.push_back(bits);
    }
    const bool coloursChanged = signature != b.colourSignature;

    if (geometryChanged || coloursChanged) {
        if (!b.image)
            b.image = vtkSmartPointer<vtkImageData>::New();
        int dims[3];
        b.image->GetDimensions(dims);
        if (dims[0] != outW || dims[1] != outH
            || b.image->GetScalarType() != VTK_UNSIGNED_CHAR
            || b.image->GetNumberOfScalarComponents() != 4) {
            b.image->SetDimensions(outW, outH, 1);
            b.image->AllocateScalars(VTK_UNSIGNED_CHAR, 4);
        }
        // Output pixel 0 stands for base pixels [0, step), centred (step-1)/2.
        int bext[6];
        double bo[3], bs[3];
        src.baseData->GetExtent(bext);
        src.baseData->GetOrigin(bo);
        src.baseData->GetSpacing(bs);
        b.image->SetSpacing(bs[0] * step, bs[1] * step, 1.0);
        b.image->SetOrigin(bo[0] + bs[0] * (bext[0] + (step - 1) / 2.0),
                           bo[1] + bs[1] * (bext[2] + (step - 1) / 2.0), 0.0);

        std::vector<vtkLookupTable *> luts;
        std::vector<double> weights;
        for (const Part &p : src.parts) {
            luts.push_back(p.lut);
            weights.push_back(p.weight);
        }
        auto *out = reinterpret_cast<Rgba8 *>(b.image->GetScalarPointer());
        std::vector<Accum> acc;
        std::vector<unsigned char> rgba;
        for (int row0 = 0; row0 < outH; row0 += kBandRows) {
            const int rows = std::min(kBandRows, outH - row0);
            const std::size_t first = static_cast<std::size_t>(row0) * static_cast<std::size_t>(outW);
            std::vector<const float *> ptrs;
            for (const auto &c : b.cache)
                ptrs.push_back(c.values.data() + first);
            blendBand(ptrs, luts, weights, static_cast<std::size_t>(rows) * static_cast<std::size_t>(outW),
                      out + first, acc, rgba);
        }
        b.image->Modified();

        b.baseData = src.baseData;
        b.baseMtime = src.baseData->GetMTime();
        b.baseWorld = src.baseWorld;
        b.step = step;
        b.colourSignature = std::move(signature);
    }

    if (!b.actor) {
        b.actor = vtkSmartPointer<vtkImageSlice>::New();
        vtkNew<vtkImageSliceMapper> mapper;
        mapper->SetInputData(b.image);
        mapper->BorderOn();
        b.actor->SetMapper(mapper);
        b.actor->GetProperty()->SetInterpolationTypeToNearest();
        b.actor->GetProperty()->SetLayerNumber(kBlendLayerNumber);
        b.matrix = vtkSmartPointer<vtkMatrix4x4>::New();
        b.actor->SetUserMatrix(b.matrix);
    }
    if (!b.inStack) {
        this->stack->AddImage(b.actor);
        b.inStack = true;
    }
    // The image is in the base's data coordinates, so it follows the base's turn.
    // (Only when it changed: a copy marks the stack modified on every frame.)
    vtkMatrix4x4 *baseMatrix = src.baseActor->GetMatrix();
    bool matrixChanged = false;
    for (int i = 0; i < 4 && !matrixChanged; ++i)
        for (int j = 0; j < 4; ++j)
            matrixChanged = matrixChanged || b.matrix->GetElement(i, j) != baseMatrix->GetElement(i, j);
    if (matrixChanged)
        b.matrix->DeepCopy(baseMatrix);
    b.actor->SetVisibility(true);
}

double vtkWindowImage::layerWavelengthMetres(int row)
{
    const QString path = this->layers ? this->layers->layerFilepath(row) : QString();
    if (path.isEmpty())
        return std::numeric_limits<double>::quiet_NaN();
    const QStringList cards = this->layerCardsForPath(path);
    const auto number = [&cards](const QString &key) {
        bool ok = false;
        const double v = visivo::layerctx::headerKeyword(cards, key).toDouble(&ok);
        return ok && std::isfinite(v) && v > 0.0 ? v : std::numeric_limits<double>::quiet_NaN();
    };
    for (const char *key : { "WAVELENG", "WAVELEN", "WAVELNTH" }) {
        const double v = number(QString::fromLatin1(key));
        if (std::isfinite(v))
            return v;
    }
    // A spectral axis of a 2-D image (degenerate third axis).
    for (int axis = 3; axis <= 4; ++axis) {
        const QString ctype = visivo::layerctx::headerKeyword(cards, u"CTYPE%1"_s.arg(axis)).trimmed().toUpper();
        const double crval = number(u"CRVAL%1"_s.arg(axis));
        if (!std::isfinite(crval))
            continue;
        if (ctype.startsWith(u"FREQ"_s))
            return 299792458.0 / crval;
        if (ctype.startsWith(u"WAVE"_s))
            return crval;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

void vtkWindowImage::makeRgbComposite()
{
    if (!this->layers || this->layers->rowCount() < 2) {
        QMessageBox::information(this, u"Make RGB Composite"_s,
                                 u"An RGB composite needs at least two images. Add the others "
                                 u"first with Add Image Layer."_s);
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(u"Make RGB Composite"_s);
    auto *layout = new QVBoxLayout(&dialog);
    auto *help = new QLabel(
            u"Tick the images to combine and drag them into order: the top one is red, then green "
            u"and blue (with more, red through blue by hue). When the headers give a wavelength "
            u"the list starts sorted with the longest on top. The blend is additive. Afterwards "
            u"each layer's range and stretch are set on its own card."_s,
            &dialog);
    help->setWordWrap(true);
    layout->addWidget(help);

    auto *list = new QListWidget(&dialog);
    const auto addRow = [this, list](int row) {
        auto *item = new QListWidgetItem(
                this->layers->data(this->layers->index(row), Qt::DisplayRole).toString(), list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
        item->setData(Qt::UserRole, row);
    };
    // Longest wavelength first when every header says it (WAVELENG/WAVELEN/
    // WAVELNTH in metres, or a FREQ/WAVE axis); otherwise the layer order.
    std::vector<std::pair<double, int>> byWavelength;
    for (int row = 0; row < this->layers->rowCount(); ++row) {
        const double lambda = this->layerWavelengthMetres(row);
        byWavelength.emplace_back(lambda, row);
    }
    const bool allKnown = std::all_of(byWavelength.begin(), byWavelength.end(),
                                      [](const auto &w) { return std::isfinite(w.first); });
    if (allKnown)
        std::stable_sort(byWavelength.begin(), byWavelength.end(),
                         [](const auto &a, const auto &b) { return a.first > b.first; });
    for (const auto &w : byWavelength)
        addRow(w.second);
    // The order is the colour assignment: rows can be dragged.
    list->setDragDropMode(QAbstractItemView::InternalMove);
    list->setDefaultDropAction(Qt::MoveAction);
    layout->addWidget(list);

    // A file that is not a layer yet: it joins the list when it has loaded.
    auto *addImage = new QPushButton(u"Add image file…"_s, &dialog);
    layout->addWidget(addImage);
    QObject::connect(addImage, &QPushButton::clicked, this, &vtkWindowImage::addLayerFromBrowser);
    QObject::connect(this->layers, &QAbstractItemModel::rowsInserted, &dialog,
                     [this, addRow](const QModelIndex &, int first, int last) {
                         for (int row = first; row <= last; ++row)
                             addRow(row);
                     });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted)
        return;

    std::vector<int> chosen;
    std::vector<int> left;
    for (int i = 0; i < list->count(); ++i) {
        const int row = list->item(i)->data(Qt::UserRole).toInt();
        (list->item(i)->checkState() == Qt::Checked ? chosen : left).push_back(row);
    }
    if (chosen.size() < 2) {
        QMessageBox::information(this, u"Make RGB Composite"_s,
                                 u"Tick at least two images."_s);
        return;
    }
    const int n = static_cast<int>(chosen.size());
    for (int k = 0; k < n; ++k) {
        const int row = chosen[static_cast<std::size_t>(k)];
        this->layerController->setCurrentColorMap(row, hueMapName(k, n));
        // Full weight: the sum, not the opacity, mixes the colours.
        this->layerController->setCurrentOpacity(row, 1.0);
        this->layers->setData(this->layers->index(row), true, Qt::CheckStateRole);
    }
    // Layers left out must not be in the sum.
    for (int row : left)
        this->layers->setData(this->layers->index(row), false, Qt::CheckStateRole);
    this->setBlendAdditive(true);
}

void vtkWindowImage::saveBlendPng()
{
    if (!this->m_blendAdditive || !this->layers)
        return;
    const QString path = QFileDialog::getSaveFileName(
            this, u"Save Blended Image"_s,
            QFileInfo(this->filepath).completeBaseName() + u"_rgb.png"_s, u"PNG image (*.png)"_s);
    if (path.isEmpty())
        return;
    // After the dialog, so the layers saved are the ones on screen now.
    Parts src;
    if (!collectParts(*this->layers, src)) {
        this->statusBar()->showMessage(u"There is no visible layer to save."_s, 4000);
        return;
    }

    // The worker gets its own copies of the tables: the window's go on changing.
    for (Part &p : src.parts) {
        auto copy = vtkSmartPointer<vtkLookupTable>::New();
        copy->DeepCopy(p.lut);
        p.lut = copy;
    }
    this->showPersistentStatusMessage(u"Saving %1..."_s.arg(QFileInfo(path).fileName()));
    auto *watcher = new QFutureWatcher<QString>(this);
    QObject::connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, path]() {
        const QString error = watcher->result();
        watcher->deleteLater();
        if (error.isEmpty()) {
            this->statusBar()->showMessage(u"Saved %1"_s.arg(path), 6000);
        } else {
            this->statusBar()->clearMessage();
            QMessageBox::warning(this, u"Save Blended Image"_s, error);
        }
    });
    watcher->setFuture(QtConcurrent::run(&writeBlendPng, std::move(src.parts), src.width,
                                         src.height, path));
}
