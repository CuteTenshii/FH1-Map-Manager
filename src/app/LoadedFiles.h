#pragma once

#include "TrackTextures.h"
#include "XboxTexture.h"

#include <QString>

#include <cstdint>
#include <vector>

/// A model file the 3D view has on the GPU, or tried to load.
struct LoadedModel {
    /// Index into WorldIndex::chunks().
    std::uint32_t chunk = 0;
    /// Index into WorldTileGrid::tiles().
    int tile = -1;
    std::uint32_t triangles = 0;
    /// Diffuse texture ids of its materials.
    std::vector<std::uint32_t> textures;
    /// Empty unless the file could not be read.
    QString error;
};

/// A texture that loaded models use.
struct LoadedTexture {
    enum class State { Loading, Loaded, Failed };

    std::uint32_t id = 0;
    State state = State::Loading;
    /// Loaded tiles whose geometry uses it.
    int tiles = 0;
    /// Video memory, once loaded.
    qint64 bytes = 0;
    int width = 0;
    int height = 0;
    int levels = 0;
    fh1::TextureSurface::Format format = fh1::TextureSurface::Format::Rgba8;
    fh1::TextureMipChain::Origin origin = fh1::TextureMipChain::Origin::Bix;
    QString files;
    /// Why loading failed.
    QString error;
};
