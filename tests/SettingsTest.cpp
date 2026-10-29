#include "Settings.h"

#include <cassert>
#include <string>

using nSCD3D11::Settings::ParseFlag;

int main() {
	bool flag = false;
	// A bare switch is on. The rest is sc4-dll-utilities' TryParse<bool>, in any case.
	for (char const *on : {"", "on", "ON", "true", "Enabled", "1"}) assert(ParseFlag(on, flag) && flag);
	for (char const *off : {"off", "false", "DISABLED", "0"}) assert(ParseFlag(off, flag) && !flag);
	for (char const *neither : {"yes", "maybe", "2", " on"}) assert(!ParseFlag(neither, flag));

	std::string features;
	for (char const *good : {"off", "rays", "rays,terrain,lamps,world", "world,lamps"}) {
		assert(nSCD3D11::Settings::ParseServerLight(good, features) && features == good);
	}
	for (char const *bad : {"", "on", "rays,", ",rays", "rays,shadows", "rays terrain"}) {
		assert(!nSCD3D11::Settings::ParseServerLight(bad, features));
	}
	return 0;
}
