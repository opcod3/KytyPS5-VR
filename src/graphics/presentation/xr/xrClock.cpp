#include "graphics/presentation/xr/xrInternal.h"

#ifdef KYTY_OPENXR

#include "common/assert.h"
#include "kernel/pthread.h"

#include <limits>

namespace Libs::Graphics::Xr {

// Guest time runs on the TSC and XrTime on CLOCK_MONOTONIC. Their offset is measured from
// back-to-back samples, keeping the pair read fastest, and remeasured every second so the two
// clocks cannot drift apart.
void Runtime::CalibrateClock() {
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	uint64_t best_rtt   = std::numeric_limits<uint64_t>::max();
	uint64_t best_guest = 0;
	timespec best_host {};
	for (int i = 0; i < 8; i++) {
		const uint64_t before = LibKernel::KernelGetProcessTime();
		timespec       host {};
		clock_gettime(CLOCK_MONOTONIC, &host);
		const uint64_t after = LibKernel::KernelGetProcessTime();
		if (after - before < best_rtt) {
			best_rtt   = after - before;
			best_guest = before + (after - before) / 2;
			best_host  = host;
		}
	}
	XrTime host_time = 0;
	if (Check(m_convert_timespec(m_instance, &best_host, &host_time),
	          "xrConvertTimespecTimeToTimeKHR")) {
		m_clock_offset = host_time - static_cast<int64_t>(best_guest) * 1000;
	}
#endif
}

XrTime Runtime::Now() const {
	return ToXrTime(LibKernel::KernelGetProcessTime());
}

XrTime Runtime::ToXrTime(uint64_t guest_time) const {
	return static_cast<XrTime>(guest_time) * 1000 + m_clock_offset.load();
}

uint64_t Runtime::ToGuestTime(XrTime time) const {
	const XrTime guest = (time - m_clock_offset.load()) / 1000;
	return guest > 0 ? static_cast<uint64_t>(guest) : 0;
}

void Runtime::OnFrameWaited(const XrFrameState& state) {
	Common::LockGuard lock(m_display_mutex);
	m_last_display_time = state.predictedDisplayTime;
	m_display_period    = state.predictedDisplayPeriod;
}

uint64_t Runtime::BindDisplayTime(int64_t flip_arg, bool rebind) {
	Common::LockGuard lock(m_display_mutex);
	DisplaySlot*      slot = nullptr;
	for (auto& s: m_display_slots) {
		if (s.used && s.flip_arg == flip_arg) {
			slot = &s;
		}
	}
	if (slot != nullptr && !rebind) {
		return ToGuestTime(slot->time);
	}

	// A frame starting now is shown at the first refresh at least one period away, which leaves
	// the guest one period to render it.
	const XrTime target = Now() + (m_display_period > 0 ? m_display_period : 1000000000 / 90);
	XrTime       time   = target;
	if (m_display_period > 0 && m_last_display_time > 0) {
		time = m_last_display_time;
		if (time < target) {
			const auto periods = (target - time + m_display_period - 1) / m_display_period;
			time += periods * m_display_period;
		}
	}

	if (slot == nullptr) {
		slot                = &m_display_slots[m_display_slot_next];
		m_display_slot_next = (m_display_slot_next + 1) % m_display_slots.size();
	}
	*slot = {flip_arg, time, true};
	return ToGuestTime(time);
}

void Runtime::UpdateViews(const std::array<XrView, 2>& views) {
	Common::LockGuard lock(m_views_mutex);
	m_eye_views       = views;
	m_eye_views_known = true;
}

bool Runtime::GetViews(std::array<XrView, 2>* views) {
	EXIT_IF(views == nullptr);
	Common::LockGuard lock(m_views_mutex);
	if (!m_eye_views_known) {
		return false;
	}
	*views = m_eye_views;
	return true;
}

} // namespace Libs::Graphics::Xr

#endif // KYTY_OPENXR
