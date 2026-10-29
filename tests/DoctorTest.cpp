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
	}
	{
		Environment environment = Clean();
		environment.arguments = {"-r1366x768"};
		assert(Has(Diagnose(environment), Severity::Warning, "expects -r<width>x<height>x32"));
		environment.arguments = {"-r1920x1080x16"};
		assert(Has(Diagnose(environment), Severity::Warning, "16-bit colour"));
	}
	{
		// SC4 fell back to another size because the screen has no such mode.
		Environment environment = Clean();
		environment.arguments = {"-f", "-CustomResolution:enabled", "-r1600x900x32"};
		environment.gameFullScreen = true;
		assert(Has(Diagnose(environment), Severity::Warning, "This screen has no 1600x900 mode"));
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

		// Borderless does not read the size, and a bad boolean stops it just like a missing key.
		(*environment.graphicsOptions)["windowmode"] = "Borderless";
		(*environment.graphicsOptions)["enableintrovideo"] = "off";
		(*environment.graphicsOptions)["forcedrawonscroll"] = "true";
		(*environment.graphicsOptions)["colordepth"] = "16";
		findings = Diagnose(environment);
		assert(Has(findings, Severity::Problem, "cannot read EnableIntroVideo=off"));
		assert(!Has(findings, Severity::Problem, "has no WindowHeight"));
		assert(Has(findings, Severity::Note, "ForceDrawOnScroll=true"));
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
	}
	{
		// Mistyped and leftover switches.
		Environment environment = Clean();
		environment.arguments = {"-UserDir:C:\\My", "Games\\SC4\\", "-LiveShadow", "-VSync=off", "-NativeShadowMasks",
		                         "-MaxFPS:60", "-SomeOtherPlugin:on", "-CPUPriority:Realtime", "-intro:off"};
		auto const findings = Diagnose(environment);
		assert(Has(findings, Severity::Warning, "Not a switch on the command line: Games\\SC4\\"));
		assert(Has(findings, Severity::Warning, "Did you mean -LiveShadows?"));
		assert(Has(findings, Severity::Warning, "Use a colon instead: -VSync:off"));
		assert(Has(findings, Severity::Warning, "-NativeShadowMasks no longer does anything"));
		assert(Has(findings, Severity::Note, "-MaxFPS:60 is an scvk switch"));
		assert(Has(findings, Severity::Note, "fine if another plugin reads them: -SomeOtherPlugin:on"));
		assert(Has(findings, Severity::Warning, "-CPUPriority:High"));
		assert(!Has(findings, Severity::Note, "-intro:off"));
	}
	{
		Environment environment = Clean();
		environment.arguments = {"-w", "-f", "-Borderless", "-r1920x1080x32", "-r1366x768x32"};
		environment.gameSize.reset();
		auto const findings = Diagnose(environment);
		assert(Has(findings, Severity::Warning, "Both -w and -f"));
		assert(Has(findings, Severity::Note, "-Borderless only applies with -f"));
		assert(Has(findings, Severity::Warning, "-r1920x1080x32 and -r1366x768x32 are both"));
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
		                       {"C:\\SC4\\Plugins\\sc4_painter.dll", "", ""}};
		auto const findings = Diagnose(environment);
		assert(Has(findings, Severity::Problem, "compatibility mode (WINXPSP3)"));
		assert(Has(findings, Severity::Note, "settings on SimCity 4.exe: HIGHDPIAWARE"));
		assert(Has(findings, Severity::Warning, "not SimCity 4 version 1.1.641"));
		assert(Has(findings, Severity::Note, "ReShade 6.8.0.0 is installed as Apps\\dxgi.dll"));
		assert(Has(findings, Severity::Warning, "Apps\\d3d11.dll is DXVK"));
		assert(Has(findings, Severity::Note, "Apps\\ddraw.dll is dgVoodoo"));
		assert(Has(findings, Severity::Note, "imgui.dll is left over"));
		assert(Has(findings, Severity::Warning, "scd3d11.dll is loaded twice"));
		assert(Has(findings, Severity::Warning, "memo.3dcamera.dll and sc4-moderncamera.dll all turn the camera"));
		assert(Has(findings, Severity::Warning, "sc4singlecpu.dll is replaced by SC4CPUOptions"));
		assert(Has(findings, Severity::Note, "scvk.dll is installed too"));
		assert(Has(findings, Severity::Note, "Overlays in the game: Steam overlay"));
		assert(Has(findings, Severity::Warning, "SC4 Painter draws its ground fills"));
	}
	{
		// Plugin versions that predate SCD3D11 support. Equal and newer versions, and DLLs without a
		// version resource, pass.
		Environment environment = Clean();
		environment.modules = {{"C:\\SC4\\Plugins\\SC4GraphicsOptions.dll", "SC4GraphicsOptions", "1.3.0.0"},
		                       {"C:\\SC4\\Plugins\\SC4CPUOptions.dll", "SC4CPUOptions", "1.1.0.0"},
		                       {"C:\\SC4\\Plugins\\SC4RenderServices.dll", "SC4RenderServices", ""}};
		auto const findings = Diagnose(environment);
		assert(Has(findings, Severity::Warning, "SC4GraphicsOptions 1.3.0.0 is older than 1.4.1"));
		assert(!Has(findings, Severity::Warning, "SC4CPUOptions"));
		assert(!Has(findings, Severity::Warning, "SC4RenderServices"));
		environment.modules[0].version = "1.10.0.0";
		assert(!Has(Diagnose(environment), Severity::Warning, "older than"));
	}
	return 0;
}
