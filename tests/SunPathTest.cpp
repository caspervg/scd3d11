#include "SunPath.h"

#include <cassert>
#include <cmath>

using nSCD3D11::SunPath::Direction;

class cRZCOMDllDirector;
// SunPath reaches SC4 through the COM director, which the tests never start.
cRZCOMDllDirector *RZGetCOMDllDirector() { return nullptr; }

int main() {
	float direction[3]{};
	float elevation = 0.0f;

	// Midsummer noon at 50 degrees north: the sun stands about 63 degrees up
	// in the south, so light travels north (-z) and down.
	Direction(50.0f, 12.0f, direction, elevation);
	assert(elevation > 63.0f && elevation < 64.0f);
	assert(std::fabs(direction[0]) < 0.02f && direction[2] < -0.4f && direction[1] < -0.85f);
	assert(std::fabs(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2] - 1.0f) <
	       1.0e-4f);

	// Mid-morning the sun is in the east, so light travels west (-x).
	Direction(50.0f, 9.0f, direction, elevation);
	assert(direction[0] < -0.2f);

	// The sun is still up at 20:00, as SC4's day is.
	Direction(50.0f, 19.8f, direction, elevation);
	assert(elevation > 0.0f);

	// South of the equator the noon sun is in the north.
	Direction(-35.0f, 12.0f, direction, elevation);
	assert(direction[2] > 0.15f);

	// Midnight: below the horizon, and the direction is held at the lowest
	// elevation.
	Direction(50.0f, 0.0f, direction, elevation);
	assert(elevation < 0.0f);
	float const lowest = std::sin(nSCD3D11::SunPath::kMinElevation * 3.14159265f / 180.0f);
	assert(std::fabs(-direction[1] - lowest) < 1.0e-4f);

	// The midsummer full moon at midnight, 51 degrees north: low in the south.
	nSCD3D11::SunPath::MoonDirection(51.0f, 0.0f, direction, elevation);
	assert(elevation > 15.0f && elevation < 16.0f);
	assert(direction[2] < -0.9f && std::fabs(direction[0]) < 0.02f);
	// At noon it is far below the horizon.
	nSCD3D11::SunPath::MoonDirection(51.0f, 12.0f, direction, elevation);
	assert(elevation < -60.0f);

	// Noon on the tropic is overhead, held at the highest.
	Direction(23.44f, 12.0f, direction, elevation);
	assert(elevation > 89.0f);
	float const highest = std::sin(nSCD3D11::SunPath::kMaxElevation * 3.14159265f / 180.0f);
	assert(std::fabs(-direction[1] - highest) < 1.0e-4f);
	return 0;
}
