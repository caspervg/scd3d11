#include "Doctor.h"

#include <cassert>
#include <string>
#include <vector>

using namespace nSCD3D11::Doctor;

namespace {
	// A healthy game: SCD3D11 drew at the size it was asked for, nothing else installed.
	Environment Clean() {
		Environment environment;
		environment.driverIsSCD3D11 = true;
		environment.screen = Size{1920, 1080};
		environment.displayModes = {Size{1366, 768}, Size{1920, 1080}};
		environment.gameSize = Size{1920, 1080};
		return environment;
	}

	bool Has(std::vector<Finding> const &findings, Severity severity, std::string const &text) {
		for (Finding const &finding : findings) {
			if (finding.severity == severity && finding.text.find(text) != std::string::npos) return true;
		}
		return false;
	}
}

int main() {
	assert(Diagnose(Clean()).empty());
	{
		Environment environment = Clean();
		environment.screenModeWarning = "SimCity 4 asked for full screen at 1600x768. SCD3D11 used the screen's own 1920x1080 instead.";
		assert(Has(Diagnose(environment), Severity::Warning, "SCD3D11 used the screen's own 1920x1080"));
		environment.arguments = {"-f", "-r1600x768x32"};
		environment.gameSize = Size{1920, 1080};
		assert(Diagnose(environment).size() == 1);
	}
	{
		// A resolution without the r is broken, not merely unknown.
		Environment environment = Clean();
		environment.arguments = {"-1280x1024x32"};
		auto const findings = Diagnose(environment);
		assert(Has(findings, Severity::Warning, "SC4 ignores -1280x1024x32. A resolution needs an r in front: -r1280x1024x32"));
		assert(!Has(findings, Severity::Note, "does not know"));
	}
	assert(IsWatched("C:\\SC4\\Plugins\\SC4GraphicsOptions.dll"));
	assert(!IsWatched("C:\\SC4\\Plugins\\NAM.dll"));
	assert(ReleaseVersion(R"({"url":"x","tag_name": "v0.3.3","name":"SCD3D11 0.3.3"})") == "0.3.3");
	assert(ReleaseVersion(R"({"message":"Not Found"})").empty());
	assert(ReleaseVersion(R"({"tag_name":"latest"})").empty());
	assert(IsNewer("0.3.3", "0.3.2.0") && IsNewer("0.10.0", "0.9.9.0"));
	assert(!IsNewer("0.3.2", "0.3.2.0") && !IsNewer("0.3.1", "0.3.2.0"));

	{
		// The colon form of -r that SC4 silently ignores.
		Environment environment = Clean();
		environment.arguments = {"-CustomResolution:enabled", "-r:1366x768x32"};
		auto const findings = Diagnose(environment);
		assert(Has(findings, Severity::Warning, "without the colon: -r1366x768x32"));
		assert(!Has(findings, Severity::Warning, "SC4 runs at"));
	}
	{
		Environment environment = Clean();
		environment.arguments = {"-r1366x768"};
		assert(Has(Diagnose(environment), Severity::Warning, "expects -r<width>x<height>x32"));
		environment.arguments = {"-r1920x1080x16"};
		assert(Has(Diagnose(environment), Severity::Warning, "16-bit colour"));
	}
	{
		// Full screen at a size the screen does not offer, listing the largest ones it does.
		Environment environment = Clean();
		environment.arguments = {"-f", "-CustomResolution:enabled", "-r1600x900x32"};
		environment.gameFullScreen = true;
		assert(Has(Diagnose(environment), Severity::Warning,
		           "-r1600x900x32 asks for full screen at 1600x900, a size this screen does not offer"));
		assert(Has(Diagnose(environment), Severity::Warning, "The screen offers 1920x1080, 1366x768."));
		environment.arguments = {"-w", "-CustomResolution:enabled", "-r1600x900x32"};
		environment.gameFullScreen = false;
		assert(!Has(Diagnose(environment), Severity::Warning, "does not offer"));
		environment.gameFullScreen = true;
		environment.arguments = {"-f", "-r1366x768x32"};
		assert(Has(Diagnose(environment), Severity::Warning, "-CustomResolution:enabled"));
		environment.gameSize = Size{1366, 768};
		assert(Diagnose(environment).empty());
		environment.arguments = {"-w", "-r2560x1440x32"};
		environment.gameSize = Size{2560, 1440};
		assert(Has(Diagnose(environment), Severity::Warning, "larger than the 1920x1080 screen"));
	}
	{
		// SC4GraphicsOptions replaces the window switches and stops reading at a missing key.
		Environment environment = Clean();
		environment.arguments = {"-r1366x768x32"};
		environment.graphicsOptions = std::map<std::string, std::string>{
			{"enableintrovideo", "false"}, {"pausegameonfocusloss", "false"}, {"driver", "DirectX"},
			{"windowmode", "FullScreen"}, {"colordepth", "32"}, {"windowwidth", "1366"}};
		auto findings = Diagnose(environment);
		assert(Has(findings, Severity::Problem, "has no WindowHeight"));
		assert(Has(findings, Severity::Warning, "replaces -r1366x768x32"));
		assert(Has(findings, Severity::Note, "WindowMode=FullScreen"));
		(*environment.graphicsOptions)["windowheight"] = "900";
		(*environment.graphicsOptions)["windowwidth"] = "1600";
		assert(Has(Diagnose(environment), Severity::Warning, "SC4GraphicsOptions.ini asks for full screen at 1600x900"));
		environment.graphicsOptions->erase("windowheight");
		(*environment.graphicsOptions)["windowwidth"] = "1366";
		findings = Diagnose(environment);

		// Borderless does not read the size, and a bad boolean stops it just like a missing key.
		(*environment.graphicsOptions)["windowmode"] = "Borderless";
		(*environment.graphicsOptions)["enableintrovideo"] = "off";
		(*environment.graphicsOptions)["forcedrawonscroll"] = "true";
		(*environment.graphicsOptions)["colordepth"] = "16";
		findings = Diagnose(environment);
		assert(Has(findings, Severity::Problem, "cannot read EnableIntroVideo=off"));
		assert(!Has(findings, Severity::Problem, "has no WindowHeight"));
		assert(Has(findings, Severity::Warning, "ForceDrawOnScroll=true"));
		assert(Has(findings, Severity::Warning, "Set ColorDepth=32"));
	}
	{
		// Not the driver in use: the reason comes from -d or SC4GraphicsOptions.
		Environment environment = Clean();
		environment.driverIsSCD3D11 = false;
		environment.activeDriverModule = "C:\\SC4\\Plugins\\scvk.dll";
		environment.arguments = {"-d:OpenGL"};
		auto const findings = Diagnose(environment);
		assert(Has(findings, Severity::Problem, "drawing with scvk.dll"));
		assert(Has(findings, Severity::Problem, "-d:OpenGL picks it"));
		assert(findings.front().severity == Severity::Problem);

		environment.arguments.clear();
		environment.graphicsOptions = std::map<std::string, std::string>{{"driver", "Software"}};
		assert(Has(Diagnose(environment), Severity::Problem, "Driver=Software. Set it to DirectX"));
		environment.driverIsSCD3D11.reset();
		assert(!Has(Diagnose(environment), Severity::Problem, "drawing with"));

		// SC4 remembers software rendering after a hardware driver failed once.
		environment = Clean();
		environment.driverIsSCD3D11 = false;
		environment.activeDriverModule = "C:\\SC4\\Apps\\SimCity 4.exe";
		environment.activeDriverBuiltIn = true;
		environment.activeDriverClass = 0x7ACA35C6;
		assert(Has(Diagnose(environment), Severity::Problem, "SC4's own software driver"));
		assert(Has(Diagnose(environment), Severity::Problem, "Start once with -d:DirectX"));
		environment.activeDriverClass = 0xBADB6906;
		assert(Has(Diagnose(environment), Severity::Problem, "SCD3D11 could not start Direct3D 11"));
	}
	{
		// Mistyped and leftover switches.
		Environment environment = Clean();
		environment.arguments = {"-UserDir:C:\\My", "Games\\SC4\\", "-LiveShadow", "-VSync=off", "-NativeShadowMasks",
		                         "-MaxFPS:60", "-SomeOtherPlugin:on", "-CPUPriority:Realtime", "-intro:off",
		                         "-NetCommandGenerator:enabled", "-FullscreenMode:Borderless", "-b"};
		auto const findings = Diagnose(environment);
		assert(Has(findings, Severity::Warning, "Not a switch on the command line: Games\\SC4\\"));
		assert(Has(findings, Severity::Warning, "Did you mean -LiveShadows?"));
		assert(Has(findings, Severity::Warning, "Use a colon instead: -VSync:off"));
		assert(Has(findings, Severity::Warning, "-NativeShadowMasks no longer does anything"));
		assert(Has(findings, Severity::Note, "-MaxFPS:60 is an scvk switch"));
		assert(Has(findings, Severity::Note, "fine if another plugin reads them: -SomeOtherPlugin:on"));
		assert(Has(findings, Severity::Warning, "-CPUPriority:High"));
		assert(!Has(findings, Severity::Note, "-intro:off"));
		assert(!Has(findings, Severity::Note, "-NetCommandGenerator"));
		assert(!Has(findings, Severity::Note, "-FullscreenMode"));
		assert(!Has(findings, Severity::Note, "-SomeOtherPlugin:on, -b"));
	}
	{
		Environment environment = Clean();
		environment.arguments = {"-w", "-f", "-Borderless", "-r1920x1080x32", "-r1366x768x32"};
		environment.gameSize.reset();
		auto const findings = Diagnose(environment);
		assert(Has(findings, Severity::Warning, "Both -w and -f"));
		assert(Has(findings, Severity::Warning, "-r1920x1080x32 and -r1366x768x32 are both"));
	}
	{
		Environment environment = Clean();
		environment.gpus = {{"AMD Radeon(TM) Graphics", 512, false}, {"NVIDIA GeForce RTX 4070", 12002, true}};
		auto const findings = Diagnose(environment);
		assert(Has(findings, Severity::Note, "GPUs in this machine: 1: AMD Radeon(TM) Graphics (512 MB), "
		                                     "2: NVIDIA GeForce RTX 4070 (12002 MB, in use). GPU=<number> in "
		                                     "SCD3D11.ini picks one."));
		assert(!Has(findings, Severity::Problem, "not the faster"));
		// The one GPU needs no hint, and a number past the last one falls back.
		environment.gpus.resize(1);
		environment.gpuNumber = 3;
		auto const single = Diagnose(environment);
		assert(Has(single, Severity::Note, "GPUs in this machine: 1: AMD Radeon(TM) Graphics (512 MB)"));
		assert(!Has(single, Severity::Note, "picks one"));
		assert(Has(single, Severity::Warning, "GPU=3 asks for a GPU this machine lacks. It has 1"));
	}
	{
		Environment environment = Clean();
		environment.monitors = {{"DELL U2720Q", {3840, 2160}, 60, true, false}, {"LG 27GL850", {2560, 1440}, 144, false, true}};
		assert(Has(Diagnose(environment), Severity::Note, "Monitors: DELL U2720Q (3840x2160 at 60 Hz, primary), "
		                                                  "LG 27GL850 (2560x1440 at 144 Hz, SC4 draws here)"));
	}
	{
		// SCD3D11.ini typos, values Settings rejected and a log from before the rename.
		Environment environment = Clean();
		environment.iniKeys = {"LiveShadow", "liveshadows", "SomethingElse"};
		environment.rejectedSettings = {{"VSync", "maybe", "SCD3D11.ini"}, {"GPU", "", "command line"},
		                                {"LogLevel", "loud", "command line"}};
		environment.oldLogLeftOver = true;
		auto const findings = Diagnose(environment);
		assert(Has(findings, Severity::Warning, "SCD3D11.ini has LiveShadow, which is not a setting. Did you mean LiveShadows?"));
		assert(!Has(findings, Severity::Warning, "liveshadows"));
		assert(Has(findings, Severity::Warning, "SCD3D11.ini has SomethingElse, which is not a setting SCD3D11 reads"));
		assert(Has(findings, Severity::Warning, "SCD3D11.ini has VSync=maybe, which SCD3D11 cannot read"));
		assert(Has(findings, Severity::Warning, "-GPU needs a value"));
		assert(Has(findings, Severity::Warning, "cannot read -LogLevel:loud"));
		assert(Has(findings, Severity::Note, "SC4D3D11.log is left over"));
	}
	{
		Environment environment = Clean();
		environment.iniError = "Invalid INI line 4";
		assert(Has(Diagnose(environment), Severity::Warning, "SCD3D11.ini cannot be read, so SCD3D11 ignores all of it: "
		                                                     "Invalid INI line 4"));
		environment.iniError.clear();
		environment.iniWithoutSection = true;
		assert(Has(Diagnose(environment), Severity::Warning, "no [SCD3D11] section"));
	}
	{
		// No 4GB patch, Wine, and a ReShade too old for the add-on.
		Environment environment = Clean();
		environment.largeAddressAware = false;
		environment.wineVersion = "9.0";
		environment.wineHost = "Linux";
		environment.appsFiles = {{"C:\\SC4\\Apps\\dxgi.dll", "ReShade", "5.9.2.0"}};
		auto const findings = Diagnose(environment);
		assert(Has(findings, Severity::Problem, "can use only 2 GB of memory"));
		assert(Has(findings, Severity::Note, "runs under Wine 9.0 on Linux"));
		assert(Has(findings, Severity::Warning, "ReShade v5.9.2 is installed as Apps\\dxgi.dll. Its effects cannot see"));
	}
	{
		// A laptop drawing on its integrated GPU. Two similar cards are fine either way.
		Environment environment = Clean();
		environment.gpus = {{"Intel(R) UHD Graphics", 128, true}, {"NVIDIA GeForce RTX 3060 Laptop GPU", 5980, false}};
		assert(Has(Diagnose(environment), Severity::Problem,
		           "draws on Intel(R) UHD Graphics, not the faster NVIDIA GeForce RTX 3060 Laptop GPU. Set GPU=2"));
		environment.gpuSystemDefault = true;
		assert(Has(Diagnose(environment), Severity::Problem, "GPU is set to default. Set GPU=2"));
		// Picked by number, it is the player's choice.
		environment.gpuSystemDefault = false;
		environment.gpuNumber = 1;
		assert(!Has(Diagnose(environment), Severity::Problem, "not the faster"));
		environment.gpuNumber = 0;
		environment.gpus = {{"NVIDIA GeForce RTX 3070", 8000, true}, {"NVIDIA GeForce RTX 3080", 10000, false}};
		assert(!Has(Diagnose(environment), Severity::Problem, "not the faster"));
	}
	{
		// Borderless turns on full screen itself, so -w with it is no window.
		Environment environment = Clean();
		environment.arguments = {"-w", "-b", "-r3840x2160x32"};
		auto const findings = Diagnose(environment);
		assert(!Has(findings, Severity::Warning, "window is larger than"));
	}
	{
		Environment environment = Clean();
		environment.rawCommandLine = "\"C:\\SC4\\SimCity 4.exe\" -UserDir:\"D:\\Games\\SC4";
		environment.userDirMissing = true;
		auto const findings = Diagnose(environment);
		assert(Has(findings, Severity::Warning, "quote that is never closed"));
		assert(Has(findings, Severity::Warning, "-UserDir folder does not exist"));
	}
	{
		// The fix names where the mode is set: the exe for all users, or for this user.
		Environment environment = Clean();
		environment.compatibilityLayer = "~ WINXPSP3";
		environment.exePath = "C:\\SC4\\Apps\\SimCity 4.exe";
		environment.compatibilityLayerUser = "~ WINXPSP3";
		environment.compatibilityLayerAllUsers = "~ HIGHDPIAWARE";
		assert(Has(Diagnose(environment), Severity::Problem, "Properties, Compatibility of C:\\SC4\\Apps\\SimCity 4.exe."));
		environment.compatibilityLayerAllUsers = "WINXPSP3";
		assert(Has(Diagnose(environment), Severity::Problem, "Change settings for all users of C:\\SC4"));
	}
	{
		// Compatibility mode, wrappers in Apps, plugins that clash.
		Environment environment = Clean();
		environment.compatibilityLayer = "~ WINXPSP3 HIGHDPIAWARE";
		environment.gameVersion641 = false;
		environment.appsFiles = {{"C:\\SC4\\Apps\\dxgi.dll", "ReShade", "6.8.0.0"},
		                         {"C:\\SC4\\Apps\\d3d11.dll", "DXVK", "10.0.17763.1"},
		                         {"C:\\SC4\\Apps\\DDraw.dll", "dgVoodoo", "2.8.7.6"},
		                         {"C:\\SC4\\Apps\\imgui.dll", "", ""}};
		environment.modules = {{"C:\\SC4\\Plugins\\SCD3D11.dll", "SCD3D11", "0.3.2.0"},
		                       {"C:\\Users\\me\\Documents\\SimCity 4\\Plugins\\SCD3D11.dll", "SCD3D11", "0.3.1.0"},
		                       {"C:\\SC4\\Plugins\\memo.3dcamera.dll", "", ""},
		                       {"C:\\SC4\\Plugins\\SC4-ModernCamera.dll", "", ""},
		                       {"C:\\SC4\\Plugins\\SC4SingleCPU.dll", "", ""},
		                       {"C:\\SC4\\Plugins\\scvk.dll", "", ""},
		                       {"C:\\Steam\\GameOverlayRenderer.dll", "", ""},
		                       {"C:\\SC4\\Plugins\\sc4_painter.dll", "", ""},
		                       {"C:\\SC4\\Apps\\VERSION.dll", "", ""},
		                       {"C:\\WINDOWS\\system32\\version.dll", "Microsoft Windows", "10.0.26100.1"}};
		environment.windowsFolder = "C:\\Windows";
		auto const findings = Diagnose(environment);
		assert(Has(findings, Severity::Problem, "compatibility mode (WINXPSP3)"));
		assert(Has(findings, Severity::Problem, "the program that started it passed it on"));
		assert(Has(findings, Severity::Note, "settings on SimCity 4.exe: HIGHDPIAWARE"));
		assert(Has(findings, Severity::Problem, "not SimCity 4 version 1.1.641"));
		assert(Has(findings, Severity::Note, "ReShade v6.8.0 is installed as Apps\\dxgi.dll"));
		assert(Has(findings, Severity::Warning, "Apps\\d3d11.dll is DXVK"));
		assert(Has(findings, Severity::Note, "Apps\\ddraw.dll is dgVoodoo"));
		assert(Has(findings, Severity::Note, "imgui.dll is left over"));
		assert(Has(findings, Severity::Warning, "scd3d11.dll is loaded twice"));
		assert(!Has(findings, Severity::Warning, "version.dll is loaded twice"));
		assert(!Has(findings, Severity::Warning, "turn the camera"));
		assert(Has(findings, Severity::Warning, "sc4singlecpu.dll is replaced by SC4CPUOptions"));
		assert(Has(findings, Severity::Note, "scvk.dll is installed too"));
		assert(Has(findings, Severity::Note, "Overlays in the game: Steam overlay"));
		assert(!Has(findings, Severity::Warning, "SCD3D11 used the screen's own"));
		assert(Has(findings, Severity::Warning, "SC4 Painter draws its ground fills"));
		assert(Has(findings, Severity::Warning, "SC4 Painter writes yellow text on the black loading screen"));
	}
	{
		// Plugin versions that predate SCD3D11 support. Equal and newer versions, and DLLs without a
		// version resource, pass.
		Environment environment = Clean();
		environment.modules = {{"C:\\SC4\\Plugins\\SC4GraphicsOptions.dll", "SC4GraphicsOptions", "1.3.0.0"},
		                       {"C:\\SC4\\Plugins\\SC4CPUOptions.dll", "SC4CPUOptions", "1.1.0.0"},
		                       {"C:\\SC4\\Plugins\\SC4RenderServices.dll", "SC4RenderServices", ""}};
		auto const findings = Diagnose(environment);
		assert(Has(findings, Severity::Warning, "SC4GraphicsOptions v1.3.0 is older than v1.4.1"));
		assert(!Has(findings, Severity::Warning, "SC4CPUOptions"));
		assert(!Has(findings, Severity::Warning, "SC4RenderServices"));
		environment.modules[0].version = "1.10.0.0";
		assert(!Has(Diagnose(environment), Severity::Warning, "older than"));
	}
	{
		// Plugins that import imgui.dll were built for SC4RenderServices before v0.8. Its own old DLL is left
		// to the version rule.
		Environment environment = Clean();
		environment.imguiPlugins = {"SC4PlopAndPaint.dll", "SC4RenderServices.dll"};
		environment.modules = {{"C:\\SC4\\Plugins\\SC4RenderServices.dll", "SC4 Render Services", "0.7.1.0"}};
		auto const findings = Diagnose(environment);
		assert(Has(findings, Severity::Warning, "SC4PlopAndPaint.dll is built for SC4RenderServices before v0.8"));
		assert(!Has(findings, Severity::Warning, "SC4RenderServices.dll is built"));
		assert(Has(findings, Severity::Warning, "SC4RenderServices v0.7.1 is older than v0.8.1"));
	}
	assert(VersionLabel("1.4.1.0") == "v1.4.1");
	assert(VersionLabel("0.8.2") == "v0.8.2");
	assert(VersionLabel("1.0.0.3") == "v1.0.0.3");
	assert(VersionLabel("").empty());
	return 0;
}
