#include "graphics/presentation/xr/xrFrameThread.h"

#ifdef KYTY_OPENXR

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/presentation/xr/xrInput.h"

#include <algorithm>
#include <cmath>

namespace Libs::Graphics::Xr {

// About 90 Hz, the PSVR2's slower refresh.
constexpr uint32_t IDLE_PERIOD_US = 11111;

// The panel holds gamma-encoded 8-bit UNORM pixels. Copying their bits into the matching sRGB
// format makes the runtime decode them correctly, without a shader pass.
static vk::Format SrgbFormat(vk::Format format) {
	switch (format) {
		case vk::Format::eB8G8R8A8Unorm: return vk::Format::eB8G8R8A8Srgb;
		case vk::Format::eR8G8B8A8Unorm: return vk::Format::eR8G8B8A8Srgb;
		default: return vk::Format::eUndefined;
	}
}

// The guest describes each eye relative to the nose; OpenXR uses left/right angles.
static XrFovf ToXrFov(const EyeFov& fov, uint32_t eye) {
	const float outward = std::atan(fov.tan_out);
	const float inward  = std::atan(fov.tan_in);
	XrFovf      result {};
	result.angleLeft  = eye == 0 ? -outward : -inward;
	result.angleRight = eye == 0 ? inward : outward;
	result.angleUp    = std::atan(fov.tan_top);
	result.angleDown  = -std::atan(fov.tan_bottom);
	return result;
}

FrameThread::FrameThread(Runtime& runtime): m_runtime(runtime) {
	m_thread = std::jthread([this](const std::stop_token& token) { Run(token); });
}

FrameThread::~FrameThread() {
	m_thread.request_stop();
	if (m_thread.joinable()) {
		m_thread.join();
	}
}

void FrameThread::Run(const std::stop_token& token) {
	KYTY_PROFILER_THREAD("Thread_XrFrame");

	auto* graphics = m_runtime.GetGraphics();
	vk::CommandPoolCreateInfo pool_info {};
	pool_info.flags            = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	pool_info.queueFamilyIndex = graphics->queue_family;
	RequireVulkanSuccess(graphics->device.createCommandPool(&pool_info, nullptr, &m_command_pool),
	                     "create OpenXR command pool");
	for (auto& copy: m_copies) {
		vk::CommandBufferAllocateInfo alloc_info {};
		alloc_info.commandPool        = m_command_pool;
		alloc_info.level              = vk::CommandBufferLevel::ePrimary;
		alloc_info.commandBufferCount = 1;
		RequireVulkanSuccess(graphics->device.allocateCommandBuffers(&alloc_info, &copy.buffer),
		                     "allocate OpenXR command buffer");
		vk::FenceCreateInfo fence_info {};
		fence_info.flags = vk::FenceCreateFlagBits::eSignaled;
		RequireVulkanSuccess(graphics->device.createFence(&fence_info, nullptr, &copy.fence),
		                     "create OpenXR fence");
	}

	while (!token.stop_requested()) {
		m_runtime.PollEvents();
		if (!m_runtime.IsSessionRunning() || !RunFrame()) {
			// Nothing paces us until the session runs, but the guest's headset clock keeps going.
			Common::Thread::SleepMicro(IDLE_PERIOD_US);
			if (!m_runtime.IsSessionRunning()) {
				m_runtime.Vblank();
			}
		}
	}

	WaitCopies();
	DestroySwapchain();
	for (auto& copy: m_copies) {
		graphics->device.destroyFence(copy.fence, nullptr);
	}
	graphics->device.destroyCommandPool(m_command_pool, nullptr);
}

bool FrameThread::RunFrame() {
	KYTY_PROFILER_BLOCK("XrFrame");

	auto* graphics = m_runtime.GetGraphics();
	auto  session  = m_runtime.GetSession();

	XrFrameWaitInfo wait_info {XR_TYPE_FRAME_WAIT_INFO};
	XrFrameState    frame_state {XR_TYPE_FRAME_STATE};
	if (!m_runtime.Check(xrWaitFrame(session, &wait_info, &frame_state), "xrWaitFrame")) {
		return false;
	}
	m_runtime.OnFrameWaited(frame_state);
	// Flips the guest completes here reach this frame's panel.
	m_runtime.Vblank();
	if (auto* input = m_runtime.GetInput(); input != nullptr) {
		input->Sync(frame_state.predictedDisplayTime, m_runtime.GetWorldSpace());
	}
	if (frame_state.predictedDisplayTime - m_last_calibration > 1000000000) {
		m_runtime.CalibrateClock();
		m_last_calibration = frame_state.predictedDisplayTime;
	}
	std::array<XrView, 2> eyes {{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
	const bool            have_eyes = LocateEyes(frame_state, eyes);
	if (have_eyes) {
		m_runtime.UpdateViews(eyes);
	} else {
		// A typical 64 mm IPD with a 90 degree field of view.
		for (uint32_t eye = 0; eye < 2; eye++) {
			eyes[eye].pose = {{0.0f, 0.0f, 0.0f, 1.0f}, {eye == 0 ? -0.032f : 0.032f, 0.0f, 0.0f}};
			eyes[eye].fov  = {-0.785f, 0.785f, 0.785f, -0.785f};
		}
	}

	// The runtime may use the graphics queue only inside begin/end frame and swapchain
	// acquire/release, so these are the only calls that take the queue lock.
	XrFrameBeginInfo begin_info {XR_TYPE_FRAME_BEGIN_INFO};
	XrResult         result = XR_SUCCESS;
	{
		Common::LockGuard lock(graphics->queue_mutex);
		result = xrBeginFrame(session, &begin_info);
	}
	if (!m_runtime.Check(result, "xrBeginFrame")) {
		return false;
	}

	std::array<XrCompositionLayerProjectionView, 2> views {};
	XrCompositionLayerProjection projection {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	const XrCompositionLayerBaseHeader* layers[] = {
	    reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};

	XrFrameEndInfo end_info {XR_TYPE_FRAME_END_INFO};
	end_info.displayTime          = frame_state.predictedDisplayTime;
	end_info.environmentBlendMode = m_runtime.GetBlendMode();
	if (frame_state.shouldRender == XR_TRUE && RenderPanel(eyes, projection, views)) {
		end_info.layerCount = 1;
		end_info.layers     = layers;
	}
	{
		Common::LockGuard lock(graphics->queue_mutex);
		result = xrEndFrame(session, &end_info);
	}
	if (!m_runtime.Check(result, "xrEndFrame")) {
		return false;
	}

	m_frame_count++;
	return true;
}

bool FrameThread::LocateEyes(const XrFrameState& frame_state, std::array<XrView, 2>& eyes) {
	XrViewLocateInfo locate_info {XR_TYPE_VIEW_LOCATE_INFO};
	locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locate_info.displayTime           = frame_state.predictedDisplayTime;
	locate_info.space                 = m_runtime.GetViewSpace();
	XrViewState view_state {XR_TYPE_VIEW_STATE};
	uint32_t    count = 0;
	return m_runtime.Check(xrLocateViews(m_runtime.GetSession(), &locate_info, &view_state,
	                                     static_cast<uint32_t>(eyes.size()), &count, eyes.data()),
	                       "xrLocateViews") &&
	       count == 2 && (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0 &&
	       (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
}

bool FrameThread::RenderPanel(const std::array<XrView, 2>&                     eyes,
                              XrCompositionLayerProjection&                    projection,
                              std::array<XrCompositionLayerProjectionView, 2>& views) {
	KYTY_PROFILER_FUNCTION();

	auto* graphics = m_runtime.GetGraphics();
	auto& panel    = m_runtime.GetPanel();

	// Held until the copy is submitted, so the panel image stays alive.
	Common::LockGuard panel_lock(panel.mutex);
	if (!panel.ready) {
		return false;
	}
	const vk::Extent2D extent {panel.image.extent.width, panel.image.extent.height};
	if (!UpdateSwapchain(panel.image.format, extent)) {
		return false;
	}

	XrSwapchainImageAcquireInfo acquire_info {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	uint32_t                    image_index = 0;
	XrResult                    result      = XR_SUCCESS;
	{
		Common::LockGuard lock(graphics->queue_mutex);
		result = xrAcquireSwapchainImage(m_swapchain.handle, &acquire_info, &image_index);
	}
	if (!m_runtime.Check(result, "xrAcquireSwapchainImage")) {
		return false;
	}
	XrSwapchainImageWaitInfo wait_info {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	wait_info.timeout = XR_INFINITE_DURATION;
	if (!m_runtime.Check(xrWaitSwapchainImage(m_swapchain.handle, &wait_info),
	                     "xrWaitSwapchainImage")) {
		return false;
	}

	auto& copy = m_copies[m_copy_index];
	m_copy_index = (m_copy_index + 1) % m_copies.size();
	RequireVulkanSuccess(graphics->device.waitForFences(1, &copy.fence, VK_TRUE, UINT64_MAX),
	                     "wait for OpenXR copy");
	RequireVulkanSuccess(graphics->device.resetFences(1, &copy.fence), "reset OpenXR fence");

	auto                       command = copy.buffer;
	vk::CommandBufferBeginInfo begin {};
	begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	RequireVulkanSuccess(command.begin(&begin), "begin OpenXR copy");

	// The whole image is overwritten, so its previous contents are discarded.
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask        = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
	barrier.srcAccessMask       = vk::AccessFlagBits2::eNone;
	barrier.dstStageMask        = vk::PipelineStageFlagBits2::eTransfer;
	barrier.dstAccessMask       = vk::AccessFlagBits2::eTransferWrite;
	barrier.oldLayout           = vk::ImageLayout::eUndefined;
	barrier.newLayout           = vk::ImageLayout::eTransferDstOptimal;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = m_swapchain.images[image_index];
	barrier.subresourceRange    = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 2};
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &barrier;
	command.pipelineBarrier2(&dependency);

	vk::ImageCopy region {};
	region.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 2};
	region.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 2};
	region.extent         = vk::Extent3D {extent.width, extent.height, 1};
	command.copyImage(panel.image.image, vk::ImageLayout::eTransferSrcOptimal,
	                  m_swapchain.images[image_index], vk::ImageLayout::eTransferDstOptimal, 1,
	                  &region);

	// Swapchain images go back to the runtime as color attachments.
	barrier.srcStageMask  = vk::PipelineStageFlagBits2::eTransfer;
	barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
	barrier.dstStageMask  = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
	barrier.dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead;
	barrier.oldLayout     = vk::ImageLayout::eTransferDstOptimal;
	barrier.newLayout     = vk::ImageLayout::eColorAttachmentOptimal;
	command.pipelineBarrier2(&dependency);
	RequireVulkanSuccess(command.end(), "end OpenXR copy");

	vk::SubmitInfo submit {};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers    = &command;
	XrSwapchainImageReleaseInfo release_info {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	{
		Common::LockGuard lock(graphics->queue_mutex);
		RequireVulkanSuccess(graphics->queue.submit(1, &submit, copy.fence), "submit OpenXR copy");
		result = xrReleaseSwapchainImage(m_swapchain.handle, &release_info);
	}
	if (!m_runtime.Check(result, "xrReleaseSwapchainImage")) {
		return false;
	}

	// A world-locked panel goes back where the guest rendered it, at the head pose it rendered
	// with, so the runtime reprojects it to where the head is now. A head-locked panel only gets
	// distortion. The eye offsets are the runtime's, as GetResult reported them.
	const auto& panel_view = panel.view;
	const bool  world      = panel_view.world_locked;
	const auto  head       = ToXrPose(panel_view.pose);
	projection.space     = world ? m_runtime.GetFrameSpace(panel_view.flip_arg)
	                             : m_runtime.GetViewSpace();
	projection.viewCount = static_cast<uint32_t>(views.size());
	projection.views     = views.data();
	for (uint32_t eye = 0; eye < 2; eye++) {
		auto& view = views[eye];
		view       = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
		view.pose  = world ? ComposePoses(head, eyes[eye].pose) : eyes[eye].pose;
		view.fov   = ToXrFov(panel_view.fov[eye], eye);
		view.subImage.swapchain        = m_swapchain.handle;
		view.subImage.imageRect.offset = {0, 0};
		view.subImage.imageRect.extent = {static_cast<int32_t>(extent.width),
		                                  static_cast<int32_t>(extent.height)};
		view.subImage.imageArrayIndex  = eye;
	}
	return true;
}

bool FrameThread::UpdateSwapchain(vk::Format panel_format, vk::Extent2D extent) {
	const auto format = SrgbFormat(panel_format);
	if (m_swapchain.handle != XR_NULL_HANDLE && m_swapchain.format == format &&
	    m_swapchain.extent == extent) {
		return true;
	}
	WaitCopies();
	DestroySwapchain();

	auto session = m_runtime.GetSession();
	uint32_t format_count = 0;
	if (!m_runtime.Check(xrEnumerateSwapchainFormats(session, 0, &format_count, nullptr),
	                     "xrEnumerateSwapchainFormats")) {
		return false;
	}
	std::vector<int64_t> formats(format_count);
	if (!m_runtime.Check(
	        xrEnumerateSwapchainFormats(session, format_count, &format_count, formats.data()),
	        "xrEnumerateSwapchainFormats")) {
		return false;
	}
	if (format == vk::Format::eUndefined ||
	    std::find(formats.begin(), formats.end(), static_cast<int64_t>(format)) == formats.end()) {
		EXIT("OpenXR: the runtime has no sRGB swapchain format matching the panel format %s\n",
		     vk::to_string(panel_format).c_str());
	}

	XrSwapchainCreateInfo create_info {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	create_info.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
	                          XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	create_info.format      = static_cast<int64_t>(format);
	create_info.sampleCount = 1;
	create_info.width       = extent.width;
	create_info.height      = extent.height;
	create_info.faceCount   = 1;
	create_info.arraySize   = 2;
	create_info.mipCount    = 1;
	if (!m_runtime.Check(xrCreateSwapchain(session, &create_info, &m_swapchain.handle),
	                     "xrCreateSwapchain")) {
		return false;
	}

	uint32_t image_count = 0;
	if (!m_runtime.Check(xrEnumerateSwapchainImages(m_swapchain.handle, 0, &image_count, nullptr),
	                     "xrEnumerateSwapchainImages")) {
		DestroySwapchain();
		return false;
	}
	std::vector<XrSwapchainImageVulkan2KHR> images(image_count,
	                                               {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
	if (!m_runtime.Check(xrEnumerateSwapchainImages(
	                         m_swapchain.handle, image_count, &image_count,
	                         reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
	                     "xrEnumerateSwapchainImages")) {
		DestroySwapchain();
		return false;
	}
	m_swapchain.format = format;
	m_swapchain.extent = extent;
	for (const auto& image: images) {
		m_swapchain.images.emplace_back(image.image);
	}
	LOGF("OpenXR: swapchain %ux%u x2, %s, %u images\n", extent.width, extent.height,
	     vk::to_string(format).c_str(), image_count);
	return true;
}

void FrameThread::DestroySwapchain() {
	if (m_swapchain.handle != XR_NULL_HANDLE) {
		xrDestroySwapchain(m_swapchain.handle);
	}
	m_swapchain = {};
}

void FrameThread::WaitCopies() {
	auto* graphics = m_runtime.GetGraphics();
	for (const auto& copy: m_copies) {
		if (copy.fence != nullptr) {
			RequireVulkanSuccess(
			    graphics->device.waitForFences(1, &copy.fence, VK_TRUE, UINT64_MAX),
			    "wait for OpenXR copy");
		}
	}
}

} // namespace Libs::Graphics::Xr

#endif // KYTY_OPENXR
