#include "graphics/presentation/xr/xrInternal.h"
#include "graphics/presentation/xr/xrInput.h"

#ifdef KYTY_OPENXR

#include "common/assert.h"
#include "common/logging/log.h"

#include <algorithm>
#include <vector>

namespace Libs::Graphics::Xr {

// Floor height assumed when a 1.0 runtime has no LOCAL_FLOOR.
constexpr float FALLBACK_FLOOR_HEIGHT = 1.6f;

void Runtime::CreateSpaces() {
	uint32_t count = 0;
	std::vector<XrReferenceSpaceType> types;
	if (Check(xrEnumerateReferenceSpaces(m_session, 0, &count, nullptr),
	          "xrEnumerateReferenceSpaces")) {
		types.resize(count);
		(void)Check(xrEnumerateReferenceSpaces(m_session, count, &count, types.data()),
		            "xrEnumerateReferenceSpaces");
	}
	const auto has = [&](XrReferenceSpaceType type) {
		return std::find(types.begin(), types.end(), type) != types.end();
	};
	m_ext.local_floor = m_ext.local_floor && has(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR);

	Common::LockGuard lock(m_spaces_mutex);
	if (has(XR_REFERENCE_SPACE_TYPE_STAGE)) {
		XrReferenceSpaceCreateInfo info {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
		info.referenceSpaceType   = XR_REFERENCE_SPACE_TYPE_STAGE;
		info.poseInReferenceSpace = IDENTITY_POSE;
		if (!Check(xrCreateReferenceSpace(m_session, &info, &m_stage_space),
		           "xrCreateReferenceSpace(STAGE)")) {
			m_stage_space = XR_NULL_HANDLE;
		}
	}
	m_raw_local       = CreateOffsetSpace(false, IDENTITY_POSE);
	m_raw_local_floor = CreateOffsetSpace(true, IDENTITY_POSE);
	if (m_raw_local == XR_NULL_HANDLE || m_raw_local_floor == XR_NULL_HANDLE ||
	    !PushGeneration(IDENTITY_POSE, IDENTITY_POSE)) {
		EXIT("OpenXR: could not create the tracking spaces\n");
	}
	LOGF("OpenXR: spaces LOCAL_FLOOR=%s STAGE=%s\n", m_ext.local_floor ? "native" : "emulated",
	     m_stage_space != XR_NULL_HANDLE ? "yes" : "no");
}

void Runtime::DestroySpaces() {
	Common::LockGuard lock(m_spaces_mutex);
	for (const auto& generation: m_generations) {
		xrDestroySpace(generation.local);
		xrDestroySpace(generation.local_floor);
	}
	m_generations.clear();
	for (auto* space: {&m_raw_local, &m_raw_local_floor, &m_stage_space}) {
		if (*space != XR_NULL_HANDLE) {
			xrDestroySpace(*space);
			*space = XR_NULL_HANDLE;
		}
	}
	m_frame_spaces = {};
}

XrSpace Runtime::CreateOffsetSpace(bool floor, const XrPosef& origin) {
	XrReferenceSpaceCreateInfo info {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	info.referenceSpaceType   = XR_REFERENCE_SPACE_TYPE_LOCAL;
	info.poseInReferenceSpace = origin;
	if (floor && m_ext.local_floor) {
		info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR;
	} else if (floor) {
		// LOCAL starts at the head, so its floor is a standing height below.
		const XrPosef floor_origin {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, -FALLBACK_FLOOR_HEIGHT, 0.0f}};
		info.poseInReferenceSpace = ComposePoses(floor_origin, origin);
	}
	XrSpace space = XR_NULL_HANDLE;
	if (!Check(xrCreateReferenceSpace(m_session, &info, &space), "xrCreateReferenceSpace")) {
		return XR_NULL_HANDLE;
	}
	return space;
}

bool Runtime::PushGeneration(const XrPosef& local_origin, const XrPosef& floor_origin) {
	SpaceGeneration generation {m_next_generation, CreateOffsetSpace(false, local_origin),
	                            CreateOffsetSpace(true, floor_origin)};
	if (generation.local == XR_NULL_HANDLE || generation.local_floor == XR_NULL_HANDLE) {
		if (generation.local != XR_NULL_HANDLE) {
			xrDestroySpace(generation.local);
		}
		if (generation.local_floor != XR_NULL_HANDLE) {
			xrDestroySpace(generation.local_floor);
		}
		return false;
	}
	m_next_generation++;
	m_generations.push_back(generation);
	// Older frames than this fall back to the current space.
	while (m_generations.size() > SPACE_GENERATIONS) {
		xrDestroySpace(m_generations.front().local);
		xrDestroySpace(m_generations.front().local_floor);
		m_generations.pop_front();
	}
	return true;
}

XrSpace Runtime::CurrentSpace(CoordinateSystem system) const {
	if (m_generations.empty()) {
		return XR_NULL_HANDLE;
	}
	return GenerationSpace(m_generations.back().id, system);
}

XrSpace Runtime::GenerationSpace(uint32_t generation, CoordinateSystem system) const {
	if (system == CoordinateSystem::Stage) {
		return m_stage_space;
	}
	for (const auto& g: m_generations) {
		if (g.id == generation) {
			return system == CoordinateSystem::Local ? g.local : g.local_floor;
		}
	}
	return XR_NULL_HANDLE;
}

bool Runtime::Locate(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation* location) const {
	*location = {XR_TYPE_SPACE_LOCATION};
	if (space == XR_NULL_HANDLE || base == XR_NULL_HANDLE) {
		return false;
	}
	return Check(xrLocateSpace(space, base, time, location), "xrLocateSpace");
}

static Location ToLocation(const XrSpaceLocation& location) {
	Location result;
	result.pose                = FromXrPose(location.pose);
	result.position_valid      = (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
	result.position_tracked    = (location.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0;
	result.orientation_valid   = (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
	result.orientation_tracked =
	    (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0;
	return result;
}

bool Runtime::LocateHead(uint64_t time, int64_t flip_arg, HeadLocation* location) {
	EXIT_IF(location == nullptr);
	*location = {};
	if (m_session == XR_NULL_HANDLE) {
		return false;
	}
	const XrTime xr_time = time != 0 ? ToXrTime(time) : Now();

	Common::LockGuard lock(m_spaces_mutex);
	const XrSpace     base = CurrentSpace(m_coordinate_system);
	if (base == XR_NULL_HANDLE) {
		return false;
	}
	m_frame_spaces[m_frame_space_next] = {flip_arg, m_generations.back().id, m_coordinate_system,
	                                      true};
	m_frame_space_next = (m_frame_space_next + 1) % m_frame_spaces.size();

	XrSpaceLocation head {};
	if (!Locate(m_view_space, base, xr_time, &head)) {
		return false;
	}
	location->device = ToLocation(head);

	XrViewLocateInfo info {XR_TYPE_VIEW_LOCATE_INFO};
	info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	info.displayTime           = xr_time;
	info.space                 = base;
	XrViewState           state {XR_TYPE_VIEW_STATE};
	std::array<XrView, 2> views {{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
	uint32_t              count = 0;
	const bool            located =
	    Check(xrLocateViews(m_session, &info, &state, 2, &count, views.data()), "xrLocateViews") &&
	    count == 2 && (state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0 &&
	    (state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
	for (uint32_t eye = 0; eye < 2; eye++) {
		if (located) {
			location->eyes[eye] = FromXrPose(views[eye].pose);
		} else {
			const XrPosef eye_in_head {{0.0f, 0.0f, 0.0f, 1.0f},
			                           {eye == 0 ? -0.032f : 0.032f, 0.0f, 0.0f}};
			location->eyes[eye] = FromXrPose(ComposePoses(head.pose, eye_in_head));
		}
	}
	return true;
}

bool Runtime::LocateController(Hand hand, uint64_t time, Location* location) {
	EXIT_IF(location == nullptr);
	*location = {};
	if (m_session == XR_NULL_HANDLE || m_input == nullptr) {
		return false;
	}
	const XrTime      xr_time = time != 0 ? ToXrTime(time) : Now();
	Common::LockGuard lock(m_spaces_mutex);
	XrSpaceLocation   grip {};
	if (!Locate(m_input->GetGripSpace(hand), CurrentSpace(m_coordinate_system), xr_time, &grip)) {
		return false;
	}
	*location = ToLocation(grip);
	return true;
}

CoordinateSystem Runtime::GetCoordinateSystem() {
	Common::LockGuard lock(m_spaces_mutex);
	return m_coordinate_system;
}

bool Runtime::SetCoordinateSystem(CoordinateSystem system) {
	Common::LockGuard lock(m_spaces_mutex);
	if (system == CoordinateSystem::Stage && m_stage_space == XR_NULL_HANDLE) {
		return false;
	}
	m_coordinate_system = system;
	return true;
}

bool Runtime::ResetLocalCoordinate(const Pose* pose) {
	if (m_session == XR_NULL_HANDLE) {
		return false;
	}
	const XrTime      now = Now();
	Common::LockGuard lock(m_spaces_mutex);
	if (m_coordinate_system == CoordinateSystem::Stage) {
		return false;
	}
	// The new origin: the head now, or the given pose in the current coordinate system.
	const XrSpace source      = pose == nullptr ? m_view_space : CurrentSpace(m_coordinate_system);
	const XrPosef source_pose = pose == nullptr ? IDENTITY_POSE : ToXrPose(*pose);

	std::array<XrPosef, 2> origins {};
	const std::array<XrSpace, 2> raw {m_raw_local, m_raw_local_floor};
	for (size_t i = 0; i < raw.size(); i++) {
		XrSpaceLocation location {};
		constexpr XrSpaceLocationFlags required =
		    XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
		if (!Locate(source, raw[i], now, &location) ||
		    (location.locationFlags & required) != required) {
			LOGF("OpenXR: recentre failed, the head is not located\n");
			return false;
		}
		const auto target = ComposePoses(location.pose, source_pose);
		// Only position and yaw move the origin; LOCAL_FLOOR keeps its floor.
		origins[i] = {YawOnly(target.orientation),
		              {target.position.x, i == 1 ? 0.0f : target.position.y, target.position.z}};
	}
	if (!PushGeneration(origins[0], origins[1])) {
		return false;
	}
	m_play_area_counter++;
	LOGF("OpenXR: recentred at %.3f %.3f %.3f\n", origins[0].position.x, origins[0].position.y,
	     origins[0].position.z);
	return true;
}

bool Runtime::LocateCoordinateSystem(CoordinateSystem system, Pose* origin) {
	EXIT_IF(origin == nullptr);
	if (m_session == XR_NULL_HANDLE) {
		return false;
	}
	const XrTime      now = Now();
	Common::LockGuard lock(m_spaces_mutex);
	XrSpaceLocation   location {};
	if (!Locate(CurrentSpace(system), CurrentSpace(m_coordinate_system), now, &location) ||
	    (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0) {
		return false;
	}
	*origin = FromXrPose(location.pose);
	return true;
}

bool Runtime::HasPlayAreaBounds() {
	if (m_session == XR_NULL_HANDLE || m_stage_space == XR_NULL_HANDLE) {
		return false;
	}
	Common::LockGuard lock(m_bounds_mutex);
	if (m_bounds_stale) {
		m_bounds_stale = false;
		m_bounds       = {};
		if (!Check(xrGetReferenceSpaceBoundsRect(m_session, XR_REFERENCE_SPACE_TYPE_STAGE,
		                                         &m_bounds),
		           "xrGetReferenceSpaceBoundsRect")) {
			m_bounds = {};
		}
		LOGF("OpenXR: play area %.2f x %.2f m\n", m_bounds.width, m_bounds.height);
	}
	return m_bounds.width > 0.0f && m_bounds.height > 0.0f;
}

bool Runtime::GetPlayAreaBounds(PlayAreaBounds* bounds) {
	EXIT_IF(bounds == nullptr);
	if (!HasPlayAreaBounds()) {
		return false;
	}
	XrExtent2Df extent {};
	{
		Common::LockGuard lock(m_bounds_mutex);
		extent = m_bounds;
	}
	Pose origin;
	if (!LocateCoordinateSystem(CoordinateSystem::Stage, &origin)) {
		return false;
	}
	*bounds = {origin, extent.width, extent.height};
	return true;
}

XrSpace Runtime::GetFrameSpace(int64_t flip_arg) {
	Common::LockGuard lock(m_spaces_mutex);
	for (const auto& frame: m_frame_spaces) {
		if (frame.used && frame.flip_arg == flip_arg) {
			if (auto space = GenerationSpace(frame.generation, frame.system);
			    space != XR_NULL_HANDLE) {
				return space;
			}
			break;
		}
	}
	return CurrentSpace(m_coordinate_system);
}

} // namespace Libs::Graphics::Xr

#endif // KYTY_OPENXR
