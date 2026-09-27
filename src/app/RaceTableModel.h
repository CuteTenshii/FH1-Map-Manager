#pragma once

#include "MapData.h"

#include <QAbstractTableModel>

/// The race events of a MapData, one row per race. Qt::DisplayRole gives
/// formatted text; Qt::EditRole gives raw values (numbers for laps, length
/// and prize) for sorting.
class RaceTableModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column { Name, Type, Route, Laps, Length, CarClass, Prize, EventId, ColumnCount };

    explicit RaceTableModel(QObject* parent = nullptr);

    /// `map` must outlive the model or be replaced by another call.
    void setMap(const fh1::MapData* map);

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    const fh1::MapData* m_map = nullptr;
};
