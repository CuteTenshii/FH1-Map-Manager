#pragma once

#include "MapData.h"

#include <QAbstractTableModel>

#include <vector>

/// Flat table of every feature in a MapData, one row per feature, listing the
/// top-most layer first to match the layer tree. Qt::DisplayRole gives
/// formatted text; Qt::EditRole gives raw values (numbers for coordinates)
/// for sorting.
class FeatureTableModel : public QAbstractTableModel {
    Q_OBJECT

public:
    /// Name is the feature's label when it has one, else its ID.
    enum Column { Name, Id, Group, LayerTitle, X, Height, Z, ColumnCount };

    struct Location {
        int layer = -1;
        int feature = -1;
    };

    explicit FeatureTableModel(QObject* parent = nullptr);

    /// `map` must outlive the model or be replaced by another call.
    void setMap(const fh1::MapData* map);

    Location locationAt(int row) const;
    int rowOf(int layer, int feature) const;

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    const fh1::MapData* m_map = nullptr;
    /// Layer index shown at each block position, top-most layer first.
    std::vector<int> m_blockLayer;
    /// First row of each block; the last entry is the total row count.
    std::vector<int> m_blockStart;
    /// First row of each layer, indexed by layer.
    std::vector<int> m_layerStart;
};
