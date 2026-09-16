#pragma once
/**
 * camera_controls.h
 *
 * Editor camera control globals: auto-orbit and cursor-lock toggles that are
 * persisted to camera_mode.cfg alongside the camera mode.
 */

namespace CameraControls {

// Auto-orbit toggle (Orbit camera mode).
inline bool gAutoOrbit = false;

// Cursor-lock FPS look toggle.
inline bool gCursorLock = false;

} // namespace CameraControls