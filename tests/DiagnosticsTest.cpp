#include "Diagnostics.h"

#include <cassert>
#include <string>
#include <vector>

using nSCD3D11::RedactAccount;

int main() {
	std::vector<std::string> const profiles{R"(C:\Users\Jane Doe)"};
	std::vector<std::string> const names{"Jane Doe"};
	auto const redact = [&](char const *text) { return RedactAccount(text, profiles, names); };

	// The profile folder, in any case and with either separator.
	assert(redact(R"(loaded: C:\Users\Jane Doe\OneDrive\Documents\SimCity 4\Plugins\SCD3D11.dll (0.4.3))") ==
	       R"(loaded: %USERPROFILE%\OneDrive\Documents\SimCity 4\Plugins\SCD3D11.dll (0.4.3))");
	assert(redact("settings read from c:/users/jane doe/Documents/SCD3D11.ini") ==
	       "settings read from %USERPROFILE%/Documents/SCD3D11.ini");
	std::string const quoted = redact("log at 'C:\\Users\\Jane Doe'");
	assert(quoted == "log at '%USERPROFILE%'");

	// The account name as a folder of its own, outside the profile.
	assert(redact(R"(in Apps: D:\Jane Doe\Games\SimCity 4\Apps\SimCity 4.exe)") ==
	       R"(in Apps: D:\%USERNAME%\Games\SimCity 4\Apps\SimCity 4.exe)");
	assert(redact(R"(E:\Backups\Jane Doe)") == R"(E:\Backups\%USERNAME%)");

	// A longer folder name or plain text that merely contains the name stays.
	assert(redact(R"(C:\Users\Jane Doe2\Plugins)") == R"(C:\Users\Jane Doe2\Plugins)");
	assert(redact(R"(D:\Jane Does\Plugins)") == R"(D:\Jane Does\Plugins)");
	assert(redact("player Jane Doe built a city") == "player Jane Doe built a city");

	// Nothing to redact without an account, and text without paths passes through.
	assert(RedactAccount(R"(C:\Users\Jane Doe\x)", {}, {}) == R"(C:\Users\Jane Doe\x)");
	assert(redact("") == "");
	assert(redact("driver in use: amdxx32.dll") == "driver in use: amdxx32.dll");
	return 0;
}
