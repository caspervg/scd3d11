// From sc4-modern-camera (https://github.com/caspervg/sc4-modern-camera), the mouse-driven camera that
// SCD3D11 runs with FreeCamera=on (see FreeCamera.cpp).
#include "Camera.h"
#include "SC4CameraControlLayout.hpp"
#include "cISC4View3DWin.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

// The rotation and zoom math is ported from SC4-ModernCamera by UncleUncleRj
// (https://github.com/UncleUncleRj/SC4-ModernCamera, LGPL-3.0).

namespace
{
	constexpr float kPi = SC4CameraControl::kPi;

	// The game's camera looks wrong at very low and very high pitch.
	constexpr float kMinPitch = 0.261f;
	constexpr float kMaxPitch = 1.483f;

	// Within a zoom level, magnification stays between these. Outside them the
	// next zoom level is used, so the game keeps picking the right detail level.
	constexpr float kMinMagnification = 0.5f;
	constexpr float kMaxMagnification = 2.0f;

	// The game only draws correctly for yaw near its own -22.5 degree angle.
	// Past +30 degrees from it, building sides start to disappear, so yaw is moved
	// back by quarter turns and the game's discrete rotation makes up for it.
	// The window is wider than 90 degrees so the handoff does not flip back and forth.
	constexpr float kNativeYaw = -kPi * 0.125f;
	constexpr float kMaxYawOffset = 30.0f * kPi / 180.0f;
	constexpr float kMinYawOffset = -70.0f * kPi / 180.0f;

	// SetZoomLimits stores the limits here. cISC4View3DWin has no getter for them.
	constexpr size_t kZoomLowerLimitOffset = 0x364;
	constexpr size_t kZoomUpperLimitOffset = 0x368;

	void RebalanceYaw(float& yaw, int32_t& rotation)
	{
		constexpr float kQuarterTurn = kPi * 0.5f;
		int32_t quarterTurns = 0;

		while (yaw - kNativeYaw > kMaxYawOffset)
		{
			yaw -= kQuarterTurn;
			++quarterTurns;
		}

		while (yaw - kNativeYaw < kMinYawOffset)
		{
			yaw += kQuarterTurn;
			--quarterTurns;
		}

		rotation = SC4CameraControl::WrapRotation(rotation + quarterTurns);
	}

	int32_t& ZoomLimitField(cISC4View3DWin* view3D, size_t offset)
	{
		return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(view3D) + offset);
	}
}

bool Camera::Enable()
{
	if (enabled)
	{
		return true;
	}

	cISC4View3DWin* view3D = SC4UI::GetView3DWin();
	SC4CameraControlLayout* cameraControl = SC4CameraControl::GetActiveCameraControl();

	if (!view3D || !cameraControl)
	{
		return false;
	}

	yaw = SC4CameraControl::SanitizeYawRadians(cameraControl->yaw);
	pitch = std::clamp(cameraControl->pitch, kMinPitch, kMaxPitch);

	savedZoomLower = ZoomLimitField(view3D, kZoomLowerLimitOffset);
	savedZoomUpper = ZoomLimitField(view3D, kZoomUpperLimitOffset);
	PinZoomLimits(cameraControl->zoom);

	enabled = true;
	RecentreIfOffMap();
	return true;
}

void Camera::Disable()
{
	if (!enabled)
	{
		return;
	}

	enabled = false;
	preparedForSave = false;
	classicHold = false;
	rotating = false;
	pendingYaw = pendingPitch = zoomRemaining = frameZoom = 0.0f;

	if (cISC4View3DWin* view3D = SC4UI::GetView3DWin())
	{
		view3D->SetZoomLimits(savedZoomLower, savedZoomUpper);
	}

	// The angle tables are global, so they must be put back even if the city is closing.
	SC4CameraControl::ApplyYawOverride(SC4CameraControl::kDefaultYawRadians);
	SC4CameraControl::ApplyDefaultPitchOverrides();

	if (SC4CameraControlLayout* cameraControl = SC4CameraControl::GetActiveCameraControl())
	{
		cameraControl->yaw = SC4CameraControl::kDefaultYawRadians;
		cameraControl->pitch = SC4CameraControl::GetDefaultPitchForZoom(cameraControl->zoom);
		SC4CameraControl::GetSetCustomMagnification()(cameraControl, 1.0f);
		SC4CameraControl::Refresh(*cameraControl);
	}
}

void Camera::BeginRotation()
{
	SC4CameraControlLayout* cameraControl = SC4CameraControl::GetActiveCameraControl();

	if (!enabled || !cameraControl)
	{
		return;
	}

	ResumeAfterSave();
	anchorViewTarget = cameraControl->viewTargetPosition;
	anchorBaseTarget = cameraControl->baseTargetForRotation;
	anchorOrthoScale = cameraControl->orthoScale;
	rotating = true;
	deferring = deferDrags && !classicHold;
	committing = false;
	deferredYaw = deferredPitch = 0.0f;
}

void Camera::EndRotation()
{
	if (deferring && rotating)
	{
		pendingYaw += deferredYaw;
		pendingPitch += deferredPitch;
		deferredYaw = deferredPitch = 0.0f;
		deferring = false;
		committing = true;
		return;
	}

	rotating = false;
	committing = false;
	anchorOrthoScale = 0.0f;
}

bool Camera::DeferredView(float& yawDelta, float& endPitch) const
{
	if (!deferring || !rotating || (deferredYaw == 0.0f && deferredPitch == 0.0f))
	{
		return false;
	}

	yawDelta = deferredYaw;
	endPitch = std::clamp(pitch + deferredPitch, kMinPitch, kMaxPitch);
	return true;
}

void Camera::PrepareForSave()
{
	SC4CameraControlLayout* cameraControl = SC4CameraControl::GetActiveCameraControl();

	if (!enabled || preparedForSave || !cameraControl)
	{
		return;
	}

	EndRotation();
	SC4CameraControl::ApplyYawOverride(SC4CameraControl::kDefaultYawRadians);
	SC4CameraControl::ApplyDefaultPitchOverrides();
	cameraControl->yaw = SC4CameraControl::kDefaultYawRadians;
	cameraControl->pitch = SC4CameraControl::GetDefaultPitchForZoom(cameraControl->zoom);
	cameraControl->viewTargetVelocity = cS3DVector3();
	SC4CameraControl::Refresh(*cameraControl);

	if (cISC43DRender* renderer = SC4CameraControl::GetActiveRenderer())
	{
		renderer->ForceFullRedraw();
	}

	preparedForSave = true;
}

void Camera::ResumeAfterSave()
{
	if (!preparedForSave)
	{
		return;
	}

	preparedForSave = false;

	if (SC4CameraControlLayout* cameraControl = SC4CameraControl::GetActiveCameraControl())
	{
		RefreshWithAngles(*cameraControl);
	}
}

void Camera::Rotate(float yawDelta, float pitchDelta)
{
	ResumeAfterSave();

	if (classicHold)
	{
		return;
	}

	pendingYaw += yawDelta;
	pendingPitch += pitchDelta;
}

void Camera::Zoom(float wheelNotches)
{
	ResumeAfterSave();
	zoomRemaining += wheelNotches * std::log2(magnificationPerNotch);
}

void Camera::SetClassicHold(bool held)
{
	if (held == classicHold || !enabled || preparedForSave)
	{
		return;
	}

	held ? BeginClassicHold() : EndClassicHold();
}

void Camera::BeginClassicHold()
{
	cISC43DRender* renderer = SC4CameraControl::GetActiveRenderer();
	SC4CameraControlLayout* cameraControl = SC4CameraControl::GetActiveCameraControl();

	if (!renderer || !cameraControl)
	{
		return;
	}

	EndRotation();
	pendingYaw = pendingPitch = 0.0f;
	heldYaw = yaw;
	heldPitch = pitch;
	heldRotation = cameraControl->rotation;

	// The nearest classic angle: the quarter turns between the free yaw and the game's
	// own yaw go into the discrete rotation.
	const auto quarterTurns = static_cast<int32_t>(std::lround((yaw - kNativeYaw) / (kPi * 0.5f)));
	snappedRotation = SC4CameraControl::WrapRotation(heldRotation + quarterTurns);

	if (!SetRotationKeepingTarget(renderer, cameraControl, snappedRotation))
	{
		return;
	}

	classicHold = true;
	RefreshWithAngles(*cameraControl);
	renderer->ForceFullRedraw();
}

void Camera::EndClassicHold()
{
	classicHold = false;

	cISC43DRender* renderer = SC4CameraControl::GetActiveRenderer();
	SC4CameraControlLayout* cameraControl = SC4CameraControl::GetActiveCameraControl();

	if (!renderer || !cameraControl)
	{
		return;
	}

	// Keep any rotation the player made with the rotate buttons while holding.
	const int32_t rotation = SC4CameraControl::WrapRotation(heldRotation + cameraControl->rotation - snappedRotation);
	yaw = heldYaw;
	pitch = heldPitch;

	if (SetRotationKeepingTarget(renderer, cameraControl, rotation))
	{
		RefreshWithAngles(*cameraControl);
		renderer->ForceFullRedraw();
	}
}

bool Camera::SetRotationKeepingTarget(cISC43DRender* renderer, SC4CameraControlLayout*& cameraControl, int32_t rotation)
{
	if (rotation == cameraControl->rotation)
	{
		return true;
	}

	const cS3DVector3 viewTarget = cameraControl->viewTargetPosition;
	const cS3DVector3 baseTarget = cameraControl->baseTargetForRotation;

	if (!renderer->SetRotation(rotation) || !(cameraControl = SC4CameraControl::GetActiveCameraControl()))
	{
		return false;
	}

	cameraControl->viewTargetPosition = viewTarget;
	cameraControl->baseTargetForRotation = baseTarget;
	cameraControl->viewTargetVelocity = cS3DVector3();
	return true;
}

// The game's own scroll bounds keep the camera near the map while playing. This only catches
// a view target that is already off the city when the modern camera takes over.
void Camera::RecentreIfOffMap()
{
	SC4CameraControlLayout* cameraControl = SC4CameraControl::GetActiveCameraControl();

	if (!cameraControl || rotating || !(cameraControl->citySizeX > 1.0f) || !(cameraControl->citySizeZ > 1.0f))
	{
		return;
	}

	const cS3DVector3& target = cameraControl->viewTargetPosition;

	if (!std::isfinite(target.fX) || !std::isfinite(target.fZ))
	{
		return;
	}

	const bool inside = target.fX >= 0.0f && target.fX <= cameraControl->citySizeX
		&& target.fZ >= 0.0f && target.fZ <= cameraControl->citySizeZ;

	if (inside)
	{
		return;
	}

	const float dx = cameraControl->citySizeX * 0.5f - target.fX;
	const float dz = cameraControl->citySizeZ * 0.5f - target.fZ;

	for (cS3DVector3* position : { &cameraControl->cameraPlaneOrigin, &cameraControl->baseTargetForRotation,
		&cameraControl->cameraPosition, &cameraControl->viewTargetPosition, &cameraControl->cameraOffset })
	{
		position->fX += dx;
		position->fZ += dz;
	}

	cameraControl->viewTargetVelocity = cS3DVector3();
	RefreshWithAngles(*cameraControl);
}

void Camera::Update()
{
	frameZoom = TakeZoomStep();

	if (!enabled)
	{
		return;
	}

	if (deferring && rotating)
	{
		deferredYaw += pendingYaw;
		deferredPitch = std::clamp(pitch + deferredPitch + pendingPitch, kMinPitch, kMaxPitch) - pitch;
		pendingYaw = pendingPitch = 0.0f;
	}

	if (pendingYaw != 0.0f || pendingPitch != 0.0f || frameZoom != 0.0f)
	{
		if (cISC43DRender* renderer = SC4CameraControl::GetActiveRenderer())
		{
			Apply(renderer);
		}

		pendingYaw = pendingPitch = frameZoom = 0.0f;
	}

	if (committing)
	{
		EndRotation();
	}
}

// Moves a fraction of the remaining zoom each frame, so the view eases into the
// zoom the wheel asked for instead of jumping a step per notch.
float Camera::TakeZoomStep()
{
	const auto now = std::chrono::steady_clock::now();
	const float seconds = std::clamp(std::chrono::duration<float>(now - lastUpdate).count(), 0.0f, 0.1f);
	lastUpdate = now;

	if (zoomRemaining == 0.0f)
	{
		return 0.0f;
	}

	float step = zoomRemaining;

	if (zoomSmoothing > 0.0f)
	{
		step = zoomRemaining * (1.0f - std::exp(-zoomSmoothing * seconds));

		// Finish the tail instead of creeping towards it forever.
		if (std::abs(zoomRemaining - step) < 0.0005f)
		{
			step = zoomRemaining;
		}
	}

	zoomRemaining -= step;
	return step;
}

bool Camera::Apply(cISC43DRender* renderer)
{
	SC4CameraControlLayout* cameraControl = SC4CameraControl::GetActiveCameraControl();

	if (!cameraControl)
	{
		return false;
	}

	const bool zoomed = frameZoom != 0.0f;
	const bool rotated = pendingYaw != 0.0f || pendingPitch != 0.0f;

	if (zoomed && !ApplyZoom(renderer, cameraControl))
	{
		return false;
	}

	if (rotated && !ApplyRotation(renderer, cameraControl))
	{
		return false;
	}

	if (!RefreshWithAngles(*cameraControl))
	{
		return false;
	}

	if (zoomed && rotating)
	{
		// The player zoomed during a drag, that is the zoom to keep from now on.
		anchorOrthoScale = cameraControl->orthoScale;
	}
	else if (rotated)
	{
		CorrectRotationFit(cameraControl);
	}

	return true;
}

bool Camera::ApplyZoom(cISC43DRender* renderer, SC4CameraControlLayout*& cameraControl)
{
	const int32_t minZoom = static_cast<int32_t>(std::floor(cameraControl->minZoom));
	const int32_t maxZoom = static_cast<int32_t>(std::floor(cameraControl->maxZoomLevelWithoutCustom));

	// Treat zoom level plus log2(magnification) as one continuous zoom value.
	const float currentZoom = static_cast<float>(cameraControl->zoom)
		+ std::log2(std::max(cameraControl->customMagnification, 0.001f));
	const float continuousZoom = std::clamp(
		currentZoom + frameZoom,
		static_cast<float>(minZoom) + std::log2(kMinMagnification),
		static_cast<float>(maxZoom) + std::log2(kMaxMagnification));

	if (continuousZoom != currentZoom + frameZoom)
	{
		// Hit the zoom limit, forget the rest of the scrolling in that direction.
		zoomRemaining = 0.0f;
	}

	const int32_t targetZoom = std::clamp(static_cast<int32_t>(std::lround(continuousZoom)), minZoom, maxZoom);
	const float targetMagnification = std::pow(2.0f, continuousZoom - static_cast<float>(targetZoom));

	if (targetZoom != cameraControl->zoom)
	{
		if (!renderer->SetZoom(targetZoom))
		{
			return false;
		}

		PinZoomLimits(targetZoom);
		cameraControl = SC4CameraControl::GetActiveCameraControl();

		if (!cameraControl)
		{
			return false;
		}
	}

	return SC4CameraControl::GetSetCustomMagnification()(cameraControl, targetMagnification);
}

bool Camera::ApplyRotation(cISC43DRender* renderer, SC4CameraControlLayout*& cameraControl)
{
	RestoreRotationAnchor(*cameraControl);

	pitch = std::clamp(pitch + pendingPitch, kMinPitch, kMaxPitch);
	yaw = SC4CameraControl::SanitizeYawRadians(yaw + pendingYaw);

	int32_t rotation = cameraControl->rotation;
	RebalanceYaw(yaw, rotation);
	yaw = SC4CameraControl::SanitizeYawRadians(yaw);

	if (rotation != cameraControl->rotation)
	{
		if (!renderer->SetRotation(rotation))
		{
			return false;
		}

		cameraControl = SC4CameraControl::GetActiveCameraControl();

		if (!cameraControl)
		{
			return false;
		}

		RestoreRotationAnchor(*cameraControl);
	}

	return true;
}

// The game widens orthoScale while a square city turns toward a diamond, so its
// corners stay in view. Undo that with magnification, so the zoom the player had
// when the drag started stays the same.
void Camera::CorrectRotationFit(SC4CameraControlLayout*& cameraControl)
{
	if (!rotating || anchorOrthoScale <= 0.0f || cameraControl->orthoScale <= 0.0f
		|| cameraControl->customMagnification <= 0.0f)
	{
		return;
	}

	const float correction = cameraControl->orthoScale / anchorOrthoScale;

	if (std::abs(correction - 1.0f) <= 0.0005f)
	{
		return;
	}

	const float magnification = std::clamp(cameraControl->customMagnification * correction, 0.001f, 10.0f);

	if (SC4CameraControl::GetSetCustomMagnification()(cameraControl, magnification))
	{
		if ((cameraControl = SC4CameraControl::GetActiveCameraControl()) != nullptr)
		{
			RefreshWithAngles(*cameraControl);
		}
	}
}

void Camera::RestoreRotationAnchor(SC4CameraControlLayout& cameraControl) const
{
	if (!rotating)
	{
		return;
	}

	cameraControl.viewTargetPosition = anchorViewTarget;
	cameraControl.baseTargetForRotation = anchorBaseTarget;
	cameraControl.viewTargetVelocity = cS3DVector3();
}

// Zoom and rotation changes make the game reload its per-level angle tables, so the
// player's angles go into those tables as well as into the camera control itself.
bool Camera::RefreshWithAngles(SC4CameraControlLayout& cameraControl)
{
	// While ALT is held the classic angles are shown, the pitch following the zoom level.
	const float shownYaw = classicHold ? kNativeYaw : yaw;
	const float shownPitch = classicHold ? SC4CameraControl::GetDefaultPitchForZoom(cameraControl.zoom) : pitch;

	SC4CameraControl::ApplyYawOverride(shownYaw);
	SC4CameraControl::ApplyPitchOverride(shownPitch);
	cameraControl.yaw = shownYaw;
	cameraControl.pitch = shownPitch;

	RestoreRotationAnchor(cameraControl);
	const bool result = SC4CameraControl::Refresh(cameraControl);
	RestoreRotationAnchor(cameraControl);
	return result;
}

void Camera::PinZoomLimits(int32_t zoom)
{
	if (cISC4View3DWin* view3D = SC4UI::GetView3DWin())
	{
		view3D->SetZoomLimits(zoom, zoom);
	}
}
