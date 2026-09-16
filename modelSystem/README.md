# Model System

3D model loading, mesh management, and LOD system using Assimp.

## 📁 Files

```
modelSystem/
├── Model.h                   # Model class header
├── model.cpp                 # Model implementation
├── ModelHandle.h             # Opaque model handle
├── ModelManager.h/.cpp       # Model registry + asset cache
├── TextureCompression.h      # Texture compression helpers
├── README.md                 # This file
└── (meshSystem/ related)
```

## 🎯 Features

### Model Loading
- **FBX Import** - Full FBX support via Assimp
- **OBJ Import** - Wavefront OBJ support
- **GLTF Import** - Modern GLTF/GLB support
- **Texture Loading** - Diffuse, normal, specular maps
- **Material System** - PBR material support

### Mesh Management
- **VAO/VBO/EBO** - OpenGL buffer management
- **Index Buffer** - Indexed geometry
- **Vertex Attributes** - Position, normal, UV, tangent
- **Mesh Optimization** - Vertex cache optimization
- **Batching** - Merge meshes by material

### LOD System
- **Multiple LOD Levels** - Distance-based switching
- **Automatic Generation** - Mesh simplification
- **Hysteresis** - Prevent LOD popping
- **Budget-based** - Triangle budget management

### Skinned Meshes
- **Bone Weights** - Up to 4 bones per vertex
- **GPU Skinning** - Vertex shader skinning
- **Bone Texture** - Large skeleton support
- **Animation Ready** - Compatible with animation system

## 📖 Usage

### Loading Models

```cpp
Model* model = new Model("character.fbx");

if (!model->IsLoaded()) {
    std::cerr << "Failed to load model\n";
    return;
}

// Get mesh count
size_t meshCount = model->GetMeshCount();

// Get specific mesh
Mesh& mesh = model->GetMesh(0);
```

### Drawing Models

```cpp
Shader& shader = shaderManager.get("pbr");
Animator animator(model);

// Draw with animation
model->Draw(shader, animator);

// Draw with LOD
float lodBias = 1.0f;
model->DrawLOD(shader, animator, cameraPos, lodBias);

// Draw instanced
std::vector<ModelInstance> instances;
// ... populate instances
model->DrawInstanced(shader, animator, instances);
```

### Programmatic Mesh Creation

```cpp
// Create from VAO (for debug meshes)
Model* cubeModel = Model::CreateFromVAO(cubeVAO, 36);

// Or set debug mesh on existing model
model->setDebugVAO(vao);
model->setDebugIndexCount(indexCount);
```

### LOD Configuration

```cpp
LODConfig lodConfig;
lodConfig.enabled = true;
lodConfig.maxDistance = 100.0f;
lodConfig.levels = {
    {1.0f, 0},    // 100% triangles at 0m
    {0.5f, 20},   // 50% triangles at 20m
    {0.25f, 50},  // 25% triangles at 50m
    {0.1f, 100}   // 10% triangles at 100m
};

model->SetLODConfig(lodConfig);
```

## 🔧 Configuration

### Model Import Settings
```cpp
ImportSettings settings;
settings.flipUV = true;
settings.generateNormals = true;
settings.generateTangents = true;
settings.optimizeMesh = true;
settings.calculateBoundingBox = true;
```

### Material Settings
```cpp
PBRMaterial material;
material.albedo = glm::vec3(0.8f);
material.metallic = 0.5f;
material.roughness = 0.5f;
material.ao = 1.0f;
material.doubleSided = false;
```

## 📊 Performance

| Operation | Time | Memory |
|-----------|------|--------|
| **Model Load (FBX)** | ~100ms | Varies |
| **Mesh Draw** | ~0.01ms | - |
| **LOD Switch** | ~0.001ms | - |
| **GPU Skinning** | ~0.1ms | Low |

## 🛠️ Build & Test

The model system compiles as part of the main engine build. From the repository root:

```bash
make            # build bin/test_runner + bin/engine (debug)
make test       # run the full unit-test suite (731 tests / 99 suites)
make run        # self-check tests, then boot the engine
make run-headless  # bounded headless engine run (CI-friendly)
make test-list  # list every test
```

FBX loading, skeleton extraction, skinning and model/material rendering are covered
by `make test` (the `FBXLoaderTest`, `FBXAnimationIntegrationTest`, `ECSRenderGLTest`,
`PbrForwardGL` and `ModelTexture` suites; filter with `--gtest_filter=FBX*`).
See the [root README](../README.md#test-suites) for the full target list and prerequisites.

## 🐛 Known Issues

See [Troubleshooting](../docs/TROUBLESHOOTING.md) (FBX `ASSIMP ERROR` asset warnings
are non-fatal) and the [Architecture](../docs/ARCHITECTURE.md) notes on the
render/data flow for the model → skeleton → skinning pipeline.

---

**Status:** ✅ Production Ready
**Last Updated:** September 2026
