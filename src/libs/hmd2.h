#ifndef EMULATOR_SRC_LIBS_HMD2_H_
#define EMULATOR_SRC_LIBS_HMD2_H_

#include <array>
#include <atomic>
#include <cstdint>

namespace Libs::Hmd2 {

inline constexpr uint32_t PANEL_WIDTH  = 4000;
inline constexpr uint32_t PANEL_HEIGHT = 2040;
inline constexpr uint64_t OUTPUT_MODE_89_91HZ  = 0x2000c;
inline constexpr uint64_t OUTPUT_MODE_119_88HZ = 0x2000f;
inline constexpr int32_t ERROR_REPROJECTION_NOT_INITIALIZED = -1972240373; // 0x8a72000b

inline constexpr uint32_t REPROJECTION_MAX_LAYERS          = 5;
inline constexpr uint32_t REPROJECTION_MAX_DISPLAY_BUFFERS = 16;

inline constexpr uint32_t LAYER_TYPE_VR3D = 0x10;
inline constexpr uint32_t LAYER_TYPE_UI2D = 0x11;
inline constexpr uint32_t LAYER_TYPE_VR2D = 0x12;

inline constexpr uint32_t LAYER_BLEND_INTERPOLATED_ALPHA  = 0;
inline constexpr uint32_t LAYER_BLEND_PREMULTIPLIED_ALPHA = 1;
inline constexpr uint32_t LAYER_BLEND_SIMPLE_ADD          = 2;
inline constexpr uint32_t LAYER_BLEND_LAYER0              = 100;

struct ReprojectionState {
	std::atomic<uint64_t> output_mode {0};
	std::atomic<uint32_t> timing_us {3000};
};

struct TextureViewport {
	uint32_t width;
	uint32_t height;
	uint32_t x;
	uint32_t y;
};

// How one reprojection source is composited; viewports are indexed left eye, right eye.
struct LayerSetting {
	uint32_t                       layer_type;
	uint32_t                       src_id;
	uint32_t                       blend_type;
	std::array<TextureViewport, 2> viewport;
};

// The layers sceHmd2ReprojectionSetParam assigned to one display buffer.
struct FrameSettings {
	int64_t                                           flip_arg = 0;
	uint32_t                                          count    = 0;
	std::array<LayerSetting, REPROJECTION_MAX_LAYERS> layers {};
};

[[nodiscard]] ReprojectionState* GetReprojectionState();
[[nodiscard]] bool GetReprojectionFrameSettings(uint32_t index, FrameSettings* settings);

} // namespace Libs::Hmd2

#endif // EMULATOR_SRC_LIBS_HMD2_H_
