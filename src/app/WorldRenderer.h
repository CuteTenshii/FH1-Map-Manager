#pragma once

#include "LoadedFiles.h"
#include "MapCalibration.h"
#include "WorldTiles.h"

#include <QImage>
#include <QMatrix4x4>
#include <QOpenGLFunctions_3_3_Core>
#include <QSize>
#include <QVector3D>

#include <memory>
#include <unordered_map>
#include <vector>

class QOpenGLShaderProgram;
class QOpenGLTexture;

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

/// OpenGL side of the 3D world: tile buffers, the game's textures, the
/// satellite texture, shaders and drawing. Every method that touches OpenGL needs the context that
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
    void setSatellite(const QImage& image, const fh1::MapCalibration& calibration);
    void setViewDistance(float metres) { m_viewDistance = metres; }
    float viewDistance() const { return m_viewDistance; }

    /// Tiles whose loaded state differs from what `camera` needs, nearest
    /// first. Tiles with a build running (inFlightState[tile] >= 0) are left
    /// out.
    std::vector<TileRequest> requests(const WorldCamera& camera, const std::vector<int>& inFlightState) const;
    /// Replaces a tile's buffers. Only OpenGL work; the mesh is built elsewhere.
    /// Textures the mesh uses that are not loaded yet are queued, see
    /// takeTextureRequests().
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
    /// Colour and distance scale of the fog draw() applies.
    static QVector3D fogColour();
    float fogDistance() const { return m_viewDistance * kFogFraction; }
    qint64 uploadedTriangles() const { return m_uploadedTriangles; }
    int tileCount() const { return static_cast<int>(m_tiles.size()); }
    int loadedState(int tile) const { return m_tiles[static_cast<std::size_t>(tile)].state; }

private:
    /// Fog distance as a fraction of the view distance.
    static constexpr float kFogFraction = 0.75F;

    struct GpuTile {
        GLuint vao = 0;
        GLuint vbo = 0;
        GLuint ebo = 0;
        GLsizei indexCount = 0;
        std::vector<fh1::TileMesh::Batch> batches;
        std::vector<fh1::TileMesh::Model> models;
        int state = -1;
    };

    struct GpuTexture {
        enum class State { Queued, Requested, Ready, Failed };
        GLuint name = 0;
        /// Loaded tiles whose batches use the texture.
        int users = 0;
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
    void addTextureUser(std::uint32_t id);
    void removeTextureUser(std::uint32_t id);
    void deleteTexture(GpuTexture& texture);
    void uploadSatellite();
    QMatrix4x4 viewProjection(const WorldCamera& camera, QSize viewport) const;

    const fh1::WorldTileGrid* m_grid = nullptr;
    std::vector<GpuTile> m_tiles;
    qint64 m_uploadedTriangles = 0;
    float m_viewDistance = 9000.0F;

    std::unordered_map<std::uint32_t, GpuTexture> m_textures;
    bool m_compressedTextures = false;
    float m_maxAnisotropy = 1.0F;

    QImage m_satellite;
    fh1::MapCalibration m_calibration;
    bool m_satelliteDirty = false;

    bool m_initialized = false;
    QString m_error;
    std::unique_ptr<QOpenGLShaderProgram> m_program;
    std::unique_ptr<QOpenGLTexture> m_satelliteTexture;
};
