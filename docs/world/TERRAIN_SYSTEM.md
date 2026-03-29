# Open World Terrain System

## Features Implemented

### 1. **Chunk-Based Terrain**
- World divided into chunks (100m x 100m each)
- Each chunk: 64x64 vertices (adjustable)
- Dynamic loading/unloading based on camera position

### 2. **Level of Detail (LOD)**
- 4 LOD levels based on distance from camera
- Close chunks: Full detail (LOD 0)
- Far chunks: Reduced detail or culled (LOD 3)
- Reduces rendering cost for distant terrain

### 3. **Procedural Generation**
- Perlin noise-based heightmap generation
- Multiple octaves for natural-looking terrain
- Configurable height scale (max height: 80m)
- Consistent terrain with fixed seed

### 4. **Streaming System**
- View distance: 4 chunks (800m x 800m visible area)
- Automatically loads new chunks as you move
- Unloads far chunks to save memory
- ~50-70 active chunks at any time

### 5. **Height-Based Coloring**
- **Sand/Beach**: Below 3m
- **Grass**: 3m - 8m
- **Rock**: 8m - 23m
- **Snow**: Above 23m

## Configuration

Edit in `test.cpp`:
```cpp
Terrain::TerrainConfig terrainConfig;
terrainConfig.chunkSize = 100.0f;       // Chunk size in meters
terrainConfig.chunkResolution = 64;     // Vertices per side
terrainConfig.viewDistance = 4;         // Chunks loaded in each direction
terrainConfig.lodDistance = 50.0f;      // LOD transition distance
terrainConfig.heightmapSize = 1024;     // Heightmap resolution
terrainConfig.heightScale = 80.0f;      // Maximum terrain height
```

## Performance

- **Active chunks**: ~50-70
- **Vertices per chunk**: 4,161 (65x65)
- **Total visible vertices**: ~250,000 (with LOD)
- **Memory usage**: ~5MB for heightmap + chunk data

## Controls

| Key | Action |
|-----|--------|
| **W/S/A/D** | Move character on terrain |
| **SPACE** | Jump |
| **C** | Toggle camera mode (fixed/orbit) |
| **Mouse** | Orbit camera |
| **Scroll** | Zoom |

## Files Created

```
world/
├── Terrain.h           # Terrain system header
├── Terrain.cpp         # Terrain implementation
├── TerrainChunk.h      # Individual chunk header
├── TerrainChunk.cpp    # Chunk implementation
├── terrainVS.glsl      # Terrain vertex shader
└── terrainFS.glsl      # Terrain fragment shader
```

## Future Enhancements

1. **Texture Splats**: Blend textures based on height/slope
2. **Vegetation**: Trees, grass on terrain
3. **Water System**: Lakes, rivers with proper shading
4. **Caves/Overhangs**: 3D terrain (currently heightmap only)
5. **Physics Integration**: Terrain collision for rigid bodies
6. **Minimap**: Render terrain heightmap to texture

## Usage Example

```cpp
// Create terrain
Terrain* terrain = new Terrain();
terrain->initialize();

// In game loop:
terrain->update(camera.Position, dt);  // Stream chunks
terrain->render();                      // Draw visible chunks

// Get height for collision
float height = terrain->getHeightAt(x, z);
```

## Known Limitations

- No texture blending (vertex colors only)
- No water rendering
- Heightmap-only (no caves/overhangs)
- Fixed LOD (could use geomorphing for smoother transitions)
