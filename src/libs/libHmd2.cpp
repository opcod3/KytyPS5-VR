#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/presentation/xr/xrRuntime.h"
#include "kernel/pthread.h"
#include "libs/agc.h"
#include "libs/errno.h"
#include "libs/hmd2.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>

namespace Libs {

LIB_VERSION("Hmd2", 1, "Hmd2", 1, 1);

namespace Hmd2 {

constexpr int32_t ERROR_ALREADY_INITIALIZED = -1972240383; // 0x8a720001
constexpr int32_t ERROR_NOT_INITIALIZED     = -1972240382; // 0x8a720002
constexpr int32_t ERROR_HANDLE_INVALID      = -1972240381; // 0x8a720003
constexpr int32_t ERROR_PARAMETER_NULL      = -1972240376; // 0x8a720008
constexpr int32_t ERROR_PARAMETER_INVALID   = -1972240375; // 0x8a720009
constexpr int32_t ERROR_REPROJECTION_ALREADY_INITIALIZED = -1972240372; // 0x8a72000c
constexpr int32_t ERROR_REPROJECTION_IN_VR_MODE = -1972240356; // 0x8a72001c
constexpr int32_t ERROR_REPROJECTION_NOT_IN_MIRRORING = -1972240355; // 0x8a72001d
constexpr int32_t ERROR_REPROJECTION_IN_MIRRORING     = -1972240354; // 0x8a72001e

struct SceHmd2InitializeParam {
	void*   reserved0;
	uint8_t reserved[8];
};

struct SceHmd2DeviceInformation {
	uint32_t status;
	uint32_t reserve0;
	struct {
		struct {
			uint32_t width;
			uint32_t height;
		} panelResolution;
		struct {
			uint16_t refreshRate90Hz;
			uint16_t refreshRate120Hz;
		} flipToDisplayLatency;
	} deviceInfo;
	uint8_t hmuMount;
	uint8_t lensSeparationDistance;
	uint8_t reserve1[2];
	float   virtualImageDistance;
};

struct SceHmd2FieldOfView {
	float tanOut;
	float tanIn;
	float tanTop;
	float tanBottom;
};

struct SceHmd2ReprojectionInitializeParam {
	void*    pReprojectionBuff;
	void*    pDisplayBuff;
	int32_t  threadPriority;
	int32_t  cpuAffinityMask;
	uint32_t pipeId;
	uint32_t queueId;
	uint32_t type;
	void*    pSeeThroughBuff;
	int32_t  reprojectionTiming;
	uint32_t reserved[5];
};

struct SceHmd2ReprojectionPoseData {
	float    position[3];
	float    orientation[4];
	uint32_t reserved;
	uint64_t timestamp;
	int64_t  flipArg;
};

struct SceHmd2ReprojectionSettingVr3d {
	uint32_t                    srcSizeL[2];
	uint32_t                    srcSizeR[2];
	uint32_t                    sampler[4];
	TextureViewport             viewportL;
	TextureViewport             viewportR;
	SceHmd2FieldOfView          fovL;
	SceHmd2FieldOfView          fovR;
	SceHmd2ReprojectionPoseData pose;
	uint32_t                    blendType;
	bool                        disableReprojection;
	uint8_t                     padding[3];
	uint32_t                    fsrLutSizeL[2];
	uint32_t                    fsrLutSizeR[2];
	uint32_t                    reserved[10];
	void*                       option;
};

struct SceHmd2ReprojectionSettingUi2d {
	uint32_t        srcSize[2];
	uint32_t        sampler[4];
	TextureViewport viewport;
	uint32_t        blendType;
	float           width;
	float           height;
	float           position[3];
	float           rotation[4];
	uint32_t        fsrLutSize[2];
	uint32_t        reserved[10];
	void*           option;
};

struct SceHmd2ReprojectionSettingVr2d {
	uint32_t        srcSize[2];
	uint32_t        sampler[4];
	TextureViewport viewport;
	uint32_t        reserved[10];
	void*           option;
};

struct SceHmd2ReprojectionSetting {
	uint32_t layerType;
	uint32_t srcId;
	union {
		SceHmd2ReprojectionSettingVr3d vr3d;
		SceHmd2ReprojectionSettingUi2d ui2d;
		SceHmd2ReprojectionSettingVr2d vr2d;
		uint32_t                       raw[80];
	} data;
};

struct SceHmd2ReprojectionParamOption {
	uint32_t sizeOfThis;
	uint32_t multilayerBlendSpaceType;
	uint32_t reserved[30];
};

struct SceHmd2ReprojectionParam {
	const SceHmd2ReprojectionSetting* settings;
	int32_t                           settingCount;
	SceHmd2ReprojectionParamOption*   option;
};

struct SceHmd2GazeVector {
	float x;
	float y;
	float z;
};

struct SceHmd2GazeGetResultParam {
	uint32_t sizeOfThis;
	int32_t  handle;
	int64_t  flipArg;
};

struct SceHmd2GazeEyeResult {
	uint32_t isPupilDiameterValid;
	float    pupilDiameter;
	uint32_t isBlink;
};

struct SceHmd2GazeResult {
	uint32_t             sizeOfThis;
	int32_t              status;
	uint64_t             timestamp;
	uint32_t             isGazeValid;
	SceHmd2GazeVector    gazeDirection;
	SceHmd2GazeEyeResult left;
	SceHmd2GazeEyeResult right;
};

struct SceHmd2GazeResultForFoveatedRendering {
	uint32_t          sizeOfThis;
	int32_t           status;
	SceHmd2GazeVector gazeDirection;
	int32_t           reserved;
};

struct SceHmd2ReprojectionMirroringParam {
	void* pWorkMemory;
	void* pDisplayBuffer;
};

constexpr uint32_t MIRRORING_AREA_WIDE   = 0;
constexpr uint32_t MIRRORING_AREA_CENTER = 1;

struct SceHmd2ReprojectionMirroringOption {
	uint32_t area;
	uint32_t reserved[7];
};

constexpr int32_t           GAZE_TRACKING_STATUS_ENABLE = 0;
constexpr SceHmd2GazeVector GAZE_FORWARD                = {0.0f, 0.0f, -1.0f};

static_assert(sizeof(SceHmd2InitializeParam) == 16);
static_assert(offsetof(SceHmd2InitializeParam, reserved) == 8);
static_assert(sizeof(SceHmd2DeviceInformation) == 28);
static_assert(offsetof(SceHmd2DeviceInformation, deviceInfo) == 8);
static_assert(offsetof(SceHmd2DeviceInformation, hmuMount) == 20);
static_assert(offsetof(SceHmd2DeviceInformation, virtualImageDistance) == 24);
static_assert(sizeof(SceHmd2FieldOfView) == 16);
static_assert(alignof(SceHmd2FieldOfView) == 4);
static_assert(sizeof(SceHmd2ReprojectionInitializeParam) == 72);
static_assert(alignof(SceHmd2ReprojectionInitializeParam) == 8);
static_assert(offsetof(SceHmd2ReprojectionInitializeParam, threadPriority) == 16);
static_assert(offsetof(SceHmd2ReprojectionInitializeParam, type) == 32);
static_assert(offsetof(SceHmd2ReprojectionInitializeParam, pSeeThroughBuff) == 40);
static_assert(offsetof(SceHmd2ReprojectionInitializeParam, reprojectionTiming) == 48);
static_assert(offsetof(SceHmd2ReprojectionInitializeParam, reserved) == 52);
static_assert(sizeof(SceHmd2ReprojectionPoseData) == 48);
static_assert(sizeof(SceHmd2ReprojectionSettingVr3d) == 216);
static_assert(offsetof(SceHmd2ReprojectionSettingVr3d, pose) == 96);
static_assert(offsetof(SceHmd2ReprojectionSettingVr3d, blendType) == 144);
static_assert(sizeof(SceHmd2ReprojectionSettingUi2d) == 136);
static_assert(sizeof(SceHmd2ReprojectionSettingVr2d) == 88);
static_assert(sizeof(SceHmd2ReprojectionSetting) == 328);
static_assert(offsetof(SceHmd2ReprojectionSetting, data) == 8);
static_assert(sizeof(SceHmd2ReprojectionParam) == 24);
static_assert(sizeof(SceHmd2GazeGetResultParam) == 16);
static_assert(sizeof(SceHmd2GazeResult) == 56);
static_assert(offsetof(SceHmd2GazeResult, gazeDirection) == 20);
static_assert(sizeof(SceHmd2GazeResultForFoveatedRendering) == 24);
static_assert(sizeof(SceHmd2ReprojectionMirroringParam) == 16);
static_assert(sizeof(SceHmd2ReprojectionMirroringOption) == 32);

static std::atomic<bool> g_initialized = false;
static std::mutex g_reprojection_mutex;
static std::atomic<ReprojectionState*> g_reprojection_state {nullptr};
static std::mutex g_frame_settings_mutex;
static std::array<FrameSettings, REPROJECTION_MAX_DISPLAY_BUFFERS> g_frame_settings {};
static std::atomic<bool>     g_mirroring = false;
static std::atomic<uint32_t> g_mirroring_area {MIRRORING_AREA_WIDE};

ReprojectionState* GetReprojectionState() {
	return g_reprojection_state.load(std::memory_order_acquire);
}

Mirroring GetMirroring() {
	if (!g_mirroring.load(std::memory_order_acquire)) {
		return Mirroring::Off;
	}
	return g_mirroring_area.load(std::memory_order_relaxed) == MIRRORING_AREA_CENTER
	           ? Mirroring::Center
	           : Mirroring::Wide;
}

bool GetReprojectionFrameSettings(uint32_t index, FrameSettings* settings) {
	if (index >= REPROJECTION_MAX_DISPLAY_BUFFERS) {
		return false;
	}
	std::scoped_lock lock {g_frame_settings_mutex};
	if (g_frame_settings[index].count == 0) {
		return false;
	}
	*settings = g_frame_settings[index];
	return true;
}

static Graphics::SizeAlign ReprojectionDisplaySize() {
	// The host display target is one uncompressed tiled 10-bit panel surface.
	Graphics::TileSizeAlign size {};
	Graphics::TileGetTextureTotalSize(Graphics::Prospero::BufferFormat::k10_10_10_2UNorm,
	                                  PANEL_WIDTH, PANEL_HEIGHT, 1, 1,
	                                  Graphics::Prospero::TileMode::kRenderTarget, false, size);
	EXIT_IF(size.size == 0 || size.align == 0);
	return {size.size, size.align};
}

static int32_t KYTY_SYSV_ABI Hmd2Initialize(const SceHmd2InitializeParam* param) {
	PRINT_NAME();
	if (param == nullptr) {
		return ERROR_PARAMETER_NULL;
	}
	if (g_initialized.exchange(true)) {
		return ERROR_ALREADY_INITIALIZED;
	}
	LOGF("Hmd2: initialized, virtual headset=%s\n", Config::VrEnabled() ? "enabled" : "disabled");
	return OK;
}

static void FillDeviceInformation(SceHmd2DeviceInformation* info) {
	*info = {};
	if (Graphics::Xr::IsActive()) {
		info->status   = Graphics::Xr::IsHeadsetReady() ? 0 : 1; // READY : NOT_READY
		info->hmuMount = Graphics::Xr::IsUserPresent() ? 1 : 0;
		float separation = 0.063f;
		(void)Graphics::Xr::GetEyeSeparation(&separation);
		info->lensSeparationDistance = static_cast<uint8_t>(std::lround(separation * 1000.0f));
		// Games size their render targets for the PSVR2 panel; the runtime resamples it.
		info->deviceInfo.panelResolution = {PANEL_WIDTH, PANEL_HEIGHT};
		info->virtualImageDistance       = 2.0f;
	} else if (Config::VrEnabled()) {
		// Virtual headset defaults: READY, MOUNT, 63 mm lens separation.
		info->hmuMount               = 1;
		info->lensSeparationDistance = 63;
		// Headset characteristics: 4000x2040 panel and 2 m virtual image distance.
		info->deviceInfo.panelResolution = {PANEL_WIDTH, PANEL_HEIGHT};
		info->virtualImageDistance       = 2.0f;
		// Host presentation has no emulated headset scan-out delay; latency remains zero.
	} else {
		info->status = 2; // SCE_HMD2_DEVICE_STATUS_NOT_DETECTED
	}
}

static int32_t KYTY_SYSV_ABI Hmd2GetDeviceInformation(SceHmd2DeviceInformation* info) {
	PRINT_NAME();
	if (!g_initialized.load()) {
		return ERROR_NOT_INITIALIZED;
	}
	if (info == nullptr) {
		return ERROR_PARAMETER_NULL;
	}
	FillDeviceInformation(info);
	LOGF("Hmd2: device status=%u panel=%ux%u mounted=%u\n", info->status,
	     info->deviceInfo.panelResolution.width, info->deviceInfo.panelResolution.height,
	     info->hmuMount);
	return OK;
}

// The host headset's field of view, once its first frame has located the views; until then, and
// with the virtual headset, the ER15 one.
static void GetFieldOfView(SceHmd2FieldOfView* fov) {
	Graphics::Xr::EyeFov host {};
	if (Graphics::Xr::IsActive() && Graphics::Xr::GetFieldOfView(&host)) {
		*fov = {host.tan_out, host.tan_in, host.tan_top, host.tan_bottom};
		return;
	}
	*fov = {DEFAULT_FIELD_OF_VIEW.tan_out, DEFAULT_FIELD_OF_VIEW.tan_in,
	        DEFAULT_FIELD_OF_VIEW.tan_top, DEFAULT_FIELD_OF_VIEW.tan_bottom};
}

static int32_t KYTY_SYSV_ABI Hmd2GetFieldOfViewWithoutHandle(SceHmd2FieldOfView* fov) {
	PRINT_NAME();
	if (!g_initialized.load()) {
		return ERROR_NOT_INITIALIZED;
	}
	if (fov == nullptr) {
		return ERROR_PARAMETER_NULL;
	}
	GetFieldOfView(fov);
	LOGF("Hmd2: FOV tangents out=%f in=%f top=%f bottom=%f\n", fov->tanOut, fov->tanIn,
	     fov->tanTop, fov->tanBottom);
	return OK;
}

static Graphics::SizeAlign KYTY_SYSV_ABI Hmd2ReprojectionQueryBufferSizeAlign() {
	const Graphics::SizeAlign size {sizeof(ReprojectionState), alignof(ReprojectionState)};
	LOGF("Hmd2: reprojection work size=%" PRIu64 " alignment=%zu\n", size.m_size, size.m_align);
	return size;
}

static Graphics::SizeAlign KYTY_SYSV_ABI Hmd2ReprojectionQueryDisplayBufferSizeAlign() {
	const auto size = ReprojectionDisplaySize();
	LOGF("Hmd2: reprojection display size=%" PRIu64 " alignment=%zu\n", size.m_size,
	     size.m_align);
	return size;
}

static int32_t KYTY_SYSV_ABI
Hmd2ReprojectionInitialize(const SceHmd2ReprojectionInitializeParam* param, void*) {
	std::scoped_lock lock {g_reprojection_mutex};
	if (GetReprojectionState() != nullptr) {
		return ERROR_REPROJECTION_ALREADY_INITIALIZED;
	}
	if (param == nullptr || param->pReprojectionBuff == nullptr || param->pDisplayBuff == nullptr) {
		return ERROR_PARAMETER_NULL;
	}
	EXIT_NOT_IMPLEMENTED(param->pSeeThroughBuff != nullptr);
	const auto display_size = ReprojectionDisplaySize();
	auto* state = new (param->pReprojectionBuff) ReprojectionState;
	if (param->reprojectionTiming != 0) {
		state->timing_us.store(static_cast<uint32_t>(param->reprojectionTiming),
		                      std::memory_order_relaxed);
	}
	g_reprojection_state.store(state, std::memory_order_release);
	LOGF("Hmd2: reprojection initialized work=%p display=%p size=%" PRIu64 " timing=%u\n",
	     static_cast<void*>(state), param->pDisplayBuff, display_size.m_size,
	     state->timing_us.load(std::memory_order_relaxed));
	return OK;
}

static int32_t KYTY_SYSV_ABI Hmd2ReprojectionEnableVrMode(uint64_t output_mode) {
	auto* state = GetReprojectionState();
	if (state == nullptr) {
		return ERROR_REPROJECTION_NOT_INITIALIZED;
	}
	if (output_mode != OUTPUT_MODE_89_91HZ && output_mode != OUTPUT_MODE_119_88HZ) {
		return VideoOut::VIDEO_OUT_ERROR_INVALID_VALUE;
	}
	if (!Config::VrEnabled()) {
		return VideoOut::VIDEO_OUT_ERROR_NO_DEVICE;
	}
	uint64_t expected = 0;
	if (!state->output_mode.compare_exchange_strong(expected, output_mode)) {
		return ERROR_REPROJECTION_IN_VR_MODE;
	}
	LOGF("Hmd2: VR output mode=0x%" PRIx64 "\n", output_mode);
	if (Graphics::Xr::IsActive()) {
		Graphics::Xr::RequestRefreshRate(OutputModeRefreshRate(output_mode));
	}
	return OK;
}

static int32_t KYTY_SYSV_ABI Hmd2ReprojectionSetTiming(int32_t timing) {
	auto* state = GetReprojectionState();
	if (state == nullptr) {
		return ERROR_REPROJECTION_NOT_INITIALIZED;
	}
	EXIT_NOT_IMPLEMENTED(timing < 2000 || timing > 7000);
	const auto rounded = static_cast<uint32_t>((timing + 99) / 100 * 100);
	state->timing_us.store(rounded, std::memory_order_release);
	LOGF("Hmd2: reprojection timing=%u us\n", rounded);
	return OK;
}

// A fake device handle handed back by Hmd2Open. There is only one virtual headset.
constexpr int32_t FAKE_HANDLE = 1;

// Generic success stub for Hmd2 entry points that have no emulated hardware effect. The game
// only needs a non-error return so VR initialization proceeds; nothing is modeled here yet.
static int32_t KYTY_SYSV_ABI Hmd2Stub() {
	PRINT_NAME();
	return OK;
}

static int32_t KYTY_SYSV_ABI Hmd2Open() {
	PRINT_NAME();
	if (!g_initialized.load()) {
		return ERROR_NOT_INITIALIZED;
	}
	LOGF("Hmd2: open -> handle=%d\n", FAKE_HANDLE);
	return FAKE_HANDLE;
}

// Polled every frame, so it does not log.
static int32_t KYTY_SYSV_ABI Hmd2GetDeviceInformationByHandle(int32_t                   handle,
                                                              SceHmd2DeviceInformation* info) {
	if (!g_initialized.load()) {
		return ERROR_NOT_INITIALIZED;
	}
	if (handle != FAKE_HANDLE) {
		return ERROR_HANDLE_INVALID;
	}
	if (info == nullptr) {
		return ERROR_PARAMETER_NULL;
	}
	FillDeviceInformation(info);
	return OK;
}

static int32_t KYTY_SYSV_ABI Hmd2GetFieldOfView([[maybe_unused]] int32_t handle,
                                                SceHmd2FieldOfView* fov) {
	PRINT_NAME();
	if (!g_initialized.load()) {
		return ERROR_NOT_INITIALIZED;
	}
	if (fov == nullptr) {
		return ERROR_PARAMETER_NULL;
	}
	GetFieldOfView(fov);
	LOGF("Hmd2: FOV (handle) tangents out=%f in=%f top=%f bottom=%f\n", fov->tanOut, fov->tanIn,
	     fov->tanTop, fov->tanBottom);
	return OK;
}

static int32_t KYTY_SYSV_ABI Hmd2ReprojectionGetPredictedDisplayTime(int64_t   flip_arg,
                                                                     uint64_t* timestamp) {
	if (GetReprojectionState() == nullptr) {
		return ERROR_REPROJECTION_NOT_INITIALIZED;
	}
	if (timestamp == nullptr) {
		return ERROR_PARAMETER_NULL;
	}
	if (Graphics::Xr::IsActive()) {
		*timestamp = Graphics::Xr::GetPredictedDisplayTime(flip_arg);
		return OK;
	}
	// No scan-out is emulated; predict one 120 Hz refresh ahead of now.
	*timestamp = LibKernel::KernelGetProcessTime() + 1000000 / 120;
	return OK;
}

// Binds flip_arg to the headset refresh it will be shown at. Polled every frame, so it does not
// log.
static int32_t KYTY_SYSV_ABI Hmd2ReprojectionBeginFrame(int64_t flip_arg) {
	if (GetReprojectionState() == nullptr) {
		return ERROR_REPROJECTION_NOT_INITIALIZED;
	}
	if (Graphics::Xr::IsActive()) {
		(void)Graphics::Xr::BeginFrame(flip_arg);
	}
	return OK;
}

static int32_t KYTY_SYSV_ABI Hmd2GazeGetResult(const SceHmd2GazeGetResultParam* param,
                                               SceHmd2GazeResult*               result) {
	if (!g_initialized.load()) {
		return ERROR_NOT_INITIALIZED;
	}
	if (param == nullptr || result == nullptr) {
		return ERROR_PARAMETER_NULL;
	}
	// No eye tracking: a valid gaze straight ahead with open eyes.
	*result               = {};
	result->sizeOfThis    = sizeof(SceHmd2GazeResult);
	result->status        = GAZE_TRACKING_STATUS_ENABLE;
	result->timestamp     = LibKernel::KernelGetProcessTime();
	result->isGazeValid   = 1;
	result->gazeDirection = GAZE_FORWARD;
	return OK;
}

static int32_t KYTY_SYSV_ABI Hmd2GazeGetResultForFoveatedRendering(
    const SceHmd2GazeGetResultParam* param, SceHmd2GazeResultForFoveatedRendering* result) {
	if (!g_initialized.load()) {
		return ERROR_NOT_INITIALIZED;
	}
	if (param == nullptr || result == nullptr) {
		return ERROR_PARAMETER_NULL;
	}
	*result               = {};
	result->sizeOfThis    = sizeof(SceHmd2GazeResultForFoveatedRendering);
	result->status        = GAZE_TRACKING_STATUS_ENABLE;
	result->gazeDirection = GAZE_FORWARD;
	return OK;
}

static RenderPose MakeRenderPose(const SceHmd2ReprojectionPoseData& data, bool head_locked) {
	RenderPose pose {};
	pose.position    = {data.position[0], data.position[1], data.position[2]};
	pose.orientation = {data.orientation[0], data.orientation[1], data.orientation[2],
	                    data.orientation[3]};
	pose.flip_arg    = data.flipArg;
	pose.head_locked = head_locked;
	// An all-zero quaternion means the guest left the pose unset.
	const float length = pose.orientation[0] * pose.orientation[0] +
	                     pose.orientation[1] * pose.orientation[1] +
	                     pose.orientation[2] * pose.orientation[2] +
	                     pose.orientation[3] * pose.orientation[3];
	pose.valid = length > 0.5f && length < 1.5f;
	return pose;
}

static bool IsValidLayerBlend(uint32_t blend_type) {
	return blend_type == LAYER_BLEND_INTERPOLATED_ALPHA ||
	       blend_type == LAYER_BLEND_PREMULTIPLIED_ALPHA || blend_type == LAYER_BLEND_SIMPLE_ADD ||
	       blend_type == LAYER_BLEND_LAYER0;
}

// Records the layer settings for one display buffer; the video-out headset bus composites them
// when that buffer is flipped. Polled every frame, so it does not log.
static int32_t KYTY_SYSV_ABI Hmd2ReprojectionSetParam(int32_t index, int64_t flip_arg,
                                                      const SceHmd2ReprojectionParam* param,
                                                      void*                           option) {
	if (GetReprojectionState() == nullptr) {
		return ERROR_REPROJECTION_NOT_INITIALIZED;
	}
	if (param == nullptr || param->settings == nullptr) {
		return ERROR_PARAMETER_NULL;
	}
	if (index < 0 || index >= static_cast<int32_t>(REPROJECTION_MAX_DISPLAY_BUFFERS) ||
	    param->settingCount < 1 ||
	    param->settingCount > static_cast<int32_t>(REPROJECTION_MAX_LAYERS) || option != nullptr) {
		return ERROR_PARAMETER_INVALID;
	}
	// Layers are blended on the stored values, which only matches gamma-space blending.
	EXIT_NOT_IMPLEMENTED(param->option != nullptr && param->option->multilayerBlendSpaceType != 0);

	FrameSettings frame {flip_arg, static_cast<uint32_t>(param->settingCount)};
	for (uint32_t i = 0; i < frame.count; i++) {
		const auto& setting = param->settings[i];
		auto&       layer   = frame.layers[i];
		layer.layer_type    = setting.layerType;
		layer.src_id        = setting.srcId;
		switch (setting.layerType) {
			case LAYER_TYPE_VR3D:
				layer.blend_type = setting.data.vr3d.blendType;
				layer.viewport   = {setting.data.vr3d.viewportL, setting.data.vr3d.viewportR};
				for (uint32_t eye = 0; eye < 2; eye++) {
					const auto& fov = eye == 0 ? setting.data.vr3d.fovL : setting.data.vr3d.fovR;
					layer.fov[eye]  = {fov.tanOut, fov.tanIn, fov.tanTop, fov.tanBottom};
				}
				layer.pose = MakeRenderPose(setting.data.vr3d.pose,
				                            setting.data.vr3d.disableReprojection);
				break;
			case LAYER_TYPE_UI2D:
				layer.blend_type = setting.data.ui2d.blendType;
				layer.viewport   = {setting.data.ui2d.viewport, setting.data.ui2d.viewport};
				break;
			case LAYER_TYPE_VR2D:
				layer.blend_type = LAYER_BLEND_LAYER0;
				layer.viewport   = {setting.data.vr2d.viewport, setting.data.vr2d.viewport};
				break;
			default: return ERROR_PARAMETER_INVALID;
		}
		if (layer.src_id >= REPROJECTION_MAX_LAYERS || !IsValidLayerBlend(layer.blend_type)) {
			return ERROR_PARAMETER_INVALID;
		}
	}

	std::scoped_lock lock {g_frame_settings_mutex};
	g_frame_settings[static_cast<uint32_t>(index)] = frame;
	return OK;
}

// The main window mirrors the headset from host memory, so the guest buffers are only tokens.
static int32_t KYTY_SYSV_ABI
Hmd2ReprojectionEnableMirroring(const SceHmd2ReprojectionMirroringParam* param) {
	if (GetReprojectionState() == nullptr) {
		return ERROR_REPROJECTION_NOT_INITIALIZED;
	}
	if (param == nullptr || param->pWorkMemory == nullptr || param->pDisplayBuffer == nullptr) {
		return ERROR_PARAMETER_NULL;
	}
	if (g_mirroring.exchange(true, std::memory_order_acq_rel)) {
		return ERROR_REPROJECTION_IN_MIRRORING;
	}
	LOGF("Hmd2: mirroring enabled\n");
	return OK;
}

static int32_t KYTY_SYSV_ABI Hmd2ReprojectionDisableMirroring() {
	if (GetReprojectionState() == nullptr) {
		return ERROR_REPROJECTION_NOT_INITIALIZED;
	}
	if (!g_mirroring.exchange(false, std::memory_order_acq_rel)) {
		return ERROR_REPROJECTION_NOT_IN_MIRRORING;
	}
	LOGF("Hmd2: mirroring disabled\n");
	return OK;
}

static int32_t KYTY_SYSV_ABI
Hmd2ReprojectionSetMirroringOption(const SceHmd2ReprojectionMirroringOption* option) {
	if (option == nullptr) {
		return ERROR_PARAMETER_NULL;
	}
	if (option->area != MIRRORING_AREA_WIDE && option->area != MIRRORING_AREA_CENTER) {
		return ERROR_PARAMETER_INVALID;
	}
	g_mirroring_area.store(option->area, std::memory_order_relaxed);
	LOGF("Hmd2: mirroring area=%u\n", option->area);
	return OK;
}

static Graphics::SizeAlign KYTY_SYSV_ABI Hmd2ReprojectionGetMirroringWorkMemorySizeAlign() {
	// The main window mirrors the headset from host memory; hand back a small, suitably aligned
	// allocation so the game's buffer bookkeeping succeeds.
	const Graphics::SizeAlign size {0x1000, 0x100};
	LOGF("Hmd2: mirroring work size=%" PRIu64 " alignment=%zu\n", size.m_size, size.m_align);
	return size;
}

static Graphics::SizeAlign KYTY_SYSV_ABI Hmd2ReprojectionGetMirroringDisplayBufferSizeAlign() {
	const auto size = ReprojectionDisplaySize();
	LOGF("Hmd2: mirroring display size=%" PRIu64 " alignment=%zu\n", size.m_size, size.m_align);
	return size;
}

} // namespace Hmd2

LIB_DEFINE(InitHmd2_1) {
	// Implemented entry points.
	LIB_FUNC("c812oYs7Vsc", Hmd2::Hmd2Initialize);                           // sceHmd2Initialize
	LIB_FUNC("bIi4YUfSRys", Hmd2::Hmd2GetDeviceInformation);                 // sceHmd2GetDeviceInformation
	LIB_FUNC("gF8+lvc7GuQ", Hmd2::Hmd2GetFieldOfViewWithoutHandle);          // sceHmd2GetFieldOfViewWithoutHandle
	LIB_FUNC("U-CnbmeyYaA", Hmd2::Hmd2ReprojectionQueryBufferSizeAlign);     // sceHmd2ReprojectionQueryBufferSizeAlign
	LIB_FUNC("-C2nkoEYOnU", Hmd2::Hmd2ReprojectionQueryDisplayBufferSizeAlign); // sceHmd2ReprojectionQueryDisplayBufferSizeAlign
	LIB_FUNC("C0rPwER-yxg", Hmd2::Hmd2ReprojectionInitialize);               // sceHmd2ReprojectionInitialize
	LIB_FUNC("VVvFh51o20s", Hmd2::Hmd2ReprojectionEnableVrMode);             // sceHmd2ReprojectionEnableVrMode
	LIB_FUNC("FkQX7rjFomk", Hmd2::Hmd2ReprojectionSetTiming);                // sceHmd2ReprojectionSetTiming
	LIB_FUNC("4BlE4IPXP0Q", Hmd2::Hmd2GetDeviceInformationByHandle);         // sceHmd2GetDeviceInformationByHandle
	LIB_FUNC("SVEG+1D7qHA", Hmd2::Hmd2ReprojectionGetPredictedDisplayTime);  // sceHmd2ReprojectionGetPredictedDisplayTime
	LIB_FUNC("lAoFUedcfqA", Hmd2::Hmd2GazeGetResult);                        // sceHmd2GazeGetResult
	LIB_FUNC("retc+-uRMhk", Hmd2::Hmd2GazeGetResultForFoveatedRendering);    // sceHmd2GazeGetResultForFoveatedRendering
	LIB_FUNC("xMo9ENEu2E0", Hmd2::Hmd2ReprojectionSetParam);                 // sceHmd2ReprojectionSetParam
	LIB_FUNC("Ocf081WpBpA", Hmd2::Hmd2ReprojectionBeginFrame);               // sceHmd2ReprojectionBeginFrame

	// Stubbed entry points (success return, no emulated effect).
	LIB_FUNC("f3kPeoTZnIE", Hmd2::Hmd2Open);                                 // sceHmd2Open
	LIB_FUNC("E+b+c1Idyew", Hmd2::Hmd2GetFieldOfView);                       // sceHmd2GetFieldOfView
	LIB_FUNC("Pb1d+j-bBSc", Hmd2::Hmd2ReprojectionGetMirroringWorkMemorySizeAlign);    // sceHmd2ReprojectionGetMirroringWorkMemorySizeAlign
	LIB_FUNC("wEO+gMHs9NU", Hmd2::Hmd2ReprojectionGetMirroringDisplayBufferSizeAlign); // sceHmd2ReprojectionGetMirroringDisplayBufferSizeAlign
	LIB_FUNC("Qx7SkcgAzok", Hmd2::Hmd2ReprojectionEnableMirroring);          // sceHmd2ReprojectionEnableMirroring
	LIB_FUNC("asilo8VNGvg", Hmd2::Hmd2ReprojectionDisableMirroring);         // sceHmd2ReprojectionDisableMirroring
	LIB_FUNC("+seEJVlljr0", Hmd2::Hmd2ReprojectionSetMirroringOption);       // sceHmd2ReprojectionSetMirroringOption

	LIB_FUNC("QU2M1pPNbaY", Hmd2::Hmd2Stub);  // sceHmd2Terminate
	LIB_FUNC("oPhtjySuHa8", Hmd2::Hmd2Stub);  // sceHmd2Close
	LIB_FUNC("Acyg4vTdGm0", Hmd2::Hmd2Stub);  // sceHmd2ReprojectionGetCurrentOutput
	LIB_FUNC("Al4qjNREVQQ", Hmd2::Hmd2Stub);  // sceHmd2SetVibration
	LIB_FUNC("4Q11W4M2h5Q", Hmd2::Hmd2Stub);  // sceHmd2ReprojectionTerminate
	LIB_FUNC("DIwqTkcZiJo", Hmd2::Hmd2Stub);  // sceHmd2ReprojectionSetUserEventStart
	LIB_FUNC("z0+0gXUJWs8", Hmd2::Hmd2Stub);  // sceHmd2ReprojectionClearUserEventStart
	LIB_FUNC("64ERZWVFBTE", Hmd2::Hmd2Stub);  // sceHmd2ReprojectionSetUserEventEnd
	LIB_FUNC("rjAd4nCY4DM", Hmd2::Hmd2Stub);  // sceHmd2ReprojectionClearUserEventEnd
	LIB_FUNC("hA9LshbSkzw", Hmd2::Hmd2Stub);  // sceHmd2ReprojectionSetRenderConfig
	LIB_FUNC("wj1kOyNF4vM", Hmd2::Hmd2Stub);  // sceHmd2ReprojectionDisableVrMode
	LIB_FUNC("8GkaY2B7opM", Hmd2::Hmd2Stub);  // sceHmd2ReprojectionGetStatus
}

namespace Hmd2Positional {

LIB_VERSION("Hmd2ReprojectionPositional", 1, "Hmd2", 1, 1);

// Newer than SDK 4.00, so the parameter's meaning is unknown; Horizon Call of the Mountain passes 2.
// Whether positional reprojection happens is the host runtime's choice, so this only logs.
static int32_t KYTY_SYSV_ABI Hmd2ReprojectionSetAllowPositionalReprojection(int32_t mode) {
	PRINT_NAME();
	LOGF("Hmd2: allow positional reprojection mode=%d\n", mode);
	return OK;
}

LIB_DEFINE(InitHmd2ReprojectionPositional_1) {
	LIB_FUNC("uv0Ae+jCeWY", Hmd2ReprojectionSetAllowPositionalReprojection); // sceHmd2ReprojectionSetAllowPositionalReprojection
}

} // namespace Hmd2Positional

} // namespace Libs
