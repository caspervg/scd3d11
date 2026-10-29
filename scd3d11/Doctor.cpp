/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "Doctor.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <iterator>
#include <set>
#include <string_view>

namespace nSCD3D11::Doctor {
	namespace {
		std::string Lower(std::string_view text) {
			std::string result(text);
			for (char &character : result) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
			return result;
		}

		bool StartsWith(std::string_view text, std::string_view prefix) {
			return text.substr(0, prefix.size()) == prefix;
		}

		// The lower-case file name and folder of a path.
		std::string FileName(std::string const &path) {
			size_t const slash = path.find_last_of("\\/");
			return Lower(slash == std::string::npos ? path : path.substr(slash + 1));
		}

		std::string Folder(std::string const &path) {
			size_t const slash = path.find_last_of("\\/");
			return Lower(slash == std::string::npos ? std::string() : path.substr(0, slash));
		}

		std::string Format(char const *format, ...) {
			char text[1024];
			va_list arguments;
			va_start(arguments, format);
			vsnprintf(text, sizeof(text), format, arguments);
			va_end(arguments);
			return text;
		}

		Module const *FindModule(Environment const &environment, std::string_view name) {
			for (Module const &module : environment.modules) {
				if (FileName(module.path) == name) return &module;
			}
			return nullptr;
		}

		// A launch switch as SC4 reads it: -Name, -Name:value, or -r<size> with no colon.
		struct Switch {
			std::string token;
			std::string name;
			std::string value;
		};

		std::optional<Switch> ParseSwitch(std::string const &token) {
			if (token.size() < 2 || token[0] != '-') return std::nullopt;
			std::string const body = token.substr(1);
			if (std::tolower(static_cast<unsigned char>(body[0])) == 'r' && body.size() > 1 &&
			    std::isdigit(static_cast<unsigned char>(body[1]))) {
				return Switch{token, "r", body.substr(1)};
			}
			size_t const colon = body.find(':');
			if (colon == std::string::npos) return Switch{token, Lower(body), ""};
			return Switch{token, Lower(body.substr(0, colon)), body.substr(colon + 1)};
		}

		// Width, height and depth from -r's value. SC4 reads it with "%ux%ux%u".
		std::optional<std::array<uint32_t, 3>> ParseResolution(std::string const &value) {
			unsigned width = 0, height = 0, depth = 0;
			char extra = 0;
			if (sscanf_s(value.c_str(), "%ux%ux%u%c", &width, &height, &depth, &extra, 1u) != 3) return std::nullopt;
			return std::array<uint32_t, 3>{width, height, depth};
		}

		size_t Distance(std::string_view a, std::string_view b) {
			std::vector<size_t> row(b.size() + 1);
			for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
			for (size_t i = 1; i <= a.size(); ++i) {
				size_t diagonal = row[0];
				row[0] = i;
				for (size_t j = 1; j <= b.size(); ++j) {
					size_t const above = row[j];
					row[j] = (std::min)({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
					diagonal = above;
				}
			}
			return row[b.size()];
		}

		// Switch names SC4 itself and common plugins read, in lower case, with their usual spelling.
		// SC4's own come from the names in SimCity 4.exe. SCD3D11's must match Settings::Load.
		constexpr char const *kKnownSwitches[] = {
			// SimCity 4
			"w", "f", "r", "d", "gp", "Intro", "CustomResolution", "CustomCursors", "AutoZoomChange", "CPUPriority",
			"CPUCount", "UserDir", "Music", "Radio", "Sound", "Audio", "IME", "Verbose", "ExceptionHandling",
			"Restart", "AllowMultipleInstances", "Logging", "DebugLevel", "LuaScript", "ForceIdle",
			"CreateException", "IgnoreMissingModelDataBugs", "WriteLog", "NetCommandGenerator",
			// SCD3D11
			"LiveShadows", "NetworkShadowMasks", "VSync", "Borderless", "FlipModel", "GPU", "ViewportCull",
			"ReShade", "DrawStats", "LogLevel", "Doctor", "DoctorDialog", "UpdateCheck",
			// SC4CPUOptions and SC4DBPFLoading
			"ParallelCull", "SimTickCap", "SimTickStats", "ModelSlice", "OverlayIndex", "StartupDBPFLoadTrace",
		};

		// Switches only scvk reads. They look like SCD3D11's but do nothing here.
		constexpr char const *kScvkSwitches[] = {"MaxFPS", "FullscreenMode"};

		bool IsKnown(std::string const &name) {
			for (char const *known : kKnownSwitches) {
				if (Lower(known) == name) return true;
			}
			return false;
		}

		char const *Closest(std::string const &name) {
			if (name.size() < 4) return nullptr;
			for (char const *known : kKnownSwitches) {
				std::string const lower = Lower(known);
				if (lower.size() >= 4 && Distance(name, lower) <= 2) return known;
			}
			return nullptr;
		}

		// Full screen switches the monitor to the requested size, which only works for sizes the monitor
		// offers.
		void CheckFullScreenSize(Environment const &environment, uint32_t width, uint32_t height, char const *source,
		                         std::vector<Finding> &findings) {
			if (ScreenOffers(environment.displayModes, width, height)) return;
			findings.push_back({Severity::Warning, Format(
				"%s asks for full screen at %ux%u, a size this screen does not offer, so SC4 closes without a "
				"message. The screen offers %s.", source, width, height, LargestSizes(environment.displayModes).c_str())});
		}

		std::string Value(std::map<std::string, std::string> const &ini, char const *key) {
			auto const found = ini.find(Lower(key));
			return found == ini.end() ? std::string() : found->second;
		}

		bool IsBoolean(std::string const &value) {
			std::string const lower = Lower(value);
			return lower == "true" || lower == "false" || lower == "1" || lower == "0";
		}

		void CheckDriver(Environment const &environment, std::vector<Switch> const &switches,
		                 std::vector<Finding> &findings) {
			if (!environment.driverIsSCD3D11.has_value() || *environment.driverIsSCD3D11) return;
			// SC4's own drivers live in the game exe and are told apart by class ID.
			constexpr uint32_t kDirectX = 0xBADB6906, kOpenGL = 0xC4554841, kSoftware = 0x7ACA35C6;
			uint32_t const driverClass = environment.activeDriverClass;
			std::string name = environment.activeDriverModule.empty() ? std::string("another driver")
			                                                          : FileName(environment.activeDriverModule);
			std::string reason = "Another plugin may have picked a different driver.";
			if (environment.activeDriverBuiltIn) {
				name = driverClass == kSoftware ? "SC4's own software driver"
				       : driverClass == kOpenGL ? "SC4's own OpenGL driver"
				       : driverClass == kDirectX ? "SC4's own DirectX driver" : "a driver inside SimCity 4.exe";
				if (driverClass == kSoftware) {
					reason = "SC4 is set to software rendering, which it also switches to after a hardware driver "
					         "fails once. Start once with -d:DirectX to go back.";
				} else if (driverClass == kDirectX) {
					reason = "SCD3D11 could not start Direct3D 11. The lines above in this log say why.";
				}
			}
			for (Switch const &entry : switches) {
				std::string const value = Lower(entry.value);
				if (entry.name == "d" && value != "directx" && value != "dx") {
					reason = Format("%s picks it. Remove that switch.", entry.token.c_str());
				}
			}
			if (environment.graphicsOptions) {
				std::string const driver = Value(*environment.graphicsOptions, "Driver");
				if (!driver.empty() && Lower(driver) != "directx") {
					reason = Format("SC4GraphicsOptions.ini has Driver=%s. Set it to DirectX.", driver.c_str());
				}
			}
			findings.push_back({Severity::Problem, Format("SimCity 4 is drawing with %s, not SCD3D11. %s",
			                                              name.c_str(), reason.c_str())});
		}

		void CheckGraphicsOptions(Environment const &environment, std::vector<Switch> const &switches,
		                          std::vector<Finding> &findings) {
			if (!environment.graphicsOptions) return;
			auto const &ini = *environment.graphicsOptions;
			std::string const mode = Value(ini, "WindowMode");
			bool const borderless = StartsWith(Lower(mode), "borderless");
			findings.push_back({Severity::Note, Format(
				"SC4GraphicsOptions sets the window: WindowMode=%s, WindowWidth=%s, WindowHeight=%s, Driver=%s",
				mode.c_str(), borderless ? "(screen)" : Value(ini, "WindowWidth").c_str(),
				borderless ? "(screen)" : Value(ini, "WindowHeight").c_str(), Value(ini, "Driver").c_str())});

			// It reads these in this order and stops at the first one that is missing or unreadable,
			// keeping its defaults for the rest. The size is not read for borderless.
			std::vector<char const *> required{"EnableIntroVideo", "PauseGameOnFocusLoss", "Driver", "WindowMode",
			                                   "ColorDepth"};
			if (!borderless) {
				required.push_back("WindowWidth");
				required.push_back("WindowHeight");
			}
			for (char const *key : required) {
				std::string const value = Value(ini, key);
				bool const boolean = std::string_view(key) == "EnableIntroVideo" ||
				                     std::string_view(key) == "PauseGameOnFocusLoss";
				if (value.empty() || (boolean && !IsBoolean(value))) {
					findings.push_back({Severity::Problem, Format(
						"SC4GraphicsOptions.ini %s %s. SC4GraphicsOptions stops reading there and uses its "
						"defaults for the rest, which can be a 1024x768 window or a size the screen lacks.",
						value.empty() ? "has no" : "cannot read", value.empty() ? key : (std::string(key) + "=" + value).c_str())});
					break;
				}
			}
			std::string const forceDraw = Value(ini, "ForceDrawOnScroll");
			if (!forceDraw.empty() && !IsBoolean(forceDraw)) {
				findings.push_back({Severity::Warning, Format(
					"SC4GraphicsOptions.ini cannot read ForceDrawOnScroll=%s. Use true or false.", forceDraw.c_str())});
			}
			if (Value(ini, "ColorDepth") == "16") {
				findings.push_back({Severity::Warning,
				                    "SC4GraphicsOptions.ini asks for 16-bit colour, and SCD3D11 only has 32-bit "
				                    "screen modes. Set ColorDepth=32."});
			}
			if (Lower(mode) == "fullscreen") {
				unsigned width = 0, height = 0;
				if (sscanf_s(Value(ini, "WindowWidth").c_str(), "%u", &width) == 1 &&
				    sscanf_s(Value(ini, "WindowHeight").c_str(), "%u", &height) == 1) {
					CheckFullScreenSize(environment, width, height, "SC4GraphicsOptions.ini", findings);
				}
			}
			if (Lower(forceDraw) == "true" || forceDraw == "1") {
				findings.push_back({Severity::Note,
				                    "SC4GraphicsOptions.ini has ForceDrawOnScroll=true. It works around tearing in "
				                    "SC4's DirectX 7 driver and only makes scrolling slower with SCD3D11. Set it to false."});
			}

			std::string overridden;
			for (Switch const &entry : switches) {
				if (entry.name == "w" || entry.name == "f" || entry.name == "r" || entry.name == "d") {
					overridden += (overridden.empty() ? "" : " ") + entry.token;
				}
			}
			if (!overridden.empty()) {
				findings.push_back({Severity::Warning, Format(
					"SC4GraphicsOptions replaces %s from the command line. Set WindowMode, WindowWidth, WindowHeight "
					"and Driver in SC4GraphicsOptions.ini instead.", overridden.c_str())});
			}
		}

		void CheckSwitches(Environment const &environment, std::vector<Switch> const &switches,
		                   std::vector<std::string> const &strays, std::vector<Finding> &findings) {
			if (!strays.empty()) {
				std::string list;
				for (std::string const &stray : strays) list += (list.empty() ? "" : ", ") + stray;
				findings.push_back({Severity::Warning, Format(
					"Not a switch on the command line: %s. A -UserDir path with spaces must be in quotes, "
					"for example -UserDir:\"D:\\My Games\\SimCity 4\\\".", list.c_str())});
			}
			if (environment.userDirMissing) {
				findings.push_back({Severity::Warning, "The -UserDir folder does not exist, so SC4 cannot load the "
				                                       "plugins in it."});
			}
			if (std::count(environment.rawCommandLine.begin(), environment.rawCommandLine.end(), '"') % 2 != 0) {
				findings.push_back({Severity::Warning,
				                    "The command line has a quote that is never closed. A shortcut cuts its Target "
				                    "off after 259 characters, so check that nothing at the end is missing."});
			} else if (environment.rawCommandLine.size() >= 250) {
				findings.push_back({Severity::Note, Format(
					"The command line is %zu characters long. A shortcut cuts its Target off after 259 characters, so "
					"check that nothing at the end is missing.", environment.rawCommandLine.size())});
			}

			std::map<std::string, Switch const *> seen;
			std::optional<std::array<uint32_t, 3>> resolution;
			std::string resolutionToken;
			bool windowed = false, fullScreen = false, borderless = false, customResolution = false;
			std::string unknown;
			for (Switch const &entry : switches) {
				auto const [previous, first] = seen.emplace(entry.name, &entry);
				if (!first && Lower(previous->second->value) != Lower(entry.value)) {
					findings.push_back({Severity::Warning, Format("%s and %s are both on the command line. Keep one.",
					                                              previous->second->token.c_str(), entry.token.c_str())});
				}
				windowed |= entry.name == "w";
				fullScreen |= entry.name == "f";
				borderless |= entry.name == "borderless";
				customResolution |= entry.name == "customresolution";

				size_t const equals = entry.name.find('=');
				if (equals != std::string::npos) {
					findings.push_back({Severity::Warning, Format(
						"%s is not read. Use a colon instead: -%s:%s", entry.token.c_str(),
						entry.token.substr(1, equals).c_str(), entry.token.substr(equals + 2).c_str())});
				} else if (entry.name == "r") {
					resolution = ParseResolution(entry.value);
					resolutionToken = entry.token;
					if (StartsWith(entry.token, "-r:") || StartsWith(entry.token, "-R:")) {
						findings.push_back({Severity::Warning, Format(
							"SC4 ignores %s. Write it without the colon: -r%s", entry.token.c_str(), entry.value.c_str())});
						// Already explained, so the size SC4 ended up with is not compared with it.
						resolution.reset();
					} else if (!resolution) {
						findings.push_back({Severity::Warning, Format(
							"SC4 ignores %s. It expects -r<width>x<height>x32, for example -r1920x1080x32.",
							entry.token.c_str())});
					} else if ((*resolution)[2] != 32) {
						findings.push_back({Severity::Warning, Format(
							"%s asks for %u-bit colour, and SCD3D11 only has 32-bit screen modes. Use x32.",
							entry.token.c_str(), (*resolution)[2])});
					}
				} else if (entry.name == "cpupriority" && StartsWith(Lower(entry.value), "real")) {
					findings.push_back({Severity::Warning, Format(
						"%s can starve Windows of time for input and sound. Use -CPUPriority:High.", entry.token.c_str())});
				} else if (entry.name == "nativeshadowmasks") {
					findings.push_back({Severity::Warning, Format(
						"%s no longer does anything. Live shadows are -LiveShadows:on, and -NetworkShadowMasks:on "
						"keeps the flat network shadows.", entry.token.c_str())});
				} else if (!IsKnown(entry.name)) {
					bool scvk = false;
					for (char const *name : kScvkSwitches) scvk |= Lower(name) == entry.name;
					char const *const closest = Closest(entry.name);
					if (scvk) {
						findings.push_back({Severity::Note, Format(
							"%s is an scvk switch. SCD3D11 does not read it.", entry.token.c_str())});
					} else if (closest != nullptr) {
						findings.push_back({Severity::Warning, Format(
							"%s is not a switch anything reads. Did you mean -%s?", entry.token.c_str(), closest)});
					} else {
						unknown += (unknown.empty() ? "" : ", ") + entry.token;
					}
				}
			}
			if (!unknown.empty()) {
				findings.push_back({Severity::Note, Format(
					"Switches SCD3D11 does not know, fine if another plugin reads them: %s", unknown.c_str())});
			}
			if (windowed && fullScreen) {
				findings.push_back({Severity::Warning, "Both -w and -f are on the command line. Keep one."});
			}
			if (borderless && windowed) {
				findings.push_back({Severity::Note, "-Borderless only applies with -f, not in a window."});
			}

			// Compare what -r asked for with what SC4 ended up with. SC4GraphicsOptions explains any
			// difference itself.
			if (!resolution || environment.graphicsOptions) return;
			uint32_t const width = (*resolution)[0], height = (*resolution)[1];
			if (windowed && (width > environment.screen.width || height > environment.screen.height) &&
			    environment.screen.width != 0) {
				findings.push_back({Severity::Warning, Format("A %ux%u window is larger than the %ux%u screen.", width,
				                                              height, environment.screen.width, environment.screen.height)});
			}
			if (fullScreen) CheckFullScreenSize(environment, width, height, resolutionToken.c_str(), findings);
			if (!environment.gameSize ||
			    (environment.gameSize->width == width && environment.gameSize->height == height)) return;
			std::string const hint =
				customResolution ? "" : " Sizes other than SC4's standard ones also need -CustomResolution:enabled.";
			findings.push_back({Severity::Warning, Format("SC4 runs at %ux%u, not the %ux%u that %s asks for.%s",
			                                              environment.gameSize->width, environment.gameSize->height,
			                                              width, height, resolutionToken.c_str(), hint.c_str())});
		}

		// What a DLL in the Apps folder is, from its version resource.
		std::string Identify(Module const &module) {
			std::string const product = Lower(module.product);
			if (product.find("reshade") != std::string::npos) return "ReShade";
			if (product.find("dxvk") != std::string::npos) return "DXVK";
			if (product.find("special k") != std::string::npos) return "Special K";
			if (product.find("dgvoodoo") != std::string::npos) return "dgVoodoo";
			if (product.find("dxwrapper") != std::string::npos) return "DxWrapper";
			return module.product.empty() ? std::string("an unknown wrapper") : module.product;
		}

		// Compares dotted versions part by part. Missing parts count as 0, so 1.4.1.0 equals 1.4.1.
		bool OlderThan(std::string const &version, char const *minimum) {
			unsigned have[4]{}, need[4]{};
			sscanf_s(version.c_str(), "%u.%u.%u.%u", &have[0], &have[1], &have[2], &have[3]);
			sscanf_s(minimum, "%u.%u.%u.%u", &need[0], &need[1], &need[2], &need[3]);
			return std::lexicographical_compare(std::begin(have), std::end(have), std::begin(need), std::end(need));
		}

		void CheckModules(Environment const &environment, std::vector<Finding> &findings) {
			// The first versions of other plugins that work with this SCD3D11. Only plugins with a version
			// resource can be checked.
			struct Minimum { char const *file; char const *name; char const *version; char const *reason; };
			static constexpr Minimum kMinimums[] = {
				{"sc4graphicsoptions.dll", "SC4GraphicsOptions", "1.4.1", "the first whose borderless mode finds SCD3D11's window"},
				{"sc4cpuoptions.dll", "SC4CPUOptions", "1.1.0", "which took over the CPU fixes older SCD3D11 versions made"},
				{"sc4renderservices.dll", "SC4RenderServices", "0.8.0", "the first that recognises SCD3D11 as the driver"},
			};
			for (Minimum const &minimum : kMinimums) {
				Module const *const module = FindModule(environment, minimum.file);
				if (module == nullptr || module->version.empty() || !OlderThan(module->version, minimum.version)) continue;
				findings.push_back({Severity::Warning, Format("%s %s is older than %s, %s. Update it.", minimum.name,
				                                              module->version.c_str(), minimum.version, minimum.reason)});
			}

			for (char const *rival : {"scvk.dll", "scgl.dll"}) {
				if (FindModule(environment, rival) != nullptr) {
					findings.push_back({Severity::Note, Format(
						"%s is installed too. SC4 uses it instead of SCD3D11 only with -d:OpenGL or Driver=OpenGL in "
						"SC4GraphicsOptions.ini.", rival)});
				}
			}
			if (FindModule(environment, "sc4_painter.dll") != nullptr &&
			    FindModule(environment, "sc4renderservices.dll") == nullptr) {
				findings.push_back({Severity::Warning, "SC4 Painter draws its ground fills and decals through "
				                                       "SC4RenderServices, which is not installed."});
			}
			if (FindModule(environment, "sc4fullscreen32bit.dll") != nullptr) {
				findings.push_back({Severity::Note, "SC4FullScreen32Bit.dll only changes SC4's DirectX 7 driver and "
				                                    "does nothing with SCD3D11. It can be removed."});
			}
			for (char const *old : {"sc4singlecpu.dll", "sc4cpupriority.dll"}) {
				if (FindModule(environment, old) != nullptr) {
					findings.push_back({Severity::Warning, Format("%s is replaced by SC4CPUOptions. Remove it.", old)});
				}
			}
			std::string cameras;
			int cameraCount = 0;
			for (char const *camera : {"memo.3dcamera.dll", "sc4-moderncamera.dll", "sc4moderncamera.dll"}) {
				if (FindModule(environment, camera) == nullptr) continue;
				cameras += (cameras.empty() ? "" : " and ") + std::string(camera);
				++cameraCount;
			}
			if (cameraCount > 1) {
				findings.push_back({Severity::Warning, Format("%s all turn the camera. Keep one.", cameras.c_str())});
			}

			// The same plugin loaded from two folders, usually the game's Plugins and the user's. A proxy
			// such as Appsersion.dll loading the Windows original is normal, so Windows' own copies do
			// not count.
			std::map<std::string, std::vector<Module const *>> byName;
			std::string const windows = Lower(environment.windowsFolder);
			for (Module const &module : environment.modules) {
				if (windows.empty() || !StartsWith(Folder(module.path), windows)) byName[FileName(module.path)].push_back(&module);
			}
			for (auto const &[name, copies] : byName) {
				std::set<std::string> folders;
				for (Module const *copy : copies) folders.insert(Folder(copy->path));
				if (folders.size() < 2) continue;
				std::string list;
				for (Module const *copy : copies) {
					list += (list.empty() ? "" : " and ") + copy->path +
						(copy->version.empty() ? "" : " (" + copy->version + ")");
				}
				findings.push_back({Severity::Warning, Format("%s is loaded twice, from %s. Remove one.", name.c_str(),
				                                              list.c_str())});
			}

			struct Overlay { char const *file; char const *name; };
			static constexpr Overlay kOverlays[] = {
				{"gameoverlayrenderer.dll", "Steam overlay"}, {"rtsshooks.dll", "RivaTuner"},
				{"discordhook.dll", "Discord overlay"}, {"graphics-hook32.dll", "OBS capture"},
				{"nvspcap.dll", "NVIDIA overlay"},
			};
			std::string overlays;
			for (Overlay const &overlay : kOverlays) {
				if (FindModule(environment, overlay.file) != nullptr) overlays += (overlays.empty() ? "" : ", ") + std::string(overlay.name);
			}
			if (!overlays.empty()) findings.push_back({Severity::Note, "Overlays in the game: " + overlays});

			for (Module const &file : environment.appsFiles) {
				std::string const name = FileName(file.path);
				std::string const kind = Identify(file);
				if (name == "imgui.dll") {
					findings.push_back({Severity::Note, "Apps\\imgui.dll is left over from SC4RenderServices before "
					                                    "0.8 and is no longer used."});
				} else if (name == "dxgi.dll" || name == "d3d11.dll") {
					if (kind == "ReShade") {
						findings.push_back({Severity::Note, Format("ReShade %s is installed as Apps\\%s.",
						                                           file.version.c_str(), name.c_str())});
					} else {
						findings.push_back({Severity::Warning, Format(
							"Apps\\%s is %s. It sits between SCD3D11 and Direct3D 11, which is untested. Remove it "
							"if anything looks wrong.", name.c_str(), kind.c_str())});
					}
				} else if (name == "ddraw.dll" || name == "d3dimm.dll") {
					findings.push_back({Severity::Note, Format(
						"Apps\\%s is %s. SCD3D11 does not need it, only SC4's own DirectX driver uses it.", name.c_str(),
						kind.c_str())});
				} else {
					findings.push_back({Severity::Note, Format("Apps\\%s is %s.", name.c_str(), kind.c_str())});
				}
			}
		}

		void CheckGame(Environment const &environment, std::vector<Finding> &findings) {
			// Windows lists its compatibility layers separated by spaces, sometimes after a ~.
			std::string old, other;
			size_t start = 0;
			std::string const &layers = environment.compatibilityLayer;
			while (start < layers.size()) {
				size_t end = layers.find(' ', start);
				if (end == std::string::npos) end = layers.size();
				std::string const layer = layers.substr(start, end - start);
				start = end + 1;
				if (layer.empty() || layer == "~") continue;
				std::string const lower = Lower(layer);
				bool const legacy = StartsWith(lower, "win") || StartsWith(lower, "vista") || StartsWith(lower, "nt4") ||
				                    lower == "256color" || lower == "16bitcolor" || lower == "640x480";
				(legacy ? old : other) += ((legacy ? old : other).empty() ? "" : " ") + layer;
			}
			if (!old.empty()) {
				findings.push_back({Severity::Problem, Format(
					"SimCity 4.exe runs in compatibility mode (%s). SCD3D11 is not tested with it. Turn it off under "
					"Properties, Compatibility of SimCity 4.exe.", old.c_str())});
			}
			if (!other.empty()) {
				findings.push_back({Severity::Note, "Windows compatibility settings on SimCity 4.exe: " + other});
			}
			if (!environment.screenModeWarning.empty()) {
				findings.push_back({Severity::Warning, environment.screenModeWarning});
			}
			if (!environment.gameVersion641) {
				findings.push_back({Severity::Warning, "This is not SimCity 4 version 1.1.641. Live shadows and the "
				                                       "other SCD3D11 features that patch the game stay off."});
			}
		}
	}

	std::string ReleaseVersion(std::string_view json) {
		size_t const key = json.find("\"tag_name\"");
		if (key == std::string_view::npos) return {};
		size_t const open = json.find('"', json.find(':', key));
		size_t const close = open == std::string_view::npos ? open : json.find('"', open + 1);
		if (close == std::string_view::npos) return {};
		std::string_view tag = json.substr(open + 1, close - open - 1);
		if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag.remove_prefix(1);
		return tag.empty() || !std::isdigit(static_cast<unsigned char>(tag[0])) ? std::string() : std::string(tag);
	}

	bool IsNewer(std::string const &version, std::string const &than) {
		return OlderThan(than, version.c_str());
	}

	bool ScreenOffers(std::vector<Size> const &modes, uint32_t width, uint32_t height) {
		return modes.empty() ||
		       std::any_of(modes.begin(), modes.end(), [&](Size mode) { return mode.width == width && mode.height == height; });
	}

	std::string LargestSizes(std::vector<Size> modes) {
		std::sort(modes.begin(), modes.end(), [](Size a, Size b) {
			return uint64_t(a.width) * a.height > uint64_t(b.width) * b.height;
		});
		std::string text;
		for (size_t index = 0; index < modes.size() && index < 8; ++index) {
			text += (text.empty() ? "" : ", ") + std::to_string(modes[index].width) + "x" + std::to_string(modes[index].height);
		}
		return text;
	}

	bool IsWatched(std::string const &path) {
		static constexpr char const *kWatched[] = {
			"scd3d11.dll", "sc4graphicsoptions.dll", "sc4cpuoptions.dll", "sc4renderservices.dll", "sc4_painter.dll",
			"memo.3dcamera.dll", "sc4-moderncamera.dll", "sc4moderncamera.dll", "memo.thumbnail-fix.dll",
			"sc4disablefpslimits.dll", "scvk.dll", "scgl.dll", "sc4fullscreen32bit.dll", "sc4singlecpu.dll",
			"sc4cpupriority.dll",
		};
		std::string const name = FileName(path);
		return std::any_of(std::begin(kWatched), std::end(kWatched), [&](char const *watched) { return name == watched; });
	}

	std::vector<Finding> Diagnose(Environment const &environment) {
		std::vector<Switch> switches;
		std::vector<std::string> strays;
		for (std::string const &argument : environment.arguments) {
			if (argument.empty()) continue;
			if (auto entry = ParseSwitch(argument)) switches.push_back(*entry);
			else strays.push_back(argument);
		}

		std::vector<Finding> findings;
		CheckDriver(environment, switches, findings);
		CheckGraphicsOptions(environment, switches, findings);
		CheckSwitches(environment, switches, strays, findings);
		CheckModules(environment, findings);
		CheckGame(environment, findings);
		std::stable_sort(findings.begin(), findings.end(),
		                 [](Finding const &a, Finding const &b) { return a.severity > b.severity; });
		return findings;
	}
}
