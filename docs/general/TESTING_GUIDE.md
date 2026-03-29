# 🧪 Testing Guide - FBX & Animation Fixes

## ✅ Build Status
```
✅ Compilation: SUCCESS
✅ Models loaded: Bot, Bear, Datsun, Rock1, Rock2, Stone
✅ Debug output added for root motion
```

---

## 🎮 How to Test

### **1. Run the Engine**
```bash
cd "/home/run-time-terror/Documents/3D GAME ENGINE"
./bin/run
```

### **2. Check Loading Output**
You should see:
```
Loading models...

=== Loading Model: Bot ===
  Bones: XX
  Meshes: XX
  Size: (X.X, X.X, X.X)
  Type: ANIMATED (skeletal)

=== Loading Model: Bear ===
  ...

=== Loading Model: Datsun ===
  ...

=== Loaded Models ===
  Bot: XX bones, X meshes
  Bear: XX bones, X meshes
  Datsun: X bones, X meshes
  ...
```

### **3. Test Movement (THE CRITICAL TEST)**

**Press W** and watch the console output:

**Expected (if working):**
```
[RootMotion] Mag=0.15 Dir=(0,1,0) MoveMag=1
[RootMotion] Mag=0.18 Dir=(0,1,0) MoveMag=1
...
```

**Character should:**
- Move FORWARD (not just rotate)
- Animation blends from idle → walk
- Position changes: `(0, 0, 0)` → `(0, 0, 5)` → etc.

**If NOT working:**
```
[RootMotion] Mag=0 Dir=(0,0,0) MoveMag=1
```
or
```
No [RootMotion] output at all
```

This means root motion isn't being extracted from the animation.

---

## 🔍 Debug Steps

### **Step 1: Press H (Motion Matching Debug)**

**Expected output:**
```
=== MOTION MATCHING DEBUG ===
Database size: XXXX poses
Search found: X results
Current pose: XXX (t=X.XX)
Blend time: X.XXs
```

**Check:**
- Database size > 0 → Animations loaded ✓
- Search found > 0 → Matching working ✓
- Current pose is not "Idle" when moving → Blending working ✓

### **Step 2: Press G (Foot IK Debug)**

**Expected:**
```
=== FOOT IK STATUS ===
Enabled: YES/NO
Floor height: X.XX
Character grounded: YES/NO
```

### **Step 3: Press F1 (GPU Stats)**

Check FPS is 60+ and frame time is reasonable.

---

## 🐛 Common Issues & Fixes

### **Issue 1: Character Only Rotates, Doesn't Move**

**Symptoms:**
- Pressing W rotates character to face forward
- Character doesn't actually move forward
- Console shows no `[RootMotion]` output

**Possible Causes:**

1. **Root motion not extracted from animation**
   - Animation might not have root bone
   - Root bone name is wrong
   - Check: `animator->ConsumeRootMotion()` returns (0,0,0)

2. **MotionMatcher not finding poses**
   - Press H, check "Search found: 0 results"
   - KD-Tree not built
   - Animations not compatible with skeleton

3. **Movement multiplier is too low**
   - Current: `movementMultiplier = 0.25f`
   - Try increasing to `0.5f` or `1.0f`

**Fix:**
Edit `test.cpp` line ~1370:
```cpp
// Increase multiplier
float movementMultiplier = 0.5f;  // Was 0.25f
```

### **Issue 2: Bear/Datsun Models Don't Load**

**Symptoms:**
```
[ERROR] Failed to load: assets/World_objects/Bear_DEMO.fbx
```

**Fix:**
Check file exists:
```bash
ls -la "assets/World_objects/Bear_DEMO.fbx"
```

If missing, the file might be in a different location or named differently.

### **Issue 3: Camera Behaves Weirdly**

**Symptoms:**
- Camera jerks or snaps when loading different model
- Camera too close/far for model size

**Fix:**
Camera distance is based on model size. For different models:
```cpp
// Adjust camera for model size
if (currentModel == bearModel) {
    cameraDistance = 25.0f;  // Farther for bear
    cameraHeight = 10.0f;
} else if (currentModel == datsunModel) {
    cameraDistance = 30.0f;  // Farther for car
    cameraHeight = 8.0f;
}
```

---

## 📊 What to Report Back

After testing, please tell me:

### **1. Model Loading:**
```
Bot: Loaded? (Y/N)
Bear: Loaded? (Y/N)
Datsun: Loaded? (Y/N)
```

### **2. Movement:**
```
Does pressing W move character forward? (Y/N)
[RootMotion] output appears? (Y/N)
If yes, what's the magnitude? (e.g., 0.15)
```

### **3. Motion Matching:**
```
Press H - Database size: XXXX
Press H - Search found: X results
```

### **4. Console Output:**
Copy/paste any error messages or unusual output.

---

## 🎯 Expected Behavior (When Fully Working)

### **Idle (no input):**
- Character stands still
- Idle animation plays
- Position: `(0, 0, 0)`

### **Walking (W pressed):**
- Character moves forward
- Walk animation plays
- Position: `(0, 0, 0)` → `(0, 0, 5)` → `(0, 0, 10)`
- `[RootMotion] Mag=0.10-0.20`

### **Running (W+Shift):**
- Character moves faster
- Run animation plays
- `[RootMotion] Mag=0.25-0.40`

### **Crouching (Ctrl):**
- Character crouches
- Crouch animation plays
- Movement slower

### **Jumping (Space):**
- Character jumps
- Jump animation plays
- Character goes up, then down

---

## 🔧 Quick Test Commands

```bash
# Build and run
make clean && make && ./bin/run

# Just run (if already built)
./bin/run

# Check model files exist
ls -la assets/World_objects/*.fbx
```

---

## 📞 Next Steps

**After you test and report back:**

1. **If movement works:** Great! We'll polish and add more features.

2. **If movement doesn't work:** I'll need the debug output to diagnose:
   - [RootMotion] output
   - Motion matching debug (H key)
   - Any error messages

3. **If models don't load:** We'll check file paths and fix.

---

**Ready to test? Run `./bin/run` and let me know what happens!** 🚀
