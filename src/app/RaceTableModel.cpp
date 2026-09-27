#include "RaceTableModel.h"

#include <QColor>
#include <QFont>
#include <QLocale>

RaceTableModel::RaceTableModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

void RaceTableModel::setMap(const fh1::MapData* map)
{
    beginResetModel();
    m_map = map;
    endResetModel();
}

void RaceTableModel::setModifiedRaces(const QSet<int>& races)
{
    if (races != m_modifiedRaces) {
        m_modifiedRaces = races;
        refresh();
    }
}

void RaceTableModel::refresh()
{
    if (rowCount() > 0) {
        emit dataChanged(index(0, 0), index(rowCount() - 1, ColumnCount - 1));
    }
}

int RaceTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() || m_map == nullptr ? 0 : static_cast<int>(m_map->races.size());
}

int RaceTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant RaceTableModel::data(const QModelIndex& index, int role) const
{
    if (m_map == nullptr || !index.isValid() || index.row() >= rowCount()) {
        return {};
    }
    const fh1::Race& race = m_map->races[static_cast<std::size_t>(index.row())];
    const bool numeric = index.column() == Laps || index.column() == Length || index.column() == Prize;
    if (role == Qt::TextAlignmentRole) {
        return numeric ? QVariant(Qt::AlignRight | Qt::AlignVCenter) : QVariant();
    }
    if (race.route < 0 && role == Qt::ForegroundRole) {
        return QColor(Qt::gray);
    }
    const bool modified = m_modifiedRaces.contains(index.row());
    if (modified && role == Qt::FontRole) {
        QFont font;
        font.setItalic(true);
        return font;
    }
    if (modified && role == Qt::ToolTipRole) {
        return tr("Its route or settings have edits that are not saved yet");
    }
    if (race.route < 0 && role == Qt::ToolTipRole) {
        return tr("This install has no route file for route %1, so the race cannot be shown").arg(race.routeId);
    }
    if (role != Qt::DisplayRole && role != Qt::EditRole) {
        return {};
    }
    const bool display = role == Qt::DisplayRole;
    const QLocale locale;
    switch (index.column()) {
    case Name:
        return display && modified ? tr("%1 *").arg(race.name) : race.name;
    case Type:
        return race.type;
    case Route:
        return race.routeName;
    case Laps:
        return race.laps;
    case Length:
        return display ? QVariant(tr("%1 km").arg(locale.toString(race.length / 1000.0, 'f', 1)))
                       : QVariant(race.length);
    case CarClass:
        return race.carClass;
    case Prize:
        return display ? QVariant(tr("%1 CR").arg(locale.toString(race.prize))) : QVariant(race.prize);
    case EventId:
        return race.eventId;
    default:
        return {};
    }
}

QVariant RaceTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    switch (section) {
    case Name:
        return tr("Event");
    case Type:
        return tr("Type");
    case Route:
        return tr("Route");
    case Laps:
        return tr("Laps");
    case Length:
        return tr("Length");
    case CarClass:
        return tr("Class");
    case Prize:
        return tr("Prize");
    case EventId:
        return tr("ID");
    default:
        return {};
    }
}
