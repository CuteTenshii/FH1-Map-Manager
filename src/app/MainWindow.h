#pragma once

#include "ForzaZip.h"
#include "GameInstall.h"
#include "MapData.h"
#include "TrackPlacements.h"
#include "TrackTextures.h"
#include "WorldIndex.h"

#include <QFutureWatcher>
#include <QMainWindow>
#include <QPointer>

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>

class BackgroundItem;
class FeatureTableModel;
class LayerItem;
class MapView;
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
    void showProperties();
    void onMapClicked(const QPointF& scenePos);
    void onCursorMoved(const QPointF& scenePos);
    void onMapContextMenu(const QPointF& scenePos, const QPoint& globalPos);
    void onTableSelectionChanged();
    void onTableActivated(const QModelIndex& index);
    void applyFilter();
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
    QAction* m_findAction = nullptr;
    QAction* m_clearSelectionAction = nullptr;
    QAction* m_clearCacheAction = nullptr;

    QDockWidget* m_debugDock = nullptr;
    WorldDebugPanel* m_debugPanel = nullptr;
    /// Coalesces the 3D view's frequent loading updates into a few refreshes
    /// per second.
    QTimer* m_debugRefresh = nullptr;
    /// The archive the debug panel shows files of, to spot world changes.
    const fh1::ForzaZip* m_debugArchive = nullptr;

    std::optional<ScriptOptions> m_script;
};
