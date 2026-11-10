#include "../scd3d11/FrameLimiter.h"

#include <cassert>
#include <windows.h>

using namespace nSCD3D11;

int main() {
	// MaxFPS=screen: VSync holds the screen's rate itself, so only VSync=off gets a limit.
	assert(FrameLimit(true, 0, true, 144) == 0);
	assert(FrameLimit(true, 0, false, 144) == 144);
	// An unknown refresh rate gives no limit.
	assert(FrameLimit(true, 0, false, 0) == 0);
	// MaxFPS=<n> holds with and without VSync, and MaxFPS=off never limits.
	assert(FrameLimit(false, 60, true, 144) == 60);
	assert(FrameLimit(false, 60, false, 144) == 60);
	assert(FrameLimit(false, 0, false, 144) == 0);

	LARGE_INTEGER frequency{}, start{}, end{};
	QueryPerformanceFrequency(&frequency);
	FrameLimiter limiter;
	limiter.Wait(200);
	QueryPerformanceCounter(&start);
	for (int frame = 0; frame < 40; ++frame) limiter.Wait(200);
	QueryPerformanceCounter(&end);
	double const seconds = static_cast<double>(end.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);
	// 40 frames at 200 a second take 0.2 s. A busy test machine can only make it longer.
	assert(seconds > 0.19);
	assert(seconds < 1.0);

	// No limit does not wait.
	QueryPerformanceCounter(&start);
	for (int frame = 0; frame < 1000; ++frame) limiter.Wait(0);
	QueryPerformanceCounter(&end);
	assert(end.QuadPart - start.QuadPart < frequency.QuadPart / 10);
	return 0;
}
