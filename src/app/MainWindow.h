#pragma once

#include "ForzaZip.h"
#include "GameInstall.h"
#include "MapData.h"
#include "ModelRemoval.h"
#include "TrackPlacements.h"
#include "TrackTextures.h"
#include "WorldIndex.h"
#include "WorldPicking.h"

#include <QFutureWatcher>
#include <QMainWindow>
#include <QPixmap>
#include <QPointer>
#include <QSet>

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>

class BackgroundItem;
class FeatureTableModel;
class LayerItem;
class MapView;
class RaceTableModel;
class EditSession;
class WorldDebugPanel;
class WorldView3D;
class QAction;
class QComboBox;
class QDockWidget;
class QGraphicsScene;
class QLabel;
class QLineEdit;
class QMenu;
class QProgressBar;
class QSortFilterProxyModel;
class QStackedWidget;
class QTableView;
class QTableWidget;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    /// Options for scripted use: open a view, optionally select something,
    /// save a screenshot of the window and quit.
    struct ScriptOptions {
        QString screenshotPath;
        /// Layer ids to show; when set, every other layer starts hidden.
        std::optional<QStringList> visibleLayers;
        /// World X/Z to centre on, with a zoom in device pixels per image pixel.
        std::optional<QPointF> centre;
        double zoom = 1.0;
        /// Name of a feature to select once the map is loaded.
        QString select;
        /// Event ID of a race to show once the map is loaded.
        QString race;
        /// Show the 3D world instead of the 2D map; the screenshot is taken
        /// once every visible tile has loaded.
        bool world3D = false;
        /// 3D camera: world X, Y, Z, heading and pitch in degrees (heading 0
        /// is east, 90 north).
        std::optional<std::array<float, 5>> camera;
    };

    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    /// Opens an extracted disc folder and loads `track` (or the last used or
    /// first available track when empty). Returns false if the folder is not
    /// a game install; the reason is shown to the user.
    bool openGameFolder(const QString& path, const QString& track = {});
    void setScriptOptions(const ScriptOptions& options);

signals:
    /// Emitted when a script screenshot has been written (or failed); the
    /// argument is the process exit code to use.
    void scriptFinished(int exitCode);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    struct LoadResult {
        std::shared_ptr<fh1::MapData> map;
        QString error;
    };
    struct Selection {
        int layer = -1;
        int feature = -1;
    };
    /// Which of the props the game only puts out for events the 3D world
    /// draws.
    enum class EventProps { None, SelectedRace, AllEvents };
    /// The gameplay objects and their map features, as one edit changes
    /// them together.
    struct GameObjectsState {
        fh1::GameObjectsFile file;
        std::vector<fh1::Feature> features;
    };

    void createActions();
    void createDocks();
    void createStatusBar();
    /// Shows `message` in a dialog, or when running a script, prints it to
    /// stderr directly: Qt's message handler may route to the journal instead.
    void reportError(const QString& title, const QString& message);
    void chooseGameFolder();
    void loadTrack(const QString& track);
    void onLoadFinished();
    void buildScene();
    void buildLayerTree();
    void onLayerTreeChanged(QTreeWidgetItem* item, int column);
    void saveLayerVisibility() const;
    void selectFeature(int layer, int feature, bool focus);
    void clearSelection();
    /// Selects a model of the 3D world that a click landed on.
    void selectModel(const fh1::PickHit& hit);
    void showProperties();
    void showModelProperties();
    void onMapClicked(const QPointF& scenePos);
    void onCursorMoved(const QPointF& scenePos);
    void onMapContextMenu(const QPointF& scenePos, const QPoint& globalPos);
    void onTableSelectionChanged();
    void onTableActivated(const QModelIndex& index);
    void applyFilter();
    void createEventsDock();
    /// Shows race `race` (an index into MapData::races): its route on the map
    /// and in the 3D world, only its event props, and its details in the
    /// Properties panel. With `focus`, both views move to it.
    void selectRace(int race, bool focus);
    /// Stops showing the selected race.
    void clearRace();
    /// Takes the selected race's route off the map and out of the 3D world.
    void removeRaceOverlay();
    /// Draws the selected race's route as it is now, on the map and in the
    /// 3D world.
    void showRaceOverlay();
    /// Index into MapData::raceRoutes of the selected race's route, or -1.
    int selectedRoute() const;
    /// Turns editing on the map (route points and gameplay objects) on or
    /// off.
    void setMapEditing(bool editing);
    /// Enables the editing actions for what is loaded and selected.
    void updateEditActions();
    /// The selected route's transform whose marker is at `scenePos`.
    std::optional<std::size_t> routePointAt(const QPointF& scenePos) const;
    /// The shown gameplay object at `scenePos`, as an index into both
    /// MapData::gameObjects and the "gameobjs" layer.
    std::optional<std::size_t> gameObjectAt(const QPointF& scenePos) const;
    /// Index into MapData::layers of the gameplay objects, or -1.
    int gameObjectsLayer() const;
    void onGrabMoved(const QPointF& scenePos, Qt::KeyboardModifiers modifiers);
    void onGrabReleased(const QPointF& scenePos, Qt::KeyboardModifiers modifiers);
    /// Height of the ground at `x`, `z` from the 3D world, else from the
    /// nearest road node; nothing when neither is known.
    std::optional<float> groundHeightAt(float x, float z) const;
    /// Registers the files the loaded track's edits are saved to.
    void registerEditableFiles();
    /// Records an edit made to route `route`, which was `before`.
    void pushRouteEdit(int route, const fh1::RaceRoute& before, const QString& description);
    /// Records an edit made to the gameplay objects, which were `before`,
    /// and redraws them.
    void pushGameObjectsEdit(const GameObjectsState& before, const QString& description);
    void onRouteChanged(int route);
    /// Copies gameplay object `index`'s position and heading to its feature.
    void syncGameObjectFeature(std::size_t index);
    /// Redraws the gameplay objects after they were edited, in the map,
    /// the Objects panel and the 3D world.
    void refreshGameObjects();
    /// Redraws layer `layer` after its features changed.
    void refreshLayer(int layer);
    /// Index into MapData::layers of the layer with id `id`, or -1.
    int layerIndex(const QString& id) const;
    /// True if the features of layer `layer` can be deleted.
    bool canDeleteFeatures(int layer) const;
    /// Deletes feature `feature` of layer `layer` from the file it came
    /// from, as one undoable edit.
    void deleteLayerFeature(int layer, int feature);
    /// The route and transform a feature of the route markers layer shows.
    std::optional<std::pair<int, std::size_t>> routeTransformOf(int feature) const;
    /// Rebuilds the route markers layer from the routes, after they changed.
    void rebuildRouteMarkers();
    /// The shown feature under `scenePos` on the map, as layer and feature.
    std::optional<std::pair<int, int>> featureAt(const QPointF& scenePos) const;
    /// Deletes the gameplay objects `indices`; with `ask`, asks first when
    /// an activity uses one of them.
    void deleteGameObjects(const std::vector<std::size_t>& indices, bool ask = true);
    /// Asks which of the selected race event's objects and database rows to
    /// delete, then deletes them.
    void deleteRaceEvent();
    /// Paths under the media folder of the originals kept in
    /// EditSession::backupFolder().
    QStringList backedUpFiles() const;
    /// Puts the backed-up originals back in the output folder, after asking.
    void restoreOriginals();
    /// Removes the selected model of the 3D world from the map, as an
    /// undoable edit of the track's bin.zip.
    void deleteModel();
    /// The part name of model chunk `chunk`, or its file name.
    QString modelName(std::uint32_t chunk) const;
    /// Adds deleting gameplay object `index`, alone and with its group, to
    /// `menu`.
    void addDeleteGameObjectActions(QMenu& menu, std::size_t index);
    void onTableContextMenu(const QPoint& position);
    /// The gameplay objects that belong with object `index`, not itself.
    std::vector<std::size_t> relatedGameObjects(std::size_t index) const;
    /// Rings the objects related to the selected gameplay object on the map.
    void showRelatedGameObjects();
    /// Selects gameplay object `index` and zooms the map to it and the
    /// objects related to it.
    void focusOnGameObjectGroup(std::size_t index);
    void onLayerTreeContextMenu(const QPoint& position);
    void onRaceTableContextMenu(const QPoint& position);
    /// Deletes what is selected: a gameplay object, or in map editing a
    /// checkpoint or waypoint of the selected race. With `withGroup`, a
    /// gameplay object's group goes too.
    void deleteSelection(bool withGroup);
    /// Opens the settings of the selected race for editing.
    void editRaceSettings();
    void onEditsChanged();
    /// Races whose route or settings have unsaved edits.
    QSet<int> editedRaces() const;
    /// The pole position of the selected race, or nullptr.
    const fh1::RouteTransform* raceStart() const;
    void showRaceProperties();
    void onRaceSelectionChanged();
    void applyRaceFilter();
    /// Moves the 2D map and the 3D camera to the selected race.
    void focusOnRace();
    /// Points the map's labels at the race overlay and the layers.
    void updateLabelSources();
    /// Switches the event props shown, updating the menu and the Events
    /// panel and remembering the choice.
    void setEventProps(EventProps mode);
    /// Hands the 3D view the event props for the mode and selected race.
    void applyEventPropFilter();
    /// Names the selected race in the Events panel and the Selected Race
    /// choices, and disables those while no race is selected.
    void updateEventPropsChoices();
    void exportView();
    void showWarnings();
    void showAbout();
    void copyProperties();
    void setLoading(bool loading);
    void updateWindowTitle();
    void runScript();
    void takeScriptScreenshot();
    /// Switches the central area between the 2D map and the 3D world.
    void showWorld3D(bool show);
    void updateCentralPage();
    /// Starts loading the current track's 3D world if it is not loaded yet.
    void ensureWorld();
    void onWorldLoaded();
    void placeWorldCamera();
    /// Deletes the cached world indexes, after asking, and reloads the 3D
    /// world if it is shown.
    void clearCache();
    /// Points the debug panel at the 3D view's current world, and refreshes
    /// its lists when it is visible.
    void onLoadedFilesChanged();
    /// Which groups of each layer the 2D map shows, for the 3D view.
    std::vector<std::vector<bool>> entityVisibility() const;
    void refreshDebugPanel();
    QTreeWidgetItem* groupTreeItem(int layer, int group) const;

    fh1::GameInstall m_install;
    bool m_installOpen = false;
    QString m_trackName;
    std::shared_ptr<fh1::MapData> m_map;
    QFutureWatcher<LoadResult> m_watcher;

    struct WorldLoad {
        std::shared_ptr<const fh1::ForzaZip> archive;
        std::shared_ptr<const fh1::WorldIndex> index;
        /// Null when the track's texture tables could not be read.
        std::shared_ptr<const fh1::TrackTextures> textures;
        QString error;
        QString textureError;
    };
    QFutureWatcher<WorldLoad> m_worldWatcher;
    std::shared_ptr<std::atomic<bool>> m_worldCancel;
    /// Track whose world is loaded or loading into the 3D view.
    QString m_worldTrack;
    bool m_showWorld3D = false;
    WorldView3D* m_world3D = nullptr;
    QAction* m_view2DAction = nullptr;
    QAction* m_view3DAction = nullptr;
    QString m_pendingTrack;

    QStackedWidget* m_stack = nullptr;
    QLabel* m_noDataLabel = nullptr;
    MapView* m_view = nullptr;
    QGraphicsScene* m_scene = nullptr;
    BackgroundItem* m_background = nullptr;
    std::vector<LayerItem*> m_layerItems;
    Selection m_selection;
    /// The model of the 3D world selected by a click, if any; a map feature
    /// selection clears it and the other way round.
    std::optional<fh1::PickHit> m_selectedModel;

    QMenu* m_viewMenu = nullptr;
    QDockWidget* m_objectsDock = nullptr;
    QTreeWidget* m_layerTree = nullptr;
    QLineEdit* m_filterEdit = nullptr;
    QTableView* m_table = nullptr;
    QLabel* m_tableCount = nullptr;
    FeatureTableModel* m_model = nullptr;
    QSortFilterProxyModel* m_proxy = nullptr;
    QTimer* m_filterTimer = nullptr;
    QTableWidget* m_properties = nullptr;
    bool m_syncingSelection = false;

    QDockWidget* m_eventsDock = nullptr;
    QLineEdit* m_raceFilterEdit = nullptr;
    QTableView* m_raceTable = nullptr;
    QLabel* m_raceCount = nullptr;
    QToolButton* m_hideRaceButton = nullptr;
    QLabel* m_selectedRaceLabel = nullptr;
    /// Event props choice in the Events panel, in EventProps order.
    QComboBox* m_eventPropsCombo = nullptr;
    RaceTableModel* m_raceModel = nullptr;
    QSortFilterProxyModel* m_raceProxy = nullptr;
    /// Index into MapData::races of the race shown, or -1.
    int m_selectedRace = -1;
    /// The layers drawn for the selected race; the items below and the 3D
    /// view refer to them.
    std::shared_ptr<const fh1::MapData> m_raceOverlay;
    std::vector<LayerItem*> m_raceItems;

    EditSession* m_edits = nullptr;
    QAction* m_editMapAction = nullptr;
    QAction* m_editRaceAction = nullptr;
    QAction* m_saveEditsAction = nullptr;
    QAction* m_outputFolderAction = nullptr;
    /// Something on the map that a press can grab: a route point of the
    /// selected race, or a gameplay object.
    struct MapGrab {
        bool routePoint = false;
        /// Index into the route's transforms or the gameplay objects.
        std::size_t index = 0;
    };
    /// A drag of a grabbed thing, with what it changes as it was before,
    /// for undo.
    struct MapDrag {
        MapGrab grab;
        std::optional<fh1::RaceRoute> routeBefore;
        std::optional<GameObjectsState> objectsBefore;
        /// The gameplay objects that move with the grabbed one, itself first.
        std::vector<std::size_t> group;
        bool moved = false;
        bool turned = false;
    };
    std::optional<MapDrag> m_drag;
    /// What is under the last press or hover on the map.
    std::optional<MapGrab> m_grab;
    /// The name of the selected race's route point clicked in map editing,
    /// which Delete removes.
    QString m_selectedRoutePoint;
    QAction* m_deleteAction = nullptr;
    QAction* m_deleteGroupAction = nullptr;
    /// The races as loaded, to tell which settings were edited; kept in
    /// step with MapData::races when races are deleted.
    std::vector<fh1::Race> m_loadedRaces;
    /// Races whose events are deleted from the database, as loaded.
    std::vector<fh1::Race> m_deletedRaces;
    /// A layer file and the layer's features, as one deletion changes them
    /// together.
    struct LayerFileState {
        fh1::XmlElementsFile file;
        std::vector<fh1::Feature> features;
    };
    /// The race lists as one edit of the database changes them together.
    struct DatabaseState {
        std::vector<fh1::Race> races;
        std::vector<fh1::Race> loaded;
        std::vector<fh1::Race> deleted;
    };
    QAction* m_deleteEventAction = nullptr;
    /// The track's bin.zip as edited: the entries changed by removing
    /// models, by entry index, and the world chunks those models are.
    struct ArchiveEditState {
        QHash<std::uint32_t, QByteArray> entries;
        std::vector<bool> hidden;
    };
    ArchiveEditState m_archiveEdits;
    /// The world index the hidden chunks refer to.
    std::shared_ptr<const fh1::WorldIndex> m_archiveEditsIndex;
    /// Where the zone files place each draw record, read on the first
    /// model removal.
    std::optional<fh1::ZoneRecordIndex> m_zoneRecords;
    QAction* m_restoreAction = nullptr;
    /// Map icons at the size the map draws them, by fh1::Feature::icon.
    QHash<QString, QPixmap> m_icons;

    QComboBox* m_trackCombo = nullptr;
    QLabel* m_cursorLabel = nullptr;
    QProgressBar* m_busy = nullptr;
    QToolButton* m_warningsButton = nullptr;

    QAction* m_openAction = nullptr;
    QAction* m_exportAction = nullptr;
    QAction* m_zoomInAction = nullptr;
    QAction* m_zoomOutAction = nullptr;
    QAction* m_fitAction = nullptr;
    QAction* m_warningsAction = nullptr;
    QAction* m_labelsAction = nullptr;
    EventProps m_eventProps = EventProps::SelectedRace;
    /// View → Event Props entries, in EventProps order.
    std::array<QAction*, 3> m_eventPropsActions{};
    QAction* m_findAction = nullptr;
    QAction* m_clearSelectionAction = nullptr;
    QAction* m_clearCacheAction = nullptr;

    QDockWidget* m_debugDock = nullptr;
    QDockWidget* m_historyDock = nullptr;
    WorldDebugPanel* m_debugPanel = nullptr;
    /// Coalesces the 3D view's frequent loading updates into a few refreshes
    /// per second.
    QTimer* m_debugRefresh = nullptr;
    /// The archive the debug panel shows files of, to spot world changes.
    const fh1::ForzaZip* m_debugArchive = nullptr;

    std::optional<ScriptOptions> m_script;
};
