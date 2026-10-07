#include "graphics/presentation/xr/xrInput.h"

#ifdef KYTY_OPENXR

#include "common/assert.h"
#include "common/logging/log.h"

#include <cstring>
#include <string>
#include <vector>

namespace Libs::Graphics::Xr {

constexpr float GRAVITY = 9.80665f;
// A haptic pulse lasts this long and is renewed while the guest keeps the motor on.
constexpr XrDuration HAPTIC_PULSE   = 100000000;
constexpr XrDuration HAPTIC_RENEWAL = 50000000;

Input::~Input() {
	Detach();
	if (m_action_set != XR_NULL_HANDLE) {
		xrDestroyActionSet(m_action_set);
	}
}

bool Input::CreateAction(XrActionType type, const char* name, const char* localized,
                         XrAction* action) {
	const std::array<XrPath, 2> hands {m_hands[0].path, m_hands[1].path};
	XrActionCreateInfo          info {XR_TYPE_ACTION_CREATE_INFO};
	info.actionType          = type;
	info.countSubactionPaths = static_cast<uint32_t>(hands.size());
	info.subactionPaths      = hands.data();
	std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
	std::strncpy(info.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
	return m_runtime.Check(xrCreateAction(m_action_set, &info, action), "xrCreateAction");
}

bool Input::Create() {
	auto instance = m_runtime.GetInstance();
	if (!m_runtime.Check(xrStringToPath(instance, "/user/hand/left", &m_hands[0].path),
	                     "xrStringToPath") ||
	    !m_runtime.Check(xrStringToPath(instance, "/user/hand/right", &m_hands[1].path),
	                     "xrStringToPath")) {
		return false;
	}

	XrActionSetCreateInfo set_info {XR_TYPE_ACTION_SET_CREATE_INFO};
	std::strncpy(set_info.actionSetName, "psvr2", XR_MAX_ACTION_SET_NAME_SIZE - 1);
	std::strncpy(set_info.localizedActionSetName, "PlayStation VR2 Sense",
	             XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
	if (!m_runtime.Check(xrCreateActionSet(instance, &set_info, &m_action_set),
	                     "xrCreateActionSet")) {
		return false;
	}

	auto& a = m_actions;
	const bool created =
	    CreateAction(XR_ACTION_TYPE_POSE_INPUT, "grip_pose", "Hand pose", &a.grip_pose) &&
	    CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "face_upper", "Triangle / Circle",
	                 &a.face_upper) &&
	    CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "face_lower", "Square / Cross", &a.face_lower) &&
	    CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "face_upper_touch", "Triangle / Circle touch",
	                 &a.face_upper_touch) &&
	    CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "face_lower_touch", "Square / Cross touch",
	                 &a.face_lower_touch) &&
	    CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Options", &a.menu) &&
	    CreateAction(XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "L2 / R2", &a.trigger) &&
	    CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "trigger_touch", "L2 / R2 touch",
	                 &a.trigger_touch) &&
	    CreateAction(XR_ACTION_TYPE_FLOAT_INPUT, "grip", "L1 / R1", &a.grip) &&
	    CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "grip_touch", "L1 / R1 touch", &a.grip_touch) &&
	    CreateAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "stick", "Stick", &a.stick) &&
	    CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click", "L3 / R3", &a.stick_click) &&
	    CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_touch", "L3 / R3 touch",
	                 &a.stick_touch) &&
	    CreateAction(XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Vibration", &a.haptic);
	if (!created) {
		return false;
	}
	SuggestBindings();
	return true;
}

// Suggests one profile at a time, so a profile the runtime rejects costs only itself.
void Input::SuggestBindings() {
	auto        instance = m_runtime.GetInstance();
	const auto& a        = m_actions;

	struct Binding {
		XrAction    action;
		const char* path; // below /user/hand/<side>
		int         side; // 0 left, 1 right, -1 both
	};
	struct Profile {
		const char*          name;
		std::vector<Binding> bindings;
	};
	// Shared by every controller profile.
	const std::vector<Binding> common = {
	    {a.grip_pose, "/input/grip/pose", -1},
	    {a.haptic, "/output/haptic", -1},
	};
	// The Sense controller's shape: Touch-like face buttons, trigger, grip and stick.
	const std::vector<Profile> profiles = {
	    {"/interaction_profiles/oculus/touch_controller",
	     {{a.face_lower, "/input/x/click", 0},
	      {a.face_upper, "/input/y/click", 0},
	      {a.face_lower_touch, "/input/x/touch", 0},
	      {a.face_upper_touch, "/input/y/touch", 0},
	      {a.menu, "/input/menu/click", 0},
	      {a.face_lower, "/input/a/click", 1},
	      {a.face_upper, "/input/b/click", 1},
	      {a.face_lower_touch, "/input/a/touch", 1},
	      {a.face_upper_touch, "/input/b/touch", 1},
	      {a.trigger, "/input/trigger/value", -1},
	      {a.trigger_touch, "/input/trigger/touch", -1},
	      {a.grip, "/input/squeeze/value", -1},
	      {a.stick, "/input/thumbstick", -1},
	      {a.stick_click, "/input/thumbstick/click", -1},
	      {a.stick_touch, "/input/thumbstick/touch", -1}}},
	    {"/interaction_profiles/valve/index_controller",
	     {{a.face_lower, "/input/a/click", -1},
	      {a.face_upper, "/input/b/click", -1},
	      {a.face_lower_touch, "/input/a/touch", -1},
	      {a.face_upper_touch, "/input/b/touch", -1},
	      {a.trigger, "/input/trigger/value", -1},
	      {a.trigger_touch, "/input/trigger/touch", -1},
	      {a.grip, "/input/squeeze/value", -1},
	      {a.stick, "/input/thumbstick", -1},
	      {a.stick_click, "/input/thumbstick/click", -1},
	      {a.stick_touch, "/input/thumbstick/touch", -1}}},
	    {"/interaction_profiles/microsoft/motion_controller",
	     {{a.menu, "/input/menu/click", -1},
	      {a.trigger, "/input/trigger/value", -1},
	      {a.grip, "/input/squeeze/click", -1},
	      {a.stick, "/input/thumbstick", -1},
	      {a.stick_click, "/input/thumbstick/click", -1},
	      {a.face_lower, "/input/trackpad/click", -1},
	      {a.face_lower_touch, "/input/trackpad/touch", -1}}},
	    {"/interaction_profiles/htc/vive_controller",
	     {{a.menu, "/input/menu/click", -1},
	      {a.trigger, "/input/trigger/value", -1},
	      {a.grip, "/input/squeeze/click", -1},
	      {a.stick, "/input/trackpad", -1},
	      {a.stick_click, "/input/trackpad/click", -1},
	      {a.stick_touch, "/input/trackpad/touch", -1}}},
	    {"/interaction_profiles/khr/simple_controller",
	     {{a.trigger, "/input/select/click", -1}, {a.menu, "/input/menu/click", -1}}},
	};

	for (const auto& profile: profiles) {
		std::vector<XrActionSuggestedBinding> suggested;
		bool                                  paths_ok = true;
		const auto add = [&](const Binding& binding) {
			for (int side = 0; side < 2; side++) {
				if (binding.side >= 0 && binding.side != side) {
					continue;
				}
				const std::string path =
				    std::string(side == 0 ? "/user/hand/left" : "/user/hand/right") + binding.path;
				XrPath xr_path = XR_NULL_PATH;
				paths_ok &= m_runtime.Check(xrStringToPath(instance, path.c_str(), &xr_path),
				                            "xrStringToPath");
				suggested.push_back({binding.action, xr_path});
			}
		};
		for (const auto& binding: common) {
			add(binding);
		}
		for (const auto& binding: profile.bindings) {
			add(binding);
		}

		XrPath profile_path = XR_NULL_PATH;
		if (!paths_ok ||
		    !m_runtime.Check(xrStringToPath(instance, profile.name, &profile_path),
		                     "xrStringToPath")) {
			continue;
		}
		XrInteractionProfileSuggestedBinding info {
		    XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
		info.interactionProfile     = profile_path;
		info.countSuggestedBindings = static_cast<uint32_t>(suggested.size());
		info.suggestedBindings      = suggested.data();
		if (!m_runtime.Check(xrSuggestInteractionProfileBindings(instance, &info),
		                     "xrSuggestInteractionProfileBindings")) {
			LOGF("OpenXR: bindings for %s were rejected\n", profile.name);
		}
	}
}

bool Input::Attach(XrSession session) {
	XrSessionActionSetsAttachInfo attach {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	attach.countActionSets = 1;
	attach.actionSets      = &m_action_set;
	if (!m_runtime.Check(xrAttachSessionActionSets(session, &attach),
	                     "xrAttachSessionActionSets")) {
		return false;
	}
	m_session = session;
	for (auto& hand: m_hands) {
		XrActionSpaceCreateInfo info {XR_TYPE_ACTION_SPACE_CREATE_INFO};
		info.action            = m_actions.grip_pose;
		info.subactionPath     = hand.path;
		info.poseInActionSpace = IDENTITY_POSE;
		if (!m_runtime.Check(xrCreateActionSpace(session, &info, &hand.grip_space),
		                     "xrCreateActionSpace")) {
			return false;
		}
	}
	return true;
}

void Input::Detach() {
	for (auto& hand: m_hands) {
		if (hand.grip_space != XR_NULL_HANDLE) {
			xrDestroySpace(hand.grip_space);
			hand.grip_space = XR_NULL_HANDLE;
		}
	}
	m_session = XR_NULL_HANDLE;
}

bool Input::GetBool(XrAction action, XrPath hand) const {
	XrActionStateGetInfo info {XR_TYPE_ACTION_STATE_GET_INFO};
	info.action        = action;
	info.subactionPath = hand;
	XrActionStateBoolean state {XR_TYPE_ACTION_STATE_BOOLEAN};
	return XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &info, &state)) &&
	       state.isActive == XR_TRUE && state.currentState == XR_TRUE;
}

float Input::GetFloat(XrAction action, XrPath hand) const {
	XrActionStateGetInfo info {XR_TYPE_ACTION_STATE_GET_INFO};
	info.action        = action;
	info.subactionPath = hand;
	XrActionStateFloat state {XR_TYPE_ACTION_STATE_FLOAT};
	if (XR_FAILED(xrGetActionStateFloat(m_session, &info, &state)) || state.isActive != XR_TRUE) {
		return 0.0f;
	}
	return state.currentState;
}

void Input::Sync(XrTime time, XrSpace world) {
	if (m_session == XR_NULL_HANDLE) {
		return;
	}
	XrActiveActionSet active {m_action_set, XR_NULL_PATH};
	XrActionsSyncInfo info {XR_TYPE_ACTIONS_SYNC_INFO};
	info.countActiveActionSets = 1;
	info.activeActionSets      = &active;
	const XrResult result      = xrSyncActions(m_session, &info);
	if (XR_FAILED(result)) {
		(void)m_runtime.Check(result, "xrSyncActions");
		return;
	}
	// The runtime's UI has the controllers.
	const bool focused = result != XR_SESSION_NOT_FOCUSED;
	for (auto& hand: m_hands) {
		SyncHand(hand, time, world, focused);
		ApplyHaptics(hand, time);
	}
}

void Input::SyncHand(HandData& hand, XrTime time, XrSpace world, bool focused) {
	const auto& a = m_actions;
	ControllerState state;
	state.focused   = focused;
	state.timestamp = m_runtime.ToGuestTime(time);

	XrActionStateGetInfo pose_info {XR_TYPE_ACTION_STATE_GET_INFO};
	pose_info.action        = a.grip_pose;
	pose_info.subactionPath = hand.path;
	XrActionStatePose pose_state {XR_TYPE_ACTION_STATE_POSE};
	state.connected = XR_SUCCEEDED(xrGetActionStatePose(m_session, &pose_info, &pose_state)) &&
	                  pose_state.isActive == XR_TRUE;

	if (focused) {
		state.face_upper       = GetBool(a.face_upper, hand.path);
		state.face_lower       = GetBool(a.face_lower, hand.path);
		state.face_upper_touch = GetBool(a.face_upper_touch, hand.path) || state.face_upper;
		state.face_lower_touch = GetBool(a.face_lower_touch, hand.path) || state.face_lower;
		state.menu             = GetBool(a.menu, hand.path);
		state.trigger          = GetFloat(a.trigger, hand.path);
		state.trigger_touch    = GetBool(a.trigger_touch, hand.path) || state.trigger > 0.0f;
		state.grip             = GetFloat(a.grip, hand.path);
		// Few controllers sense a resting finger on the grip; a light squeeze stands in.
		state.grip_touch  = GetBool(a.grip_touch, hand.path) || state.grip > 0.05f;
		state.stick_click = GetBool(a.stick_click, hand.path);
		state.stick_touch = GetBool(a.stick_touch, hand.path) || state.stick_click;

		XrActionStateGetInfo stick_info {XR_TYPE_ACTION_STATE_GET_INFO};
		stick_info.action        = a.stick;
		stick_info.subactionPath = hand.path;
		XrActionStateVector2f stick {XR_TYPE_ACTION_STATE_VECTOR2F};
		if (XR_SUCCEEDED(xrGetActionStateVector2f(m_session, &stick_info, &stick)) &&
		    stick.isActive == XR_TRUE) {
			state.stick_x = stick.currentState.x;
			state.stick_y = stick.currentState.y;
		}
	}

	// Motion: what an IMU in the grip would measure.
	XrSpaceVelocity velocity {XR_TYPE_SPACE_VELOCITY};
	XrSpaceLocation location {XR_TYPE_SPACE_LOCATION};
	location.next = &velocity;
	const bool located =
	    state.connected && world != XR_NULL_HANDLE && hand.grip_space != XR_NULL_HANDLE &&
	    XR_SUCCEEDED(xrLocateSpace(hand.grip_space, world, time, &location)) &&
	    (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
	// Only this thread writes the orientation; ResetOrientation reads it under the lock.
	const auto orientation = located ? location.pose.orientation : hand.orientation;
	const auto to_local    = Conjugate(orientation);

	XrVector3f acceleration {0.0f, GRAVITY, 0.0f};
	if (located && (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0) {
		if (hand.velocity_time != 0 && time > hand.velocity_time) {
			const float dt = static_cast<float>(time - hand.velocity_time) * 1e-9f;
			acceleration.x += (velocity.linearVelocity.x - hand.velocity.x) / dt;
			acceleration.y += (velocity.linearVelocity.y - hand.velocity.y) / dt;
			acceleration.z += (velocity.linearVelocity.z - hand.velocity.z) / dt;
		}
		hand.velocity      = velocity.linearVelocity;
		hand.velocity_time = time;
	} else {
		hand.velocity_time = 0;
	}
	const auto local_acceleration = Rotate(to_local, acceleration);
	state.acceleration            = {local_acceleration.x / GRAVITY,
	                                 local_acceleration.y / GRAVITY,
	                                 local_acceleration.z / GRAVITY};
	if (located && (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0) {
		const auto angular     = Rotate(to_local, velocity.angularVelocity);
		state.angular_velocity = {angular.x, angular.y, angular.z};
	}

	Common::LockGuard lock(m_mutex);
	hand.orientation = orientation;
	if (state.connected && !hand.was_connected) {
		hand.reference = hand.orientation;
		hand.state.connected_count++;
	}
	hand.was_connected    = state.connected;
	const auto relative   = Multiply(Conjugate(hand.reference), hand.orientation);
	state.orientation     = {relative.x, relative.y, relative.z, relative.w};
	state.connected_count = hand.state.connected_count;
	hand.state            = state;
}

void Input::ApplyHaptics(HandData& hand, XrTime time) {
	const float amplitude = hand.vibration.load();
	XrHapticActionInfo info {XR_TYPE_HAPTIC_ACTION_INFO};
	info.action        = m_actions.haptic;
	info.subactionPath = hand.path;
	if (amplitude <= 0.0f) {
		if (hand.applied > 0.0f) {
			(void)m_runtime.Check(xrStopHapticFeedback(m_session, &info), "xrStopHapticFeedback");
			hand.applied = 0.0f;
		}
		return;
	}
	if (amplitude == hand.applied && time - hand.applied_time < HAPTIC_RENEWAL) {
		return;
	}
	XrHapticVibration vibration {XR_TYPE_HAPTIC_VIBRATION};
	vibration.duration  = HAPTIC_PULSE;
	vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
	vibration.amplitude = amplitude;
	(void)m_runtime.Check(
	    xrApplyHapticFeedback(m_session, &info,
	                          reinterpret_cast<const XrHapticBaseHeader*>(&vibration)),
	    "xrApplyHapticFeedback");
	hand.applied      = amplitude;
	hand.applied_time = time;
}

bool Input::GetState(Hand hand, ControllerState* state) {
	EXIT_IF(state == nullptr);
	Common::LockGuard lock(m_mutex);
	*state = m_hands[static_cast<int>(hand)].state;
	return m_session != XR_NULL_HANDLE;
}

void Input::ResetOrientation(Hand hand) {
	Common::LockGuard lock(m_mutex);
	auto&             data = m_hands[static_cast<int>(hand)];
	data.reference         = data.orientation;
}

void Input::SetVibration(Hand hand, float amplitude) {
	m_hands[static_cast<int>(hand)].vibration = amplitude;
}

} // namespace Libs::Graphics::Xr

#endif // KYTY_OPENXR
