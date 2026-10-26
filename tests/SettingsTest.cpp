#include "Settings.h"

#include <cassert>
#include <string>

using nSCD3D11::Settings::FindSwitch;
using nSCD3D11::Settings::ParseFlag;

int main() {
	std::string const commandLine =
		"\"C:\\Games\\SimCity 4.exe\" -w -LiveShadows:ON -UserDir:\"C:\\Users\\me\\SimCity 4\\\" -DrawStats -vsync:off";
	std::string value;

	// Values come back lower-cased; names match in any case.
	assert(FindSwitch(commandLine, "LiveShadows", value) && value == "on");
	assert(FindSwitch(commandLine, "VSync", value) && value == "off");

	// A bare switch is present with no value, and a quoted value loses its quotes.
	assert(FindSwitch(commandLine, "DrawStats", value) && value.empty());
	assert(FindSwitch("-GPU:\"Default\"", "GPU", value) && value == "default");

	// Only whole switch names count: -w is not -wide, -Draw is not -DrawStats.
	assert(!FindSwitch(commandLine, "Draw", value));
	assert(!FindSwitch("-wide", "w", value));
	assert(!FindSwitch(commandLine, "GPU", value));

	// A switch must start an argument, and the last one wins.
	assert(!FindSwitch("x-VSync:off", "VSync", value));
	assert(FindSwitch("-VSync:off -VSync:on", "VSync", value) && value == "on");

	assert(ParseFlag("", false) && ParseFlag("on", false) && ParseFlag("true", false) && ParseFlag("yes", false) &&
	       ParseFlag("1", false));
	assert(!ParseFlag("off", true) && !ParseFlag("false", true) && !ParseFlag("no", true) && !ParseFlag("0", true));
	assert(ParseFlag("maybe", true) && !ParseFlag("maybe", false));
	return 0;
}
