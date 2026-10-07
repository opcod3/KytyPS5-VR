# PSVR2 on OpenXR

This document maps the PlayStation VR2 guest APIs (`libSceHmd2`, `libSceVrTracker2`, the VR
controller part of `libScePad`, and the HMD2 video-out bus) onto OpenXR. It also sets out how to
replace the stubbed headset on the `psvr2-reprojection` branch with a real OpenXR runtime
(Monado or SteamVR on Linux, any conformant runtime on Windows).

Sources: PS5 SDK 4.00 headers `hmd2.h`, `hmd2/*.h`, `vision/vr_tracker2.h`,
`video_out/hmd2_sub.h`, `pad_vr_controller.h`, `pad/vr_controller_trigger_effect.h`, and the
samples `tutorial_vr2/01-basic-vr`, `tutorial_vr2/common/tutorial_utils/src/vr.cpp`,
`api_vr_tracker2`, `tutorial_hand_interaction_of_vr_controller`, `tutorial_fr`. OpenXR names,
struct layouts and behavioural rules are checked against the OpenXR 1.1.60 headers
(`/usr/include/openxr`) and the 1.1 specification.

---

## 0. Status (2026-10-07)

Phases 1–4 of §9 are implemented and were checked by hand against Monado (simulated and
`qwerty` devices) with Horizon Call of the Mountain (`PPSA05666`). Head tracking, world-locked
reprojection, controllers and the headset's own video-out clock work. Phases 5–6 are open; §11
says where to pick up.

What `--vr-mode openxr` does today:

- **Runtime** (`xr/xrRuntime.cpp`). `Xr::Initialize` runs in `WindowInit` before any window
  exists, so a working runtime suppresses the SDL headset window. OpenXR creates the Vulkan
  instance and device (`XR_KHR_vulkan_enable2`); the session runs on our single graphics queue.
  Initialization failure falls back to the virtual headset (the SDL window is then created).
- **Display** (`xr/xrFrameThread.cpp`). `Presenter::PresentHmd` copies each composited panel
  into a persistent 2-layer panel image (`Runtime::RecordPanelCopy`). The frame thread copies
  that into an sRGB swapchain every XR frame and submits one projection layer. The layer is
  world-locked in the space the frame was rendered in, using the pose and FOV from `SetParam`;
  `disableReprojection` or a missing pose gives a head-locked layer in `VIEW`.
- **HMD2 bus clock** (`videoOut.cpp`, `Xr::SetVblankHandler`). Right after each `xrWaitFrame`,
  the frame thread fires the HMD2 port's pre-vblank and vblank events and completes its oldest
  ready flip, so the guest flips at the headset rate and the new panel goes out in that XR frame.
  The main present thread no longer touches that port.
- **Refresh rate.** `EnableVrMode` and `sceVideoOutConfigureOutput` on the HMD2 port request
  the enumerated rate nearest to 89.91 or 119.88 Hz through `XR_FB_display_refresh_rate`
  (`Xr::RequestRefreshRate`). Monado's Qwerty HMD only offers 60 Hz.
- **Mirroring** (`Hmd2::GetMirroring`, `Presenter::PresentHmdMirror`). With mirroring enabled,
  the main window shows the left eye of the newest headset panel: a rectangle of the window's
  aspect ratio centred on the eye's optical axis (from the `SetParam` FOV), so it always fills the
  window. `CENTER` keeps it inside the circle of the smallest eye tangent, clear of the game's lens
  mask; `WIDE` takes the largest such rectangle inside the image.
- **Tracking** (`xr/xrTracking.cpp`, `xr/xrClock.cpp`). `sceVrTracker2GetResult` for the HMD and
  controllers, coordinate systems, recentre, `LocateCoordinateSystem` and the play-area calls.
  `sceHmd2GetFieldOfView`, device status, mount state and lens separation come from the runtime.
  `BeginFrame` and `GetPredictedDisplayTime` bind `flipArg` to display slots of the frame
  thread.
- **Controllers** (`xr/xrInput.cpp`). Action set `psvr2` bound for Touch, Index, WMR, Vive and
  `khr/simple_controller`. `scePadVrControllerReadState`, `GetDeviceInformation`,
  `scePadSetVibration` and `scePadResetOrientation` on the Sense handles use it.
  `scePadVrControllerSetTriggerEffect[s]` play the `VIBRATION` and `MULTIPLE_POSITION_VIBRATION`
  modes as controller haptics while the trigger is pulled to a vibrating position
  (`Xr::SetTriggerVibration`); the stronger of that and `scePadSetVibration` wins. Feedback modes
  have no OpenXR equivalent and only validate.

Deliberate deviations from the plan below:

- `panelResolution` stays 4000×2040 rather than the host panel (§4.1). Games are tuned to it, the
  swapchain is created at that size per eye, and Monado accepts it although it exceeds the
  simulated HMD's `maxImageRect`. `vr_force_psvr2_panel` therefore does not exist.
- `sceHmd2GetFieldOfView` reports the PSVR2's own field of view, not the runtime's (§4.1).
  Horizon masks what the PSVR2 lenses cannot see with a fixed shape; rendered with Monado's
  symmetric frustum, that mask blacked out the temporal quarter of each eye. The projection layer
  carries the frustum the game rendered with, so the runtime still places the image correctly.
  The runtime's field of view is only logged.
- The swapchain is fed by a raw `vkCmdCopyImage` from the 8-bit UNORM panel into the matching
  `*_SRGB` format, not by `gpu_hmd_compose.frag` writing through an sRGB view (§4.3). The bits are
  identical, so colour is right without a shader change. This breaks if the presenter's frame
  format ever stops being `B8G8R8A8_UNORM`/`R8G8B8A8_UNORM` (the frame thread `EXIT`s then).
- The panel is still composited side by side at 4000×2040 by `HmdCompositor` and then split per
  eye by the copy; `HmdCompositor` did not gain a layered target (§6).
- `headPose` is offset by (0, −0.03, +0.09) m in device space, i.e. behind the eyes. §4.5 used to
  say −0.09 Z, which is in front of the face.
- Controller `devicePose` is the grip pose with no per-profile offset (§5.4); nothing looked off
  in Horizon, so no calibration was done.
- Recentring ignores `coordinateResetType` (head and device reset behave the same).
- The HMD2 bus shares `FlipQueue` with the other buses rather than having its own queue (§4.4).
  Each request records its clock, and `Flip(micros, headset)` takes only that clock's groups. A
  flip master/slave pair across the two clocks `EXIT_NOT_IMPLEMENTED`s.
- While the session is not running, the frame thread still fires HMD2 vblanks about every
  11 ms, so a guest waiting on them does not hang.
- The SDK says mirroring needs the main port closed. Horizon keeps it open after flipping it
  once at start-up, so the mirror only gives way to a main port that has flipped in the last
  500 ms.
- Windows: `Xr::Initialize` refuses, because guest-time conversion needs
  `XR_KHR_win32_convert_performance_counter_time`, which is not implemented.

## 1. Where the branch is today

| Commit | What it does | What is fake |
|---|---|---|
| `8818d606` HMD2 video-out bus | `sceVideoOutOpen(BUS_TYPE_HMD2_MAIN)` works; flips on bus `0x20` are presented to a second SDL window. | Paced by the main vblank clock (`Config::GetVblankFrequency()`), not by a headset. |
| `8feda32e` VrTracker2 + Sense pads | Device registry, `sceVrTracker2GetResult`, `scePadVrControllerReadState`, device info, trigger-effect state. | Fixed poses: head at 1.6 m looking down −Z, hands at ±0.2 m. The pads split the host gamepad in half. |
| `9895eb3a` reprojection layers | `sceHmd2ReprojectionSetParam` stores per-buffer layer settings. `HmdCompositor` draws VR3D layers side by side into a 4000×2040 image. | No reprojection, no pose use, no distortion. UI2D and VR2D layers `EXIT_NOT_IMPLEMENTED`. |
| `8b26d19b` stereo expansion | Right-eye replay of `GE_STEREO_CNTL` draws. | Not a stub. This is real GPU emulation and stays. |
| `e8c7a3d2` linear FSR | FSR-space layers are sampled linearly, because draws are rasterized without the foveation warp. | Not a stub. This stays. |

Still plain stubs (they return `OK` and do nothing): `sceHmd2ReprojectionBeginFrame`,
`SetRenderConfig`, `Enable/DisableMirroring`, `SetMirroringOption`, `DisableVrMode`,
`GetStatus`, `SetUserEvent*`, `SetVibration`, `Terminate`/`Close`, and all VrTracker2
coordinate-system and play-area calls. `sceHmd2ReprojectionGetPredictedDisplayTime` returns
`now + 1/120 s`.

The GPU side (stereo expansion, FSR handling, guest texture resolve, layer composition) is
reusable as is. What changes is the **source of poses and timing** and the **final sink**: an
OpenXR swapchain instead of an SDL window.

---

## 2. How the two models line up

PSVR2 and OpenXR share the same shape: the app renders with a predicted pose, then a compositor
reprojects to the real pose at scan-out. That makes the mapping fairly direct.

| Concept | PS5 | OpenXR |
|---|---|---|
| Device discovery | `sceHmd2Initialize`, `sceHmd2Open` | `xrCreateInstance`, `xrGetSystem(HEAD_MOUNTED_DISPLAY)` |
| Session / VR mode | `sceHmd2ReprojectionInitialize` + `EnableVrMode(outputMode)` | `xrCreateSession` (Vulkan binding) + `xrBeginSession(PRIMARY_STEREO)` |
| Frame identity | `flipArg` (game-chosen `int64`) | `XrFrameState` from `xrWaitFrame` |
| Frame pacing | HMD2 bus vblank (90 or 120 Hz), `BeginFrame` | `xrWaitFrame` |
| Predicted display time | `sceHmd2ReprojectionGetPredictedDisplayTime(flipArg)` | `XrFrameState::predictedDisplayTime` |
| Pose query | `sceVrTracker2GetResult(handle, predictionTime)` | `xrLocateSpace` / `xrLocateViews` at an `XrTime` |
| Eye poses (with canting) | `hmdInfo.leftEyePose/rightEyePose` | `XrView::pose` from `xrLocateViews` |
| Per-eye frustum | `SceHmd2FieldOfView` (tangents, one per device, mirrored per eye) | `XrFovf` (angles, per view) |
| Submission | `sceHmd2ReprojectionSetParam(index, flipArg, layers)` + `setFlip(hmdHandle, index, flipArg)` | `xrEndFrame` with `XrCompositionLayer*` |
| Render-pose hand-off | `SceHmd2ReprojectionSettingVr3d::pose` | `XrCompositionLayerProjectionView::pose` |
| Reprojection / distortion | Firmware reprojection thread at every HMD vblank | The runtime compositor |
| Reference spaces | `LOCAL_FLOOR`, `LOCAL`, `STAGE` | `LOCAL_FLOOR` (1.1 / `XR_EXT_local_floor`), `LOCAL`, `STAGE` |
| Recenter | `sceVrTracker2ResetLocalCoordinate` | Recreate the reference space with an offset pose |
| Play area | Polygon + OBB, room-scale or stationary | `xrGetReferenceSpaceBoundsRect(STAGE)` (rectangle only) |
| Controllers | `scePadOpen(VR_CONTROLLER_LEFT/RIGHT)` + `VrTracker2` pose | Action set, `/user/hand/{left,right}`, grip pose |
| Eye tracking | `sceHmd2GazeGetResult` | `XR_EXT_eye_gaze_interaction` |

Both use a right-handed, Y-up, −Z-forward coordinate system in metres. The tracker samples
already assume this (the floor is `y = 0` in `LOCAL_FLOOR`/`STAGE`). **Positions and
quaternions copy across without axis swaps.** Both use the `x, y, z, w` quaternion order.

---

## 3. Architecture

```
 guest threads                         host
 ─────────────                         ────
 sceHmd2ReprojectionBeginFrame ──┐
 sceHmd2...GetPredictedDisplayTime ─┼──► XrFrameClock (latest XrFrameState, period, time sync)
 sceVrTracker2GetResult ─────────┘        │
 scePadVrControllerReadState ────────► XrInput (action sync, cached per xrSyncActions)
                                          │
 sceHmd2ReprojectionSetParam ───► Hmd2 FrameSettings[index]  (already exists; now keeps pose+fov)
 GPU setFlip(hmdHandle, index, flipArg)
        │
        ▼
 VideoOut HMD2 bus flip queue ──► XrFrameThread ──────────────────────────────────────────┐
                                   loop:                                                 │
                                     xrWaitFrame → signal HMD2 vblank events             │
                                     xrBeginFrame                                        │
                                     take newest completed flip (or reuse previous)      │
                                     HmdCompositor → XR swapchain images                 │
                                     xrEndFrame(projection [+ quads], pose/fov from      │
                                                SetParam of that flip)                   │
                                     complete guest flip (flip events, GetFlipStatus)    │
                                     update SceHmd2ReprojectionStatus                    │
                                   ◄─────────────────────────────────────────────────────┘
```

### New module: `src/graphics/presentation/xr/`

- **`xrRuntime.{h,cpp}`** owns `XrInstance`, `XrSystemId`, `XrSession`, the reference spaces,
  the event poll, and the session state machine. It is created before Vulkan, because
  `XR_KHR_vulkan_enable2` must create the `VkInstance` and `VkDevice` (see §6).
- **`xrFrameThread.{h,cpp}`** runs the `xrWaitFrame` → `xrBeginFrame` → `xrEndFrame` loop. It is
  the HMD2 bus's scan-out clock and replaces the second SDL window as the bus's sink.
- **`xrClock.{h,cpp}`** converts between guest process time (µs, from
  `LibKernel::KernelGetProcessTime`) and `XrTime` (ns).
- **`xrInput.{h,cpp}`** holds the action set, suggested bindings, haptics, and per-hand cached
  state.

The `libHmd2.cpp`, `libVrTracker2.cpp`, `controller.cpp`, and `videoOut.cpp` HLE code keeps the
guest ABI structs. It calls into these host classes instead of filling in constants.

### Why a dedicated frame thread rather than calling `xrWaitFrame` from `BeginFrame`

PS5 reprojection is decoupled from the game. The reprojection thread runs at every HMD vblank
and reprojects the newest flipped buffer, even if the game misses frames or runs in
`RENDER60_AT_REPROJECTION120` mode. A dedicated OpenXR frame thread reproduces that exactly:

- It always calls `xrEndFrame` at the runtime rate. Runtimes punish apps that miss frames
  (SteamVR shows its grey loading void; Monado drops the session's priority).
- `xrBeginFrame` for frame N+1 implicitly discards frame N if `xrEndFrame` was not called. That
  would happen if the guest pipelines `BeginFrame` ahead of its flips, so begin and end must
  live on one thread that the guest cannot reorder.
- A guest that calls `BeginFrame` without flipping (loading screens) cannot deadlock the
  runtime.
- 60→120 Hz mode needs nothing extra: the thread simply submits the same flip twice.

Swapchain images cannot be resubmitted without an acquire. The thread therefore composites each
new guest flip once into a persistent **panel image** (what `PrepareHmdFrame` already produces,
one array layer per eye). Every XR frame then copies that image into the acquired swapchain
image. The copy is about 2×2000×2040 pixels, which is negligible.

---

## 4. API mapping

Legend: **Direct** means a 1:1 OpenXR call. **Derived** means computed from OpenXR data.
**Host** means handled entirely inside the emulator. **None** means no OpenXR equivalent, so we
keep a plausible fake.

### 4.1 `libSceHmd2` core (`hmd2.h`, `hmd2/distortion_correct.h`)

| Guest API | Mapping | Notes |
|---|---|---|
| `sceHmd2Initialize` | Host | Succeeds whether or not a runtime exists. The device status reports presence. |
| `sceHmd2Terminate` | Host | Clears the init flag. The XR instance lives for the whole emulator run. |
| `sceHmd2Open(userId, type, index)` | Derived | Return the single handle if `xrGetSystem` succeeded; otherwise `SCE_HMD2_ERROR_DEVICE_DISCONNECTED`. A second open returns `ERROR_ALREADY_OPENED`. |
| `sceHmd2Close` | Host | |
| `sceHmd2GetDeviceInformation[ByHandle]` → `status` | Derived from `XrSessionState` | `FOCUSED`/`VISIBLE`/`SYNCHRONIZED` → `READY`; `IDLE`/`READY`/`STOPPING` → `NOT_READY`; no system → `NOT_DETECTED`. |
| … `hmuMount` | Derived from `XR_EXT_user_presence` | `XrEventDataUserPresenceChangedEXT::isUserPresent` → `MOUNT`/`DISMOUNT`. If `XrSystemUserPresencePropertiesEXT::supportsUserPresence` is false or the extension is missing, always report `MOUNT`. The spec warns against inferring presence from session state, so session state is **not** used here. |
| … `panelResolution` | Derived | `2 × recommendedImageRectWidth` × `recommendedImageRectHeight` from `xrEnumerateViewConfigurationViews`. Games use this to size render targets (`tutorial_fr` does), so reporting the real host panel gives native-resolution rendering. A config option can force the PSVR2 4000×2040 for games that assume it. |
| … `flipToDisplayLatency` | Derived | Average `predictedDisplayTime − xrWaitFrame return time`, in µs, reported in both the 90 Hz and 120 Hz fields. |
| … `lensSeparationDistance` | Derived | Distance between the two `XrView::pose.position` values in `VIEW` space, in mm. |
| … `virtualImageDistance` | None | Keep 2.0 m. |
| `sceHmd2GetFieldOfView[WithoutHandle]` | ~~Derived from `xrLocateViews`~~ Host: PSVR2 ER15 constants | Reporting the runtime's FOV (§5.3) put Horizon's fixed lens mask in view, so the PSVR2 values are reported (§0). The runtime's FOV is logged. |
| `sceHmd2SetVibration(handle, 0..25 Hz)` | None | No headset haptics in OpenXR. Accept and log once. Optionally a config flag could forward it to both controllers at low amplitude. |

### 4.2 `libSceHmd2` reprojection (`hmd2/reprojection.h`)

| Guest API | Mapping | Notes |
|---|---|---|
| `QueryBufferSizeAlign` / `QueryDisplayBufferSizeAlign` | Host | Unchanged. The guest memory is a token; all real state lives on the host. |
| `sceHmd2ReprojectionInitialize` | Host | Records the border type (`BORDER_TEXEL` = clamp vs `NONE` = black) for the compositor sampler. `pipeId/queueId/threadPriority` are ignored. |
| `sceHmd2ReprojectionTerminate` | Host | Drops HMD2 state; the session stays alive. |
| `EnableVrMode(outputMode)` | Derived | Requires `XrSession` to exist and be at least `READY`; otherwise `VIDEO_OUT_ERROR_NO_DEVICE`. Requests the refresh rate (`HMD2_89_91HZ` → ~90, `HMD2_119_88HZ` → ~120) via `xrRequestDisplayRefreshRateFB` when `XR_FB_display_refresh_rate` exists. The request must be `0.0f` or exactly one of the values from `xrEnumerateDisplayRefreshRatesFB` (otherwise `XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB`), so pick the nearest enumerated rate. Otherwise the runtime rate stands; the guest is told the real rate through vblank timing. |
| `DisableVrMode` | Host | Stops accepting HMD2 flips. The frame thread keeps submitting empty frames (zero layers), which is valid OpenXR. |
| `SetRenderConfig(syncMode, phase)` | Host | `RENDER60_AT_REPROJECTION120`: guest-facing HMD2 vblanks fire every second XR frame, and predicted display time is two periods ahead. `phase` is ignored (there is no HDMI coupling on the host). |
| `SetTiming(2000..6000 µs)` | None | The runtime picks when it reprojects. Accept and store. |
| `BeginFrame(flipArg)` | Derived | Non-blocking. Bind `flipArg` to the next XR display slot: `slot = lastPredicted + k·period`, the first one later than `now + render budget`. Track up to N outstanding to emulate `TOO_MANY_CALL_BEGIN_FRAME`. |
| `GetPredictedDisplayTime(flipArg)` | Derived | Look up the slot bound to `flipArg` and convert with `XrClock` to guest µs. An unknown `flipArg` returns `ERROR_REPROJECTION_INVALID_FLIPARG`. The value then feeds `sceVrTracker2GetResult::predictionTime`. |
| `SetParam(index, flipArg, layers)` | Host → consumed at `xrEndFrame` | Already stored per display buffer. **Extend `LayerSetting`** to keep `fovL/fovR`, `pose`, `disableReprojection`, UI2D `width/height/position/rotation`, the sampler wrap mode, and the blend space. Layer mapping is in §4.3. |
| `SetUserEventStart/End` + `Clear…` | Host | Trigger the guest equeue user event when the frame thread starts and finishes compositing a frame. Some games time CPU work off these. |
| `GetStatus` | Derived | Fill from frame-thread timestamps (converted to the process time counter): `ptcBeginFrame`/`ptcSetParam` from the HLE calls, `ptcRequestFlip` at guest `setFlip`, `ptcAccept` at the frame thread's pick-up, `ptcReprojectionStart/End` around the composite plus `xrEndFrame`, `ptcDisplayPredicted` from `XrFrameState`, `ptcLastVblank` at `xrWaitFrame` return. `reprojectionCount` counts submitted XR frames. |
| `GetMirroringWorkMemorySizeAlign` / `…DisplayBufferSizeAlign` | Host | Unchanged. |
| `EnableMirroring` / `DisableMirroring` | Host | Mirroring needs `BUS_MAIN` closed, so the main window is free. Show the left eye of the panel image there. |
| `SetMirroringOption(area)` | Host | `WIDE`: the whole eye. `CENTER`: a centre crop at the TV aspect ratio. |
| `GetCurrentOutput` (nosubmission) | Host | Debug only. Return the panel image texture, or keep the stub. |

### 4.3 Reprojection layers → composition layers

`SceHmd2ReprojectionSource` (registered through `sceVideoOutRegisterBuffers2` with
`CATEGORY_HMD2`) supplies textures. `SceHmd2ReprojectionSetting` (per flip) supplies placement.
At most 5 layers.

| PS5 layer | OpenXR layer | Space | Pose / geometry |
|---|---|---|---|
| `VR3D`, layer 0 | `XrCompositionLayerProjection`, 2 views | Space of the guest's coordinate system when it called `GetResult` (§5.1) | `views[i].pose = devicePose(SetParam) ∘ eyeInHead[i]`, `views[i].fov` = `fovL/fovR` from SetParam (§5.3), `subImage` = the eye's rect in the XR swapchain. |
| `VR3D`, `disableReprojection = true` | Projection layer | `VIEW` | Eye poses = `eyeInHead[i]`. This makes the layer head-locked, i.e. "distortion only". |
| `VR3D`, layers 1..4 | Flattened into the same projection layer by `HmdCompositor` | | They share layer 0's pose. That matches PS5, where all VR3D sources go through one reprojection. |
| `UI2D` | `XrCompositionLayerQuad` with its own swapchain | `VIEW` ("position from hmdDevice origin", fixed in the view) | `pose = {rotation, position}`, `size = {width, height}`, `subImage` = viewport. |
| `VR2D` | `XrCompositionLayerQuad` | `LOCAL` | It has no placement fields, so this is presumably a system-placed virtual screen. Put a 16:9 quad 2 m ahead of the local origin. **Unverified**; check against a title that uses it. |

**`subImage` and the guest viewport.** The guest viewport (`viewportL/R`) selects the region of
the source texture the eye was rendered into. `HmdCompositor` already copies exactly that region
using `Draw::region`. The swapchain should be sized to the guest viewport rather than the
runtime recommendation, to avoid resampling twice; it is recreated when the guest size changes
and capped at `maxImageRect`. The runtime does the only resample, during distortion.

**Blend types.** OpenXR composites layers in array order:

| `SceHmd2ReprojectionLayerBlendType` | OpenXR `layerFlags` | Compositor-side treatment |
|---|---|---|
| `LAYER0` | none (opaque) | Write alpha = 1. |
| `INTERPOLATED_ALPHA` | `BLEND_TEXTURE_SOURCE_ALPHA_BIT \| UNPREMULTIPLIED_ALPHA_BIT` | None. |
| `PREMULTIPLIED_ALPHA` | `BLEND_TEXTURE_SOURCE_ALPHA_BIT` | None. |
| `SIMPLE_ADD` | `BLEND_TEXTURE_SOURCE_ALPHA_BIT` | Write alpha = 0 when copying into the quad swapchain. Premultiplied blending with α = 0 is exactly `dst = over + under`. |

**Blend space.** OpenXR runtimes blend in linear light. The PS5 default is
`MULTILAYER_BLEND_GAMMA_SPACE`, with `LINEAR_SPACE_AND_GAMMA_22/SRGB` as options. A UI quad
blended over the scene in a native OpenXR layer is therefore blended in linear light, whereas
PS5 blends in gamma. This only affects semi-transparent edges, so native quads are acceptable.
A config option can flatten UI2D into the projection layer for exact gamma blending, at the
cost of the UI being reprojected with the scene.

**Colour.** The guest panel format is `k10_10_10_2UNorm` with sRGB colorimetry, i.e.
gamma-encoded values in a UNORM container. OpenXR treats `*_SRGB` swapchain formats as sRGB and
`*_UNORM` formats as **linear**. Choose `VK_FORMAT_R8G8B8A8_SRGB` (or `B8G8R8A8_SRGB`), and have
`gpu_hmd_compose.frag` decode sRGB to linear before writing through the sRGB view. Do **not**
`vkCmdBlitImage` from the guest UNORM image into the sRGB swapchain: the blit treats the
gamma-encoded values as linear and encodes them again, which washes the image out.
`lumaScaleFromSdrToHdr` is ignored while host headsets are driven as SDR.

**FSR / foveated sources.** These stay as in `e8c7a3d2`: draws are rasterized without the FSR
warp, so `enableFsrReprojection` sources are already linear and the LUTs go unused.

### 4.4 Video-out HMD2 bus (`video_out/hmd2_sub.h`)

| Guest API | Mapping |
|---|---|
| `sceVideoOutOpen(SYSTEM, BUS_TYPE_HMD2_MAIN)` | Succeeds when an `XrSystemId` exists (or in virtual-headset mode, §7). |
| `sceVideoOutConfigureOutput(handle, HMD2_89_91HZ / HMD2_119_88HZ)` | `xrRequestDisplayRefreshRateFB` with the nearest enumerated rate, when available. Always accepted. |
| `sceVideoOutRegisterBuffers2(..., CATEGORY_HMD2)` | Unchanged: parse `SceHmd2ReprojectionBuffer` sources. |
| `setFlip` / `sceVideoOutSubmitFlip` on the HMD2 handle | Enqueue onto the HMD2 bus flip queue. The **frame thread** completes the flip when it consumes it, instead of the main vblank loop. |
| `sceVideoOutAddVblankEvent` on the HMD2 handle | Fired by the frame thread at each `xrWaitFrame` return, or every second one in 60→120 mode. |
| `sceVideoOutGetVblankStatus` / `GetFlipStatus` on HMD2 | Counters and times from the frame thread; times converted with `XrClock`. |
| `SCE_VIDEO_OUT_REFRESH_RATE_89_91HZ` (35) | Report it when the runtime rate is about 90 Hz, and `119_88HZ` when it is about 120 Hz. |

`FlipQueue` serves both clocks: the main present thread calls `Flip(0, false)`, and with an
OpenXR headset the frame thread calls `Flip(0, true)` for the HMD2 port (see §0).

### 4.5 `libSceVrTracker2` (`vision/vr_tracker2.h`)

| Guest API | Mapping | Notes |
|---|---|---|
| `QueryMemory`, `Initialize`, `Finalize` | Host | Unchanged. `coordinateResetType` (head vs device) is stored for `ResetLocalCoordinate`. |
| `RegisterDevice`/`UnregisterDevice` | Host | Unchanged registry. The HMD must be the open Hmd2 handle; controllers must be the VR pad handles. |
| `GetResult` — HMD | **Direct**: `xrLocateViews(PRIMARY_STEREO, predictedTime, baseSpace)` plus `xrLocateSpace(viewSpace, baseSpace, predictedTime)` | `devicePose` = `VIEW` in base. `leftEyePose/rightEyePose` = `XrView[0/1].pose`. `headPose` = `devicePose` offset by a fixed +0.09 m Z / −0.03 m Y in device space (centre of the skull, behind the eyes; an estimate). `gazeOriginPose` = midpoint of the eye positions with device orientation, or the eye-gaze pose position if available. |
| `GetResult` — controller | **Direct**: `xrLocateSpace(gripSpace[hand], baseSpace, t)` | `devicePose` = grip pose ∘ per-profile offset (§5.4). `lastValidVisionTimestamp`/`lastValidSensorTimestamp` = last time the position/orientation `TRACKED` bit was set. |
| `GetResult` — `timestamp` | Derived | The `predictionTime` passed in (already guest µs), or "now". |
| `GetResult` — `status` | Derived | `XR_SPACE_LOCATION_*_VALID` set → `TRACKING`; else `NOT_TRACKING`; session not running → `NOT_STARTED`. |
| `GetResult` — `positionQuality` / `orientationQuality` | Derived from location flags | `VALID + TRACKED` → `FULL`; `VALID` only → `PARTIAL` (it was tracked recently) or `NOT_VISIBLE` once untracked for more than 500 ms; not valid → `NONE`. |
| `GetResult` — `connected` | Derived | HMD: system present. Controller: the grip pose action `isActive`. |
| `GetResult` — `playAreaMode` | Derived | `ROOM_SCALE` if `xrGetReferenceSpaceBoundsRect(STAGE)` returns a non-zero extent, else `STATIONARY`. |
| `GetResult` — `playAreaUpdateCounter` | Derived | Incremented on `XrEventDataReferenceSpaceChangePending` and on each guest `ResetLocalCoordinate`. |
| `GetResult` — `flipArg` | Host | Echo it back. Also record `{flipArg → base-space generation}` so the projection layer is submitted in the space the frame was rendered in. |
| `SetPoseToReprojection` | Host | This is a `static inline` in the header; implement it the same way (copy `devicePose`, `flipArg`, `timestamp`). |
| `GetCoordinateSystem`/`SetCoordinateSystem` | Host → selects base space | `LOCAL_FLOOR` → `XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR`. Every OpenXR 1.1 runtime must support it; on 1.0 it needs `XR_EXT_local_floor`. Fallbacks, only for 1.0 runtimes without the extension, in order: `STAGE` re-anchored at the LOCAL origin's x/z and yaw; then `LOCAL` lifted by `−floorHeight`, where `floorHeight` comes from `STAGE` if it exists or is configured (default 1.6 m). `LOCAL` → `LOCAL`. `STAGE` → `STAGE`. Default is `LOCAL_FLOOR`. |
| `ResetLocalCoordinate(hmdHandle)` | Derived | Locate `VIEW` in the current un-offset `LOCAL` at "now", keep position + yaw (and drop the height for `LOCAL_FLOOR`), and recreate `LOCAL`/`LOCAL_FLOOR` with that as `poseInReferenceSpace`. With `coordinateResetType = device`, use the device pose instead of the head pose. On `STAGE`, return `ERROR_NOT_AVAILABLE_IN_CURRENT_PLAY_AREA_MODE`, as the sample expects. |
| `ResetLocalCoordinateWithPose(pose)` | Derived | Same, with the given pose (yaw only). |
| `LocateCoordinateSystem(type, *pose)` | **Direct**: `xrLocateSpace(space(type), currentBase, now)` | The sample uses this to draw the STAGE floor while in LOCAL. |
| `GetPlayAreaOrientedBoundingBox` | Derived | `pose` = `STAGE` origin located in current base. `scale = {w, ~2.5, d}` from `xrGetReferenceSpaceBoundsRect`. |
| `GetPlayAreaBoundaryGeometry` | Derived | The 4 corners of the bounds rectangle in current base (core OpenXR exposes only a rectangle). |
| `CheckPointIsInsidePlayAreaBoundary` | Host | Point-in-rectangle test in STAGE space. Returns closest point, normal and distance. `UNKNOWN` if there are no bounds. |
| `CheckDeviceIsInsidePlayAreaBoundary` | Host | Locate the device, then the same test. |

### 4.6 VR controllers (`pad_vr_controller.h`, `pad/vr_controller_trigger_effect.h`)

Action set `"psvr2"` with these actions, sub-action paths `/user/hand/left` and
`/user/hand/right`:

| Action | Type | `ScePadVrControllerData` field |
|---|---|---|
| `grip_pose` | pose | Tracker pose (§4.5); also orientation and IMU (below) |
| `face_upper` (left: triangle, right: circle) | boolean | `BUTTON_TRIANGLE` / `BUTTON_CIRCLE` |
| `face_lower` (left: square, right: cross) | boolean | `BUTTON_SQUARE` / `BUTTON_CROSS` |
| `face_upper_touch`, `face_lower_touch` | boolean | `fingerTouchDetection` TRIANGLE/SQUARE/CIRCLE/CROSS |
| `menu` | boolean | `BUTTON_OPTIONS` (right controller). On Touch, `menu/click` exists only on the **left** hand and the right `system/click` may be reserved by the runtime, so bind left `menu/click` and report it on the right Sense controller. |
| `trigger` | float | `analogButtons.l2/r2` (0–255); `BUTTON_L2/R2` when above 0.5 |
| `trigger_touch` | boolean | `fingerTouchDetection` L2/R2; `analogFingerTouchDetection.l2/r2` = 255 when touched (no proximity analog in OpenXR) |
| `grip` | float | `BUTTON_L1/R1` when above 0.5 (on Sense, L1/R1 is the grip button) |
| `grip_touch` | boolean | `fingerTouchDetection` L1/R1 (Index `squeeze/value > 0` as a proxy, since Touch has no squeeze touch) |
| `stick` | vector2 | `analogStick` (map −1..1 → 0..255, Y inverted: PS up = 0) |
| `stick_click` | boolean | `BUTTON_L3/R3` |
| `stick_touch` | boolean | `fingerTouchDetection` L3/R3 |
| `haptic` | vibration output | `scePadSetVibration` on VR handles, trigger-effect vibration modes |

Suggested bindings: `/interaction_profiles/oculus/touch_controller` (closest layout: X/Y ↔
square/triangle, A/B ↔ cross/circle), `/valve/index_controller`, `/htc/vive_controller`,
`/microsoft/motion_controller`, `/khr/generic_controller` (OpenXR 1.1; primary/secondary
buttons, thumbstick, trigger, squeeze), `/khr/simple_controller` (pose + select + menu only).
The 1.1 spec defines no Sony interaction profile; SteamVR and Monado rebind to these. Index
has no `squeeze/touch`, and Touch has no squeeze touch either.

Other fields and calls:

- **`connected`, `connectedCount`.** `connected` = the grip pose action `isActive`.
  `connectedCount` increments on each inactive→active edge.
- **`BUTTON_INTERCEPTED` (0x80000000).** Set while the session is not `FOCUSED` (runtime
  dashboard open), with all inputs zeroed. This is the PS5 semantic for "the system has the
  controllers".
- **`orientation`.** Grip orientation, relative to the reference captured at connect or at
  `scePadResetOrientation`.
- **`angularVelocity`.** `XrSpaceVelocity::angularVelocity` (base space, rad/s), rotated into
  the controller frame.
- **`acceleration` (G).** OpenXR has no accelerometer. Compute it as
  `(Δ linearVelocity / Δt − g_world)` rotated into the controller frame, divided by 9.81. At rest
  this reads +1 G "up", like a real IMU. The PS pad sensor axis convention must be checked
  against `api_pad_mouse/vr_controller_basic`.
- **`timestamp`.** The `xrSyncActions` time in guest µs.
- **`scePadVrControllerRead` (history), `ReadCombinedState`.** Not bound yet. Build them from the
  same cached state; history is a ring filled once per `xrSyncActions`. NIDs still need to be
  computed.
- **`scePadVrControllerSetTriggerEffect[s]`.** No adaptive triggers in OpenXR. Map the
  `VIBRATION`/`MULTI_POSITION_VIBRATION` modes to `xrApplyHapticFeedback`, ignore the rest, and
  keep `GetTriggerEffectState` returning idle.
- **Haptics.** `xrApplyHapticFeedback(XrHapticVibration{duration = 50 ms, frequency =
  XR_FREQUENCY_UNSPECIFIED, amplitude = motor / 255})`, renewed on every guest call. A motor
  value of 0 → `xrStopHapticFeedback`.

`xrSyncActions` runs once per XR frame on the frame thread. The pad HLE reads the cached
snapshot, so no OpenXR calls are made on guest threads apart from `xrLocate*`. The spec puts
no external-synchronization requirement on `xrLocateSpace`/`xrLocateViews` (only on
`xrDestroySpace`), so they are safe to call from guest threads. Spaces retired by a recentre
must not be destroyed while a guest thread may still be locating them.

### 4.7 Gaze (`hmd2/gaze.h`)

| Guest API | Mapping |
|---|---|
| `sceHmd2GazeGetResult` | With `XR_EXT_eye_gaze_interaction` (check `XrSystemEyeGazeInteractionPropertiesEXT`): locate `/user/eyes_ext/input/gaze_ext/pose` **relative to the gaze origin pose**. `gazeDirection` = that pose's −Z axis in that frame. `tutorial_fr` transforms `gazeDirection` by `gazeOriginPose`, so the direction is local to it. `isGazeValid` = `ORIENTATION_TRACKED`. `timestamp` = `XrEyeGazeSampleTimeEXT::time`, chained to the `XrSpaceLocation`. Pupil diameter and blink: `isPupilDiameterValid = 0`, `isBlink = 0`. Without the extension: keep today's "valid, straight ahead". |
| `sceHmd2GazeGetResultForFoveatedRendering` | Same direction; `status = ENABLE`. |
| `status` | `NOT_FOCUSED` when the session is not `FOCUSED`. |

### 4.8 Out of scope / trivial

- **`libSceVrSetupDialog`.** Open → `RUNNING` → next update `FINISHED` with `RESULT_OK`. The host
  runtime owns room setup.
- **`SCE_AUDIO_OUT2_PORT_PARAM_FLAG_VR_CONTROLLER`.** Route to the default output, or drop it.
  There is no controller speaker on most host controllers.
- **`libSceFaceTracker`.** Out of scope.
- **`libSceHmd` (PSVR1).** PS4 back-compat; the stubs stay as they are.

---

## 5. Conversion details

### 5.1 Spaces and the "render space" of a frame

The guest renders frame `flipArg` using poses from `GetResult(flipArg)` in whatever coordinate
system was current at that call, including any reset offset. If the guest recentres between
render and submit, the projection layer must still be submitted in the **old** space.
Implementation:

- Keep every `XrSpace` created for a reset alive for a few frames.
- Tag each `GetResult` with `(flipArg → spaceGeneration)`.
- In `xrEndFrame`, use the space of the generation recorded for the flip's `flipArg`, falling
  back to the current one.

### 5.2 Time

- Guest time is `KernelGetProcessTime()` (µs since process start, TSC based).
- Linux: `XR_KHR_convert_timespec_time`; `xrConvertTimespecTimeToTimeKHR(CLOCK_MONOTONIC now)`.
  Windows: `XR_KHR_win32_convert_performance_counter_time`.
- `XrClock` samples `(processTimeUs, XrTime)` pairs back to back, with minimum-RTT filtering,
  once a second. Conversion in either direction is `offset + Δ`.
- Every guest-visible timestamp (`predictionTime`, tracker `timestamp`, gaze `timestamp`, status
  `ptc*`, vblank times) goes through `XrClock`.

### 5.3 Field of view

`SceHmd2FieldOfView` is a single, mirrored, positive-tangent description:

```
left eye:   tanOut = tan(-fov[L].angleLeft)   tanIn = tan(fov[L].angleRight)
right eye:  tanIn  = tan(-fov[R].angleLeft)   tanOut = tan(fov[R].angleRight)
both:       tanTop = tan(fov.angleUp)         tanBottom = tan(-fov.angleDown)
```

The sample builds the projection matrices exactly this way (`frustumReverseInfiniteZ(-tanOut,
tanIn, tanTop, -tanBottom)` for the left eye, mirrored for the right). If the host runtime's
eyes are not mirror images, report the per-component **maximum** of both eyes so the guest
over-renders slightly rather than clipping.

On submit, convert the `fovL/fovR` the guest passed in `SetParam` back to angles:
`angleLeft = -atan(tanOut)`, `angleRight = atan(tanIn)` for the left eye, and swapped for the
right eye (the sample sets `fovR.tanIn = tanOut`). The runtime then reprojects exactly the
frustum the game rendered.

### 5.4 Eye-in-head and controller offsets

- `eyeInHead[i]` = `xrLocateViews` in `VIEW` space. It is effectively constant (IPD, canting),
  so refresh it every frame and use the latest value.
- Projection view pose = `pose(SetParam.vr3d.pose) ∘ eyeInHead[i]`. The guest passes `devicePose`
  back unchanged, so this reproduces the eye poses it rendered with, without a lookup table.
- Controller `devicePose` = `gripPose ∘ offset[profile]`. The Sense controller "device origin" is
  not documented. Start from identity and calibrate with `tutorial_hand_interaction_of_vr_controller`
  (the rendered hand should sit in the real hand), then keep the result per interaction profile.

---

## 6. Vulkan integration (`XR_KHR_vulkan_enable2`)

`WindowContext::CreateVulkan` (`src/graphics/presentation/window/vulkanWindow.cpp`) changes
when an OpenXR system is present:

1. `xrCreateInstance` (with the extensions in §8) and `xrGetSystem` **before** Vulkan.
2. `xrGetVulkanGraphicsRequirements2KHR`: check that `VULKAN_TARGET_API_VERSION` (1.3) is in range.
3. `vk::createInstance` → `xrCreateVulkanInstanceKHR`, passing the same `VkInstanceCreateInfo`
   (SDL extensions, debug utils, validation features). The runtime appends its own extensions.
4. `xrGetVulkanGraphicsDevice2KHR` → **this physical device is mandatory**.
   `VulkanFindPhysicalDevice` must be limited to it, and must fail clearly if that device lacks
   our required extensions.
5. `physical_device.createDevice` → `xrCreateVulkanDeviceKHR`, with the same create info.
6. `xrCreateSession` with `XrGraphicsBindingVulkan2KHR {instance, physicalDevice, device,
   queue_family, queueIndex = 0}`.
7. Per the spec, the runtime may access that queue only in `xrBeginFrame`, `xrEndFrame`,
   `xrAcquireSwapchainImage`, and `xrReleaseSwapchainImage`. Hold `graphic_ctx.queue_mutex`
   around exactly those four. Do **not** hold it across `xrWaitSwapchainImage` or
   `xrWaitFrame`: neither touches the queue, and both block, which would stall guest GPU
   submission.
8. Swapchain images arrive as `XrSwapchainImageVulkan2KHR` `VkImage`s. Transition them to
   `COLOR_ATTACHMENT_OPTIMAL` for `HmdCompositor`, and back to the layout the runtime expects
   (`COLOR_ATTACHMENT_OPTIMAL`) before release.

Swapchains:

- **Projection.** One swapchain, `arraySize = 2` (one layer per eye), format sRGB (§4.3). Usage
  `COLOR_ATTACHMENT | SAMPLED | TRANSFER_DST`.
- **Quad.** One swapchain per active UI2D/VR2D source, created lazily and sized to the viewport.
- `HmdCompositor::Record` gains a target layer index (it currently renders one side-by-side
  2D image), and a mode that writes per eye into a 2-layer image.

The SDL headset window from `8818d606` becomes an optional desktop preview of the panel image.
It is no longer the bus's sink.

---

## 7. Configuration and fallback

Replace the boolean `vr_enabled` with `vr_mode = off | virtual | openxr`:

- **`off`.** As today with VR disabled: `NOT_DETECTED`, HMD2 bus open fails.
- **`virtual`.** Today's behaviour (fixed poses, SDL window), kept for headless debugging and CI.
  Also useful for games that never reach a running session.
- **`openxr`.** This document. If `xrCreateInstance`/`xrGetSystem` fails at start-up, log the
  reason and fall back to `virtual` (or `off`, via config).

Additional options: `vr_force_psvr2_panel` (report 4000×2040), `vr_floor_height`,
`vr_controller_offset_<profile>`, `vr_ui_flatten` (gamma-exact UI2D blending).

---

## 8. OpenXR extensions

| Extension | Need | Used for |
|---|---|---|
| `XR_KHR_vulkan_enable2` | **Required** | Vulkan instance/device creation, session binding, swapchains |
| `XR_KHR_convert_timespec_time` (Linux) / `XR_KHR_win32_convert_performance_counter_time` (Windows) | **Required** | Guest ↔ `XrTime` |
| `XR_EXT_local_floor` | Only on 1.0 runtimes | `LOCAL_FLOOR` (core and mandatory in 1.1) |
| `XR_EXT_user_presence` | Recommended | `hmuMount` |
| `XR_FB_display_refresh_rate` | Optional | 90/120 Hz output modes |
| `XR_EXT_eye_gaze_interaction` | Optional | `sceHmd2Gaze*` |
| `XR_EXT_debug_utils` | Optional | Runtime messages into our log |

Interaction profiles are core and need no extension. The loader is `openxr-loader`: add it as a
submodule in `3rdparty/` (KhronosGroup/OpenXR-SDK, building only the loader) or as a system
package, and add `openxr-loader` to `nix/devshell.nix`.

---

## 9. Implementation plan

Each phase can be tested on its own. Monado (`monado-service` with the `qwerty` or simulated
driver) gives a headset-free test loop on Linux; SteamVR null driver is the alternative.

Status: 1–4 done (with the deviations in §0); 5 and 6 open. The
*done when* checks were made with Horizon Call of the Mountain on Monado, not with the SDK
samples named below.

1. **[done] Runtime bring-up.** Add the OpenXR loader, `XrRuntime`, the `vr_mode` config, the
   `vulkan_enable2` path in `CreateVulkan`, session state machine and event polling. Run the
   frame thread with empty frames.
   *Done when:* the runtime shows the app as running with the session `FOCUSED`, and nothing
   regresses with `vr_mode = off`.
2. **[done] Clock and tracking.** Add `XrClock`, the HMD `GetResult`, and coordinate systems and reset.
   `Hmd2GetDeviceInformation` and `GetFieldOfView` use real data. `BeginFrame` and
   `GetPredictedDisplayTime` use frame-thread slots.
   *Done when:* in `api_vr_tracker2`, the logged poses follow the headset, and LOCAL/STAGE
   switching and reset work.
3. **[done] Display.** Give the HMD2 bus its own flip queue and the frame thread as its
   clock. Make the panel image per-eye layered; add the sRGB swapchain and per-flip VR3D
   projection layers with pose and FOV from `SetParam`. Fire HMD2 vblank and flip events from the
   frame thread.
   *Done when:* `tutorial_vr2/01-basic-vr` renders correctly in the headset with stable
   world-locked geometry. Rotating your head after a stalled frame should show runtime
   reprojection rather than swimming.
4. **[done] Controllers.** Add the action set, bindings, controller `GetResult`, `ReadState`, IMU
   derivation, haptics, `BUTTON_INTERCEPTED`, and the controller-offset calibration
   (calibration skipped, see §0).
   *Done when:* the hands in `tutorial_hand_interaction_of_vr_controller` track and the buttons
   work.
5. **Layers and extras.** Add UI2D/VR2D quads, blend flags, mirroring, `GetStatus`, user events,
   60→120 sync mode, refresh-rate requests, gaze, play-area queries (done with phase 2), the
   setup dialog, and the missing pad NIDs.
6. **Retire the virtual path's coupling.** The virtual mode keeps working through the same
   interfaces, through an `XrRuntime`-shaped fake. Drop the special cases in `videoOut.cpp` and
   `swapchain.cpp`.

---

## 10. Risks and open questions

- **Panel resolution vs fixed-size games.** Some titles may hard-code PSVR2 sizes or FSR curve
  tables tuned to its lenses (`SCE_HMD2_FSR_CURVE_LEFT/RIGHT`). `vr_force_psvr2_panel` covers
  sizing. FSR stays unwarped (`e8c7a3d2`), so curve mismatch costs only performance, never
  image correctness.
- **FOV mismatch.** Games are rendered with the PSVR2 FOV (§0), which suits fixed lens masks but
  renders some pixels a smaller host FOV never shows, and leaves the edges of a wider host FOV
  black.
- **VR2D semantics.** These are inferred from the struct shape only (§4.3). Verify on a title.
- **`flipArg` reuse.** Games use frame counters, so values are unique in practice. Slot and
  space-generation maps should still be bounded rings, not unbounded maps.
- **Sense controller origin and IMU axes.** Undocumented; calibrate (§5.4, §4.6).
- **Queue contention.** The runtime submitting on our single graphics queue while the guest
  renders could add latency. If profiling shows it, a second queue from the same family,
  dedicated to OpenXR, is the fix (pass `queueIndex = 1`).
- **SteamVR on Linux** wants `VK_KHR_external_memory_fd`-capable drivers and sometimes rejects
  validation layers in `xrCreateVulkanInstanceKHR`. Test both Monado and SteamVR.
- **Headset audio.** The PS5 routes headset audio through its own port (not covered by these
  headers); on the host the user picks the headset as the default sink.

---

## 11. Picking up

### Code map

| File | Role |
|---|---|
| `src/graphics/presentation/xr/xrRuntime.h` | Public, OpenXR-free API used by HLE and the presenter. Stubs exist for builds without the loader. |
| `xr/xrInternal.h` | `Runtime` class: instance, session, spaces, clock, panel, display slots. Only compiled with `KYTY_OPENXR`. |
| `xr/xrRuntime.cpp` | Instance/system/session, events, panel copy, public wrappers. |
| `xr/xrFrameThread.cpp` | `xrWaitFrame`/`BeginFrame`/`EndFrame` loop, swapchain, per-frame copy, projection layer, action sync, clock recalibration. |
| `xr/xrTracking.cpp` | Reference spaces, recentre generations, head/controller locate, play area. |
| `xr/xrClock.cpp` | Guest µs ↔ `XrTime`, display slots for `flipArg`, cached eye views. |
| `xr/xrInput.cpp` | Action set, bindings, per-frame controller state, IMU estimate, haptics. |
| `xr/xrMath.cpp` | Pose and quaternion helpers. |
| `libs/libHmd2.cpp`, `libs/libVrTracker2.cpp`, `libs/controller.cpp`, `libs/libPad.cpp` | HLE; each branches on `Graphics::Xr::IsActive()` and keeps the virtual-headset path. |
| `graphics/presentation/window/swapchain.cpp` (`PresentHmd`) | Hands panels to OpenXR instead of the SDL window. |
| `graphics/presentation/videoOut.cpp` (`HeadsetVblank`, `FlipQueue::Flip`) | HMD2 port vblanks and flips on the frame thread. |

Build: `KYTY_ENABLE_OPENXR` (default on) finds the system `OpenXR` CMake package and defines
`KYTY_OPENXR`. Without it everything compiles to stubs and `--vr-mode openxr` falls back.
`--vr` still means `--vr-mode virtual`.

### Testing with Monado on this machine

- The active OpenXR runtime is SteamVR, so point the loader at Monado:
  `XR_RUNTIME_JSON=/usr/share/openxr/1/openxr_monado.json`.
- `monado-service` must be running. When it is not attached to a terminal it needs
  `XRT_NO_STDIN=1`, or it fails with `epoll_ctl(stdin) failed`. Closing its window ends the
  service.
- Without hardware it uses the "Simulated HMD" (sways on its own, no controllers).
  `QWERTY_ENABLE=1 XRT_DEBUG_GUI=1 monado-service` gives a keyboard/mouse HMD and two
  controllers instead. Input only goes through while Monado's debug window has focus: hold
  Ctrl for the left controller, Alt for the right, neither for the head; WASD/QE move, the arrow
  keys or a right-mouse drag rotate, left click is the trigger, middle click the grip.
- Qwerty controllers are WMR controllers inside Monado, so they bind through
  `microsoft/motion_controller` (trigger, squeeze, menu, thumbstick, trackpad).
- Logs need `--printf-direction File` or `Console`; `Silent` (the launcher's default) hides
  everything, including the `OpenXR:` lines.
- `hello_xr -g Vulkan2` is installed and is a quick check that the runtime itself works.
- Monado source is in `~/.cache/paru/clone/monado/src/monado-v25.1.0`.
  `src/xrt/auxiliary/bindings/bindings.json` lists every interaction profile and its paths.
- PS5 SDK headers are in
  `~/Downloads/PS5_SDK_4_00_0_7/extracted/SDK-4_00_00_31-00_00_02_1_1/sdk/target/include`.

### Things learnt the hard way

- Monado 25.1's IPC client does not implement `xrGetReferenceSpaceBoundsRect`. It logs an
  error on every call, so the bounds are queried once per session and again only after
  `XrEventDataReferenceSpaceChangePending` (`Runtime::HasPlayAreaBounds`). Never query them per
  `GetResult`; the game is then told the play area is stationary.
- Monado has `XR_MNDX_psvr2_interaction`, but its `/interaction_profiles/mndx/psvr2_mndx`
  describes the headset (system button, haptics), not the Sense controllers. Real Sense
  controllers in Monado only expose `khr/simple_controller` (trigger, options, poses).
- Guest time is TSC-based (`KernelGetProcessTime`), not `CLOCK_MONOTONIC`. `CalibrateClock`
  measures the offset from back-to-back samples and the frame thread redoes it every second.
- Queue lock rule: `graphic_ctx.queue_mutex` is held around `xrBeginFrame`, `xrEndFrame`,
  `xrAcquireSwapchainImage`, `xrReleaseSwapchainImage` and our own submits, never around
  `xrWaitFrame` or `xrWaitSwapchainImage`.
- Panel synchronisation relies on single-queue submission order plus barriers. `PresentHmd`
  records the copy into the presenter's command buffer and calls `PanelCopySubmitted` only
  after submitting; until then the frame thread keeps the previous panel. The frame thread
  holds `Panel::mutex` while it records and submits its read, so the image cannot be replaced
  under it; replacing it waits for the queue to idle.
- Recentring creates a new generation of offset LOCAL/LOCAL_FLOOR spaces and keeps the last 16
  (`SPACE_GENERATIONS`), so frames rendered before a recentre are still submitted in their own
  space (§5.1). Guest-thread locates take `m_spaces_mutex`, which also makes destroying old
  spaces safe.
- The frame thread runs the guest's HMD2 flips, so it takes the renderer lock in `PresentHmd`
  (once per draw or dispatch at worst) and `Presenter::Impl::hmd_present_mutex`. It must not take
  `present_mutex`, because the main present holds that across the window's vsync. Lock order:
  renderer → `Panel::mutex` → `queue_mutex`.
- Monado's Qwerty HMD refreshes at 60 Hz, the same as the default `--vblank-frequency`. To see
  that the HMD2 bus follows the headset, run with `--vblank-frequency 30`: Horizon still flips the
  headset about 60 times a second.
- Horizon imports `sceHmd2ReprojectionSetAllowPositionalReprojection` (NID `uv0Ae+jCeWY`,
  library `Hmd2ReprojectionPositional`), which is newer than SDK 4.00. It takes one `int` (Horizon
  passes 2, just before `QueryBufferSizeAlign`), and Horizon ignores the return value. It only logs;
  the host runtime decides on positional reprojection.
- Horizon enables mirroring with `area = CENTER` and asks for 119.88 Hz.
- Horizon double-buffers the headset with two VR3D sources per buffer: source 0 is a 4000×2040
  side-by-side scene (layer 0, opaque), source 1 two 2000×2040 per-eye textures blended over it
  (`INTERPOLATED_ALPHA`, likely UI and hands). It renders nothing separate for the TV: no unused
  source, and the main port is flipped once at start-up. The log shows the sources at registration
  and the layer layout whenever it changes.
- Horizon queries the FOV right after `sceHmd2Open`. By then the session is normally focused and
  the eye views are cached; if not, the ER15 defaults are returned.

### Where to pick up

In rough order of value:

1. **UI2D/VR2D layers (phase 5).** `Hmd2Composition` still `EXIT_NOT_IMPLEMENTED`s on them.
   Store UI2D placement in `LayerSetting`, give each its own quad swapchain, and submit
   `XrCompositionLayerQuad` after the projection layer (§4.3). Find a title that uses them first.
2. **`GetStatus`, user events, `SetRenderConfig`** (§4.2). Horizon calls none of them. For
   `RENDER60_AT_REPROJECTION120`, have `HeadsetVblank` skip every second XR frame.
3. **Gaze** via `XR_EXT_eye_gaze_interaction` (enabled when present; nothing reads it yet).
4. **`scePadVrControllerRead` / `ReadCombinedState`.** NIDs still need computing; build them from
   `Input`'s cached state.
5. **Windows** time conversion (`XR_KHR_win32_convert_performance_counter_time`), then remove the
   refusal in `Runtime::CreateInstance`.
6. **Untested:** trigger-effect haptics (Monado's Qwerty controllers ignore haptics), SteamVR (§10), real hardware, a runtime without `LOCAL_FLOOR`
   (the 1.6 m fallback in `CreateOffsetSpace`), recentre in a game, and play-area queries on a
   runtime that reports bounds.
