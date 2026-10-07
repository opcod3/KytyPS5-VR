#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_XR_XRFRAMETHREAD_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_XR_XRFRAMETHREAD_H_

#include "common/common.h"

#ifdef KYTY_OPENXR

#include "graphics/presentation/xr/xrInternal.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <stop_token>
#include <thread>
#include <vector>

namespace Libs::Graphics::Xr {

// Runs xrWaitFrame -> xrBeginFrame -> xrEndFrame at the runtime's rate. This is the headset's
// scan-out clock: it keeps submitting the newest panel whether or not the guest flips, as the
// PS5 reprojection thread does.
class FrameThread final {
public:
	explicit FrameThread(Runtime& runtime);
	~FrameThread();
	KYTY_CLASS_NO_COPY(FrameThread);

	[[nodiscard]] uint64_t GetFrameCount() const { return m_frame_count; }

private:
	struct Swapchain {
		XrSwapchain            handle = XR_NULL_HANDLE;
		vk::Format             format = vk::Format::eUndefined;
		vk::Extent2D           extent;
		std::vector<vk::Image> images;
	};

	// Command buffers that copy the panel into swapchain images, reused round robin.
	struct CopyCommand {
		vk::CommandBuffer buffer = nullptr;
		vk::Fence         fence  = nullptr;
	};

	void Run(const std::stop_token& token);
	bool RunFrame();
	// Eye poses in VIEW space at the display time; false when the runtime cannot locate them.
	bool LocateEyes(const XrFrameState& frame_state, std::array<XrView, 2>& eyes);
	// Copies the panel into the swapchain and fills the layer; false when there is no panel yet.
	bool RenderPanel(const std::array<XrView, 2>& eyes, XrCompositionLayerProjection& projection,
	                 std::array<XrCompositionLayerProjectionView, 2>& views);
	bool UpdateSwapchain(vk::Format panel_format, vk::Extent2D extent);
	void DestroySwapchain();
	void WaitCopies();

	Runtime& m_runtime;

	Swapchain                  m_swapchain;
	vk::CommandPool            m_command_pool = nullptr;
	std::array<CopyCommand, 3> m_copies {};
	uint32_t                   m_copy_index = 0;

	std::atomic<uint64_t> m_frame_count       = 0;
	XrTime                m_last_calibration = 0;

	std::jthread m_thread;
};

} // namespace Libs::Graphics::Xr

#endif // KYTY_OPENXR

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_XR_XRFRAMETHREAD_H_
