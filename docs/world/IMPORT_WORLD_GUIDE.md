# 🌍 Import Actual Game Worlds Guide

## Overview

The engine can now import **complete game worlds** from various formats including Unity, Unreal Engine, Blender, and more!

---

## 📁 Supported Formats

| Format | Extension | Source | Status |
|--------|-----------|--------|--------|
| **FBX Scene** | `.fbx` | Unity, Unreal, Blender | ✅ Supported |
| **OBJ World** | `.obj` | Any 3D software | ✅ Supported |
| **glTF Scene** | `.gltf`/`.glb` | Modern 3D apps | ✅ Supported |
| **Blend File** | `.blend` | Blender | ⚠️ Requires conversion |
| **Unity Prefab** | `.unity` | Unity | ⚠️ Partial support |
| **Unreal Map** | `.umap` | Unreal Engine | ❌ Requires conversion |

---

## 🎯 How To Import Worlds

### Method 1: FBX Export (Recommended)

**From Unity:**
1. Select objects/terrain in Hierarchy
2. Right-click → Export To FBX
3. Export as `.fbx` with "Export Scene" enabled
4. Copy to `assets/worlds/`

**From Unreal Engine:**
1. Select actors in World Outliner
2. File → Export All
3. Choose FBX format
4. Enable "Export Map" if available
5. Copy to `assets/worlds/`

**From Blender:**
1. File → Export → FBX
2. Enable "Selected Objects" if needed
3. Check "Apply Scalings: FBX All"
4. Copy to `assets/worlds/`

### Method 2: OBJ Export

**Any 3D Software:**
1. Export world/terrain as `.obj`
2. Ensure "Write Materials" is enabled
3. Copy `.obj` and `.mtl` files to `assets/worlds/`

### Method 3: glTF Export

**Modern Workflow:**
1. Export as `.gltf` or `.glb` (binary)
2. glTF 2.0 recommended
3. Copy to `assets/worlds/`

---

## 📂 Directory Structure

```
assets/
├── worlds/              # Imported worlds
│   ├── my_level.fbx
│   ├── terrain.obj
│   └── scene.gltf
├── world_objects/       # Individual props
│   ├── trees/
│   ├── rocks/
│   └── buildings/
└── textures/            # World textures
```

---

## 🔧 Using Imported Worlds

### In test.cpp

```cpp
#include "world/WorldImporter.h"

// Create importer
WorldImporter importer;

// Import world file
WorldImporter::ImportedWorld world = importer.importWorld("assets/worlds/my_level.fbx");

// Check what was imported
std::cout << "World: " << world.name << "\n";
std::cout << "Size: " << world.worldSize.x << " x " << world.worldSize.y << "\n";
std::cout << "Objects: " << world.objects.size() << "\n";

// Place imported objects in engine
for (const auto& obj : world.objects) {
    std::cout << "  - " << obj.name << " at (" 
              << obj.position.x << ", " << obj.position.y << ", " 
              << obj.position.z << ")\n";
}

// Convert to engine format (optional)
importer.convertToEngineFormat(world, "assets/converted_worlds/");
```

---

## 🎮 Example: Import Unity Level

### Step 1: Export from Unity
```
1. Open your Unity scene
2. Select terrain, buildings, props
3. Right-click → Export To FBX
4. Save as "forest_level.fbx"
5. Enable:
   - ☑ Export Scene
   - ☑ Include Materials
   - ☑ Apply Transform
```

### Step 2: Copy to Engine
```bash
mkdir -p assets/worlds
cp forest_level.fbx assets/worlds/
```

### Step 3: Import in Code
```cpp
WorldImporter importer;
auto world = importer.importWorld("assets/worlds/forest_level.fbx");

// World data now available:
// - world.objects[] - All placed objects
// - world.worldSize - Dimensions
// - world.name - Level name
```

---

## 🌍 Example: Import Blender World

### Step 1: Prepare in Blender
```
1. Organize world in collections:
   - Terrain
   - Buildings
   - Vegetation
   - Props

2. Apply all transforms (Ctrl+A)
3. Ensure origin points are correct
```

### Step 2: Export
```
File → Export → FBX:
- ☑ Selected Objects (if needed)
- Apply Scalings: FBX All
- Forward: -Z Forward
- Up: Y Up
- Export
```

### Step 3: Import
```cpp
WorldImporter importer;
auto blenderWorld = importer.importWorld("assets/worlds/blender_scene.fbx");
```

---

## 📊 Import Statistics

When you import a world, you'll see:

```
=== WORLD IMPORTER ===
Importing: assets/worlds/forest_level.fbx
Detected format: FBX scene
Loading with Assimp...
Scene loaded successfully!
  Meshes: 156
  Materials: 24
  Nodes: 89
World bounds: (-500, 0, -500) to (500, 200, 500)
World size: 1000 x 200 x 1000 meters
Objects found: 156
```

---

## 🔍 Troubleshooting

### World Not Loading

**Check:**
1. File path is correct
2. File format is supported (.fbx, .obj, .gltf)
3. File isn't corrupted
4. Console for error messages

### Objects Missing

**Solutions:**
1. Re-export with "Export Scene" enabled
2. Check object visibility in source app
3. Ensure objects aren't hidden layers
4. Try glTF format (more reliable)

### Scale Issues

**Fix:**
1. Apply transforms in source app (Ctrl+A in Blender)
2. Export with "Apply Scalings" enabled
3. Adjust scale in engine: `obj.scale *= 10.0f`

### Rotation Wrong

**Fix:**
1. Check coordinate system (Y-up vs Z-up)
2. Export with correct forward/up axes
3. Apply rotation in source app

---

## 🚀 Advanced Features

### Batch Import Multiple Worlds

```cpp
WorldImporter importer;
std::vector<std::string> worldFiles = {
    "assets/worlds/level1.fbx",
    "assets/worlds/level2.fbx",
    "assets/worlds/level3.fbx"
};

for (const auto& file : worldFiles) {
    auto world = importer.importWorld(file);
    // Process each world
}
```

### Extract Specific Objects

```cpp
auto world = importer.importWorld("assets/worlds/city.fbx");

// Find only buildings
for (const auto& obj : world.objects) {
    if (obj.type == "building") {
        // Place building in engine
    }
}
```

### Merge Multiple Worlds

```cpp
WorldImporter::ImportedWorld merged;
merged.name = "merged_world";

auto world1 = importer.importWorld("assets/worlds/forest.fbx");
auto world2 = importer.importWorld("assets/worlds/city.fbx");

// Combine objects
merged.objects.insert(merged.objects.end(), 
                      world1.objects.begin(), 
                      world1.objects.end());
merged.objects.insert(merged.objects.end(), 
                      world2.objects.begin(), 
                      world2.objects.end());
```

---

## 📝 Best Practices

### For Unity Users
- Use **FBX** format
- Export with **embedded textures**
- Apply all transforms before export
- Keep polygon count reasonable (<100k tris)

### For Unreal Users
- Use **FBX** or **glTF**
- Export actors in logical groups
- Bake lighting if needed
- Check LOD settings

### For Blender Users
- Use **FBX** or **glTF**
- Apply transforms (Ctrl+A)
- Set correct coordinate system
- Use collections for organization

---

## 🎯 Next Steps

After importing:

1. **Place in scene** - Use object positions from import
2. **Add collision** - Generate physics colliders
3. **Setup lighting** - Configure lights from import
4. **Add navigation** - Generate navmeshes
5. **Test performance** - Check FPS with imported world

---

## 📚 Related Documentation

- **[FEATURES_COMPLETE.md](FEATURES_COMPLETE.md)** - Engine features
- **[IMPORT_MODELS_GUIDE.md](IMPORT_MODELS_GUIDE.md)** - Model import
- **[README.md](README.md)** - Engine overview

---

**Version**: 0.5.0-alpha (World Import Edition)  
**Last Updated**: 2025  
**Status**: ✅ FBX/OBJ/glTF Import Ready
