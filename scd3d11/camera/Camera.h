// From sc4-modern-camera (https://github.com/caspervg/sc4-modern-camera), the mouse-driven camera that
// SCD3D11 runs with FreeCamera=on (see FreeCamera.cpp).
#pragma once
#include "cS3DVector3.h"
#include <chrono>
#include <cstdint>

class cISC43DRender;
struct SC4CameraControlLayout;

// Free yaw/pitch and continuous zoom on top of the game's camera control.
//
// Input only queues changes. Update applies them once per frame, because every
// change ends in a native camera refresh and mouse moves arrive far more often.
class Camera
{
public:
	// Takes over from the native camera. Pins the city view's zoom limits to the
	// current level so the game's own wheel, key and button zoom stop working.
	bool Enable();

	// Hands the camera back: restores the zoom limits, the native angle tables
	// and a magnification of 1.
	void Disable();

	bool IsEnabled() const { return enabled; }

	// Freezes the view target while the player drags, so rotating does not drift.
	void BeginRotation();
	void EndRotation();

	void Rotate(float yawDelta, float pitchDelta);
	void Zoom(float wheelNotches);

	// The game renders the region view thumbnail with the global angle tables, so
	// before a save they go back to the native angles. There is no message after the
	// save, the free angles come back with the next click, wheel or key press.
	void PrepareForSave();
	void ResumeAfterSave();

	// While held (ALT), the camera shows the nearest classic angle: the game's own yaw and
	// the native pitch for the zoom level. Letting go brings the free angles back.
	void SetClassicHold(bool held);

	// Called once per frame.
	void Update();

	// While set when a drag starts, the drag turns and tilts only the view DeferredView gives, and the game's
	// camera takes it in one step when the drag ends. SCD3D11 sets it while its server draws a view of its own,
	// so the game does not redraw the city for every mouse move.
	bool deferDrags = false;

	// The turn of a deferred drag so far, in radians, and the pitch it ends at. False without one.
	bool DeferredView(float& yawDelta, float& endPitch) const;

	// How much one wheel notch scales the view.
	float magnificationPerNotch = 1.04f;

	// How fast the view glides to the zoom the wheel asked for, per second.
	// 0 applies each notch at once.
	float zoomSmoothing = 12.0f;

private:
	void BeginClassicHold();
	void EndClassicHold();
	bool SetRotationKeepingTarget(cISC43DRender* renderer, SC4CameraControlLayout*& cameraControl, int32_t rotation);
	void RecentreIfOffMap();
	float TakeZoomStep();
	bool Apply(cISC43DRender* renderer);
	bool ApplyZoom(cISC43DRender* renderer, SC4CameraControlLayout*& cameraControl);
	bool ApplyRotation(cISC43DRender* renderer, SC4CameraControlLayout*& cameraControl);
	void CorrectRotationFit(SC4CameraControlLayout*& cameraControl);
	void RestoreRotationAnchor(SC4CameraControlLayout& cameraControl) const;
	bool RefreshWithAngles(SC4CameraControlLayout& cameraControl);
	void PinZoomLimits(int32_t zoom);

	bool enabled = false;
	bool preparedForSave = false;
	float yaw = 0.0f;
	float pitch = 0.0f;

	float pendingYaw = 0.0f;
	float pendingPitch = 0.0f;
	// Zoom is tracked as log2 of the magnification: what the wheel asked for but
	// has not been applied yet, and the part to apply this frame.
	float zoomRemaining = 0.0f;
	float frameZoom = 0.0f;
	std::chrono::steady_clock::time_point lastUpdate{};

	bool rotating = false;
	bool deferring = false;
	// The deferred turn waits for the next Update, which applies it while the view target is still anchored.
	bool committing = false;
	float deferredYaw = 0.0f;
	float deferredPitch = 0.0f;
	cS3DVector3 anchorViewTarget{};
	cS3DVector3 anchorBaseTarget{};
	float anchorOrthoScale = 0.0f;

	int32_t savedZoomLower = 0;
	int32_t savedZoomUpper = 0;

	bool classicHold = false;
	float heldYaw = 0.0f;
	float heldPitch = 0.0f;
	int32_t heldRotation = 0;
	int32_t snappedRotation = 0;
};
