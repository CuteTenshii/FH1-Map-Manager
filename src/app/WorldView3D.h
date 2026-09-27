#pragma once

#include "EntityRenderer.h"
#include "ForzaZip.h"
#include "TrackTextures.h"
#include "WorldIndex.h"
#include "WorldPicking.h"
#include "WorldRenderer.h"
#include "WorldTiles.h"

#include <QElapsedTimer>
#include <QOpenGLWidget>
#include <QPainter>
#include <QSet>
#include <QThreadPool>
#include <QTimer>

#include <memory>
#include <optional>
#include <vector>

/// Fly-through 3D view of a track's world geometry.
///
/// Tiles are decoded and merged on worker threads (buildTileMesh) and handed
/// to a WorldRenderer, which uploads a few per frame and draws them. The
/// textures the tiles use are decoded on the same threads once the tiles are
/// in. Geometry without a known texture gets a plain ground colour.
class WorldView3D : public QOpenGLWidget {
    Q_OBJECT

public:
    using Camera = WorldCamera;

    explicit WorldView3D(QWidget* parent = nullptr);
    ~WorldView3D() override;

    /// Shows a world. `textures` may be null. Replaces any previous world.
    void setWorld(std::shared_ptr<const fh1::ForzaZip> archive, std::shared_ptr<const fh1::WorldIndex> index,
        std::shared_ptr<const fh1::TrackTextures> textures);
    void clearWorld();
    bool hasWorld() const { return m_index != nullptr; }

    Camera camera() const { return m_camera; }
    void setCamera(const Camera& camera);
    /// Places the camera `height` metres above the geometry at `x`, `z`,
    /// keeping its heading and pitch.
    void lookFromAbove(float x, float z, float height);

    /// Which event props (see fh1::Placement::eventProp), which the game
    /// only shows during races and other events, to draw. None by default.
    void setEventPropFilter(const fh1::EventPropFilter& filter);

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

    /// Draws the layers of `overlay` (a selected race's route) over the world
    /// and the map layers, or nothing when it is null. Its features are not
    /// picked.
    void setOverlay(std::shared_ptr<const fh1::MapData> overlay);
    /// Moves the camera behind and above `position`, looking along `facing`
    /// and down at it: the view from behind a car on a start grid.
    void lookAlong(const QVector3D& position, const QVector3D& facing);

    /// Outlines one model of the world (an index into its chunks); nothing
    /// clears the outline.
    void setHighlightedModel(std::optional<std::uint32_t> chunk);
    std::optional<std::uint32_t> highlightedModel() const { return m_highlightedModel; }
    /// Height of the ground (terrain and roads, not props) at `x`, `z`, read
    /// from the finest models of the loaded world, or nothing when no world
    /// is loaded or none lies there.
    std::optional<float> groundHeightAt(float x, float z) const;
    /// The model a click would pick at `position` (logical pixels): the one
    /// the view ray meets first among the loaded tiles' models.
    std::optional<fh1::PickHit> pickModelAt(const QPointF& position) const;

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
    /// Emitted when a click lands on no map feature but on a model of the
    /// world.
    void modelClicked(const fh1::PickHit& hit);
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
    void drawModelOutline(QPainter& painter);

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
    fh1::EventPropFilter m_eventPropFilter;
    std::optional<std::uint32_t> m_highlightedModel;

    Camera m_camera;
    float m_speed = 60.0F;
    QSet<int> m_keys;
    bool m_looking = false;
    QPoint m_lastMouse;
    QPoint m_pressPosition;
    EntityRenderer m_entities;
    EntityRenderer m_overlay;
    QTimer m_ticker;
    QElapsedTimer m_frameClock;
    bool m_wasSettled = false;
};
