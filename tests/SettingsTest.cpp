#include "Settings.h"

#include <cassert>
#include <optional>
#include <string>

using nSCD3D11::Settings::ParseAnisotropy;
using nSCD3D11::Settings::ParseFlag;
using nSCD3D11::Settings::ParseLatitude;

int main() {
	bool flag = false;
	// A bare switch is on. The rest is sc4-dll-utilities' TryParse<bool>, in any case.
	for (char const *on : {"", "on", "ON", "true", "Enabled", "1"}) assert(ParseFlag(on, flag) && flag);
	for (char const *off : {"off", "false", "DISABLED", "0"}) assert(ParseFlag(off, flag) && !flag);
	for (char const *neither : {"yes", "maybe", "2", " on"}) assert(!ParseFlag(neither, flag));
	uint32_t anisotropy = 0;
	assert(ParseAnisotropy("16", anisotropy) && anisotropy == 16);
	assert(ParseAnisotropy("4", anisotropy) && anisotropy == 4);
	assert(ParseAnisotropy("off", anisotropy) && anisotropy == 1);
	for (char const *invalid : {"", "on", "3", "32", "1"}) assert(!ParseAnisotropy(invalid, anisotropy));

	std::optional<float> latitude;
	assert(ParseLatitude("51.2", latitude) && latitude && *latitude > 51.1f && *latitude < 51.3f);
	assert(ParseLatitude("0", latitude) && latitude && *latitude == 0.0f);
	assert(ParseLatitude("-33", latitude) && latitude && *latitude == -33.0f);
	assert(ParseLatitude("off", latitude) && !latitude);
	for (char const *invalid : {"", "on", "91", "-90.5", "51N", "nan"}) assert(!ParseLatitude(invalid, latitude));
	return 0;
}
