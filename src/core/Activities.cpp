#include "Activities.h"

#include "Loaders.h"

#include <QPainter>
#include <QXmlStreamReader>

#include <algorithm>
#include <map>

namespace fh1::activities {

namespace {

[[noreturn]] void failXml(const QString& file, const QXmlStreamReader& xml)
{
    throw LoadError(
        QStringLiteral("%1: line %2: %3").arg(file).arg(xml.lineNumber()).arg(xml.errorString()).toStdString());
}

bool startsWithCaseless(const QString& text, const QString& prefix)
{
    return text.startsWith(prefix, Qt::CaseInsensitive);
}

} // namespace

std::vector<Activity> parse(const QByteArray& xmlData, const QString& file)
{
    std::vector<Activity> result;
    QXmlStreamReader xml(xmlData);
    bool inActivity = false;
    int depth = 0;
    int activityDepth = 0;
    Activity current;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            ++depth;
            const QXmlStreamAttributes attributes = xml.attributes();
            if (!inActivity && xml.name() == QLatin1String("Activity")) {
                inActivity = true;
                activityDepth = depth;
                current = Activity();
                current.type = attributes.value(QLatin1String("type")).toString();
                current.name = attributes.value(QLatin1String("name")).toString();
                current.file = file;
                continue;
            }
            if (!inActivity) {
                continue;
            }
            if (xml.name() == QLatin1String("TriggerZone")) {
                const QString object = attributes.value(QLatin1String("object")).toString();
                if (!object.isEmpty()) {
                    current.triggerObjects.append(object);
                    const QString radius = attributes.value(QLatin1String("radius")).toString();
                    if (!radius.isEmpty()) {
                        current.triggerRadius.insert(object, radius);
                    }
                }
                const QString tag = attributes.value(QLatin1String("mapTag")).toString();
                if (current.mapTag.isEmpty() && !tag.isEmpty()) {
                    current.mapTag = tag;
                }
            } else if (xml.name() == QLatin1String("Behaviour")
                && attributes.value(QLatin1String("id")) == QLatin1String("CPlaceCarAtObject")) {
                const QString object = attributes.value(QLatin1String("object_name")).toString();
                if (!object.isEmpty()) {
                    current.carPlacementObjects.append(object);
                }
            } else if (xml.name() == QLatin1String("UnlockCar")) {
                current.unlockCarId = attributes.value(QLatin1String("id")).toString();
            }
            for (const QXmlStreamAttribute& attribute : attributes) {
                current.referencedValues.append(attribute.value().toString());
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (inActivity && depth == activityDepth) {
                inActivity = false;
                result.push_back(std::move(current));
                current = Activity();
            }
            --depth;
        }
    }
    if (xml.hasError()) {
        failXml(file, xml);
    }
    return result;
}

QString iconCategory(const Activity& activity, const QString& eventId)
{
    if (!activity.mapTag.isEmpty()) {
        return activity.mapTag;
    }
    // Activity types without a mapTag, matched to the map profile's
    // activity_type filters. Speed traps and speed zones have "discovered" and
    // "completed" icons; a viewer shows every one, so "discovered" is used.
    static const QHash<QString, QString> byType{
        {QStringLiteral("ActivityGasStation"), QStringLiteral("gas_station")},
        {QStringLiteral("ActivityFlyers"), QStringLiteral("flyer")},
        {QStringLiteral("ActivitySpeedCamera"), QStringLiteral("speed_camera_discovered")},
        {QStringLiteral("ActivityAverageSpeed"), QStringLiteral("average_speed_camera_discovered")},
        {QStringLiteral("ActivityBarnFind"), QStringLiteral("barnfind")},
    };
    if (activity.type == QLatin1String("ActivityCareerEventActivation")) {
        if (startsWithCaseless(eventId, QStringLiteral("EXHIBITION"))) {
            return QStringLiteral("exhibition");
        }
        if (startsWithCaseless(eventId, QStringLiteral("NEM"))) {
            return QStringLiteral("nemesisrace");
        }
        return QStringLiteral("race");
    }
    return byType.value(activity.type);
}

QString categoryTitle(const QString& iconCategory, const QString& activityType)
{
    static const QHash<QString, QString> byIcon{
        {QStringLiteral("race"), QStringLiteral("Race events")},
        {QStringLiteral("exhibition"), QStringLiteral("Exhibitions")},
        {QStringLiteral("nemesisrace"), QStringLiteral("Nemesis races")},
        {QStringLiteral("streetrace"), QStringLiteral("Street race hubs")},
        {QStringLiteral("barnfind"), QStringLiteral("Barn finds")},
        {QStringLiteral("gas_station"), QStringLiteral("Gas stations")},
        {QStringLiteral("flyer"), QStringLiteral("Flyers")},
        {QStringLiteral("speed_camera_discovered"), QStringLiteral("Speed traps")},
        {QStringLiteral("average_speed_camera_discovered"), QStringLiteral("Speed zones")},
        {QStringLiteral("workshop"), QStringLiteral("Workshops")},
        {QStringLiteral("autoshow"), QStringLiteral("Autoshow")},
        {QStringLiteral("paintshop"), QStringLiteral("Paint shop")},
        {QStringLiteral("carclub"), QStringLiteral("Car club")},
        {QStringLiteral("dlccenter"), QStringLiteral("DLC center")},
        {QStringLiteral("racecentral"), QStringLiteral("Race central")},
    };
    if (const auto it = byIcon.constFind(iconCategory); it != byIcon.cend()) {
        return it.value();
    }
    if (activityType == QLatin1String("ActivityFestivalEntrance")) {
        return QStringLiteral("Festival entrances");
    }
    QString name = activityType;
    if (name.startsWith(QLatin1String("Activity"))) {
        name = name.mid(8);
    }
    return name.isEmpty() ? QStringLiteral("Other activities") : name;
}

QHash<QString, QImage> buildIcons(const QByteArray& profileXml, const QImage& iconSheet)
{
    struct Cell {
        int sortOrder;
        QRect source;
    };
    std::map<QString, std::vector<Cell>> cellsByCategory;

    QXmlStreamReader xml(profileXml);
    std::vector<Cell> groupCells;
    QString category;
    bool inSymbolizer = false;
    bool iconSymbolizer = false;
    int sortOrder = 0;
    QRect atlasCell;
    bool usesSheet = false;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        const auto name = xml.name();
        if (token == QXmlStreamReader::StartElement) {
            const QXmlStreamAttributes attributes = xml.attributes();
            if (name == QLatin1String("Group")) {
                groupCells.clear();
                category.clear();
            } else if (name == QLatin1String("Symbolizer")) {
                inSymbolizer = true;
                // Only the base layers make the icon. The profile also stacks
                // player-progress overlays on it ("_new" badge, "_finish"
                // trophy, "_note"), a per-event colour ring ("_colour") and,
                // for speed zones, a bar drawn along the road ("icon").
                const auto type = attributes.value(QLatin1String("type"));
                iconSymbolizer = type == QLatin1String("icon_up") || type == QLatin1String("icon_up_career");
                sortOrder = attributes.value(QLatin1String("sort_order")).toInt();
                atlasCell = QRect();
                usesSheet = false;
            } else if (inSymbolizer && name == QLatin1String("Atlas")) {
                const int slotsX = std::max(1, attributes.value(QLatin1String("x_slots")).toInt());
                const int slotsY = std::max(1, attributes.value(QLatin1String("y_slots")).toInt());
                const int cellW = iconSheet.width() / slotsX;
                const int cellH = iconSheet.height() / slotsY;
                atlasCell = QRect(attributes.value(QLatin1String("x")).toInt() * cellW,
                    attributes.value(QLatin1String("y")).toInt() * cellH, cellW, cellH);
            } else if (inSymbolizer && name == QLatin1String("Texture")) {
                usesSheet = attributes.value(QLatin1String("value"))
                                .endsWith(QLatin1String("MapIconSheet.tga"), Qt::CaseInsensitive);
            } else if (name == QLatin1String("Filter")
                && attributes.value(QLatin1String("tag")) == QLatin1String("activity_type")) {
                category = attributes.value(QLatin1String("value")).toString();
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (name == QLatin1String("Symbolizer")) {
                if (iconSymbolizer && usesSheet && atlasCell.isValid()) {
                    groupCells.push_back({sortOrder, atlasCell});
                }
                inSymbolizer = false;
            } else if (name == QLatin1String("Group") && !category.isEmpty() && !groupCells.empty()) {
                cellsByCategory[category] = groupCells;
            }
        }
    }
    if (xml.hasError()) {
        failXml(QStringLiteral("MapProfileFullscreen.xml"), xml);
    }

    QHash<QString, QImage> icons;
    for (auto& [name, cells] : cellsByCategory) {
        std::stable_sort(
            cells.begin(), cells.end(), [](const Cell& a, const Cell& b) { return a.sortOrder < b.sortOrder; });
        QImage icon(cells.front().source.size(), QImage::Format_ARGB32_Premultiplied);
        icon.fill(Qt::transparent);
        QPainter painter(&icon);
        for (const Cell& cell : cells) {
            painter.drawImage(QPoint(0, 0), iconSheet, cell.source);
        }
        painter.end();
        icons.insert(name, icon);
    }
    return icons;
}

} // namespace fh1::activities
