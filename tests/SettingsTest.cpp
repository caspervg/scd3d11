#include "Settings.h"

#include <cassert>
#include <string>

using nSCD3D11::Settings::ParseFlag;
using nSCD3D11::Settings::ParseNumber;

int main() {
	bool flag = false;
	// A bare switch is on. The rest is sc4-dll-utilities' TryParse<bool>, in any case.
	for (char const *on : {"", "on", "ON", "true", "Enabled", "1"}) assert(ParseFlag(on, flag) && flag);
	for (char const *off : {"off", "false", "DISABLED", "0"}) assert(ParseFlag(off, flag) && !flag);
	for (char const *neither : {"yes", "maybe", "2", " on"}) assert(!ParseFlag(neither, flag));

	uint32_t number = 7;
	assert(ParseNumber("1", 1, 16, number) && number == 1);
	assert(ParseNumber("16", 1, 16, number) && number == 16);
	assert(ParseNumber("08", 1, 16, number) && number == 8);
	// Out of range or not a plain number leaves the value alone.
	for (char const *rejected : {"", "0", "17", "-1", "8mb", " 8", "1.5", "9999999999"}) {
		assert(!ParseNumber(rejected, 1, 16, number) && number == 8);
	}
	return 0;
}
