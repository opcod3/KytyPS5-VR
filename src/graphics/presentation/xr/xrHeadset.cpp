#include "graphics/presentation/xr/xrHeadset.h"

#include "common/assert.h"

#include <memory>
#include <utility>

namespace Libs::Graphics::Xr {

static std::unique_ptr<Headset> g_virtual;

// The OpenXR runtime when there is one, else the virtual headset, else nullptr.
static Headset* Current() {
	if (auto* runtime = GetRuntimeHeadset(); runtime != nullptr) {
		return runtime;
	}
	return g_virtual.get();
}

void InitializeVirtual() {
	EXIT_IF(g_virtual != nullptr);
	g_virtual = CreateVirtualHeadset();
}

void Shutdown() {
	ShutdownRuntime();
	g_virtual.reset();
}

bool HasHeadset() {
	return Current() != nullptr;
}

SessionState GetSessionState() {
	auto* headset = Current();
	return headset != nullptr ? headset->GetSessionState() : SessionState::None;
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
	auto* headset = Current();
	return headset != nullptr && headset->IsUserPresent();
}

bool GetFieldOfView(EyeFov* fov) {
	EXIT_IF(fov == nullptr);
	auto* headset = Current();
	return headset != nullptr && headset->GetFieldOfView(fov);
}

bool GetEyeSeparation(float* meters) {
	EXIT_IF(meters == nullptr);
	auto* headset = Current();
	return headset != nullptr && headset->GetEyeSeparation(meters);
}

void RequestRefreshRate(float hz) {
	if (auto* headset = Current(); headset != nullptr) {
		headset->RequestRefreshRate(hz);
	}
}

uint64_t BeginFrame(int64_t flip_arg) {
	auto* headset = Current();
	EXIT_IF(headset == nullptr);
	return headset->BindDisplayTime(flip_arg, true);
}

uint64_t GetPredictedDisplayTime(int64_t flip_arg) {
	auto* headset = Current();
	EXIT_IF(headset == nullptr);
	return headset->BindDisplayTime(flip_arg, false);
}

bool LocateHead(uint64_t time, int64_t flip_arg, HeadLocation* location) {
	auto* headset = Current();
	return headset != nullptr && headset->LocateHead(time, flip_arg, location);
}

CoordinateSystem GetCoordinateSystem() {
	auto* headset = Current();
	return headset != nullptr ? headset->GetCoordinateSystem() : CoordinateSystem::LocalFloor;
}

bool SetCoordinateSystem(CoordinateSystem system) {
	auto* headset = Current();
	return headset != nullptr && headset->SetCoordinateSystem(system);
}

bool ResetLocalCoordinate() {
	auto* headset = Current();
	return headset != nullptr && headset->ResetLocalCoordinate(nullptr);
}

bool ResetLocalCoordinate(const Pose& pose) {
	auto* headset = Current();
	return headset != nullptr && headset->ResetLocalCoordinate(&pose);
}

bool LocateCoordinateSystem(CoordinateSystem system, Pose* origin) {
	auto* headset = Current();
	return headset != nullptr && headset->LocateCoordinateSystem(system, origin);
}

uint32_t GetPlayAreaUpdateCounter() {
	auto* headset = Current();
	return headset != nullptr ? headset->GetPlayAreaUpdateCounter() : 0;
}

bool GetPlayAreaBounds(PlayAreaBounds* bounds) {
	auto* headset = Current();
	return headset != nullptr && headset->GetPlayAreaBounds(bounds);
}

bool HasPlayAreaBounds() {
	auto* headset = Current();
	return headset != nullptr && headset->HasPlayAreaBounds();
}

bool HasControllerInput() {
	auto* headset = Current();
	return headset != nullptr && headset->HasControllerInput();
}

bool GetControllerState(Hand hand, ControllerState* state) {
	EXIT_IF(state == nullptr);
	*state        = {};
	auto* headset = Current();
	return headset != nullptr && headset->GetControllerState(hand, state);
}

bool LocateController(Hand hand, uint64_t time, Location* location) {
	auto* headset = Current();
	return headset != nullptr && headset->LocateController(hand, time, location);
}

void ResetControllerOrientation(Hand hand) {
	if (auto* headset = Current(); headset != nullptr) {
		headset->ResetControllerOrientation(hand);
	}
}

void SetControllerVibration(Hand hand, float amplitude) {
	if (auto* headset = Current(); headset != nullptr) {
		headset->SetControllerVibration(hand, amplitude);
	}
}

void SetTriggerVibration(Hand hand, const TriggerVibration& vibration) {
	if (auto* headset = Current(); headset != nullptr) {
		headset->SetTriggerVibration(hand, vibration);
	}
}

void SetVblankHandler(std::function<void()> handler) {
	if (auto* headset = Current(); headset != nullptr) {
		headset->SetVblankHandler(std::move(handler));
	}
}

} // namespace Libs::Graphics::Xr
