# 🌳 Importing Custom 3D Models Guide

## Overview

The engine now supports importing **real 3D models** (trees, rocks, props) to replace polygon billboards with realistic objects.

---

## 📁 Directory Structure

Create this folder for your world objects:

```
assets/world_objects/
├── trees/
│   ├── pine_tree.fbx
│   ├── oak_tree.fbx
│   └── birch_tree.fbx
├── rocks/
│   ├── boulder.fbx
│   ├── stone.fbx
│   └── cliff.fbx
├── vegetation/
│   ├── bush.fbx
│   ├── grass_cluster.fbx
│   └── flowers.fbx
└── props/
    ├── log.fbx
    └── stump.fbx
```

---

## 🎨 Model Requirements

### Format
- **FBX** or **OBJ** (Assimp-supported formats)
- **Triangulated** meshes
- **UV unwrapped** for texturing

### Scale
- **Real-world units** (1 unit = 1 meter)
- Tree height: 4-12m
- Rock size: 0.5-3m
- Bush height: 0.5-2m

### Optimization
- **LOD models**: Create 3 LOD levels (high, medium, low)
- **Low poly count**: 500-5000 triangles for trees
- **Shared materials**: Use same material for similar objects
- **Texture atlases**: Combine multiple textures

---

## 🔧 Import Process

### Step 1: Place Models in Directory

Copy your FBX/OBJ files to `assets/world_objects/`

### Step 2: Engine Will Auto-Load

The `WorldObjectManager` automatically loads models on startup:

```cpp
worldObjects->initialize("assets/world_objects/");
```

### Step 3: Models Are Placed Procedurally

Trees and rocks are placed based on:
- **Terrain height** (trees avoid water)
- **Slope** (no trees on cliffs)
- **Density settings** (configurable per type)

---

## 📝 Configuration

Edit in `test.cpp`:

```cpp
// Tree density (trees per m²)
vegConfig.treeDensity = 0.03f;

// Max trees per chunk (100m x 100m)
vegConfig.maxTreesPerChunk = 80;

// Rock density
vegConfig.rockDensity = 0.02f;

// LOD distances (meters)
config.lodDistance[0] = 20.0f;   // High detail
config.lodDistance[1] = 50.0f;   // Medium
config.lodDistance[2] = 100.0f;  // Low
```

---

## 🎯 Supported Object Types

| Type | Description | Default Scale |
|------|-------------|---------------|
| `TREE_PINE` | Pine/conifer tree | 0.8-1.5x |
| `TREE_OAK` | Oak/deciduous tree | 0.9-1.3x |
| `TREE_BIRCH` | Birch tree | 0.85-1.2x |
| `ROCK_BOULDER` | Large boulder | 0.5-2.0x |
| `ROCK_STONE` | Small stone | 0.3-0.8x |
| `ROCK_CLIFF` | Cliff rock | 1.0-3.0x |
| `BUSH` | Bush/shrub | 0.7-1.2x |
| `GRASS_CLUSTER` | Grass patch | 0.6-1.0x |
| `LOG` | Fallen log | 0.8-1.5x |
| `STUMP` | Tree stump | 0.6-1.0x |

---

## 🔍 Troubleshooting

### Models Not Loading

**Check:**
1. File path is correct: `assets/world_objects/`
2. File format is FBX or OBJ
3. Model has meshes (not empty)
4. Console shows load messages

### Models Too Big/Small

**Fix:**
- Scale model in Blender/Maya before export
- Or adjust `minScale`/`maxScale` in config

### Performance Issues

**Optimize:**
- Reduce polygon count
- Lower `maxTreesPerChunk`
- Reduce view distance
- Use LOD models

---

## 📊 Performance Guidelines

| Object Count | Expected FPS Impact |
|--------------|---------------------|
| 0-100 | Minimal (~1-2%) |
| 100-500 | Low (~5-10%) |
| 500-1000 | Moderate (~10-20%) |
| 1000+ | High (20%+) |

**Optimization Tips:**
- Use instanced rendering (coming soon)
- Enable frustum culling
- Use LOD models
- Limit render distance

---

## 🛠️ Creating Models in Blender

### Quick Tree Tutorial

1. **Create trunk**: Cylinder (radius: 0.2m, height: 2m)
2. **Create foliage**: Cone or sphere (radius: 1-2m)
3. **Add materials**: Bark (brown), leaves (green)
4. **UV unwrap**: For texture mapping
5. **Export as FBX**: 
   - Apply transforms (Ctrl+A)
   - Include materials
   - Triangulate faces

### Quick Rock Tutorial

1. **Create base**: Icosphere or cube
2. **Sculpt**: Add displacement modifier
3. **Decimate**: Reduce poly count
4. **Bake normals**: From high-poly to low-poly
5. **Export as FBX**

---

## 📥 Download Free Models

**Recommended Sources:**
- [Sketchfab](https://sketchfab.com) - Free CC-licensed models
- [TurboSquid](https://turbosquid.com) - Free section
- [CGTrader](https://cgtrader.com) - Free models
- [Kenney.nl](https://kenney.nl) - Low-poly assets
- [OpenGameArt](https://opengameart.org) - Game assets

**Search Terms:**
- "Low poly tree"
- "Pine tree FBX"
- "Rock boulder game"
- "Nature pack"

---

## 🎮 Testing Your Models

1. **Place model** in `assets/world_objects/`
2. **Run engine**: `./bin/run`
3. **Watch console** for load messages
4. **Walk around** to see placed models
5. **Check performance** with F11 (if implemented)

---

## 🚀 Future Enhancements

**Coming Soon:**
- [ ] GPU instancing for 10x performance
- [ ] Wind animation for trees
- [ ] Seasonal variations (autumn, winter)
- [ ] Flower color randomization
- [ ] Collision with tree trunks
- [ ] Tree harvesting/chopping

---

## 📚 Related Documentation

- [OPEN_WORLD_GUIDE.md](OPEN_WORLD_GUIDE.md) - Terrain system
- [TERRAIN_SYSTEM.md](TERRAIN_SYSTEM.md) - Technical details
- [README.md](README.md) - Engine overview

---

**Last Updated**: 2025
**Status**: ✅ Model loading ready, instancing WIP
