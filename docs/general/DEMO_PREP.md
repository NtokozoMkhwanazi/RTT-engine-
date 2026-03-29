# 🎬 Demo Preparation Checklist

## ✅ Engine Prep Status

### Phase 1: Code Preparation

- [x] Demo configuration header created (`demo/DemoConfig.h`)
- [x] Demo recorder class created (`demo/DemoRecorder.h/.cpp`)
- [ ] Integrate demo recorder into `test.cpp`
- [ ] Add F6 hotkey for demo mode toggle
- [ ] Test demo playback mode

### Phase 2: Visual Polish

- [ ] Ensure terrain loads correctly
- [ ] Verify water is visible and animated
- [ ] Check vegetation is rendering (trees, rocks)
- [ ] Confirm character model loads
- [ ] Test motion matching (walk, run, crouch, jump)
- [ ] Verify foot IK is working

### Phase 3: Performance

- [ ] Run `make clean && make` - no errors
- [ ] Launch engine - no console errors
- [ ] Press F1 - check FPS is 60+
- [ ] Check frame time is under 16ms
- [ ] Verify no memory leaks (run for 5+ minutes)

### Phase 4: Recording Setup

- [ ] Install OBS Studio: `sudo apt install obs-studio`
- [ ] Configure OBS:
  - Resolution: 1920x1080
  - FPS: 60
  - Bitrate: 20000+ kbps
  - Format: MP4
- [ ] Test recording (5 seconds, play back)
- [ ] Check audio (if adding music)

### Phase 5: Demo Rehearsal

- [ ] Run through demo sequence manually
- [ ] Note key presses and timing
- [ ] Practice smooth camera movement
- [ ] Time each segment (~20 seconds each)
- [ ] Do 3-5 practice runs

### Phase 6: Recording

- [ ] Close all other applications
- [ ] Disable notifications
- [ ] Clean desktop
- [ ] Start OBS recording
- [ ] Run engine
- [ ] Execute demo sequence
- [ ] Record 5-10 takes
- [ ] Stop recording

### Phase 7: Post-Processing

- [ ] Review all takes
- [ ] Pick best 2-3 minutes
- [ ] Trim start/end
- [ ] Add title card (optional)
- [ ] Add end card with credits
- [ ] Export final video

### Phase 8: Sharing

- [ ] Upload to YouTube
- [ ] Post to Twitter/X
- [ ] Submit to Reddit (r/gamedev, r/cpp, r/opengl)
- [ ] Update GitHub README with video
- [ ] Write devlog post

---

## 🎮 Demo Sequence Script

```
┌─────────────────────────────────────────────────────────────────┐
│ TIME  │ SEGMENT              │ ACTION                          │
├───────┼──────────────────────┼─────────────────────────────────┤
│ 0:00  │ Title Card           │ "RTT Engine - Feb 2026"         │
│ 0:10  │ Terrain Overview     │ Wide camera pan over landscape  │
│ 0:30  │ Character Movement   │ Walk → Run → Crouch → Jump      │
│ 0:50  │ Foot IK              │ Close-up on feet, uneven ground │
│ 1:10  │ Water System         │ Character walks into water      │
│ 1:30  │ Vegetation           │ Walk through forest area        │
│ 1:50  │ Camera Orbit         │ Smooth camera rotation          │
│ 2:10  │ Performance          │ Press F1, show 60+ FPS          │
│ 2:20  │ End Card             │ "Built in South Africa"         │
│ 2:30  │ END                  │                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## 🎯 Quick Start Commands

```bash
# Build engine
cd "/home/run-time-terror/Documents/3D GAME ENGINE"
make clean && make

# Run engine
./bin/run

# Run tests (verify everything works)
make test

# Install OBS for recording
sudo apt install obs-studio
```

---

## 🔧 Demo Mode Controls

| Key | Action |
|-----|--------|
| **F6** | Toggle demo recording/playback |
| **WASD** | Character movement |
| **SPACE** | Jump |
| **SHIFT** | Sprint |
| **CTRL** | Crouch |
| **C** | Toggle camera mode |
| **Mouse** | Camera orbit |
| **Scroll** | Zoom |
| **F1** | Show GPU stats |
| **F** | Toggle wireframe |
| **ESC** | Exit |

---

## 📊 Expected Performance

| Metric | Target | Acceptable |
|--------|--------|------------|
| FPS | 60+ | 50+ |
| Frame Time | <10ms | <16ms |
| Draw Calls | <100 | <200 |
| Triangles | <100K | <200K |

---

## 🐛 Troubleshooting

**Problem:** Low FPS
**Solution:** Reduce view distance, lower vegetation density

**Problem:** Black screen
**Solution:** Check shader compilation, verify asset paths

**Problem:** Character not moving
**Solution:** Check motion matching initialization, verify animations loaded

**Problem:** Water not visible
**Solution:** Walk toward lower elevation, check water level setting

---

## 📞 Pre-Recording Checklist (Day Of)

- [ ] Engine builds without errors
- [ ] All features work correctly
- [ ] FPS is stable at 60+
- [ ] OBS is configured and tested
- [ ] Desktop is clean
- [ ] Notifications disabled
- [ ] Phone on silent
- [ ] Practice run completed

---

**Ready to record? Let's make this count.** 🎬
