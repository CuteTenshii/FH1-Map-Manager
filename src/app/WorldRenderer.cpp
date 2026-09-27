#include "WorldRenderer.h"

#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLShaderProgram>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace {

constexpr float kFieldOfView = WorldRenderer::kFieldOfViewDegrees;
/// Tiles are built out to this multiple of the view distance, so turning
/// around does not expose missing terrain, and freed beyond a further margin
/// so a camera moving back and forth does not rebuild them constantly.
constexpr float kStreamMargin = 1.1F;
constexpr float kEvictMargin = 1.3F;
/// Metres of reach added per metre of clearance. From straight above, the
/// edges of a view are about twice as far as the ground under the camera.
constexpr float kReachPerClearance = 2.0F;
/// The near plane moves out with the clearance, by this fraction of it, to
/// keep depth precise when the far plane is kilometres away.
constexpr float kNearPlanePerClearance = 0.02F;

const char* const kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexcoord;
layout(location = 3) in vec2 aTexcoord1;
layout(location = 4) in vec4 aColour;
uniform mat4 uViewProjection;
out vec3 vPosition;
out vec3 vNormal;
out vec2 vTexcoord;
out vec2 vTexcoord1;
out vec4 vColour;
void main()
{
    vPosition = aPosition;
    vNormal = aNormal;
    vTexcoord = aTexcoord;
    vTexcoord1 = aTexcoord1;
    vColour = aColour;
    // World Z points north; the game's own files, and OpenGL, use Z south.
    gl_Position = uViewProjection * vec4(aPosition.x, aPosition.y, -aPosition.z, 1.0);
}
)";

// Shading values, as in fh1::TileMesh::Shading.
const char* const kFragmentShader = R"(#version 330 core
in vec3 vPosition;
in vec3 vNormal;
in vec2 vTexcoord;
in vec2 vTexcoord1;
in vec4 vColour;
out vec4 fragColour;
uniform sampler2D uDiffuse;
uniform sampler2D uLayerB;
uniform sampler2D uLayerC;
uniform sampler2D uSplat;
uniform sampler2D uOcclusion;
uniform int uShading;
uniform bool uTextured;
uniform bool uHasLayerB;
uniform bool uHasLayerC;
uniform bool uHasSplat;
uniform bool uHasOcclusion;
uniform vec2 uScaleA;
uniform vec2 uScaleB;
uniform vec2 uScaleC;
uniform vec3 uCamera;
uniform vec3 uSunDirection;
uniform vec3 uFogColour;
uniform float uFogDistance;
// Drawing only what lies below the camera.
uniform bool uBelowCameraOnly;

const int kPlain = 0;
const int kSplat = 1;
const int kWater = 3;

// Geometry without a loaded texture: a plain ground colour, greener where
// the surface is level and a neutral rock tone on steep faces.
vec3 untexturedColour(float up)
{
    vec3 ground = mix(vec3(0.46, 0.43, 0.37), vec3(0.40, 0.44, 0.33), up);
    return mix(vec3(0.42, 0.40, 0.37), ground, smoothstep(0.35, 0.75, up));
}

// Ground: up to three layers mixed by the splat map (red weighs B, green C,
// the rest A) or by the vertex colour's alpha, darkened by the occlusion map.
vec3 groundColour()
{
    vec3 a = texture(uDiffuse, vTexcoord * uScaleA).rgb;
    vec3 b = uHasLayerB ? texture(uLayerB, vTexcoord * uScaleB).rgb : a;
    vec3 blended;
    if (uShading == kSplat) {
        vec3 c = uHasLayerC ? texture(uLayerC, vTexcoord * uScaleC).rgb : a;
        vec2 w = uHasSplat ? texture(uSplat, vTexcoord1).rg : vec2(0.0);
        float wa = max(1.0 - w.x - w.y, 0.0);
        blended = (a * wa + b * w.x + c * w.y) / max(wa + w.x + w.y, 1e-4);
    } else {
        blended = mix(a, b, vColour.a);
    }
    if (uHasOcclusion) {
        blended *= texture(uOcclusion, vTexcoord1).r;
    }
    return blended;
}

void main()
{
    if (uBelowCameraOnly && vPosition.y > uCamera.y) {
        discard;
    }
    float distance = length(vPosition - uCamera);
    float fog = clamp(1.0 - exp(-pow(distance / uFogDistance, 2.0)), 0.0, 1.0);

    if (uShading == kWater) {
        // A deep tint and the sky it reflects, more of the sky at grazing
        // angles; the normal map ripples it. The game reflects a cube map
        // of the scene, which the viewer does not have.
        vec3 ripple = uTextured ? texture(uDiffuse, vTexcoord).xyz * 2.0 - 1.0 : vec3(0.0);
        vec3 surfaceNormal = normalize(vec3(ripple.x * 0.15, 1.0, ripple.y * 0.15));
        vec3 toEye = normalize(uCamera - vPosition);
        float fresnel = pow(1.0 - clamp(dot(toEye, surfaceNormal), 0.0, 1.0), 3.0);
        vec3 colour = mix(vec3(0.09, 0.20, 0.25), uFogColour, 0.15 + 0.75 * fresnel);
        float glint = pow(max(dot(reflect(-toEye, surfaceNormal), uSunDirection), 0.0), 80.0);
        colour += vec3(0.6 * glint);
        fragColour = vec4(mix(colour, uFogColour, fog), mix(0.75, 1.0, fresnel));
        return;
    }

    vec3 normal = normalize(gl_FrontFacing ? vNormal : -vNormal);
    float up = clamp(normal.y, 0.0, 1.0);
    vec3 base;
    if (!uTextured) {
        base = untexturedColour(up);
    } else if (uShading == kPlain) {
        vec4 texel = texture(uDiffuse, vTexcoord);
        // Foliage, fences and decals are cut-outs.
        if (texel.a < 0.5) {
            discard;
        }
        base = texel.rgb;
    } else {
        base = groundColour();
    }
    float diffuse = max(dot(normal, uSunDirection), 0.0);
    float sky = 0.5 + 0.5 * normal.y;
    vec3 colour = base * (0.28 + 0.22 * sky + 0.75 * diffuse);
    fragColour = vec4(mix(colour, uFogColour, fog), 1.0);
}
)";

const QVector3D kFogColour(0.70F, 0.78F, 0.86F);

// From EXT_texture_compression_s3tc and EXT_texture_filter_anisotropic,
// which Qt's core-profile headers do not define.
constexpr GLenum kCompressedDxt1 = 0x83F1;
constexpr GLenum kCompressedDxt5 = 0x83F3;
constexpr GLenum kTextureMaxAnisotropy = 0x84FE;
constexpr GLenum kMaxTextureMaxAnisotropy = 0x84FF;
constexpr float kWantedAnisotropy = 8.0F;

constexpr GLuint kDiffuseUnit = 0;

/// Frustum planes (a, b, c, d with a*x + b*y + c*z + d >= 0 inside) from a
/// combined projection matrix, by the Gribb-Hartmann method.
std::array<QVector4D, 6> frustumPlanes(const QMatrix4x4& m)
{
    const QVector4D r0 = m.row(0);
    const QVector4D r1 = m.row(1);
    const QVector4D r2 = m.row(2);
    const QVector4D r3 = m.row(3);
    return {r3 + r0, r3 - r0, r3 + r1, r3 - r1, r3 + r2, r3 - r2};
}

bool boxVisible(const std::array<QVector4D, 6>& planes, const QVector3D& lo, const QVector3D& hi)
{
    for (const QVector4D& p : planes) {
        // The box corner furthest along the plane normal.
        const QVector3D corner(
            p.x() >= 0.0F ? hi.x() : lo.x(), p.y() >= 0.0F ? hi.y() : lo.y(), p.z() >= 0.0F ? hi.z() : lo.z());
        if (p.x() * corner.x() + p.y() * corner.y() + p.z() * corner.z() + p.w() < 0.0F) {
            return false;
        }
    }
    return true;
}

/// World coordinates (Z north) to the render frame (Z south).
QMatrix4x4 worldToRender()
{
    QMatrix4x4 m;
    m.scale(1.0F, 1.0F, -1.0F);
    return m;
}

} // namespace

QVector3D WorldCamera::forward() const
{
    const float c = std::cos(pitch);
    return {c * std::cos(yaw), std::sin(pitch), c * std::sin(yaw)};
}

WorldRenderer::WorldRenderer() = default;
WorldRenderer::~WorldRenderer() = default;

bool WorldRenderer::initialize()
{
    m_initialized = false;
    m_error.clear();
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (context == nullptr) {
        m_error = QStringLiteral("no OpenGL context is current");
        return false;
    }
    const QSurfaceFormat format = context->format();
    if (context->isOpenGLES() || format.version() < qMakePair(3, 3)) {
        m_error = QStringLiteral("the 3D view needs desktop OpenGL 3.3 or newer; this context is %1 %2.%3")
                      .arg(context->isOpenGLES() ? QStringLiteral("OpenGL ES") : QStringLiteral("OpenGL"))
                      .arg(format.majorVersion())
                      .arg(format.minorVersion());
        return false;
    }
    if (!initializeOpenGLFunctions()) {
        m_error = QStringLiteral("could not resolve OpenGL 3.3 core functions");
        return false;
    }
    // Without S3TC the game's DXT textures are decoded to RGBA on upload,
    // which costs four to eight times the video memory.
    m_compressedTextures = context->hasExtension(QByteArrayLiteral("GL_EXT_texture_compression_s3tc"));
    m_maxAnisotropy = 1.0F;
    if (context->hasExtension(QByteArrayLiteral("GL_EXT_texture_filter_anisotropic"))
        || context->hasExtension(QByteArrayLiteral("GL_ARB_texture_filter_anisotropic"))) {
        glGetFloatv(kMaxTextureMaxAnisotropy, &m_maxAnisotropy);
    }
    m_program = std::make_unique<QOpenGLShaderProgram>();
    const bool linked = m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)
        && m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader) && m_program->link();
    if (!linked) {
        m_error = QStringLiteral("shaders failed to build: %1").arg(m_program->log());
        m_program.reset();
        return false;
    }
    m_initialized = true;
    return true;
}

void WorldRenderer::release()
{
    if (!m_initialized) {
        return;
    }
    for (GpuTile& tile : m_tiles) {
        releaseTile(tile);
    }
    for (auto& [id, texture] : m_textures) {
        deleteTexture(texture);
    }
    m_textures.clear();
    m_program.reset();
    m_uploadedTriangles = 0;
    m_initialized = false;
}

void WorldRenderer::setGrid(const fh1::WorldTileGrid* grid)
{
    if (m_initialized) {
        for (GpuTile& tile : m_tiles) {
            releaseTile(tile);
        }
        for (auto& [id, texture] : m_textures) {
            deleteTexture(texture);
        }
    }
    // Without a context there are no OpenGL names left to free: release()
    // already freed them.
    m_textures.clear();
    m_grid = grid;
    m_tiles.assign(grid != nullptr ? grid->tiles().size() : 0, GpuTile{});
    m_uploadedTriangles = 0;
}

void WorldRenderer::releaseTile(GpuTile& tile)
{
    if (tile.vao != 0) {
        glDeleteVertexArrays(1, &tile.vao);
        glDeleteBuffers(1, &tile.vbo);
        glDeleteBuffers(1, &tile.ebo);
    }
    for (const fh1::TileMesh::Batch& batch : tile.batches) {
        for (const std::uint32_t id : batch.textures()) {
            removeTextureUser(id);
        }
    }
    m_uploadedTriangles -= tile.indexCount / 3;
    tile = GpuTile{};
}

void WorldRenderer::addTextureUser(std::uint32_t id)
{
    ++m_textures[id].users;
}

void WorldRenderer::removeTextureUser(std::uint32_t id)
{
    const auto it = m_textures.find(id);
    if (it == m_textures.end()) {
        return;
    }
    if (--it->second.users <= 0) {
        // A texture still being loaded is dropped too; uploadTexture()
        // ignores results nobody waits for.
        deleteTexture(it->second);
        m_textures.erase(it);
    }
}

void WorldRenderer::deleteTexture(GpuTexture& texture)
{
    if (texture.name != 0) {
        glDeleteTextures(1, &texture.name);
        texture.name = 0;
    }
}

std::vector<std::uint32_t> WorldRenderer::takeTextureRequests()
{
    std::vector<std::uint32_t> ids;
    for (auto& [id, texture] : m_textures) {
        if (texture.state == GpuTexture::State::Queued) {
            texture.state = GpuTexture::State::Requested;
            ids.push_back(id);
        }
    }
    return ids;
}

void WorldRenderer::uploadTexture(std::uint32_t id, const fh1::TextureMipChain& chain)
{
    const auto it = m_textures.find(id);
    if (!m_initialized || it == m_textures.end() || chain.levels.empty()) {
        return;
    }
    GpuTexture& texture = it->second;
    deleteTexture(texture);
    glGenTextures(1, &texture.name);
    glActiveTexture(GL_TEXTURE0 + kDiffuseUnit);
    glBindTexture(GL_TEXTURE_2D, texture.name);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    texture.bytes = 0;
    const auto levelCount = static_cast<int>(chain.levels.size());
    for (int level = 0; level < levelCount; ++level) {
        const QByteArray& data = chain.levels[static_cast<std::size_t>(level)];
        const int width = chain.levelWidth(level);
        const int height = chain.levelHeight(level);
        if (chain.format == fh1::TextureSurface::Format::Rgba8) {
            glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data.constData());
            texture.bytes += data.size();
        } else if (m_compressedTextures) {
            const GLenum format = chain.format == fh1::TextureSurface::Format::Dxt1 ? kCompressedDxt1 : kCompressedDxt5;
            glCompressedTexImage2D(
                GL_TEXTURE_2D, level, format, width, height, 0, static_cast<GLsizei>(data.size()), data.constData());
            texture.bytes += data.size();
        } else {
            const QImage image = fh1::surfaceToImage(chain.level(level)).convertToFormat(QImage::Format_RGBA8888);
            glTexImage2D(
                GL_TEXTURE_2D, level, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.constBits());
            texture.bytes += image.sizeInBytes();
        }
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levelCount - 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    if (m_maxAnisotropy > 1.0F) {
        glTexParameterf(GL_TEXTURE_2D, kTextureMaxAnisotropy, std::min(kWantedAnisotropy, m_maxAnisotropy));
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    texture.state = GpuTexture::State::Ready;
    texture.width = chain.width;
    texture.height = chain.height;
    texture.levels = levelCount;
    texture.format = chain.format;
    texture.origin = chain.origin;
    texture.files = chain.files;
    texture.error.clear();
}

void WorldRenderer::failTexture(std::uint32_t id, const QString& error)
{
    const auto it = m_textures.find(id);
    if (it != m_textures.end()) {
        it->second.state = GpuTexture::State::Failed;
        it->second.error = error;
    }
}

std::vector<LoadedModel> WorldRenderer::loadedModels() const
{
    std::vector<LoadedModel> models;
    for (std::size_t t = 0; t < m_tiles.size(); ++t) {
        for (const fh1::TileMesh::Model& model : m_tiles[t].models) {
            models.push_back({model.chunk, static_cast<int>(t), model.triangles, model.textures, model.error});
        }
    }
    return models;
}

std::vector<LoadedTexture> WorldRenderer::loadedTextures() const
{
    std::vector<LoadedTexture> textures;
    textures.reserve(m_textures.size());
    for (const auto& [id, texture] : m_textures) {
        LoadedTexture loaded;
        loaded.id = id;
        switch (texture.state) {
        case GpuTexture::State::Queued:
        case GpuTexture::State::Requested:
            loaded.state = LoadedTexture::State::Loading;
            break;
        case GpuTexture::State::Ready:
            loaded.state = LoadedTexture::State::Loaded;
            break;
        case GpuTexture::State::Failed:
            loaded.state = LoadedTexture::State::Failed;
            break;
        }
        loaded.tiles = texture.users;
        loaded.bytes = texture.bytes;
        loaded.width = texture.width;
        loaded.height = texture.height;
        loaded.levels = texture.levels;
        loaded.format = texture.format;
        loaded.origin = texture.origin;
        loaded.files = texture.files;
        loaded.error = texture.error;
        textures.push_back(std::move(loaded));
    }
    std::sort(
        textures.begin(), textures.end(), [](const LoadedTexture& a, const LoadedTexture& b) { return a.id < b.id; });
    return textures;
}

WorldRenderer::TextureStats WorldRenderer::textureStats() const
{
    TextureStats stats;
    for (const auto& [id, texture] : m_textures) {
        switch (texture.state) {
        case GpuTexture::State::Queued:
        case GpuTexture::State::Requested:
            ++stats.pending;
            break;
        case GpuTexture::State::Ready:
            ++stats.loaded;
            stats.bytes += texture.bytes;
            break;
        case GpuTexture::State::Failed:
            ++stats.failed;
            break;
        }
    }
    return stats;
}

bool WorldRenderer::texturesPending() const
{
    return std::any_of(m_textures.begin(), m_textures.end(), [](const auto& entry) {
        return entry.second.state == GpuTexture::State::Queued || entry.second.state == GpuTexture::State::Requested;
    });
}

float WorldRenderer::clearance(const WorldCamera& camera) const
{
    if (m_grid == nullptr) {
        return 0.0F;
    }
    // Tile bounds include far-off backdrop meshes; the lowest top of the
    // tiles under the camera is the closest cheap guess at the ground.
    std::optional<float> ground;
    for (const auto& tile : m_grid->tiles()) {
        if (camera.position.x() >= tile.boundsMin.x() && camera.position.x() <= tile.boundsMax.x()
            && camera.position.z() >= tile.boundsMin.z() && camera.position.z() <= tile.boundsMax.z()) {
            ground = ground ? std::min(*ground, tile.boundsMax.y()) : tile.boundsMax.y();
        }
    }
    return ground ? std::max(0.0F, camera.position.y() - *ground) : 0.0F;
}

float WorldRenderer::reach(const WorldCamera& camera) const
{
    return m_viewDistance + kReachPerClearance * clearance(camera);
}

float WorldRenderer::tileDistance(const fh1::WorldTileGrid::Tile& tile, const WorldCamera& camera) const
{
    const float along = m_grid->distanceTo(tile, camera.position.x(), camera.position.z());
    const float up = std::max(0.0F, camera.position.y() - tile.boundsMax.y());
    return std::hypot(along, up);
}

std::vector<TileRequest> WorldRenderer::requests(const WorldCamera& camera, const std::vector<int>& inFlightState) const
{
    std::vector<TileRequest> result;
    if (m_grid == nullptr) {
        return result;
    }
    const float range = reach(camera) * kStreamMargin;
    const auto& tiles = m_grid->tiles();
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        const float distance = tileDistance(tiles[i], camera);
        if (distance > range) {
            continue;
        }
        const int state = m_grid->stateAt(tiles[i], distance);
        const int inFlight = i < inFlightState.size() ? inFlightState[i] : -1;
        if ((m_tiles[i].state == state && !m_tiles[i].stale) || inFlight >= 0) {
            continue;
        }
        result.push_back({static_cast<int>(i), state, distance, m_grid->chunksAt(tiles[i], distance)});
    }
    std::sort(result.begin(), result.end(),
        [](const TileRequest& a, const TileRequest& b) { return a.distance < b.distance; });
    return result;
}

bool WorldRenderer::isComplete(const WorldCamera& camera) const
{
    if (m_grid == nullptr) {
        return false;
    }
    const float range = reach(camera) * kStreamMargin;
    const auto& tiles = m_grid->tiles();
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        const float distance = tileDistance(tiles[i], camera);
        if (distance <= range && (m_tiles[i].state != m_grid->stateAt(tiles[i], distance) || m_tiles[i].stale)) {
            return false;
        }
    }
    return true;
}

void WorldRenderer::invalidateTile(int tile)
{
    if (tile >= 0 && static_cast<std::size_t>(tile) < m_tiles.size()
        && m_tiles[static_cast<std::size_t>(tile)].state >= 0) {
        m_tiles[static_cast<std::size_t>(tile)].stale = true;
    }
}

void WorldRenderer::upload(int tile, int state, const fh1::TileMesh& mesh)
{
    if (!m_initialized || tile < 0 || static_cast<std::size_t>(tile) >= m_tiles.size()) {
        return;
    }
    // Counting the new batches' textures before releasing the old ones
    // keeps textures both states share from being freed and reloaded.
    for (const fh1::TileMesh::Batch& batch : mesh.batches) {
        for (const std::uint32_t id : batch.textures()) {
            addTextureUser(id);
        }
    }
    GpuTile& gpu = m_tiles[static_cast<std::size_t>(tile)];
    releaseTile(gpu);
    gpu.state = state;
    gpu.stale = false;
    gpu.batches = mesh.batches;
    gpu.models = mesh.models;
    if (mesh.indices.empty()) {
        return;
    }
    glGenVertexArrays(1, &gpu.vao);
    glGenBuffers(1, &gpu.vbo);
    glGenBuffers(1, &gpu.ebo);
    glBindVertexArray(gpu.vao);
    glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(mesh.vertices.size() * sizeof(float)), mesh.vertices.data(),
        GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(mesh.indices.size() * sizeof(std::uint32_t)),
        mesh.indices.data(), GL_STATIC_DRAW);
    constexpr GLsizei kStride = fh1::TileMesh::kFloatsPerVertex * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, kStride, nullptr);
    // OpenGL takes offsets into the bound buffer as pointer values.
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, kStride,
        reinterpret_cast<const void*>(3 * sizeof(float))); // NOLINT(performance-no-int-to-ptr)
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, kStride,
        reinterpret_cast<const void*>(6 * sizeof(float))); // NOLINT(performance-no-int-to-ptr)
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, kStride,
        reinterpret_cast<const void*>( // NOLINT(performance-no-int-to-ptr)
            fh1::TileMesh::kSecondTexcoord * sizeof(float)));
    // The colour's four bytes, stored in a float's space.
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, kStride,
        reinterpret_cast<const void*>(fh1::TileMesh::kColour * sizeof(float))); // NOLINT(performance-no-int-to-ptr)
    glBindVertexArray(0);
    gpu.indexCount = static_cast<GLsizei>(mesh.indices.size());
    m_uploadedTriangles += gpu.indexCount / 3;
}

bool WorldRenderer::evictDistant(const WorldCamera& camera)
{
    if (!m_initialized || m_grid == nullptr) {
        return false;
    }
    bool evicted = false;
    const float range = reach(camera) * kEvictMargin;
    const auto& tiles = m_grid->tiles();
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        if (m_tiles[i].state >= 0 && tileDistance(tiles[i], camera) > range) {
            releaseTile(m_tiles[i]);
            evicted = true;
        }
    }
    return evicted;
}

QMatrix4x4 WorldRenderer::worldViewProjection(const WorldCamera& camera, QSize viewport) const
{
    return viewProjection(camera, viewport) * worldToRender();
}

QVector3D WorldRenderer::fogColour()
{
    return kFogColour;
}

QMatrix4x4 WorldRenderer::viewProjection(const WorldCamera& camera, QSize viewport) const
{
    const QMatrix4x4 toRender = worldToRender();
    QMatrix4x4 view;
    view.lookAt(
        toRender.map(camera.position), toRender.map(camera.position + camera.forward()), QVector3D(0.0F, 1.0F, 0.0F));
    QMatrix4x4 projection;
    const float aspect
        = viewport.height() > 0 ? static_cast<float>(viewport.width()) / static_cast<float>(viewport.height()) : 1.0F;
    projection.perspective(
        kFieldOfView, aspect, std::max(m_nearPlane, clearance(camera) * kNearPlanePerClearance), reach(camera) * 1.2F);
    return projection * view;
}

WorldRenderer::Stats WorldRenderer::draw(const WorldCamera& camera, QSize viewport)
{
    Stats stats;
    if (!m_initialized) {
        return stats;
    }
    glViewport(0, 0, viewport.width(), viewport.height());
    glClearColor(kFogColour.x(), kFogColour.y(), kFogColour.z(), 1.0F);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (m_grid == nullptr) {
        return stats;
    }
    const QMatrix4x4 viewProj = viewProjection(camera, viewport);
    const auto planes = frustumPlanes(viewProj * worldToRender());

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    m_program->bind();
    m_program->setUniformValue("uViewProjection", viewProj);
    m_program->setUniformValue("uCamera", camera.position);
    m_program->setUniformValue("uSunDirection", QVector3D(0.45F, 0.8F, 0.35F).normalized());
    m_program->setUniformValue("uFogColour", kFogColour);
    m_program->setUniformValue("uFogDistance", fogDistance(camera));
    m_program->setUniformValue("uDiffuse", static_cast<GLint>(kDiffuseUnit));
    m_program->setUniformValue("uLayerB", static_cast<GLint>(kDiffuseUnit + 1 + fh1::TileMesh::LayerB));
    m_program->setUniformValue("uLayerC", static_cast<GLint>(kDiffuseUnit + 1 + fh1::TileMesh::LayerC));
    m_program->setUniformValue("uSplat", static_cast<GLint>(kDiffuseUnit + 1 + fh1::TileMesh::SplatMap));
    m_program->setUniformValue("uOcclusion", static_cast<GLint>(kDiffuseUnit + 1 + fh1::TileMesh::OcclusionMap));
    m_uniforms.textured = m_program->uniformLocation("uTextured");
    m_uniforms.shading = m_program->uniformLocation("uShading");
    m_uniforms.hasLayer = {m_program->uniformLocation("uHasLayerB"), m_program->uniformLocation("uHasLayerC"),
        m_program->uniformLocation("uHasSplat"), m_program->uniformLocation("uHasOcclusion")};
    m_uniforms.scales = {m_program->uniformLocation("uScaleA"), m_program->uniformLocation("uScaleB"),
        m_program->uniformLocation("uScaleC")};
    m_uniforms.belowCamera = m_program->uniformLocation("uBelowCameraOnly");

    const auto& tiles = m_grid->tiles();
    std::vector<std::size_t> visible;
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        if (m_tiles[i].vao != 0 && boxVisible(planes, tiles[i].boundsMin, tiles[i].boundsMax)) {
            visible.push_back(i);
            ++stats.drawnTiles;
            stats.drawnTriangles += m_tiles[i].indexCount / 3;
        }
    }
    // The backdrop terrain lies within a few metres of the detailed ground
    // where both exist, often above it. Drawing it into the back half of the
    // depth range makes any other geometry win wherever they overlap, while
    // the backdrop still shows, in place, where it is alone. Near the
    // camera it can lie well above the ground and hang over the view like
    // a ceiling; the zone the camera is in says which backdrop pieces the
    // game draws from there. The others still fill the cracks between
    // detailed ground pieces of different levels of detail, so they are
    // drawn below the camera's height, where they can only show through
    // such gaps.
    const fh1::ZoneGrid* zones = m_grid->index().zoneGrid();
    const int zone = zones != nullptr ? zones->zoneAt(camera.position.x(), camera.position.z()) : -1;
    glDepthRange(0.0, kForegroundDepthFar);
    drawBatches(visible, Pass::Foreground, zone, stats);
    glDepthRange(kForegroundDepthFar, 1.0);
    drawBatches(visible, Pass::Backdrop, zone, stats);
    // Water last, blended over what lies beneath it, without hiding what
    // lies behind it from later water.
    glDepthRange(0.0, kForegroundDepthFar);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    drawBatches(visible, Pass::Water, zone, stats);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDepthRange(0.0, 1.0);
    glBindVertexArray(0);
    for (int unit = fh1::TileMesh::LayerCount; unit >= 0; --unit) {
        glActiveTexture(GL_TEXTURE0 + kDiffuseUnit + unit);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    m_program->release();
    // QPainter draws the overlay into the same framebuffer next and expects
    // depth testing off.
    glDisable(GL_DEPTH_TEST);
    return stats;
}

void WorldRenderer::drawBatches(const std::vector<std::size_t>& visible, Pass pass, int zone, Stats& stats)
{
    const auto& chunks = m_grid->index().chunks();
    const auto readyTexture = [this](std::uint32_t id) -> GLuint {
        if (id == fh1::TileMesh::kNoTexture) {
            return 0;
        }
        const auto it = m_textures.find(id);
        return it != m_textures.end() && it->second.state == GpuTexture::State::Ready ? it->second.name : 0;
    };
    for (const std::size_t i : visible) {
        const GpuTile& gpu = m_tiles[i];
        glBindVertexArray(gpu.vao);
        for (const fh1::TileMesh::Batch& batch : gpu.batches) {
            const bool water = batch.shading == fh1::TileMesh::Shading::Water;
            const bool inPass = pass == Pass::Water ? water
                : pass == Pass::Backdrop            ? batch.backdrop && !water
                                                    : !batch.backdrop && !water;
            if (!inPass) {
                continue;
            }
            const bool fillOnly = batch.chunk < chunks.size() && !chunks[batch.chunk].visibleFrom(zone);
            m_program->setUniformValue(m_uniforms.belowCamera, fillOnly);
            m_program->setUniformValue(m_uniforms.shading, static_cast<GLint>(batch.shading));
            const GLuint diffuse = readyTexture(batch.texture);
            m_program->setUniformValue(m_uniforms.textured, diffuse != 0);
            glActiveTexture(GL_TEXTURE0 + kDiffuseUnit);
            glBindTexture(GL_TEXTURE_2D, diffuse);
            for (int layer = 0; layer < fh1::TileMesh::LayerCount; ++layer) {
                const GLuint name = readyTexture(batch.layers[static_cast<std::size_t>(layer)]);
                m_program->setUniformValue(m_uniforms.hasLayer[static_cast<std::size_t>(layer)], name != 0);
                glActiveTexture(GL_TEXTURE0 + kDiffuseUnit + 1 + layer);
                glBindTexture(GL_TEXTURE_2D, name);
            }
            for (std::size_t k = 0; k < m_uniforms.scales.size(); ++k) {
                m_program->setUniformValue(
                    m_uniforms.scales[k], QVector2D(batch.layerScales[2 * k], batch.layerScales[2 * k + 1]));
            }
            // The offset into the bound index buffer, as a pointer value.
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(batch.indexCount), GL_UNSIGNED_INT,
                reinterpret_cast<const void*>( // NOLINT(performance-no-int-to-ptr)
                    static_cast<std::uintptr_t>(batch.firstIndex) * sizeof(std::uint32_t)));
            ++stats.drawCalls;
        }
    }
    m_program->setUniformValue(m_uniforms.belowCamera, false);
    glActiveTexture(GL_TEXTURE0 + kDiffuseUnit);
}
