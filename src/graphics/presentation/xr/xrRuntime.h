#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_XR_XRRUNTIME_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_XR_XRRUNTIME_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <cstdint>

// Host OpenXR runtime that backs the PSVR2 headset in VR mode openxr. The interface is free of
// OpenXR types so that HLE code and builds without the OpenXR loader can use it.
//
// Poses use the PS5 and OpenXR convention: right-handed, Y up, -Z forward, metres, quaternions
// as x, y, z, w. Times are guest process time in microseconds.
namespace Libs::Graphics {
struct GraphicContext;
} // namespace Libs::Graphics

namespace Libs::Graphics::Xr {

// Mirrors XrSessionState.
enum class SessionState : uint8_t {
	None,
	Idle,
	Ready,
	Synchronized,
	Visible,
	Focused,
	Stopping,
	LossPending,
	Exiting,
};

// Values match SceVrTracker2CoordinateSystemType.
enum class CoordinateSystem : uint32_t { LocalFloor = 0, Local = 1, Stage = 2 };

struct Pose {
	std::array<float, 3> position {};
	std::array<float, 4> orientation {0.0f, 0.0f, 0.0f, 1.0f};
};

// Tangents of the frustum an eye was rendered with; out is temporal, in is nasal.
struct EyeFov {
	float tan_out;
	float tan_in;
	float tan_top;
	float tan_bottom;
};

struct Location {
	Pose pose;
	bool position_valid      = false;
	bool position_tracked    = false;
	bool orientation_valid   = false;
	bool orientation_tracked = false;
};

struct HeadLocation {
	Location            device;
	std::array<Pose, 2> eyes;
};

// How the headset shows one panel.
struct PanelView {
	std::array<EyeFov, 2> fov {};
	// Head pose the panel was rendered with, in the space GetResult used for flip_arg.
	Pose    pose;
	int64_t flip_arg     = 0;
	bool    world_locked = false;
};

enum class Hand : uint8_t { Left = 0, Right = 1 };

// One hand's input as of the frame thread's last action sync. Buttons follow the Sense
// controller layout: face_upper is triangle or circle, face_lower square or cross.
struct ControllerState {
	// The grip pose is tracked, i.e. a controller is in that hand.
	bool     connected       = false;
	uint32_t connected_count = 0;
	// False while the runtime's own UI holds input; the values are then neutral.
	bool focused = false;

	bool  face_upper       = false;
	bool  face_lower       = false;
	bool  face_upper_touch = false;
	bool  face_lower_touch = false;
	bool  menu             = false;
	float trigger          = 0.0f;
	bool  trigger_touch    = false;
	float grip             = 0.0f;
	bool  grip_touch       = false;
	float stick_x          = 0.0f; // -1 left .. 1 right
	float stick_y          = 0.0f; // -1 down .. 1 up
	bool  stick_click      = false;
	bool  stick_touch      = false;

	// Motion sensor: orientation relative to the one at connection or the last reset, angular
	// velocity in rad/s and acceleration in G, both in the controller's frame.
	std::array<float, 4> orientation {0.0f, 0.0f, 0.0f, 1.0f};
	std::array<float, 3> angular_velocity {};
	std::array<float, 3> acceleration {0.0f, 1.0f, 0.0f};
	// Guest time of the sync.
	uint64_t timestamp = 0;
};

struct PlayAreaBounds {
	// The STAGE origin in the current coordinate system; the area is centred on it.
	Pose  origin;
	float width = 0.0f; // along STAGE x
	float depth = 0.0f; // along STAGE z
};

// Creates the OpenXR instance and finds the headset. Must run before Vulkan is created, because
// XR_KHR_vulkan_enable2 creates the Vulkan instance and device. Returns false when there is no
// usable runtime or headset; the reason is logged.
bool Initialize();
// Initialize succeeded and Shutdown has not run.
bool IsActive();
// Destroys the instance. Runs after the Vulkan instance is destroyed.
void Shutdown();

// XR_KHR_vulkan_enable2 replacements for vkCreateInstance, physical device selection, and
// vkCreateDevice. The runtime appends the extensions it needs to the create infos.
vk::Result         CreateVulkanInstance(const vk::InstanceCreateInfo& create_info,
                                        vk::Instance*                 instance);
vk::PhysicalDevice GetVulkanPhysicalDevice(vk::Instance instance);
vk::Result         CreateVulkanDevice(vk::PhysicalDevice physical_device,
                                      const vk::DeviceCreateInfo& create_info, vk::Device* device);

// Creates the session on the graphics queue and starts the frame thread. The session must be
// stopped before the Vulkan device is destroyed.
void StartSession(GraphicContext* graphics);
void StopSession();

[[nodiscard]] SessionState GetSessionState();
// The headset is ready to show frames.
[[nodiscard]] bool IsHeadsetReady();
// The user wears the headset; always true when the runtime cannot tell.
[[nodiscard]] bool IsUserPresent();
// Mirrored per-eye field of view of the headset, the larger of both eyes per side. False until
// the first frame has located the views.
bool GetFieldOfView(EyeFov* fov);
// Distance between the eyes in metres; false until known.
bool GetEyeSeparation(float* meters);

// Display time of the frame the guest starts with flip_arg: the first headset refresh it can
// still make. BeginFrame binds flip_arg to a refresh; GetPredictedDisplayTime looks it up and
// binds it when the guest skipped BeginFrame.
uint64_t BeginFrame(int64_t flip_arg);
uint64_t GetPredictedDisplayTime(int64_t flip_arg);

// Locates the headset at a guest time (0 means now) in the current coordinate system, and
// remembers that coordinate system for flip_arg so the frame is shown where it was rendered.
bool LocateHead(uint64_t time, int64_t flip_arg, HeadLocation* location);

[[nodiscard]] CoordinateSystem GetCoordinateSystem();
// False when the runtime has no such space.
bool SetCoordinateSystem(CoordinateSystem system);
// Moves the LOCAL and LOCAL_FLOOR origins to the head's current position and yaw, or to a pose
// given in the current coordinate system.
bool ResetLocalCoordinate();
bool ResetLocalCoordinate(const Pose& pose);
// Origin of another coordinate system in the current one.
bool LocateCoordinateSystem(CoordinateSystem system, Pose* origin);
// Bumped when the runtime moves a reference space or the guest recentres.
[[nodiscard]] uint32_t GetPlayAreaUpdateCounter();
// False when the runtime reports no bounds.
bool GetPlayAreaBounds(PlayAreaBounds* bounds);
// Cheap: the runtime reports bounds, i.e. the user set up a room-scale play area.
bool HasPlayAreaBounds();

// Controllers. State is sampled once per headset frame; poses are located on demand.
bool GetControllerState(Hand hand, ControllerState* state);
// Locates a controller's grip at a guest time (0 means now) in the current coordinate system.
bool LocateController(Hand hand, uint64_t time, Location* location);
// Makes the current orientation the reference ControllerState::orientation is relative to.
void ResetControllerOrientation(Hand hand);
// Vibration strength 0..1, held until changed; 0 stops it.
void SetControllerVibration(Hand hand, float amplitude);

// Records a copy of a side-by-side headset panel (left eye in the left half) into the image the
// frame thread shows. The source must be in TRANSFER_SRC_OPTIMAL layout, and its contents
// gamma-encoded in an 8-bit UNORM format. Call PanelCopySubmitted once the command buffer is
// submitted; until then the frame thread keeps showing the previous panel.
void RecordPanelCopy(vk::CommandBuffer command, vk::Image source, vk::Format format,
                     vk::Extent2D extent, const PanelView& view);
void PanelCopySubmitted();

} // namespace Libs::Graphics::Xr

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_XR_XRRUNTIME_H_
