#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_PRESENTER_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_PRESENTER_H_

#include "common/common.h"
#include "graphics/shader/shaderBindings.h"

#include <array>
#include <memory>
#include <span>

namespace Libs::Graphics {

class CommandBuffer;
class RenderContext;
struct ImageInfo;
struct WindowContext;

class Presenter final {
public:
	// Video-out bus of the PSVR2 headset; its layer goes to the OpenXR runtime, or to a separate
	// window with the virtual headset.
	static constexpr int HMD_BUS = 32;

	struct Frame;
	struct Layer {
		Frame* frame;
		int    bus;
		bool   premultiplied_alpha;
	};

	// How a headset layer lands on the panel, applied to what is already there.
	enum class HmdBlend : uint8_t { Opaque, Alpha, PremultipliedAlpha, Add };

	// One eye of a headset layer: a region of a guest texture.
	struct HmdEye {
		ShaderTextureResource texture;
		uint32_t              x      = 0;
		uint32_t              y      = 0;
		uint32_t              width  = 0;
		uint32_t              height = 0;
	};
	// Tangents of the frustum an eye was rendered with; out is temporal, in is nasal.
	struct HmdFov {
		float tan_out    = 0.0f;
		float tan_in     = 0.0f;
		float tan_top    = 0.0f;
		float tan_bottom = 0.0f;
	};
	// The head pose the panel was rendered with, in the tracking space of its flip_arg.
	struct HmdPose {
		std::array<float, 3> position {};
		std::array<float, 4> orientation {};
		int64_t              flip_arg    = 0;
		bool                 valid       = false;
		bool                 head_locked = false;
	};
	struct HmdLayer {
		std::array<HmdEye, 2> eyes;
		HmdBlend              blend = HmdBlend::Opaque;
	};
	// Layers are drawn in order, each eye into its half of a side-by-side panel image.
	struct HmdComposition {
		uint32_t                width       = 0;
		uint32_t                height      = 0;
		uint32_t                layer_count = 0;
		std::array<HmdLayer, 5> layers;
		std::array<HmdFov, 2>   fov;
		HmdPose                 pose;
	};

	// Which part of the headset's left eye the main window mirrors.
	struct HmdMirror {
		// False: the whole eye width. True: the central half, i.e. magnified.
		bool center = false;
	};

	explicit Presenter(WindowContext& window);
	~Presenter();
	KYTY_CLASS_NO_COPY(Presenter);

	[[nodiscard]] Frame&         PrepareFrame(CommandBuffer& command, const ImageInfo& info,
	                                         bool hdr = false);
	[[nodiscard]] Frame&         PrepareHmdFrame(CommandBuffer&        command,
	                                             const HmdComposition& composition);
	[[nodiscard]] Frame&         PrepareBlankFrame(uint32_t width, uint32_t height, bool opaque,
	                                               CommandBuffer* producer = nullptr);
	[[nodiscard]] bool           PresentLastFrame();
	// Shows the newest headset panel in the main window; false before the first one.
	[[nodiscard]] bool           PresentHmdMirror(const HmdMirror& mirror);
	[[nodiscard]] bool           IsGuestPaused() const noexcept;
	[[nodiscard]] bool           IsHdrSupported() const noexcept;
	[[nodiscard]] bool           NeedsSystemOverlayRefresh() const noexcept;
	[[nodiscard]] RenderContext& Renderer() const noexcept;
	void                         Present(Frame& frame);
	void                         Present(std::span<const Layer> layers);
	void                         ClearLayer(int bus);
	void                         Discard(Frame& frame);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_PRESENTER_H_
