// From sc4-modern-camera (https://github.com/caspervg/sc4-modern-camera), the mouse-driven camera that
// SCD3D11 runs with FreeCamera=on (see FreeCamera.cpp).
#pragma once
#include "cSC4BaseViewInputControlEx.h"

class Camera;

// Added as a persistent view input control, so it keeps working whatever tool
// the player has picked. See cISC4ViewInputControl for the event order.
class ModernCameraControl final : public cSC4BaseViewInputControlEx
{
public:
	explicit ModernCameraControl(Camera& camera);

	bool Shutdown() override;
	void Deactivate() override;

	bool OnKeyDown(int32_t vkCode, uint32_t modifiers) override;
	bool OnMouseDownL(int32_t x, int32_t z, uint32_t modifiers) override;
	bool OnMouseDownR(int32_t x, int32_t z, uint32_t modifiers) override;
	bool OnMouseDownM(int32_t x, int32_t z, uint32_t modifiers) override;
	bool OnMouseUpM(int32_t x, int32_t z, uint32_t modifiers) override;
	bool OnMouseMove(int32_t x, int32_t z, uint32_t modifiers) override;
	bool OnMouseWheel(int32_t x, int32_t z, uint32_t modifiers, int32_t wheelDelta) override;

protected:
	bool ShouldReceiveExtraButtons() override;

private:
	void StopDragging();

	Camera& camera;
	int32_t lastX = 0;
	int32_t lastZ = 0;
};
