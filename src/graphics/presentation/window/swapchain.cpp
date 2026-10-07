#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "gpu_blit_shaders/gpu_blit_fs_triangle_spv.h"
#include "gpu_blit_shaders/gpu_video_out_overlay_spv.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/hmdCompositor.h"
#include "graphics/presentation/xr/xrRuntime.h"
#include "graphics/presentation/presenter.h"
#include "graphics/presentation/systemOverlay.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window/windowInternal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <limits>
#include <memory>
#include <vector>
#include <vulkan/vk_platform.h>

// IWYU pragma: no_include <intrin.h>

namespace Libs::Graphics {

struct Presenter::Frame {
	VulkanImage   image;
	vk::ImageView view         = nullptr;
	uint64_t      present_tick     = 0;
	bool          busy             = false;
	bool          hdr              = false;
	bool          blank            = false;
	bool          color_attachment = false;
	// Headset panels only: the frustums the eyes were rendered with.
	std::array<HmdFov, 2> hmd_fov {};
	HmdPose               hmd_pose {};

	// A render-target frame is also drawn into as a color attachment.
	void Configure(GraphicContext& graphics, vk::Extent2D extent, vk::Format format,
	               bool render_target = false);
	void Transit(vk::CommandBuffer command, vk::ImageLayout layout, vk::AccessFlags2 access);
	void CopyFrom(CommandBuffer& command, Image& source);
	void Clear(CommandBuffer& command, const vk::ClearColorValue& color);
};

class FramePool final {
public:
	FramePool(WindowContext& window, CommandScheduler& scheduler)
	    : m_window(window), m_scheduler(scheduler) {}
	~FramePool() {
		m_scheduler.Wait(m_scheduler.CurrentTick() - 1);
		for (auto& frame: m_frames) {
			m_window.graphic_ctx.device.destroyImageView(frame->view, nullptr);
			if (frame->image.image != nullptr) {
				m_window.graphic_ctx.DeleteImage(frame->image);
			}
		}
	}
	KYTY_CLASS_NO_COPY(FramePool);

	void Initialize(uint32_t count, vk::Format format) {
		if (count == 0 || format == vk::Format::eUndefined) {
			EXIT("prepared-frame pool requires at least one frame\n");
		}
		Common::LockGuard lock(m_mutex);
		if (!m_frames.empty()) {
			EXIT("prepared-frame pool was initialized twice\n");
		}
		m_format = format;
		m_frames.reserve(count);
		m_free.reserve(count);
		for (uint32_t i = 0; i < count; i++) {
			auto frame = std::make_unique<Presenter::Frame>();
			m_free.push_back(frame.get());
			m_frames.push_back(std::move(frame));
		}
	}

	void SetFormat(vk::Format format) {
		if (format == vk::Format::eUndefined) {
			EXIT("prepared-frame pool requires a presentation format\n");
		}
		Common::LockGuard lock(m_mutex);
		m_format = format;
	}

	vk::Format GetFormat() {
		Common::LockGuard lock(m_mutex);
		if (m_format == vk::Format::eUndefined) {
			EXIT("prepared-frame pool has no presentation format\n");
		}
		return m_format;
	}

	Presenter::Frame* Acquire(vk::Extent2D extent, vk::Format format) {
		m_mutex.Lock();
		if (m_frames.empty()) {
			EXIT("prepared-frame pool was used before swapchain initialization\n");
		}
		// A synchronized flip may need more frames than the swapchain has images.
		// Queue capacity bounds growth; waiting here can prevent its master from recording.
		if (m_free.empty()) {
			auto frame = std::make_unique<Presenter::Frame>();
			m_free.push_back(frame.get());
			m_frames.push_back(std::move(frame));
		}
		const auto compatible = std::ranges::find_if(m_free, [&](const auto* frame) {
			return frame->image.extent.width == extent.width &&
			       frame->image.extent.height == extent.height && frame->image.format == format;
		});
		auto*      frame      = compatible == m_free.end() ? m_free.back() : *compatible;
		if (compatible != m_free.end()) {
			*compatible = m_free.back();
		}
		m_free.pop_back();
		if (frame->busy) {
			EXIT("prepared-frame pool returned an invalid frame\n");
		}
		frame->busy = true;
		m_mutex.Unlock();

		m_scheduler.Wait(frame->present_tick);
		return frame;
	}

	void ValidateForPresent(Presenter::Frame* frame) {
		Common::LockGuard lock(m_mutex);
		if (frame == nullptr || !frame->busy) {
			EXIT("prepared frame has invalid presentation ownership\n");
		}
	}

	void Release(Presenter::Frame* frame) {
		if (frame == nullptr) {
			EXIT("cannot release a null prepared frame\n");
		}
		Common::LockGuard lock(m_mutex);
		if (!frame->busy) {
			EXIT("prepared frame was released twice\n");
		}
		frame->busy = false;
		m_free.push_back(frame);
	}

private:
	WindowContext&                                 m_window;
	CommandScheduler&                              m_scheduler;
	Common::Mutex                                  m_mutex;
	std::vector<std::unique_ptr<Presenter::Frame>> m_frames;
	std::vector<Presenter::Frame*>                 m_free;
	vk::Format                                     m_format = vk::Format::eUndefined;
};

void Presenter::Frame::Configure(GraphicContext& graphics, vk::Extent2D extent, vk::Format format,
                                 bool render_target) {
	if (extent.width == 0 || extent.height == 0 || format == vk::Format::eUndefined) {
		EXIT("unsupported prepared frame, extent=%ux%u format=%d\n", extent.width, extent.height,
		     static_cast<int>(format));
	}
	auto&      dst        = image;
	const bool compatible = dst.image != nullptr && dst.extent.width == extent.width &&
	                        dst.extent.height == extent.height && dst.format == format &&
	                        (!render_target || color_attachment);
	if (compatible) {
		return;
	}

	const auto features = graphics.GetFormatProperties(format).optimalTilingFeatures;
	auto       required =
	    vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eSampledImageFilterLinear |
	    vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eTransferSrc |
	    vk::FormatFeatureFlagBits::eTransferDst;
	if (render_target) {
		required |= vk::FormatFeatureFlagBits::eColorAttachment;
	}
	if ((features & required) != required) {
		EXIT("prepared presentation format lacks optimal blit support: format=%d features=0x%x\n",
		     static_cast<int>(format), static_cast<vk::FormatFeatureFlags::MaskType>(features));
	}

	if (dst.image != nullptr) {
		graphics.device.destroyImageView(view, nullptr);
		view = nullptr;
		graphics.DeleteImage(dst);
	}

	vk::ImageCreateInfo create {};
	create.sType         = vk::StructureType::eImageCreateInfo;
	create.imageType     = vk::ImageType::e2D;
	create.extent        = {extent.width, extent.height, 1};
	create.mipLevels     = 1;
	create.arrayLayers   = 1;
	create.format        = format;
	create.tiling        = vk::ImageTiling::eOptimal;
	create.initialLayout = vk::ImageLayout::eUndefined;
	create.usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst |
	               vk::ImageUsageFlagBits::eSampled;
	if (render_target) {
		create.usage |= vk::ImageUsageFlagBits::eColorAttachment;
	}
	create.sharingMode = vk::SharingMode::eExclusive;
	create.samples     = vk::SampleCountFlagBits::e1;
	if (!graphics.CreateImage(create, dst)) {
		EXIT("failed to allocate prepared presentation image, extent=%ux%u format=%d\n",
		     extent.width, extent.height, static_cast<int>(format));
	}
	color_attachment = render_target;
}

void Presenter::Frame::Transit(vk::CommandBuffer command, vk::ImageLayout layout,
                               vk::AccessFlags2 access) {
	const auto     stage  = access == vk::AccessFlagBits2::eTransferRead ||
	                                access == vk::AccessFlagBits2::eTransferWrite
	                            ? vk::PipelineStageFlagBits2::eTransfer
	                            : vk::PipelineStageFlagBits2::eAllCommands;
	constexpr auto writes = vk::AccessFlagBits2::eTransferWrite |
	                        vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eMemoryWrite;
	if (image.state.layout == layout && image.state.access_mask == access &&
	    !static_cast<bool>(image.state.access_mask & writes)) {
		return;
	}
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask                    = image.state.pl_stage;
	barrier.srcAccessMask                   = image.state.access_mask;
	barrier.dstStageMask                    = stage;
	barrier.dstAccessMask                   = access;
	barrier.oldLayout                       = image.state.layout;
	barrier.newLayout                       = layout;
	barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.image                           = image.image;
	barrier.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	barrier.subresourceRange.baseMipLevel   = 0;
	barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount     = VK_REMAINING_ARRAY_LAYERS;
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dependency);
	image.state = {stage, access, layout};
	image.subresource_states.clear();
}

void Presenter::Frame::CopyFrom(CommandBuffer& command_buffer, Image& source) {
	command_buffer.EndRendering();
	auto command = command_buffer.Handle();
	source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
	               command);
	Transit(command, vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite);
	vk::ImageCopy copy {};
	copy.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, source.backing.layers};
	copy.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, image.layers};
	copy.extent         = {std::min(source.backing.extent.width, image.extent.width),
	                       std::min(source.backing.extent.height, image.extent.height), 1};
	EXIT_IF(copy.srcSubresource.layerCount != copy.dstSubresource.layerCount);
	command.copyImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal, image.image,
	                  vk::ImageLayout::eTransferDstOptimal, copy);
}

void Presenter::Frame::Clear(CommandBuffer& command_buffer, const vk::ClearColorValue& color) {
	command_buffer.EndRendering();
	auto command = command_buffer.Handle();
	Transit(command, vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite);
	const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	command.clearColorImage(image.image, vk::ImageLayout::eTransferDstOptimal, &color, 1, &range);
}

class Swapchain final {
public:
	enum class Status : uint8_t { Success, Recreate, SurfaceLost };

	// Presents to the main window unless a secondary window is given.
	explicit Swapchain(WindowContext& window, SecondaryWindow* target = nullptr)
	    : m_window(window), m_target(target) {}
	~Swapchain();
	KYTY_CLASS_NO_COPY(Swapchain);

	void                 Create();
	void                 Recreate(bool surface_lost = false);
	[[nodiscard]] bool   NeedsResize() const;
	[[nodiscard]] Status AcquireNextImage(CommandScheduler& scheduler);
	[[nodiscard]] bool   PrepareSystemOverlay();
	// Scales source, or the part of it in source_rect, to the whole window.
	void     RecordPresentCommands(CommandBuffer& command, Presenter::Frame* source,
	                               const Presenter::Layer& overlay, bool draw_system_overlay,
	                               const vk::Rect2D* source_rect = nullptr);
	uint64_t Submit(CommandScheduler& scheduler);
	[[nodiscard]] Status Present();

	[[nodiscard]] uint32_t ImageCount() const noexcept {
		return static_cast<uint32_t>(m_images.size());
	}
	[[nodiscard]] vk::Format Format() const noexcept { return m_format; }
	[[nodiscard]] bool IsHdrSupported() const noexcept { return m_hdr_supported.load(); }
	bool SetHdr(bool hdr) {
		const bool changed = m_hdr_active != (hdr && IsHdrSupported());
		m_hdr_requested = hdr;
		return changed;
	}
	[[nodiscard]] vk::Extent2D Extent() const noexcept { return m_extent; }

private:
	[[nodiscard]] vk::SurfaceKHR             Surface() const;
	[[nodiscard]] vk::Extent2D               WindowExtent() const;
	[[nodiscard]] const SurfaceCapabilities& RefreshSurfaceCapabilities();
	vk::SurfaceFormatKHR                     SelectFormat(const SurfaceCapabilities& surface);
	void                                     RecreateSurface();
	void                                     Destroy();
	void DrawOverlay(vk::CommandBuffer command, const Presenter::Layer& layer);

	WindowContext&   m_window;
	SecondaryWindow* m_target = nullptr;
	vk::SwapchainKHR m_handle = nullptr;
	vk::Format       m_format = vk::Format::eUndefined;
	std::atomic_bool m_hdr_supported {false};
	bool             m_hdr_requested = false;
	bool             m_hdr_active    = false;
	vk::Extent2D     m_extent {};
	// Drawable pixel size observed when this swapchain was created.
	vk::Extent2D                   m_window_extent {};
	std::vector<vk::Image>         m_images;
	std::vector<vk::ImageView>     m_image_views;
	std::vector<vk::Semaphore>     m_image_acquired;
	std::vector<uint64_t>          m_image_acquired_ticks;
	std::vector<vk::Semaphore>     m_render_complete;
	std::unique_ptr<SystemOverlay> m_system_overlay;
	vk::DescriptorSetLayout        m_overlay_descriptors = nullptr;
	vk::PipelineLayout             m_overlay_layout      = nullptr;
	vk::Pipeline                   m_overlay_pipeline    = nullptr;
	vk::Sampler                    m_overlay_sampler     = nullptr;
	uint32_t                       m_image_index         = static_cast<uint32_t>(-1);
	uint32_t                       m_frame_index         = 0;
};

struct Presenter::Impl {
	explicit Impl(WindowContext& owner)
	    : renderer(*owner.render_context), window(owner), swapchain(owner),
	      present_scheduler(renderer, owner.graphic_ctx), frames(owner, present_scheduler),
	      hmd_to_openxr(Xr::IsActive()) {
		EXIT_IF(owner.render_context == nullptr);
		swapchain.Create();
		frames.Initialize(swapchain.ImageCount(), swapchain.Format());
	}

	void RecoverSwapchain(Swapchain::Status status) {
		LOGF("Recovering Vulkan swapchain%s\n",
		     status == Swapchain::Status::SurfaceLost ? " and surface" : "");
		swapchain.Recreate(status == Swapchain::Status::SurfaceLost);
		frames.SetFormat(swapchain.Format());
	}

	Image& ResolveSurface(const ImageInfo& info) {
		TextureCache::ImageDesc desc {};
		desc.info                  = info;
		desc.view_info.format      = info.pixel_format;
		desc.view_info.type        = vk::ImageViewType::e2D;
		desc.view_info.aspect      = vk::ImageAspectFlagBits::eColor;
		desc.view_info.base_level  = 0;
		desc.view_info.level_count = 1;
		desc.view_info.base_layer  = 0;
		desc.view_info.layer_count = 1;
		desc.view_info.usage       = vk::ImageUsageFlagBits::eTransferSrc;
		desc.type                  = TextureCache::BindingType::VideoOut;

		auto&      cache      = renderer.GetTextureCache();
		const auto image_id   = cache.FindImage(desc);
		auto&      image      = cache.GetImage(image_id);
		image.usage.video_out = true;
		cache.UpdateImage(image_id);
		return image;
	}
	// Headset layer textures are single-level color textures, resolved the
	// same way a shader samples them so they alias the guest's own images.
	vk::ImageView ResolveHmdTexture(const ShaderTextureResource& texture,
	                                vk::CommandBuffer            command) {
		const auto     format = texture.Format();
		const auto     tile   = texture.TileMode();
		const auto     type   = texture.Type();
		const uint32_t width  = static_cast<uint32_t>(texture.Width5()) + 1u;
		const uint32_t height = static_cast<uint32_t>(texture.Height5()) + 1u;
		if (texture.IsNull() || texture.BaseLevel() != 0 || texture.LastLevel() != 0 ||
		    (type != Prospero::ImageType::kColor1D && type != Prospero::ImageType::kColor2D)) {
			EXIT("unsupported headset layer texture: addr=0x%016" PRIx64
			     " type=%u levels=%u..%u\n",
			     texture.Base40(), static_cast<uint32_t>(type), texture.BaseLevel(),
			     texture.LastLevel());
		}
		const auto     surface = TextureGetSurfaceFormatInfo(format);
		TileSizeAlign  size {};
		TileSizeOffset level {};
		TilePaddedSize padded {};
		TileGetTextureTotalSize(format, width, height, 1, 1, tile, false, size);
		TileGetTextureSize(format, width, height, 1, tile, nullptr, &level, &padded);
		EXIT_NOT_IMPLEMENTED(size.size == 0 || Prospero::BlockCompressedBytesPerBlock(format) != 0);

		TextureCache::ImageDesc desc {};
		desc.info.data            = {texture.Base40(), size.size};
		desc.info.pixel_format    = surface.vk_format;
		desc.info.guest_format    = format;
		desc.info.type            = Prospero::ImageType::kColor2D;
		desc.info.extent          = {width, height, 1};
		desc.info.resources       = {1, 1};
		desc.info.pitch           = TileGetTexturePitch(format, width, tile);
		desc.info.bytes_per_block = Prospero::NumBytesPerElement(format);
		desc.info.samples         = 1;
		desc.info.tile_mode       = tile;
		desc.info.mip_layout[0]   = {level.src_size != 0 ? level.src_offset : level.offset,
		                             level.src_size != 0 ? level.src_size : level.size,
		                             padded.width, padded.height};
		if (texture.MetaCompress()) {
			TileSizeAlign metadata_size {};
			(void)TileGetDccSize(width, height, 1, desc.info.bytes_per_block, 1, tile,
			                     metadata_size);
			desc.info.metadata.kind          = ImageMetadataKind::Dcc;
			desc.info.metadata.range         = {texture.MetaAddr() << 8u, metadata_size.size};
			desc.info.metadata.dcc_alpha_msb = texture.DccAlphaPos();
		}
		desc.view_info.format = surface.vk_format;
		desc.view_info.type   = vk::ImageViewType::e2D;
		desc.view_info.mapping =
		    surface.conversion_format != Prospero::BufferFormat::kInvalid
		        ? vk::ComponentMapping {}
		        : TextureGetComponentMapping(texture.DstSelXYZW(), surface.host_to_storage);
		desc.type = TextureCache::BindingType::Texture;

		auto&      cache = renderer.GetTextureCache();
		const auto conversion = surface.conversion_format != Prospero::BufferFormat::kInvalid;
		const auto id         = cache.FindImage(desc, conversion);
		auto&      image      = cache.GetImage(id);
		if (image.info.IsDepth() || image.depth_id) {
			EXIT("headset layer texture aliases a depth target: addr=0x%016" PRIx64 "\n",
			     texture.Base40());
		}
		const auto view     = cache.FindTexture(id, desc);
		image.usage.texture = true;
		image.Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead,
		              {}, command);
		return view;
	}
	// Presents the main window; with mirror, the left eye of the headset panel replaces the
	// main bus layer.
	void Present(const HmdMirror* mirror = nullptr);
	// Shows the newest headset panel. The destination is fixed once the OpenXR fallback is
	// resolved: the OpenXR runtime's panel, or the SDL headset window for the virtual headset.
	void PresentHmd();
	void PresentHmdToOpenxr();
	void PresentHmdToWindow();

	RenderContext&        renderer;
	WindowContext&        window;
	Swapchain             swapchain;
	CommandScheduler      present_scheduler;
	FramePool             frames;
	Common::Mutex         present_mutex;
	std::array<Layer, 2>  layers {};
	std::atomic<uint64_t> presented_overlay_revision {0};
	// Guards the headset layer. The OpenXR frame thread presents headset flips, and must not
	// wait for the main window's vsync under present_mutex.
	Common::Mutex hmd_present_mutex;
	// The OpenXR runtime shows the headset panel; otherwise the SDL headset window does. Fixed at
	// construction, by when the OpenXR fallback has been resolved.
	const bool hmd_to_openxr;
	// Created on the first headset flip, after its window is shown.
	std::unique_ptr<Swapchain> hmd_swapchain;
	Layer                      hmd_layer {};
	HmdCompositor              hmd_compositor {window.graphic_ctx};
};

vk::SurfaceKHR Swapchain::Surface() const {
	return m_target != nullptr ? m_target->surface : m_window.surface;
}

vk::Extent2D Swapchain::WindowExtent() const {
	if (m_target != nullptr) {
		Common::LockGuard lock(m_target->mutex);
		return {m_target->width, m_target->height};
	}
	Common::LockGuard lock(m_window.mutex);
	return {m_window.graphic_ctx.screen_width, m_window.graphic_ctx.screen_height};
}

const SurfaceCapabilities& Swapchain::RefreshSurfaceCapabilities() {
	if (m_target != nullptr) {
		m_target->RefreshSurfaceCapabilities(m_window.graphic_ctx);
		return m_target->surface_capabilities;
	}
	m_window.RefreshSurfaceCapabilities();
	return m_window.surface_capabilities;
}

void Swapchain::RecreateSurface() {
	if (m_target != nullptr) {
		m_target->RecreateSurface(m_window.graphic_ctx);
		return;
	}
	m_window.RecreateSurface();
}

vk::SurfaceFormatKHR Swapchain::SelectFormat(const SurfaceCapabilities& surface) {
	const auto& formats    = surface.formats;
	auto&       graphics   = m_window.graphic_ctx;
	const auto  hdr_format = std::ranges::find_if(formats, [&](const auto& candidate) {
		return candidate.colorSpace == vk::ColorSpaceKHR::eHdr10St2084EXT &&
		       IsHdrSwapchainFormat(candidate.format) &&
		       static_cast<bool>(
		           graphics.GetFormatProperties(candidate.format).optimalTilingFeatures &
		           vk::FormatFeatureFlagBits::eBlitDst);
	});
	m_hdr_supported        = Config::HdrEnabled() && hdr_format != formats.end();
	m_hdr_active           = m_hdr_requested && IsHdrSupported();
	if (m_hdr_active) {
		return *hdr_format;
	}
	if (Config::HdrEnabled() && !IsHdrSupported()) {
		LOGF("HDR10 surface unavailable; using SDR output\n");
	}
	if (formats.size() == 1 && formats.front().format == vk::Format::eUndefined) {
		return {vk::Format::eR8G8B8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear};
	}
	for (const auto& candidate: formats) {
		if (candidate.colorSpace != vk::ColorSpaceKHR::eSrgbNonlinear) {
			continue;
		}
		switch (candidate.format) {
			case vk::Format::eB8G8R8A8Unorm:
			case vk::Format::eR8G8B8A8Unorm: return candidate;
			default: break;
		}
	}
	EXIT("no supported UNORM swapchain format\n");
}

void Swapchain::Create() {
	auto& graphics = m_window.graphic_ctx;
	EXIT_IF(graphics.device == nullptr);
	EXIT_IF(Surface() == nullptr);

	// Capture the size before querying surface capabilities. If the window changes
	// during creation, the next presentation will detect the newer size.
	m_window_extent = WindowExtent();
	EXIT_IF(m_window_extent.width == 0);
	EXIT_IF(m_window_extent.height == 0);
	const auto& surface = RefreshSurfaceCapabilities();
	EXIT_NOT_IMPLEMENTED(surface.formats.empty());

	m_extent = surface.capabilities.currentExtent;
	if (m_extent.width == std::numeric_limits<uint32_t>::max()) {
		m_extent.width =
		    std::clamp(m_window_extent.width, surface.capabilities.minImageExtent.width,
		               surface.capabilities.maxImageExtent.width);
		m_extent.height =
		    std::clamp(m_window_extent.height, surface.capabilities.minImageExtent.height,
		               surface.capabilities.maxImageExtent.height);
	}
	uint32_t image_count = surface.capabilities.minImageCount + 1;
	if (surface.capabilities.maxImageCount != 0) {
		image_count = std::min(image_count, surface.capabilities.maxImageCount);
	}
	const auto transform =
	    surface.capabilities.supportedTransforms & vk::SurfaceTransformFlagBitsKHR::eIdentity
	        ? vk::SurfaceTransformFlagBitsKHR::eIdentity
	        : surface.capabilities.currentTransform;
	const auto composite =
	    surface.capabilities.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque
	        ? vk::CompositeAlphaFlagBitsKHR::eOpaque
	        : vk::CompositeAlphaFlagBitsKHR::eInherit;

	const auto format = SelectFormat(surface);
	m_format                      = format.format;
	const auto swapchain_features = graphics.GetFormatProperties(m_format).optimalTilingFeatures;
	if (!static_cast<bool>(swapchain_features & vk::FormatFeatureFlagBits::eBlitDst)) {
		EXIT("swapchain format cannot be a blit destination: format=%d\n",
		     static_cast<int>(m_format));
	}

	vk::SwapchainCreateInfoKHR create_info {};
	create_info.sType            = vk::StructureType::eSwapchainCreateInfoKHR;
	create_info.surface          = Surface();
	create_info.minImageCount    = image_count;
	create_info.imageFormat      = format.format;
	create_info.imageColorSpace  = format.colorSpace;
	create_info.imageExtent      = m_extent;
	create_info.imageArrayLayers = 1;
	create_info.imageUsage =
	    vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst;
	create_info.imageSharingMode = vk::SharingMode::eExclusive;
	create_info.preTransform     = transform;
	create_info.compositeAlpha   = composite;
	switch (Config::GetPresentMode()) {
		case Config::PresentMode::Mailbox:
			create_info.presentMode = vk::PresentModeKHR::eMailbox;
			break;
		case Config::PresentMode::Immediate:
			create_info.presentMode = vk::PresentModeKHR::eImmediate;
			break;
		case Config::PresentMode::Fifo:
		default: create_info.presentMode = vk::PresentModeKHR::eFifo; break;
	}
	if (std::find(surface.present_modes.begin(), surface.present_modes.end(),
	              create_info.presentMode) == surface.present_modes.end()) {
		LOGF("warning: requested present mode is unavailable; falling back to Fifo\n");
		create_info.presentMode = vk::PresentModeKHR::eFifo;
	}
	create_info.clipped = VK_TRUE;
	RequireVulkanSuccess(graphics.device.createSwapchainKHR(&create_info, nullptr, &m_handle),
	                     "vkCreateSwapchainKHR");
	EXIT_IF(m_handle == nullptr);
	LOGF("Vulkan presentation: %s\n", m_hdr_active ? "HDR10 (PQ)" : "SDR");

	m_images = EnumerateVulkan<vk::Image>(
	    "vkGetSwapchainImagesKHR", [&](uint32_t* count, vk::Image* images) {
		    return graphics.device.getSwapchainImagesKHR(m_handle, count, images);
	    });
	EXIT_NOT_IMPLEMENTED(m_images.empty());

	m_image_views.resize(m_images.size());
	for (size_t i = 0; i < m_images.size(); i++) {
		vk::ImageViewCreateInfo view {};
		view.sType                           = vk::StructureType::eImageViewCreateInfo;
		view.image                           = m_images[i];
		view.viewType                        = vk::ImageViewType::e2D;
		view.format                          = m_format;
		view.components                      = {};
		view.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
		view.subresourceRange.baseArrayLayer = 0;
		view.subresourceRange.baseMipLevel   = 0;
		view.subresourceRange.layerCount     = 1;
		view.subresourceRange.levelCount     = 1;
		RequireVulkanSuccess(graphics.device.createImageView(&view, nullptr, &m_image_views[i]),
		                     "vkCreateImageView");
		EXIT_IF(m_image_views[i] == nullptr);
	}

	vk::SemaphoreCreateInfo semaphore_info {};
	semaphore_info.sType = vk::StructureType::eSemaphoreCreateInfo;
	m_image_acquired.resize(m_images.size());
	m_image_acquired_ticks.assign(m_images.size(), 0);
	m_render_complete.resize(m_images.size());
	for (size_t i = 0; i < m_images.size(); i++) {
		RequireVulkanSuccess(
		    graphics.device.createSemaphore(&semaphore_info, nullptr, &m_image_acquired[i]),
		    "create swapchain image-acquired semaphore");
		RequireVulkanSuccess(
		    graphics.device.createSemaphore(&semaphore_info, nullptr, &m_render_complete[i]),
		    "create swapchain render-complete semaphore");
	}
	m_image_index = static_cast<uint32_t>(-1);
	m_frame_index = 0;
}

Swapchain::~Swapchain() {
	Destroy();
}

void Swapchain::Destroy() {
	if (m_handle == nullptr && m_image_acquired.empty() && m_render_complete.empty() &&
	    m_image_views.empty()) {
		return;
	}
	auto& graphics = m_window.graphic_ctx;

	{
		Common::LockGuard queue_lock(graphics.queue_mutex);
		RequireVulkanSuccess(graphics.queue.waitIdle(), "wait for swapchain queue");
	}
	if (m_system_overlay != nullptr) {
		m_system_overlay->ReleaseVulkan();
	}
	graphics.device.destroyPipeline(m_overlay_pipeline, nullptr);
	graphics.device.destroyPipelineLayout(m_overlay_layout, nullptr);
	graphics.device.destroyDescriptorSetLayout(m_overlay_descriptors, nullptr);
	graphics.device.destroySampler(m_overlay_sampler, nullptr);
	m_overlay_pipeline    = nullptr;
	m_overlay_layout      = nullptr;
	m_overlay_descriptors = nullptr;
	m_overlay_sampler     = nullptr;

	for (const auto semaphore: m_image_acquired) {
		if (semaphore != nullptr) {
			graphics.device.destroySemaphore(semaphore, nullptr);
		}
	}
	for (const auto semaphore: m_render_complete) {
		if (semaphore != nullptr) {
			graphics.device.destroySemaphore(semaphore, nullptr);
		}
	}
	for (const auto view: m_image_views) {
		if (view != nullptr) {
			graphics.device.destroyImageView(view, nullptr);
		}
	}
	if (m_handle != nullptr) {
		graphics.device.destroySwapchainKHR(m_handle, nullptr);
	}

	m_handle        = nullptr;
	m_format        = vk::Format::eUndefined;
	m_extent        = {};
	m_window_extent = {};
	m_image_index   = static_cast<uint32_t>(-1);
	m_frame_index   = 0;
	m_images.clear();
	m_image_views.clear();
	m_image_acquired.clear();
	m_image_acquired_ticks.clear();
	m_render_complete.clear();
}

void Swapchain::Recreate(bool surface_lost) {
	Destroy();
	if (surface_lost) {
#if defined(__APPLE__)
		// Surface recreation goes through SDL_Vulkan_CreateSurface, which touches the
		// window's view/layer and must run on the main thread on macOS.
		EXIT_IF(!SDL_RunOnMainThread(
		    [](void* swapchain) { static_cast<Swapchain*>(swapchain)->RecreateSurface(); }, this,
		    true));
#else
		RecreateSurface();
#endif
	}
	Create();
}

bool Swapchain::NeedsResize() const {
	return m_window_extent != WindowExtent();
}

Swapchain::Status Swapchain::AcquireNextImage(CommandScheduler& scheduler) {
	EXIT_IF(m_handle == nullptr || m_frame_index >= m_image_acquired.size());
	// Repeated frames can wrap the semaphore ring without acquiring a new source frame.
	// Wait for the submission that consumed this slot before signaling it again.
	scheduler.Wait(m_image_acquired_ticks[m_frame_index]);
	m_image_index     = static_cast<uint32_t>(-1);
	const auto result = m_window.graphic_ctx.device.acquireNextImageKHR(
	    m_handle, std::numeric_limits<uint64_t>::max(), m_image_acquired[m_frame_index], nullptr,
	    &m_image_index);
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eSuboptimalKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eSuboptimalKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorOutOfDateKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorUnknown:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorUnknown\n");
			return Status::Recreate;
		case vk::Result::eErrorSurfaceLostKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorSurfaceLostKHR\n");
			return Status::SurfaceLost;
		default: EXIT("vkAcquireNextImageKHR failed: %s\n", vk::to_string(result).c_str());
	}
	EXIT_IF(m_image_index >= m_images.size());
	return Status::Success;
}

bool Swapchain::PrepareSystemOverlay() {
	if (m_system_overlay == nullptr) {
		m_system_overlay = std::make_unique<SystemOverlay>(m_window.graphic_ctx);
	}
	return m_system_overlay->PrepareFrame(m_extent, m_format, ImageCount());
}

void Swapchain::DrawOverlay(vk::CommandBuffer command, const Presenter::Layer& layer) {
	auto device = m_window.graphic_ctx.device;
	if (m_overlay_pipeline == nullptr) {
		vk::SamplerCreateInfo sampler {};
		sampler.magFilter    = vk::Filter::eLinear;
		sampler.minFilter    = vk::Filter::eLinear;
		sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
		RequireVulkanSuccess(device.createSampler(&sampler, nullptr, &m_overlay_sampler),
		                     "create video-out overlay sampler");
		const vk::DescriptorSetLayoutBinding binding {0, vk::DescriptorType::eCombinedImageSampler,
		                                              1, vk::ShaderStageFlagBits::eFragment};
		vk::DescriptorSetLayoutCreateInfo    descriptors {};
		descriptors.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
		descriptors.bindingCount = 1;
		descriptors.pBindings    = &binding;
		RequireVulkanSuccess(
		    device.createDescriptorSetLayout(&descriptors, nullptr, &m_overlay_descriptors),
		    "create video-out overlay descriptor layout");
		const vk::PushConstantRange alpha {vk::ShaderStageFlagBits::eFragment, 0, sizeof(uint32_t)};
		vk::PipelineLayoutCreateInfo layout {};
		layout.setLayoutCount         = 1;
		layout.pSetLayouts            = &m_overlay_descriptors;
		layout.pushConstantRangeCount = 1;
		layout.pPushConstantRanges    = &alpha;
		RequireVulkanSuccess(device.createPipelineLayout(&layout, nullptr, &m_overlay_layout),
		                     "create video-out overlay pipeline layout");
		const auto vertex   = CompileSPV(GPU_BLIT_FS_TRIANGLE_SPV, device);
		const auto fragment = CompileSPV(GPU_VIDEO_OUT_OVERLAY_SPV, device);
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
		attachment.blendEnable         = VK_TRUE;
		attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
		attachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
		attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
		attachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
		attachment.colorWriteMask = vk::ColorComponentFlagBits::eR |
		                            vk::ColorComponentFlagBits::eG |
		                            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
		vk::PipelineColorBlendStateCreateInfo blend {};
		blend.attachmentCount = 1;
		blend.pAttachments    = &attachment;
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
		create.pColorBlendState    = &blend;
		create.pDynamicState       = &dynamic;
		create.layout              = m_overlay_layout;
		RequireVulkanSuccess(
		    device.createGraphicsPipelines(nullptr, 1, &create, nullptr, &m_overlay_pipeline),
		    "create video-out overlay pipeline");
		device.destroyShaderModule(fragment, nullptr);
		device.destroyShaderModule(vertex, nullptr);
	}
	auto& frame = *layer.frame;
	if (frame.view == nullptr) {
		vk::ImageViewCreateInfo view {};
		view.image            = frame.image.image;
		view.viewType         = vk::ImageViewType::e2D;
		view.format           = frame.image.format;
		view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		RequireVulkanSuccess(device.createImageView(&view, nullptr, &frame.view),
		                     "create video-out overlay image view");
	}
	const vk::DescriptorImageInfo image {m_overlay_sampler, frame.view,
	                                     vk::ImageLayout::eShaderReadOnlyOptimal};
	vk::WriteDescriptorSet        write {};
	write.dstBinding      = 0;
	write.descriptorCount = 1;
	write.descriptorType  = vk::DescriptorType::eCombinedImageSampler;
	write.pImageInfo      = &image;
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_overlay_layout, 0, 1, &write);
	const uint32_t flags = (layer.premultiplied_alpha ? 1u : 0u) |
	                       (m_hdr_active ? 2u : 0u) | (frame.hdr ? 4u : 0u) |
	                       (frame.image.format == vk::Format::eR16G16B16A16Sfloat ? 8u : 0u);
	command.pushConstants(m_overlay_layout, vk::ShaderStageFlagBits::eFragment, 0,
	                      sizeof(flags), &flags);
	command.bindPipeline(vk::PipelineBindPoint::eGraphics, m_overlay_pipeline);
	vk::RenderingAttachmentInfo attachment {};
	attachment.imageView   = m_image_views[m_image_index];
	attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	attachment.loadOp      = vk::AttachmentLoadOp::eLoad;
	attachment.storeOp     = vk::AttachmentStoreOp::eStore;
	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = m_extent;
	rendering.layerCount           = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments    = &attachment;
	command.beginRendering(&rendering);
	const vk::Viewport viewport {
	    0.0f, 0.0f, static_cast<float>(m_extent.width), static_cast<float>(m_extent.height),
	    0.0f, 1.0f};
	const vk::Rect2D scissor {{0, 0}, m_extent};
	command.setViewport(0, 1, &viewport);
	command.setScissor(0, 1, &scissor);
	command.draw(3, 1, 0, 0);
	command.endRendering();
}

void Swapchain::RecordPresentCommands(CommandBuffer& command, Presenter::Frame* source,
                                      const Presenter::Layer& overlay, bool draw_system_overlay,
                                      const vk::Rect2D* source_rect) {
	EXIT_IF(m_image_index >= m_images.size());
	auto       vk_command      = command.Handle();
	const bool draw_overlay    = overlay.frame != nullptr;
	const bool draw_attachment = draw_overlay || draw_system_overlay;
	if (source != nullptr) {
		source->Transit(vk_command, vk::ImageLayout::eTransferSrcOptimal,
		                vk::AccessFlagBits2::eTransferRead);
	}
	if (draw_overlay) {
		overlay.frame->Transit(vk_command, vk::ImageLayout::eShaderReadOnlyOptimal,
		                       vk::AccessFlagBits2::eShaderRead);
	}

	vk::ImageMemoryBarrier to_transfer {};
	to_transfer.sType                           = vk::StructureType::eImageMemoryBarrier;
	to_transfer.dstAccessMask                   = vk::AccessFlagBits::eTransferWrite;
	to_transfer.oldLayout                       = vk::ImageLayout::eUndefined;
	to_transfer.newLayout                       = vk::ImageLayout::eTransferDstOptimal;
	to_transfer.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	to_transfer.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	to_transfer.image                           = m_images[m_image_index];
	to_transfer.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	to_transfer.subresourceRange.baseMipLevel   = 0;
	to_transfer.subresourceRange.levelCount     = 1;
	to_transfer.subresourceRange.baseArrayLayer = 0;
	to_transfer.subresourceRange.layerCount     = 1;
	// Match the acquire wait stage so the layout transition cannot precede acquisition.
	vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                           vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlags {}, 0,
	                           nullptr, 0, nullptr, 1, &to_transfer);

	if (source != nullptr) {
		vk::ImageBlit region {};
		region.srcSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
		region.srcSubresource.mipLevel       = 0;
		region.srcSubresource.baseArrayLayer = 0;
		region.srcSubresource.layerCount     = 1;
		region.srcOffsets[1].x               = static_cast<int>(source->image.extent.width);
		region.srcOffsets[1].y               = static_cast<int>(source->image.extent.height);
		region.srcOffsets[1].z               = 1;
		if (source_rect != nullptr) {
			region.srcOffsets[0].x = source_rect->offset.x;
			region.srcOffsets[0].y = source_rect->offset.y;
			region.srcOffsets[1].x =
			    source_rect->offset.x + static_cast<int>(source_rect->extent.width);
			region.srcOffsets[1].y =
			    source_rect->offset.y + static_cast<int>(source_rect->extent.height);
		}
		region.dstSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
		region.dstSubresource.mipLevel       = 0;
		region.dstSubresource.baseArrayLayer = 0;
		region.dstSubresource.layerCount     = 1;
		region.dstOffsets[1].x               = static_cast<int>(m_extent.width);
		region.dstOffsets[1].y               = static_cast<int>(m_extent.height);
		region.dstOffsets[1].z               = 1;
		vk_command.blitImage(source->image.image, vk::ImageLayout::eTransferSrcOptimal,
		                     m_images[m_image_index], vk::ImageLayout::eTransferDstOptimal, 1,
		                     &region, vk::Filter::eLinear);
	} else {
		const vk::ClearColorValue black {};
		vk_command.clearColorImage(m_images[m_image_index], vk::ImageLayout::eTransferDstOptimal,
		                           &black, 1, &to_transfer.subresourceRange);
	}

	vk::ImageMemoryBarrier to_present {};
	to_present.sType               = vk::StructureType::eImageMemoryBarrier;
	to_present.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
	to_present.dstAccessMask       = draw_attachment ? vk::AccessFlagBits::eColorAttachmentRead |
	                                                       vk::AccessFlagBits::eColorAttachmentWrite
	                                                 : vk::AccessFlagBits::eMemoryRead;
	to_present.oldLayout           = vk::ImageLayout::eTransferDstOptimal;
	to_present.newLayout           = draw_attachment ? vk::ImageLayout::eColorAttachmentOptimal
	                                                 : vk::ImageLayout::ePresentSrcKHR;
	to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_present.image               = m_images[m_image_index];
	to_present.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	to_present.subresourceRange.baseMipLevel   = 0;
	to_present.subresourceRange.levelCount     = 1;
	to_present.subresourceRange.baseArrayLayer = 0;
	to_present.subresourceRange.layerCount     = 1;
	vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                           draw_attachment ? vk::PipelineStageFlagBits::eColorAttachmentOutput
	                                           : vk::PipelineStageFlagBits::eAllCommands,
	                           vk::DependencyFlagBits::eByRegion, 0, nullptr, 0, nullptr, 1,
	                           &to_present);
	if (draw_attachment) {
		if (draw_overlay) {
			DrawOverlay(vk_command, overlay);
			if (draw_system_overlay) {
				to_present.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
				to_present.oldLayout     = vk::ImageLayout::eColorAttachmentOptimal;
				vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
				                           vk::PipelineStageFlagBits::eColorAttachmentOutput,
				                           vk::DependencyFlagBits::eByRegion, 0, nullptr, 0,
				                           nullptr, 1, &to_present);
			}
		}
		if (draw_system_overlay) {
			m_system_overlay->Record(vk_command, m_image_views[m_image_index]);
		}
		to_present.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
		to_present.dstAccessMask = vk::AccessFlagBits::eMemoryRead;
		to_present.oldLayout     = vk::ImageLayout::eColorAttachmentOptimal;
		to_present.newLayout     = vk::ImageLayout::ePresentSrcKHR;
		vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
		                           vk::PipelineStageFlagBits::eAllCommands,
		                           vk::DependencyFlagBits::eByRegion, 0, nullptr, 0, nullptr, 1,
		                           &to_present);
	}
}

uint64_t Swapchain::Submit(CommandScheduler& scheduler) {
	EXIT_IF(m_frame_index >= m_image_acquired.size() || m_image_index >= m_render_complete.size());
	SubmitInfo submit;
	submit.AddWait(m_image_acquired[m_frame_index], 1, vk::PipelineStageFlagBits::eTransfer);
	submit.AddSignal(m_render_complete[m_image_index]);
	const auto tick                       = scheduler.Submit(submit);
	m_image_acquired_ticks[m_frame_index] = tick;
	return tick;
}

Swapchain::Status Swapchain::Present() {
	EXIT_IF(m_image_index >= m_render_complete.size());
	const auto         ready = m_render_complete[m_image_index];
	vk::PresentInfoKHR present {};
	present.sType              = vk::StructureType::ePresentInfoKHR;
	present.swapchainCount     = 1;
	present.pSwapchains        = &m_handle;
	present.pImageIndices      = &m_image_index;
	present.pWaitSemaphores    = &ready;
	present.waitSemaphoreCount = 1;

	vk::Result result;
	{
		Common::LockGuard lock(m_window.graphic_ctx.queue_mutex);
		result = m_window.graphic_ctx.queue.presentKHR(&present);
	}
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eSuboptimalKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eSuboptimalKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eErrorOutOfDateKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorSurfaceLostKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eErrorSurfaceLostKHR\n");
			return Status::SurfaceLost;
		default: EXIT("vkQueuePresentKHR failed: %s\n", vk::to_string(result).c_str());
	}
	m_frame_index = (m_frame_index + 1u) % static_cast<uint32_t>(m_images.size());
	return Status::Success;
}

Presenter::Presenter(WindowContext& window): m_impl(std::make_unique<Impl>(window)) {}

Presenter::~Presenter() = default;

Presenter::Frame& Presenter::PrepareFrame(CommandBuffer& buffer, const ImageInfo& info, bool hdr) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(buffer.IsInvalid());
	auto frame_format = info.pixel_format;
	switch (frame_format) {
		case vk::Format::eR8G8B8A8Srgb: frame_format = vk::Format::eR8G8B8A8Unorm; break;
		case vk::Format::eB8G8R8A8Srgb: frame_format = vk::Format::eB8G8R8A8Unorm; break;
		default: break;
	}
	auto* frame = m_impl->frames.Acquire({info.extent.width, info.extent.height}, frame_format);
	frame->hdr   = hdr;
	frame->blank = false;
	Common::LockGuard render_lock(m_impl->renderer.GetMutex());
	auto&             image = m_impl->ResolveSurface(info);
	if (image.backing.format == vk::Format::eUndefined) {
		EXIT("unsupported presentation source, image=%p\n", static_cast<const void*>(&image));
	}
	frame->Configure(m_impl->window.graphic_ctx,
	                 {image.backing.extent.width, image.backing.extent.height}, frame_format);
	frame->CopyFrom(buffer, image);
	return *frame;
}

Presenter::Frame& Presenter::PrepareHmdFrame(CommandBuffer&        buffer,
                                             const HmdComposition& composition) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(buffer.IsInvalid());
	EXIT_IF(composition.layer_count == 0 || composition.layer_count > composition.layers.size() ||
	        composition.width < 2 || composition.height == 0);
	const vk::Extent2D extent {composition.width, composition.height};
	const auto         format = m_impl->frames.GetFormat();
	auto*              frame  = m_impl->frames.Acquire(extent, format);
	frame->hdr                = false;
	frame->blank              = false;
	Common::LockGuard  render_lock(m_impl->renderer.GetMutex());
	auto&              graphics = m_impl->window.graphic_ctx;
	frame->Configure(graphics, extent, format, true);
	frame->hmd_fov  = composition.fov;
	frame->hmd_pose = composition.pose;
	buffer.EndRendering();
	auto command = buffer.Handle();

	// Each eye covers its half of the panel; layer order is blend order.
	const uint32_t eye_width = composition.width / 2;
	std::vector<HmdCompositor::Draw> draws;
	draws.reserve(static_cast<size_t>(composition.layer_count) * 2);
	for (uint32_t i = 0; i < composition.layer_count; i++) {
		const auto& layer = composition.layers[i];
		for (uint32_t eye = 0; eye < 2; eye++) {
			const auto&          source = layer.eyes[eye];
			HmdCompositor::Draw draw {};
			draw.source = m_impl->ResolveHmdTexture(source.texture, command);
			draw.target = {{static_cast<int32_t>(eye * eye_width), 0},
			               {eye_width, composition.height}};
			const auto width  = static_cast<float>(source.texture.Width5() + 1u);
			const auto height = static_cast<float>(source.texture.Height5() + 1u);
			draw.region       = {static_cast<float>(source.x) / width,
			                     static_cast<float>(source.y) / height,
			                     static_cast<float>(source.width) / width,
			                     static_cast<float>(source.height) / height};
			draw.blend        = layer.blend;
			draws.push_back(draw);
		}
	}

	if (frame->view == nullptr) {
		vk::ImageViewCreateInfo view {};
		view.image            = frame->image.image;
		view.viewType         = vk::ImageViewType::e2D;
		view.format           = frame->image.format;
		view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		RequireVulkanSuccess(graphics.device.createImageView(&view, nullptr, &frame->view),
		                     "create headset frame image view");
	}
	frame->Transit(command, vk::ImageLayout::eColorAttachmentOptimal,
	               vk::AccessFlagBits2::eColorAttachmentWrite);
	m_impl->hmd_compositor.Record(command, frame->view, format, extent, draws);
	return *frame;
}

Presenter::Frame& Presenter::PrepareBlankFrame(uint32_t width, uint32_t height, bool opaque,
                                               CommandBuffer* producer) {
	KYTY_PROFILER_FUNCTION();
	auto              format = m_impl->frames.GetFormat();
	auto*             frame  = m_impl->frames.Acquire({width, height}, format);
	frame->hdr   = false;
	frame->blank = true;
	Common::LockGuard render_lock(m_impl->renderer.GetMutex());
	frame->Configure(m_impl->window.graphic_ctx, {width, height}, format);
	vk::ClearColorValue clear {};
	clear.float32[3] = opaque ? 1.0f : 0.0f;
	if (producer != nullptr) {
		EXIT_IF(producer->IsInvalid());
		frame->Clear(*producer, clear);
	} else {
		auto& command = m_impl->present_scheduler.BeginCommand();
		frame->Clear(command, clear);
		frame->present_tick = m_impl->present_scheduler.Submit();
	}
	return *frame;
}

bool Presenter::PresentLastFrame() {
	Common::LockGuard lock(m_impl->present_mutex);
	if (m_impl->layers[0].frame == nullptr && m_impl->layers[1].frame == nullptr) {
		return false;
	}
	m_impl->Present();
	return true;
}

bool Presenter::PresentHmdMirror(const HmdMirror& mirror) {
	Common::LockGuard lock(m_impl->present_mutex);
	{
		Common::LockGuard hmd_lock(m_impl->hmd_present_mutex);
		if (m_impl->hmd_layer.frame == nullptr) {
			return false;
		}
	}
	m_impl->Present(&mirror);
	return true;
}

// The part of the left eye a window of the given size shows: a rectangle of the window's aspect
// ratio, centred on the eye's optical axis. CENTER keeps it inside the circle the lens shows,
// whose radius is the smallest of the eye's tangents, so masked corners stay out of view. WIDE
// takes the largest such rectangle inside the image. Sizes are in tangent space, where the
// rendered image is linear.
static vk::Rect2D MirrorRect(const Presenter::Frame& frame, vk::Extent2D window,
                             const Presenter::HmdMirror& mirror) {
	const float eye_width  = static_cast<float>(frame.image.extent.width / 2);
	const float eye_height = static_cast<float>(frame.image.extent.height);
	const auto& fov        = frame.hmd_fov[0];
	const float across     = fov.tan_out + fov.tan_in;
	const float down       = fov.tan_top + fov.tan_bottom;
	if (std::min({fov.tan_out, fov.tan_in, fov.tan_top, fov.tan_bottom}) <= 0.0f) {
		return {{0, 0}, {static_cast<uint32_t>(eye_width), static_cast<uint32_t>(eye_height)}};
	}
	const float aspect = static_cast<float>(window.width) / static_cast<float>(window.height);
	float       half_width = 0.0f;
	if (mirror.center) {
		const float radius = std::min({fov.tan_out, fov.tan_in, fov.tan_top, fov.tan_bottom});
		half_width         = radius * aspect / std::sqrt(aspect * aspect + 1.0f);
	} else {
		half_width = std::min({fov.tan_out, fov.tan_in, std::min(fov.tan_top, fov.tan_bottom) * aspect});
	}
	const float half_height = half_width / aspect;
	// The left eye's temporal side is on the left.
	const float x_scale = eye_width / across;
	const float y_scale = eye_height / down;
	const float x       = (fov.tan_out - half_width) * x_scale;
	const float y       = (fov.tan_top - half_height) * y_scale;
	return {{static_cast<int32_t>(x), static_cast<int32_t>(y)},
	        {static_cast<uint32_t>(2.0f * half_width * x_scale),
	         static_cast<uint32_t>(2.0f * half_height * y_scale)}};
}

bool Presenter::IsGuestPaused() const noexcept {
	return m_impl->window.loop.paused.load(std::memory_order_acquire);
}

bool Presenter::IsHdrSupported() const noexcept {
	return m_impl->swapchain.IsHdrSupported();
}

bool Presenter::NeedsSystemOverlayRefresh() const noexcept {
	const auto visual = GetSystemOverlayVisualState();
	return visual.active ||
	       visual.revision != m_impl->presented_overlay_revision.load(std::memory_order_acquire);
}

RenderContext& Presenter::Renderer() const noexcept {
	return m_impl->renderer;
}

void Presenter::Present(Frame& frame) {
	const Layer layer {&frame, 0, false};
	Present(std::span(&layer, 1));
}

void Presenter::Present(std::span<const Layer> layers) {
	// Replaces the layers of one output; false when it has none in this flip.
	auto update = [this, layers](bool hmd) {
		bool updated = false;
		for (const auto& layer: layers) {
			if ((layer.bus == HMD_BUS) != hmd) {
				continue;
			}
			EXIT_IF(!hmd && (layer.bus < 0 || layer.bus >= static_cast<int>(m_impl->layers.size())));
			m_impl->frames.ValidateForPresent(layer.frame);
			auto& previous = hmd ? m_impl->hmd_layer : m_impl->layers[layer.bus];
			if (previous.frame != nullptr) {
				m_impl->frames.Release(previous.frame);
			}
			previous = layer;
			updated  = true;
		}
		return updated;
	};
	{
		Common::LockGuard lock(m_impl->present_mutex);
		if (update(false)) {
			m_impl->Present();
		}
	}
	Common::LockGuard lock(m_impl->hmd_present_mutex);
	if (update(true)) {
		m_impl->PresentHmd();
	}
}

void Presenter::ClearLayer(int bus) {
	if (bus != HMD_BUS && static_cast<size_t>(bus) >= m_impl->layers.size()) {
		return;
	}
	Common::LockGuard lock(bus == HMD_BUS ? m_impl->hmd_present_mutex : m_impl->present_mutex);
	auto&             layer = bus == HMD_BUS ? m_impl->hmd_layer : m_impl->layers[bus];
	if (layer.frame != nullptr) {
		m_impl->frames.Release(layer.frame);
		layer = {};
	}
}

void Presenter::Impl::Present(const HmdMirror* mirror) {
	KYTY_PROFILER_FUNCTION();

	const auto overlay_visual = GetSystemOverlayVisualState();
	// Keep black/blank flips in the current color space; only real main-bus frames switch it.
	const auto* main_frame = layers[0].frame;
	const bool hdr_changed = main_frame != nullptr && !main_frame->blank &&
	                         swapchain.SetHdr(main_frame->hdr);
	// Some window systems keep presenting an old swapchain after a resize.
	if (hdr_changed || swapchain.NeedsResize()) {
		RecoverSwapchain(Swapchain::Status::Recreate);
	}
	for (uint32_t attempt = 0; attempt < 2; attempt++) {
		auto status = swapchain.AcquireNextImage(present_scheduler);
		if (status != Swapchain::Status::Success) {
			RecoverSwapchain(status);
			continue;
		}
		// A mirrored panel stays the headset's while it is read; the OpenXR frame thread takes
		// this lock before the renderer's.
		if (mirror != nullptr) {
			hmd_present_mutex.Lock();
		}
		{
			Common::LockGuard render_lock(renderer.GetMutex());
			auto&             command = present_scheduler.BeginCommand();
			const bool        draw_system_overlay =
			    overlay_visual.active && swapchain.PrepareSystemOverlay();
			auto* source = mirror != nullptr ? hmd_layer.frame : layers[0].frame;
			if (mirror != nullptr && source != nullptr) {
				const auto rect = MirrorRect(*source, swapchain.Extent(), *mirror);
				swapchain.RecordPresentCommands(command, source, layers[1], draw_system_overlay,
				                                &rect);
			} else {
				swapchain.RecordPresentCommands(command, source, layers[1], draw_system_overlay);
			}
			const auto tick = swapchain.Submit(present_scheduler);
			for (auto* frame: {source, layers[1].frame}) {
				if (frame != nullptr) {
					frame->present_tick = std::max(frame->present_tick, tick);
				}
			}
		}
		if (mirror != nullptr) {
			hmd_present_mutex.Unlock();
		}
		status = swapchain.Present();
		if (status != Swapchain::Status::Success) {
			RecoverSwapchain(status);
			continue;
		}

		presented_overlay_revision.store(overlay_visual.revision, std::memory_order_release);
		window.loop.presented_frames.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	LOGF("Vulkan presentation retry exhausted; dropping frame\n");
}

void Presenter::Impl::PresentHmd() {
	KYTY_PROFILER_FUNCTION();

	if (hmd_to_openxr) {
		PresentHmdToOpenxr();
	} else {
		PresentHmdToWindow();
	}
}

void Presenter::Impl::PresentHmdToOpenxr() {
	// The OpenXR frame thread shows the newest panel at the headset's own rate.
	auto* frame = hmd_layer.frame;
	EXIT_IF(frame == nullptr);
	Xr::PanelView view {};
	for (uint32_t eye = 0; eye < 2; eye++) {
		const auto& f = frame->hmd_fov[eye];
		view.fov[eye] = {f.tan_out, f.tan_in, f.tan_top, f.tan_bottom};
	}
	const auto& pose  = frame->hmd_pose;
	view.pose         = {pose.position, pose.orientation};
	view.flip_arg     = pose.flip_arg;
	view.world_locked = pose.valid && !pose.head_locked;
	{
		Common::LockGuard render_lock(renderer.GetMutex());
		auto&             command = present_scheduler.BeginCommand();
		frame->Transit(command.Handle(), vk::ImageLayout::eTransferSrcOptimal,
		               vk::AccessFlagBits2::eTransferRead);
		Xr::RecordPanelCopy(command.Handle(), frame->image.image, frame->image.format,
		                    {frame->image.extent.width, frame->image.extent.height}, view);
		frame->present_tick = present_scheduler.Submit();
	}
	Xr::PanelCopySubmitted();
}

void Presenter::Impl::PresentHmdToWindow() {
	auto* target = window.vr_window.get();
	EXIT_IF(target == nullptr || hmd_layer.frame == nullptr);
	if (hmd_swapchain == nullptr) {
		target->Show();
		hmd_swapchain = std::make_unique<Swapchain>(window, target);
		hmd_swapchain->Create();
	}
	if (hmd_swapchain->NeedsResize()) {
		hmd_swapchain->Recreate();
	}
	for (uint32_t attempt = 0; attempt < 2; attempt++) {
		auto status = hmd_swapchain->AcquireNextImage(present_scheduler);
		if (status != Swapchain::Status::Success) {
			hmd_swapchain->Recreate(status == Swapchain::Status::SurfaceLost);
			continue;
		}
		{
			Common::LockGuard render_lock(renderer.GetMutex());
			auto&             command = present_scheduler.BeginCommand();
			hmd_swapchain->RecordPresentCommands(command, hmd_layer.frame, {}, false);
			hmd_layer.frame->present_tick = hmd_swapchain->Submit(present_scheduler);
		}
		status = hmd_swapchain->Present();
		if (status != Swapchain::Status::Success) {
			hmd_swapchain->Recreate(status == Swapchain::Status::SurfaceLost);
			continue;
		}
		return;
	}
	LOGF("Vulkan headset presentation retry exhausted; dropping frame\n");
}

void Presenter::Discard(Frame& frame) {
	m_impl->frames.Release(&frame);
}

WindowContext::WindowContext() = default;

} // namespace Libs::Graphics
