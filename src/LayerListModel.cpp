#include "LayerListModel.h"
#include "ImageLayerSet.h"
#include "ImageLayerLoadTask.h"

#include <QDataStream>
#include <QFileInfo>
#include <QIODevice>
#include <QMimeData>
#include <QSize>

LayerListModel::LayerListModel(const std::string &filepath, QObject *parent)
    : QAbstractListModel(parent), layerSet(std::make_unique<ImageLayerSet>(filepath)),
      layerMimeType("application/x-image-layer")
{
}

LayerListModel::LayerListModel(const ImageLayerLoadResult &masterResult, QObject *parent)
    : QAbstractListModel(parent), layerSet(std::make_unique<ImageLayerSet>(masterResult)),
      layerMimeType("application/x-image-layer")
{
}

vtkImageSlice *LayerListModel::getMasterLayerActor() const
{
    return this->layerSet->masterLayerActor();
}

int LayerListModel::getMasterIndex() const
{
    return this->layerSet->masterIndex();
}

LayerListModel::~LayerListModel() = default;

vtkImageSlice *LayerListModel::addLayer(const std::string &filepath)
{
    const int newRow = this->rowCount();
    this->beginInsertRows(QModelIndex(), newRow, newRow);
    auto actor = this->layerSet->addLayer(filepath);
    this->endInsertRows();
    return actor;
}

vtkImageSlice *LayerListModel::addLayer(const ImageLayerLoadResult &result)
{
    const int newRow = this->rowCount();
    this->beginInsertRows(QModelIndex(), newRow, newRow);
    auto actor = this->layerSet->addLayer(result);
    this->endInsertRows();
    return actor;
}

vtkImageSlice *LayerListModel::replaceMasterLayer(const ImageLayerLoadResult &result)
{
    this->beginResetModel();
    auto actor = this->layerSet->replaceMasterLayer(result);
    this->endResetModel();
    return actor;
}

vtkImageSlice *LayerListModel::updateMasterLayer(const ImageLayerLoadResult &result)
{
    this->beginResetModel();
    auto actor = this->layerSet->updateMasterLayer(result);
    this->endResetModel();
    return actor;
}

float LayerListModel::getPixelValue(int index, int x, int y) const
{
    return this->layerSet->pixelValue(index, x, y);
}

vtkImageData *LayerListModel::getImageData(int index) const
{
    return this->layerSet->imageData(index);
}

vtkLookupTable *LayerListModel::getLookupTable(int index) const
{
    return this->layerSet->lookupTable(index);
}

double LayerListModel::getLayerOpacity(int index) const
{
    return this->layerSet->layerOpacity(index);
}

void LayerListModel::setLayerOpacity(int index, double opacity)
{
    if (index < 0 || index >= this->rowCount()) {
        return;
    }

    this->layerSet->setLayerOpacity(index, opacity);
    emit this->dataChanged(this->index(index), this->index(index));
}

bool LayerListModel::usingLogScale(int index) const
{
    return this->layerSet->usingLogScale(index);
}

void LayerListModel::setLogScale(int index, bool flag)
{
    if (index < 0 || index >= this->rowCount()) {
        return;
    }

    this->layerSet->setLogScale(index, flag);
    emit this->dataChanged(this->index(index), this->index(index));
}

LutScale::Mode LayerListModel::scaleMode(int index) const
{
    return this->layerSet->scaleMode(index);
}

double LayerListModel::scaleGamma(int index) const
{
    return this->layerSet->scaleGamma(index);
}

void LayerListModel::setScaleMode(int index, LutScale::Mode mode, double gamma)
{
    if (index < 0 || index >= this->rowCount()) {
        return;
    }

    this->layerSet->setScaleMode(index, mode, gamma);
    emit this->dataChanged(this->index(index), this->index(index));
}

std::string LayerListModel::getColorMapName(int index) const
{
    return this->layerSet->colorMapName(index);
}

void LayerListModel::setColorMap(int index, const std::string &name)
{
    if (index < 0 || index >= this->rowCount()) {
        return;
    }

    this->layerSet->setColorMap(index, name);
    emit this->dataChanged(this->index(index), this->index(index));
}

int LayerListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }

    return this->layerSet->size();
}

QVariant LayerListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) {
        return QVariant();
    }

    switch (role) {
    case Qt::DisplayRole: {
        if (m_displayNameOverrides.contains(index.row()))
            return m_displayNameOverrides.value(index.row());
        const auto full = QString::fromStdString(this->layerSet->filepath(index.row()));
        return QFileInfo(full).fileName();
    }
    case Qt::ToolTipRole: {
        if (m_displayNameOverrides.contains(index.row()))
            return m_displayNameOverrides.value(index.row());
        return QString::fromStdString(this->layerSet->filepath(index.row()));
    }
    case Qt::CheckStateRole: {
        return this->layerSet->isVisible(index.row()) ? Qt::Checked : Qt::Unchecked;
    }
    case Qt::SizeHintRole: {
        return QSize(-1, 36);
    }
    default: {
        return QVariant();
    }
    }
}

vtkImageSlice *LayerListModel::getLayerActor(int index) const
{
    return this->layerSet->layerActor(index);
}

void LayerListModel::setLayerDisplayName(int row, const QString &name)
{
    if (row < 0 || row >= this->rowCount())
        return;
    if (name.isEmpty())
        m_displayNameOverrides.remove(row);
    else
        m_displayNameOverrides.insert(row, name);
    emit this->dataChanged(this->index(row), this->index(row),
                           { Qt::DisplayRole, Qt::ToolTipRole });
}

bool LayerListModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!this->checkIndex(index)) {
        return false;
    }

    if (role == Qt::CheckStateRole) {
        this->layerSet->setVisible(index.row(), value.toBool());
        emit this->dataChanged(index, index, { role });
        return true;
    }

    return false;
}

Qt::ItemFlags LayerListModel::flags(const QModelIndex &index) const
{
    auto defaults = QAbstractListModel::flags(index);
    if (index.isValid()) {
        return defaults | Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled;
    }

    return defaults | Qt::ItemIsDropEnabled;
}

Qt::DropActions LayerListModel::supportedDropActions() const
{
    return Qt::MoveAction;
}

QStringList LayerListModel::mimeTypes() const
{
    return { this->layerMimeType };
}

QMimeData *LayerListModel::mimeData(const QModelIndexList &indexes) const
{
    if (indexes.isEmpty()) {
        return nullptr;
    }

    auto mimeData = new QMimeData;
    QByteArray encodedData;
    QDataStream stream(&encodedData, QIODevice::WriteOnly);
    stream << indexes.front().row();
    mimeData->setData(this->layerMimeType, encodedData);
    return mimeData;
}

bool LayerListModel::canDropMimeData(const QMimeData *data, Qt::DropAction action, int row,
                                     int column, const QModelIndex &parent)
{
    Q_UNUSED(action);
    Q_UNUSED(row);
    Q_UNUSED(column);
    Q_UNUSED(parent);

    return data && data->hasFormat(this->layerMimeType);
}

bool LayerListModel::dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                                  const QModelIndex &parent)
{
    Q_UNUSED(column);

    if (!this->canDropMimeData(data, action, row, column, parent)) {
        return false;
    }

    if (action == Qt::IgnoreAction) {
        return true;
    }

    int destinationRow = row;
    if (destinationRow < 0) {
        destinationRow = parent.isValid() ? parent.row() : this->rowCount();
    }

    QByteArray encodedData = data->data(this->layerMimeType);
    QDataStream stream(&encodedData, QIODevice::ReadOnly);
    int sourceRow = -1;
    stream >> sourceRow;

    if (sourceRow < 0 || sourceRow >= this->rowCount()) {
        return false;
    }

    // A move, announced as a move. This used to reset the model, which had two
    // costs: a reset emits neither dataChanged nor rowsMoved, so the view that
    // renders on dataChanged never repainted — the new stacking order only
    // appeared when something else provoked a render, such as clicking on the
    // VTK widget — and a reset invalidates every persistent index, so the
    // selection fell back to the first layer and the Layer Settings panel
    // stopped describing the layer the user had just dragged.
    //
    // beginMoveRows' destination is an insert-before position, which is exactly
    // what dropMimeData is handed, so the two conventions agree. It rejects a
    // move onto the row's own position; ImageLayerSet::moveLayer rejects the
    // same case, and the check here keeps the two from disagreeing.
    if (destinationRow >= sourceRow && destinationRow <= sourceRow + 1) {
        return false;
    }
    if (!this->beginMoveRows(QModelIndex(), sourceRow, sourceRow, QModelIndex(),
                             destinationRow)) {
        return false;
    }
    const bool moved = this->layerSet->moveLayer(sourceRow, destinationRow);
    this->endMoveRows();

    if (!moved) {
        // Unreachable unless the two validations drift apart. If they ever do,
        // the view has been told about a move that did not happen, and a stale
        // view is worse than a slow one: resynchronise it from scratch.
        this->beginResetModel();
        this->endResetModel();
        return false;
    }

    return true;
}
