# Documentation Structure

Organized documentation for the RTT Engine project.

## 📁 Root Level

```
3D GAME ENGINE/
├── README.md                 # Main project overview
├── Makefile                  # Build system
├── test.cpp                  # Main editor application
├── docs/                     # All documentation
├── ecs/                      # ECS framework
├── renderer/                 # Rendering system
├── animationSystem/          # Animation system
├── physicsSystem/            # Physics system
├── modelSystem/              # Model loading
└── ... (other systems)
```

## 📚 Documentation Folders

### docs/
Main documentation index and guides:
- [README.md](README.md) - Documentation index
- **general/** - General engine documentation
- **ui/** - Editor UI documentation
- **ecs/** - ECS architecture docs
- **animation/** - Animation system docs
- **physics/** - Physics system docs
- **world/** - World/terrain docs
- **memory/** - Memory management docs
- **camera/** - Camera system docs

### System READMEs
Each major system folder has a README.md:
- [ecs/README.md](../ecs/README.md)
- [renderer/README.md](../renderer/README.md)
- [animationSystem/README.md](../animationSystem/README.md)
- [physicsSystem/README.md](../physicsSystem/README.md)
- [modelSystem/README.md](../modelSystem/README.md)

## 📝 Documentation Types

### 1. README Files (Keep)
- System overviews
- Usage examples
- API documentation
- Configuration options

### 2. Technical Guides (In docs/)
- Implementation details
- Debugging guides
- Performance optimization
- Integration guides

### 3. Feature Documentation (In docs/)
- Feature specifications
- Design documents
- Architecture diagrams

### 4. Temporary Files (Delete after resolution)
- Bug fix summaries
- Work-in-progress notes
- Debug logs
- Resolved issue reports

## 🗂️ File Organization Rules

### Where to Put Documentation

| Content Type | Location |
|--------------|----------|
| System overview | `system/README.md` |
| Usage guide | `docs/system/` |
| Debug guide | `docs/system/` |
| Bug fix (resolved) | Delete after merge |
| Bug fix (ongoing) | `docs/system/BUG_NAME.md` |
| Feature proposal | `docs/general/FEATURE_PROPOSAL.md` |
| Architecture | `docs/general/` or `docs/system/` |

### Naming Conventions

```
README.md                      # System overview
FEATURE_NAME.md                # Feature documentation
SYSTEM_GUIDE.md                # Usage guide
SYSTEM_DEBUG.md                # Debug guide
BUG_DESCRIPTION.md             # Bug documentation (temporary)
```

## 📊 Current Documentation Status

### Complete ✅
- Main README.md
- ecs/README.md
- renderer/README.md
- animationSystem/README.md
- physicsSystem/README.md
- modelSystem/README.md
- docs/README.md

### In Progress 🔄
- boneSystem/README.md
- cameraSystem/README.md
- meshSystem/README.md
- lighting/README.md
- shaderSystem/README.md
- world/README.md
- memory/README.md

### Missing ⏳
- playerSystem/README.md
- motionMatching/README.md
- demo/README.md
- tests/README.md

## 🎯 Documentation Goals

1. **One README per system** - Clear entry point for each system
2. **Consolidated guides** - No duplicate information
3. **Temporary files cleaned** - Remove resolved issue docs
4. **Up-to-date examples** - Code examples that work
5. **Searchable structure** - Easy to find information

## 🧹 Cleanup Checklist

- [x] Remove temporary .md files from root
- [x] Create system READMEs for major systems
- [x] Organize docs/ folder structure
- [x] Create documentation index
- [ ] Create remaining system READMEs
- [ ] Archive old bug fix docs
- [ ] Update all code examples
- [ ] Add missing API documentation

---

**Status:** 🔄 Documentation reorganization complete
**Last Updated:** March 27, 2025
