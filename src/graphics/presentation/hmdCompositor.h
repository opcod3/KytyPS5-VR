#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_HMDCOMPOSITOR_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_HMDCOMPOSITOR_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/presenter.h"

#include <array>
#include <span>

namespace Libs::Graphics {

struct GraphicContext;

// Composites PSVR2 reprojection layers into the side-by-side headset panel image.
class HmdCompositor final {
public:
	struct Draw {
		vk::ImageView source = nullptr;
		// Horizontal and vertical FSR resolve tables; null when the source is not in FSR space.
		std::array<vk::ImageView, 2> fsr_lut {};
		vk::Rect2D                   target;
		// Normalized offset and size of the drawn region in the source.
		std::array<float, 4> region {};
		Presenter::HmdBlend  blend = Presenter::HmdBlend::Opaque;
	};

	explicit HmdCompositor(GraphicContext& graphics): m_graphics(graphics) {}
	~HmdCompositor();
	KYTY_CLASS_NO_COPY(HmdCompositor);

	// Sources must be shader-readable and the target a color attachment.
	void Record(vk::CommandBuffer command, vk::ImageView target, vk::Format format,
	            vk::Extent2D extent, std::span<const Draw> draws);

private:
	void         CreateLayout();
	vk::Pipeline FindPipeline(Presenter::HmdBlend blend, vk::Format format);

	GraphicContext&             m_graphics;
	vk::DescriptorSetLayout     m_descriptors = nullptr;
	vk::PipelineLayout          m_layout      = nullptr;
	vk::Sampler                 m_sampler     = nullptr;
	vk::Format                  m_format      = vk::Format::eUndefined;
	std::array<vk::Pipeline, 4> m_pipelines {};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_HMDCOMPOSITOR_H_
