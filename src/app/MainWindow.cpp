#include "MainWindow.h"

#include "BackgroundItem.h"
#include "EditHistoryPanel.h"
#include "EditSession.h"
#include "FeatureTableModel.h"
#include "GameDatabase.h"
#include "LayerItem.h"
#include "Loaders.h"
#include "MapLoader.h"
#include "MapView.h"
#include "RaceTableModel.h"
#include "Races.h"
#include "RenderMesh.h"
#include "RouteEditing.h"
#include "WorldDebugPanel.h"
#include "WorldView3D.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGraphicsScene>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMap>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTableView>
#include <QTableWidget>
#include <QTimeEdit>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoCommand>
#include <QUndoGroup>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <numbers>

namespace {

/// Refreshes of the debug panel while the 3D view is loading.
constexpr int kDebugRefreshMs = 500;

/// Where each track's world index is cached.
QString worldCacheDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/world");
}

constexpr int kLayerRole = Qt::UserRole;
constexpr int kGroupRole = Qt::UserRole + 1;
/// On Properties rows naming a related gameplay object: its index.
constexpr int kRelatedObjectRole = Qt::UserRole + 2;
constexpr int kBackgroundLayer = -1;

/// Click tolerance around the cursor, in device pixels; about half the size of
/// a map icon.
constexpr double kPickRadius = 11.0;
/// Size icons are kept at in memory; they are drawn at up to 30 px.
constexpr int kIconPixels = 64;
/// Zoom (device pixels per image pixel) used when jumping to a point.
constexpr double kFocusZoom = 3.0;
/// Radius, in device pixels, of the start grid and checkpoint markers.
constexpr double kRaceMarkerRadius = 5.0;
/// The race route is drawn wider than the map's lines, to stand out from
/// the AI routes and roads under it.
constexpr double kRaceLineWidthScale = 2.0;
/// A road node this close to a moved route point gives its height when the
/// 3D world is not loaded.
constexpr float kRoadSnapRadius = 40.0F;
/// Gameplay objects further apart than this are not one group, whatever
/// their names say.
constexpr float kGroupRadius = 500.0F;
/// Race overlay items are drawn above every map layer.
constexpr double kRaceOverlayZ = 10000.0;

/// Layers shown when a track is opened for the first time. The others hold
/// tens of thousands of props or technical data that would bury the map.
const QSet<QString>& layersShownByDefault()
{
    static const QSet<QString> ids{QStringLiteral("gameobjs"), QStringLiteral("airoutes")};
    return ids;
}

/// Groups hidden when a track is first opened even though their layer is
/// shown: objects that sit at, or belong to, an activity already drawn with an
/// icon, which would otherwise pile up under it.
bool groupShownByDefault(const QString& group)
{
    return !group.endsWith(QLatin1String("(related objects)"))
        && !group.endsWith(QLatin1String("(car placement points)")) && group != QLatin1String("Event objects");
}

QIcon swatch(const QColor& color)
{
    QPixmap pixmap(14, 14);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(0, 0, 0, 160), 1.0));
    painter.setBrush(color);
    painter.drawEllipse(QRectF(1.5, 1.5, 11.0, 11.0));
    return QIcon(pixmap);
}

/// Keys of the edited files in the EditSession.
constexpr QLatin1StringView kGameObjectsFile("gameobjs");
constexpr QLatin1StringView kDatabaseFile("gamedb");

QString routeFileKey(int route)
{
    return QStringLiteral("route:%1").arg(route);
}

/// Limits of the race settings dialog; the game's own races have up to 5
/// laps and prizes up to 5,000,000 CR.
constexpr int kMaxLaps = 50;
constexpr int kMaxPrize = 99'999'999;
constexpr int kPrizeStep = 500;
/// Gameplay objects named in the delete event dialog before "…".
constexpr std::size_t kListedNames = 6;
/// Opponents allowed when the route's grid is unknown: the game's grids
/// have eight places.
constexpr int kDefaultMaxOpponents = 7;

/// An edit kept as the state before and after it. The edit is already made
/// when the command is pushed, so its first redo does nothing; later ones,
/// and undo, put a state back through `apply`.
template <typename State> class StateCommand : public QUndoCommand {
public:
    StateCommand(State before, State after, std::function<void(const State&)> apply, const QString& text)
        : QUndoCommand(text)
        , m_before(std::move(before))
        , m_after(std::move(after))
        , m_apply(std::move(apply))
    {
    }

    void undo() override { m_apply(m_before); }

    void redo() override
    {
        if (m_pushed) {
            m_apply(m_after);
        }
        m_pushed = true;
    }

private:
    State m_before;
    State m_after;
    std::function<void(const State&)> m_apply;
    bool m_pushed = false;
};

/// Settings values of MainWindow::EventProps, in its order.
const std::array<QLatin1StringView, 3> kEventPropsSettings{
    QLatin1StringView("none"), QLatin1StringView("race"), QLatin1StringView("all")};

QString eventPropsToolTip(int mode)
{
    switch (mode) {
    case 0:
        return QCoreApplication::translate("MainWindow", "Hide the props the game only puts out for races and events");
    case 1:
        return QCoreApplication::translate("MainWindow",
            "Show the barriers, chevrons and banners of the race selected in the Events panel, and no others");
    default:
        return QCoreApplication::translate(
            "MainWindow", "Show the barriers, chevrons, banners and festival gear of every race and event at once");
    }
}

QString settingsKey(const QString& track, const QString& layerId)
{
    return QStringLiteral("layers/%1/%2").arg(track.toLower(), layerId);
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_scene(new QGraphicsScene(this))
{
    m_view = new MapView;
    m_view->setScene(m_scene);

    auto* emptyPage = new QWidget;
    auto* emptyLayout = new QVBoxLayout(emptyPage);
    auto* emptyText = new QLabel(tr("<p><b>No game folder open</b></p>"
                                    "<p>Choose the extracted Forza Horizon disc: the folder that holds "
                                    "<tt>default.xex</tt> and <tt>media</tt>, or the <tt>media</tt> folder "
                                    "itself.</p>"));
    emptyText->setAlignment(Qt::AlignCenter);
    emptyText->setWordWrap(true);
    auto* emptyButton = new QPushButton(tr("Open Game Folder…"));
    connect(emptyButton, &QPushButton::clicked, this, &MainWindow::chooseGameFolder);
    emptyLayout->addStretch();
    emptyLayout->addWidget(emptyText);
    emptyLayout->addWidget(emptyButton, 0, Qt::AlignHCenter);
    emptyLayout->addStretch();

    m_noDataLabel = new QLabel;
    m_noDataLabel->setAlignment(Qt::AlignCenter);
    m_noDataLabel->setWordWrap(true);
    m_noDataLabel->setMargin(24);

    m_stack = new QStackedWidget;
    m_stack->addWidget(emptyPage);
    m_stack->addWidget(m_view);
    m_stack->addWidget(m_noDataLabel);
    m_world3D = new WorldView3D;
    m_stack->addWidget(m_world3D);
    setCentralWidget(m_stack);
    connect(&m_worldWatcher, &QFutureWatcher<WorldLoad>::finished, this, &MainWindow::onWorldLoaded);
    connect(m_world3D, &WorldView3D::entityClicked, this,
        [this](int layer, int feature) { selectFeature(layer, feature, false); });
    connect(m_world3D, &WorldView3D::modelClicked, this, &MainWindow::selectModel);
    connect(m_world3D, &WorldView3D::emptyClicked, this, &MainWindow::clearSelection);

    m_edits = new EditSession(this);
    createActions();
    createDocks();
    createStatusBar();
    connect(m_edits, &EditSession::modifiedChanged, this, &MainWindow::onEditsChanged);
    connect(m_edits, &EditSession::saved, this, [this](const QString& path) {
        statusBar()->showMessage(tr("Saved %1").arg(QDir::toNativeSeparators(path)), 8000);
    });
    connect(m_view, &MapView::grabMoved, this, &MainWindow::onGrabMoved);
    connect(m_view, &MapView::grabReleased, this, &MainWindow::onGrabReleased);

    QSettings eventPropsSettings;
    const QString savedEventProps = eventPropsSettings.value(QStringLiteral("view/eventProps")).toString();
    const auto saved = std::find(kEventPropsSettings.begin(), kEventPropsSettings.end(), savedEventProps);
    if (saved != kEventPropsSettings.end()) {
        setEventProps(static_cast<EventProps>(saved - kEventPropsSettings.begin()));
    } else {
        // Before the three-way choice there was an on/off switch.
        setEventProps(eventPropsSettings.value(QStringLiteral("view/showEventProps"), false).toBool()
                ? EventProps::AllEvents
                : EventProps::SelectedRace);
    }

    connect(m_view, &MapView::clicked, this, &MainWindow::onMapClicked);
    connect(m_view, &MapView::cursorMoved, this, &MainWindow::onCursorMoved);
    connect(m_view, &MapView::cursorLeft, this, [this] { m_cursorLabel->clear(); });
    connect(m_view, &MapView::contextMenuRequested, this, &MainWindow::onMapContextMenu);
    connect(&m_watcher, &QFutureWatcher<LoadResult>::finished, this, &MainWindow::onLoadFinished);

    setLoading(false);
    updateWindowTitle();
    updateEditActions();

    QSettings settings;
    if (!restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray())) {
        resize(1400, 900);
    }
    restoreState(settings.value(QStringLiteral("window/state")).toByteArray());
    // A layout saved before the debug dock existed puts it wherever space is
    // left; it starts tabbed with the Objects panel instead.
    if (!settings.value(QStringLiteral("window/stateHasWorldDebug"), false).toBool()) {
        tabifyDockWidget(m_objectsDock, m_debugDock);
        m_debugDock->hide();
        m_objectsDock->raise();
    }
    if (!settings.value(QStringLiteral("window/stateHasEditHistory"), false).toBool()) {
        tabifyDockWidget(m_objectsDock, m_historyDock);
        m_historyDock->show();
        m_objectsDock->raise();
    }
    if (!settings.value(QStringLiteral("window/stateHasEvents"), false).toBool()) {
        tabifyDockWidget(m_objectsDock, m_eventsDock);
        m_eventsDock->show();
        m_objectsDock->raise();
    }
}

MainWindow::~MainWindow()
{
    if (m_worldCancel) {
        m_worldCancel->store(true);
    }
    m_worldWatcher.waitForFinished();
    m_watcher.waitForFinished();
}

void MainWindow::createActions()
{
    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    m_openAction = fileMenu->addAction(tr("&Open Game Folder…"), this, &MainWindow::chooseGameFolder);
    m_openAction->setShortcut(QKeySequence::Open);
    m_exportAction = fileMenu->addAction(tr("&Export View as PNG…"), this, &MainWindow::exportView);
    m_exportAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    fileMenu->addSeparator();
    m_saveEditsAction = fileMenu->addAction(tr("&Save Edits"), m_edits, &EditSession::saveAll);
    m_saveEditsAction->setShortcut(QKeySequence::Save);
    m_saveEditsAction->setToolTip(tr("Save every edited route, gameplay object and race setting"));
    m_outputFolderAction = fileMenu->addAction(tr("&Where to Save Edits…"), this, [this] {
        if (m_edits->chooseOutputFolder()) {
            statusBar()->showMessage(
                tr("Edits are saved under %1").arg(QDir::toNativeSeparators(m_edits->outputFolder())), 8000);
        }
    });
    m_outputFolderAction->setToolTip(tr("Choose where edits are saved: the game folder or another"));
    fileMenu->addSeparator();
    m_clearCacheAction = fileMenu->addAction(tr("Clear &Cache…"), this, &MainWindow::clearCache);
    m_clearCacheAction->setToolTip(tr("Delete the 3D world indexes this viewer keeps between sessions"));
    fileMenu->addSeparator();
    QAction* quitAction = fileMenu->addAction(tr("&Quit"), this, &QWidget::close);
    quitAction->setShortcut(QKeySequence::Quit);

    QMenu* editMenu = menuBar()->addMenu(tr("&Edit"));
    QAction* undoAction = m_edits->undoGroup()->createUndoAction(this, tr("&Undo"));
    undoAction->setShortcut(QKeySequence::Undo);
    editMenu->addAction(undoAction);
    QAction* redoAction = m_edits->undoGroup()->createRedoAction(this, tr("&Redo"));
    redoAction->setShortcut(QKeySequence::Redo);
    editMenu->addAction(redoAction);
    editMenu->addSeparator();
    m_editMapAction = editMenu->addAction(tr("Edit on &Map"));
    m_editMapAction->setCheckable(true);
    m_editMapAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    m_editMapAction->setToolTip(tr("Drag the selected race's route points and the gameplay objects on the map to "
                                   "move them; Alt-drag turns them, and right-clicking adds or deletes"));
    connect(m_editMapAction, &QAction::toggled, this, &MainWindow::setMapEditing);
    editMenu->addSeparator();
    m_deleteAction = editMenu->addAction(tr("&Delete"), this, [this] { deleteSelection(false); });
    m_deleteAction->setShortcut(QKeySequence::Delete);
    m_deleteAction->setToolTip(tr("Delete the selected race event or gameplay object, or the checkpoint or "
                                  "waypoint clicked while editing on the map"));
    m_deleteGroupAction
        = editMenu->addAction(tr("Delete with Related &Objects"), this, [this] { deleteSelection(true); });
    m_deleteGroupAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Delete));
    m_deleteGroupAction->setToolTip(tr("Delete the selected gameplay object and the ones that belong with it"));
    editMenu->addSeparator();
    m_editRaceAction = editMenu->addAction(tr("Race &Settings…"), this, &MainWindow::editRaceSettings);
    // Delete reaches this through deleteSelection(); two actions cannot share
    // a key, so the menu only names it.
    m_deleteEventAction = editMenu->addAction(tr("Delete Race &Event…\tDel"), this, &MainWindow::deleteRaceEvent);
    m_deleteEventAction->setToolTip(tr("Delete the selected race event's gameplay objects and database rows"));
    m_editRaceAction->setToolTip(tr("Change the selected race's laps, opponents, prize, car class and start time"));

    m_viewMenu = menuBar()->addMenu(tr("&View"));
    m_zoomInAction = m_viewMenu->addAction(tr("Zoom &In"), this, [this] { m_view->zoomBy(1.5); });
    m_zoomInAction->setShortcuts({QKeySequence::ZoomIn, QKeySequence(Qt::CTRL | Qt::Key_Equal)});
    m_zoomOutAction = m_viewMenu->addAction(tr("Zoom &Out"), this, [this] { m_view->zoomBy(1.0 / 1.5); });
    m_zoomOutAction->setShortcut(QKeySequence::ZoomOut);
    m_fitAction = m_viewMenu->addAction(tr("&Fit Map"), this, [this] { m_view->fitScene(); });
    m_fitAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    m_viewMenu->addSeparator();
    m_findAction = m_viewMenu->addAction(tr("Find &Object"), this, [this] {
        m_objectsDock->show();
        m_objectsDock->raise();
        m_filterEdit->setFocus();
        m_filterEdit->selectAll();
    });
    m_findAction->setShortcut(QKeySequence::Find);
    m_clearSelectionAction = m_viewMenu->addAction(tr("&Clear Selection"), this, &MainWindow::clearSelection);
    m_clearSelectionAction->setShortcut(QKeySequence(Qt::Key_Escape));
    m_viewMenu->addSeparator();
    m_labelsAction = m_viewMenu->addAction(tr("Show &Labels"));
    m_labelsAction->setCheckable(true);
    m_labelsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
    m_labelsAction->setChecked(QSettings().value(QStringLiteral("view/showLabels"), true).toBool());
    m_view->setLabelsVisible(m_labelsAction->isChecked());
    connect(m_labelsAction, &QAction::toggled, this, [this](bool checked) {
        m_view->setLabelsVisible(checked);
        QSettings().setValue(QStringLiteral("view/showLabels"), checked);
    });
    QMenu* eventPropsMenu = m_viewMenu->addMenu(tr("&Event Props"));
    eventPropsMenu->setToolTipsVisible(true);
    auto* eventPropsGroup = new QActionGroup(this);
    // updateEventPropsChoices() names the selected race in the second entry.
    const std::array<QString, 3> eventPropsNames{tr("&None"), QString(), tr("&All Events")};
    for (std::size_t i = 0; i < m_eventPropsActions.size(); ++i) {
        QAction* action = eventPropsMenu->addAction(eventPropsNames[i]);
        action->setCheckable(true);
        action->setToolTip(eventPropsToolTip(static_cast<int>(i)));
        eventPropsGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, i] { setEventProps(static_cast<EventProps>(i)); });
        m_eventPropsActions[i] = action;
    }
    m_viewMenu->addSeparator();
    auto* viewModes = new QActionGroup(this);
    m_view2DAction = m_viewMenu->addAction(tr("&2D Map"));
    m_view2DAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_1));
    m_view2DAction->setCheckable(true);
    m_view2DAction->setChecked(true);
    m_view3DAction = m_viewMenu->addAction(tr("&3D World"));
    m_view3DAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_2));
    m_view3DAction->setCheckable(true);
    m_view3DAction->setToolTip(tr("Fly through the track's terrain and roads"));
    viewModes->addAction(m_view2DAction);
    viewModes->addAction(m_view3DAction);
    connect(m_view2DAction, &QAction::triggered, this, [this] { showWorld3D(false); });
    connect(m_view3DAction, &QAction::triggered, this, [this] { showWorld3D(true); });
    m_viewMenu->addSeparator();
    m_warningsAction = m_viewMenu->addAction(tr("Load &Warnings…"), this, &MainWindow::showWarnings);
    m_viewMenu->addSeparator();

    QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&About"), this, &MainWindow::showAbout);
    helpMenu->addAction(tr("About &Qt"), qApp, &QApplication::aboutQt);

    QToolBar* toolbar = addToolBar(tr("Map"));
    toolbar->setObjectName(QStringLiteral("mapToolbar"));
    toolbar->addAction(m_openAction);
    toolbar->addSeparator();
    toolbar->addWidget(new QLabel(tr("Track ")));
    m_trackCombo = new QComboBox;
    m_trackCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_trackCombo->setToolTip(tr("Track folders under media/tracks that have ribbon data"));
    connect(m_trackCombo, &QComboBox::activated, this, [this](int index) { loadTrack(m_trackCombo->itemText(index)); });
    toolbar->addWidget(m_trackCombo);
    toolbar->addSeparator();
    toolbar->addAction(m_zoomInAction);
    toolbar->addAction(m_zoomOutAction);
    toolbar->addAction(m_fitAction);
    toolbar->addSeparator();
    toolbar->addAction(m_view2DAction);
    toolbar->addAction(m_view3DAction);
    toolbar->addSeparator();
    toolbar->addAction(m_exportAction);
}

void MainWindow::createDocks()
{
    m_layerTree = new QTreeWidget;
    m_layerTree->setHeaderHidden(true);
    m_layerTree->setUniformRowHeights(true);
    connect(m_layerTree, &QTreeWidget::itemChanged, this, &MainWindow::onLayerTreeChanged);
    m_layerTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_layerTree, &QTreeWidget::customContextMenuRequested, this, &MainWindow::onLayerTreeContextMenu);
    auto* layersDock = new QDockWidget(tr("Layers"), this);
    layersDock->setObjectName(QStringLiteral("layersDock"));
    layersDock->setWidget(m_layerTree);
    addDockWidget(Qt::LeftDockWidgetArea, layersDock);

    m_model = new FeatureTableModel(this);
    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setFilterKeyColumn(-1);
    m_proxy->setSortRole(Qt::EditRole);
    m_proxy->setSortCaseSensitivity(Qt::CaseInsensitive);

    m_filterEdit = new QLineEdit;
    m_filterEdit->setPlaceholderText(tr("Filter by name, group or layer"));
    m_filterEdit->setClearButtonEnabled(true);
    m_filterTimer = new QTimer(this);
    m_filterTimer->setSingleShot(true);
    m_filterTimer->setInterval(200);
    connect(m_filterEdit, &QLineEdit::textChanged, m_filterTimer, qOverload<>(&QTimer::start));
    connect(m_filterTimer, &QTimer::timeout, this, &MainWindow::applyFilter);

    m_table = new QTableView;
    m_table->setModel(m_proxy);
    m_table->setSortingEnabled(true);
    m_table->sortByColumn(-1, Qt::AscendingOrder);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setWordWrap(false);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(m_table->fontMetrics().height() + 6);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->setColumnWidth(FeatureTableModel::Name, 180);
    m_table->setColumnWidth(FeatureTableModel::Id, 140);
    m_table->setColumnWidth(FeatureTableModel::Group, 110);
    m_table->setColumnWidth(FeatureTableModel::LayerTitle, 150);
    for (int column : {FeatureTableModel::X, FeatureTableModel::Height, FeatureTableModel::Z}) {
        m_table->setColumnWidth(column, 72);
    }
    connect(
        m_table->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MainWindow::onTableSelectionChanged);
    connect(m_table, &QTableView::activated, this, &MainWindow::onTableActivated);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableView::customContextMenuRequested, this, &MainWindow::onTableContextMenu);

    m_tableCount = new QLabel;
    auto* objectsPanel = new QWidget;
    auto* objectsLayout = new QVBoxLayout(objectsPanel);
    objectsLayout->setContentsMargins(4, 4, 4, 4);
    objectsLayout->addWidget(m_filterEdit);
    objectsLayout->addWidget(m_table);
    objectsLayout->addWidget(m_tableCount);
    m_objectsDock = new QDockWidget(tr("Objects"), this);
    m_objectsDock->setObjectName(QStringLiteral("objectsDock"));
    m_objectsDock->setWidget(objectsPanel);
    addDockWidget(Qt::RightDockWidgetArea, m_objectsDock);

    createEventsDock();

    m_properties = new QTableWidget(0, 2);
    m_properties->setHorizontalHeaderLabels({tr("Property"), tr("Value")});
    m_properties->verticalHeader()->hide();
    m_properties->horizontalHeader()->setStretchLastSection(true);
    m_properties->setColumnWidth(0, 150);
    m_properties->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_properties->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_properties->setWordWrap(false);
    auto* copyAction = new QAction(tr("Copy"), m_properties);
    copyAction->setShortcut(QKeySequence::Copy);
    copyAction->setShortcutContext(Qt::WidgetShortcut);
    connect(copyAction, &QAction::triggered, this, &MainWindow::copyProperties);
    m_properties->addAction(copyAction);
    auto* copyAllAction = new QAction(tr("Copy All"), m_properties);
    connect(copyAllAction, &QAction::triggered, this, [this] {
        m_properties->selectAll();
        copyProperties();
    });
    m_properties->addAction(copyAllAction);
    m_properties->setContextMenuPolicy(Qt::ActionsContextMenu);
    connect(m_properties, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem* item) {
        const QVariant index = item->data(kRelatedObjectRole);
        if (index.isValid() && gameObjectsLayer() >= 0) {
            selectFeature(gameObjectsLayer(), index.toInt(), true);
        }
    });
    auto* propertiesDock = new QDockWidget(tr("Properties"), this);
    propertiesDock->setObjectName(QStringLiteral("propertiesDock"));
    propertiesDock->setWidget(m_properties);
    addDockWidget(Qt::RightDockWidgetArea, propertiesDock);
    splitDockWidget(m_objectsDock, propertiesDock, Qt::Vertical);

    m_historyDock = new QDockWidget(tr("Edit History"), this);
    m_historyDock->setObjectName(QStringLiteral("editHistoryDock"));
    m_historyDock->setWidget(new EditHistoryPanel(m_edits));
    addDockWidget(Qt::RightDockWidgetArea, m_historyDock);

    m_debugPanel = new WorldDebugPanel;
    m_debugDock = new QDockWidget(tr("World Debug"), this);
    m_debugDock->setObjectName(QStringLiteral("worldDebugDock"));
    m_debugDock->setWidget(m_debugPanel);
    addDockWidget(Qt::RightDockWidgetArea, m_debugDock);
    // Placed next to Objects in the constructor, after the saved layout.
    m_debugDock->hide();
    m_debugRefresh = new QTimer(this);
    m_debugRefresh->setSingleShot(true);
    m_debugRefresh->setInterval(kDebugRefreshMs);
    connect(m_debugRefresh, &QTimer::timeout, this, &MainWindow::refreshDebugPanel);
    connect(m_world3D, &WorldView3D::loadedFilesChanged, this, &MainWindow::onLoadedFilesChanged);
    connect(m_debugDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (visible) {
            refreshDebugPanel();
        }
    });

    m_viewMenu->addAction(layersDock->toggleViewAction());
    m_viewMenu->addAction(m_objectsDock->toggleViewAction());
    m_viewMenu->addAction(m_eventsDock->toggleViewAction());
    m_viewMenu->addAction(m_historyDock->toggleViewAction());
    m_viewMenu->addAction(propertiesDock->toggleViewAction());
    QAction* debugAction = m_debugDock->toggleViewAction();
    debugAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D));
    debugAction->setToolTip(tr("List the model and texture files the 3D world has loaded"));
    m_viewMenu->addAction(debugAction);
}

void MainWindow::createEventsDock()
{
    m_raceModel = new RaceTableModel(this);
    m_raceProxy = new QSortFilterProxyModel(this);
    m_raceProxy->setSourceModel(m_raceModel);
    m_raceProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_raceProxy->setFilterKeyColumn(-1);
    m_raceProxy->setSortRole(Qt::EditRole);
    m_raceProxy->setSortCaseSensitivity(Qt::CaseInsensitive);

    m_raceFilterEdit = new QLineEdit;
    m_raceFilterEdit->setPlaceholderText(tr("Filter by name, type, route or class"));
    m_raceFilterEdit->setClearButtonEnabled(true);
    connect(m_raceFilterEdit, &QLineEdit::textChanged, this, &MainWindow::applyRaceFilter);

    m_hideRaceButton = new QToolButton;
    m_hideRaceButton->setText(tr("Hide Race"));
    m_hideRaceButton->setToolTip(tr("Stop showing the selected race's route and props"));
    m_hideRaceButton->setEnabled(false);
    connect(m_hideRaceButton, &QToolButton::clicked, this, &MainWindow::clearRace);

    m_raceTable = new QTableView;
    m_raceTable->setModel(m_raceProxy);
    m_raceTable->setSortingEnabled(true);
    m_raceTable->sortByColumn(-1, Qt::AscendingOrder);
    m_raceTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_raceTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_raceTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_raceTable->setWordWrap(false);
    m_raceTable->verticalHeader()->hide();
    m_raceTable->verticalHeader()->setDefaultSectionSize(m_raceTable->fontMetrics().height() + 6);
    m_raceTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_raceTable->setColumnWidth(RaceTableModel::Name, 200);
    m_raceTable->setColumnWidth(RaceTableModel::Type, 170);
    m_raceTable->setColumnWidth(RaceTableModel::Route, 150);
    for (int column : {RaceTableModel::Laps, RaceTableModel::CarClass}) {
        m_raceTable->setColumnWidth(column, 50);
    }
    m_raceTable->setColumnWidth(RaceTableModel::Length, 70);
    m_raceTable->setColumnWidth(RaceTableModel::Prize, 90);
    connect(m_raceTable->selectionModel(), &QItemSelectionModel::selectionChanged, this,
        &MainWindow::onRaceSelectionChanged);
    m_raceTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_raceTable, &QTableView::customContextMenuRequested, this, &MainWindow::onRaceTableContextMenu);
    connect(m_raceTable, &QTableView::activated, this, [this](const QModelIndex& index) {
        const int race = m_raceProxy->mapToSource(index).row();
        if (race == m_selectedRace) {
            focusOnRace();
        } else {
            selectRace(race, true);
        }
    });

    m_eventPropsCombo = new QComboBox;
    m_eventPropsCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    for (const QString& name : {tr("None"), QString(), tr("All events")}) {
        m_eventPropsCombo->addItem(name);
        m_eventPropsCombo->setItemData(
            m_eventPropsCombo->count() - 1, eventPropsToolTip(m_eventPropsCombo->count() - 1), Qt::ToolTipRole);
    }
    connect(m_eventPropsCombo, &QComboBox::activated, this,
        [this](int index) { setEventProps(static_cast<EventProps>(index)); });
    auto* eventPropsLabel = new QLabel(tr("Event props in 3D:"));
    eventPropsLabel->setBuddy(m_eventPropsCombo);
    auto* eventPropsRow = new QHBoxLayout;
    eventPropsRow->addWidget(eventPropsLabel);
    eventPropsRow->addWidget(m_eventPropsCombo);
    eventPropsRow->addStretch();

    m_raceCount = new QLabel;
    m_selectedRaceLabel = new QLabel;
    m_selectedRaceLabel->setTextFormat(Qt::PlainText);
    auto* editRouteButton = new QToolButton;
    editRouteButton->setDefaultAction(m_editMapAction);
    auto* raceSettingsButton = new QToolButton;
    raceSettingsButton->setDefaultAction(m_editRaceAction);
    auto* selectedRow = new QHBoxLayout;
    selectedRow->addWidget(m_selectedRaceLabel, 1);
    selectedRow->addWidget(editRouteButton);
    selectedRow->addWidget(raceSettingsButton);
    selectedRow->addWidget(m_hideRaceButton);
    auto* panel = new QWidget;
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(m_raceFilterEdit);
    layout->addLayout(selectedRow);
    layout->addLayout(eventPropsRow);
    layout->addWidget(m_raceTable);
    layout->addWidget(m_raceCount);
    m_eventsDock = new QDockWidget(tr("Events"), this);
    m_eventsDock->setObjectName(QStringLiteral("eventsDock"));
    m_eventsDock->setWidget(panel);
    addDockWidget(Qt::RightDockWidgetArea, m_eventsDock);
}

void MainWindow::applyRaceFilter()
{
    m_raceProxy->setFilterFixedString(m_raceFilterEdit->text().trimmed());
    m_raceCount->setText(m_raceModel->rowCount() == 0 ? tr("No race events for this track")
                                                      : tr("%1 of %2 races; select one to show its route")
                                                            .arg(m_raceProxy->rowCount())
                                                            .arg(m_raceModel->rowCount()));
}

void MainWindow::onRaceSelectionChanged()
{
    if (m_syncingSelection) {
        return;
    }
    const QModelIndexList rows = m_raceTable->selectionModel()->selectedRows();
    // Filtering the selected row away clears the table's selection; the race
    // stays shown until the user picks another or hides it.
    if (!rows.isEmpty()) {
        selectRace(m_raceProxy->mapToSource(rows.first()).row(), true);
    }
}

void MainWindow::selectRace(int race, bool focus)
{
    if (!m_map || race < 0 || static_cast<std::size_t>(race) >= m_map->races.size()) {
        return;
    }
    removeRaceOverlay();
    m_selectedRoutePoint.clear();
    m_selectedRace = race;
    m_hideRaceButton->setEnabled(true);
    const fh1::Race& selected = m_map->races[static_cast<std::size_t>(race)];

    if (!m_syncingSelection) {
        m_syncingSelection = true;
        const QModelIndex proxy = m_raceProxy->mapFromSource(m_raceModel->index(race, 0));
        if (proxy.isValid()) {
            m_raceTable->selectionModel()->select(
                proxy, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            m_raceTable->scrollTo(proxy);
        }
        m_syncingSelection = false;
    }

    // Other props of the same event are shown in the 3D world even when the
    // install lacks the route file.
    applyEventPropFilter();
    // Undo follows the route on screen, once it has been edited.
    if (selected.route >= 0) {
        m_edits->setActiveFile(routeFileKey(selected.route));
    }
    updateEditActions();
    if (selected.route >= 0) {
        showRaceOverlay();
    } else {
        statusBar()->showMessage(tr("%1 runs on route %2, which this install has no route file for")
                                     .arg(selected.name)
                                     .arg(selected.routeId),
            8000);
    }
    if (m_selection.layer < 0 && !m_selectedModel) {
        showRaceProperties();
    }
    if (focus) {
        focusOnRace();
    }
}

void MainWindow::showRaceOverlay()
{
    removeRaceOverlay();
    const int route = selectedRoute();
    if (route < 0) {
        return;
    }
    auto overlay = std::make_shared<fh1::MapData>();
    overlay->trackName = m_map->trackName;
    overlay->calibration = m_map->calibration;
    overlay->layers = fh1::raceOverlay(
        m_map->races[static_cast<std::size_t>(m_selectedRace)], m_map->raceRoutes[static_cast<std::size_t>(route)]);
    m_raceOverlay = overlay;
    for (std::size_t i = 0; i < overlay->layers.size(); ++i) {
        const fh1::Layer& layer = overlay->layers[i];
        // Past the map's layer indices, so the palette offsets differ; the
        // race layers set their own colours anyway.
        const int layerIndex = static_cast<int>(m_map->layers.size() + i);
        LayerItem* item = nullptr;
        if (layer.kind == fh1::FeatureKind::Point) {
            item = new PointLayerItem(layer, m_map->calibration, layerIndex, kRaceMarkerRadius);
        } else {
            auto* shapes = new ShapeLayerItem(layer, m_map->calibration, layerIndex);
            shapes->setLineWidthScale(kRaceLineWidthScale);
            item = shapes;
        }
        item->setZValue(kRaceOverlayZ + static_cast<double>(i));
        if (layer.id == fh1::kRacePointsLayer && !m_selectedRoutePoint.isEmpty()) {
            for (std::size_t f = 0; f < layer.features.size(); ++f) {
                if (layer.features[f].name == m_selectedRoutePoint) {
                    item->setHighlightedFeature(static_cast<int>(f));
                }
            }
        }
        m_scene->addItem(item);
        m_raceItems.push_back(item);
    }
    m_world3D->setOverlay(m_raceOverlay);
    updateLabelSources();
}

int MainWindow::selectedRoute() const
{
    if (!m_map || m_selectedRace < 0) {
        return -1;
    }
    return m_map->races[static_cast<std::size_t>(m_selectedRace)].route;
}

void MainWindow::updateEditActions()
{
    const bool canEditMap = m_map && (selectedRoute() >= 0 || gameObjectsLayer() >= 0);
    m_editMapAction->setEnabled(canEditMap);
    if (!canEditMap && m_editMapAction->isChecked()) {
        m_editMapAction->setChecked(false);
    }
    m_editRaceAction->setEnabled(m_map && m_selectedRace >= 0
        && m_map->races[static_cast<std::size_t>(m_selectedRace)].eventRow >= 0 && !m_map->carClasses.empty());
    m_saveEditsAction->setEnabled(!m_edits->modifiedFiles().isEmpty());
    m_outputFolderAction->setEnabled(m_installOpen);

    // Actions that need a selection are greyed out without one.
    const int layer = gameObjectsLayer();
    const bool objectSelected = layer >= 0 && m_selection.layer == layer;
    bool pointDeletable = false;
    if (!m_selectedRoutePoint.isEmpty() && selectedRoute() >= 0 && m_editMapAction->isChecked()) {
        const fh1::RaceRoute& route = m_map->raceRoutes[static_cast<std::size_t>(selectedRoute())];
        const std::optional<std::size_t> index = fh1::routeTransformIndex(route, m_selectedRoutePoint);
        pointDeletable = index && fh1::canInsertOrRemove(route, *index);
    }
    m_deleteAction->setEnabled(objectSelected || pointDeletable || m_selectedRace >= 0);
    m_deleteGroupAction->setEnabled(objectSelected
        && fh1::gameObjectGroup(m_map->gameObjects, static_cast<std::size_t>(m_selection.feature), kGroupRadius).size()
            > 1);
    m_clearSelectionAction->setEnabled(m_map && (m_selection.layer >= 0 || m_selectedModel.has_value()));
    m_hideRaceButton->setEnabled(m_selectedRace >= 0);
    m_deleteEventAction->setEnabled(m_map && m_selectedRace >= 0);
}

void MainWindow::setMapEditing(bool editing)
{
    m_drag.reset();
    if (editing) {
        // The map asks on hover and on press; what is under a press is what
        // a drag that follows moves. The race's points are drawn on top, so
        // they win.
        m_view->setGrabTest([this](const QPointF& scenePos) {
            m_grab.reset();
            if (const std::optional<std::size_t> point = routePointAt(scenePos)) {
                m_grab = MapGrab{true, *point};
            } else if (const std::optional<std::size_t> object = gameObjectAt(scenePos)) {
                m_grab = MapGrab{false, *object};
            }
            return m_grab.has_value();
        });
        if (m_showWorld3D) {
            showWorld3D(false);
        }
        statusBar()->showMessage(tr("Editing on the map: drag a route point or gameplay object to move it, Alt-drag "
                                    "to turn it, right-click it to add or delete"),
            10000);
    } else {
        m_view->setGrabTest({});
        if (!m_selectedRoutePoint.isEmpty()) {
            m_selectedRoutePoint.clear();
            showRaceOverlay();
        }
    }
    updateEditActions();
}

std::optional<std::size_t> MainWindow::routePointAt(const QPointF& scenePos) const
{
    const int route = selectedRoute();
    if (route < 0 || !m_raceOverlay) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < m_raceItems.size() && i < m_raceOverlay->layers.size(); ++i) {
        const fh1::Layer& layer = m_raceOverlay->layers[i];
        if (layer.id != fh1::kRacePointsLayer) {
            continue;
        }
        const std::optional<LayerItem::Hit> hit = m_raceItems[i]->hitTest(scenePos, kPickRadius / m_view->zoom());
        if (!hit) {
            return std::nullopt;
        }
        return fh1::routeTransformIndex(m_map->raceRoutes[static_cast<std::size_t>(route)],
            layer.features[static_cast<std::size_t>(hit->feature)].name);
    }
    return std::nullopt;
}

int MainWindow::gameObjectsLayer() const
{
    if (!m_map || m_map->gameObjects.objects.empty()) {
        return -1;
    }
    for (std::size_t i = 0; i < m_map->layers.size(); ++i) {
        if (m_map->layers[i].id == QLatin1String("gameobjs")
            && m_map->layers[i].features.size() == m_map->gameObjects.objects.size()) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::optional<std::size_t> MainWindow::gameObjectAt(const QPointF& scenePos) const
{
    const int layer = gameObjectsLayer();
    if (layer < 0 || static_cast<std::size_t>(layer) >= m_layerItems.size()) {
        return std::nullopt;
    }
    const std::optional<LayerItem::Hit> hit
        = m_layerItems[static_cast<std::size_t>(layer)]->hitTest(scenePos, kPickRadius / m_view->zoom());
    if (!hit) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(hit->feature);
}

void MainWindow::onGrabMoved(const QPointF& scenePos, Qt::KeyboardModifiers modifiers)
{
    if (!m_map) {
        return;
    }
    const int route = selectedRoute();
    const int layer = gameObjectsLayer();
    if (!m_drag) {
        if (!m_grab) {
            return;
        }
        MapDrag drag;
        drag.grab = *m_grab;
        if (drag.grab.routePoint && route >= 0) {
            drag.routeBefore = m_map->raceRoutes[static_cast<std::size_t>(route)];
        } else if (!drag.grab.routePoint && layer >= 0) {
            drag.objectsBefore
                = GameObjectsState{m_map->gameObjects, m_map->layers[static_cast<std::size_t>(layer)].features};
            // Shift takes one object out of its group.
            drag.group = (modifiers & Qt::ShiftModifier) != 0
                ? std::vector<std::size_t>{drag.grab.index}
                : fh1::gameObjectGroup(m_map->gameObjects, drag.grab.index, kGroupRadius);
        } else {
            return;
        }
        m_drag = std::move(drag);
    }
    MapDrag& drag = m_drag.value();
    const QPointF world = m_map->calibration.imageToWorld(scenePos);
    const bool turning = (modifiers & Qt::AltModifier) != 0;
    const std::size_t index = drag.grab.index;
    if (drag.grab.routePoint) {
        fh1::RaceRoute& state = m_map->raceRoutes[static_cast<std::size_t>(route)];
        const QVector3D current = state.transforms[index].position;
        const QVector3D target(static_cast<float>(world.x()), current.y(), static_cast<float>(world.y()));
        if (turning) {
            fh1::turnRoutePoint(state, index, target - current);
        } else {
            fh1::moveRoutePoint(state, index, target);
        }
        showRaceOverlay();
    } else {
        fh1::GameObjectsFile& file = m_map->gameObjects;
        const QVector3D current = file.objects[index].position;
        const QVector3D target(static_cast<float>(world.x()), current.y(), static_cast<float>(world.y()));
        const QVector3D forward = file.objects[index].axes[2];
        if (turning) {
            // The group turns about the grabbed object until it faces the
            // cursor.
            const QVector3D toward = target - current;
            if (!QVector3D(toward.x(), 0.0F, toward.z()).isNull()
                && !QVector3D(forward.x(), 0.0F, forward.z()).isNull()) {
                fh1::turnGameObjects(file, drag.group, current,
                    std::atan2(toward.z(), toward.x()) - std::atan2(forward.z(), forward.x()));
            }
        } else {
            const QVector3D delta = target - current;
            for (const std::size_t member : drag.group) {
                fh1::moveGameObject(file, member, file.objects[member].position + delta);
            }
        }
        for (const std::size_t member : drag.group) {
            syncGameObjectFeature(member);
        }
        if (auto* item = dynamic_cast<PointLayerItem*>(m_layerItems[static_cast<std::size_t>(layer)])) {
            item->refreshPositions();
        }
    }
    (turning ? drag.turned : drag.moved) = true;
}

void MainWindow::onGrabReleased(const QPointF& scenePos, Qt::KeyboardModifiers modifiers)
{
    Q_UNUSED(scenePos);
    Q_UNUSED(modifiers);
    if (!m_drag) {
        // A click without a drag selects what it landed on.
        if (!m_grab) {
            return;
        }
        const MapGrab grab = *m_grab;
        if (grab.routePoint && selectedRoute() >= 0) {
            const fh1::RaceRoute& state = m_map->raceRoutes[static_cast<std::size_t>(selectedRoute())];
            if (grab.index < state.transforms.size()) {
                m_selectedRoutePoint = state.transforms[grab.index].name;
                showRaceOverlay();
                updateEditActions();
                statusBar()->showMessage(fh1::canInsertOrRemove(state, grab.index)
                        ? tr("Selected %1; press Delete to delete it").arg(m_selectedRoutePoint)
                        : tr("Selected %1").arg(m_selectedRoutePoint),
                    8000);
            }
        } else if (!grab.routePoint && gameObjectsLayer() >= 0) {
            m_selectedRoutePoint.clear();
            showRaceOverlay();
            selectFeature(gameObjectsLayer(), static_cast<int>(grab.index), false);
        }
        return;
    }
    const MapDrag drag = std::move(*m_drag);
    m_drag.reset();
    if (!drag.moved && !drag.turned) {
        return;
    }
    const std::size_t index = drag.grab.index;
    if (drag.grab.routePoint && drag.routeBefore) {
        const int route = selectedRoute();
        fh1::RaceRoute& state = m_map->raceRoutes[static_cast<std::size_t>(route)];
        const QString name = state.transforms[index].name;
        if (drag.moved) {
            // Dragging on the map moves a point across it; the ground
            // decides its height.
            const QVector3D position = state.transforms[index].position;
            if (const std::optional<float> ground = groundHeightAt(position.x(), position.z())) {
                fh1::moveRoutePoint(state, index, QVector3D(position.x(), *ground, position.z()));
            }
        }
        pushRouteEdit(route, *drag.routeBefore, drag.moved ? tr("Move %1").arg(name) : tr("Turn %1").arg(name));
        showRaceOverlay();
        showRaceProperties();
    } else if (!drag.grab.routePoint && drag.objectsBefore) {
        fh1::GameObjectsFile& file = m_map->gameObjects;
        if (drag.moved) {
            // Each object of a group stands on the ground where it ended up.
            for (const std::size_t member : drag.group) {
                const QVector3D position = file.objects[member].position;
                if (const std::optional<float> ground = groundHeightAt(position.x(), position.z())) {
                    fh1::moveGameObject(file, member, QVector3D(position.x(), *ground, position.z()));
                    syncGameObjectFeature(member);
                }
            }
        }
        const QString id = file.objects[index].gameplayId;
        const auto others = static_cast<int>(drag.group.size()) - 1;
        QString description = drag.moved ? tr("Move %1").arg(id) : tr("Turn %1").arg(id);
        if (others > 0) {
            description = drag.moved ? tr("Move %1 and %n related object(s)", nullptr, others).arg(id)
                                     : tr("Turn %1 and %n related object(s)", nullptr, others).arg(id);
        }
        pushGameObjectsEdit(*drag.objectsBefore, description);
    }
}

std::optional<float> MainWindow::groundHeightAt(float x, float z) const
{
    if (const std::optional<float> ground = m_world3D->groundHeightAt(x, z)) {
        return ground;
    }
    return m_map ? fh1::roadHeightNear(*m_map, x, z, kRoadSnapRadius) : std::nullopt;
}

void MainWindow::registerEditableFiles()
{
    m_edits->reset(m_install.mediaPath());
    m_loadedRaces = m_map->races;
    m_deletedRaces.clear();
    if (gameObjectsLayer() >= 0) {
        m_edits->addFile(QString(kGameObjectsFile), tr("GameObjs.xml (gameplay objects)"), m_map->gameObjects.mediaPath,
            [this](QString*) -> std::optional<QByteArray> { return fh1::writeGameObjects(m_map->gameObjects); });
    }
    const QString database = m_install.resolve(QStringLiteral("db/gamedb.slt"));
    if (!database.isEmpty() && !m_map->races.empty()) {
        m_edits->addFile(QString(kDatabaseFile), tr("gamedb.slt (race settings)"),
            QDir(m_install.mediaPath()).relativeFilePath(database), [this, database](QString* error) {
                return fh1::writeRaceSettings(database, m_map->races, m_loadedRaces, m_deletedRaces, error);
            });
    }
}

void MainWindow::pushRouteEdit(int route, const fh1::RaceRoute& before, const QString& description)
{
    const QString key = routeFileKey(route);
    const auto index = static_cast<std::size_t>(route);
    // The history names a route file after the races run on it.
    QStringList races;
    for (const fh1::Race& race : m_map->races) {
        if (race.route == route) {
            races.append(race.name);
        }
    }
    const QString file = QFileInfo(m_map->raceRoutes[index].mediaPath).fileName();
    m_edits->addFile(key, races.isEmpty() ? file : tr("%1 (%2)").arg(file, races.join(QStringLiteral(", "))),
        m_map->raceRoutes[index].mediaPath,
        [this, index](QString*) -> std::optional<QByteArray> { return fh1::writeRaceRoute(m_map->raceRoutes[index]); });
    m_edits->push(key,
        new StateCommand<fh1::RaceRoute>(
            before, m_map->raceRoutes[index],
            [this, route](const fh1::RaceRoute& state) {
                m_map->raceRoutes[static_cast<std::size_t>(route)] = state;
                onRouteChanged(route);
            },
            description));
}

void MainWindow::pushGameObjectsEdit(const GameObjectsState& before, const QString& description)
{
    const int layer = gameObjectsLayer();
    if (layer < 0) {
        return;
    }
    GameObjectsState after{m_map->gameObjects, m_map->layers[static_cast<std::size_t>(layer)].features};
    m_edits->push(QString(kGameObjectsFile),
        new StateCommand<GameObjectsState>(
            before, std::move(after),
            [this, layer](const GameObjectsState& state) {
                m_map->gameObjects = state.file;
                m_map->layers[static_cast<std::size_t>(layer)].features = state.features;
                refreshGameObjects();
            },
            description));
    refreshGameObjects();
}

void MainWindow::onRouteChanged(int route)
{
    if (route == selectedRoute()) {
        showRaceOverlay();
        if (m_selection.layer < 0 && !m_selectedModel) {
            showRaceProperties();
        }
    }
}

void MainWindow::syncGameObjectFeature(std::size_t index)
{
    const int layer = gameObjectsLayer();
    if (layer < 0 || index >= m_map->gameObjects.objects.size()) {
        return;
    }
    const fh1::GameObject& object = m_map->gameObjects.objects[index];
    fh1::Feature& feature = m_map->layers[static_cast<std::size_t>(layer)].features[index];
    feature.position = object.position;
    feature.forward = object.axes[2];
    for (auto& [name, value] : feature.properties) {
        if (name == QLatin1String("Position")) {
            value = fh1::loaders::formatVector(object.position);
        } else if (name == QLatin1String("Forward (Z axis)")) {
            value = fh1::loaders::formatVector(object.axes[2]);
        }
    }
}

void MainWindow::refreshGameObjects()
{
    const int layer = gameObjectsLayer();
    if (layer < 0 || static_cast<std::size_t>(layer) >= m_layerItems.size()) {
        return;
    }
    // Features may have gone or come back, so selections by index are
    // dropped rather than kept pointing at another object.
    if (m_selection.layer == layer) {
        clearSelection();
    }
    if (!m_script) {
        saveLayerVisibility();
    }
    m_view->setLabelSources({});
    LayerItem*& slot = m_layerItems[static_cast<std::size_t>(layer)];
    const fh1::Layer& data = m_map->layers[static_cast<std::size_t>(layer)];
    auto* item = new PointLayerItem(data, m_map->calibration, layer, markerRadiusFor(data.id));
    item->setIcons(m_icons);
    item->setZValue(slot->zValue());
    m_scene->removeItem(slot);
    delete slot;
    slot = item;
    m_scene->addItem(item);
    buildLayerTree();
    m_model->setMap(m_map.get());
    applyFilter();
    m_world3D->setEntities(m_map, entityVisibility());
    updateLabelSources();
}

void MainWindow::deleteGameObjects(const std::vector<std::size_t>& indices, bool ask)
{
    const int layer = gameObjectsLayer();
    if (layer < 0 || indices.empty()) {
        return;
    }
    std::vector<std::size_t> sorted = indices;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    if (sorted.back() >= m_map->gameObjects.objects.size()) {
        return;
    }
    std::vector<fh1::Feature>& features = m_map->layers[static_cast<std::size_t>(layer)].features;
    // Activities that use one of the objects, with the objects they use.
    QMap<QString, QStringList> used;
    for (const std::size_t index : sorted) {
        QString activity;
        QString config;
        for (const auto& [name, value] : features[index].properties) {
            if (name == QLatin1String("Activity")) {
                activity = value;
            } else if (name == QLatin1String("Config")) {
                config = value;
            }
        }
        const QString id = m_map->gameObjects.objects[index].gameplayId;
        if (!activity.isEmpty()) {
            used[tr("the activity %1 (%2)").arg(activity, config)].append(id);
        }
        const QStringList scripts = m_map->scripts.filesUsing(id);
        if (!scripts.isEmpty()) {
            used[tr("the game's scripts (%1)").arg(scripts.join(QStringLiteral(", ")))].append(id);
        }
    }
    const QString first = m_map->gameObjects.objects[sorted.front()].gameplayId;
    if (ask && !used.isEmpty()) {
        QStringList lines;
        for (auto [activity, ids] : used.asKeyValueRange()) {
            lines.append(tr("%1, used by %2").arg(ids.join(QStringLiteral(", ")), activity));
        }
        if (QMessageBox::question(this, tr("Delete Gameplay Objects"),
                tr("The game uses objects you are deleting:\n\n%1\n\nDeleting them can break the game where it "
                   "uses them. Delete anyway?")
                    .arg(lines.join(QLatin1Char('\n'))),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes) {
            return;
        }
    }
    GameObjectsState before{m_map->gameObjects, features};
    for (auto it = sorted.rbegin(); it != sorted.rend(); ++it) {
        fh1::removeGameObject(m_map->gameObjects, *it);
        features.erase(features.begin() + static_cast<std::ptrdiff_t>(*it));
    }
    const auto others = static_cast<int>(sorted.size()) - 1;
    pushGameObjectsEdit(before,
        others > 0 ? tr("Delete %1 and %n related object(s)", nullptr, others).arg(first) : tr("Delete %1").arg(first));
}

void MainWindow::deleteSelection(bool withGroup)
{
    if (!m_map) {
        return;
    }
    // Delete acts on the panel being worked in: in the Events list, the
    // race event.
    const QWidget* focus = QApplication::focusWidget();
    const bool inRaceList = focus != nullptr && (focus == m_raceTable || m_raceTable->isAncestorOf(focus));
    if (inRaceList && m_selectedRace >= 0) {
        deleteRaceEvent();
        return;
    }
    const int route = selectedRoute();
    if (!m_selectedRoutePoint.isEmpty() && route >= 0 && m_editMapAction->isChecked()) {
        fh1::RaceRoute& state = m_map->raceRoutes[static_cast<std::size_t>(route)];
        const std::optional<std::size_t> index = fh1::routeTransformIndex(state, m_selectedRoutePoint);
        if (index && fh1::canInsertOrRemove(state, *index)) {
            const fh1::RaceRoute before = state;
            const QString name = m_selectedRoutePoint;
            fh1::removeRoutePoint(state, *index);
            m_selectedRoutePoint.clear();
            pushRouteEdit(route, before, tr("Delete %1").arg(name));
            showRaceOverlay();
            showRaceProperties();
        } else {
            statusBar()->showMessage(
                tr("Only checkpoints and waypoints can be deleted; drag the others instead"), 8000);
        }
        return;
    }
    const int layer = gameObjectsLayer();
    if (layer >= 0 && m_selection.layer == layer) {
        const auto index = static_cast<std::size_t>(m_selection.feature);
        deleteGameObjects(withGroup ? fh1::gameObjectGroup(m_map->gameObjects, index, kGroupRadius)
                                    : std::vector<std::size_t>{index});
        return;
    }
    if (m_selectedRace >= 0) {
        deleteRaceEvent();
        return;
    }
    statusBar()->showMessage(tr("Select a race event, a gameplay object, or a checkpoint or waypoint while editing "
                                "on the map, to delete it"),
        8000);
}

void MainWindow::onLayerTreeContextMenu(const QPoint& position)
{
    QTreeWidgetItem* item = m_layerTree->itemAt(position);
    if (item == nullptr) {
        return;
    }
    const int layer = item->data(0, kLayerRole).toInt();
    const int group = item->data(0, kGroupRole).toInt();
    const auto setAll = [this](bool checked) {
        for (int i = 0; i < m_layerTree->topLevelItemCount(); ++i) {
            m_layerTree->topLevelItem(i)->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
        }
    };
    QMenu menu(this);
    if (layer != kBackgroundLayer && group >= 0 && item->parent() != nullptr) {
        QTreeWidgetItem* parent = item->parent();
        menu.addAction(tr("Show Only This Group"), this, [parent, item] {
            for (int c = 0; c < parent->childCount(); ++c) {
                parent->child(c)->setCheckState(0, parent->child(c) == item ? Qt::Checked : Qt::Unchecked);
            }
        });
        menu.addAction(tr("Show Every Group of %1").arg(parent->text(0)), this,
            [parent] { parent->setCheckState(0, Qt::Checked); });
    } else if (layer != kBackgroundLayer) {
        menu.addAction(tr("Show Only This Layer"), this, [this, item] {
            for (int i = 0; i < m_layerTree->topLevelItemCount(); ++i) {
                QTreeWidgetItem* top = m_layerTree->topLevelItem(i);
                if (top->data(0, kLayerRole).toInt() != kBackgroundLayer) {
                    top->setCheckState(0, top == item ? Qt::Checked : Qt::Unchecked);
                }
            }
        });
    }
    menu.addSeparator();
    menu.addAction(tr("Show All"), this, [setAll] { setAll(true); });
    menu.addAction(tr("Hide All"), this, [setAll] { setAll(false); });
    menu.exec(m_layerTree->viewport()->mapToGlobal(position));
}

void MainWindow::onRaceTableContextMenu(const QPoint& position)
{
    const QModelIndex row = m_raceTable->indexAt(position);
    if (!row.isValid() || !m_map) {
        return;
    }
    const int race = m_raceProxy->mapToSource(row).row();
    if (race != m_selectedRace) {
        selectRace(race, false);
    }
    QMenu menu(this);
    menu.addAction(tr("&Show Route"), this, &MainWindow::focusOnRace);
    menu.addAction(m_editMapAction);
    menu.addAction(m_editRaceAction);
    menu.addAction(tr("&Hide Race"), this, &MainWindow::clearRace);
    menu.addAction(m_deleteEventAction);
    menu.addSeparator();
    menu.addAction(tr("Copy Event &ID"), this,
        [id = m_map->races[static_cast<std::size_t>(race)].eventId] { QApplication::clipboard()->setText(id); });
    menu.exec(m_raceTable->viewport()->mapToGlobal(position));
}

void MainWindow::addDeleteGameObjectActions(QMenu& menu, std::size_t index)
{
    // The menu offers the Edit menu's own actions, which act on the
    // selection and show their shortcuts, so the object is selected first.
    const int layer = gameObjectsLayer();
    if (layer < 0 || index >= m_map->gameObjects.objects.size()) {
        return;
    }
    m_selectedRoutePoint.clear();
    selectFeature(layer, static_cast<int>(index), false);
    if (!relatedGameObjects(index).empty()) {
        menu.addAction(tr("Show &Related Objects"), this, [this, index] {
            if (index < m_map->gameObjects.objects.size()) {
                focusOnGameObjectGroup(index);
            }
        });
    }
    menu.addAction(m_deleteAction);
    if (fh1::gameObjectGroup(m_map->gameObjects, index, kGroupRadius).size() > 1) {
        menu.addAction(m_deleteGroupAction);
    }
}

void MainWindow::onTableContextMenu(const QPoint& position)
{
    const QModelIndex row = m_table->indexAt(position);
    if (!row.isValid() || !m_map) {
        return;
    }
    const FeatureTableModel::Location location = m_model->locationAt(m_proxy->mapToSource(row).row());
    if (location.layer < 0) {
        return;
    }
    const fh1::Feature& feature
        = m_map->layers[static_cast<std::size_t>(location.layer)].features[static_cast<std::size_t>(location.feature)];
    QMenu menu(this);
    menu.addAction(tr("&Jump To"), this, [this, location] {
        m_syncingSelection = true;
        selectFeature(location.layer, location.feature, true);
        m_syncingSelection = false;
    });
    menu.addAction(tr("Copy &ID"), this, [name = feature.name] { QApplication::clipboard()->setText(name); });
    menu.addAction(tr("Copy &Position"), this,
        [position = feature.position] { QApplication::clipboard()->setText(fh1::loaders::formatVector(position)); });
    if (location.layer == gameObjectsLayer()) {
        menu.addSeparator();
        addDeleteGameObjectActions(menu, static_cast<std::size_t>(location.feature));
    }
    menu.exec(m_table->viewport()->mapToGlobal(position));
}

void MainWindow::editRaceSettings()
{
    if (!m_map || m_selectedRace < 0) {
        return;
    }
    const int raceIndex = m_selectedRace;
    const fh1::Race& race = m_map->races[static_cast<std::size_t>(raceIndex)];
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Race Settings: %1").arg(race.name));

    auto* laps = new QSpinBox;
    laps->setRange(1, kMaxLaps);
    laps->setValue(race.laps);
    // Every car needs a place on the start grid.
    int gridSlots = 0;
    if (race.route >= 0) {
        gridSlots = static_cast<int>(
            fh1::routePoints(m_map->raceRoutes[static_cast<std::size_t>(race.route)], fh1::RoutePointKind::StartSlot)
                .size());
    }
    auto* opponents = new QSpinBox;
    opponents->setRange(0, std::max(gridSlots > 0 ? gridSlots - 1 : kDefaultMaxOpponents, race.opponents));
    opponents->setValue(race.opponents);
    auto* prize = new QSpinBox;
    prize->setRange(0, kMaxPrize);
    prize->setSingleStep(kPrizeStep);
    prize->setGroupSeparatorShown(true);
    prize->setSuffix(tr(" CR"));
    prize->setValue(race.prize);
    auto* carClass = new QComboBox;
    for (const fh1::CarClass& entry : m_map->carClasses) {
        carClass->addItem(entry.name, entry.id);
    }
    carClass->setCurrentIndex(std::max(0, carClass->findData(race.carClassId)));
    auto* start = new QTimeEdit(QTime(0, 0).addSecs(race.timeOfDay));
    start->setDisplayFormat(QStringLiteral("HH:mm"));

    auto* form = new QFormLayout;
    form->addRow(tr("&Laps:"), laps);
    form->addRow(tr("&Opponents:"), opponents);
    form->addRow(tr("&Prize:"), prize);
    form->addRow(tr("Car &class:"), carClass);
    form->addRow(tr("&Start time:"), start);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(dialog);
    layout->addLayout(form);
    layout->addWidget(buttons);

    connect(dialog, &QDialog::accepted, this, [=, this] {
        if (!m_map || static_cast<std::size_t>(raceIndex) >= m_map->races.size()) {
            return;
        }
        fh1::Race& target = m_map->races[static_cast<std::size_t>(raceIndex)];
        const fh1::Race before = target;
        target.laps = laps->value();
        target.opponents = opponents->value();
        target.prize = prize->value();
        target.carClassId = carClass->currentData().toInt();
        target.carClass = carClass->currentText();
        target.timeOfDay = QTime(0, 0).secsTo(start->time());
        if (target.laps == before.laps && target.opponents == before.opponents && target.prize == before.prize
            && target.carClassId == before.carClassId && target.timeOfDay == before.timeOfDay) {
            return;
        }
        const auto apply = [this, raceIndex](const fh1::Race& state) {
            m_map->races[static_cast<std::size_t>(raceIndex)] = state;
            m_raceModel->refresh();
            onEditsChanged();
        };
        m_edits->push(QString(kDatabaseFile),
            new StateCommand<fh1::Race>(before, target, apply, tr("Change the settings of %1").arg(target.eventId)));
        apply(target);
    });
    dialog->open();
}

void MainWindow::deleteRaceEvent()
{
    if (!m_map || m_selectedRace < 0) {
        return;
    }
    const int raceIndex = m_selectedRace;
    const fh1::Race race = m_map->races[static_cast<std::size_t>(raceIndex)];

    // The event's gameplay objects: the one named after it and its group.
    std::vector<std::size_t> objects;
    if (gameObjectsLayer() >= 0) {
        for (std::size_t i = 0; i < m_map->gameObjects.objects.size(); ++i) {
            if (m_map->gameObjects.objects[i].gameplayId.compare(race.eventId, Qt::CaseInsensitive) == 0) {
                objects = fh1::gameObjectGroup(m_map->gameObjects, i, kGroupRadius);
                break;
            }
        }
    }
    // The rows that refer to it, as the game's database has them.
    const QString database = m_install.resolve(QStringLiteral("db/gamedb.slt"));
    std::vector<std::pair<QString, int>> references;
    if (!database.isEmpty() && race.eventRow >= 0) {
        fh1::GameDatabase db;
        if (db.open(database)) {
            references = db.eventReferences(race.eventRow);
        }
    }
    static const QHash<QString, QString> tableNames{
        {QStringLiteral("Races"), tr("race")},
        {QStringLiteral("EventParticipants"), tr("AI drivers")},
        {QStringLiteral("EventRecommendedCars"), tr("recommended cars")},
        {QStringLiteral("EventRestrictions"), tr("car restrictions")},
        {QStringLiteral("EventShowroomChallenges"), tr("showroom challenges")},
        {QStringLiteral("EventUIColors"), tr("colours")},
        {QStringLiteral("Event_Music"), tr("music")},
        {QStringLiteral("EventHubInitialEvents"), tr("hub")},
        {QStringLiteral("Rewards_EventPrizes"), tr("prizes")},
        {QStringLiteral("Rewards_EventUnlock"), tr("unlocks")},
    };
    QStringList referenceText;
    for (const auto& [table, count] : references) {
        referenceText.append(QStringLiteral("%1 %2").arg(tableNames.value(table, table)).arg(count));
    }

    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Delete Race Event"));
    auto* question = new QLabel(
        tr("<b>Delete %1 (%2)?</b> Choose what goes:").arg(race.name.toHtmlEscaped(), race.eventId.toHtmlEscaped()));
    QStringList objectNames;
    for (std::size_t i = 0; i < objects.size() && i < kListedNames; ++i) {
        objectNames.append(m_map->gameObjects.objects[objects[i]].gameplayId);
    }
    if (objects.size() > kListedNames) {
        objectNames.append(tr("…"));
    }
    auto* objectsBox = new QCheckBox(objects.empty()
            ? tr("Gameplay objects: none are named after %1").arg(race.eventId)
            : tr("Its %n gameplay object(s) in GameObjs.xml: %1", nullptr, static_cast<int>(objects.size()))
                  .arg(objectNames.join(QStringLiteral(", "))));
    objectsBox->setChecked(!objects.empty());
    objectsBox->setEnabled(!objects.empty());
    const bool canDeleteRows = !database.isEmpty() && race.eventRow >= 0;
    auto* rowsBox = new QCheckBox(!canDeleteRows ? tr("Database rows: the event is not in gamedb.slt")
            : referenceText.isEmpty()            ? tr("The event in gamedb.slt")
                                                 : tr("The event in gamedb.slt, and the rows that refer to it: %1")
                                                       .arg(referenceText.join(QStringLiteral(", "))));
    rowsBox->setChecked(canDeleteRows);
    rowsBox->setEnabled(canDeleteRows);

    // Events the game itself relies on start unticked: deleting the opening
    // race froze the game at the start.
    QStringList scripts = m_map->scripts.filesUsing(race.eventId);
    for (const std::size_t index : objects) {
        scripts += m_map->scripts.filesUsing(m_map->gameObjects.objects[index].gameplayId);
    }
    scripts.removeDuplicates();
    scripts.sort(Qt::CaseInsensitive);
    QStringList risks;
    if (race.level < 0) {
        risks.append(tr("It is a special event of the career, such as its opening race (Level %1 in gamedb.slt).")
                .arg(race.level));
    }
    if (!scripts.isEmpty()) {
        risks.append(tr("The game's scripts use it or its objects: %1.").arg(scripts.join(QStringLiteral(", "))));
    }
    QWidget* warning = nullptr;
    if (!risks.isEmpty()) {
        objectsBox->setChecked(false);
        rowsBox->setChecked(false);
        warning = new QWidget;
        auto* icon = new QLabel;
        const int size = style()->pixelMetric(QStyle::PM_MessageBoxIconSize);
        icon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(size, size));
        icon->setAlignment(Qt::AlignTop);
        auto* text = new QLabel(
            tr("%1 Deleting it can stop the game working, so nothing is ticked.").arg(risks.join(QLatin1Char(' '))));
        text->setWordWrap(true);
        auto* row = new QHBoxLayout(warning);
        row->setContentsMargins(0, 0, 0, 0);
        row->addWidget(icon);
        row->addWidget(text, 1);
    }
    auto* note = new QLabel(tr("Left as they are: its props (barriers, banners) in bin.zip, its activity in "
                               "gamemodes.zip, its effects in ParticleEmitters.xml, and its route file, which "
                               "other races can share."));
    note->setWordWrap(true);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    QPushButton* deleteButton = buttons->addButton(tr("Delete"), QDialogButtonBox::AcceptRole);
    const auto updateButton = [deleteButton, objectsBox, rowsBox] {
        deleteButton->setEnabled(objectsBox->isChecked() || rowsBox->isChecked());
    };
    connect(objectsBox, &QCheckBox::toggled, dialog, updateButton);
    connect(rowsBox, &QCheckBox::toggled, dialog, updateButton);
    updateButton();
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(dialog);
    layout->addWidget(question);
    if (warning != nullptr) {
        layout->addWidget(warning);
    }
    layout->addWidget(objectsBox);
    layout->addWidget(rowsBox);
    layout->addWidget(note);
    layout->addWidget(buttons);

    connect(dialog, &QDialog::accepted, this, [this, raceIndex, eventId = race.eventId, objects, objectsBox, rowsBox] {
        if (!m_map || static_cast<std::size_t>(raceIndex) >= m_map->races.size()
            || m_map->races[static_cast<std::size_t>(raceIndex)].eventId != eventId) {
            return;
        }
        const bool deleteObjects = objectsBox->isChecked();
        const bool deleteRows = rowsBox->isChecked();
        clearRace();
        if (deleteObjects) {
            deleteGameObjects(objects, false);
        }
        if (deleteRows) {
            const DatabaseState before{m_map->races, m_loadedRaces, m_deletedRaces};
            DatabaseState after = before;
            const auto at = static_cast<std::ptrdiff_t>(raceIndex);
            after.deleted.push_back(after.loaded[static_cast<std::size_t>(raceIndex)]);
            after.races.erase(after.races.begin() + at);
            after.loaded.erase(after.loaded.begin() + at);
            const auto apply = [this](const DatabaseState& state) {
                clearRace();
                m_map->races = state.races;
                m_loadedRaces = state.loaded;
                m_deletedRaces = state.deleted;
                m_raceModel->setMap(m_map.get());
                applyRaceFilter();
                onEditsChanged();
            };
            m_edits->push(QString(kDatabaseFile),
                new StateCommand<DatabaseState>(before, after, apply, tr("Delete event %1").arg(eventId)));
            apply(after);
        }
    });
    dialog->open();
}

void MainWindow::onEditsChanged()
{
    m_raceModel->setModifiedRaces(editedRaces());
    setWindowModified(!m_edits->modifiedFiles().isEmpty());
    updateEditActions();
    updateEventPropsChoices();
    if (m_selectedRace >= 0 && m_selection.layer < 0 && !m_selectedModel) {
        showRaceProperties();
    }
}

QSet<int> MainWindow::editedRaces() const
{
    QSet<int> races;
    if (!m_map) {
        return races;
    }
    const bool database = m_edits->isModified(QString(kDatabaseFile));
    for (std::size_t i = 0; i < m_map->races.size(); ++i) {
        const fh1::Race& race = m_map->races[i];
        bool edited = race.route >= 0 && m_edits->isModified(routeFileKey(race.route));
        if (database && i < m_loadedRaces.size()) {
            const fh1::Race& loaded = m_loadedRaces[i];
            edited = edited || race.laps != loaded.laps || race.opponents != loaded.opponents
                || race.prize != loaded.prize || race.carClassId != loaded.carClassId
                || race.timeOfDay != loaded.timeOfDay;
        }
        if (edited) {
            races.insert(static_cast<int>(i));
        }
    }
    return races;
}

void MainWindow::removeRaceOverlay()
{
    m_view->setLabelSources({});
    for (LayerItem* item : m_raceItems) {
        m_scene->removeItem(item);
        delete item;
    }
    m_raceItems.clear();
    m_raceOverlay.reset();
    m_world3D->setOverlay(nullptr);
    updateLabelSources();
}

const fh1::RouteTransform* MainWindow::raceStart() const
{
    if (!m_map || m_selectedRace < 0) {
        return nullptr;
    }
    const fh1::Race& race = m_map->races[static_cast<std::size_t>(m_selectedRace)];
    if (race.route < 0) {
        return nullptr;
    }
    const std::vector<const fh1::RouteTransform*> grid
        = fh1::routePoints(m_map->raceRoutes[static_cast<std::size_t>(race.route)], fh1::RoutePointKind::StartSlot);
    return grid.empty() ? nullptr : grid.front();
}

void MainWindow::clearRace()
{
    removeRaceOverlay();
    const bool hadRace = m_selectedRace >= 0;
    m_selectedRace = -1;
    m_selectedRoutePoint.clear();
    updateEditActions();
    applyEventPropFilter();
    m_hideRaceButton->setEnabled(false);
    if (!m_syncingSelection) {
        m_syncingSelection = true;
        m_raceTable->clearSelection();
        m_syncingSelection = false;
    }
    if (hadRace && m_selection.layer < 0 && !m_selectedModel) {
        m_properties->setRowCount(0);
    }
}

void MainWindow::focusOnRace()
{
    if (m_raceItems.empty()) {
        return;
    }
    QRectF bounds;
    for (const LayerItem* item : m_raceItems) {
        bounds = bounds.united(item->boundingRect());
    }
    m_view->fitRect(bounds, kFocusZoom);
    if (const fh1::RouteTransform* start = raceStart()) {
        m_world3D->lookAlong(start->position, start->facing);
    }
}

void MainWindow::showRaceProperties()
{
    m_properties->setRowCount(0);
    if (!m_map || m_selectedRace < 0) {
        return;
    }
    const fh1::Race& race = m_map->races[static_cast<std::size_t>(m_selectedRace)];
    const QLocale locale;
    fh1::Properties rows{
        {tr("Event"), race.name},
        {tr("Event ID"), race.eventId},
        {tr("Type"), race.type},
        {tr("Route"), race.routeName},
        {tr("Route ID"), QString::number(race.routeId)},
        {tr("Laps"), QString::number(race.laps)},
        {tr("Opponents"), QString::number(race.opponents)},
        {tr("Start time"), QTime(0, 0).addSecs(race.timeOfDay).toString(QStringLiteral("HH:mm"))},
        {tr("Length"), tr("%1 m").arg(locale.toString(race.length))},
        {tr("Car class"), race.carClass},
        {tr("Prize"), tr("%1 CR").arg(locale.toString(race.prize))},
    };
    if (race.route < 0) {
        rows.append({tr("Route file"), tr("missing from this install")});
    } else {
        const fh1::RaceRoute& route = m_map->raceRoutes[static_cast<std::size_t>(race.route)];
        rows.append({tr("Route file"), route.source});
        rows.append({tr("Racing line"),
            route.racingLine.empty()
                ? tr("none; the route is drawn straight between checkpoints")
                : tr("%n point(s) from aiopenworld.zip", nullptr, static_cast<int>(route.racingLine.size()))});
        rows.append({tr("Start grid"),
            tr("%n slot(s)", nullptr,
                static_cast<int>(fh1::routePoints(route, fh1::RoutePointKind::StartSlot).size()))});
        rows.append(
            {tr("Checkpoints"), QString::number(fh1::routePoints(route, fh1::RoutePointKind::Checkpoint).size())});
        if (fh1::isRouteEdited(route)) {
            rows.append({tr("Route edits"),
                m_edits->isModified(routeFileKey(race.route)) ? tr("not saved yet (File → Save Edits)") : tr("saved")});
            if (!route.racingLine.empty()) {
                rows.append({tr("Racing line"), tr("the game's; it does not follow the edited checkpoints")});
            }
        }
    }
    switch (m_eventProps) {
    case EventProps::None:
        rows.append({tr("Event props"), tr("hidden (Event props in 3D: None)")});
        break;
    case EventProps::SelectedRace:
        rows.append({tr("Event props"),
            tr("only the props named after %1 or put out for route %2").arg(race.eventId).arg(race.routeId)});
        break;
    case EventProps::AllEvents:
        rows.append({tr("Event props"), tr("every event's props (Event props in 3D: All events)")});
        break;
    }
    m_properties->setRowCount(static_cast<int>(rows.size()));
    for (int i = 0; i < rows.size(); ++i) {
        auto* value = new QTableWidgetItem(rows.at(i).second);
        value->setToolTip(rows.at(i).second);
        m_properties->setItem(i, 0, new QTableWidgetItem(rows.at(i).first));
        m_properties->setItem(i, 1, value);
    }
}

void MainWindow::setEventProps(EventProps mode)
{
    m_eventProps = mode;
    const auto index = static_cast<std::size_t>(mode);
    m_eventPropsActions[index]->setChecked(true);
    m_eventPropsCombo->setCurrentIndex(static_cast<int>(index));
    if (!m_script) {
        QSettings settings;
        settings.setValue(QStringLiteral("view/eventProps"), QString(kEventPropsSettings[index]));
        settings.remove(QStringLiteral("view/showEventProps"));
    }
    applyEventPropFilter();
    if (m_selectedRace >= 0 && m_selection.layer < 0 && !m_selectedModel) {
        showRaceProperties();
    }
}

void MainWindow::updateEventPropsChoices()
{
    const auto selectedRace = static_cast<std::size_t>(EventProps::SelectedRace);
    const bool haveRace = m_map && m_selectedRace >= 0;
    QString race;
    if (haveRace) {
        const fh1::Race& selected = m_map->races[static_cast<std::size_t>(m_selectedRace)];
        race = selected.name == selected.eventId ? selected.name
                                                 : QStringLiteral("%1 (%2)").arg(selected.name, selected.eventId);
    }
    QAction* action = m_eventPropsActions[selectedRace];
    // Menu text treats '&' as a mnemonic marker; race names can contain one.
    QString escaped = race;
    escaped.replace(QLatin1Char('&'), QLatin1String("&&"));
    action->setText(haveRace ? tr("&Selected Race: %1").arg(escaped) : tr("&Selected Race (none selected)"));
    action->setEnabled(haveRace);

    const bool edited = haveRace && editedRaces().contains(m_selectedRace);
    m_selectedRaceLabel->setText(!haveRace ? tr("No race selected")
            : edited                       ? tr("Selected: %1, route edited").arg(race)
                                           : tr("Selected: %1").arg(race));

    const auto item = static_cast<int>(selectedRace);
    m_eventPropsCombo->setItemText(
        item, haveRace ? tr("Selected race: %1").arg(race) : tr("Selected race (none selected)"));
    if (auto* model = qobject_cast<QStandardItemModel*>(m_eventPropsCombo->model())) {
        model->item(item)->setEnabled(haveRace);
    }
}

void MainWindow::applyEventPropFilter()
{
    updateEventPropsChoices();
    switch (m_eventProps) {
    case EventProps::None:
        m_world3D->setEventPropFilter({});
        break;
    case EventProps::SelectedRace:
        // With no race selected, the world looks as it does outside events.
        m_world3D->setEventPropFilter(m_map && m_selectedRace >= 0
                ? fh1::EventPropFilter::race(m_map->races[static_cast<std::size_t>(m_selectedRace)].eventId,
                      m_map->races[static_cast<std::size_t>(m_selectedRace)].routeId)
                : fh1::EventPropFilter());
        break;
    case EventProps::AllEvents:
        m_world3D->setEventPropFilter(fh1::EventPropFilter::all());
        break;
    }
}

void MainWindow::updateLabelSources()
{
    std::vector<const LayerItem*> sources(m_raceItems.rbegin(), m_raceItems.rend());
    sources.insert(sources.end(), m_layerItems.rbegin(), m_layerItems.rend());
    m_view->setLabelSources(std::move(sources));
}

std::vector<std::vector<bool>> MainWindow::entityVisibility() const
{
    std::vector<std::vector<bool>> visible;
    visible.reserve(m_layerItems.size());
    for (const LayerItem* item : m_layerItems) {
        std::vector<bool>& groups = visible.emplace_back();
        for (int g = 0; g < item->groups().size(); ++g) {
            groups.push_back(item->isVisible() && item->isGroupVisible(g));
        }
    }
    return visible;
}

void MainWindow::onLoadedFilesChanged()
{
    if (m_world3D->archive().get() != m_debugArchive) {
        m_debugArchive = m_world3D->archive().get();
        m_debugPanel->setWorld(m_world3D->archive(), m_world3D->index(), m_world3D->textures());
    }
    if (!m_debugRefresh->isActive()) {
        m_debugRefresh->start();
    }
}

void MainWindow::refreshDebugPanel()
{
    if (m_debugDock->isVisible() && m_debugArchive != nullptr) {
        m_debugPanel->setLoadedFiles(m_world3D->loadedModels(), m_world3D->loadedTextures());
    }
}

void MainWindow::clearCache()
{
    const QDir directory(worldCacheDirectory());
    const QFileInfoList files = directory.entryInfoList({QStringLiteral("*.index")}, QDir::Files);
    if (files.isEmpty()) {
        QMessageBox::information(this, tr("Clear Cache"), tr("The cache is already empty."));
        return;
    }
    qint64 bytes = 0;
    for (const QFileInfo& file : files) {
        bytes += file.size();
    }
    const QString question
        = tr("Delete %n cached world index(es), %1 MiB in %2?", nullptr, static_cast<int>(files.size()))
              .arg(QLocale().toString(static_cast<double>(bytes) / (1024.0 * 1024.0), 'f', 1),
                  QDir::toNativeSeparators(directory.path()));
    const QString detail = m_world3D->hasWorld()
        ? tr("A track's index is rebuilt the next time its 3D world opens, which takes a few seconds. "
             "The 3D world shown now is rebuilt straight away.")
        : tr("A track's index is rebuilt the next time its 3D world opens, which takes a few seconds.");
    QMessageBox box(QMessageBox::Question, tr("Clear Cache"), question, QMessageBox::Cancel, this);
    box.setInformativeText(detail);
    QPushButton* deleteButton = box.addButton(tr("Delete"), QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != deleteButton) {
        return;
    }

    QStringList failed;
    for (const QFileInfo& file : files) {
        if (!QFile::remove(file.absoluteFilePath())) {
            failed << file.fileName();
        }
    }
    if (!failed.isEmpty()) {
        reportError(tr("Clear Cache"), tr("Could not delete %1.").arg(failed.join(QStringLiteral(", "))));
    }
    const auto deleted = static_cast<int>(files.size() - failed.size());
    statusBar()->showMessage(tr("Deleted %n cached world index(es).", nullptr, deleted), 8000);

    if (m_world3D->hasWorld()) {
        m_world3D->clearWorld();
        m_worldTrack.clear();
        if (m_showWorld3D) {
            ensureWorld();
        }
    }
}

void MainWindow::createStatusBar()
{
    m_busy = new QProgressBar;
    m_busy->setRange(0, 0);
    m_busy->setMaximumWidth(120);
    m_busy->setTextVisible(false);
    statusBar()->addPermanentWidget(m_busy);

    m_warningsButton = new QToolButton;
    m_warningsButton->setAutoRaise(true);
    m_warningsButton->setToolTip(tr("Show problems met while loading this track"));
    connect(m_warningsButton, &QToolButton::clicked, this, &MainWindow::showWarnings);
    statusBar()->addPermanentWidget(m_warningsButton);

    m_cursorLabel = new QLabel;
    m_cursorLabel->setMinimumWidth(
        m_cursorLabel->fontMetrics().horizontalAdvance(QStringLiteral("X -00000.0   Z -00000.0")));
    m_cursorLabel->setToolTip(tr("World position under the cursor, in metres"));
    statusBar()->addPermanentWidget(m_cursorLabel);
}

void MainWindow::reportError(const QString& title, const QString& message)
{
    if (m_script) {
        std::fprintf(stderr, "%s\n", qPrintable(message));
    } else {
        QMessageBox::warning(this, title, message);
    }
}

bool MainWindow::openGameFolder(const QString& path, const QString& track)
{
    fh1::GameInstall install;
    if (!install.open(path)) {
        reportError(tr("Not a game folder"), install.errorString());
        return false;
    }
    const QStringList tracks = install.trackFolders();
    if (tracks.isEmpty()) {
        reportError(tr("No tracks found"),
            tr("%1 has no track folders with ribbon data under tracks.").arg(install.mediaPath()));
        return false;
    }

    QString chosen;
    auto pick = [&tracks, &chosen](const QString& candidate) {
        if (!chosen.isEmpty() || candidate.isEmpty()) {
            return;
        }
        for (const QString& name : tracks) {
            if (name.compare(candidate, Qt::CaseInsensitive) == 0) {
                chosen = name;
                return;
            }
        }
    };
    pick(track);
    if (!track.isEmpty() && chosen.isEmpty()) {
        reportError(tr("Track not found"),
            tr("Track %1 was not found; available: %2").arg(track, tracks.join(QStringLiteral(", "))));
        if (m_script) {
            return false;
        }
    }
    QSettings settings;
    pick(settings.value(QStringLiteral("lastTrack")).toString());
    pick(QStringLiteral("colorado"));
    pick(tracks.first());

    m_install = install;
    m_installOpen = true;
    settings.setValue(QStringLiteral("gameFolder"), path);
    m_trackCombo->clear();
    m_trackCombo->addItems(tracks);
    m_trackCombo->setCurrentText(chosen);
    loadTrack(chosen);
    return true;
}

void MainWindow::setScriptOptions(const ScriptOptions& options)
{
    m_script = options;
}

void MainWindow::chooseGameFolder()
{
    QSettings settings;
    const QString start = settings.value(QStringLiteral("gameFolder")).toString();
    const QString path = QFileDialog::getExistingDirectory(this, tr("Open Game Folder"), start);
    if (!path.isEmpty()) {
        openGameFolder(path);
    }
}

void MainWindow::loadTrack(const QString& track)
{
    if (!m_installOpen || track.isEmpty()) {
        return;
    }
    // Loading replaces the routes, edits and all.
    if (!m_script && !m_watcher.isRunning() && !m_edits->maybeSave()) {
        m_trackCombo->setCurrentText(m_trackName);
        return;
    }
    if (m_watcher.isRunning()) {
        m_pendingTrack = track;
        return;
    }
    setLoading(true);
    statusBar()->showMessage(tr("Loading %1…").arg(track));

    const fh1::GameInstall install = m_install;
    const QPointer<MainWindow> guard(this);
    m_watcher.setFuture(QtConcurrent::run([install, track, guard]() -> LoadResult {
        auto progress = [guard](const QString& step) {
            QMetaObject::invokeMethod(
                qApp,
                [guard, step] {
                    if (guard) {
                        guard->statusBar()->showMessage(step);
                    }
                },
                Qt::QueuedConnection);
        };
        try {
            return {std::make_shared<fh1::MapData>(fh1::MapLoader::load(install, track, progress)), {}};
        } catch (const fh1::LoadError& e) {
            return {nullptr, QString::fromStdString(e.what())};
        }
    }));
}

void MainWindow::onLoadFinished()
{
    LoadResult result = m_watcher.result();

    if (!m_pendingTrack.isEmpty()) {
        const QString next = m_pendingTrack;
        m_pendingTrack.clear();
        setLoading(false);
        loadTrack(next);
        return;
    }

    if (!result.map) {
        setLoading(false);
        statusBar()->showMessage(tr("Could not load the track"));
        reportError(tr("Could not load the track"), result.error);
        if (m_script) {
            emit scriptFinished(1);
        }
        return;
    }

    clearSelection();
    clearRace();
    m_model->setMap(nullptr);
    m_raceModel->setMap(nullptr);
    m_map = std::move(result.map);
    m_trackName = m_map->trackName;
    QSettings().setValue(QStringLiteral("lastTrack"), m_trackName);
    if (m_worldCancel) {
        m_worldCancel->store(true);
    }
    m_world3D->clearWorld();
    m_worldTrack.clear();

    buildScene();
    buildLayerTree();
    m_world3D->setEntities(m_map, entityVisibility());
    m_model->setMap(m_map.get());
    applyFilter();
    m_raceModel->setMap(m_map.get());
    registerEditableFiles();
    applyRaceFilter();
    m_noDataLabel->setText(tr("<p><b>Nothing to show for %1</b></p>"
                              "<p>Its ribbon files hold no placed objects, routes or zones, and the game "
                              "ships no map image for it.</p>")
            .arg(m_trackName.toHtmlEscaped()));
    updateCentralPage();
    m_view->fitScene();
    if (m_showWorld3D) {
        ensureWorld();
    }
    setLoading(false);

    const auto warningCount = static_cast<int>(m_map->warnings.size());
    m_warningsButton->setVisible(warningCount > 0);
    m_warningsButton->setText(tr("%n load warning(s)", nullptr, warningCount));

    std::size_t total = 0;
    for (const fh1::Layer& layer : m_map->layers) {
        total += layer.features.size();
    }
    statusBar()->showMessage(
        tr("Loaded %1: %2 objects in %3 layers").arg(m_trackName).arg(total).arg(m_map->layers.size()), 8000);
    updateWindowTitle();

    if (m_script) {
        QTimer::singleShot(0, this, &MainWindow::runScript);
    }
}

void MainWindow::buildScene()
{
    m_view->setLabelSources({});
    m_scene->clear();
    m_layerItems.clear();
    m_background = nullptr;

    const fh1::MapCalibration calibration = m_map->calibration;
    m_icons.clear();
    for (auto [key, image] : m_map->icons.asKeyValueRange()) {
        m_icons.insert(key,
            QPixmap::fromImage(image.scaled(kIconPixels, kIconPixels, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    }
    QRectF bounds;
    if (!m_map->background.isNull()) {
        m_background = new BackgroundItem(m_map->background);
        m_background->setZValue(0.0);
        m_scene->addItem(m_background);
        bounds = m_background->boundingRect();
        // The pixmaps now hold the image; the decoded copy is no longer needed.
        m_map->background = QImage();
    }

    for (std::size_t i = 0; i < m_map->layers.size(); ++i) {
        const fh1::Layer& layer = m_map->layers[i];
        LayerItem* item = nullptr;
        if (layer.kind == fh1::FeatureKind::Point) {
            auto* points = new PointLayerItem(layer, calibration, static_cast<int>(i), markerRadiusFor(layer.id));
            points->setIcons(m_icons);
            item = points;
        } else {
            item = new ShapeLayerItem(layer, calibration, static_cast<int>(i));
        }
        item->setZValue(1.0 + static_cast<double>(i));
        m_scene->addItem(item);
        m_layerItems.push_back(item);
        if (m_background == nullptr) {
            bounds = bounds.united(item->boundingRect());
        }
    }

    if (m_background == nullptr) {
        constexpr double kMarginFraction = 0.05;
        bounds.adjust(-bounds.width() * kMarginFraction, -bounds.height() * kMarginFraction,
            bounds.width() * kMarginFraction, bounds.height() * kMarginFraction);
    }
    m_scene->setSceneRect(bounds);
    m_view->setMetresPerSceneUnit(calibration.metresPerPixel());
    updateLabelSources();
}

void MainWindow::buildLayerTree()
{
    const QSignalBlocker blocker(m_layerTree);
    m_layerTree->clear();
    QSettings settings;

    const QStringList* scriptedLayers
        = m_script && m_script->visibleLayers ? &m_script->visibleLayers.value() : nullptr;
    // The top of the tree is the top of the drawing order.
    for (auto it = m_layerItems.rbegin(); it != m_layerItems.rend(); ++it) {
        LayerItem* layerItem = *it;
        const fh1::Layer& layer = layerItem->layer();
        const QString key = settingsKey(m_trackName, layer.id) + QStringLiteral("/hidden");
        const bool useDefaults = scriptedLayers != nullptr || !settings.contains(key);
        const QStringList hidden = settings.value(key).toStringList();
        const bool visibleByDefault = scriptedLayers != nullptr ? scriptedLayers->contains(layer.id)
                                                                : layersShownByDefault().contains(layer.id);

        auto* top = new QTreeWidgetItem(m_layerTree);
        top->setText(0, tr("%1 (%2)").arg(layer.title).arg(layer.features.size()));
        top->setToolTip(0, tr("Source: %1").arg(layer.source));
        top->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
        top->setData(0, kLayerRole, layerItem->layerIndex());
        top->setData(0, kGroupRole, -1);

        const QStringList& groups = layerItem->groups();
        for (int g = 0; g < groups.size(); ++g) {
            const bool visible = useDefaults
                ? visibleByDefault && (scriptedLayers != nullptr || groupShownByDefault(groups.at(g)))
                : !hidden.contains(groups.at(g));
            layerItem->setGroupVisible(g, visible);
            auto* child = new QTreeWidgetItem(top);
            const QString name = groups.at(g).isEmpty() ? tr("(none)") : groups.at(g);
            child->setText(0, tr("%1 (%2)").arg(name).arg(layerItem->groupSize(g)));
            const QPixmap icon = layerItem->groupIcon(g);
            child->setIcon(0, icon.isNull() ? swatch(layerItem->groupColor(g)) : QIcon(icon));
            child->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            child->setData(0, kLayerRole, layerItem->layerIndex());
            child->setData(0, kGroupRole, g);
            child->setCheckState(0, visible ? Qt::Checked : Qt::Unchecked);
        }
    }

    if (m_background != nullptr) {
        auto* item = new QTreeWidgetItem(m_layerTree, {tr("Satellite image")});
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        item->setData(0, kLayerRole, kBackgroundLayer);
        item->setData(0, kGroupRole, -1);
        item->setCheckState(0, Qt::Checked);
        item->setToolTip(0, tr("Source: %1").arg(m_map->backgroundSource));
    }
}

QTreeWidgetItem* MainWindow::groupTreeItem(int layer, int group) const
{
    for (int i = 0; i < m_layerTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* top = m_layerTree->topLevelItem(i);
        if (top->data(0, kLayerRole).toInt() != layer) {
            continue;
        }
        for (int c = 0; c < top->childCount(); ++c) {
            if (top->child(c)->data(0, kGroupRole).toInt() == group) {
                return top->child(c);
            }
        }
    }
    return nullptr;
}

void MainWindow::onLayerTreeChanged(QTreeWidgetItem* item, int column)
{
    if (column != 0) {
        return;
    }
    const int layer = item->data(0, kLayerRole).toInt();
    const int group = item->data(0, kGroupRole).toInt();
    const bool checked = item->checkState(0) == Qt::Checked;
    if (layer == kBackgroundLayer) {
        if (m_background != nullptr) {
            m_background->setVisible(checked);
        }
        return;
    }
    if (group < 0 || layer < 0 || static_cast<std::size_t>(layer) >= m_layerItems.size()) {
        return;
    }
    m_layerItems[static_cast<std::size_t>(layer)]->setGroupVisible(group, checked);
    m_world3D->setEntityGroupVisible(layer, group, checked);
    if (!m_script) {
        saveLayerVisibility();
    }
}

void MainWindow::saveLayerVisibility() const
{
    QSettings settings;
    for (const LayerItem* layerItem : m_layerItems) {
        QStringList hidden;
        for (int g = 0; g < layerItem->groups().size(); ++g) {
            if (!layerItem->isGroupVisible(g)) {
                hidden.append(layerItem->groups().at(g));
            }
        }
        settings.setValue(settingsKey(m_trackName, layerItem->layer().id) + QStringLiteral("/hidden"), hidden);
    }
}

void MainWindow::selectFeature(int layer, int feature, bool focus)
{
    if (layer < 0 || static_cast<std::size_t>(layer) >= m_layerItems.size()) {
        return;
    }
    LayerItem* item = m_layerItems[static_cast<std::size_t>(layer)];
    m_selectedModel.reset();
    m_world3D->setHighlightedModel(std::nullopt);
    if (m_selection.layer >= 0 && m_selection.layer != layer) {
        m_layerItems[static_cast<std::size_t>(m_selection.layer)]->setHighlightedFeature(-1);
    }
    m_selection = {layer, feature};
    item->setHighlightedFeature(feature);
    showRelatedGameObjects();

    // A feature picked from the table may belong to a hidden group; show the
    // group so the highlight is visible on the map.
    const int group = item->groupOf(feature);
    if (!item->isGroupVisible(group)) {
        if (QTreeWidgetItem* treeItem = groupTreeItem(layer, group)) {
            treeItem->setCheckState(0, Qt::Checked);
        }
    }

    m_world3D->setHighlightedEntity(layer, feature);
    if (focus) {
        m_view->focusOn(item->featureBounds(feature), kFocusZoom);
        if (m_showWorld3D) {
            m_world3D->focusOnEntity(layer, feature);
        }
    }
    showProperties();
    updateEditActions();

    if (!m_syncingSelection) {
        m_syncingSelection = true;
        const QModelIndex source = m_model->index(m_model->rowOf(layer, feature), 0);
        const QModelIndex proxy = m_proxy->mapFromSource(source);
        if (proxy.isValid()) {
            m_table->selectionModel()->select(proxy, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            // Right after a model reset the table still has a layout pending
            // and ignores scrollTo(), so scroll once that has run.
            QTimer::singleShot(0, m_table, [table = m_table, row = QPersistentModelIndex(proxy)] {
                if (row.isValid()) {
                    table->scrollTo(row, QAbstractItemView::PositionAtCenter);
                }
            });
        } else {
            m_table->clearSelection();
        }
        m_syncingSelection = false;
    }
}

void MainWindow::clearSelection()
{
    if (m_selection.layer >= 0 && static_cast<std::size_t>(m_selection.layer) < m_layerItems.size()) {
        m_layerItems[static_cast<std::size_t>(m_selection.layer)]->setHighlightedFeature(-1);
    }
    m_selection = {};
    showRelatedGameObjects();
    m_world3D->setHighlightedEntity(-1, -1);
    m_selectedModel.reset();
    m_world3D->setHighlightedModel(std::nullopt);
    m_properties->setRowCount(0);
    if (!m_syncingSelection) {
        m_syncingSelection = true;
        m_table->clearSelection();
        m_syncingSelection = false;
    }
    // With nothing else selected, the panel describes the race shown.
    showRaceProperties();
    updateEditActions();
}

void MainWindow::selectModel(const fh1::PickHit& hit)
{
    // Only one thing is selected at a time.
    clearSelection();
    m_selectedModel = hit;
    m_world3D->setHighlightedModel(hit.chunk);
    // The debug panel lists the loaded model files; it shows this one on
    // its own there.
    m_debugPanel->showModel(hit.chunk);
    showModelProperties();
    updateEditActions();
}

void MainWindow::showModelProperties()
{
    m_properties->setRowCount(0);
    const std::shared_ptr<const fh1::WorldIndex> index = m_world3D->index();
    const std::shared_ptr<const fh1::ForzaZip> archive = m_world3D->archive();
    if (!m_selectedModel || !index || !archive || m_selectedModel->chunk >= index->chunks().size()) {
        return;
    }
    const fh1::WorldChunk& chunk = index->chunks()[m_selectedModel->chunk];
    const auto point = [](const QVector3D& p) {
        return QStringLiteral("%1, %2, %3").arg(p.x(), 0, 'f', 1).arg(p.y(), 0, 'f', 1).arg(p.z(), 0, 'f', 1);
    };
    const auto metres
        = [](float value) { return std::isinf(value) ? tr("no limit") : tr("%1 m").arg(value, 0, 'f', 0); };
    QString file;
    QString part;
    if (chunk.entry < archive->entries().size()) {
        const fh1::ZipEntry& entry = archive->entries()[chunk.entry];
        file = entry.name;
        if (const auto header
            = fh1::rendermesh::readHeader(archive->readPrefix(entry, fh1::rendermesh::kHeaderBytes))) {
            part = header->firstPartName;
        }
    }
    QString kind = tr("World geometry, drawn where its file puts it");
    if (chunk.backdrop) {
        kind = tr("Backdrop terrain");
    } else if (chunk.scattered) {
        kind = tr("Copy placed by a procedural set (trees, bushes, fences)");
    } else if (chunk.placed) {
        kind = tr("Prop placed by a zone file");
    }
    const QVector3D size = chunk.boundsMax - chunk.boundsMin;
    fh1::Properties rows{
        {tr("Model"), part},
        {tr("File"), file},
        {tr("Kind"), kind},
        {tr("Level of detail"), chunk.lod < 0 ? tr("none") : QString::number(static_cast<int>(chunk.lod))},
        {tr("Drawn from"), tr("%1 to %2").arg(metres(chunk.bandStart), metres(chunk.bandEnd))},
        {tr("Position"), point(chunk.placed ? chunk.placement.position : (chunk.boundsMin + chunk.boundsMax) / 2.0F)},
        {tr("Size"), tr("%1 × %2 × %3 m").arg(size.x(), 0, 'f', 1).arg(size.y(), 0, 'f', 1).arg(size.z(), 0, 'f', 1)},
        {tr("Triangles"), QLocale().toString(static_cast<qulonglong>(m_selectedModel->triangles))},
        {tr("Clicked at"), point(m_selectedModel->point)},
    };
    if (chunk.placed && chunk.placement.eventProp) {
        rows.append({tr("Shown"), tr("Only during races and events")});
    }
    m_properties->setRowCount(static_cast<int>(rows.size()));
    for (int i = 0; i < rows.size(); ++i) {
        auto* value = new QTableWidgetItem(rows.at(i).second);
        value->setToolTip(rows.at(i).second);
        m_properties->setItem(i, 0, new QTableWidgetItem(rows.at(i).first));
        m_properties->setItem(i, 1, value);
    }
}

void MainWindow::showProperties()
{
    m_properties->setRowCount(0);
    if (m_selection.layer < 0 || !m_map) {
        return;
    }
    const fh1::Layer& layer = m_map->layers[static_cast<std::size_t>(m_selection.layer)];
    const fh1::Feature& feature = layer.features[static_cast<std::size_t>(m_selection.feature)];

    fh1::Properties rows{
        {tr("ID"), feature.name},
        {tr("Layer"), layer.title},
        {tr("Group"), feature.group},
        {tr("Source"), layer.source},
    };
    rows.append(feature.properties);
    // Related gameplay objects are listed last; double-clicking one selects
    // it.
    std::vector<std::size_t> related;
    if (m_selection.layer == gameObjectsLayer()) {
        related = relatedGameObjects(static_cast<std::size_t>(m_selection.feature));
    }
    const auto firstRelated = static_cast<int>(rows.size());
    for (const std::size_t index : related) {
        const fh1::Feature& other = layer.features[index];
        rows.append({tr("Related object"),
            other.label.isEmpty() || other.label == other.name ? other.name
                                                               : tr("%1 (%2)").arg(other.name, other.label)});
    }

    m_properties->setRowCount(static_cast<int>(rows.size()));
    for (int i = 0; i < rows.size(); ++i) {
        auto* value = new QTableWidgetItem(rows.at(i).second);
        value->setToolTip(rows.at(i).second);
        auto* name = new QTableWidgetItem(rows.at(i).first);
        if (i >= firstRelated) {
            const auto index = static_cast<qulonglong>(related[static_cast<std::size_t>(i - firstRelated)]);
            for (QTableWidgetItem* cell : {name, value}) {
                cell->setData(kRelatedObjectRole, index);
                cell->setToolTip(tr("%1\nDouble-click to select it").arg(rows.at(i).second));
            }
        }
        m_properties->setItem(i, 0, name);
        m_properties->setItem(i, 1, value);
    }
}

std::vector<std::size_t> MainWindow::relatedGameObjects(std::size_t index) const
{
    if (gameObjectsLayer() < 0) {
        return {};
    }
    std::vector<std::size_t> group = fh1::gameObjectGroup(m_map->gameObjects, index, kGroupRadius);
    if (!group.empty()) {
        group.erase(group.begin());
    }
    return group;
}

void MainWindow::showRelatedGameObjects()
{
    const int layer = gameObjectsLayer();
    if (layer < 0 || static_cast<std::size_t>(layer) >= m_layerItems.size()) {
        return;
    }
    auto* item = dynamic_cast<PointLayerItem*>(m_layerItems[static_cast<std::size_t>(layer)]);
    if (item == nullptr) {
        return;
    }
    std::vector<int> related;
    if (m_selection.layer == layer) {
        for (const std::size_t index : relatedGameObjects(static_cast<std::size_t>(m_selection.feature))) {
            related.push_back(static_cast<int>(index));
        }
    }
    item->setRelatedFeatures(std::move(related));
}

void MainWindow::focusOnGameObjectGroup(std::size_t index)
{
    const int layer = gameObjectsLayer();
    if (layer < 0) {
        return;
    }
    selectFeature(layer, static_cast<int>(index), false);
    // QRectF::united() ignores zero-size rectangles, so the bounds of the
    // points are gathered from their extremes.
    double left = std::numeric_limits<double>::max();
    double top = std::numeric_limits<double>::max();
    double right = std::numeric_limits<double>::lowest();
    double bottom = std::numeric_limits<double>::lowest();
    for (const std::size_t member : fh1::gameObjectGroup(m_map->gameObjects, index, kGroupRadius)) {
        const QVector3D& p = m_map->gameObjects.objects[member].position;
        const QPointF scene = m_map->calibration.worldToImage(p.x(), p.z());
        left = std::min(left, scene.x());
        top = std::min(top, scene.y());
        right = std::max(right, scene.x());
        bottom = std::max(bottom, scene.y());
    }
    // A margin, so markers at the edges are not cut in half.
    constexpr double kMargin = 8.0;
    m_view->fitRect(
        QRectF(QPointF(left, top), QPointF(right, bottom)).adjusted(-kMargin, -kMargin, kMargin, kMargin), kFocusZoom);
}

void MainWindow::copyProperties()
{
    QStringList lines;
    const QModelIndexList rows = m_properties->selectionModel()->selectedRows();
    for (const QModelIndex& row : rows) {
        lines.append(QStringLiteral("%1\t%2").arg(
            m_properties->item(row.row(), 0)->text(), m_properties->item(row.row(), 1)->text()));
    }
    if (!lines.isEmpty()) {
        QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    }
}

void MainWindow::onMapClicked(const QPointF& scenePos)
{
    const double tolerance = kPickRadius / m_view->zoom();
    std::optional<LayerItem::Hit> best;
    int bestLayer = -1;
    // Walk from the top layer down so that, at equal distance, the feature
    // drawn on top wins.
    for (auto it = m_layerItems.rbegin(); it != m_layerItems.rend(); ++it) {
        const std::optional<LayerItem::Hit> hit = (*it)->hitTest(scenePos, tolerance);
        if (hit && (!best || hit->distance < best->distance)) {
            best = hit;
            bestLayer = (*it)->layerIndex();
        }
    }
    if (best) {
        selectFeature(bestLayer, best->feature, false);
    } else {
        clearSelection();
    }
}

void MainWindow::onCursorMoved(const QPointF& scenePos)
{
    if (!m_map) {
        return;
    }
    const QPointF world = m_map->calibration.imageToWorld(scenePos);
    m_cursorLabel->setText(tr("X %1   Z %2").arg(world.x(), 0, 'f', 1).arg(world.y(), 0, 'f', 1));
}

void MainWindow::onMapContextMenu(const QPointF& scenePos, const QPoint& globalPos)
{
    if (!m_map) {
        return;
    }
    const QPointF world = m_map->calibration.imageToWorld(scenePos);
    QMenu menu(this);
    const int route = selectedRoute();
    const bool editing = m_editMapAction->isChecked();
    const std::optional<std::size_t> point = editing ? routePointAt(scenePos) : std::optional<std::size_t>();
    const std::optional<std::size_t> object = !point ? gameObjectAt(scenePos) : std::optional<std::size_t>();
    if (route >= 0 && point && fh1::canInsertOrRemove(m_map->raceRoutes[static_cast<std::size_t>(route)], *point)) {
        const fh1::RaceRoute& state = m_map->raceRoutes[static_cast<std::size_t>(route)];
        const bool checkpoint = fh1::routePointKind(state.transforms[*point].name) == fh1::RoutePointKind::Checkpoint;
        const QString name = state.transforms[*point].name;
        const auto edit = [this, route, name](const QString& description, auto&& change) {
            fh1::RaceRoute& target = m_map->raceRoutes[static_cast<std::size_t>(route)];
            const fh1::RaceRoute before = target;
            const std::optional<std::size_t> index = fh1::routeTransformIndex(target, name);
            if (index && change(target, *index)) {
                pushRouteEdit(route, before, description);
                showRaceOverlay();
                showRaceProperties();
            }
        };
        menu.addAction(checkpoint ? tr("Add Checkpoint After") : tr("Add Waypoint After"), this, [=] {
            edit(tr("Add after %1").arg(name), [](fh1::RaceRoute& target, std::size_t index) {
                return fh1::insertRoutePointAfter(target, index).has_value();
            });
        });
        m_selectedRoutePoint = name;
        showRaceOverlay();
        updateEditActions();
        menu.addAction(m_deleteAction);
        menu.addSeparator();
    } else if (object) {
        addDeleteGameObjectActions(menu, *object);
        menu.addSeparator();
    }
    menu.addAction(tr("Copy World Position (X, Z)"), this, [world] {
        QApplication::clipboard()->setText(
            QStringLiteral("%1, %2").arg(world.x(), 0, 'f', 2).arg(world.y(), 0, 'f', 2));
    });
    menu.addSeparator();
    menu.addAction(m_fitAction);
    menu.addAction(m_exportAction);
    menu.exec(globalPos);
}

void MainWindow::onTableSelectionChanged()
{
    if (m_syncingSelection) {
        return;
    }
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        return;
    }
    const FeatureTableModel::Location location = m_model->locationAt(m_proxy->mapToSource(rows.first()).row());
    if (location.layer >= 0) {
        m_syncingSelection = true;
        selectFeature(location.layer, location.feature, false);
        m_syncingSelection = false;
    }
}

void MainWindow::onTableActivated(const QModelIndex& index)
{
    const FeatureTableModel::Location location = m_model->locationAt(m_proxy->mapToSource(index).row());
    if (location.layer >= 0) {
        m_syncingSelection = true;
        selectFeature(location.layer, location.feature, true);
        m_syncingSelection = false;
    }
}

void MainWindow::applyFilter()
{
    m_proxy->setFilterFixedString(m_filterEdit->text().trimmed());
    m_tableCount->setText(
        tr("%1 of %2 objects; double-click one to jump to it").arg(m_proxy->rowCount()).arg(m_model->rowCount()));
}

void MainWindow::exportView()
{
    if (!m_map) {
        return;
    }
    const QString suggested = QStringLiteral("%1-map.png").arg(m_trackName);
    const QString path
        = QFileDialog::getSaveFileName(this, tr("Export View as PNG"), suggested, tr("PNG images (*.png)"));
    if (path.isEmpty()) {
        return;
    }
    if (!m_view->viewport()->grab().save(path, "PNG")) {
        QMessageBox::warning(this, tr("Export failed"), tr("Could not write %1.").arg(path));
        return;
    }
    statusBar()->showMessage(tr("Saved %1").arg(path), 5000);
}

void MainWindow::showWarnings()
{
    if (!m_map || m_map->warnings.isEmpty()) {
        return;
    }
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Load warnings"));
    box.setText(tr("%n problem(s) while loading %1. The layers that loaded are shown.", nullptr,
        static_cast<int>(m_map->warnings.size()))
            .arg(m_trackName));
    box.setDetailedText(m_map->warnings.join(QLatin1Char('\n')));
    box.exec();
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("About FH1 Map Viewer"),
        tr("<p><b>FH1 Map Viewer</b> %1</p>"
           "<p>Shows the world of Forza Horizon (Xbox 360) from an extracted disc: the "
           "in-game satellite map from <tt>UI.zip</tt>, gameplay and collision objects "
           "from the track's ribbon XML, AI race routes from <tt>aiopenworld.zip</tt> "
           "named through <tt>gamedb.slt</tt>, the road network from the track's "
           "<tt>.nav</tt> file, particle emitters and post-processing zones. Names come "
           "from the game's string tables and activity configs, and icons from its map "
           "render profile.</p>"
           "<p>Compressed archives are read with a built-in XMemCompress (LZX) decoder, "
           "and every file is checked against its CRC-32.</p>")
            .arg(QApplication::applicationVersion()));
}

void MainWindow::setLoading(bool loading)
{
    m_busy->setVisible(loading);
    m_openAction->setEnabled(!loading);
    m_trackCombo->setEnabled(!loading && m_trackCombo->count() > 1);
    const bool haveMap = !loading && m_map != nullptr;
    m_exportAction->setEnabled(haveMap);
    m_zoomInAction->setEnabled(haveMap);
    m_zoomOutAction->setEnabled(haveMap);
    m_fitAction->setEnabled(haveMap);
    m_findAction->setEnabled(haveMap);
    m_clearSelectionAction->setEnabled(haveMap && (m_selection.layer >= 0 || m_selectedModel.has_value()));
    m_warningsAction->setEnabled(haveMap && !m_map->warnings.isEmpty());
    m_view2DAction->setEnabled(haveMap);
    m_view3DAction->setEnabled(haveMap);
    if (!haveMap) {
        m_warningsButton->setVisible(false);
    }
}

void MainWindow::updateWindowTitle()
{
    setWindowTitle(m_trackName.isEmpty() ? tr("FH1 Map Viewer[*]") : tr("%1[*] — FH1 Map Viewer").arg(m_trackName));
}

void MainWindow::runScript()
{
    if (!m_script) {
        return;
    }
    const ScriptOptions& script = *m_script;
    if (script.centre) {
        const QPointF scenePos = m_map->calibration.worldToImage(script.centre->x(), script.centre->y());
        m_view->focusOn(QRectF(scenePos, QSizeF(0.0, 0.0)), script.zoom);
    }
    if (!script.race.isEmpty()) {
        const auto race = std::find_if(m_map->races.begin(), m_map->races.end(),
            [&script](const fh1::Race& r) { return r.eventId.compare(script.race, Qt::CaseInsensitive) == 0; });
        if (race != m_map->races.end()) {
            selectRace(static_cast<int>(race - m_map->races.begin()), !script.centre.has_value());
            m_eventsDock->show();
            m_eventsDock->raise();
        } else {
            std::fprintf(stderr, "No race with event ID %s\n", qPrintable(script.race));
        }
    }
    if (!script.select.isEmpty()) {
        bool found = false;
        for (std::size_t l = 0; l < m_map->layers.size() && !found; ++l) {
            const auto& features = m_map->layers[l].features;
            for (std::size_t f = 0; f < features.size(); ++f) {
                if (features[f].name.compare(script.select, Qt::CaseInsensitive) == 0
                    || features[f].label.compare(script.select, Qt::CaseInsensitive) == 0) {
                    selectFeature(static_cast<int>(l), static_cast<int>(f), !script.centre.has_value());
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            std::fprintf(stderr, "No object named %s\n", qPrintable(script.select));
        }
    }
    if (script.world3D) {
        if (!script.screenshotPath.isEmpty()) {
            connect(
                m_world3D, &WorldView3D::settled, this, &MainWindow::takeScriptScreenshot, Qt::SingleShotConnection);
            // A world that never settles (missing archive, decode errors)
            // still produces a screenshot rather than hanging the script.
            QTimer::singleShot(180000, this, &MainWindow::takeScriptScreenshot);
        }
        showWorld3D(true);
        return;
    }
    if (!script.screenshotPath.isEmpty()) {
        takeScriptScreenshot();
    }
}

void MainWindow::takeScriptScreenshot()
{
    if (!m_script || m_script->screenshotPath.isEmpty()) {
        return;
    }
    const QString path = m_script->screenshotPath;
    m_script->screenshotPath.clear();
    // Give the view a moment to repaint at its final size before grabbing it.
    QTimer::singleShot(200, this, [this, path] {
        QPixmap shot = grab();
        if (m_showWorld3D && m_world3D->isVisible()) {
            // The window capture includes the 3D frame but not what QPainter
            // drew over it (the HUD); the widget's own framebuffer has both.
            QPainter painter(&shot);
            painter.drawImage(
                QRect(m_world3D->mapTo(this, QPoint(0, 0)), m_world3D->size()), m_world3D->grabFramebuffer());
        }
        const bool saved = shot.save(path, "PNG");
        if (!saved) {
            std::fprintf(stderr, "Could not write %s\n", qPrintable(path));
        }
        emit scriptFinished(saved ? 0 : 1);
    });
}

void MainWindow::updateCentralPage()
{
    if (!m_map) {
        m_stack->setCurrentIndex(0);
    } else if (m_showWorld3D) {
        m_stack->setCurrentWidget(m_world3D);
    } else if (m_map->layers.empty() && m_background == nullptr) {
        m_stack->setCurrentWidget(m_noDataLabel);
    } else {
        m_stack->setCurrentWidget(m_view);
    }
}

void MainWindow::showWorld3D(bool show)
{
    m_showWorld3D = show;
    (show ? m_view3DAction : m_view2DAction)->setChecked(true);
    updateCentralPage();
    if (show) {
        ensureWorld();
        m_world3D->setFocus();
    }
}

void MainWindow::ensureWorld()
{
    if (!m_map || m_worldTrack == m_trackName) {
        return;
    }
    const QString archivePath = m_install.resolve(QStringLiteral("tracks/%1/bin.zip").arg(m_trackName));
    if (archivePath.isEmpty()) {
        statusBar()->showMessage(tr("%1 has no bin.zip, so there is no 3D world to show").arg(m_trackName), 8000);
        return;
    }
    if (m_worldWatcher.isRunning()) {
        // The previous load was cancelled; this one starts when it returns.
        return;
    }
    m_worldTrack = m_trackName;
    m_worldCancel = std::make_shared<std::atomic<bool>>(false);
    // Deleting the cache while the index is being built would race it.
    m_clearCacheAction->setEnabled(false);
    const QString cachePath = QStringLiteral("%1/%2.index").arg(worldCacheDirectory(), m_trackName.toLower());
    const QString pvsPath = m_install.trackPvsPath(m_trackName);
    const QString zoneGridPath = m_install.trackZoneGridPath(m_trackName);
    const QPointer<MainWindow> guard(this);
    auto cancel = m_worldCancel;
    statusBar()->showMessage(tr("Opening the 3D world…"));
    m_busy->setVisible(true);
    m_worldWatcher.setFuture(
        QtConcurrent::run([archivePath, cachePath, pvsPath, zoneGridPath, cancel, guard]() -> WorldLoad {
            auto archive = std::make_shared<fh1::ForzaZip>();
            if (!archive->open(archivePath)) {
                return {nullptr, nullptr, nullptr, archive->errorString(), {}};
            }
            // The PVS file holds the texture tables and, with the zone files,
            // where props are placed.
            QByteArray pvs;
            QString pvsError = QStringLiteral("the track has no PVS file");
            if (!pvsPath.isEmpty()) {
                QFile file(pvsPath);
                if (file.open(QIODevice::ReadOnly)) {
                    pvs = file.readAll();
                } else {
                    pvsError = file.errorString();
                }
            }
            const QString signature = fh1::WorldIndex::archiveSignature(archivePath) + QLatin1Char('|')
                + fh1::WorldIndex::archiveSignature(pvsPath) + QLatin1Char('|')
                + fh1::WorldIndex::archiveSignature(zoneGridPath);
            std::optional<fh1::WorldIndex> index = fh1::WorldIndex::load(cachePath, signature);
            if (!index) {
                std::optional<fh1::TrackPlacements> placements;
                if (!pvs.isEmpty()) {
                    QString placementError;
                    placements = fh1::TrackPlacements::load(pvs, *archive, cancel.get(), &placementError);
                    if (!placements && !cancel->load()) {
                        std::fprintf(stderr, "Props are not placed: %s\n", qPrintable(placementError));
                    }
                }
                QString zoneError = QStringLiteral("the track has no zone grid file");
                const std::optional<fh1::ZoneGrid> zones
                    = zoneGridPath.isEmpty() ? std::nullopt : fh1::ZoneGrid::readFile(zoneGridPath, &zoneError);
                if (!zones) {
                    std::fprintf(stderr, "The backdrop terrain is drawn from everywhere: %s\n", qPrintable(zoneError));
                }
                index = fh1::WorldIndex::build(
                    *archive,
                    [guard](int done, int total) {
                        QMetaObject::invokeMethod(
                            qApp,
                            [guard, done, total] {
                                if (guard) {
                                    guard->statusBar()->showMessage(
                                        guard->tr("Indexing the 3D world (first time only): %1%")
                                            .arg(done * 100 / std::max(total, 1)));
                                }
                            },
                            Qt::QueuedConnection);
                    },
                    cancel.get(), placements ? &*placements : nullptr, zones ? &*zones : nullptr);
                if (!index) {
                    return {nullptr, nullptr, nullptr, QStringLiteral("cancelled"), {}};
                }
                QString error;
                if (!index->save(cachePath, signature, &error)) {
                    std::fprintf(stderr, "Could not cache the world index: %s\n", qPrintable(error));
                }
            }
            std::shared_ptr<const fh1::TrackTextures> textures;
            QString textureError = pvsError;
            if (!pvs.isEmpty()) {
                if (std::optional<fh1::TrackTextures> loaded = fh1::TrackTextures::load(pvs, *archive, &textureError)) {
                    textures = std::make_shared<const fh1::TrackTextures>(std::move(*loaded));
                }
            }
            return {archive, std::make_shared<const fh1::WorldIndex>(std::move(*index)), textures, {}, textureError};
        }));
}

void MainWindow::onWorldLoaded()
{
    const WorldLoad result = m_worldWatcher.result();
    m_busy->setVisible(false);
    m_clearCacheAction->setEnabled(true);
    if (!result.index) {
        const QString failedTrack = m_worldTrack;
        m_worldTrack.clear();
        if (result.error != QLatin1String("cancelled")) {
            statusBar()->showMessage(tr("Could not open the 3D world: %1").arg(result.error), 10000);
        }
        // A track switched while the old world was loading gets its turn now.
        if (m_showWorld3D && m_map && failedTrack != m_trackName) {
            ensureWorld();
        }
        return;
    }
    if (m_worldTrack != m_trackName) {
        m_worldTrack.clear();
        if (m_showWorld3D) {
            ensureWorld();
        }
        return;
    }
    m_world3D->setWorld(result.archive, result.index, result.textures);
    placeWorldCamera();
    QString message = tr("3D world: %1 meshes, %2 of them placed props")
                          .arg(result.index->chunks().size())
                          .arg(result.index->placedCount());
    if (result.index->localModelCount() > 0) {
        message += tr("; %n prop model(s) have no placement", nullptr, result.index->localModelCount());
    }
    if (!result.textures) {
        message += tr("; no game textures (%1)").arg(result.textureError);
    }
    statusBar()->showMessage(message, 10000);
}

void MainWindow::placeWorldCamera()
{
    if (m_script && m_script->camera) {
        const auto& c = *m_script->camera;
        WorldView3D::Camera camera;
        camera.position = QVector3D(c[0], c[1], c[2]);
        camera.yaw = c[3] * std::numbers::pi_v<float> / 180.0F;
        camera.pitch = c[4] * std::numbers::pi_v<float> / 180.0F;
        m_world3D->setCamera(camera);
        return;
    }
    if (const fh1::RouteTransform* start = raceStart()) {
        m_world3D->lookAlong(start->position, start->facing);
        return;
    }
    // Start above the middle of what the 2D map shows, facing north.
    const QPointF centre = m_view->mapToScene(m_view->viewport()->rect().center());
    const QPointF world = m_map->calibration.imageToWorld(centre);
    WorldView3D::Camera camera = m_world3D->camera();
    camera.yaw = std::numbers::pi_v<float> / 2.0F;
    camera.pitch = -0.35F;
    m_world3D->setCamera(camera);
    m_world3D->lookFromAbove(static_cast<float>(world.x()), static_cast<float>(world.y()), 150.0F);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (!m_script && !m_edits->maybeSave()) {
        event->ignore();
        return;
    }
    if (!m_script) {
        QSettings settings;
        settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
        settings.setValue(QStringLiteral("window/state"), saveState());
        settings.setValue(QStringLiteral("window/stateHasWorldDebug"), true);
        settings.setValue(QStringLiteral("window/stateHasEvents"), true);
        settings.setValue(QStringLiteral("window/stateHasEditHistory"), true);
    }
    QMainWindow::closeEvent(event);
}
