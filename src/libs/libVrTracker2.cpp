#include "common/logging/log.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <array>
#include <atomic>
#include <cinttypes>
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

constexpr uint32_t DEVICE_HMD                 = 0;
constexpr uint32_t DEVICE_VR_CONTROLLER_RIGHT = 1;
constexpr uint32_t DEVICE_VR_CONTROLLER_LEFT  = 2;

constexpr uint32_t STATUS_TRACKING           = 1;
constexpr uint32_t QUALITY_FULL              = 9;
constexpr uint32_t PLAY_AREA_MODE_ROOM_SCALE = 1;

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

// Fixed virtual poses in the LOCAL_FLOOR coordinate system: a standing user looking down -Z
// with both hands held forward. Nothing moves until host input is wired to tracking.
constexpr float       HEAD_HEIGHT = 1.6f;
constexpr float       HALF_IPD    = 0.0315f;
constexpr FQuaternion IDENTITY    = {0.0f, 0.0f, 0.0f, 1.0f};
constexpr FVector3    HAND_LEFT   = {-0.2f, 1.2f, -0.3f};
constexpr FVector3    HAND_RIGHT  = {0.2f, 1.2f, -0.3f};

static SceVrTracker2PoseData MakePose(FVector3 position) {
	SceVrTracker2PoseData pose {};
	pose.position    = position;
	pose.orientation = IDENTITY;
	return pose;
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

	*result                    = {};
	result->handle             = param->handle;
	result->connected          = 1;
	result->playAreaMode       = PLAY_AREA_MODE_ROOM_SCALE;
	result->timestamp          = param->predictionTime != 0 ? param->predictionTime
	                                                        : LibKernel::KernelGetProcessTime();
	result->status             = STATUS_TRACKING;
	result->positionQuality    = QUALITY_FULL;
	result->orientationQuality = QUALITY_FULL;
	result->flipArg            = param->flipArg;

	if (device_type == DEVICE_HMD) {
		auto& hmd          = result->hmdInfo;
		hmd.devicePose     = MakePose({0.0f, HEAD_HEIGHT, 0.0f});
		hmd.leftEyePose    = MakePose({-HALF_IPD, HEAD_HEIGHT, 0.0f});
		hmd.rightEyePose   = MakePose({HALF_IPD, HEAD_HEIGHT, 0.0f});
		hmd.headPose       = MakePose({0.0f, HEAD_HEIGHT, 0.0f});
		hmd.gazeOriginPose = MakePose({0.0f, HEAD_HEIGHT, 0.0f});
	} else {
		auto& controller = result->vrControllerInfo;
		controller.devicePose =
		    MakePose(device_type == DEVICE_VR_CONTROLLER_LEFT ? HAND_LEFT : HAND_RIGHT);
		controller.lastValidVisionTimestamp = result->timestamp;
		controller.lastValidSensorTimestamp = result->timestamp;
	}
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

	// Stubbed entry points (success return, no emulated effect).
	LIB_FUNC("O0kfz4OhINI", VrTracker2::VrTracker2Stub);  // sceVrTracker2SetPoseToReprojection
	LIB_FUNC("IdI2f+xHIeA", VrTracker2::VrTracker2Stub);  // sceVrTracker2ResetLocalCoordinate
	LIB_FUNC("HDlD-xE1Xuk", VrTracker2::VrTracker2Stub);  // sceVrTracker2ResetLocalCoordinateWithPose
	LIB_FUNC("Y-3JCiU9bbU", VrTracker2::VrTracker2Stub);  // sceVrTracker2GetCoordinateSystem
	LIB_FUNC("UVCMLmS-Eas", VrTracker2::VrTracker2Stub);  // sceVrTracker2SetCoordinateSystem
	LIB_FUNC("f7G97dWnEis", VrTracker2::VrTracker2Stub);  // sceVrTracker2LocateCoordinateSystem
	LIB_FUNC("IGdE6nsMTAY", VrTracker2::VrTracker2Stub);  // sceVrTracker2CheckPointIsInsidePlayAreaBoundary
	LIB_FUNC("DSTergmOvvE", VrTracker2::VrTracker2Stub);  // sceVrTracker2CheckDeviceIsInsidePlayAreaBoundary
	LIB_FUNC("SCph4ZbkqzU", VrTracker2::VrTracker2Stub);  // sceVrTracker2GetPlayAreaBoundaryGeometry
	LIB_FUNC("snYs7Nf-RKk", VrTracker2::VrTracker2Stub);  // sceVrTracker2GetPlayAreaOrientedBoundingBox
}

} // namespace Libs
