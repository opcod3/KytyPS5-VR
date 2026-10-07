#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_XR_XRINTERNAL_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_XR_XRINTERNAL_H_

#include "common/common.h"

#ifdef KYTY_OPENXR

#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/xr/xrRuntime.h"

#include <array>
#include <atomic>
#include <ctime>
#include <deque>
#include <memory>
#include <string>

#define XR_USE_GRAPHICS_API_VULKAN
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#define XR_USE_TIMESPEC
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace Libs::Graphics::Xr {

class FrameThread;
class Input;

[[nodiscard]] const char* ResultToString(XrInstance instance, XrResult result);

// Pose helpers; see xrMath.cpp.
[[nodiscard]] XrQuaternionf Multiply(const XrQuaternionf& a, const XrQuaternionf& b);
[[nodiscard]] XrQuaternionf Conjugate(const XrQuaternionf& q);
[[nodiscard]] XrVector3f    Rotate(const XrQuaternionf& q, const XrVector3f& v);
[[nodiscard]] XrPosef ComposePoses(const XrPosef& a, const XrPosef& b);
[[nodiscard]] XrPosef InversePose(const XrPosef& pose);
[[nodiscard]] XrVector3f TransformPoint(const XrPosef& pose, const XrVector3f& point);
// Keeps only the rotation about +Y.
[[nodiscard]] XrQuaternionf YawOnly(const XrQuaternionf& orientation);
[[nodiscard]] XrPosef ToXrPose(const Pose& pose);
[[nodiscard]] Pose    FromXrPose(const XrPosef& pose);
inline constexpr XrPosef IDENTITY_POSE {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};

class Runtime final {
public:
	Runtime() = default;
	~Runtime();
	KYTY_CLASS_NO_COPY(Runtime);

	bool Create();

	vk::Result         CreateVulkanInstance(const vk::InstanceCreateInfo& create_info,
	                                        vk::Instance*                 instance);
	vk::PhysicalDevice GetVulkanPhysicalDevice(vk::Instance instance);
	vk::Result         CreateVulkanDevice(vk::PhysicalDevice          physical_device,
	                                      const vk::DeviceCreateInfo& create_info,
	                                      vk::Device*                 device);

	void StartSession(GraphicContext* graphics);
	void StopSession();

	void RecordPanelCopy(vk::CommandBuffer command, vk::Image source, vk::Format format,
	                     vk::Extent2D extent, const PanelView& view);
	void PanelCopySubmitted();

	// The newest headset panel, one array layer per eye, in TRANSFER_SRC_OPTIMAL once ready.
	// The frame thread holds the mutex while it records and submits its copy out of the panel,
	// so the image cannot be replaced under it.
	struct Panel {
		Common::Mutex   mutex;
		VulkanImage     image;
		vk::ImageLayout layout = vk::ImageLayout::eUndefined;
		PanelView       view;
		PanelView       pending_view;
		bool            ready = false;
	};

	[[nodiscard]] Panel& GetPanel() { return m_panel; }

	// Drains the event queue and drives the session state machine. Frame thread only.
	void PollEvents();

	[[nodiscard]] bool Check(XrResult result, const char* operation) const;

	[[nodiscard]] XrInstance   GetInstance() const { return m_instance; }
	[[nodiscard]] XrSession    GetSession() const { return m_session; }
	[[nodiscard]] XrSystemId   GetSystemId() const { return m_system_id; }
	[[nodiscard]] XrSpace      GetViewSpace() const { return m_view_space; }
	[[nodiscard]] SessionState GetSessionState() const { return m_state; }
	[[nodiscard]] bool         IsSessionRunning() const { return m_session_running; }
	[[nodiscard]] bool         IsUserPresent() const { return m_user_present; }
	[[nodiscard]] XrEnvironmentBlendMode GetBlendMode() const { return m_blend_mode; }
	[[nodiscard]] GraphicContext*        GetGraphics() const { return m_graphics; }

	struct Extensions {
		bool convert_time  = false;
		bool local_floor   = false;
		bool user_presence = false;
		bool refresh_rate  = false;
		bool eye_gaze      = false;
		bool debug_utils   = false;
	};

	[[nodiscard]] const Extensions& GetExtensions() const { return m_ext; }

	// Clock (xrClock.cpp). Guest time is KernelGetProcessTime microseconds.
	void                   CalibrateClock();
	[[nodiscard]] XrTime   Now() const;
	[[nodiscard]] XrTime   ToXrTime(uint64_t guest_time) const;
	[[nodiscard]] uint64_t ToGuestTime(XrTime time) const;

	// Frame pacing, fed by the frame thread.
	void     OnFrameWaited(const XrFrameState& state);
	uint64_t BindDisplayTime(int64_t flip_arg, bool rebind);

	// Eye poses relative to the head and their fields of view, refreshed every frame.
	void UpdateViews(const std::array<XrView, 2>& views);
	bool GetViews(std::array<XrView, 2>* views);

	// Tracking spaces (xrTracking.cpp).
	bool             LocateHead(uint64_t time, int64_t flip_arg, HeadLocation* location);
	CoordinateSystem GetCoordinateSystem();
	bool             SetCoordinateSystem(CoordinateSystem system);
	bool             ResetLocalCoordinate(const Pose* pose);
	bool             LocateCoordinateSystem(CoordinateSystem system, Pose* origin);
	uint32_t         GetPlayAreaUpdateCounter() const { return m_play_area_counter; }
	bool             GetPlayAreaBounds(PlayAreaBounds* bounds);
	bool             HasPlayAreaBounds();
	// The space a frame was rendered in; falls back to the current coordinate system. The space
	// stays valid for at least SPACE_GENERATIONS recentres.
	XrSpace          GetFrameSpace(int64_t flip_arg);
	// A space fixed in the room, for motion sampling.
	XrSpace          GetWorldSpace() const { return m_raw_local; }

	[[nodiscard]] Input* GetInput() const { return m_input.get(); }
	bool                 LocateController(Hand hand, uint64_t time, Location* location);

private:
	// One set of recentred LOCAL and LOCAL_FLOOR spaces.
	struct SpaceGeneration {
		uint32_t id          = 0;
		XrSpace  local       = XR_NULL_HANDLE;
		XrSpace  local_floor = XR_NULL_HANDLE;
	};
	// The coordinate system GetResult used for a flip_arg.
	struct FrameSpace {
		int64_t          flip_arg   = 0;
		uint32_t         generation = 0;
		CoordinateSystem system     = CoordinateSystem::LocalFloor;
		bool             used       = false;
	};
	struct DisplaySlot {
		int64_t flip_arg = 0;
		XrTime  time     = 0;
		bool    used     = false;
	};
	static constexpr size_t SPACE_GENERATIONS = 16;

	bool CreateInstance();
	bool FindSystem();
	void HandleSessionStateChange(const XrEventDataSessionStateChanged& event);
	void DestroyPanel();

	void    CreateSpaces();
	void    DestroySpaces();
	// A LOCAL or LOCAL_FLOOR space whose origin sits at origin in the un-recentred space.
	XrSpace CreateOffsetSpace(bool floor, const XrPosef& origin);
	bool    PushGeneration(const XrPosef& local_origin, const XrPosef& floor_origin);
	XrSpace CurrentSpace(CoordinateSystem system) const;
	XrSpace GenerationSpace(uint32_t generation, CoordinateSystem system) const;
	bool    Locate(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation* location) const;

	XrInstance               m_instance        = XR_NULL_HANDLE;
	XrDebugUtilsMessengerEXT m_debug_messenger = XR_NULL_HANDLE;
	XrSystemId               m_system_id       = XR_NULL_SYSTEM_ID;
	XrVersion                m_api_version     = 0;
	Extensions               m_ext;
	std::string              m_system_name;

	XrEnvironmentBlendMode  m_blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	XrViewConfigurationView m_views[2] {};

	PFN_xrGetVulkanGraphicsRequirements2KHR m_get_graphics_requirements = nullptr;
	PFN_xrCreateVulkanInstanceKHR           m_create_vulkan_instance    = nullptr;
	PFN_xrGetVulkanGraphicsDevice2KHR       m_get_vulkan_device         = nullptr;
	PFN_xrCreateVulkanDeviceKHR             m_create_vulkan_device      = nullptr;
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	PFN_xrConvertTimespecTimeToTimeKHR m_convert_timespec = nullptr;
#endif

	GraphicContext*           m_graphics        = nullptr;
	XrSession                 m_session         = XR_NULL_HANDLE;
	XrSpace                   m_view_space      = XR_NULL_HANDLE;
	std::atomic<SessionState> m_state           = SessionState::None;
	std::atomic_bool          m_session_running = false;
	std::atomic_bool          m_user_present    = true;

	// XrTime = guest microseconds * 1000 + offset.
	std::atomic<int64_t> m_clock_offset = 0;

	Common::Mutex                m_display_mutex;
	XrTime                       m_last_display_time = 0;
	XrDuration                   m_display_period    = 0;
	std::array<DisplaySlot, 32>  m_display_slots {};
	uint32_t                     m_display_slot_next = 0;

	Common::Mutex         m_views_mutex;
	std::array<XrView, 2> m_eye_views {};
	bool                  m_eye_views_known = false;

	mutable Common::Mutex       m_spaces_mutex;
	XrSpace                     m_stage_space = XR_NULL_HANDLE;
	// LOCAL and LOCAL_FLOOR as the runtime defines them; recentres are relative to these.
	XrSpace                     m_raw_local       = XR_NULL_HANDLE;
	XrSpace                     m_raw_local_floor = XR_NULL_HANDLE;
	std::deque<SpaceGeneration> m_generations;
	uint32_t                    m_next_generation   = 0;
	CoordinateSystem            m_coordinate_system = CoordinateSystem::LocalFloor;
	std::array<FrameSpace, 32>  m_frame_spaces {};
	uint32_t                    m_frame_space_next = 0;
	std::atomic<uint32_t>       m_play_area_counter = 0;

	// STAGE bounds, queried once and again after the runtime moves the reference spaces. Some
	// runtimes fail the query on every call, so it is not repeated per GetResult.
	Common::Mutex m_bounds_mutex;
	XrExtent2Df   m_bounds {};
	bool          m_bounds_stale = true;

	Panel                        m_panel;
	std::unique_ptr<Input>       m_input;
	std::unique_ptr<FrameThread> m_frame_thread;
};

} // namespace Libs::Graphics::Xr

#endif // KYTY_OPENXR

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_XR_XRINTERNAL_H_
