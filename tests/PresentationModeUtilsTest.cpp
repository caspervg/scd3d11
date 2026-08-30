#include "PresentationModeUtils.h"

#include <cassert>
#include <string>

using nSCD3D11::BorderlessFullscreenRequested;
using nSCD3D11::CentredClientRectangle;
using nSCD3D11::ClientRectangle;
using nSCD3D11::LowercaseCopy;
using nSCD3D11::PresentationMode;
using nSCD3D11::RequestedMonitorIndex;
using nSCD3D11::SelectPresentationMode;

namespace {
	// The driver folds the process command line once and hands the result to every query, so the
	// tests go through the same fold rather than hand-writing lowercase strings.
	PresentationMode Select(bool fullscreen, bool fallbackToWindowed, char const *commandLine) {
		return SelectPresentationMode(fullscreen, fallbackToWindowed, LowercaseCopy(commandLine));
	}

	uint32_t Monitor(char const *commandLine) {
		return RequestedMonitorIndex(LowercaseCopy(commandLine));
	}

	void TestLowercaseCopy() {
		assert(LowercaseCopy("SimCity 4.exe -Borderless") == "simcity 4.exe -borderless");
		assert(LowercaseCopy("") == "");
		// Only ASCII letters fold; digits, punctuation and the switch separators survive.
		assert(LowercaseCopy("-Monitor:2 -VSync:off") == "-monitor:2 -vsync:off");
	}

	void TestBorderlessDetection() {
		assert(BorderlessFullscreenRequested(LowercaseCopy("sc4.exe -Borderless")));
		assert(BorderlessFullscreenRequested(LowercaseCopy("sc4.exe -borderless")));
		assert(BorderlessFullscreenRequested(LowercaseCopy("sc4.exe -FullscreenMode:Borderless")));
		assert(!BorderlessFullscreenRequested(LowercaseCopy("sc4.exe")));
		assert(!BorderlessFullscreenRequested(LowercaseCopy("sc4.exe -CustomResolution -VSync:off")));
	}

	// The decision table the driver runs on every SetVideoMode.
	void TestPresentationModeSelection() {
		// A windowed mode ignores the fullscreen switches entirely.
		assert(Select(false, false, "sc4.exe") == PresentationMode::Windowed);
		assert(Select(false, false, "sc4.exe -Borderless") == PresentationMode::Windowed);

		// A fullscreen mode goes exclusive unless borderless was asked for.
		assert(Select(true, false, "sc4.exe") == PresentationMode::ExclusiveFullscreen);
		assert(Select(true, false, "sc4.exe -Borderless") == PresentationMode::BorderlessFullscreen);
		assert(Select(true, false, "sc4.exe -FullscreenMode:Borderless") == PresentationMode::BorderlessFullscreen);

		// The recovery fallback overrides both, so a failing mode switch still lands in a window.
		assert(Select(true, true, "sc4.exe") == PresentationMode::Windowed);
		assert(Select(true, true, "sc4.exe -Borderless") == PresentationMode::Windowed);
	}

	void TestMonitorIndex() {
		assert(Monitor("sc4.exe") == 0);
		assert(Monitor("sc4.exe -Monitor:1") == 1);
		assert(Monitor("sc4.exe -Monitor:2 -Borderless") == 2);
		assert(Monitor("sc4.exe -Monitor:10") == 10);
		// 0 is the documented "use the primary monitor" value and needs no special casing.
		assert(Monitor("sc4.exe -Monitor:0") == 0);
		// Anything that does not parse falls back to the primary monitor rather than a garbage index.
		assert(Monitor("sc4.exe -Monitor:") == 0);
		assert(Monitor("sc4.exe -Monitor:abc") == 0);
		assert(Monitor("sc4.exe -Monitor: 2") == 0);
		assert(Monitor("sc4.exe -Monitor:-2") == 0);
	}

	void TestCentredClientRectangle() {
		ClientRectangle const monitor{0, 0, 2560, 1440};

		// A mode matching the monitor fills it exactly, which is the common borderless case.
		ClientRectangle const exact = CentredClientRectangle(monitor, 2560, 1440);
		assert(exact.left == 0 && exact.top == 0 && exact.right == 2560 && exact.bottom == 1440);

		// A smaller mode keeps its own size and is centred rather than stretched, so the client
		// area still matches what the game renders and where it thinks the mouse is.
		ClientRectangle const smaller = CentredClientRectangle(monitor, 1920, 1080);
		assert(smaller.right - smaller.left == 1920 && smaller.bottom - smaller.top == 1080);
		assert(smaller.left == 320 && smaller.top == 180);

		// A secondary monitor's origin is carried through, so -Monitor:<n> lands on that display.
		ClientRectangle const secondary = CentredClientRectangle(ClientRectangle{2560, 0, 4480, 1080}, 1280, 720);
		assert(secondary.left == 2880 && secondary.top == 180);
		assert(secondary.right == 4160 && secondary.bottom == 900);

		// A mode larger than the monitor still keeps its requested size; it overhangs instead of
		// being silently clamped to something the game is not rendering at.
		ClientRectangle const larger = CentredClientRectangle(monitor, 3840, 2160);
		assert(larger.right - larger.left == 3840 && larger.bottom - larger.top == 2160);
		assert(larger.left == -640 && larger.top == -360);
	}
}

int main() {
	TestLowercaseCopy();
	TestBorderlessDetection();
	TestPresentationModeSelection();
	TestMonitorIndex();
	TestCentredClientRectangle();
	return 0;
}
