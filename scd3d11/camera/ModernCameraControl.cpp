// From sc4-modern-camera (https://github.com/caspervg/sc4-modern-camera), the mouse-driven camera that
// SCD3D11 runs with FreeCamera=on (see FreeCamera.cpp).
#include "ModernCameraControl.h"
#include "Camera.h"
#include <algorithm>

namespace
{
	constexpr uint32_t kControlID = 0x5F1C2A07;

	// Radians of yaw or pitch per pixel of mouse movement.
	constexpr float kRotationPerPixel = 0.005f;

	// The game turns a WM_MOUSEWHEEL delta into delta * 100 / 120 times the Windows
	// "lines to scroll" setting (at most 10), so undo both to get notches.
	float WheelDeltaPerNotch()
	{
		UINT lines = 1;

		if (SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0))
		{
			lines = std::clamp(lines, 1u, 10u);
		}

		return 100.0f * static_cast<float>(lines);
	}
}

ModernCameraControl::ModernCameraControl(Camera& camera)
	: cSC4BaseViewInputControlEx(kControlID),
	  camera(camera)
{
}

bool ModernCameraControl::Shutdown()
{
	StopDragging();
	return cSC4BaseViewInputControlEx::Shutdown();
}

void ModernCameraControl::Deactivate()
{
	StopDragging();
	cSC4BaseViewInputControlEx::Deactivate();
}

// After a save the camera shows the native angles until the player does something,
// see Camera::PrepareForSave. These never handle the event.
bool ModernCameraControl::OnKeyDown(int32_t, uint32_t)
{
	camera.ResumeAfterSave();
	return false;
}

bool ModernCameraControl::OnMouseDownL(int32_t, int32_t, uint32_t)
{
	camera.ResumeAfterSave();
	return false;
}

bool ModernCameraControl::OnMouseDownR(int32_t, int32_t, uint32_t)
{
	camera.ResumeAfterSave();
	return false;
}

bool ModernCameraControl::OnMouseDownM(int32_t x, int32_t z, uint32_t)
{
	if (!camera.IsEnabled())
	{
		return false;
	}

	lastX = x;
	lastZ = z;
	camera.BeginRotation();

	// Keep getting mouse moves when the cursor leaves the view during the drag.
	SetCapture();
	return true;
}

bool ModernCameraControl::OnMouseUpM(int32_t, int32_t, uint32_t)
{
	StopDragging();
	return true;
}

bool ModernCameraControl::OnMouseMove(int32_t x, int32_t z, uint32_t)
{
	if (IsMiddleMouseDown() && camera.IsEnabled())
	{
		camera.Rotate(static_cast<float>(x - lastX) * kRotationPerPixel, static_cast<float>(z - lastZ) * kRotationPerPixel);
	}

	lastX = x;
	lastZ = z;

	// Persistent controls cannot stop mouse moves anyway, let the active tool have them.
	return false;
}

bool ModernCameraControl::OnMouseWheel(int32_t, int32_t, uint32_t, int32_t wheelDelta)
{
	if (camera.IsEnabled())
	{
		// The game still calls its own ZoomIn/ZoomOut after this, the pinned zoom
		// limits are what stop them.
		camera.Zoom(static_cast<float>(wheelDelta) / WheelDeltaPerNotch());
	}

	return false;
}

bool ModernCameraControl::ShouldReceiveExtraButtons()
{
	return true;
}

void ModernCameraControl::StopDragging()
{
	if (AmCapturing())
	{
		ReleaseCapture();
	}

	camera.EndRotation();
}
