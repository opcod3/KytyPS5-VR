#include "graphics/presentation/hmdCompositor.h"

#include "common/assert.h"
#include "gpu_blit_shaders/gpu_blit_fs_triangle_spv.h"
#include "gpu_blit_shaders/gpu_hmd_compose_spv.h"
#include "graphics/host_gpu/graphicContext.h"

namespace Libs::Graphics {

struct HmdLayerConstants {
	std::array<float, 4> region;
	uint32_t             fsr;
};

HmdCompositor::~HmdCompositor() {
	if (m_layout == nullptr) {
		return;
	}
	auto device = m_graphics.device;
	{
		Common::LockGuard queue_lock(m_graphics.queue_mutex);
		RequireVulkanSuccess(m_graphics.queue.waitIdle(), "wait for headset compositor queue");
	}
	for (auto pipeline: m_pipelines) {
		device.destroyPipeline(pipeline, nullptr);
	}
	device.destroyPipelineLayout(m_layout, nullptr);
	device.destroyDescriptorSetLayout(m_descriptors, nullptr);
	device.destroySampler(m_sampler, nullptr);
}

void HmdCompositor::CreateLayout() {
	auto                  device = m_graphics.device;
	vk::SamplerCreateInfo sampler {};
	sampler.magFilter    = vk::Filter::eLinear;
	sampler.minFilter    = vk::Filter::eLinear;
	sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
	sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
	sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
	RequireVulkanSuccess(device.createSampler(&sampler, nullptr, &m_sampler),
	                     "create headset compositor sampler");
	// Source, then the horizontal and vertical FSR tables.
	std::array<vk::DescriptorSetLayoutBinding, 3> bindings {};
	for (uint32_t i = 0; i < bindings.size(); i++) {
		bindings[i] = {i, vk::DescriptorType::eCombinedImageSampler, 1,
		               vk::ShaderStageFlagBits::eFragment};
	}
	vk::DescriptorSetLayoutCreateInfo descriptors {};
	descriptors.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	descriptors.bindingCount = static_cast<uint32_t>(bindings.size());
	descriptors.pBindings    = bindings.data();
	RequireVulkanSuccess(device.createDescriptorSetLayout(&descriptors, nullptr, &m_descriptors),
	                     "create headset compositor descriptor layout");
	const vk::PushConstantRange  constants {vk::ShaderStageFlagBits::eFragment, 0,
	                                       sizeof(HmdLayerConstants)};
	vk::PipelineLayoutCreateInfo layout {};
	layout.setLayoutCount         = 1;
	layout.pSetLayouts            = &m_descriptors;
	layout.pushConstantRangeCount = 1;
	layout.pPushConstantRanges    = &constants;
	RequireVulkanSuccess(device.createPipelineLayout(&layout, nullptr, &m_layout),
	                     "create headset compositor pipeline layout");
}

vk::Pipeline HmdCompositor::FindPipeline(Presenter::HmdBlend blend, vk::Format format) {
	auto device = m_graphics.device;
	if (m_layout == nullptr) {
		CreateLayout();
	}
	// The panel image format follows the swapchain, which can change on recreation.
	if (format != m_format) {
		for (auto& pipeline: m_pipelines) {
			if (pipeline != nullptr) {
				Common::LockGuard queue_lock(m_graphics.queue_mutex);
				RequireVulkanSuccess(m_graphics.queue.waitIdle(),
				                     "wait for headset compositor queue");
				break;
			}
		}
		for (auto& pipeline: m_pipelines) {
			device.destroyPipeline(pipeline, nullptr);
			pipeline = nullptr;
		}
		m_format = format;
	}
	auto& pipeline = m_pipelines[static_cast<size_t>(blend)];
	if (pipeline != nullptr) {
		return pipeline;
	}

	const auto vertex   = CompileSPV(GPU_BLIT_FS_TRIANGLE_SPV, device);
	const auto fragment = CompileSPV(GPU_HMD_COMPOSE_SPV, device);
	std::array<vk::PipelineShaderStageCreateInfo, 2> stages {};
	stages[0].stage  = vk::ShaderStageFlagBits::eVertex;
	stages[0].module = vertex;
	stages[0].pName  = "main";
	stages[1].stage  = vk::ShaderStageFlagBits::eFragment;
	stages[1].module = fragment;
	stages[1].pName  = "main";
	vk::PipelineVertexInputStateCreateInfo   vertex_input {};
	vk::PipelineInputAssemblyStateCreateInfo assembly {};
	assembly.topology = vk::PrimitiveTopology::eTriangleList;
	vk::PipelineViewportStateCreateInfo viewport {};
	viewport.viewportCount = 1;
	viewport.scissorCount  = 1;
	vk::PipelineRasterizationStateCreateInfo rasterization {};
	rasterization.lineWidth = 1.0f;
	vk::PipelineMultisampleStateCreateInfo multisample {};
	multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
	vk::PipelineColorBlendAttachmentState attachment {};
	attachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
	                            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
	attachment.colorBlendOp   = vk::BlendOp::eAdd;
	attachment.alphaBlendOp   = vk::BlendOp::eAdd;
	switch (blend) {
		case Presenter::HmdBlend::Opaque: break;
		case Presenter::HmdBlend::Alpha:
			attachment.blendEnable         = VK_TRUE;
			attachment.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
			attachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
			attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
			attachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
			break;
		case Presenter::HmdBlend::PremultipliedAlpha:
			attachment.blendEnable         = VK_TRUE;
			attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
			attachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
			attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
			attachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
			break;
		case Presenter::HmdBlend::Add:
			attachment.blendEnable         = VK_TRUE;
			attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
			attachment.dstColorBlendFactor = vk::BlendFactor::eOne;
			attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
			attachment.dstAlphaBlendFactor = vk::BlendFactor::eOne;
			break;
	}
	vk::PipelineColorBlendStateCreateInfo blend_state {};
	blend_state.attachmentCount = 1;
	blend_state.pAttachments    = &attachment;
	const std::array dynamic_states {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
	vk::PipelineDynamicStateCreateInfo dynamic {};
	dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
	dynamic.pDynamicStates    = dynamic_states.data();
	vk::PipelineRenderingCreateInfo rendering {};
	rendering.colorAttachmentCount    = 1;
	rendering.pColorAttachmentFormats = &m_format;
	vk::GraphicsPipelineCreateInfo create {};
	create.pNext               = &rendering;
	create.stageCount          = static_cast<uint32_t>(stages.size());
	create.pStages             = stages.data();
	create.pVertexInputState   = &vertex_input;
	create.pInputAssemblyState = &assembly;
	create.pViewportState      = &viewport;
	create.pRasterizationState = &rasterization;
	create.pMultisampleState   = &multisample;
	create.pColorBlendState    = &blend_state;
	create.pDynamicState       = &dynamic;
	create.layout              = m_layout;
	RequireVulkanSuccess(device.createGraphicsPipelines(nullptr, 1, &create, nullptr, &pipeline),
	                     "create headset compositor pipeline");
	device.destroyShaderModule(fragment, nullptr);
	device.destroyShaderModule(vertex, nullptr);
	return pipeline;
}

void HmdCompositor::Record(vk::CommandBuffer command, vk::ImageView target, vk::Format format,
                           vk::Extent2D extent, std::span<const Draw> draws) {
	// Pipelines are created before rendering begins.
	for (const auto& draw: draws) {
		(void)FindPipeline(draw.blend, format);
	}

	vk::RenderingAttachmentInfo attachment {};
	attachment.imageView   = target;
	attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	attachment.loadOp      = vk::AttachmentLoadOp::eClear;
	attachment.storeOp     = vk::AttachmentStoreOp::eStore;
	attachment.clearValue.color.float32[3] = 1.0f;
	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = extent;
	rendering.layerCount           = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments    = &attachment;
	command.beginRendering(&rendering);
	for (const auto& draw: draws) {
		std::array<vk::DescriptorImageInfo, 3> images {};
		images[0] = {m_sampler, draw.source, vk::ImageLayout::eShaderReadOnlyOptimal};
		const bool fsr = draw.fsr_lut[0] != nullptr;
		for (uint32_t i = 0; i < 2; i++) {
			// Unused tables still need a valid descriptor; the source stands in for them.
			images[i + 1] = {m_sampler, fsr ? draw.fsr_lut[i] : draw.source,
			                 vk::ImageLayout::eShaderReadOnlyOptimal};
		}
		std::array<vk::WriteDescriptorSet, 3> writes {};
		for (uint32_t i = 0; i < writes.size(); i++) {
			writes[i].dstBinding      = i;
			writes[i].descriptorCount = 1;
			writes[i].descriptorType  = vk::DescriptorType::eCombinedImageSampler;
			writes[i].pImageInfo      = &images[i];
		}
		command.bindPipeline(vk::PipelineBindPoint::eGraphics, FindPipeline(draw.blend, format));
		command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_layout, 0,
		                             static_cast<uint32_t>(writes.size()), writes.data());
		const HmdLayerConstants constants {draw.region, fsr ? 1u : 0u};
		command.pushConstants(m_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(constants),
		                      &constants);
		const vk::Viewport viewport {static_cast<float>(draw.target.offset.x),
		                             static_cast<float>(draw.target.offset.y),
		                             static_cast<float>(draw.target.extent.width),
		                             static_cast<float>(draw.target.extent.height),
		                             0.0f,
		                             1.0f};
		command.setViewport(0, 1, &viewport);
		command.setScissor(0, 1, &draw.target);
		command.draw(3, 1, 0, 0);
	}
	command.endRendering();
}

} // namespace Libs::Graphics
