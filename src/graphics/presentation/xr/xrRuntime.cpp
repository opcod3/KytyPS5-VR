#include "graphics/presentation/xr/xrRuntime.h"

#include "common/assert.h"
#include "common/logging/log.h"

#ifdef KYTY_OPENXR

#include "graphics/host_gpu/graphicContext.h"
#include "graphics/presentation/xr/xrFrameThread.h"
#include "graphics/presentation/xr/xrInput.h"
#include "graphics/presentation/xr/xrInternal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fmt/format.h>
#include <utility>
#include <vector>

namespace Libs::Graphics::Xr {

static std::unique_ptr<Runtime> g_runtime;

template <typename T>
static bool GetFunction(XrInstance instance, const char* name, T& out) {
	PFN_xrVoidFunction function = nullptr;
	if (XR_FAILED(xrGetInstanceProcAddr(instance, name, &function)) || function == nullptr) {
		LOGF("OpenXR: %s is unavailable\n", name);
		return false;
	}
	out = reinterpret_cast<T>(function);
	return true;
}

static bool HasExtension(const std::vector<XrExtensionProperties>& extensions, const char* name) {
	return std::any_of(extensions.begin(), extensions.end(), [name](const auto& e) {
		return std::strcmp(e.extensionName, name) == 0;
	});
}

static SessionState ToSessionState(XrSessionState state) {
	switch (state) {
		case XR_SESSION_STATE_IDLE: return SessionState::Idle;
		case XR_SESSION_STATE_READY: return SessionState::Ready;
		case XR_SESSION_STATE_SYNCHRONIZED: return SessionState::Synchronized;
		case XR_SESSION_STATE_VISIBLE: return SessionState::Visible;
		case XR_SESSION_STATE_FOCUSED: return SessionState::Focused;
		case XR_SESSION_STATE_STOPPING: return SessionState::Stopping;
		case XR_SESSION_STATE_LOSS_PENDING: return SessionState::LossPending;
		case XR_SESSION_STATE_EXITING: return SessionState::Exiting;
		default: return SessionState::None;
	}
}

static XRAPI_ATTR XrBool32 XRAPI_CALL
DebugMessengerCallback(XrDebugUtilsMessageSeverityFlagsEXT         severity,
                       XrDebugUtilsMessageTypeFlagsEXT             /*types*/,
                       const XrDebugUtilsMessengerCallbackDataEXT* data, void* /*user_data*/) {
	const char* level = (severity & XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0     ? "E"
	                    : (severity & XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0 ? "W"
	                                                                                        : "I";
	LOGF("[OpenXR][%s] %s: %s\n", level, data->functionName != nullptr ? data->functionName : "",
	     data->message != nullptr ? data->message : "");
	return XR_FALSE;
}

const char* ResultToString(XrInstance instance, XrResult result) {
	thread_local char buffer[XR_MAX_RESULT_STRING_SIZE] {};
	if (instance == XR_NULL_HANDLE || XR_FAILED(xrResultToString(instance, result, buffer))) {
		auto text = fmt::format("XrResult({})", static_cast<int32_t>(result));
		std::strncpy(buffer, text.c_str(), sizeof(buffer) - 1);
	}
	return buffer;
}

bool Runtime::Check(XrResult result, const char* operation) const {
	if (XR_SUCCEEDED(result)) {
		return true;
	}
	LOGF("OpenXR: %s failed: %s\n", operation, ResultToString(m_instance, result));
	return false;
}

Runtime::~Runtime() {
	StopSession();
	m_input.reset();
	if (m_debug_messenger != XR_NULL_HANDLE) {
		PFN_xrDestroyDebugUtilsMessengerEXT destroy = nullptr;
		if (GetFunction(m_instance, "xrDestroyDebugUtilsMessengerEXT", destroy)) {
			destroy(m_debug_messenger);
		}
	}
	if (m_instance != XR_NULL_HANDLE) {
		xrDestroyInstance(m_instance);
	}
}

bool Runtime::Create() {
	if (!CreateInstance() || !FindSystem()) {
		return false;
	}
	// The headset works without controllers.
	m_input = std::make_unique<Input>(*this);
	if (!m_input->Create()) {
		Log::WriteToConsoleAndLog("OpenXR: controller input is unavailable\n");
		m_input.reset();
	}
	return true;
}

bool Runtime::CreateInstance() {
	std::vector<XrExtensionProperties> available;
	uint32_t                           count = 0;
	if (!Check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr),
	           "xrEnumerateInstanceExtensionProperties")) {
		return false;
	}
	available.resize(count, {XR_TYPE_EXTENSION_PROPERTIES});
	if (!Check(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, available.data()),
	           "xrEnumerateInstanceExtensionProperties")) {
		return false;
	}

	std::vector<const char*> extensions;
	const auto require = [&](const char* name) {
		if (!HasExtension(available, name)) {
			Log::WriteToConsoleAndLog(
			    fmt::format("OpenXR: the runtime lacks required extension {}\n", name));
			return false;
		}
		extensions.push_back(name);
		return true;
	};
	const auto request = [&](const char* name) {
		if (!HasExtension(available, name)) {
			return false;
		}
		extensions.push_back(name);
		return true;
	};

	if (!require(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) {
		return false;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	m_ext.convert_time = require(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME);
	if (!m_ext.convert_time) {
		return false;
	}
#else
	// Guest time conversion needs XR_KHR_win32_convert_performance_counter_time, not done yet.
	Log::WriteToConsoleAndLog("OpenXR: only Linux is supported for now\n");
	return false;
#endif
	m_ext.user_presence = request(XR_EXT_USER_PRESENCE_EXTENSION_NAME);
	m_ext.refresh_rate  = request(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
	m_ext.eye_gaze      = request(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
	m_ext.debug_utils   = request(XR_EXT_DEBUG_UTILS_EXTENSION_NAME);
	const bool local_floor_ext = request(XR_EXT_LOCAL_FLOOR_EXTENSION_NAME);

	XrInstanceCreateInfo create_info {XR_TYPE_INSTANCE_CREATE_INFO};
	std::strncpy(create_info.applicationInfo.applicationName, "Kyty",
	             XR_MAX_APPLICATION_NAME_SIZE - 1);
	std::strncpy(create_info.applicationInfo.engineName, "Kyty", XR_MAX_ENGINE_NAME_SIZE - 1);
	create_info.applicationInfo.applicationVersion = 1;
	create_info.applicationInfo.engineVersion      = 1;
	create_info.enabledExtensionCount              = static_cast<uint32_t>(extensions.size());
	create_info.enabledExtensionNames              = extensions.data();

	// LOCAL_FLOOR is core in 1.1; 1.0 runtimes need XR_EXT_local_floor.
	XrResult result = XR_ERROR_API_VERSION_UNSUPPORTED;
	for (const XrVersion version: {XR_API_VERSION_1_1, XR_API_VERSION_1_0}) {
		create_info.applicationInfo.apiVersion = version;
		result = xrCreateInstance(&create_info, &m_instance);
		if (result != XR_ERROR_API_VERSION_UNSUPPORTED) {
			m_api_version = version;
			break;
		}
	}
	if (!Check(result, "xrCreateInstance")) {
		Log::WriteToConsoleAndLog(fmt::format("OpenXR: could not create an instance ({})\n",
		                                      ResultToString(XR_NULL_HANDLE, result)));
		return false;
	}
	m_ext.local_floor = XR_VERSION_MINOR(m_api_version) >= 1 || local_floor_ext;

	XrInstanceProperties properties {XR_TYPE_INSTANCE_PROPERTIES};
	if (Check(xrGetInstanceProperties(m_instance, &properties), "xrGetInstanceProperties")) {
		Log::WriteToConsoleAndLog(fmt::format(
		    "OpenXR: runtime {} {}.{}.{}, API {}.{}\n", properties.runtimeName,
		    XR_VERSION_MAJOR(properties.runtimeVersion), XR_VERSION_MINOR(properties.runtimeVersion),
		    XR_VERSION_PATCH(properties.runtimeVersion), XR_VERSION_MAJOR(m_api_version),
		    XR_VERSION_MINOR(m_api_version)));
	}
	for (const char* name: extensions) {
		LOGF("OpenXR: enabled extension %s\n", name);
	}

	if (m_ext.debug_utils) {
		PFN_xrCreateDebugUtilsMessengerEXT create = nullptr;
		if (GetFunction(m_instance, "xrCreateDebugUtilsMessengerEXT", create)) {
			XrDebugUtilsMessengerCreateInfoEXT info {XR_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
			info.messageSeverities = XR_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
			                         XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
			                         XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
			info.messageTypes = XR_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
			                    XR_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
			                    XR_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
			                    XR_DEBUG_UTILS_MESSAGE_TYPE_CONFORMANCE_BIT_EXT;
			info.userCallback = DebugMessengerCallback;
			(void)Check(create(m_instance, &info, &m_debug_messenger),
			            "xrCreateDebugUtilsMessengerEXT");
		}
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	if (!GetFunction(m_instance, "xrConvertTimespecTimeToTimeKHR", m_convert_timespec)) {
		return false;
	}
#endif
	if (m_ext.refresh_rate) {
		m_ext.refresh_rate =
		    GetFunction(m_instance, "xrEnumerateDisplayRefreshRatesFB", m_enumerate_refresh_rates) &&
		    GetFunction(m_instance, "xrRequestDisplayRefreshRateFB", m_request_refresh_rate);
	}
	return GetFunction(m_instance, "xrGetVulkanGraphicsRequirements2KHR",
	                   m_get_graphics_requirements) &&
	       GetFunction(m_instance, "xrCreateVulkanInstanceKHR", m_create_vulkan_instance) &&
	       GetFunction(m_instance, "xrGetVulkanGraphicsDevice2KHR", m_get_vulkan_device) &&
	       GetFunction(m_instance, "xrCreateVulkanDeviceKHR", m_create_vulkan_device);
}

bool Runtime::FindSystem() {
	XrSystemGetInfo get_info {XR_TYPE_SYSTEM_GET_INFO};
	get_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	const XrResult result = xrGetSystem(m_instance, &get_info, &m_system_id);
	if (result == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
		Log::WriteToConsoleAndLog("OpenXR: no headset is connected\n");
		return false;
	}
	if (!Check(result, "xrGetSystem")) {
		return false;
	}

	XrSystemUserPresencePropertiesEXT presence {XR_TYPE_SYSTEM_USER_PRESENCE_PROPERTIES_EXT};
	XrSystemProperties                properties {XR_TYPE_SYSTEM_PROPERTIES};
	if (m_ext.user_presence) {
		properties.next = &presence;
	}
	if (!Check(xrGetSystemProperties(m_instance, m_system_id, &properties),
	           "xrGetSystemProperties")) {
		return false;
	}
	m_system_name = properties.systemName;
	// Without presence reporting the headset counts as always worn.
	m_ext.user_presence = m_ext.user_presence && presence.supportsUserPresence == XR_TRUE;

	uint32_t view_count = 0;
	if (!Check(xrEnumerateViewConfigurationViews(m_instance, m_system_id,
	                                             XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
	                                             &view_count, nullptr),
	           "xrEnumerateViewConfigurationViews")) {
		return false;
	}
	if (view_count != 2) {
		Log::WriteToConsoleAndLog(
		    fmt::format("OpenXR: expected a stereo headset, but it has {} views\n", view_count));
		return false;
	}
	m_views[0] = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
	m_views[1] = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
	if (!Check(xrEnumerateViewConfigurationViews(m_instance, m_system_id,
	                                             XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
	                                             &view_count, m_views),
	           "xrEnumerateViewConfigurationViews")) {
		return false;
	}

	uint32_t blend_count = 0;
	if (Check(xrEnumerateEnvironmentBlendModes(m_instance, m_system_id,
	                                           XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 1,
	                                           &blend_count, &m_blend_mode),
	          "xrEnumerateEnvironmentBlendModes") &&
	    blend_count == 0) {
		m_blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	}

	Log::WriteToConsoleAndLog(fmt::format(
	    "OpenXR: headset {} (vendor {}), eye {}x{} (max {}x{}), tracking {}{}\n", m_system_name,
	    properties.vendorId, m_views[0].recommendedImageRectWidth,
	    m_views[0].recommendedImageRectHeight, m_views[0].maxImageRectWidth,
	    m_views[0].maxImageRectHeight,
	    properties.trackingProperties.orientationTracking == XR_TRUE ? "rotation" : "none",
	    properties.trackingProperties.positionTracking == XR_TRUE ? "+position" : ""));

	XrGraphicsRequirementsVulkan2KHR requirements {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
	if (!Check(m_get_graphics_requirements(m_instance, m_system_id, &requirements),
	           "xrGetVulkanGraphicsRequirements2KHR")) {
		return false;
	}
	const XrVersion target = XR_MAKE_VERSION(VK_API_VERSION_MAJOR(VULKAN_TARGET_API_VERSION),
	                                         VK_API_VERSION_MINOR(VULKAN_TARGET_API_VERSION), 0);
	if (target < requirements.minApiVersionSupported) {
		Log::WriteToConsoleAndLog(fmt::format(
		    "OpenXR: the runtime needs Vulkan {}.{} or newer\n",
		    XR_VERSION_MAJOR(requirements.minApiVersionSupported),
		    XR_VERSION_MINOR(requirements.minApiVersionSupported)));
		return false;
	}
	if (target > requirements.maxApiVersionSupported) {
		LOGF("OpenXR: the runtime was tested up to Vulkan %u.%u only\n",
		     static_cast<uint32_t>(XR_VERSION_MAJOR(requirements.maxApiVersionSupported)),
		     static_cast<uint32_t>(XR_VERSION_MINOR(requirements.maxApiVersionSupported)));
	}
	return true;
}

vk::Result Runtime::CreateVulkanInstance(const vk::InstanceCreateInfo& create_info,
                                         vk::Instance*                 instance) {
	const VkInstanceCreateInfo& vk_create_info = create_info;

	XrVulkanInstanceCreateInfoKHR info {XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
	info.systemId               = m_system_id;
	info.pfnGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
	info.vulkanCreateInfo       = &vk_create_info;

	VkInstance vk_instance = VK_NULL_HANDLE;
	VkResult   vk_result   = VK_SUCCESS;
	if (!Check(m_create_vulkan_instance(m_instance, &info, &vk_instance, &vk_result),
	           "xrCreateVulkanInstanceKHR")) {
		return vk::Result::eErrorInitializationFailed;
	}
	*instance = vk_instance;
	return static_cast<vk::Result>(vk_result);
}

vk::PhysicalDevice Runtime::GetVulkanPhysicalDevice(vk::Instance instance) {
	XrVulkanGraphicsDeviceGetInfoKHR info {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
	info.systemId       = m_system_id;
	info.vulkanInstance = instance;

	VkPhysicalDevice device = VK_NULL_HANDLE;
	if (!Check(m_get_vulkan_device(m_instance, &info, &device), "xrGetVulkanGraphicsDevice2KHR")) {
		return nullptr;
	}
	return device;
}

vk::Result Runtime::CreateVulkanDevice(vk::PhysicalDevice          physical_device,
                                       const vk::DeviceCreateInfo& create_info, vk::Device* device) {
	const VkDeviceCreateInfo& vk_create_info = create_info;

	XrVulkanDeviceCreateInfoKHR info {XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
	info.systemId               = m_system_id;
	info.pfnGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
	info.vulkanPhysicalDevice   = physical_device;
	info.vulkanCreateInfo       = &vk_create_info;

	VkDevice vk_device = VK_NULL_HANDLE;
	VkResult vk_result = VK_SUCCESS;
	if (!Check(m_create_vulkan_device(m_instance, &info, &vk_device, &vk_result),
	           "xrCreateVulkanDeviceKHR")) {
		return vk::Result::eErrorInitializationFailed;
	}
	*device = vk_device;
	return static_cast<vk::Result>(vk_result);
}

void Runtime::StartSession(GraphicContext* graphics) {
	EXIT_IF(graphics == nullptr || graphics->device == nullptr);
	EXIT_IF(m_session != XR_NULL_HANDLE);

	XrGraphicsBindingVulkan2KHR binding {XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
	binding.instance         = graphics->instance;
	binding.physicalDevice   = graphics->physical_device;
	binding.device           = graphics->device;
	binding.queueFamilyIndex = graphics->queue_family;
	binding.queueIndex       = 0;

	XrSessionCreateInfo create_info {XR_TYPE_SESSION_CREATE_INFO};
	create_info.next     = &binding;
	create_info.systemId = m_system_id;
	if (!Check(xrCreateSession(m_instance, &create_info, &m_session), "xrCreateSession")) {
		EXIT("OpenXR: could not create a session\n");
	}

	XrReferenceSpaceCreateInfo space_info {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	space_info.referenceSpaceType   = XR_REFERENCE_SPACE_TYPE_VIEW;
	space_info.poseInReferenceSpace = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
	if (!Check(xrCreateReferenceSpace(m_session, &space_info, &m_view_space),
	           "xrCreateReferenceSpace(VIEW)")) {
		EXIT("OpenXR: could not create the view space\n");
	}

	CreateSpaces();
	CalibrateClock();
	if (m_input != nullptr && !m_input->Attach(m_session)) {
		Log::WriteToConsoleAndLog("OpenXR: controller input is unavailable\n");
		m_input->Detach();
	}

	m_graphics     = graphics;
	m_state        = SessionState::Idle;
	m_frame_thread = std::make_unique<FrameThread>(*this);
	LOGF("OpenXR: session created\n");
}

void Runtime::StopSession() {
	m_frame_thread.reset();
	DestroyPanel();
	if (m_input != nullptr) {
		m_input->Detach();
	}
	DestroySpaces();
	if (m_view_space != XR_NULL_HANDLE) {
		xrDestroySpace(m_view_space);
		m_view_space = XR_NULL_HANDLE;
	}
	if (m_session != XR_NULL_HANDLE) {
		xrDestroySession(m_session);
		m_session         = XR_NULL_HANDLE;
		m_session_running = false;
		m_state           = SessionState::None;
	}
	m_graphics = nullptr;
	Common::LockGuard lock(m_bounds_mutex);
	m_bounds_stale = true;
}

void Runtime::DestroyPanel() {
	Common::LockGuard lock(m_panel.mutex);
	m_panel.ready = false;
	if (m_panel.image.image == nullptr) {
		return;
	}
	// Earlier copies into and out of the panel may still be running.
	{
		Common::LockGuard queue_lock(m_graphics->queue_mutex);
		RequireVulkanSuccess(m_graphics->queue.waitIdle(), "wait for the headset panel");
	}
	m_graphics->DeleteImage(m_panel.image);
	m_panel.layout = vk::ImageLayout::eUndefined;
}

void Runtime::RecordPanelCopy(vk::CommandBuffer command, vk::Image source, vk::Format format,
                              vk::Extent2D extent, const PanelView& view) {
	EXIT_IF(m_graphics == nullptr || extent.width < 2 || extent.height == 0);
	const vk::Extent2D eye {extent.width / 2, extent.height};

	auto& image = m_panel.image;
	if (image.image != nullptr && (image.format != format || image.extent.width != eye.width ||
	                               image.extent.height != eye.height)) {
		DestroyPanel();
	}

	Common::LockGuard lock(m_panel.mutex);
	if (image.image == nullptr) {
		vk::ImageCreateInfo info {};
		info.imageType     = vk::ImageType::e2D;
		info.format        = format;
		info.extent        = vk::Extent3D {eye.width, eye.height, 1};
		info.mipLevels     = 1;
		info.arrayLayers   = 2;
		info.samples       = vk::SampleCountFlagBits::e1;
		info.tiling        = vk::ImageTiling::eOptimal;
		info.usage         = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
		info.sharingMode   = vk::SharingMode::eExclusive;
		info.initialLayout = vk::ImageLayout::eUndefined;
		if (!m_graphics->CreateImage(info, image)) {
			EXIT("OpenXR: could not allocate the %ux%u headset panel\n", eye.width, eye.height);
		}
		m_panel.layout = vk::ImageLayout::eUndefined;
		LOGF("OpenXR: headset panel %ux%u per eye, format %s\n", eye.width, eye.height,
		     vk::to_string(format).c_str());
	}

	// The frame thread's earlier reads of the panel are transfers on the same queue.
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask        = vk::PipelineStageFlagBits2::eTransfer;
	barrier.srcAccessMask       = vk::AccessFlagBits2::eNone;
	barrier.dstStageMask        = vk::PipelineStageFlagBits2::eTransfer;
	barrier.dstAccessMask       = vk::AccessFlagBits2::eTransferWrite;
	barrier.oldLayout           = m_panel.layout;
	barrier.newLayout           = vk::ImageLayout::eTransferDstOptimal;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = image.image;
	barrier.subresourceRange    = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 2};
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &barrier;
	command.pipelineBarrier2(&dependency);

	std::array<vk::ImageCopy, 2> regions {};
	for (uint32_t i = 0; i < 2; i++) {
		regions[i].srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
		regions[i].srcOffset      = vk::Offset3D {static_cast<int32_t>(i * eye.width), 0, 0};
		regions[i].dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, i, 1};
		regions[i].extent         = vk::Extent3D {eye.width, eye.height, 1};
	}
	command.copyImage(source, vk::ImageLayout::eTransferSrcOptimal, image.image,
	                  vk::ImageLayout::eTransferDstOptimal, static_cast<uint32_t>(regions.size()),
	                  regions.data());

	barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
	barrier.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
	barrier.oldLayout     = vk::ImageLayout::eTransferDstOptimal;
	barrier.newLayout     = vk::ImageLayout::eTransferSrcOptimal;
	command.pipelineBarrier2(&dependency);

	m_panel.layout      = vk::ImageLayout::eTransferSrcOptimal;
	m_panel.pending_view = view;
}

void Runtime::PanelCopySubmitted() {
	Common::LockGuard lock(m_panel.mutex);
	m_panel.view  = m_panel.pending_view;
	m_panel.ready = m_panel.image.image != nullptr;
}

void Runtime::RequestRefreshRate(float hz) {
	if (!m_ext.refresh_rate || m_session == XR_NULL_HANDLE) {
		LOGF("OpenXR: cannot request %.2f Hz; the runtime keeps its own rate\n", hz);
		return;
	}
	uint32_t count = 0;
	if (!Check(m_enumerate_refresh_rates(m_session, 0, &count, nullptr),
	           "xrEnumerateDisplayRefreshRatesFB") ||
	    count == 0) {
		return;
	}
	std::vector<float> rates(count);
	if (!Check(m_enumerate_refresh_rates(m_session, count, &count, rates.data()),
	           "xrEnumerateDisplayRefreshRatesFB")) {
		return;
	}
	// The runtime accepts only a rate it enumerated.
	const float nearest = *std::min_element(rates.begin(), rates.end(), [hz](float a, float b) {
		return std::abs(a - hz) < std::abs(b - hz);
	});
	if (Check(m_request_refresh_rate(m_session, nearest), "xrRequestDisplayRefreshRateFB")) {
		LOGF("OpenXR: requested %.2f Hz for %.2f Hz\n", nearest, hz);
	}
}

void Runtime::SetVblankHandler(std::function<void()> handler) {
	Common::LockGuard lock(m_vblank_mutex);
	m_vblank_handler = std::move(handler);
}

void Runtime::Vblank() {
	Common::LockGuard lock(m_vblank_mutex);
	if (m_vblank_handler) {
		m_vblank_handler();
	}
}

void Runtime::HandleSessionStateChange(const XrEventDataSessionStateChanged& event) {
	if (event.session != m_session) {
		return;
	}
	m_state = ToSessionState(event.state);
	LOGF("OpenXR: session state %d\n", static_cast<int>(event.state));

	switch (event.state) {
		case XR_SESSION_STATE_READY: {
			XrSessionBeginInfo begin_info {XR_TYPE_SESSION_BEGIN_INFO};
			begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			if (Check(xrBeginSession(m_session, &begin_info), "xrBeginSession")) {
				m_session_running = true;
			}
			break;
		}
		case XR_SESSION_STATE_STOPPING:
			m_session_running = false;
			(void)Check(xrEndSession(m_session), "xrEndSession");
			break;
		case XR_SESSION_STATE_LOSS_PENDING:
		case XR_SESSION_STATE_EXITING:
			// The headset stays dark until the emulator restarts.
			m_session_running = false;
			Log::WriteToConsoleAndLog("OpenXR: the runtime ended the session\n");
			break;
		default: break;
	}
}

void Runtime::PollEvents() {
	for (;;) {
		XrEventDataBuffer event {XR_TYPE_EVENT_DATA_BUFFER};
		const XrResult    result = xrPollEvent(m_instance, &event);
		if (result != XR_SUCCESS) {
			if (result != XR_EVENT_UNAVAILABLE) {
				(void)Check(result, "xrPollEvent");
			}
			return;
		}

		switch (event.type) {
			case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
				HandleSessionStateChange(
				    *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event));
				break;
			case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
				m_session_running = false;
				Log::WriteToConsoleAndLog("OpenXR: the runtime is going away\n");
				break;
			case XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT:
				m_user_present =
				    reinterpret_cast<const XrEventDataUserPresenceChangedEXT*>(&event)
				        ->isUserPresent == XR_TRUE;
				break;
			case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
				// The guest learns of it through the play area counter.
				m_play_area_counter++;
				{
					Common::LockGuard lock(m_bounds_mutex);
					m_bounds_stale = true;
				}
				break;
			case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
				const auto* changed =
				    reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB*>(&event);
				LOGF("OpenXR: refresh rate %.2f -> %.2f Hz\n", changed->fromDisplayRefreshRate,
				     changed->toDisplayRefreshRate);
				break;
			}
			case XR_TYPE_EVENT_DATA_EVENTS_LOST:
				LOGF("OpenXR: %u events lost\n",
				     reinterpret_cast<const XrEventDataEventsLost*>(&event)->lostEventCount);
				break;
			default: LOGF("OpenXR: unhandled event %d\n", static_cast<int>(event.type)); break;
		}
	}
}

bool Initialize() {
	EXIT_IF(g_runtime != nullptr);

	auto runtime = std::make_unique<Runtime>();
	if (!runtime->Create()) {
		return false;
	}
	g_runtime = std::move(runtime);
	return true;
}

bool IsActive() {
	return g_runtime != nullptr;
}

void Shutdown() {
	g_runtime.reset();
}

vk::Result CreateVulkanInstance(const vk::InstanceCreateInfo& create_info,
                                vk::Instance*                 instance) {
	EXIT_IF(g_runtime == nullptr);
	return g_runtime->CreateVulkanInstance(create_info, instance);
}

vk::PhysicalDevice GetVulkanPhysicalDevice(vk::Instance instance) {
	EXIT_IF(g_runtime == nullptr);
	return g_runtime->GetVulkanPhysicalDevice(instance);
}

vk::Result CreateVulkanDevice(vk::PhysicalDevice          physical_device,
                              const vk::DeviceCreateInfo& create_info, vk::Device* device) {
	EXIT_IF(g_runtime == nullptr);
	return g_runtime->CreateVulkanDevice(physical_device, create_info, device);
}

void StartSession(GraphicContext* graphics) {
	EXIT_IF(g_runtime == nullptr);
	g_runtime->StartSession(graphics);
}

void StopSession() {
	if (g_runtime != nullptr) {
		g_runtime->StopSession();
	}
}

SessionState GetSessionState() {
	return g_runtime != nullptr ? g_runtime->GetSessionState() : SessionState::None;
}

bool IsHeadsetReady() {
	switch (GetSessionState()) {
		case SessionState::Synchronized:
		case SessionState::Visible:
		case SessionState::Focused: return true;
		default: return false;
	}
}

bool IsUserPresent() {
	return g_runtime != nullptr && g_runtime->IsUserPresent();
}

bool GetFieldOfView(EyeFov* fov) {
	EXIT_IF(fov == nullptr);
	std::array<XrView, 2> views {};
	if (g_runtime == nullptr || !g_runtime->GetViews(&views)) {
		return false;
	}
	// The guest describes one eye and mirrors it for the other; take the wider side of both.
	const auto& left  = views[0].fov;
	const auto& right = views[1].fov;
	fov->tan_out      = std::max(std::tan(-left.angleLeft), std::tan(right.angleRight));
	fov->tan_in       = std::max(std::tan(left.angleRight), std::tan(-right.angleLeft));
	fov->tan_top      = std::max(std::tan(left.angleUp), std::tan(right.angleUp));
	fov->tan_bottom   = std::max(std::tan(-left.angleDown), std::tan(-right.angleDown));
	return true;
}

bool GetEyeSeparation(float* meters) {
	EXIT_IF(meters == nullptr);
	std::array<XrView, 2> views {};
	if (g_runtime == nullptr || !g_runtime->GetViews(&views)) {
		return false;
	}
	const auto& a = views[0].pose.position;
	const auto& b = views[1].pose.position;
	*meters = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
	                    (a.z - b.z) * (a.z - b.z));
	return true;
}

uint64_t BeginFrame(int64_t flip_arg) {
	EXIT_IF(g_runtime == nullptr);
	return g_runtime->BindDisplayTime(flip_arg, true);
}

uint64_t GetPredictedDisplayTime(int64_t flip_arg) {
	EXIT_IF(g_runtime == nullptr);
	return g_runtime->BindDisplayTime(flip_arg, false);
}

bool LocateHead(uint64_t time, int64_t flip_arg, HeadLocation* location) {
	return g_runtime != nullptr && g_runtime->LocateHead(time, flip_arg, location);
}

CoordinateSystem GetCoordinateSystem() {
	return g_runtime != nullptr ? g_runtime->GetCoordinateSystem() : CoordinateSystem::LocalFloor;
}

bool SetCoordinateSystem(CoordinateSystem system) {
	return g_runtime != nullptr && g_runtime->SetCoordinateSystem(system);
}

bool ResetLocalCoordinate() {
	return g_runtime != nullptr && g_runtime->ResetLocalCoordinate(nullptr);
}

bool ResetLocalCoordinate(const Pose& pose) {
	return g_runtime != nullptr && g_runtime->ResetLocalCoordinate(&pose);
}

bool LocateCoordinateSystem(CoordinateSystem system, Pose* origin) {
	return g_runtime != nullptr && g_runtime->LocateCoordinateSystem(system, origin);
}

uint32_t GetPlayAreaUpdateCounter() {
	return g_runtime != nullptr ? g_runtime->GetPlayAreaUpdateCounter() : 0;
}

bool GetPlayAreaBounds(PlayAreaBounds* bounds) {
	return g_runtime != nullptr && g_runtime->GetPlayAreaBounds(bounds);
}

bool HasPlayAreaBounds() {
	return g_runtime != nullptr && g_runtime->HasPlayAreaBounds();
}

bool GetControllerState(Hand hand, ControllerState* state) {
	EXIT_IF(state == nullptr);
	*state = {};
	return g_runtime != nullptr && g_runtime->GetInput() != nullptr &&
	       g_runtime->GetInput()->GetState(hand, state);
}

bool LocateController(Hand hand, uint64_t time, Location* location) {
	return g_runtime != nullptr && g_runtime->LocateController(hand, time, location);
}

void ResetControllerOrientation(Hand hand) {
	if (g_runtime != nullptr && g_runtime->GetInput() != nullptr) {
		g_runtime->GetInput()->ResetOrientation(hand);
	}
}

void SetControllerVibration(Hand hand, float amplitude) {
	if (g_runtime != nullptr && g_runtime->GetInput() != nullptr) {
		g_runtime->GetInput()->SetVibration(hand, amplitude);
	}
}

void RecordPanelCopy(vk::CommandBuffer command, vk::Image source, vk::Format format,
                     vk::Extent2D extent, const PanelView& view) {
	EXIT_IF(g_runtime == nullptr);
	g_runtime->RecordPanelCopy(command, source, format, extent, view);
}

void PanelCopySubmitted() {
	EXIT_IF(g_runtime == nullptr);
	g_runtime->PanelCopySubmitted();
}

void RequestRefreshRate(float hz) {
	if (g_runtime != nullptr) {
		g_runtime->RequestRefreshRate(hz);
	}
}

void SetVblankHandler(std::function<void()> handler) {
	if (g_runtime != nullptr) {
		g_runtime->SetVblankHandler(std::move(handler));
	}
}

} // namespace Libs::Graphics::Xr

#else // KYTY_OPENXR

namespace Libs::Graphics::Xr {

bool Initialize() {
	Log::WriteToConsoleAndLog("OpenXR: this build has no OpenXR support\n");
	return false;
}

bool IsActive() {
	return false;
}

void Shutdown() {}

vk::Result CreateVulkanInstance(const vk::InstanceCreateInfo& /*create_info*/,
                                vk::Instance* /*instance*/) {
	EXIT("OpenXR: this build has no OpenXR support\n");
	return vk::Result::eErrorInitializationFailed;
}

vk::PhysicalDevice GetVulkanPhysicalDevice(vk::Instance /*instance*/) {
	return nullptr;
}

vk::Result CreateVulkanDevice(vk::PhysicalDevice /*physical_device*/,
                              const vk::DeviceCreateInfo& /*create_info*/,
                              vk::Device* /*device*/) {
	EXIT("OpenXR: this build has no OpenXR support\n");
	return vk::Result::eErrorInitializationFailed;
}

void StartSession(GraphicContext* /*graphics*/) {}

void StopSession() {}

SessionState GetSessionState() {
	return SessionState::None;
}

bool IsHeadsetReady() {
	return false;
}

bool IsUserPresent() {
	return false;
}

bool GetFieldOfView(EyeFov* /*fov*/) {
	return false;
}

bool GetEyeSeparation(float* /*meters*/) {
	return false;
}

uint64_t BeginFrame(int64_t /*flip_arg*/) {
	EXIT("OpenXR: this build has no OpenXR support\n");
	return 0;
}

uint64_t GetPredictedDisplayTime(int64_t /*flip_arg*/) {
	EXIT("OpenXR: this build has no OpenXR support\n");
	return 0;
}

bool LocateHead(uint64_t /*time*/, int64_t /*flip_arg*/, HeadLocation* /*location*/) {
	return false;
}

CoordinateSystem GetCoordinateSystem() {
	return CoordinateSystem::LocalFloor;
}

bool SetCoordinateSystem(CoordinateSystem /*system*/) {
	return false;
}

bool ResetLocalCoordinate() {
	return false;
}

bool ResetLocalCoordinate(const Pose& /*pose*/) {
	return false;
}

bool LocateCoordinateSystem(CoordinateSystem /*system*/, Pose* /*origin*/) {
	return false;
}

uint32_t GetPlayAreaUpdateCounter() {
	return 0;
}

bool GetPlayAreaBounds(PlayAreaBounds* /*bounds*/) {
	return false;
}

bool HasPlayAreaBounds() {
	return false;
}

bool GetControllerState(Hand /*hand*/, ControllerState* state) {
	*state = {};
	return false;
}

bool LocateController(Hand /*hand*/, uint64_t /*time*/, Location* /*location*/) {
	return false;
}

void ResetControllerOrientation(Hand /*hand*/) {}

void SetControllerVibration(Hand /*hand*/, float /*amplitude*/) {}

void RecordPanelCopy(vk::CommandBuffer /*command*/, vk::Image /*source*/, vk::Format /*format*/,
                     vk::Extent2D /*extent*/, const PanelView& /*view*/) {
	EXIT("OpenXR: this build has no OpenXR support\n");
}

void PanelCopySubmitted() {}

void RequestRefreshRate(float /*hz*/) {}

void SetVblankHandler(std::function<void()> /*handler*/) {}

} // namespace Libs::Graphics::Xr

#endif // KYTY_OPENXR
