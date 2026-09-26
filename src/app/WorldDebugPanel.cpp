#include "WorldDebugPanel.h"

#include "ModelPreview.h"

#include <QAbstractTableModel>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableView>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kKeyRole = Qt::UserRole;
constexpr int kCheckerSize = 8;

QString formatName(fh1::TextureSurface::Format format)
{
    switch (format) {
    case fh1::TextureSurface::Format::Dxt1:
        return QStringLiteral("DXT1");
    case fh1::TextureSurface::Format::Dxt3:
        return QStringLiteral("DXT3");
    case fh1::TextureSurface::Format::Dxt5:
        return QStringLiteral("DXT5");
    case fh1::TextureSurface::Format::Rgba8:
        break;
    }
    return QStringLiteral("RGBA8");
}

QString originName(fh1::TextureMipChain::Origin origin)
{
    switch (origin) {
    case fh1::TextureMipChain::Origin::Bix:
        return QStringLiteral("BIX");
    case fh1::TextureMipChain::Origin::Caff:
        return QStringLiteral("CAFF");
    case fh1::TextureMipChain::Origin::Bundle:
        break;
    }
    return QStringLiteral("Bundle");
}

QString kibibytes(qint64 bytes)
{
    return QLocale().toString(static_cast<double>(bytes) / 1024.0, 'f', 1);
}

/// Copies the key column of the selected rows, one per line.
void addCopyAction(QTableView* view, int keyColumn)
{
    auto* copy = new QAction(QObject::tr("Copy File Name"), view);
    copy->setShortcut(QKeySequence::Copy);
    copy->setShortcutContext(Qt::WidgetShortcut);
    QObject::connect(copy, &QAction::triggered, view, [view, keyColumn] {
        QStringList names;
        const QModelIndexList rows = view->selectionModel()->selectedRows(keyColumn);
        for (const QModelIndex& row : rows) {
            names << row.data(Qt::DisplayRole).toString();
        }
        if (!names.isEmpty()) {
            QApplication::clipboard()->setText(names.join(QLatin1Char('\n')));
        }
    });
    view->addAction(copy);
    view->setContextMenuPolicy(Qt::ActionsContextMenu);
}

QTableView* makeTable(QAbstractItemModel* model)
{
    auto* view = new QTableView;
    view->setModel(model);
    view->setSortingEnabled(true);
    view->sortByColumn(0, Qt::AscendingOrder);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setWordWrap(false);
    view->verticalHeader()->hide();
    view->verticalHeader()->setDefaultSectionSize(view->fontMetrics().height() + 6);
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    view->horizontalHeader()->setStretchLastSection(true);
    // Room for a few rows even when the details below take most of the dock.
    view->setMinimumHeight(
        view->horizontalHeader()->sizeHint().height() + 4 * view->verticalHeader()->defaultSectionSize() + 16);
    return view;
}

QSortFilterProxyModel* makeProxy(QAbstractItemModel* source, QObject* parent)
{
    auto* proxy = new QSortFilterProxyModel(parent);
    proxy->setSourceModel(source);
    proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy->setFilterKeyColumn(-1);
    proxy->setSortRole(Qt::EditRole);
    proxy->setSortCaseSensitivity(Qt::CaseInsensitive);
    return proxy;
}

/// Selects the row whose key is `key`, if any. Returns whether it did.
bool selectKey(QTableView* view, quint64 key)
{
    QAbstractItemModel* model = view->model();
    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex index = model->index(row, 0);
        if (index.data(kKeyRole).toULongLong() == key) {
            view->selectionModel()->setCurrentIndex(
                index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            view->scrollTo(index);
            return true;
        }
    }
    return false;
}

} // namespace

class LoadedModelTable : public QAbstractTableModel {
public:
    enum Column { File, Lod, Tile, Triangles, Textures, Status, ColumnCount };

    struct Row {
        LoadedModel model;
        QString file;
        int lod = -1;
    };

    using QAbstractTableModel::QAbstractTableModel;

    void setRows(std::vector<Row> rows)
    {
        beginResetModel();
        m_rows = std::move(rows);
        endResetModel();
    }

    int rowCount(const QModelIndex& parent = {}) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
    }

    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : ColumnCount; }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return {};
        }
        switch (section) {
        case File:
            return tr("Model file");
        case Lod:
            return tr("LOD");
        case Tile:
            return tr("Tile");
        case Triangles:
            return tr("Triangles");
        case Textures:
            return tr("Textures");
        case Status:
            return tr("Status");
        default:
            return {};
        }
    }

    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid() || index.row() >= static_cast<int>(m_rows.size())) {
            return {};
        }
        const Row& row = m_rows[static_cast<std::size_t>(index.row())];
        if (role == kKeyRole) {
            return static_cast<qulonglong>(row.model.chunk);
        }
        if (role == Qt::ToolTipRole && index.column() == Status && !row.model.error.isEmpty()) {
            return row.model.error;
        }
        const bool sortValue = role == Qt::EditRole;
        if (role != Qt::DisplayRole && !sortValue) {
            return {};
        }
        switch (index.column()) {
        case File:
            return row.file;
        case Lod:
            if (sortValue) {
                return row.lod;
            }
            return row.lod < 0 ? tr("none") : QString::number(row.lod);
        case Tile:
            return row.model.tile;
        case Triangles:
            if (sortValue) {
                return row.model.triangles;
            }
            return QLocale().toString(row.model.triangles);
        case Textures:
            return static_cast<int>(row.model.textures.size());
        case Status:
            return row.model.error.isEmpty() ? tr("Loaded") : tr("Failed");
        default:
            return {};
        }
    }

    const Row* rowFor(std::uint32_t chunk) const
    {
        const auto it
            = std::find_if(m_rows.begin(), m_rows.end(), [chunk](const Row& r) { return r.model.chunk == chunk; });
        return it == m_rows.end() ? nullptr : &*it;
    }

private:
    std::vector<Row> m_rows;
};

class LoadedTextureTable : public QAbstractTableModel {
public:
    enum Column { Name, Size, Format, Source, Memory, Tiles, State, ColumnCount };

    using QAbstractTableModel::QAbstractTableModel;

    void setTextures(std::vector<LoadedTexture> textures)
    {
        beginResetModel();
        m_textures = std::move(textures);
        endResetModel();
    }

    int rowCount(const QModelIndex& parent = {}) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(m_textures.size());
    }

    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : ColumnCount; }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return {};
        }
        switch (section) {
        case Name:
            return tr("Texture");
        case Size:
            return tr("Size");
        case Format:
            return tr("Format");
        case Source:
            return tr("Source");
        case Memory:
            return tr("KiB");
        case Tiles:
            return tr("Tiles");
        case State:
            return tr("Status");
        default:
            return {};
        }
    }

    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid() || index.row() >= static_cast<int>(m_textures.size())) {
            return {};
        }
        const LoadedTexture& t = m_textures[static_cast<std::size_t>(index.row())];
        if (role == kKeyRole) {
            return static_cast<qulonglong>(t.id);
        }
        if (role == Qt::ToolTipRole) {
            return t.state == LoadedTexture::State::Failed ? t.error : t.files;
        }
        const bool sortValue = role == Qt::EditRole;
        if (role != Qt::DisplayRole && !sortValue) {
            return {};
        }
        const bool loaded = t.state == LoadedTexture::State::Loaded;
        switch (index.column()) {
        case Name:
            return fh1::TrackTextures::textureName(t.id);
        case Size:
            if (sortValue) {
                return t.width * t.height;
            }
            return loaded ? tr("%1 × %2").arg(t.width).arg(t.height) : QString();
        case Format:
            return loaded ? formatName(t.format) : QString();
        case Source:
            return loaded ? originName(t.origin) : QString();
        case Memory:
            if (sortValue) {
                return t.bytes;
            }
            return loaded ? kibibytes(t.bytes) : QString();
        case Tiles:
            return t.tiles;
        case State:
            switch (t.state) {
            case LoadedTexture::State::Loading:
                return tr("Loading");
            case LoadedTexture::State::Loaded:
                return tr("Loaded");
            case LoadedTexture::State::Failed:
                return tr("Missing");
            }
            return {};
        default:
            return {};
        }
    }

    const LoadedTexture* texture(std::uint32_t id) const
    {
        const auto it
            = std::find_if(m_textures.begin(), m_textures.end(), [id](const LoadedTexture& t) { return t.id == id; });
        return it == m_textures.end() ? nullptr : &*it;
    }

private:
    std::vector<LoadedTexture> m_textures;
};

/// Draws one image over a checkerboard, so transparent texels show as such,
/// scaled without smoothing so single texels stay visible.
class TexturePreview : public QWidget {
public:
    using QWidget::QWidget;

    void setImage(const QImage& image, const QString& placeholder = {})
    {
        m_image = image;
        m_placeholder = placeholder;
        updateGeometry();
        update();
    }

    const QImage& image() const { return m_image; }

    /// 0 fits the image to the widget; otherwise screen pixels per texel.
    void setZoom(int zoom)
    {
        m_zoom = zoom;
        updateGeometry();
        update();
    }

    QSize sizeHint() const override
    {
        if (m_image.isNull() || m_zoom == 0) {
            return {256, 256};
        }
        return m_image.size() * m_zoom;
    }

    QSize minimumSizeHint() const override { return m_zoom == 0 || m_image.isNull() ? QSize(64, 64) : sizeHint(); }

protected:
    void paintEvent(QPaintEvent* /*event*/) override
    {
        QPainter painter(this);
        if (m_image.isNull()) {
            painter.setPen(palette().color(QPalette::PlaceholderText));
            painter.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, m_placeholder);
            return;
        }
        double scale = m_zoom;
        if (m_zoom == 0) {
            scale = std::min(
                static_cast<double>(width()) / m_image.width(), static_cast<double>(height()) / m_image.height());
            // Whole texels when enlarging, so every texel is the same size.
            if (scale >= 1.0) {
                scale = std::floor(scale);
            }
        }
        const QSizeF size(m_image.width() * scale, m_image.height() * scale);
        const QRectF target(QPointF((width() - size.width()) / 2.0, (height() - size.height()) / 2.0), size);
        // The checkerboard uses the palette's two base colours so it follows
        // the light or dark theme.
        painter.setClipRect(target);
        const QColor light = palette().color(QPalette::Base);
        const QColor dark = palette().color(QPalette::AlternateBase);
        for (int y = 0; y * kCheckerSize < static_cast<int>(target.height()) + kCheckerSize; ++y) {
            for (int x = 0; x * kCheckerSize < static_cast<int>(target.width()) + kCheckerSize; ++x) {
                painter.fillRect(QRectF(target.left() + x * kCheckerSize, target.top() + y * kCheckerSize, kCheckerSize,
                                     kCheckerSize),
                    (x + y) % 2 == 0 ? light : dark);
            }
        }
        painter.setClipping(false);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.drawImage(target, m_image);
        painter.setPen(palette().color(QPalette::Mid));
        painter.drawRect(target.adjusted(-0.5, -0.5, 0.5, 0.5));
    }

private:
    QImage m_image;
    QString m_placeholder;
    int m_zoom = 0;
};

WorldDebugPanel::WorldDebugPanel(QWidget* parent)
    : QWidget(parent)
{
    m_summary = new QLabel;
    m_summary->setWordWrap(true);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_tabs = new QTabWidget;
    auto* modelsPage = new QWidget;
    auto* texturesPage = new QWidget;
    buildModelsTab(modelsPage);
    buildTexturesTab(texturesPage);
    m_tabs->addTab(modelsPage, tr("Models"));
    m_tabs->addTab(texturesPage, tr("Textures"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(m_summary);
    layout->addWidget(m_tabs, 1);

    setWorld(nullptr, nullptr, nullptr);
}

WorldDebugPanel::~WorldDebugPanel() = default;

void WorldDebugPanel::buildModelsTab(QWidget* page)
{
    m_modelTable = new LoadedModelTable(this);
    m_modelProxy = makeProxy(m_modelTable, this);
    auto* filter = new QLineEdit;
    filter->setPlaceholderText(tr("Filter by file name"));
    filter->setClearButtonEnabled(true);
    connect(filter, &QLineEdit::textChanged, m_modelProxy, &QSortFilterProxyModel::setFilterFixedString);

    m_modelView = makeTable(m_modelProxy);
    addCopyAction(m_modelView, LoadedModelTable::File);
    connect(
        m_modelView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &WorldDebugPanel::onModelSelected);

    m_modelDetails = new QLabel;
    m_modelDetails->setWordWrap(true);
    m_modelDetails->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_modelPreview = new ModelPreview;
    m_modelPreview->setMinimumHeight(160);
    m_modelTextures = new QListWidget;
    m_modelTextures->setToolTip(tr("Double-click or press Enter to preview a texture"));
    m_modelTextures->setMaximumHeight(m_modelTextures->fontMetrics().height() * 4 + 8);
    connect(m_modelTextures, &QListWidget::itemActivated, this,
        [this](QListWidgetItem* item) { showTexture(item->data(kKeyRole).toUInt()); });

    auto* details = new QWidget;
    auto* detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    detailsLayout->addWidget(m_modelPreview, 1);
    detailsLayout->addWidget(m_modelDetails);
    detailsLayout->addWidget(m_modelTextures);

    auto* splitter = new QSplitter(Qt::Vertical);
    splitter->addWidget(m_modelView);
    splitter->addWidget(details);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->addWidget(filter);
    layout->addWidget(splitter, 1);
}

void WorldDebugPanel::buildTexturesTab(QWidget* page)
{
    m_textureTable = new LoadedTextureTable(this);
    m_textureProxy = makeProxy(m_textureTable, this);
    auto* filter = new QLineEdit;
    filter->setPlaceholderText(tr("Filter by texture, format, source or status"));
    filter->setClearButtonEnabled(true);
    connect(filter, &QLineEdit::textChanged, m_textureProxy, &QSortFilterProxyModel::setFilterFixedString);

    m_textureView = makeTable(m_textureProxy);
    addCopyAction(m_textureView, LoadedTextureTable::Name);
    connect(m_textureView->selectionModel(), &QItemSelectionModel::selectionChanged, this,
        &WorldDebugPanel::onTextureSelected);

    m_preview = new TexturePreview;
    auto* scroll = new QScrollArea;
    scroll->setWidget(m_preview);
    scroll->setWidgetResizable(true);
    scroll->setAlignment(Qt::AlignCenter);
    scroll->setMinimumHeight(140);

    m_levelCombo = new QComboBox;
    m_levelCombo->setToolTip(tr("Mip level to show"));
    connect(m_levelCombo, &QComboBox::currentIndexChanged, this, &WorldDebugPanel::updatePreviewImage);
    m_channelsCombo = new QComboBox;
    m_channelsCombo->addItem(tr("Colour and alpha"), static_cast<int>(Channels::ColourAndAlpha));
    m_channelsCombo->addItem(tr("Colour only"), static_cast<int>(Channels::Colour));
    m_channelsCombo->addItem(tr("Alpha only"), static_cast<int>(Channels::Alpha));
    connect(m_channelsCombo, &QComboBox::currentIndexChanged, this, &WorldDebugPanel::updatePreviewImage);
    m_zoomCombo = new QComboBox;
    m_zoomCombo->addItem(tr("Fit"), 0);
    for (int zoom : {1, 2, 4, 8}) {
        m_zoomCombo->addItem(tr("%1%").arg(zoom * 100), zoom);
    }
    m_zoomCombo->setToolTip(tr("Zoom"));
    connect(m_zoomCombo, &QComboBox::currentIndexChanged, this, [this, scroll] {
        const int zoom = m_zoomCombo->currentData().toInt();
        // A fitted preview follows the panel's size; a zoomed one scrolls.
        scroll->setWidgetResizable(zoom == 0);
        m_preview->setZoom(zoom);
        if (zoom != 0) {
            m_preview->resize(m_preview->sizeHint());
        }
    });
    m_saveButton = new QPushButton(tr("Save PNG…"));
    m_saveButton->setToolTip(tr("Save the level as shown, with the chosen channels"));
    connect(m_saveButton, &QPushButton::clicked, this, &WorldDebugPanel::savePreview);

    // Two rows, so the controls fit a dock of ordinary width.
    auto* controls = new QGridLayout;
    controls->setContentsMargins(0, 0, 0, 0);
    controls->addWidget(m_levelCombo, 0, 0);
    controls->addWidget(m_channelsCombo, 0, 1);
    controls->addWidget(m_zoomCombo, 1, 0);
    controls->addWidget(m_saveButton, 1, 1);
    controls->setColumnStretch(0, 1);
    controls->setColumnStretch(1, 1);

    m_textureDetails = new QLabel;
    m_textureDetails->setWordWrap(true);
    m_textureDetails->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_textureUsers = new QListWidget;
    m_textureUsers->setToolTip(tr("Model files that use this texture; double-click or press Enter to select one"));
    m_textureUsers->setMaximumHeight(m_textureUsers->fontMetrics().height() * 4 + 8);
    connect(m_textureUsers, &QListWidget::itemActivated, this,
        [this](QListWidgetItem* item) { showModel(item->data(kKeyRole).toUInt()); });

    auto* details = new QWidget;
    auto* detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    detailsLayout->addLayout(controls);
    detailsLayout->addWidget(scroll, 1);
    detailsLayout->addWidget(m_textureDetails);
    detailsLayout->addWidget(m_textureUsers);

    auto* splitter = new QSplitter(Qt::Vertical);
    splitter->addWidget(m_textureView);
    splitter->addWidget(details);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->addWidget(filter);
    layout->addWidget(splitter, 1);
}

void WorldDebugPanel::setWorld(std::shared_ptr<const fh1::ForzaZip> archive,
    std::shared_ptr<const fh1::WorldIndex> index, std::shared_ptr<const fh1::TrackTextures> textures)
{
    m_archive = std::move(archive);
    m_index = std::move(index);
    m_textures = std::move(textures);
    ++m_previewRequest;
    m_previewId.reset();
    m_previewChain.reset();
    setLoadedFiles({}, {});
}

QString WorldDebugPanel::modelFileName(std::uint32_t chunk) const
{
    if (!m_index || !m_archive || chunk >= m_index->chunks().size()) {
        return tr("chunk %1").arg(chunk);
    }
    const std::uint32_t entry = m_index->chunks()[chunk].entry;
    return entry < m_archive->entries().size() ? m_archive->entries()[entry].name : tr("chunk %1").arg(chunk);
}

void WorldDebugPanel::setLoadedFiles(std::vector<LoadedModel> models, std::vector<LoadedTexture> textures)
{
    const std::optional<std::uint32_t> chunk = selectedChunk();
    const std::optional<std::uint32_t> texture = selectedTexture();

    m_models = std::move(models);
    m_loadedTextures = std::move(textures);
    std::vector<LoadedModelTable::Row> rows;
    rows.reserve(m_models.size());
    for (const LoadedModel& model : m_models) {
        LoadedModelTable::Row row;
        row.model = model;
        row.file = modelFileName(model.chunk);
        if (m_index && model.chunk < m_index->chunks().size()) {
            // The level is a small signed number, -1 for models without levels.
            row.lod = static_cast<int>(m_index->chunks()[model.chunk].lod); // NOLINT(bugprone-signed-char-misuse)
        }
        rows.push_back(std::move(row));
    }
    m_modelTable->setRows(std::move(rows));
    m_textureTable->setTextures(m_loadedTextures);

    // Keep what was selected, if it is still loaded; the preview stays up
    // either way until another texture is chosen.
    if (chunk) {
        selectKey(m_modelView, *chunk);
    }
    if (texture) {
        selectKey(m_textureView, *texture);
    }
    onModelSelected();
    updateSummary();
    m_tabs->setTabText(0, tr("Models (%1)").arg(m_models.size()));
    m_tabs->setTabText(1, tr("Textures (%1)").arg(m_loadedTextures.size()));
    if (!m_previewId) {
        m_preview->setImage({}, m_archive ? tr("Select a texture to preview it.") : QString());
        m_textureDetails->clear();
        m_textureUsers->clear();
        m_levelCombo->clear();
    }
    m_saveButton->setEnabled(m_previewChain != nullptr);
}

void WorldDebugPanel::updateSummary()
{
    if (!m_archive) {
        m_summary->setText(tr("Nothing loaded. Open the 3D world (View → 3D World) to list the files it uses."));
        return;
    }
    QSet<int> tiles;
    int failedModels = 0;
    for (const LoadedModel& model : m_models) {
        tiles.insert(model.tile);
        failedModels += model.error.isEmpty() ? 0 : 1;
    }
    int loading = 0;
    int missing = 0;
    qint64 bytes = 0;
    for (const LoadedTexture& texture : m_loadedTextures) {
        loading += texture.state == LoadedTexture::State::Loading ? 1 : 0;
        missing += texture.state == LoadedTexture::State::Failed ? 1 : 0;
        bytes += texture.bytes;
    }
    QStringList lines;
    lines << tr("%1 model files in %2 tiles, from %3.")
                 .arg(m_models.size())
                 .arg(tiles.size())
                 .arg(QFileInfo(m_archive->path()).fileName());
    lines << tr("%1 textures, %2 MiB of video memory.")
                 .arg(m_loadedTextures.size())
                 .arg(QLocale().toString(static_cast<double>(bytes) / (1024.0 * 1024.0), 'f', 1));
    if (failedModels > 0) {
        lines << tr("%n model file(s) could not be read.", nullptr, failedModels);
    }
    if (loading > 0) {
        lines << tr("%n texture(s) still loading.", nullptr, loading);
    }
    if (missing > 0) {
        lines << tr("%n texture(s) missing from the archive.", nullptr, missing);
    }
    if (!m_textures) {
        lines << tr("The track's texture tables could not be read, so nothing is textured.");
    }
    m_summary->setText(lines.join(QLatin1Char('\n')));
}

std::optional<std::uint32_t> WorldDebugPanel::selectedChunk() const
{
    const QModelIndexList rows = m_modelView->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        return std::nullopt;
    }
    return rows.front().data(kKeyRole).toUInt();
}

std::optional<std::uint32_t> WorldDebugPanel::selectedTexture() const
{
    const QModelIndexList rows = m_textureView->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        return std::nullopt;
    }
    return rows.front().data(kKeyRole).toUInt();
}

void WorldDebugPanel::onModelSelected()
{
    m_modelTextures->clear();
    const std::optional<std::uint32_t> chunk = selectedChunk();
    const LoadedModelTable::Row* row = chunk ? m_modelTable->rowFor(*chunk) : nullptr;
    if (row == nullptr) {
        m_modelDetails->setText(m_models.empty() ? QString() : tr("Select a model file to see it and its textures."));
        m_modelPreview->clear(m_models.empty() ? QString() : tr("Select a model file to see it here."));
        return;
    }
    if (!row->model.error.isEmpty()) {
        m_modelDetails->setText(tr("%1 could not be read: %2").arg(row->file, row->model.error));
        m_modelPreview->clear(tr("%1 could not be read.").arg(row->file));
        return;
    }
    // Refreshes of the lists call this too; the preview keeps its camera
    // while the same model stays selected.
    m_modelPreview->setModel(m_archive, m_index, m_textures, row->model.chunk);
    m_modelDetails->setText(row->model.textures.empty()
            ? tr("%1 has no textures; it is drawn in a plain ground colour.").arg(row->file)
            : tr("Textures of %1:").arg(row->file));
    for (std::uint32_t id : row->model.textures) {
        const LoadedTexture* texture = m_textureTable->texture(id);
        QString text = fh1::TrackTextures::textureName(id);
        if (texture != nullptr && texture->state == LoadedTexture::State::Loaded) {
            text += tr("   %1 × %2 %3, %4")
                        .arg(texture->width)
                        .arg(texture->height)
                        .arg(formatName(texture->format), originName(texture->origin));
        } else if (texture != nullptr && texture->state == LoadedTexture::State::Failed) {
            text += tr("   missing");
        }
        auto* item = new QListWidgetItem(text, m_modelTextures);
        item->setData(kKeyRole, id);
    }
}

bool WorldDebugPanel::showTexture(std::uint32_t id)
{
    m_tabs->setCurrentIndex(1);
    if (selectKey(m_textureView, id)) {
        return true;
    }
    // Hidden by the filter, perhaps; the preview works without a table row.
    requestPreview(id);
    return false;
}

bool WorldDebugPanel::showModel(std::uint32_t chunk)
{
    m_tabs->setCurrentIndex(0);
    return selectKey(m_modelView, chunk);
}

void WorldDebugPanel::onTextureSelected()
{
    if (const std::optional<std::uint32_t> id = selectedTexture(); id && id != m_previewId) {
        requestPreview(*id);
    }
}

void WorldDebugPanel::requestPreview(std::uint32_t id)
{
    m_previewId = id;
    m_previewChain.reset();
    m_levelCombo->clear();
    m_saveButton->setEnabled(false);
    m_textureUsers->clear();
    for (const LoadedModel& model : m_models) {
        if (std::find(model.textures.begin(), model.textures.end(), id) != model.textures.end()) {
            auto* item = new QListWidgetItem(modelFileName(model.chunk), m_textureUsers);
            item->setData(kKeyRole, model.chunk);
        }
    }
    const QString name = fh1::TrackTextures::textureName(id);
    if (!m_archive || !m_textures) {
        m_preview->setImage({}, tr("%1 cannot be previewed without the track's texture tables.").arg(name));
        emit previewReady();
        return;
    }
    m_preview->setImage({}, tr("Decoding %1…").arg(name));
    const quint64 request = ++m_previewRequest;
    struct Result {
        std::shared_ptr<const fh1::TextureMipChain> chain;
        QString error;
    };
    auto archive = m_archive;
    auto textures = m_textures;
    auto* watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, request, name] {
        watcher->deleteLater();
        if (request != m_previewRequest) {
            return;
        }
        const Result result = watcher->result();
        m_previewChain = result.chain;
        const int users = m_textureUsers->count();
        if (!m_previewChain) {
            m_preview->setImage({}, tr("%1 could not be decoded.").arg(name));
            m_textureDetails->setText(
                tr("%1: %2\nUsed by %n loaded model file(s).", nullptr, users).arg(name, result.error));
            emit previewReady();
            return;
        }
        const fh1::TextureMipChain& chain = *m_previewChain;
        m_textureDetails->setText(
            tr("%1: %2 × %3 %4, %5 mip levels, from %6.\nUsed by %n loaded model file(s).", nullptr, users)
                .arg(name)
                .arg(chain.width)
                .arg(chain.height)
                .arg(formatName(chain.format))
                .arg(chain.levels.size())
                .arg(chain.files));
        const QSignalBlocker blocker(m_levelCombo);
        m_levelCombo->clear();
        for (int level = 0; level < static_cast<int>(chain.levels.size()); ++level) {
            m_levelCombo->addItem(
                tr("Level %1 (%2 × %3)").arg(level).arg(chain.levelWidth(level)).arg(chain.levelHeight(level)), level);
        }
        m_saveButton->setEnabled(true);
        updatePreviewImage();
        emit previewReady();
    });
    watcher->setFuture(QtConcurrent::run([archive, textures, id] {
        Result result;
        std::optional<fh1::TextureMipChain> chain = textures->loadTexture(*archive, id, &result.error);
        if (chain) {
            result.chain = std::make_shared<const fh1::TextureMipChain>(std::move(*chain));
        }
        return result;
    }));
}

void WorldDebugPanel::updatePreviewImage()
{
    if (!m_previewChain) {
        return;
    }
    const int level = std::max(0, m_levelCombo->currentIndex());
    QImage image = fh1::surfaceToImage(m_previewChain->level(level)).convertToFormat(QImage::Format_ARGB32);
    switch (static_cast<Channels>(m_channelsCombo->currentData().toInt())) {
    case Channels::ColourAndAlpha:
        break;
    case Channels::Colour:
        image = image.convertToFormat(QImage::Format_RGB32);
        break;
    case Channels::Alpha:
        for (int y = 0; y < image.height(); ++y) {
            auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const int a = qAlpha(line[x]);
                line[x] = qRgb(a, a, a);
            }
        }
        break;
    }
    m_preview->setImage(image);
    if (m_zoomCombo->currentData().toInt() != 0) {
        m_preview->resize(m_preview->sizeHint());
    }
}

QImage WorldDebugPanel::previewImage() const
{
    return m_preview->image();
}

void WorldDebugPanel::setPreviewLevel(int level)
{
    m_levelCombo->setCurrentIndex(level);
}

void WorldDebugPanel::setPreviewChannels(Channels channels)
{
    m_channelsCombo->setCurrentIndex(m_channelsCombo->findData(static_cast<int>(channels)));
}

void WorldDebugPanel::savePreview()
{
    if (!m_previewId || m_preview->image().isNull()) {
        return;
    }
    const QString suggested = QStringLiteral("%1/%2_level%3.png")
                                  .arg(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
                                      fh1::TrackTextures::textureName(*m_previewId))
                                  .arg(std::max(0, m_levelCombo->currentIndex()));
    const QString path
        = QFileDialog::getSaveFileName(this, tr("Save Texture as PNG"), suggested, tr("PNG images (*.png)"));
    if (path.isEmpty()) {
        return;
    }
    if (!m_preview->image().save(path, "PNG")) {
        QMessageBox::warning(this, tr("Save Texture as PNG"), tr("Could not write %1.").arg(path));
    }
}
