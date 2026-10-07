#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_XR_XRINPUT_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_XR_XRINPUT_H_

#include "common/common.h"

#ifdef KYTY_OPENXR

#include "common/threads.h"
#include "graphics/presentation/xr/xrInternal.h"

#include <array>
#include <atomic>

namespace Libs::Graphics::Xr {

// The Sense controllers as one OpenXR action set, bound to the common interaction profiles.
// The frame thread syncs actions once per headset frame and caches the result; guest threads
// read the cache and locate the grip spaces directly.
class Input final {
public:
	explicit Input(Runtime& runtime): m_runtime(runtime) {}
	~Input();
	KYTY_CLASS_NO_COPY(Input);

	// Creates the actions and suggests bindings; before the session exists.
	bool Create();
	// Attaches the actions to the session and creates the grip spaces.
	bool Attach(XrSession session);
	void Detach();

	// Frame thread: syncs actions, samples state and motion, and drives haptics. world is a
	// space fixed in the room.
	void Sync(XrTime time, XrSpace world);

	bool    GetState(Hand hand, ControllerState* state);
	void    ResetOrientation(Hand hand);
	void    SetVibration(Hand hand, float amplitude);
	XrSpace GetGripSpace(Hand hand) const { return m_hands[static_cast<int>(hand)].grip_space; }

private:
	struct Actions {
		XrAction grip_pose        = XR_NULL_HANDLE;
		XrAction face_upper       = XR_NULL_HANDLE;
		XrAction face_lower       = XR_NULL_HANDLE;
		XrAction face_upper_touch = XR_NULL_HANDLE;
		XrAction face_lower_touch = XR_NULL_HANDLE;
		XrAction menu             = XR_NULL_HANDLE;
		XrAction trigger          = XR_NULL_HANDLE;
		XrAction trigger_touch    = XR_NULL_HANDLE;
		XrAction grip             = XR_NULL_HANDLE;
		XrAction grip_touch       = XR_NULL_HANDLE;
		XrAction stick            = XR_NULL_HANDLE;
		XrAction stick_click      = XR_NULL_HANDLE;
		XrAction stick_touch      = XR_NULL_HANDLE;
		XrAction haptic           = XR_NULL_HANDLE;
	};

	struct HandData {
		XrPath  path       = XR_NULL_PATH;
		XrSpace grip_space = XR_NULL_HANDLE;

		// Motion tracking between syncs.
		bool          was_connected = false;
		XrQuaternionf reference {0.0f, 0.0f, 0.0f, 1.0f};
		XrQuaternionf orientation {0.0f, 0.0f, 0.0f, 1.0f};
		XrVector3f    velocity {};
		XrTime        velocity_time = 0;

		// Haptics as last applied by the frame thread.
		std::atomic<float> vibration = 0.0f;
		float              applied   = 0.0f;
		XrTime             applied_time = 0;

		ControllerState state;
	};

	bool CreateAction(XrActionType type, const char* name, const char* localized,
	                  XrAction* action);
	void SuggestBindings();
	void SyncHand(HandData& hand, XrTime time, XrSpace world, bool focused);
	void ApplyHaptics(HandData& hand, XrTime time);

	[[nodiscard]] bool  GetBool(XrAction action, XrPath hand) const;
	[[nodiscard]] float GetFloat(XrAction action, XrPath hand) const;

	Runtime&                m_runtime;
	XrActionSet             m_action_set = XR_NULL_HANDLE;
	XrSession               m_session    = XR_NULL_HANDLE;
	Actions                 m_actions;
	std::array<HandData, 2> m_hands;
	Common::Mutex           m_mutex;
};

} // namespace Libs::Graphics::Xr

#endif // KYTY_OPENXR

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_XR_XRINPUT_H_
