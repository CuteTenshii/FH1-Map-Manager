#include "MainWindow.h"

#include "BackgroundItem.h"
#include "FeatureTableModel.h"
#include "LayerItem.h"
#include "Loaders.h"
#include "MapLoader.h"
#include "MapView.h"
#include "WorldDebugPanel.h"
#include "WorldView3D.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QGraphicsScene>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableView>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <cstdio>
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
constexpr int kBackgroundLayer = -1;

/// Click tolerance around the cursor, in device pixels; about half the size of
/// a map icon.
constexpr double kPickRadius = 11.0;
/// Size icons are kept at in memory; they are drawn at up to 30 px.
constexpr int kIconPixels = 64;
/// Zoom (device pixels per image pixel) used when jumping to a point.
constexpr double kFocusZoom = 3.0;

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
    connect(m_world3D, &WorldView3D::emptyClicked, this, &MainWindow::clearSelection);

    createActions();
    createDocks();
    createStatusBar();

    connect(m_view, &MapView::clicked, this, &MainWindow::onMapClicked);
    connect(m_view, &MapView::cursorMoved, this, &MainWindow::onCursorMoved);
    connect(m_view, &MapView::cursorLeft, this, [this] { m_cursorLabel->clear(); });
    connect(m_view, &MapView::contextMenuRequested, this, &MainWindow::onMapContextMenu);
    connect(&m_watcher, &QFutureWatcher<LoadResult>::finished, this, &MainWindow::onLoadFinished);

    setLoading(false);
    updateWindowTitle();

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
    m_clearCacheAction = fileMenu->addAction(tr("Clear &Cache…"), this, &MainWindow::clearCache);
    m_clearCacheAction->setToolTip(tr("Delete the 3D world indexes this viewer keeps between sessions"));
    fileMenu->addSeparator();
    QAction* quitAction = fileMenu->addAction(tr("&Quit"), this, &QWidget::close);
    quitAction->setShortcut(QKeySequence::Quit);

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
    m_properties->setContextMenuPolicy(Qt::ActionsContextMenu);
    auto* propertiesDock = new QDockWidget(tr("Properties"), this);
    propertiesDock->setObjectName(QStringLiteral("propertiesDock"));
    propertiesDock->setWidget(m_properties);
    addDockWidget(Qt::RightDockWidgetArea, propertiesDock);
    splitDockWidget(m_objectsDock, propertiesDock, Qt::Vertical);

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
    m_viewMenu->addAction(propertiesDock->toggleViewAction());
    QAction* debugAction = m_debugDock->toggleViewAction();
    debugAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D));
    debugAction->setToolTip(tr("List the model and texture files the 3D world has loaded"));
    m_viewMenu->addAction(debugAction);
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
    m_model->setMap(nullptr);
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
    QHash<QString, QPixmap> icons;
    for (auto [key, image] : m_map->icons.asKeyValueRange()) {
        icons.insert(key,
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
            points->setIcons(icons);
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
    m_view->setLabelSources({m_layerItems.rbegin(), m_layerItems.rend()});
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
    if (m_selection.layer >= 0 && m_selection.layer != layer) {
        m_layerItems[static_cast<std::size_t>(m_selection.layer)]->setHighlightedFeature(-1);
    }
    m_selection = {layer, feature};
    item->setHighlightedFeature(feature);

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
    m_world3D->setHighlightedEntity(-1, -1);
    m_properties->setRowCount(0);
    if (!m_syncingSelection) {
        m_syncingSelection = true;
        m_table->clearSelection();
        m_syncingSelection = false;
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

    m_properties->setRowCount(static_cast<int>(rows.size()));
    for (int i = 0; i < rows.size(); ++i) {
        auto* value = new QTableWidgetItem(rows.at(i).second);
        value->setToolTip(rows.at(i).second);
        m_properties->setItem(i, 0, new QTableWidgetItem(rows.at(i).first));
        m_properties->setItem(i, 1, value);
    }
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
    m_clearSelectionAction->setEnabled(haveMap);
    m_warningsAction->setEnabled(haveMap && !m_map->warnings.isEmpty());
    m_view2DAction->setEnabled(haveMap);
    m_view3DAction->setEnabled(haveMap);
    if (!haveMap) {
        m_warningsButton->setVisible(false);
    }
}

void MainWindow::updateWindowTitle()
{
    setWindowTitle(m_trackName.isEmpty() ? tr("FH1 Map Viewer") : tr("%1 — FH1 Map Viewer").arg(m_trackName));
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
    const QPointer<MainWindow> guard(this);
    auto cancel = m_worldCancel;
    statusBar()->showMessage(tr("Opening the 3D world…"));
    m_busy->setVisible(true);
    m_worldWatcher.setFuture(QtConcurrent::run([archivePath, cachePath, pvsPath, cancel, guard]() -> WorldLoad {
        auto archive = std::make_shared<fh1::ForzaZip>();
        if (!archive->open(archivePath)) {
            return {nullptr, nullptr, nullptr, archive->errorString(), {}};
        }
        const QString signature = fh1::WorldIndex::archiveSignature(archivePath);
        std::optional<fh1::WorldIndex> index = fh1::WorldIndex::load(cachePath, signature);
        if (!index) {
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
                cancel.get());
            if (!index) {
                return {nullptr, nullptr, nullptr, QStringLiteral("cancelled"), {}};
            }
            QString error;
            if (!index->save(cachePath, signature, &error)) {
                std::fprintf(stderr, "Could not cache the world index: %s\n", qPrintable(error));
            }
        }
        std::shared_ptr<const fh1::TrackTextures> textures;
        QString textureError;
        if (pvsPath.isEmpty()) {
            textureError = QStringLiteral("the track has no PVS file");
        } else {
            QFile pvs(pvsPath);
            if (!pvs.open(QIODevice::ReadOnly)) {
                textureError = pvs.errorString();
            } else if (std::optional<fh1::TrackTextures> loaded
                = fh1::TrackTextures::load(pvs.readAll(), *archive, &textureError)) {
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
    QString message = tr("3D world: %1 meshes; %2 prop models are not placed yet")
                          .arg(result.index->chunks().size())
                          .arg(result.index->localModelCount());
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
    if (!m_script) {
        QSettings settings;
        settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
        settings.setValue(QStringLiteral("window/state"), saveState());
        settings.setValue(QStringLiteral("window/stateHasWorldDebug"), true);
    }
    QMainWindow::closeEvent(event);
}
