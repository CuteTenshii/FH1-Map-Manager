# FH1 Map Viewer

A Qt 6 / C++20 desktop viewer for the world of Forza Horizon (Xbox 360), read straight from an extracted disc.

The repository contains no game data: the viewer reads the files of your own copy of the game.

![The 2D map of Colorado: the game's satellite image with gameplay objects, route markers, collision objects and road nodes drawn over it, and the Objects panel listing barn finds](.forgejo/screenshots/app.png)

It shows the game's own satellite map (`UI.zip` → `New Map/MapGameReady.jpg`) with these layers over it:

| Layer                                                                       | Source                                   | Colorado count |
|-----------------------------------------------------------------------------|------------------------------------------|----------------|
| Gameplay objects (barn finds, speed cameras, festival sites, gas stations…) | `Ribbon_00/GameObjs.xml`                 | 2,148          |
| Route start points and markers (start grids, waypoints, checkpoints…)       | `Ribbon_00/TrackRouteNNN.xml`            | ~7,800         |
| Particle emitters and trigger zones                                         | `Ribbon_00/ParticleEmitters.xml`         | 2,367          |
| Collision objects (barriers, signs, benches…)                               | `Ribbon_00/CollObjs.xml`                 | 21,877         |
| Road network nodes                                                          | `<track>.nav`                            | 12,036         |
| AI race routes, named from `gamedb.slt`                                     | `aiopenworld.zip` → `route_NNN.owt`      | 46             |
| Post-processing zones                                                       | `Ribbon_00/PostProcessingZones_Safe.xml` | 16             |

Gameplay objects are drawn with the game's own map icons and named from the game's text:

- **Race events, exhibitions and nemesis races:** event names such as "Oakley Blitz".
- **Barn finds:** full car names such as "1971 Plymouth Cuda 426 HEMI".
- **Other activities:** speed traps, speed zones, flyers, gas stations and the festival buildings are grouped by their in-game category.
- **AI routes:** real names such as "Beaumont Circuit".
- **Post-processing zones:** labelled with their region names.

Labels can be toggled with View → Show Labels (Ctrl+L).

Each layer is split into groups that can be toggled separately in the Layers panel. Click an object to see its properties, or search for one in the Objects panel and double-click to jump to it. The status bar shows the world X/Z under the cursor, and right-clicking copies it.

**3D world** (View → 3D World, Ctrl+2): fly through the track's terrain, roads, rocks, rivers and distant mountains, decoded from the game's render models in `bin.zip`.
- **Textures:** everything is drawn with the game's own textures, streamed in after the geometry. Terrain shaders blend several texture layers; only the first layer is drawn so far, so ground cover changes with hard edges. Textures that exist only as small copies in the `.bundle` packs (mostly flat colours) are drawn from those copies.
- **Map layers in 3D:** the layers shown on the 2D map are drawn in the world at their real positions and heights: objects as the game's icons or coloured markers (with a tick for their heading up close), AI routes as lines, and zones as translucent areas, with labels nearby. Hills hide what is behind them. The Layers panel's checkboxes apply to both views.
- **Selection:** click a marker, route or zone to select it; the Objects and Properties panels follow, and the selection stays highlighted in both views. Double-clicking an object in the Objects panel flies the camera to it.
- **Controls:** click to select, drag to look, W A S D to move, Q and E to go down and up, Shift to go faster, and the mouse wheel to change speed.
- **Loading:** the world streams in 500 m tiles at a level of detail chosen by distance. The first opening of a track indexes its models (a few seconds); the index is cached after that.
- **World Debug** (View → World Debug, Ctrl+Shift+D): lists the model files in the loaded tiles (LOD, tile, triangles, textures, read errors) and the textures they use (size, format, source file, video memory, status). Select a texture to preview it at any mip level, as colour, alpha or both, and save it as a PNG. Select a model to list its textures; select a texture to list the models that use it.
- **Clear Cache** (File menu): deletes the cached world indexes (`world/*.index` in the user cache folder). The 3D world shown at the time is rebuilt straight away.
- **Not yet shown:** props placed in their own local space (town buildings, signs, festival structures) are left out until their placement data is decoded.

## Build

Requires CMake ≥ 3.21, a C++20 compiler, Qt ≥ 6.5 (Core, Gui, Widgets, Sql with the SQLite driver, Concurrent, OpenGL, OpenGLWidgets, Test), zlib, and a GPU with desktop OpenGL 3.3 for the 3D view.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build
```

To also run the checks against a real disc:

```sh
FH1_GAME_DIR=/path/to/extracted/disc ctest --test-dir build
```

## Run

```sh
./build/fh1mapviewer /path/to/extracted/disc      # or pick it with File → Open Game Folder
```

The folder can be the disc root (holding `default.xex` and `media`) or the `media` folder itself. The last folder, track, layer visibility and window layout are remembered.

For scripted use (no display needed with `QT_QPA_PLATFORM=offscreen`, except for the 3D view):

```sh
fh1mapviewer DISC --layers gameobjs,nav --centre -1239,1435 --zoom 2 --screenshot out.png
fh1mapviewer DISC --select BF_CUDA_426BF_CLOSEDC --screenshot car.png
```

`fh1render` renders a 3D view to a PNG without a window, with the same renderer (it needs OpenGL 3.3 but no display):

```sh
fh1render DISC out.png --camera 1600,400,-2400,60,-12   # x,y,z, heading (0 = east, 90 = north), pitch
fh1render DISC out.png --untextured                      # plain ground colours, no textures
fh1render DISC out.png --layers gameobjs,airoutes        # with map layers and their labels
```

`fh1meshscan` parses every render model in an archive and reports LOD statistics:

```sh
fh1meshscan media/tracks/colorado/bin.zip
```

`fh1zip` lists, extracts and CRC-verifies entries of the game's zip archives:

```sh
fh1zip list media/UI.zip "New Map"
fh1zip extract media/UI.zip "New Map/MapGameReady.jpg" map.jpg
fh1zip verify media/tracks/colorado/bin.zip
```

## Format notes

These were worked out while building the viewer and are verified against the disc.

**XMemCompress LZX (zip method 21).** Each entry is a series of chunks: `FF, u16be outSize, u16be inSize` or just `u16be inSize` for a 32 KiB chunk. Each chunk decodes to one LZX frame. Decoder state carries across chunks, but the bit reader restarts at every chunk. Two details differ from cabinet LZX (and from libmspack):

- An odd-length uncompressed block is **not** followed by a pad byte. The next block header starts on the very next byte.
- 16-bit word alignment is measured from where bit reading resumed: the chunk start, or the byte after an uncompressed block's data. It is not measured from the stream start.

With these rules, all 230,057 entries of `tracks/colorado/bin.zip` and every other archive on the disc decode and match their CRC-32.

**Zip layout.** The data offset of each entry is in extra field `0x1123`. Many archives have no local headers. The 16-bit entry count in the end record wraps for `bin.zip`, so the central directory is walked by size.

**`<track>.nav`** (big-endian): a 10-word header (word 1 = node count, word 4 = total links), then 32-byte nodes `{u32 id, f32 x, y, z, u32 linkCount, u32 firstLink, u32 ?, u32 ?}`. The link table that follows is not decoded yet.

**`route_NNN.owt`** (big-endian): `"OWTM"`, `u32 version = 1`, 8 zero bytes, `u32 pointCount`, `u32 isLoop`, 8 zero bytes. Then 48-byte points starting with `f32 x, y, z`, then a 16-byte trailer. `NNN` is `Tracks.RouteId` in `gamedb.slt`.

**Map image calibration.** `MapGameReady.jpg` is 5120×3072. Image pixel = (0.2675·X + 2559.0, −0.2673·Z + 1227.5), about 3.74 m per pixel. This was fitted by projecting the `.nav` road nodes onto the image and maximising their overlap with road pixels. Tracks without a known image use 1 px = 1 m.

**String tables** (`stringtables/<LANG>.zip` → `*.str`): `"LSB2"`, header words, then at 0x28 a `u32` count N. Then N entries `{u16 key, u32 offset}` sorted by key, a `0xFFFF` sentinel entry, and NUL-terminated UTF-16BE strings. Offsets count UTF-16 units from the start of the strings. A database text value such as `_&202713995` refers to key `value & 0xFFFF` in the table named after the database table (`Events.str`, `Tracks.str`, `Data_Car.str`, `List_CarMake.str`). The high bits identify the table.

**Map labels and icons.**

- **Activity locations.** An activity config in `gamemodes.zip/<Track>/*.xml` places its activity at the GameObjs IDs named by `<TriggerZone object=…>`. The trigger zone's `radius` is how close you must drive to be offered the activity. `<Behaviour id="CPlaceCarAtObject" object_name="…_NODE">` names where the game places your car when the activity starts, typically about 10 m from the trigger. Activities without trigger zones (speed traps, speed zones, flyers) are placed at objects whose IDs extend the activity's name, such as `speed_camera_30_left`.
- **Event names.** `Events.HorizonEventID` (e.g. `FR05`) links objects to career events.
- **Icon categories.** `ui/MapProfileFullscreen.xml` maps each map category (`mapTag`, e.g. `barnfind`) to cells of `ui/textures/Horizon.zip → Map/icons/MapIcons/<LANG>/MapIconSheet.xds`. The icon is the stack of its `icon_up` and `icon_up_career` symbolizers. The other symbolizer types are player-progress overlays such as the "NEW" badge and the finished trophy.

**Render models (`*.rmb.bin` in a track's `bin.zip`)**, big-endian. All 102,012 Colorado models parse with this layout:

- **Header:** `u32 6`, the bounding box (two float4s at 0x04 and 0x14), a 4×4 transform at 0x24 (identity in every file), and the part count at 0x74. Parts start at 0xB0.
- **Part:** a name, `u32 3`, the vertex count, the stride (16–36), `u32 0`, then the vertices, each starting with `f32 x, y, z`. Then `u32 1, materialCount, 1` and the materials. Between parts: a 48-byte bounds block and `u32 1`.
- **Material:** `u32 2`, a name, `0, ?, table index, 1`, 48 bytes (position offset, position scale, texture coordinate offset u, v and scale u, v, each a float4), then `4, 0, indexCount, indexWidth (2|4)`, the indices, and `endTag, 1`.
- **Material table** (after the last part): `u32 1, 1, 1`, the entry count, and per entry `3, shader, 0`, two groups of `1, n, n × float4` shader constants, then `1, slotCount` and the texture slots (an index into the model's texture list, or `0xFFFFFFFE` for engine textures such as lightmaps). Then `1, shaderCount` and the shader paths (`shaders\track\h_diff_1.fx`), followed by compiled shader code.
- **Index buffers:** triangle strips with all-ones restart markers. A buffer without restarts whose length is a multiple of three is a triangle list; this rule is inferred from the data.
- **Coordinates:** positions have Z negated compared with the placement XMLs and the 2D map.
- **Duplicates:** most models are stored several times under the same name with identical contents; Colorado has 14,291 distinct models.
- **Local-space props:** models centred on the origin are props in local space.
- **Backdrop terrain:** `TERR_UberLOD_*` models are a low-detail terrain for the whole map. Where detailed terrain exists they lie within a few metres of it, sometimes up to 9 m above it; beyond the drivable area they are the only ground. Their level names start at `LOD00` like everything else (and `TERR_UberLOD_Patch18` has none), so distance bands alone would draw them close up. The viewer draws them behind all other geometry instead, so they only show where nothing else covers the ground.
- **Level of detail:** comes from the `_LODnn` part of the name.
- **Vertices:** after the position come 4-byte inputs in the order of the shader's input table (normal, texture coordinates, tangent, colour). Texture coordinates are two unsigned 16-bit fractions, mapped through the material's offset and scale.

**Texture binding.** The track's `Ribbon_00/<Track>_00.pvs` (magic `FPVS`) lists the texture records (`u32 textureId, recordNumber, 1.0f, 1.0f, 0, 0, flags`), then shader paths, draw records, and one record per render model: `n`, n texture record numbers, `m`, m shader numbers, and 15 floats. Record *i* belongs to `<track>.<i>.rmb.bin`. A material's slot *k* is the texture for the shader's sampler register *k*; register 0 is the diffuse texture (`Diffuse_TextureSampler`, or `Blend_ASampler` for terrain) in every track shader. Each compiled shader (`shaders/track/*.fxobj`) has a vertex input table after its `vs_3_0` version string, which gives the offset of TEXCOORD0.

**Track textures.** `_0x<ID>.bix` is a `BIX1` header (big-endian width, height, mip count, fetch-constant format word, total size, top-level size) followed by the smaller mips; `_0x<ID>_B.bix` holds the tiled top level. Small textures are whole in `_0x<ID>.bin`, a `CAFF` container whose `.data` section describes the texture (format word at 0x18, width and height at 0x24) and whose `.gpu` section holds the texels. The viewer uploads the top level's DXT blocks unchanged and builds smaller mips itself.

**Bundle packs (`_0x1000xxxx.bundle`).** Small copies (4×4 to 16×16) of nearly every texture, keyed by texture record number; for about a third of the textures the models use (all flagged `0x101` in the PVS file) they are the only copy. A pack is `u32 count`, then per texture `u32 record, width, height, mipCount, formatWord, 0xFFFFFFFF, size` and `size` bytes of tiled data. The same texture appears in several packs.

**Packed mip tail.** Tiled textures 16 texels or less on their shorter side keep their top level inside a shared 32×32-block tile, 16 texels along the shorter axis (along X for square textures). The bundle copies match the full-size textures only with this offset, and it applies to small CAFF strips such as 256×16 too.

**Xbox 360 textures (`.xds`).** A 52-byte header whose last 24 bytes are the GPU texture fetch constant: word 0 bit 31 = tiled; word 1 bits 0–5 = format, bits 6–7 = byte order; word 2 = (width − 1) | (height − 1) << 13. The top mip level follows, in the GPU's tiled block layout. DXT1/3/5 and 8_8_8_8 are decoded.

## Layout

- `src/core`: formats and loading (LZX, zip, XML/binary parsers, string tables, Xbox and track textures, activity configs, database, calibration). No widget code.
- `src/app`: the viewer (map view, layer items, 3D view with its world and map-layer renderers, World Debug panel, main window).
- `tools`: `fh1zip` (archives), `fh1render` (headless 3D render), `fh1meshscan` (model statistics).
- `tests`: unit tests, with a small LZX encoder for building test streams, plus the optional real-disc suite.

## License

This library is licensed under the MIT License. See [LICENSE](LICENSE)

Forza and Forza Horizon are trademarks of Microsoft Corporation. Forza Horizon was developed by Playground Games and published by Microsoft Studios, and the game's data belongs to Microsoft. This project is not affiliated with or endorsed by Microsoft or Playground Games.
