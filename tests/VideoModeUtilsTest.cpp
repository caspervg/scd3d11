#include "VideoModeUtils.h"

#include <cassert>
#include <vector>

int main() {
	std::vector<sGDMode> modes;
	assert(nSCD3D11::AppendVideoMode(modes, 1920, 1080, 32, false, true, true));
	assert(!nSCD3D11::AppendVideoMode(modes, 1920, 1080, 32, false, true, true));
	assert(nSCD3D11::AppendVideoMode(modes, 1920, 1080, 32, true, true, true));
	assert(nSCD3D11::AppendVideoMode(modes, 3200, 1800, 32, false, false, false));
	assert(modes.size() == 3);
	assert(modes[0].index == 0 && modes[1].index == 1 && modes[2].index == 2);
	assert(modes[2].width == 3200 && modes[2].height == 1800 && !modes[2].isFullscreen);
	assert(modes[2].isInitialized && modes[2].textureStageCount == 2 && modes[2].redColorMask == 0x00ff0000);
	assert(!modes[2].supportsStencilBuffer && !modes[2].supportsDxtTextures && !modes[2].supportsFogCoord);
	return 0;
}
