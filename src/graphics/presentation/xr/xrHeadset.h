#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_XR_XRHEADSET_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_XR_XRHEADSET_H_

#include "common/common.h"
#include "graphics/presentation/xr/xrRuntime.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace Libs::Graphics::Xr {

// What the public headset functions in xrRuntime.h run on: the OpenXR runtime, or the virtual
// headset. Each method behaves as the public function of the same name.
class Headset {
public:
	Headset() = default;
	virtual ~Headset() = default;
	KYTY_CLASS_NO_COPY(Headset);

	[[nodiscard]] virtual SessionState GetSessionState() const = 0;
	[[nodiscard]] virtual bool         IsUserPresent() const   = 0;
	virtual bool                       GetFieldOfView(EyeFov* fov)       = 0;
	virtual bool                       GetEyeSeparation(float* meters)   = 0;
	virtual void                       RequestRefreshRate(float hz)      = 0;
	// The display time flip_arg is shown at; rebind moves it to the next refresh it can make.
	virtual uint64_t BindDisplayTime(int64_t flip_arg, bool rebind) = 0;

	virtual bool             LocateHead(uint64_t time, int64_t flip_arg, HeadLocation* location) = 0;
	virtual CoordinateSystem GetCoordinateSystem()                                           = 0;
	virtual bool             SetCoordinateSystem(CoordinateSystem system)                    = 0;
	// pose == nullptr recentres on the head.
	virtual bool             ResetLocalCoordinate(const Pose* pose)                          = 0;
	virtual bool             LocateCoordinateSystem(CoordinateSystem system, Pose* origin)    = 0;
	[[nodiscard]] virtual uint32_t GetPlayAreaUpdateCounter() const                          = 0;
	virtual bool             GetPlayAreaBounds(PlayAreaBounds* bounds)                       = 0;
	virtual bool             HasPlayAreaBounds()                                             = 0;

	[[nodiscard]] virtual bool HasControllerInput() const                                    = 0;
	virtual bool GetControllerState(Hand hand, ControllerState* state)                       = 0;
	virtual bool LocateController(Hand hand, uint64_t time, Location* location)              = 0;
	virtual void ResetControllerOrientation(Hand hand)                                       = 0;
	virtual void SetControllerVibration(Hand hand, float amplitude)                          = 0;
	virtual void SetTriggerVibration(Hand hand, const TriggerVibration& vibration)           = 0;

	virtual void SetVblankHandler(std::function<void()> handler) = 0;
};

// The OpenXR runtime when Initialize succeeded, else nullptr (xrRuntime.cpp).
[[nodiscard]] Headset* GetRuntimeHeadset();
// Destroys the OpenXR runtime (xrRuntime.cpp).
void ShutdownRuntime();
// The virtual headset (xrVirtual.cpp).
[[nodiscard]] std::unique_ptr<Headset> CreateVirtualHeadset();

} // namespace Libs::Graphics::Xr

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_XR_XRHEADSET_H_
