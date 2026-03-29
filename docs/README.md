# RTT Engine Documentation

Organized technical documentation for the RTT Engine.

## 📚 Documentation Structure

### General Documentation
- [README.md](general/README.md) - Engine overview
- [IMPORT_MODELS_GUIDE.md](general/IMPORT_MODELS_GUIDE.md) - Model import guide
- [TESTING_GUIDE.md](general/TESTING_GUIDE.md) - Testing instructions
- [PERFORMANCE_OPTIMIZATION_PLAN.md](general/PERFORMANCE_OPTIMIZATION_PLAN.md) - Performance guide

### System Documentation

#### Animation System
- [MOTION_MATCHING_COMPLETE.md](animation/MOTION_MATCHING_COMPLETE.md) - Motion matching overview
- [HYBRID_MM_FSM_IMPLEMENTATION_COMPLETE.md](animation/HYBRID_MM_FSM_IMPLEMENTATION_COMPLETE.md) - Hybrid system
- [BONE_MATRIX_UBO_OPTIMIZATION.md](animation/BONE_MATRIX_UBO_OPTIMIZATION.md) - GPU skinning
- [FOOT_IK_CHARACTER_GROUNDING_FIX.md](animation/FOOT_IK_CHARACTER_GROUNDING_FIX.md) - IK system
- [FSM_UNREAL_STYLE.md](animation/FSM_UNREAL_STYLE.md) - State machine UI

#### Physics System
- [GJK_PHYSICS_OPTIMIZATIONS.md](physics/GJK_PHYSICS_OPTIMIZATIONS.md) - Collision detection

#### World System
- [TERRAIN_SYSTEM.md](world/TERRAIN_SYSTEM.md) - Terrain generation
- [OPEN_WORLD_GUIDE.md](world/OPEN_WORLD_GUIDE.md) - Open world streaming
- [IMPORT_WORLD_GUIDE.md](world/IMPORT_WORLD_GUIDE.md) - World import

#### Memory System
- [MEMORY_MANAGEMENT_COMPLETE.md](memory/MEMORY_MANAGEMENT_COMPLETE.md) - Memory optimization

#### Camera System
- [CAMERA_FIX.md](camera/CAMERA_FIX.md) - Camera controls

## 🗂️ Cleanup Status

### Root Directory .md Files
The following files in the root directory are temporary/work-in-progress and should be consolidated:

**Keep:**
- `README.md` - Main project readme
- `ECS_ARCHITECTURE.md` - ECS overview diagram
- `EDITOR_V2_FEATURES.md` - Editor feature list

**Consolidate into system READMEs:**
- `EDITOR_FEATURES.md` → Merge with main README
- `FBO_VIEWPORT_FIX.md` → Merge with renderer/README.md
- `UNREAL_STYLE_UI.md` → Merge with docs/general/
- `LIVE_STATISTICS.md` → Merge with renderer/README.md
- `PROFILER_TAB.md` → Merge with renderer/README.md
- `TABBED_PANELS*.md` → Merge with docs/general/
- `UI_*.md` → Merge into single UI guide
- `TODO_IMPLEMENTATION.md` → Update main README
- `VIEWPORT_*.md` → Merge with renderer/README.md
- `RESIZE_FIX.md` → Delete (resolved)
- `UI_MINOR_FIXES.md` → Delete (resolved)
- `UI_BUG_FIXES.md` → Delete (resolved)

**Delete (resolved issues):**
- `ECS_ENHANCEMENT_SUMMARY.md` - Superseded by ecs/README.md
- `INSTALL_IMGUI.md` - Setup complete
- `PROFILER_UI_FINAL_STATUS.md` - Superseded
- `FONT_LOAD.log` - Debug file

### docs/ Folder Organization

```
docs/
├── README.md                 # This file - documentation index
├── general/                  # General documentation
│   ├── README.md
│   ├── IMPORT_MODELS_GUIDE.md
│   ├── TESTING_GUIDE.md
│   └── ... (keep existing)
├── animation/                # Animation system docs
│   └── ... (keep existing)
├── physics/                  # Physics system docs
│   └── ... (keep existing)
├── world/                    # World system docs
│   └── ... (keep existing)
├── memory/                   # Memory system docs
│   └── ... (keep existing)
└── camera/                   # Camera system docs
    └── ... (keep existing)
```

## 📝 Documentation Guidelines

### When to Create New .md Files
1. **Major feature documentation** - New systems, major changes
2. **Debug guides** - Troubleshooting specific issues
3. **Performance guides** - Optimization techniques
4. **Integration guides** - How to use systems together

### When NOT to Create New .md Files
1. **Minor bug fixes** - Update existing docs or commit message
2. **Temporary work notes** - Use comments in code
3. **Duplicate information** - Link to existing docs instead
4. **Resolved issues** - Archive or delete after resolution

### Documentation Structure
Each .md file should have:
```markdown
# Title

Brief description of what this covers.

## Overview
What, why, when to use.

## Usage
Code examples, commands, steps.

## Technical Details
Implementation details if relevant.

## Known Issues
Any limitations or bugs.

## Related Documentation
Links to related docs.
```

## 🎯 Next Steps

1. **Consolidate root .md files** - Move to appropriate folders
2. **Update system READMEs** - Ensure all have complete docs
3. **Archive resolved issues** - Move to docs/archive/ or delete
4. **Create missing docs** - physics/, lighting/, shaderSystem/

---

**Status:** 🔄 Documentation reorganization in progress
**Last Updated:** March 27, 2025
