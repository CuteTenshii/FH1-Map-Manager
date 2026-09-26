#include "FeatureTableModel.h"

#include <algorithm>

FeatureTableModel::FeatureTableModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

void FeatureTableModel::setMap(const fh1::MapData* map)
{
    beginResetModel();
    m_map = map;
    m_blockLayer.clear();
    m_blockStart.clear();
    m_layerStart.clear();
    if (m_map != nullptr) {
        const auto layerCount = static_cast<int>(m_map->layers.size());
        m_layerStart.assign(static_cast<std::size_t>(layerCount), 0);
        int start = 0;
        for (int layer = layerCount - 1; layer >= 0; --layer) {
            m_blockLayer.push_back(layer);
            m_blockStart.push_back(start);
            m_layerStart[static_cast<std::size_t>(layer)] = start;
            start += static_cast<int>(m_map->layers[static_cast<std::size_t>(layer)].features.size());
        }
        m_blockStart.push_back(start);
    }
    endResetModel();
}

FeatureTableModel::Location FeatureTableModel::locationAt(int row) const
{
    if (m_blockStart.size() < 2 || row < 0 || row >= m_blockStart.back()) {
        return {};
    }
    const auto it = std::upper_bound(m_blockStart.begin(), m_blockStart.end(), row);
    const auto block = static_cast<std::size_t>(std::distance(m_blockStart.begin(), it) - 1);
    return {m_blockLayer[block], row - m_blockStart[block]};
}

int FeatureTableModel::rowOf(int layer, int feature) const
{
    if (layer < 0 || static_cast<std::size_t>(layer) >= m_layerStart.size()) {
        return -1;
    }
    return m_layerStart[static_cast<std::size_t>(layer)] + feature;
}

int FeatureTableModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid() || m_blockStart.empty()) {
        return 0;
    }
    return m_blockStart.back();
}

int FeatureTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant FeatureTableModel::data(const QModelIndex& index, int role) const
{
    if (m_map == nullptr || !index.isValid() || (role != Qt::DisplayRole && role != Qt::EditRole)) {
        return {};
    }
    const Location location = locationAt(index.row());
    if (location.layer < 0) {
        return {};
    }
    const fh1::Layer& layer = m_map->layers[static_cast<std::size_t>(location.layer)];
    const fh1::Feature& feature = layer.features[static_cast<std::size_t>(location.feature)];
    const bool display = role == Qt::DisplayRole;
    auto coordinate = [display](float value) -> QVariant {
        return display ? QVariant(QString::number(static_cast<double>(value), 'f', 1))
                       : QVariant(static_cast<double>(value));
    };
    switch (index.column()) {
    case Name:
        return feature.label.isEmpty() ? feature.name : feature.label;
    case Id:
        return feature.name;
    case Group:
        return feature.group;
    case LayerTitle:
        return layer.title;
    case X:
        return coordinate(feature.position.x());
    case Height:
        return coordinate(feature.position.y());
    case Z:
        return coordinate(feature.position.z());
    default:
        return {};
    }
}

QVariant FeatureTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    switch (section) {
    case Name:
        return tr("Name");
    case Id:
        return tr("ID");
    case Group:
        return tr("Group");
    case LayerTitle:
        return tr("Layer");
    case X:
        return tr("X");
    case Height:
        return tr("Y (height)");
    case Z:
        return tr("Z");
    default:
        return {};
    }
}
