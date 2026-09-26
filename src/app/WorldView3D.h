#pragma once

#include "EntityRenderer.h"
#include "ForzaZip.h"
#include "MapCalibration.h"
#include "TrackTextures.h"
#include "WorldIndex.h"
#include "WorldRenderer.h"
#include "WorldTiles.h"

#include <QElapsedTimer>
#include <QImage>
#include <QOpenGLWidget>
#include <QSet>
#include <QThreadPool>
#include <QTimer>

#include <memory>
#include <vector>

/// Fly-through 3D view of a track's world geometry.
///
/// Tiles are decoded and merged on worker threads (buildTileMesh) and handed
/// to a WorldRenderer, which uploads a few per frame and draws them. The
/// textures the tiles use are decoded on the same threads once the tiles are
/// in. Geometry without a texture is coloured from the satellite map image
/// through the 2D map's calibration.
class WorldView3D : public QOpenGLWidget {
    Q_OBJECT

public:
    using Camera = WorldCamera;

    explicit WorldView3D(QWidget* parent = nullptr);
    ~WorldView3D() override;

    /// Shows a world. `textures` and `satellite` may be null. Replaces any
    /// previous world.
    void setWorld(std::shared_ptr<const fh1::ForzaZip> archive, std::shared_ptr<const fh1::WorldIndex> index,
        std::shared_ptr<const fh1::TrackTextures> textures, const QImage& satellite,
        const fh1::MapCalibration& calibration);
    void clearWorld();
    bool hasWorld() const { return m_index != nullptr; }

    Camera camera() const { return m_camera; }
    void setCamera(const Camera& camera);
    /// Places the camera `height` metres above the geometry at `x`, `z`,
    /// keeping its heading and pitch.
    void lookFromAbove(float x, float z, float height);

    /// Far limit of drawing and of the fog, in metres.
    void setViewDistance(float metres);

    /// The map layers to draw in the world (see EntityRenderer), or nothing
    /// when `map` is null. `visible[layer][group]` gives each group's
    /// starting visibility.
    void setEntities(std::shared_ptr<const fh1::MapData> map, const std::vector<std::vector<bool>>& visible);
    void setEntityGroupVisible(int layer, int group, bool visible);
    /// Emphasises one feature; -1 clears the highlight.
    void setHighlightedEntity(int layer, int feature);
    /// Moves the camera to look at a feature from a short distance, keeping
    /// its heading.
    void focusOnEntity(int layer, int feature);

    /// True when every tile in range is loaded at the detail its distance
    /// calls for, with its textures, and no decoding is pending.
    bool isSettled() const;

    /// What the view has loaded, for the debug panel.
    std::vector<LoadedModel> loadedModels() const { return m_renderer.loadedModels(); }
    std::vector<LoadedTexture> loadedTextures() const { return m_renderer.loadedTextures(); }
    std::shared_ptr<const fh1::ForzaZip> archive() const { return m_archive; }
    std::shared_ptr<const fh1::WorldIndex> index() const { return m_index; }
    std::shared_ptr<const fh1::TrackTextures> textures() const { return m_textures; }

signals:
    /// Emitted when the view becomes settled (see isSettled()).
    void settled();
    /// Emitted when a click (not a drag) lands on a shown map feature, or on
    /// none.
    void entityClicked(int layer, int feature);
    void emptyClicked();
    /// Emitted when tiles or textures are loaded or freed, or the world
    /// changes.
    void loadedFilesChanged();

protected:
    void initializeGL() override;
    void paintGL() override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    struct BuiltTile {
        int tile = -1;
        int state = -1;
        std::shared_ptr<fh1::TileMesh> mesh;
    };
    struct DecodedTexture {
        std::uint32_t id = 0;
        /// Null when the texture could not be loaded; `error` says why.
        std::shared_ptr<fh1::TextureMipChain> chain;
        QString error;
    };

    void tick();
    void requestTiles();
    void requestTextures();
    /// Frees every OpenGL object while the widget's context still exists.
    void releaseGL();
    void drawOverlay(const WorldRenderer::Stats& stats);

    std::shared_ptr<const fh1::ForzaZip> m_archive;
    std::shared_ptr<const fh1::WorldIndex> m_index;
    std::shared_ptr<const fh1::TrackTextures> m_textures;
    std::unique_ptr<fh1::WorldTileGrid> m_grid;
    WorldRenderer m_renderer;
    /// Per tile: the state being built on a worker thread, or -1.
    std::vector<int> m_inFlight;
    std::vector<BuiltTile> m_ready;
    int m_jobsInFlight = 0;
    std::vector<DecodedTexture> m_readyTextures;
    int m_textureJobsInFlight = 0;
    /// Incremented on every world change; results of older jobs are dropped.
    quint64 m_epoch = 0;
    QThreadPool m_pool;
    int m_failedChunks = 0;

    Camera m_camera;
    float m_speed = 60.0F;
    QSet<int> m_keys;
    bool m_looking = false;
    QPoint m_lastMouse;
    QPoint m_pressPosition;
    EntityRenderer m_entities;
    QTimer m_ticker;
    QElapsedTimer m_frameClock;
    bool m_wasSettled = false;
};
