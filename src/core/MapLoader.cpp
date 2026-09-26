#include "MapLoader.h"

#include "Activities.h"
#include "ForzaZip.h"
#include "GameDatabase.h"
#include "Loaders.h"
#include "StringTable.h"
#include "XboxTexture.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QRegularExpression>

namespace fh1 {

namespace {

/// Text and icons come from the English tables; the icon sheet is localized
/// too because some icons carry words ("NEW").
constexpr QLatin1StringView kLanguage("EN");

class Context {
public:
    Context(const GameInstall& install, QString trackFolder, const MapLoader::Progress& progress, MapData& map)
        : m_install(install)
        , m_track(std::move(trackFolder))
        , m_progress(progress)
        , m_map(map)
    {
    }

    void step(const QString& text) const
    {
        if (m_progress) {
            m_progress(text);
        }
    }

    void warn(const QString& text) const { m_map.warnings.append(text); }

    /// Reads a track file relative to the media folder. Returns a null array
    /// if it is absent, which is not a problem: tracks such as the menu scenes
    /// ship without a .nav file or zones, and simply get no such layer. A file
    /// that exists but cannot be read is reported as a warning.
    QByteArray readMediaFile(const QString& relativePath) const
    {
        const QString path = m_install.resolve(relativePath);
        if (path.isEmpty()) {
            return {};
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            warn(QStringLiteral("cannot read %1: %2").arg(path, file.errorString()));
            return {};
        }
        return file.readAll();
    }

    /// Runs a layer parser, turning a LoadError into a warning so one bad file
    /// does not hide the rest of the map. Returns the added layer, if any.
    template <typename Parse> Layer* addLayer(Parse&& parse) const
    {
        try {
            Layer layer = parse();
            if (!layer.features.empty()) {
                m_map.layers.push_back(std::move(layer));
                return &m_map.layers.back();
            }
        } catch (const LoadError& e) {
            warn(QString::fromStdString(e.what()));
        }
        return nullptr;
    }

    const QString& track() const { return m_track; }
    QString ribbonPath(const QString& file) const
    {
        return QStringLiteral("tracks/%1/Ribbon_00/%2").arg(m_track, file);
    }
    const GameInstall& install() const { return m_install; }
    MapData& map() const { return m_map; }

    GameDatabase& database() const { return m_database; }
    StringTables& strings() const { return m_strings; }

private:
    const GameInstall& m_install;
    QString m_track;
    const MapLoader::Progress& m_progress;
    MapData& m_map;
    mutable GameDatabase m_database;
    mutable StringTables m_strings;
};

void openTextSources(const Context& ctx)
{
    const QString dbPath = ctx.install().resolve(QStringLiteral("db/gamedb.slt"));
    if (dbPath.isEmpty()) {
        ctx.warn(QStringLiteral("db/gamedb.slt not found; routes and events are shown by their IDs"));
    } else if (!ctx.database().open(dbPath)) {
        ctx.warn(ctx.database().errorString());
    }
    if (!ctx.strings().open(ctx.install(), kLanguage)) {
        ctx.warn(QStringLiteral("%1; names from the game's text are not shown").arg(ctx.strings().errorString()));
    }
}

void loadBackground(const Context& ctx)
{
    MapData& map = ctx.map();
    const std::optional<BackgroundSource> source = knownBackground(ctx.track());
    if (!source) {
        return;
    }
    ctx.step(QStringLiteral("Decompressing map image from %1").arg(source->archive));
    const QString archivePath = ctx.install().resolve(source->archive);
    if (archivePath.isEmpty()) {
        ctx.warn(QStringLiteral("%1 not found; showing the map without its satellite image").arg(source->archive));
        return;
    }
    ForzaZip zip;
    if (!zip.open(archivePath)) {
        ctx.warn(zip.errorString());
        return;
    }
    QString error;
    const QByteArray bytes = zip.read(source->entry, &error);
    if (bytes.isNull()) {
        ctx.warn(error);
        return;
    }
    QImage image = QImage::fromData(bytes);
    if (image.isNull()) {
        ctx.warn(QStringLiteral("%1 in %2 is not a readable image").arg(source->entry, source->archive));
        return;
    }
    if (image.size() != source->expectedSize) {
        ctx.warn(QStringLiteral("%1 is %2x%3, expected %4x%5; its calibration does not apply")
                .arg(source->entry)
                .arg(image.width())
                .arg(image.height())
                .arg(source->expectedSize.width())
                .arg(source->expectedSize.height()));
        return;
    }
    map.background = std::move(image);
    map.backgroundSource = QStringLiteral("%1/%2").arg(source->archive, source->entry);
    map.calibration = source->calibration;
}

void loadIcons(const Context& ctx)
{
    ctx.step(QStringLiteral("Reading map icons"));
    const QString profilePath = ctx.install().resolve(QStringLiteral("ui/MapProfileFullscreen.xml"));
    const QString texturesPath = ctx.install().resolve(QStringLiteral("ui/textures/Horizon.zip"));
    if (profilePath.isEmpty() || texturesPath.isEmpty()) {
        ctx.warn(QStringLiteral("ui/MapProfileFullscreen.xml or ui/textures/Horizon.zip not found; "
                                "markers are drawn without the game's icons"));
        return;
    }
    QFile profile(profilePath);
    if (!profile.open(QIODevice::ReadOnly)) {
        ctx.warn(QStringLiteral("cannot read %1: %2").arg(profilePath, profile.errorString()));
        return;
    }
    ForzaZip zip;
    if (!zip.open(texturesPath)) {
        ctx.warn(zip.errorString());
        return;
    }
    QString error;
    const QString sheetEntry = QStringLiteral("Map/icons/MapIcons/%1/MapIconSheet.xds").arg(kLanguage);
    const QByteArray sheetData = zip.read(sheetEntry, &error);
    if (sheetData.isNull()) {
        ctx.warn(error);
        return;
    }
    const QImage sheet = decodeXboxTexture(sheetData, &error);
    if (sheet.isNull()) {
        ctx.warn(QStringLiteral("%1: %2").arg(sheetEntry, error));
        return;
    }
    try {
        ctx.map().icons = activities::buildIcons(profile.readAll(), sheet);
    } catch (const LoadError& e) {
        ctx.warn(QString::fromStdString(e.what()));
    }
}

void loadAiRoutes(const Context& ctx)
{
    ctx.step(QStringLiteral("Reading AI routes"));
    const QString archivePath = ctx.install().resolve(QStringLiteral("aiopenworld.zip"));
    if (archivePath.isEmpty()) {
        ctx.warn(QStringLiteral("aiopenworld.zip not found; AI routes are not shown"));
        return;
    }
    ForzaZip zip;
    if (!zip.open(archivePath)) {
        ctx.warn(zip.errorString());
        return;
    }
    const QHash<int, GameDatabase::Route> routes = ctx.database().routes(ctx.track());

    Layer layer;
    layer.id = QStringLiteral("airoutes");
    layer.title = QStringLiteral("AI race routes");
    layer.source = QStringLiteral("aiopenworld.zip");
    layer.kind = FeatureKind::Polyline;

    static const QRegularExpression routePattern(
        QStringLiteral("route_(\\d+)\\.owt$"), QRegularExpression::CaseInsensitiveOption);
    const QString prefix = ForzaZip::normalizeName(QStringLiteral("%1/Ribbon_00/").arg(ctx.track()));
    for (const ZipEntry& entry : zip.entries()) {
        if (!ForzaZip::normalizeName(entry.name).startsWith(prefix)) {
            continue;
        }
        const QRegularExpressionMatch match = routePattern.match(entry.name);
        if (!match.hasMatch()) {
            continue;
        }
        const int routeId = match.captured(1).toInt();
        QString error;
        const QByteArray data = zip.read(entry, &error);
        if (data.isNull()) {
            ctx.warn(error);
            continue;
        }
        const GameDatabase::Route route = routes.value(routeId);
        const QString name = route.devName.isEmpty() ? QStringLiteral("Route %1").arg(routeId)
                                                     : QStringLiteral("%1 (route %2)").arg(route.devName).arg(routeId);
        try {
            Feature feature = loaders::aiRoute(data, name);
            feature.label = ctx.strings().resolve(QStringLiteral("Tracks.str"), route.displayName);
            feature.group
                = route.devName.isEmpty() ? QStringLiteral("(unnamed)") : loaders::gameplayGroup(route.devName);
            feature.properties.prepend({QStringLiteral("File"), entry.name});
            feature.properties.prepend({QStringLiteral("Route id"), QString::number(routeId)});
            if (!route.devName.isEmpty()) {
                feature.properties.prepend({QStringLiteral("Database name"), route.devName});
            }
            layer.features.push_back(std::move(feature));
        } catch (const LoadError& e) {
            ctx.warn(QString::fromStdString(e.what()));
        }
    }
    if (!layer.features.empty()) {
        ctx.map().layers.push_back(std::move(layer));
    }
}

void loadTrackRoutes(const Context& ctx)
{
    ctx.step(QStringLiteral("Reading track route transforms"));
    const QString ribbon = ctx.install().resolve(QStringLiteral("tracks/%1/Ribbon_00").arg(ctx.track()));
    if (ribbon.isEmpty()) {
        return;
    }
    Layer layer;
    layer.id = QStringLiteral("trackroutes");
    layer.title = QStringLiteral("Route start points and markers");
    layer.source = QStringLiteral("Ribbon_00/TrackRouteNNN.xml");
    layer.kind = FeatureKind::Point;

    const QDir dir(ribbon);
    const QStringList files
        = dir.entryList({QStringLiteral("TrackRoute*.xml")}, QDir::Files, QDir::Name | QDir::IgnoreCase);
    for (const QString& file : files) {
        QFile handle(dir.filePath(file));
        if (!handle.open(QIODevice::ReadOnly)) {
            ctx.warn(QStringLiteral("cannot read %1: %2").arg(handle.fileName(), handle.errorString()));
            continue;
        }
        try {
            loaders::appendTrackRoute(handle.readAll(), QFileInfo(file).completeBaseName(), layer);
        } catch (const LoadError& e) {
            ctx.warn(QString::fromStdString(e.what()));
        }
    }
    if (!layer.features.empty()) {
        ctx.map().layers.push_back(std::move(layer));
    }
}

/// Reads every activity config of the track from gamemodes.zip.
std::vector<Activity> loadActivities(const Context& ctx)
{
    std::vector<Activity> result;
    const QString archivePath = ctx.install().resolve(QStringLiteral("gamemodes.zip"));
    if (archivePath.isEmpty()) {
        ctx.warn(QStringLiteral("gamemodes.zip not found; gameplay objects are grouped by ID only"));
        return result;
    }
    ForzaZip zip;
    if (!zip.open(archivePath)) {
        ctx.warn(zip.errorString());
        return result;
    }
    const QString prefix = ForzaZip::normalizeName(ctx.track() + QLatin1Char('/'));
    for (const ZipEntry& entry : zip.entries()) {
        const QString name = ForzaZip::normalizeName(entry.name);
        if (!name.startsWith(prefix) || !name.endsWith(QLatin1String(".xml"))) {
            continue;
        }
        QString error;
        const QByteArray data = zip.read(entry, &error);
        if (data.isNull()) {
            ctx.warn(error);
            continue;
        }
        try {
            std::vector<Activity> parsed = activities::parse(data, entry.name);
            std::move(parsed.begin(), parsed.end(), std::back_inserter(result));
        } catch (const LoadError& e) {
            ctx.warn(QString::fromStdString(e.what()));
        }
    }
    return result;
}

/// "<year> <make> <model>", as the game names cars; parts it cannot resolve
/// are left out.
QString carName(const Context& ctx, const QString& carId)
{
    const std::optional<GameDatabase::Car> car = ctx.database().car(carId);
    if (!car) {
        return {};
    }
    const QString model = ctx.strings().resolve(QStringLiteral("Data_Car.str"), car->model);
    if (model.isEmpty()) {
        return {};
    }
    QStringList parts;
    if (car->year > 0) {
        parts.append(QString::number(car->year));
    }
    const QString make = ctx.strings().resolve(QStringLiteral("List_CarMake.str"), car->make);
    if (!make.isEmpty()) {
        parts.append(make);
    }
    parts.append(model);
    return parts.join(QLatin1Char(' '));
}

/// Candidate keys for an ID, longest first: "PLANE_RACE_004_14" gives
/// "PLANE_RACE_004_14", "PLANE_RACE_004", "PLANE_RACE", "PLANE".
QStringList underscorePrefixes(const QString& id)
{
    QStringList result{id};
    QString current = id;
    for (qsizetype cut = current.lastIndexOf(QLatin1Char('_')); cut > 0; cut = current.lastIndexOf(QLatin1Char('_'))) {
        current.truncate(cut);
        result.append(current);
    }
    return result;
}

/// Gives gameplay objects their in-game category, icon and name. An object is
/// the location of an activity when a config's `<TriggerZone object=...>`
/// names it or, for activities without trigger zones, when its ID extends the
/// activity's name (speed trap "speed_camera_30" owns objects
/// "speed_camera_30_left" and "_right").
/// Objects an activity puts the player's car at (the "_NODE" objects) and
/// other objects it refers to (a barn find's door models) are grouped with it
/// without an icon. Event names come from Events.HorizonEventID.
void enrichGameplayObjects(const Context& ctx, Layer& layer)
{
    ctx.step(QStringLiteral("Naming gameplay objects"));
    const std::vector<Activity> configs = loadActivities(ctx);

    QHash<QString, int> byTrigger;
    QHash<QString, int> byName;
    QHash<QString, int> byReference;
    for (int i = 0; i < static_cast<int>(configs.size()); ++i) {
        const Activity& activity = configs[static_cast<std::size_t>(i)];
        for (const QString& object : activity.triggerObjects) {
            byTrigger.insert(object.toLower(), i);
        }
        // An activity with trigger zones is located by them; only activities
        // without any are located through objects named after them.
        if (activity.triggerObjects.isEmpty() && !activity.name.isEmpty()) {
            byName.insert(activity.name.toLower(), i);
        }
        for (const QString& value : activity.referencedValues) {
            byReference.insert(value.toLower(), i);
        }
    }

    QHash<QString, GameDatabase::Event> events;
    for (auto [id, event] : ctx.database().events().asKeyValueRange()) {
        events.insert(id.toLower(), event);
    }

    for (Feature& feature : layer.features) {
        const QString id = feature.name;
        const QStringList prefixes = underscorePrefixes(id.toLower());

        QString eventId;
        QString eventName;
        for (const QString& prefix : prefixes) {
            if (const auto it = events.constFind(prefix); it != events.cend()) {
                eventId = prefix.toUpper();
                eventName = ctx.strings().resolve(QStringLiteral("Events.str"), it->name);
                break;
            }
        }

        int activityIndex = byTrigger.value(id.toLower(), -1);
        for (qsizetype p = 0; activityIndex < 0 && p < prefixes.size(); ++p) {
            activityIndex = byName.value(prefixes.at(p), -1);
        }
        const bool located = activityIndex >= 0;
        if (!located) {
            activityIndex = byReference.value(id.toLower(), -1);
        }

        if (activityIndex >= 0) {
            const Activity& activity = configs[static_cast<std::size_t>(activityIndex)];
            const QString category = activities::iconCategory(activity, eventId);
            const QString title = activities::categoryTitle(category, activity.type);
            const bool carPlacement = !located && activity.carPlacementObjects.contains(id, Qt::CaseInsensitive);
            if (located) {
                feature.group = title;
                if (const auto radius = activity.triggerRadius.constFind(id); radius != activity.triggerRadius.cend()) {
                    feature.properties.append({QStringLiteral("Trigger radius"), QStringLiteral("%1 m").arg(*radius)});
                }
                if (ctx.map().icons.contains(category)) {
                    feature.icon = category;
                }
                if (!eventName.isEmpty()) {
                    feature.label = eventName;
                } else if (!activity.unlockCarId.isEmpty()) {
                    feature.label = carName(ctx, activity.unlockCarId);
                }
            } else if (carPlacement) {
                feature.group = QStringLiteral("%1 (car placement points)").arg(title);
                feature.properties.append({QStringLiteral("Role"),
                    QStringLiteral("The game places the player's car here when the activity starts "
                                   "(CPlaceCarAtObject)")});
            } else {
                feature.group = QStringLiteral("%1 (related objects)").arg(title);
            }
            feature.properties.append({QStringLiteral("Activity"), activity.name});
            feature.properties.append({QStringLiteral("Activity type"), activity.type});
            feature.properties.append({QStringLiteral("Config"), activity.file});
            if (!activity.mapTag.isEmpty()) {
                feature.properties.append({QStringLiteral("Map tag"), activity.mapTag});
            }
            if (!activity.unlockCarId.isEmpty()) {
                feature.properties.append({QStringLiteral("Unlocks car id"), activity.unlockCarId});
            }
        } else if (!eventId.isEmpty()) {
            feature.group = QStringLiteral("Event objects");
        }

        if (!eventId.isEmpty()) {
            feature.properties.append({QStringLiteral("Event ID"), eventId});
            if (!eventName.isEmpty()) {
                feature.properties.append({QStringLiteral("Event"), eventName});
            }
        }
        if (!feature.label.isEmpty()) {
            feature.properties.prepend({QStringLiteral("Label"), feature.label});
        }
    }
}

} // namespace

MapData MapLoader::load(const GameInstall& install, const QString& trackFolder, const Progress& progress)
{
    if (install.resolve(QStringLiteral("tracks/%1/Ribbon_00").arg(trackFolder)).isEmpty()) {
        throw LoadError(QStringLiteral("track %1 has no Ribbon_00 folder under %2/tracks")
                .arg(trackFolder, install.mediaPath())
                .toStdString());
    }

    MapData map;
    map.trackName = trackFolder;
    const Context ctx(install, trackFolder, progress, map);

    openTextSources(ctx);
    loadBackground(ctx);
    loadIcons(ctx);

    // Layers are stored bottom to top: broad areas first, point markers last.
    ctx.step(QStringLiteral("Reading post-processing zones"));
    if (Layer* zones = ctx.addLayer([&] {
            // Colorado ships only the _Safe variant; the menu scenes ship only
            // the plain one.
            QString name = QStringLiteral("PostProcessingZones_Safe.xml");
            if (install.resolve(ctx.ribbonPath(name)).isEmpty()) {
                name = QStringLiteral("PostProcessingZones.xml");
            }
            const QByteArray data = ctx.readMediaFile(ctx.ribbonPath(name));
            return data.isNull() ? Layer{} : loaders::postProcessingZones(data, QStringLiteral("Ribbon_00/") + name);
        })) {
        for (Feature& zone : zones->features) {
            zone.label = zone.name;
        }
    }

    loadAiRoutes(ctx);

    ctx.step(QStringLiteral("Reading the road network"));
    ctx.addLayer([&] {
        const QString file = QStringLiteral("tracks/%1/%1.nav").arg(trackFolder);
        const QByteArray data = ctx.readMediaFile(file);
        return data.isNull() ? Layer{} : loaders::navNodes(data, QStringLiteral("%1.nav").arg(trackFolder));
    });

    ctx.step(QStringLiteral("Reading collision objects"));
    ctx.addLayer([&] {
        const QByteArray data = ctx.readMediaFile(ctx.ribbonPath(QStringLiteral("CollObjs.xml")));
        return data.isNull()
            ? Layer{}
            : loaders::placements(data, QStringLiteral("collobjs"), QStringLiteral("Collision objects"),
                  QStringLiteral("Ribbon_00/CollObjs.xml"), QStringLiteral("PhysicsType"));
    });

    ctx.step(QStringLiteral("Reading particle emitters"));
    ctx.addLayer([&] {
        const QByteArray data = ctx.readMediaFile(ctx.ribbonPath(QStringLiteral("ParticleEmitters.xml")));
        return data.isNull() ? Layer{}
                             : loaders::particleEmitters(data, QStringLiteral("Ribbon_00/ParticleEmitters.xml"));
    });

    loadTrackRoutes(ctx);

    ctx.step(QStringLiteral("Reading gameplay objects"));
    if (Layer* gameplay = ctx.addLayer([&] {
            const QByteArray data = ctx.readMediaFile(ctx.ribbonPath(QStringLiteral("GameObjs.xml")));
            return data.isNull()
                ? Layer{}
                : loaders::placements(data, QStringLiteral("gameobjs"), QStringLiteral("Gameplay objects"),
                      QStringLiteral("Ribbon_00/GameObjs.xml"), QStringLiteral("GameplayID"));
        })) {
        enrichGameplayObjects(ctx, *gameplay);
    }

    return map;
}

} // namespace fh1
