#include "common/logging/log.h"
#include "graphics/presentation/xr/xrRuntime.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <array>
#include <atomic>
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace Libs {

LIB_VERSION("VrTracker2", 1, "VrTracker2", 1, 1);

namespace VrTracker2 {

constexpr int32_t ERROR_NOT_INIT                  = -1972109311; // 0x8a740001
constexpr int32_t ERROR_ALREADY_INITIALIZED       = -1972109310; // 0x8a740002
constexpr int32_t ERROR_DEVICE_ALREADY_REGISTERED = -1972109308; // 0x8a740004
constexpr int32_t ERROR_DEVICE_LIMIT              = -1972109307; // 0x8a740005
constexpr int32_t ERROR_ARGUMENT_INVALID          = -1972109306; // 0x8a740006
constexpr int32_t ERROR_INVALID_DEVICE_HANDLE     = -1972109305; // 0x8a740007
constexpr int32_t ERROR_NOT_AVAILABLE             = -1972109280; // 0x8a740020
constexpr int32_t ERROR_NOT_AVAILABLE_IN_CURRENT_PLAY_AREA_MODE = -1972109279; // 0x8a740021

constexpr uint32_t DEVICE_HMD                 = 0;
constexpr uint32_t DEVICE_VR_CONTROLLER_RIGHT = 1;
constexpr uint32_t DEVICE_VR_CONTROLLER_LEFT  = 2;

constexpr uint32_t STATUS_NOT_STARTED  = 0;
constexpr uint32_t STATUS_TRACKING     = 1;
constexpr uint32_t STATUS_NOT_TRACKING = 2;

constexpr uint32_t QUALITY_NONE        = 0;
constexpr uint32_t QUALITY_NOT_VISIBLE = 3;
constexpr uint32_t QUALITY_PARTIAL     = 6;
constexpr uint32_t QUALITY_FULL        = 9;

constexpr uint32_t PLAY_AREA_MODE_ROOM_SCALE = 1;
constexpr uint32_t PLAY_AREA_MODE_STATIONARY = 2;

constexpr uint32_t BOUNDARY_CHECK_OUTSIDE = 0;
constexpr uint32_t BOUNDARY_CHECK_INSIDE  = 1;
constexpr uint32_t BOUNDARY_CHECK_UNKNOWN = 2;

constexpr uint32_t PLAY_AREA_BOUNDARY_POINTS_MAX = 256;

struct FVector3 {
	float x;
	float y;
	float z;
};

struct FQuaternion {
	float x;
	float y;
	float z;
	float w;
};

struct SceVrTracker2QueryMemoryParam {
	uint32_t sizeOfThis;
	uint32_t profile;
	uint32_t reserved[6];
};

struct SceVrTracker2QueryMemoryResult {
	uint32_t sizeOfThis;
	uint32_t memorySize;
	uint32_t memoryAlignment;
	uint32_t reserved[5];
};

struct SceVrTracker2InitializeParam {
	uint32_t sizeOfThis;
	uint32_t profile;
	int32_t  highThreadPriority;
	int32_t  lowThreadPriority;
	uint64_t highThreadAffinity;
	uint64_t lowThreadAffinity;
	void*    memoryPointer;
	uint32_t memorySize;
	uint32_t memoryAlignment;
	int32_t  maxDurationForControllerInertialTracking;
	uint32_t coordinateResetType;
	uint8_t  disableControllerOcclusionHandling;
	uint8_t  reserved[15];
};

struct SceVrTracker2PoseData {
	FVector3    position;
	uint32_t    reserved00[1];
	FQuaternion orientation;
	uint32_t    reserved01[8];
};

struct SceVrTracker2HmdInfo {
	SceVrTracker2PoseData devicePose;
	SceVrTracker2PoseData leftEyePose;
	SceVrTracker2PoseData rightEyePose;
	SceVrTracker2PoseData headPose;
	SceVrTracker2PoseData gazeOriginPose;
};

struct SceVrTracker2VrControllerInfo {
	SceVrTracker2PoseData devicePose;
	uint64_t              lastValidVisionTimestamp;
	uint64_t              lastValidSensorTimestamp;
	uint32_t              reserved[60];
};

struct SceVrTracker2GetResultParam {
	uint32_t sizeOfThis;
	int32_t  handle;
	uint32_t reserved00;
	uint64_t predictionTime;
	uint32_t reserved01;
	int64_t  flipArg;
	uint32_t reserved02[2];
};

struct SceVrTracker2ResultData {
	int32_t  handle;
	uint32_t connected;
	uint32_t playAreaUpdateCounter;
	uint32_t playAreaMode;
	uint64_t timestamp;
	uint32_t status;
	uint32_t positionQuality;
	uint32_t orientationQuality;
	uint32_t reserved01[13];
	int64_t  flipArg;
	union {
		SceVrTracker2HmdInfo          hmdInfo;
		SceVrTracker2VrControllerInfo vrControllerInfo;
	};
	uint32_t reserved02;
};

struct SceVrTracker2PlayAreaBoundaryCheckResult {
	uint32_t sizeOfThis;
	uint32_t isInsideBoundary;
	FVector3 closestPoint;
	FVector3 closestPointNormal;
	float    closestDistance;
};

struct SceVrTracker2PlayAreaBoundaryGeometryData {
	uint32_t sizeOfThis;
	uint32_t numPoints;
	FVector3 point[PLAY_AREA_BOUNDARY_POINTS_MAX];
};

struct SceVrTracker2PlayAreaOrientedBoundingBoxData {
	uint32_t              sizeOfThis;
	SceVrTracker2PoseData pose;
	FVector3              scale;
};

static_assert(sizeof(SceVrTracker2PlayAreaBoundaryCheckResult) == 36);
static_assert(sizeof(SceVrTracker2PlayAreaBoundaryGeometryData) == 3080);
static_assert(offsetof(SceVrTracker2PlayAreaOrientedBoundingBoxData, pose) == 4);
static_assert(sizeof(SceVrTracker2PlayAreaOrientedBoundingBoxData) == 80);
static_assert(sizeof(SceVrTracker2QueryMemoryParam) == 32);
static_assert(sizeof(SceVrTracker2QueryMemoryResult) == 32);
static_assert(sizeof(SceVrTracker2InitializeParam) == 72);
static_assert(offsetof(SceVrTracker2InitializeParam, memoryPointer) == 32);
static_assert(sizeof(SceVrTracker2PoseData) == 64);
static_assert(offsetof(SceVrTracker2PoseData, orientation) == 16);
static_assert(sizeof(SceVrTracker2HmdInfo) == 320);
static_assert(sizeof(SceVrTracker2VrControllerInfo) == 320);
static_assert(sizeof(SceVrTracker2GetResultParam) == 48);
static_assert(offsetof(SceVrTracker2GetResultParam, predictionTime) == 16);
static_assert(offsetof(SceVrTracker2GetResultParam, flipArg) == 32);
static_assert(sizeof(SceVrTracker2ResultData) == 424);
static_assert(offsetof(SceVrTracker2ResultData, status) == 24);
static_assert(offsetof(SceVrTracker2ResultData, flipArg) == 88);
static_assert(offsetof(SceVrTracker2ResultData, hmdInfo) == 96);

struct Device {
	bool     registered = false;
	uint32_t type       = 0;
	int32_t  handle     = 0;
};

static std::atomic<bool>     g_initialized = false;
static std::mutex            g_devices_mutex;
static std::array<Device, 3> g_devices {};

// Used when the runtime cannot tell, as by the virtual headset.
constexpr float PLAY_AREA_HEIGHT = 2.5f;
// A position or orientation untracked for longer than this has stopped being trustworthy.
constexpr uint64_t PARTIAL_QUALITY_US = 500000;

static std::atomic<uint64_t> g_hmd_position_tracked_time {0};
static std::atomic<uint64_t> g_hmd_orientation_tracked_time {0};
// Per hand, left then right: when position and orientation were last tracked.
static std::array<std::atomic<uint64_t>, 2> g_controller_position_tracked_time {};
static std::array<std::atomic<uint64_t>, 2> g_controller_orientation_tracked_time {};

static SceVrTracker2PoseData FromXrPose(const Graphics::Xr::Pose& from) {
	SceVrTracker2PoseData pose {};
	pose.position    = {from.position[0], from.position[1], from.position[2]};
	pose.orientation = {from.orientation[0], from.orientation[1], from.orientation[2],
	                    from.orientation[3]};
	return pose;
}

static Graphics::Xr::Pose ToXrPose(const SceVrTracker2PoseData& from) {
	return {{from.position.x, from.position.y, from.position.z},
	        {from.orientation.x, from.orientation.y, from.orientation.z, from.orientation.w}};
}

static FVector3 Rotate(const FQuaternion& q, const FVector3& v) {
	// v + 2w(u x v) + 2u x (u x v), with u the vector part of q.
	const FVector3 u {q.x, q.y, q.z};
	const FVector3 t {2.0f * (u.y * v.z - u.z * v.y), 2.0f * (u.z * v.x - u.x * v.z),
	                  2.0f * (u.x * v.y - u.y * v.x)};
	return {v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
	        v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
}

static FVector3 Transform(const SceVrTracker2PoseData& pose, const FVector3& v) {
	const auto r = Rotate(pose.orientation, v);
	return {pose.position.x + r.x, pose.position.y + r.y, pose.position.z + r.z};
}

static FVector3 InverseTransform(const SceVrTracker2PoseData& pose, const FVector3& v) {
	const FQuaternion inverse {-pose.orientation.x, -pose.orientation.y, -pose.orientation.z,
	                           pose.orientation.w};
	return Rotate(inverse, {v.x - pose.position.x, v.y - pose.position.y, v.z - pose.position.z});
}

static uint32_t Quality(bool valid, bool tracked, uint64_t now, std::atomic<uint64_t>& tracked_time) {
	if (tracked) {
		tracked_time = now;
		return QUALITY_FULL;
	}
	if (!valid) {
		return QUALITY_NONE;
	}
	return now - tracked_time.load() < PARTIAL_QUALITY_US ? QUALITY_PARTIAL : QUALITY_NOT_VISIBLE;
}

// The play area in the current coordinate system: the pose of its centre on the floor and its
// size along the centre's x and z.
static bool GetPlayArea(SceVrTracker2PoseData* centre, float* width, float* depth) {
	Graphics::Xr::PlayAreaBounds bounds;
	if (!Graphics::Xr::GetPlayAreaBounds(&bounds)) {
		return false;
	}
	*centre = FromXrPose(bounds.origin);
	*width  = bounds.width;
	*depth  = bounds.depth;
	return true;
}

// Polled with every result, so it must stay cheap.
static uint32_t PlayAreaMode() {
	return Graphics::Xr::HasPlayAreaBounds() ? PLAY_AREA_MODE_ROOM_SCALE : PLAY_AREA_MODE_STATIONARY;
}

// Fills the controller part of a result from the host runtime. The grip pose stands in for the
// Sense controller's device origin, which is undocumented.
static void LocateController(const SceVrTracker2GetResultParam& param, uint32_t device_type,
                             SceVrTracker2ResultData* result) {
	const auto hand  = device_type == DEVICE_VR_CONTROLLER_LEFT ? Graphics::Xr::Hand::Left
	                                                             : Graphics::Xr::Hand::Right;
	const auto index = static_cast<size_t>(hand);
	Graphics::Xr::ControllerState state;
	(void)Graphics::Xr::GetControllerState(hand, &state);
	result->connected = state.connected ? 1 : 0;

	Graphics::Xr::Location grip;
	if (!state.connected || !Graphics::Xr::LocateController(hand, param.predictionTime, &grip)) {
		result->status             = STATUS_NOT_TRACKING;
		result->positionQuality    = QUALITY_NONE;
		result->orientationQuality = QUALITY_NONE;
		return;
	}
	const auto now             = LibKernel::KernelGetProcessTime();
	result->status             = grip.orientation_valid ? STATUS_TRACKING : STATUS_NOT_TRACKING;
	result->positionQuality    = Quality(grip.position_valid, grip.position_tracked, now,
	                                     g_controller_position_tracked_time[index]);
	result->orientationQuality = Quality(grip.orientation_valid, grip.orientation_tracked, now,
	                                     g_controller_orientation_tracked_time[index]);

	auto& controller      = result->vrControllerInfo;
	controller.devicePose = FromXrPose(grip.pose);
	controller.lastValidVisionTimestamp = g_controller_position_tracked_time[index].load();
	controller.lastValidSensorTimestamp = g_controller_orientation_tracked_time[index].load();
}

// Fills the headset part of a result from the host runtime.
static void LocateHmd(const SceVrTracker2GetResultParam& param, SceVrTracker2ResultData* result) {
	Graphics::Xr::HeadLocation head;
	if (!Graphics::Xr::LocateHead(param.predictionTime, param.flipArg, &head)) {
		result->status = STATUS_NOT_STARTED;
		return;
	}
	const auto now             = LibKernel::KernelGetProcessTime();
	const auto& device         = head.device;
	result->status             = device.orientation_valid ? STATUS_TRACKING : STATUS_NOT_TRACKING;
	result->positionQuality    = Quality(device.position_valid, device.position_tracked, now,
	                                     g_hmd_position_tracked_time);
	result->orientationQuality = Quality(device.orientation_valid, device.orientation_tracked, now,
	                                     g_hmd_orientation_tracked_time);

	auto& hmd        = result->hmdInfo;
	hmd.devicePose   = FromXrPose(device.pose);
	hmd.leftEyePose  = FromXrPose(head.eyes[0]);
	hmd.rightEyePose = FromXrPose(head.eyes[1]);
	// The centre of the head sits behind and below the eyes; an estimate.
	hmd.headPose          = hmd.devicePose;
	hmd.headPose.position = Transform(hmd.devicePose, {0.0f, -0.03f, 0.09f});
	hmd.gazeOriginPose    = hmd.devicePose;
	hmd.gazeOriginPose.position = {(hmd.leftEyePose.position.x + hmd.rightEyePose.position.x) / 2.0f,
	                               (hmd.leftEyePose.position.y + hmd.rightEyePose.position.y) / 2.0f,
	                               (hmd.leftEyePose.position.z + hmd.rightEyePose.position.z) / 2.0f};
}

static int32_t KYTY_SYSV_ABI VrTracker2QueryMemory(const SceVrTracker2QueryMemoryParam* param,
                                                   SceVrTracker2QueryMemoryResult*      result) {
	PRINT_NAME();
	if (param == nullptr || result == nullptr) {
		return ERROR_ARGUMENT_INVALID;
	}
	// Tracking runs on the host; the guest buffer is accepted but never touched.
	result->memorySize      = 0x10000;
	result->memoryAlignment = 0x100;
	return OK;
}

static int32_t KYTY_SYSV_ABI VrTracker2Initialize(const SceVrTracker2InitializeParam* param) {
	PRINT_NAME();
	if (param == nullptr) {
		return ERROR_ARGUMENT_INVALID;
	}
	if (g_initialized.exchange(true)) {
		return ERROR_ALREADY_INITIALIZED;
	}
	LOGF("VrTracker2: initialized profile=%u memory=%p size=%u\n", param->profile,
	     param->memoryPointer, param->memorySize);
	return OK;
}

static int32_t KYTY_SYSV_ABI VrTracker2Finalize() {
	PRINT_NAME();
	if (!g_initialized.exchange(false)) {
		return ERROR_NOT_INIT;
	}
	std::scoped_lock lock {g_devices_mutex};
	g_devices = {};
	return OK;
}

static int32_t KYTY_SYSV_ABI VrTracker2RegisterDevice(uint32_t device_type, int32_t handle) {
	PRINT_NAME();
	if (!g_initialized.load()) {
		return ERROR_NOT_INIT;
	}
	if (device_type > DEVICE_VR_CONTROLLER_LEFT) {
		return ERROR_ARGUMENT_INVALID;
	}
	std::scoped_lock lock {g_devices_mutex};
	Device*          free_slot = nullptr;
	for (auto& device: g_devices) {
		if (device.registered && device.handle == handle) {
			return ERROR_DEVICE_ALREADY_REGISTERED;
		}
		if (!device.registered && free_slot == nullptr) {
			free_slot = &device;
		}
	}
	if (free_slot == nullptr) {
		return ERROR_DEVICE_LIMIT;
	}
	*free_slot = {true, device_type, handle};
	LOGF("VrTracker2: registered device type=%u handle=%d\n", device_type, handle);
	return OK;
}

static int32_t KYTY_SYSV_ABI VrTracker2UnregisterDevice(int32_t handle) {
	PRINT_NAME();
	if (!g_initialized.load()) {
		return ERROR_NOT_INIT;
	}
	std::scoped_lock lock {g_devices_mutex};
	for (auto& device: g_devices) {
		if (device.registered && device.handle == handle) {
			device = {};
			return OK;
		}
	}
	return ERROR_INVALID_DEVICE_HANDLE;
}

static int32_t KYTY_SYSV_ABI VrTracker2GetResult(const SceVrTracker2GetResultParam* param,
                                                 SceVrTracker2ResultData*           result) {
	if (!g_initialized.load()) {
		return ERROR_NOT_INIT;
	}
	if (param == nullptr || result == nullptr) {
		return ERROR_ARGUMENT_INVALID;
	}
	uint32_t device_type = 0;
	{
		std::scoped_lock lock {g_devices_mutex};
		const Device*    found = nullptr;
		for (const auto& device: g_devices) {
			if (device.registered && device.handle == param->handle) {
				found = &device;
			}
		}
		if (found == nullptr) {
			return ERROR_INVALID_DEVICE_HANDLE;
		}
		device_type = found->type;
	}

	*result                       = {};
	result->handle                = param->handle;
	result->connected             = 1;
	result->playAreaMode          = PlayAreaMode();
	result->playAreaUpdateCounter = Graphics::Xr::GetPlayAreaUpdateCounter();
	result->timestamp             = param->predictionTime != 0 ? param->predictionTime
	                                                           : LibKernel::KernelGetProcessTime();
	result->status                = STATUS_TRACKING;
	result->positionQuality       = QUALITY_FULL;
	result->orientationQuality    = QUALITY_FULL;
	result->flipArg               = param->flipArg;

	if (device_type == DEVICE_HMD) {
		LocateHmd(*param, result);
	} else {
		LocateController(*param, device_type, result);
	}
	return OK;
}

static bool IsRegisteredHmd(int32_t handle) {
	std::scoped_lock lock {g_devices_mutex};
	return std::any_of(g_devices.begin(), g_devices.end(), [handle](const Device& device) {
		return device.registered && device.handle == handle && device.type == DEVICE_HMD;
	});
}

static int32_t KYTY_SYSV_ABI VrTracker2GetCoordinateSystem(uint32_t* system) {
	PRINT_NAME();
	if (!g_initialized.load()) {
		return ERROR_NOT_INIT;
	}
	if (system == nullptr) {
		return ERROR_ARGUMENT_INVALID;
	}
	*system = static_cast<uint32_t>(Graphics::Xr::GetCoordinateSystem());
	return OK;
}

static int32_t KYTY_SYSV_ABI VrTracker2SetCoordinateSystem(uint32_t system) {
	PRINT_NAME();
	if (!g_initialized.load()) {
		return ERROR_NOT_INIT;
	}
	if (system > static_cast<uint32_t>(Graphics::Xr::CoordinateSystem::Stage)) {
		return ERROR_ARGUMENT_INVALID;
	}
	LOGF("VrTracker2: coordinate system %u\n", system);
	return Graphics::Xr::SetCoordinateSystem(static_cast<Graphics::Xr::CoordinateSystem>(system))
	           ? OK
	           : ERROR_NOT_AVAILABLE;
}

static bool InStage() {
	return Graphics::Xr::GetCoordinateSystem() == Graphics::Xr::CoordinateSystem::Stage;
}

static int32_t KYTY_SYSV_ABI VrTracker2ResetLocalCoordinate(int32_t handle) {
	PRINT_NAME();
	if (!g_initialized.load()) {
		return ERROR_NOT_INIT;
	}
	if (!IsRegisteredHmd(handle)) {
		return ERROR_INVALID_DEVICE_HANDLE;
	}
	if (InStage()) {
		return ERROR_NOT_AVAILABLE_IN_CURRENT_PLAY_AREA_MODE;
	}
	if (!Graphics::Xr::ResetLocalCoordinate()) {
		return ERROR_NOT_AVAILABLE;
	}
	return OK;
}

static int32_t KYTY_SYSV_ABI VrTracker2ResetLocalCoordinateWithPose(const SceVrTracker2PoseData* pose) {
	PRINT_NAME();
	if (!g_initialized.load()) {
		return ERROR_NOT_INIT;
	}
	if (pose == nullptr) {
		return ERROR_ARGUMENT_INVALID;
	}
	if (InStage()) {
		return ERROR_NOT_AVAILABLE_IN_CURRENT_PLAY_AREA_MODE;
	}
	if (!Graphics::Xr::ResetLocalCoordinate(ToXrPose(*pose))) {
		return ERROR_NOT_AVAILABLE;
	}
	return OK;
}

static int32_t KYTY_SYSV_ABI VrTracker2LocateCoordinateSystem(uint32_t               system,
                                                              SceVrTracker2PoseData* pose) {
	PRINT_NAME();
	if (!g_initialized.load()) {
		return ERROR_NOT_INIT;
	}
	if (pose == nullptr || system > static_cast<uint32_t>(Graphics::Xr::CoordinateSystem::Stage)) {
		return ERROR_ARGUMENT_INVALID;
	}
	Graphics::Xr::Pose origin;
	if (!Graphics::Xr::LocateCoordinateSystem(static_cast<Graphics::Xr::CoordinateSystem>(system),
	                                          &origin)) {
		return ERROR_NOT_AVAILABLE;
	}
	*pose = FromXrPose(origin);
	return OK;
}

// The point on the play-area rectangle closest to a point, both in the current coordinate
// system. The normal points into the play area.
static void CheckPlayAreaPoint(const FVector3& point, SceVrTracker2PlayAreaBoundaryCheckResult* r) {
	SceVrTracker2PoseData centre {};
	float                 width = 0.0f;
	float                 depth = 0.0f;
	r->sizeOfThis               = sizeof(SceVrTracker2PlayAreaBoundaryCheckResult);
	if (!GetPlayArea(&centre, &width, &depth)) {
		r->isInsideBoundary = BOUNDARY_CHECK_UNKNOWN;
		return;
	}
	const auto  local  = InverseTransform(centre, point);
	const float half_w = width / 2.0f;
	const float half_d = depth / 2.0f;
	const bool  inside = std::abs(local.x) <= half_w && std::abs(local.z) <= half_d;
	FVector3    closest {};
	FVector3    normal {};
	if (inside) {
		// The nearest wall.
		if (half_w - std::abs(local.x) < half_d - std::abs(local.z)) {
			closest = {std::copysign(half_w, local.x), local.y, local.z};
			normal  = {-std::copysign(1.0f, local.x), 0.0f, 0.0f};
		} else {
			closest = {local.x, local.y, std::copysign(half_d, local.z)};
			normal  = {0.0f, 0.0f, -std::copysign(1.0f, local.z)};
		}
	} else {
		closest = {std::clamp(local.x, -half_w, half_w), local.y,
		           std::clamp(local.z, -half_d, half_d)};
		const float dx     = local.x - closest.x;
		const float dz     = local.z - closest.z;
		const float length = std::sqrt(dx * dx + dz * dz);
		normal             = {-dx / length, 0.0f, -dz / length};
	}
	const float dx = local.x - closest.x;
	const float dz = local.z - closest.z;

	r->isInsideBoundary   = inside ? BOUNDARY_CHECK_INSIDE : BOUNDARY_CHECK_OUTSIDE;
	r->closestPoint       = Transform(centre, closest);
	r->closestPointNormal = Rotate(centre.orientation, normal);
	r->closestDistance    = std::sqrt(dx * dx + dz * dz);
}

static int32_t KYTY_SYSV_ABI VrTracker2CheckPointIsInsidePlayAreaBoundary(
    const FVector3* point, SceVrTracker2PlayAreaBoundaryCheckResult* result) {
	if (!g_initialized.load()) {
		return ERROR_NOT_INIT;
	}
	if (point == nullptr || result == nullptr) {
		return ERROR_ARGUMENT_INVALID;
	}
	CheckPlayAreaPoint(*point, result);
	return OK;
}

static int32_t KYTY_SYSV_ABI VrTracker2CheckDeviceIsInsidePlayAreaBoundary(
    int32_t handle, SceVrTracker2PlayAreaBoundaryCheckResult* result) {
	if (!g_initialized.load()) {
		return ERROR_NOT_INIT;
	}
	if (result == nullptr) {
		return ERROR_ARGUMENT_INVALID;
	}
	SceVrTracker2GetResultParam param {};
	param.sizeOfThis = sizeof(param);
	param.handle     = handle;
	SceVrTracker2ResultData data {};
	if (const auto error = VrTracker2GetResult(&param, &data); error != OK) {
		return error;
	}
	// The pose leads both result variants.
	CheckPlayAreaPoint(data.hmdInfo.devicePose.position, result);
	return OK;
}

static int32_t KYTY_SYSV_ABI
VrTracker2GetPlayAreaBoundaryGeometry(SceVrTracker2PlayAreaBoundaryGeometryData* geometry) {
	PRINT_NAME();
	if (!g_initialized.load()) {
		return ERROR_NOT_INIT;
	}
	if (geometry == nullptr) {
		return ERROR_ARGUMENT_INVALID;
	}
	SceVrTracker2PoseData centre {};
	float                 width = 0.0f;
	float                 depth = 0.0f;
	geometry->sizeOfThis        = sizeof(SceVrTracker2PlayAreaBoundaryGeometryData);
	geometry->numPoints         = 0;
	if (!GetPlayArea(&centre, &width, &depth)) {
		return ERROR_NOT_AVAILABLE_IN_CURRENT_PLAY_AREA_MODE;
	}
	// OpenXR only exposes the rectangle inside the boundary.
	const float half_w = width / 2.0f;
	const float half_d = depth / 2.0f;
	for (const auto& corner: {FVector3 {-half_w, 0.0f, -half_d}, FVector3 {half_w, 0.0f, -half_d},
	                          FVector3 {half_w, 0.0f, half_d}, FVector3 {-half_w, 0.0f, half_d}}) {
		geometry->point[geometry->numPoints++] = Transform(centre, corner);
	}
	return OK;
}

static int32_t KYTY_SYSV_ABI
VrTracker2GetPlayAreaOrientedBoundingBox(SceVrTracker2PlayAreaOrientedBoundingBoxData* box) {
	PRINT_NAME();
	if (!g_initialized.load()) {
		return ERROR_NOT_INIT;
	}
	if (box == nullptr) {
		return ERROR_ARGUMENT_INVALID;
	}
	SceVrTracker2PoseData centre {};
	float                 width = 0.0f;
	float                 depth = 0.0f;
	if (!GetPlayArea(&centre, &width, &depth)) {
		return ERROR_NOT_AVAILABLE_IN_CURRENT_PLAY_AREA_MODE;
	}
	box->sizeOfThis = sizeof(SceVrTracker2PlayAreaOrientedBoundingBoxData);
	box->pose       = centre;
	box->scale      = {width, PLAY_AREA_HEIGHT, depth};
	return OK;
}

// Generic success stub for VrTracker2 entry points with no emulated effect yet.
static int32_t KYTY_SYSV_ABI VrTracker2Stub() {
	PRINT_NAME();
	return OK;
}

} // namespace VrTracker2

LIB_DEFINE(InitVrTracker2_1) {
	// Implemented entry points.
	LIB_FUNC("TwqZnaIjWv4", VrTracker2::VrTracker2QueryMemory);       // sceVrTracker2QueryMemory
	LIB_FUNC("6Jy73SRfG-o", VrTracker2::VrTracker2Initialize);        // sceVrTracker2Initialize
	LIB_FUNC("IQ3UD6SZbXo", VrTracker2::VrTracker2Finalize);          // sceVrTracker2Finalize
	LIB_FUNC("Dog+g25QYjw", VrTracker2::VrTracker2RegisterDevice);    // sceVrTracker2RegisterDevice
	LIB_FUNC("kFt4MB3SUEk", VrTracker2::VrTracker2UnregisterDevice);  // sceVrTracker2UnregisterDevice
	LIB_FUNC("J4Vh3VVX0iU", VrTracker2::VrTracker2GetResult);         // sceVrTracker2GetResult
	LIB_FUNC("IdI2f+xHIeA", VrTracker2::VrTracker2ResetLocalCoordinate);          // sceVrTracker2ResetLocalCoordinate
	LIB_FUNC("HDlD-xE1Xuk", VrTracker2::VrTracker2ResetLocalCoordinateWithPose);  // sceVrTracker2ResetLocalCoordinateWithPose
	LIB_FUNC("Y-3JCiU9bbU", VrTracker2::VrTracker2GetCoordinateSystem);           // sceVrTracker2GetCoordinateSystem
	LIB_FUNC("UVCMLmS-Eas", VrTracker2::VrTracker2SetCoordinateSystem);           // sceVrTracker2SetCoordinateSystem
	LIB_FUNC("f7G97dWnEis", VrTracker2::VrTracker2LocateCoordinateSystem);        // sceVrTracker2LocateCoordinateSystem
	LIB_FUNC("IGdE6nsMTAY", VrTracker2::VrTracker2CheckPointIsInsidePlayAreaBoundary);   // sceVrTracker2CheckPointIsInsidePlayAreaBoundary
	LIB_FUNC("DSTergmOvvE", VrTracker2::VrTracker2CheckDeviceIsInsidePlayAreaBoundary);  // sceVrTracker2CheckDeviceIsInsidePlayAreaBoundary
	LIB_FUNC("SCph4ZbkqzU", VrTracker2::VrTracker2GetPlayAreaBoundaryGeometry);   // sceVrTracker2GetPlayAreaBoundaryGeometry
	LIB_FUNC("snYs7Nf-RKk", VrTracker2::VrTracker2GetPlayAreaOrientedBoundingBox); // sceVrTracker2GetPlayAreaOrientedBoundingBox

	// Stubbed entry points (success return, no emulated effect).
	LIB_FUNC("O0kfz4OhINI", VrTracker2::VrTracker2Stub);  // sceVrTracker2SetPoseToReprojection
}

} // namespace Libs
