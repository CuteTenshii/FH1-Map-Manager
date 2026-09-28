#pragma once

#include "LoadedFiles.h"
#include "WorldTiles.h"

#include <QMatrix4x4>
#include <QOpenGLFunctions_3_3_Core>
#include <QSize>
#include <QVector3D>

#include <array>
#include <list>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

class QOpenGLShaderProgram;

/// Camera for the 3D world, in world coordinates (X east, Y up, Z north).
struct WorldCamera {
    QVector3D position;
    /// Radians; 0 looks east (+X), increasing turns toward north (+Z).
    float yaw = 0.0F;
    /// Radians; negative looks down.
    float pitch = -0.35F;

    QVector3D forward() const;
};

/// A tile that should be (re)built for the current camera.
struct TileRequest {
    int tile = -1;
    int state = -1;
    float distance = 0.0F;
    std::vector<std::uint32_t> chunks;
};

/// OpenGL side of the 3D world: tile buffers, the game's textures, shaders
/// and drawing. Every method that touches OpenGL needs the context that
/// initialize() ran in to be current. Used by the interactive view and by
/// offscreen rendering.
class WorldRenderer : protected QOpenGLFunctions_3_3_Core {
public:
    struct Stats {
        int drawnTiles = 0;
        qint64 drawnTriangles = 0;
        int drawCalls = 0;
    };

    struct TextureStats {
        int loaded = 0;
        int pending = 0;
        int failed = 0;
        qint64 bytes = 0;
        /// Loaded textures no tile uses any more, kept for reuse.
        int cached = 0;
        qint64 cachedBytes = 0;
    };

    WorldRenderer();
    /// Call release() first, with the context current: OpenGL objects cannot
    /// be freed without it.
    ~WorldRenderer() override;
    WorldRenderer(const WorldRenderer&) = delete;
    WorldRenderer& operator=(const WorldRenderer&) = delete;

    /// Prepares OpenGL objects in the current context. Returns false, with
    /// errorString() set, when the context lacks OpenGL 3.3 core functions or
    /// the shaders do not compile; the renderer then draws nothing.
    bool initialize();
    bool isReady() const { return m_initialized; }
    QString errorString() const { return m_error; }
    /// Frees every OpenGL object; the current context must be the one
    /// initialize() ran in. Tile contents are forgotten and must be rebuilt.
    void release();

    /// Uses `grid` (which must outlive the renderer or the next setGrid) and
    /// forgets previous tiles.
    void setGrid(const fh1::WorldTileGrid* grid);
    void setViewDistance(float metres) { m_viewDistance = metres; }
    /// Nearest distance drawn, in metres; smaller values let the camera come
    /// closer to small objects at the cost of depth precision far away.
    void setNearPlane(float metres) { m_nearPlane = metres; }
    float viewDistance() const { return m_viewDistance; }
    /// How high `camera` is above the world under it, estimated from the
    /// tile bounds there (0 at or below them).
    float clearance(const WorldCamera& camera) const;
    /// How far `camera` sees: the view distance, lengthened by its
    /// clearance so that a view from high up still reaches the ground.
    float reach(const WorldCamera& camera) const;

    /// Tiles whose loaded state differs from what `camera` needs, nearest
    /// first. Tiles with a build running (inFlightState[tile] >= 0) are left
    /// out.
    std::vector<TileRequest> requests(const WorldCamera& camera, const std::vector<int>& inFlightState) const;
    /// Replaces a tile's buffers. Only OpenGL work; the mesh is built elsewhere.
    /// Textures the mesh uses that are not loaded yet are queued, see
    /// takeTextureRequests(). A tile already drawn keeps being drawn as it
    /// was until those textures are loaded or have failed, so a change of
    /// detail never shows untextured geometry.
    void upload(int tile, int state, const fh1::TileMesh& mesh);

    /// Texture ids that loaded tiles use and nobody has been asked to load
    /// yet. Each id is returned once; answer with uploadTexture() or
    /// failTexture(). Until then its geometry is drawn untextured.
    std::vector<std::uint32_t> takeTextureRequests();
    /// Stores a decoded texture, unless no loaded tile uses it any more.
    void uploadTexture(std::uint32_t id, const fh1::TextureMipChain& chain);
    /// Records that a texture could not be loaded, and why; its geometry
    /// stays untextured.
    void failTexture(std::uint32_t id, const QString& error = {});
    TextureStats textureStats() const;
    /// True while a texture used by a loaded tile is waiting to be loaded.
    bool texturesPending() const;
    /// Frees tiles far outside the view distance. Returns whether any were
    /// freed.
    bool evictDistant(const WorldCamera& camera);

    /// The model files in loaded tiles, including those that failed.
    std::vector<LoadedModel> loadedModels() const;
    /// The textures loaded tiles use, whatever their state.
    std::vector<LoadedTexture> loadedTextures() const;
    /// True when every tile within range holds the state the camera needs.
    bool isComplete(const WorldCamera& camera) const;

    Stats draw(const WorldCamera& camera, QSize viewport);

    /// Maps world coordinates (Z north) straight to clip space for `camera`,
    /// for anything drawn over the world or projected onto the screen.
    QMatrix4x4 worldViewProjection(const WorldCamera& camera, QSize viewport) const;
    /// draw() puts everything but the backdrop terrain into depth values
    /// [0, kForegroundDepthFar], and the backdrop behind it; anything drawn
    /// over the world and depth-tested against it uses the same front part.
    static constexpr double kForegroundDepthFar = 0.5;
    /// Vertical field of view of draw().
    static constexpr float kFieldOfViewDegrees = 60.0F;

    /// Colour and distance scale of the fog draw() applies.
    static QVector3D fogColour();
    float fogDistance(const WorldCamera& camera) const { return reach(camera) * kFogFraction; }
    qint64 uploadedTriangles() const { return m_uploadedTriangles; }
    int tileCount() const { return static_cast<int>(m_tiles.size()); }
    /// The state of `tile` that is drawn, or -1.
    int loadedState(int tile) const { return m_tiles[static_cast<std::size_t>(tile)].state; }
    /// Marks `tile` for building again, after the grid changed its chunks.
    /// Its current mesh stays drawn until the new one is uploaded.
    void invalidateTile(int tile);

private:
    /// Fog distance as a fraction of the reach.
    static constexpr float kFogFraction = 0.75F;
    /// Distance from `camera` to `tile` that decides its detail and
    /// whether it is loaded: along the ground, and up to the camera.
    float tileDistance(const fh1::WorldTileGrid::Tile& tile, const WorldCamera& camera) const;

    struct GpuTile {
        GLuint vao = 0;
        GLuint vbo = 0;
        GLuint ebo = 0;
        GLsizei indexCount = 0;
        std::vector<fh1::TileMesh::Batch> batches;
        std::vector<fh1::TileMesh::Model> models;
        int state = -1;
        /// The tile's chunks changed since it was built; it is drawn as it
        /// was until built again.
        bool stale = false;
    };

    struct GpuTexture {
        enum class State { Queued, Requested, Ready, Failed };
        GLuint name = 0;
        /// Loaded tiles whose batches use the texture, drawn or waiting.
        int users = 0;
        /// Set while no tile uses the loaded texture: its place in
        /// m_unusedTextures.
        std::optional<std::list<std::uint32_t>::iterator> unused;
        State state = State::Queued;
        qint64 bytes = 0;
        /// What was uploaded, for loadedTextures().
        int width = 0;
        int height = 0;
        int levels = 0;
        fh1::TextureSurface::Format format = fh1::TextureSurface::Format::Rgba8;
        fh1::TextureMipChain::Origin origin = fh1::TextureMipChain::Origin::Bix;
        QString files;
        QString error;
    };

    void releaseTile(GpuTile& tile);
    /// Fills `gpu`, which must be empty, with `mesh`'s buffers.
    void fillTile(GpuTile& gpu, int state, const fh1::TileMesh& mesh);
    /// True when every texture `tile` uses is loaded or has failed.
    bool texturesSettled(const GpuTile& tile) const;
    /// Draws the waiting state of `tile` instead of the current one, if
    /// there is one and nothing is lost by it: the tile had nothing drawn,
    /// or the textures of the waiting state are settled.
    void promoteIfReady(std::size_t tile);
    /// True when the tile's newest state, waiting or drawn, is `state` and
    /// its chunks have not changed since.
    bool holdsState(std::size_t tile, int state) const;
    /// Frees the least recently used unused textures beyond the budget.
    void trimTextureCache();
    /// The three passes of draw(): everything but backdrop and water, the
    /// backdrop terrain, then water blended over both.
    enum class Pass { Foreground, Backdrop, Water };

    /// Draws the batches of the `visible` tiles that belong to `pass`, with
    /// the world program bound. Of backdrop chunks not drawn from `zone`
    /// (see fh1::WorldChunk::visibleFrom), only the parts below the camera
    /// are drawn.
    void drawBatches(const std::vector<std::size_t>& visible, Pass pass, int zone, Stats& stats);
    void addTextureUser(std::uint32_t id);
    void removeTextureUser(std::uint32_t id);
    void deleteTexture(GpuTexture& texture);
    QMatrix4x4 viewProjection(const WorldCamera& camera, QSize viewport) const;

    const fh1::WorldTileGrid* m_grid = nullptr;
    /// Per tile: the state drawn, and the newer one waiting for its
    /// textures (state -1 when there is none).
    std::vector<GpuTile> m_tiles;
    std::vector<GpuTile> m_waitingTiles;
    qint64 m_uploadedTriangles = 0;
    float m_viewDistance = 9000.0F;
    float m_nearPlane = 1.0F;

    std::unordered_map<std::uint32_t, GpuTexture> m_textures;
    /// Loaded textures no tile uses, least recently used first, and the
    /// video memory they hold.
    std::list<std::uint32_t> m_unusedTextures;
    qint64 m_unusedTextureBytes = 0;
    bool m_compressedTextures = false;
    float m_maxAnisotropy = 1.0F;

    bool m_initialized = false;
    QString m_error;
    std::unique_ptr<QOpenGLShaderProgram> m_program;
    /// Uniform locations of the world program, set by draw().
    struct Uniforms {
        int textured = -1;
        int shading = -1;
        /// uHasLayerB, uHasLayerC, uHasSplat, uHasOcclusion.
        std::array<int, fh1::TileMesh::LayerCount> hasLayer{-1, -1, -1, -1};
        /// uScaleA, uScaleB, uScaleC.
        std::array<int, 3> scales{-1, -1, -1};
        int belowCamera = -1;
    };
    Uniforms m_uniforms;
};
