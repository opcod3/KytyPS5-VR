#include "graphics/presentation/xr/xrInternal.h"

#ifdef KYTY_OPENXR

#include <cmath>

namespace Libs::Graphics::Xr {

XrQuaternionf Multiply(const XrQuaternionf& a, const XrQuaternionf& b) {
	return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
	        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
	        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
	        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

XrQuaternionf Conjugate(const XrQuaternionf& q) {
	return {-q.x, -q.y, -q.z, q.w};
}

XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v) {
	const XrQuaternionf p {v.x, v.y, v.z, 0.0f};
	const auto          r = Multiply(Multiply(q, p), Conjugate(q));
	return {r.x, r.y, r.z};
}

XrPosef ComposePoses(const XrPosef& a, const XrPosef& b) {
	const auto offset = Rotate(a.orientation, b.position);
	return {Multiply(a.orientation, b.orientation),
	        {a.position.x + offset.x, a.position.y + offset.y, a.position.z + offset.z}};
}

XrPosef InversePose(const XrPosef& pose) {
	const auto orientation = Conjugate(pose.orientation);
	const auto position    = Rotate(orientation, pose.position);
	return {orientation, {-position.x, -position.y, -position.z}};
}

XrVector3f TransformPoint(const XrPosef& pose, const XrVector3f& point) {
	const auto rotated = Rotate(pose.orientation, point);
	return {pose.position.x + rotated.x, pose.position.y + rotated.y, pose.position.z + rotated.z};
}

XrQuaternionf YawOnly(const XrQuaternionf& orientation) {
	const auto  forward = Rotate(orientation, {0.0f, 0.0f, -1.0f});
	const float yaw     = std::atan2(-forward.x, -forward.z);
	return {0.0f, std::sin(yaw / 2.0f), 0.0f, std::cos(yaw / 2.0f)};
}

XrPosef ToXrPose(const Pose& pose) {
	XrQuaternionf q {pose.orientation[0], pose.orientation[1], pose.orientation[2],
	                 pose.orientation[3]};
	const float   length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	if (length > 0.0f) {
		q = {q.x / length, q.y / length, q.z / length, q.w / length};
	} else {
		q = {0.0f, 0.0f, 0.0f, 1.0f};
	}
	return {q, {pose.position[0], pose.position[1], pose.position[2]}};
}

Pose FromXrPose(const XrPosef& pose) {
	return {{pose.position.x, pose.position.y, pose.position.z},
	        {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w}};
}

} // namespace Libs::Graphics::Xr

#endif // KYTY_OPENXR
