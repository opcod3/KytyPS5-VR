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
	// Video-out bus of the PSVR2 headset; its layer is presented to a separate window.
	static constexpr int HMD_BUS = 32;

	struct Frame;
	struct Layer {
		Frame* frame;
		int    bus;
		bool   premultiplied_alpha;
	};

	// How a headset layer lands on the panel, applied to what is already there.
	enum class HmdBlend : uint8_t { Opaque, Alpha, PremultipliedAlpha, Add };

	// One eye of a headset layer: a region of a guest texture, optionally rendered in FSR space
	// and resolved through its horizontal and vertical lookup tables.
	struct HmdEye {
		ShaderTextureResource texture;
		ShaderTextureResource fsr_lut[2];
		uint32_t              x      = 0;
		uint32_t              y      = 0;
		uint32_t              width  = 0;
		uint32_t              height = 0;
		bool                  fsr    = false;
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
