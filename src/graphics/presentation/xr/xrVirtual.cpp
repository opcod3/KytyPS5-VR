#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/presentation/xr/xrHeadset.h"
#include "kernel/pthread.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <stop_token>
#include <thread>
#include <utility>

namespace Libs::Graphics::Xr {

// A headset that never moves: the head 1.6 m above the floor looking down -Z, the hands in front
// of it, and a 2 m square play area around the origin. Its Sense controllers have no input of
// their own; the pad library maps the host gamepad onto them. Frames go to a desktop window, and
// its clock ticks at the refresh rate the guest asks for.
class VirtualHeadset final : public Headset {
public:
	VirtualHeadset() {
		m_thread = std::jthread([this](const std::stop_token& token) { Run(token); });
	}
	~VirtualHeadset() override = default;
	KYTY_CLASS_NO_COPY(VirtualHeadset);

	[[nodiscard]] SessionState GetSessionState() const override { return SessionState::Focused; }
	[[nodiscard]] bool         IsUserPresent() const override { return true; }
	// Unknown; the guest is told the PSVR2's.
	bool GetFieldOfView(EyeFov* /*fov*/) override { return false; }
	bool GetEyeSeparation(float* meters) override {
		*meters = 2.0f * HALF_IPD;
		return true;
	}
	void RequestRefreshRate(float hz) override {
		m_period_us = static_cast<uint32_t>(1000000.0f / hz);
		LOGF("Virtual headset: %.2f Hz\n", hz);
	}
	// Nothing is scanned out; one refresh ahead of now.
	uint64_t BindDisplayTime(int64_t /*flip_arg*/, bool /*rebind*/) override {
		return LibKernel::KernelGetProcessTime() + m_period_us.load();
	}

	bool LocateHead(uint64_t /*time*/, int64_t /*flip_arg*/, HeadLocation* location) override {
		location->device = Tracked({0.0f, HEAD_HEIGHT, 0.0f});
		location->eyes   = {Tracked({-HALF_IPD, HEAD_HEIGHT, 0.0f}).pose,
		                    Tracked({HALF_IPD, HEAD_HEIGHT, 0.0f}).pose};
		return true;
	}
	CoordinateSystem GetCoordinateSystem() override { return m_coordinate_system; }
	bool             SetCoordinateSystem(CoordinateSystem system) override {
		m_coordinate_system = system;
		return true;
	}
	// The head never moves, so it is always centred.
	bool ResetLocalCoordinate(const Pose* /*pose*/) override { return true; }
	// All coordinate systems share the floor origin.
	bool LocateCoordinateSystem(CoordinateSystem /*system*/, Pose* origin) override {
		*origin = {};
		return true;
	}
	[[nodiscard]] uint32_t GetPlayAreaUpdateCounter() const override { return 0; }
	bool                   GetPlayAreaBounds(PlayAreaBounds* bounds) override {
		*bounds = {{}, PLAY_AREA_SIZE, PLAY_AREA_SIZE};
		return true;
	}
	bool HasPlayAreaBounds() override { return true; }

	[[nodiscard]] bool HasControllerInput() const override { return false; }
	bool               GetControllerState(Hand /*hand*/, ControllerState* state) override {
		state->connected       = true;
		state->connected_count = 1;
		state->focused         = true;
		state->timestamp       = LibKernel::KernelGetProcessTime();
		return true;
	}
	bool LocateController(Hand hand, uint64_t /*time*/, Location* location) override {
		*location = Tracked({hand == Hand::Left ? -HAND_X : HAND_X, HAND_HEIGHT, HAND_Z});
		return true;
	}
	void ResetControllerOrientation(Hand /*hand*/) override {}
	void SetControllerVibration(Hand /*hand*/, float /*amplitude*/) override {}
	void SetTriggerVibration(Hand /*hand*/, const TriggerVibration& /*vibration*/) override {}

	void SetVblankHandler(std::function<void()> handler) override {
		Common::LockGuard lock(m_vblank_mutex);
		m_vblank_handler = std::move(handler);
	}

private:
	static constexpr float HEAD_HEIGHT    = 1.6f;
	static constexpr float HALF_IPD       = 0.0315f;
	static constexpr float HAND_X         = 0.2f;
	static constexpr float HAND_HEIGHT    = 1.2f;
	static constexpr float HAND_Z         = -0.3f;
	static constexpr float PLAY_AREA_SIZE = 2.0f;

	static Location Tracked(const std::array<float, 3>& position) {
		Location location;
		location.pose.position      = position;
		location.position_valid     = true;
		location.position_tracked   = true;
		location.orientation_valid  = true;
		location.orientation_tracked = true;
		return location;
	}

	// Ticks on a fixed schedule, so a slow handler does not slow the clock down.
	void Run(const std::stop_token& token) {
		KYTY_PROFILER_THREAD("Thread_VirtualHeadset");
		auto next = std::chrono::steady_clock::now();
		while (!token.stop_requested()) {
			next += std::chrono::microseconds(m_period_us.load());
			std::this_thread::sleep_until(next);
			// Catch up without bursting after a stall.
			next = std::max(next, std::chrono::steady_clock::now() -
			                          std::chrono::microseconds(m_period_us.load()));
			Common::LockGuard lock(m_vblank_mutex);
			if (m_vblank_handler) {
				m_vblank_handler();
			}
		}
	}

	std::atomic<uint32_t>         m_period_us {1000000 / 120};
	std::atomic<CoordinateSystem> m_coordinate_system {CoordinateSystem::LocalFloor};
	Common::Mutex                 m_vblank_mutex;
	std::function<void()>         m_vblank_handler;
	// Last, so it stops before the members it uses go away.
	std::jthread m_thread;
};

std::unique_ptr<Headset> CreateVirtualHeadset() {
	return std::make_unique<VirtualHeadset>();
}

} // namespace Libs::Graphics::Xr
