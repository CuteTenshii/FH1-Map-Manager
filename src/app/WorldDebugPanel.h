#pragma once

#include "ForzaZip.h"
#include "LoadedFiles.h"
#include "TrackTextures.h"
#include "WorldIndex.h"

#include <QImage>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSortFilterProxyModel;
class QTabWidget;
class QTableView;
class LoadedModelTable;
class LoadedTextureTable;
class TexturePreview;

/// Lists the files the 3D view has loaded: model files per tile and the
/// textures they use, with a preview of any single texture.
///
/// The panel only shows data it is given (setLoadedFiles()), so it works
/// without OpenGL. Previews are decoded again from the archive on a worker
/// thread; the renderer keeps no copy of texture data in memory.
class WorldDebugPanel : public QWidget {
    Q_OBJECT

public:
    enum class Channels { ColourAndAlpha, Colour, Alpha };

    explicit WorldDebugPanel(QWidget* parent = nullptr);
    ~WorldDebugPanel() override;

    /// The world the listed files belong to; null pointers clear the panel.
    void setWorld(std::shared_ptr<const fh1::ForzaZip> archive, std::shared_ptr<const fh1::WorldIndex> index,
        std::shared_ptr<const fh1::TrackTextures> textures);
    /// Replaces the lists, keeping the selection where the item still exists.
    void setLoadedFiles(std::vector<LoadedModel> models, std::vector<LoadedTexture> textures);

    /// Switches to the Textures tab and selects texture `id`, if listed.
    bool showTexture(std::uint32_t id);
    /// Switches to the Models tab and selects the model of chunk `chunk`.
    bool showModel(std::uint32_t chunk);

    /// The texture being previewed and the image shown for it (after the
    /// level and channel choices); null while none is loaded.
    std::optional<std::uint32_t> previewedTexture() const { return m_previewId; }
    QImage previewImage() const;
    void setPreviewLevel(int level);
    void setPreviewChannels(Channels channels);

signals:
    /// Emitted when a texture preview has been decoded, or failed to be.
    void previewReady();

private:
    void buildModelsTab(QWidget* page);
    void buildTexturesTab(QWidget* page);
    void updateSummary();
    void onModelSelected();
    void onTextureSelected();
    void requestPreview(std::uint32_t id);
    void updatePreviewImage();
    void savePreview();
    std::optional<std::uint32_t> selectedTexture() const;
    std::optional<std::uint32_t> selectedChunk() const;
    QString modelFileName(std::uint32_t chunk) const;

    std::shared_ptr<const fh1::ForzaZip> m_archive;
    std::shared_ptr<const fh1::WorldIndex> m_index;
    std::shared_ptr<const fh1::TrackTextures> m_textures;
    std::vector<LoadedModel> m_models;
    std::vector<LoadedTexture> m_loadedTextures;

    QLabel* m_summary = nullptr;
    QTabWidget* m_tabs = nullptr;

    LoadedModelTable* m_modelTable = nullptr;
    QSortFilterProxyModel* m_modelProxy = nullptr;
    QTableView* m_modelView = nullptr;
    QLabel* m_modelDetails = nullptr;
    QListWidget* m_modelTextures = nullptr;

    LoadedTextureTable* m_textureTable = nullptr;
    QSortFilterProxyModel* m_textureProxy = nullptr;
    QTableView* m_textureView = nullptr;
    TexturePreview* m_preview = nullptr;
    QComboBox* m_levelCombo = nullptr;
    QComboBox* m_channelsCombo = nullptr;
    QComboBox* m_zoomCombo = nullptr;
    QPushButton* m_saveButton = nullptr;
    QLabel* m_textureDetails = nullptr;
    QListWidget* m_textureUsers = nullptr;

    /// The decoded texture being previewed.
    std::optional<std::uint32_t> m_previewId;
    std::shared_ptr<const fh1::TextureMipChain> m_previewChain;
    /// Incremented per preview request; results of older requests are dropped.
    quint64 m_previewRequest = 0;
};
