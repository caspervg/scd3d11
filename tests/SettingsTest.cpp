#include "Settings.h"

#include <cassert>
#include <string>

using nSCD3D11::Settings::ParseFlag;

int main() {
	assert(ParseFlag("", false) && ParseFlag("on", false) && ParseFlag("true", false) && ParseFlag("yes", false) &&
	       ParseFlag("1", false));
	assert(!ParseFlag("off", true) && !ParseFlag("false", true) && !ParseFlag("no", true) && !ParseFlag("0", true));
	assert(ParseFlag("maybe", true) && !ParseFlag("maybe", false));
	return 0;
}
