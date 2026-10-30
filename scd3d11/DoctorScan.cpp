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

#include "Diagnostics.h"
#include "GameVersion.h"
#include "Settings.h"

#include <GZCLSIDDefs.h>
#include <GZServPtrs.h>
#include <SC4NotificationDialog.h>
#include "cIGZGDriver.h"
#include <cGZDisplayMetrics.h>
#include <cIGZCmdLine.h>
#include <cIGZFrameWork.h>
#include <cIGZGraphicSystem.h>
#include <cIGZGraphicSystem2.h>
#include <cIGZMessage2.h>
#include <cIGZMessageServer2.h>
#include <cIGZMessageTarget2.h>
#include <cRZBaseString.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <wininet.h>

namespace nSCD3D11::Doctor {
	namespace {
		// Set when CheckRequestedScreenMode replaced a full-screen size the screen lacks, for the report.
		std::string gScreenModeWarning;

		std::string Utf8(std::wstring_view text) {
			if (text.empty()) return {};
			int const size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
			                                     nullptr, nullptr);
			std::string result(static_cast<size_t>(size), '\0');
			WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr,
			                    nullptr);
			return result;
		}

		std::wstring ModulePath(HMODULE module) {
			wchar_t path[MAX_PATH]{};
			DWORD const length = GetModuleFileNameW(module, path, MAX_PATH);
			return length == 0 || length == MAX_PATH ? std::wstring() : std::wstring(path, length);
		}

		HMODULE ModuleAt(void const *address) {
			HMODULE module = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			                   static_cast<LPCWSTR>(address), &module);
			return module;
		}

		// The product name and version from a file's version resource.
		Module Describe(std::wstring const &path) {
			Module module{Utf8(path), {}, {}};
			DWORD handle = 0;
			DWORD const size = GetFileVersionInfoSizeW(path.c_str(), &handle);
			if (size == 0) return module;
			std::vector<uint8_t> data(size);
			if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return module;

			VS_FIXEDFILEINFO *fixed = nullptr;
			UINT length = 0;
			if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&fixed), &length) && fixed != nullptr) {
				char version[32];
				sprintf_s(version, "%u.%u.%u.%u", HIWORD(fixed->dwProductVersionMS), LOWORD(fixed->dwProductVersionMS),
				          HIWORD(fixed->dwProductVersionLS), LOWORD(fixed->dwProductVersionLS));
				module.version = version;
			}
			struct Translation {
				WORD language;
				WORD codePage;
			} *translation = nullptr;
			if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void **>(&translation),
			                   &length) && length >= sizeof(Translation)) {
				wchar_t key[64];
				swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\ProductName", translation->language, translation->codePage);
				wchar_t *product = nullptr;
				if (VerQueryValueW(data.data(), key, reinterpret_cast<void **>(&product), &length) && product != nullptr) {
					module.product = Utf8(product);
				}
			}
			return module;
		}

		// Maps the DLL as an image without loading what it imports or running it, then reads its import table.
		bool ImportsImGui(std::filesystem::path const &file) {
			HMODULE const image = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
			if (image == nullptr) return false;
			// The low bits of the handle mark it as a resource mapping.
			auto const base = reinterpret_cast<uint8_t const *>(reinterpret_cast<uintptr_t>(image) & ~uintptr_t(3));
			auto const headers = reinterpret_cast<IMAGE_NT_HEADERS32 const *>(
				base + reinterpret_cast<IMAGE_DOS_HEADER const *>(base)->e_lfanew);
			bool found = false;
			if (headers->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
				IMAGE_DATA_DIRECTORY const &imports = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
				for (auto entry = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR const *>(base + imports.VirtualAddress);
				     imports.VirtualAddress != 0 && entry->Name != 0 && !found; ++entry) {
					found = _stricmp(reinterpret_cast<char const *>(base + entry->Name), "imgui.dll") == 0;
				}
			}
			FreeLibrary(image);
			return found;
		}

		// SC4 loads the DLLs at the top of the game's Plugins folder and of the user's.
		std::vector<std::string> ImGuiPlugins(std::filesystem::path const &userFolder) {
			std::set<std::filesystem::path> folders{
				std::filesystem::path(ModulePath(nullptr)).parent_path().parent_path() / L"Plugins", userFolder / L"Plugins",
				std::filesystem::path(ModulePath(ModuleAt(reinterpret_cast<void *>(&Run)))).parent_path()};
			std::vector<std::string> plugins;
			for (std::filesystem::path const &folder : folders) {
				std::error_code error;
				for (auto const &entry : std::filesystem::directory_iterator(folder, error)) {
					if (_wcsicmp(entry.path().extension().c_str(), L".dll") == 0 && ImportsImGui(entry.path())) {
						plugins.push_back(Utf8(entry.path().filename().wstring()));
					}
				}
			}
			return plugins;
		}

		std::vector<Module> LoadedModules() {
			std::vector<Module> modules;
			HANDLE const snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
			if (snapshot == INVALID_HANDLE_VALUE) return modules;
			MODULEENTRY32W entry{sizeof(entry)};
			for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry)) {
				modules.push_back(Describe(entry.szExePath));
			}
			CloseHandle(snapshot);
			return modules;
		}

		// SC4GraphicsOptions.ini beside the loaded SC4GraphicsOptions.dll, as the plugin reads it.
		std::optional<std::map<std::string, std::string>> ReadGraphicsOptions() {
			HMODULE const module = GetModuleHandleW(L"SC4GraphicsOptions.dll");
			if (module == nullptr) return std::nullopt;
			std::wstring const ini = std::filesystem::path(ModulePath(module)).replace_filename(L"SC4GraphicsOptions.ini");
			std::map<std::string, std::string> values;
			for (wchar_t const *key : {L"EnableIntroVideo", L"PauseGameOnFocusLoss", L"ForceDrawOnScroll", L"Driver",
			                           L"WindowMode", L"ColorDepth", L"WindowWidth", L"WindowHeight"}) {
				wchar_t value[256]{};
				GetPrivateProfileStringW(L"GraphicsOptions", key, L"", value, 256, ini.c_str());
				if (value[0] == L'\0') continue;
				std::string name = Utf8(key);
				for (char &character : name) character = static_cast<char>(tolower(static_cast<unsigned char>(character)));
				values[name] = Utf8(value);
			}
			return values;
		}

		void ReadDriver(Environment &environment) {
			cIGZGraphicSystem2Ptr graphics2;
			cIGZGDriver *const driver = graphics2 ? graphics2->GetGDriver() : nullptr;
			if (driver != nullptr) {
				HMODULE const owner = ModuleAt(*reinterpret_cast<void **>(driver));
				environment.driverIsSCD3D11 = owner != nullptr && owner == ModuleAt(reinterpret_cast<void *>(&Run));
				environment.activeDriverModule = Utf8(ModulePath(owner));
				environment.activeDriverClass = driver->GetGZCLSID();
				environment.activeDriverBuiltIn = owner != nullptr && owner == GetModuleHandleW(nullptr);
			}
			cIGZGraphicSystemPtr graphics;
			if (graphics) {
				cGZDisplayMetrics metrics{};
				graphics->GetGameMetrics(metrics);
				if (metrics.width != 0) environment.gameSize = Size{metrics.width, metrics.height};
				environment.gameFullScreen = graphics->IsFullScreenMode();
			}
		}

		// The sizes Windows offers in 32-bit colour, the same list SCD3D11 gives SC4 for full screen.
		std::vector<Size> DisplayModes() {
			DEVMODEW mode{};
			mode.dmSize = sizeof(mode);
			std::set<std::pair<uint32_t, uint32_t>> sizes;
			for (DWORD index = 0; EnumDisplaySettingsW(nullptr, index, &mode); ++index) {
				if (mode.dmBitsPerPel >= 24) sizes.emplace(mode.dmPelsWidth, mode.dmPelsHeight);
			}
			std::vector<Size> modes;
			for (auto const &[width, height] : sizes) modes.push_back(Size{width, height});
			return modes;
		}

		Environment Collect(cIGZCmdLine *commandLine) {
			Environment environment;
			if (commandLine != nullptr) {
				for (uint32_t index = 1; index < commandLine->argc(); ++index) {
					char const *const argument = commandLine->argv(static_cast<int32_t>(index));
					if (argument != nullptr) environment.arguments.emplace_back(argument);
				}
			}
			environment.rawCommandLine = GetCommandLineA();
			std::filesystem::path userFolder;
			for (std::string const &argument : environment.arguments) {
				if (_strnicmp(argument.c_str(), "-UserDir:", 9) != 0) continue;
				// SC4's arguments are in the ANSI code page, which is what a narrow path expects.
				std::string folder = argument.substr(9);
				std::erase(folder, '"');
				userFolder = folder;
				std::error_code error;
				environment.userDirMissing = !folder.empty() && !std::filesystem::exists(userFolder, error);
			}
			wchar_t *documents = nullptr;
			if (userFolder.empty() && SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents))) {
				userFolder = std::filesystem::path(documents) / L"SimCity 4";
			}
			CoTaskMemFree(documents);

			environment.modules = LoadedModules();
			environment.imguiPlugins = ImGuiPlugins(userFolder);
			std::filesystem::path const apps = std::filesystem::path(ModulePath(nullptr)).parent_path();
			for (wchar_t const *name : {L"dxgi.dll", L"d3d11.dll", L"ddraw.dll", L"d3dimm.dll", L"d3d8.dll", L"d3d9.dll",
			                            L"opengl32.dll", L"imgui.dll"}) {
				std::error_code error;
				if (std::filesystem::exists(apps / name, error)) environment.appsFiles.push_back(Describe(apps / name));
			}

			wchar_t layer[512]{};
			if (GetEnvironmentVariableW(L"__COMPAT_LAYER", layer, 512) < 512) environment.compatibilityLayer = Utf8(layer);
			environment.graphicsOptions = ReadGraphicsOptions();

			environment.displayModes = DisplayModes();
			environment.screenModeWarning = gScreenModeWarning;
			environment.screen = Size{static_cast<uint32_t>(GetSystemMetrics(SM_CXSCREEN)),
			                          static_cast<uint32_t>(GetSystemMetrics(SM_CYSCREEN))};
			environment.gameVersion641 = IsGameVersion641();
			wchar_t windowsFolder[MAX_PATH]{};
			if (GetWindowsDirectoryW(windowsFolder, MAX_PATH) != 0) environment.windowsFolder = Utf8(windowsFolder);
			ReadDriver(environment);
			return environment;
		}

		constexpr uint32_t kSC4MessagePostRegionInit = 0xCBB5BB45;
		constexpr char kLatestReleaseUrl[] = "https://api.github.com/repos/caspervg/scd3d11/releases/latest";
		constexpr char kReleasesPage[] = "https://github.com/caspervg/scd3d11/releases";

		// Shows the doctor's problems in SC4's own dialog when the region view first opens, then offers the
		// releases page when a newer SCD3D11 is out. Players see both in any window mode. It lives as long as
		// the game, so it does not count references.
		class ProblemDialog final : public cIGZMessageTarget2 {
		public:
			bool QueryInterface(uint32_t riid, void **object) override {
				if (riid != GZIID_cIGZUnknown && riid != static_cast<uint32_t>(GZCLSID::kcIGZMessageTarget2)) {
					*object = nullptr;
					return false;
				}
				*object = static_cast<cIGZMessageTarget2 *>(this);
				return true;
			}

			uint32_t AddRef() override { return 1; }
			uint32_t Release() override { return 1; }

			bool DoMessage(cIGZMessage2 *message) override {
				if (message == nullptr || message->GetType() != kSC4MessagePostRegionInit) return true;
				std::string text, notice;
				{
					std::lock_guard<std::mutex> const lock(mutex);
					if (shown) return true;
					shown = true;
					// SC4's dialog shows about five lines without scrolling, so this stays compact.
					for (std::string const &problem : problems) text += problem + "\n";
					notice = update;
				}
				if (!text.empty()) {
					text += "Details in Logs\\SCD3D11.log. DoctorDialog=off hides this.";
					SC4NotificationDialog::ShowDialog(cRZBaseString(text.c_str()), cRZBaseString("SCD3D11"));
				}
				if (!notice.empty() && IsGameVersion641()) {
					// SC4's own two-button dialog. It waits for a click and returns true for the first button.
					using ShowConfirmation = bool(__cdecl *)(cIGZString const &caption, cIGZString const &message,
					                                         cIGZString const *confirmLabel, cIGZString const *cancelLabel);
					cRZBaseString const download("Download"), later("Later");
					if (reinterpret_cast<ShowConfirmation>(0x78DFF0)(cRZBaseString("Update available"),
					                                                 cRZBaseString(notice.c_str()), &download, &later)) {
						ShellExecuteA(nullptr, "open", kReleasesPage, nullptr, nullptr, SW_SHOWNORMAL);
					}
				}
				return true;
			}

			void AddProblem(std::string const &problem) {
				std::lock_guard<std::mutex> const lock(mutex);
				problems.push_back(problem);
			}

			// The update check runs on its own thread and may finish after the region opened. Then only
			// the log has it.
			void SetUpdate(std::string const &notice) {
				std::lock_guard<std::mutex> const lock(mutex);
				update = notice;
			}

		private:
			std::mutex mutex;
			std::vector<std::string> problems;
			std::string update;
			bool shown = false;
		};

		ProblemDialog gProblemDialog;

		// The reply to a GET, or nothing when it fails. WinINet is the HTTP library SC4 itself loads, and it
		// follows the player's Windows proxy settings.
		std::optional<std::string> Fetch(char const *url, std::string const &agent) {
			HINTERNET const session = InternetOpenA(agent.c_str(), INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
			if (session == nullptr) return std::nullopt;
			DWORD timeout = 3000;
			for (DWORD option : {INTERNET_OPTION_CONNECT_TIMEOUT, INTERNET_OPTION_SEND_TIMEOUT, INTERNET_OPTION_RECEIVE_TIMEOUT}) {
				InternetSetOptionA(session, option, &timeout, sizeof(timeout));
			}
			std::optional<std::string> body;
			HINTERNET const request = InternetOpenUrlA(
				session, url, "Accept: application/vnd.github+json\r\n", static_cast<DWORD>(-1),
				INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_UI |
				INTERNET_FLAG_SECURE, 0);
			if (request != nullptr) {
				DWORD status = 0, size = sizeof(status);
				if (HttpQueryInfoA(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &size, nullptr) &&
				    status == 200) {
					std::string text;
					char buffer[4096];
					DWORD read = 0;
					while (text.size() < (1u << 20) && InternetReadFile(request, buffer, sizeof(buffer), &read) && read != 0) {
						text.append(buffer, read);
					}
					body = std::move(text);
				}
				InternetCloseHandle(request);
			}
			InternetCloseHandle(session);
			return body;
		}

		std::string Today() {
			SYSTEMTIME time{};
			GetLocalTime(&time);
			char text[16];
			sprintf_s(text, "%04u-%02u-%02u", time.wYear, time.wMonth, time.wDay);
			return text;
		}

		// Asks GitHub for the latest release at most once a day. SCD3D11-update.txt in the Logs folder
		// keeps the day of the last answer and the version it named. Runs on its own thread, so a slow or
		// missing connection never holds up the game.
		void CheckForUpdate() {
			try {
				std::string const installed = Describe(ModulePath(ModuleAt(reinterpret_cast<void *>(&Run)))).version;
				if (installed.empty() || installed == "0.0.0.0") {
					Log(LogCategory::Doctor, "update check: skipped for a development build");
					return;
				}
				std::filesystem::path const stamp = LogDirectory() / L"SCD3D11-update.txt";
				std::string day, latest;
				std::ifstream(stamp) >> day >> latest;
				if (day != Today() || latest.empty()) {
					std::optional<std::string> const reply = Fetch(kLatestReleaseUrl, "SCD3D11/" + installed);
					latest = reply ? ReleaseVersion(*reply) : std::string();
					if (latest.empty()) {
						Log(LogCategory::Doctor, "update check: GitHub could not be reached, trying again next start");
						return;
					}
					std::ofstream(stamp) << Today() << ' ' << latest << '\n';
				}
				if (!IsNewer(latest, installed)) {
					Log(LogCategory::Doctor, "update check: this is the latest release (%s)", VersionLabel(latest).c_str());
					return;
				}
				Log(LogCategory::Doctor, "update check: SCD3D11 %s is out, you have %s. Get it from %s.",
				    VersionLabel(latest).c_str(), VersionLabel(installed).c_str(), kReleasesPage);
				gProblemDialog.SetUpdate("SCD3D11 " + VersionLabel(latest) + " is out, you have " +
				                         VersionLabel(installed) + ".\nPlease update, as older versions are not supported.");
			} catch (std::exception const &error) {
				Log(LogCategory::Doctor, "update check failed: %s", error.what());
			}
		}

		std::string Label(Module const &module) {
			if (module.product.empty() && module.version.empty()) return "no version info";
			return module.product + (module.product.empty() || module.version.empty() ? "" : " ") + VersionLabel(module.version);
		}

		// The driver, the screen and the DLLs the checks know about, with their versions, so every log
		// says what a player runs. Doctor=on adds every other DLL and setting the checks looked at.
		void LogInventory(Environment const &environment, bool everything) {
			if (everything) {
				Log(LogCategory::Doctor, "command line as Windows passed it: %s", environment.rawCommandLine.c_str());
				if (!environment.compatibilityLayer.empty()) {
					Log(LogCategory::Doctor, "compatibility layers: %s", environment.compatibilityLayer.c_str());
				}
			}
			Log(LogCategory::Doctor, "driver in use: %s", environment.activeDriverModule.empty()
				                                              ? "unknown" : environment.activeDriverModule.c_str());
			if (environment.gameSize) {
				Log(LogCategory::Doctor, "game screen: %ux%u %s", environment.gameSize->width,
				    environment.gameSize->height, environment.gameFullScreen ? "full screen" : "windowed");
			}
			for (Module const &module : environment.modules) {
				if (!everything && !IsWatched(module.path)) continue;
				Log(LogCategory::Doctor, "loaded: %s (%s)", module.path.c_str(), Label(module).c_str());
			}
			for (Module const &file : environment.appsFiles) {
				Log(LogCategory::Doctor, "in Apps: %s (%s)", file.path.c_str(), Label(file).c_str());
			}
			if (everything && environment.graphicsOptions) {
				for (auto const &[key, value] : *environment.graphicsOptions) {
					Log(LogCategory::Doctor, "SC4GraphicsOptions.ini %s=%s", key.c_str(), value.c_str());
				}
			}
		}
	}

	void CheckRequestedScreenMode() {
		cIGZGraphicSystemPtr graphics;
		if (!graphics) return;
		cGZDisplayMetrics requested{};
		graphics->GetGameMetrics(requested);
		bool const fullScreen = graphics->IsFullScreenMode();
		Log(LogCategory::Doctor, "SC4 asks for %ux%u in %u-bit colour, %s", requested.width, requested.height,
		    requested.bitDepth, fullScreen ? "full screen" : "in a window");
		if (!fullScreen) return;

		// A size from -r or SC4GraphicsOptions is the player's choice. Without one, SC4 uses the size it
		// saved last time, 1024x768 at first, where the screen's own size is what a player expects.
		bool sizeGiven = GetModuleHandleW(L"SC4GraphicsOptions.dll") != nullptr;
		cIGZFrameWork *const framework = RZGetFrameWork();
		cIGZCmdLine *const commandLine = framework != nullptr ? framework->CommandLine() : nullptr;
		for (uint32_t index = 1; commandLine != nullptr && index < commandLine->argc(); ++index) {
			char const *const argument = commandLine->argv(static_cast<int32_t>(index));
			sizeGiven |= argument != nullptr && (argument[0] == '-') && (argument[1] == 'r' || argument[1] == 'R') &&
			             (isdigit(static_cast<unsigned char>(argument[2])) || argument[2] == ':');
		}
		std::vector<Size> const modes = DisplayModes();
		bool const offered = requested.bitDepth == 32 && ScreenOffers(modes, requested.width, requested.height);
		if (offered && sizeGiven) return;

		DEVMODEW current{};
		current.dmSize = sizeof(current);
		if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &current)) return;
		cGZDisplayMetrics wanted = requested;
		wanted.width = current.dmPelsWidth;
		wanted.height = current.dmPelsHeight;
		wanted.bitDepth = 32;
		if (wanted.width == requested.width && wanted.height == requested.height && requested.bitDepth == 32) return;
		bool const changed = graphics->PreInitSetDesiredGameResolution(wanted);

		char text[1024];
		if (offered) {
			Log(LogCategory::Doctor, "full screen without a size: %s the screen's own %ux%u instead of SC4's saved %ux%u",
			    changed ? "using" : "could not switch to", wanted.width, wanted.height, requested.width, requested.height);
			return;
		}
		if (changed) {
			sprintf_s(text, "SimCity 4 asked for full screen at %ux%u in %u-bit colour, which this screen does not offer. "
			          "SCD3D11 used the screen's own %ux%u instead, where SC4 would have closed without a message. The "
			          "screen offers %s. Change -r on the command line, or WindowWidth, WindowHeight and ColorDepth in "
			          "SC4GraphicsOptions.ini.", requested.width, requested.height, requested.bitDepth, wanted.width,
			          wanted.height, LargestSizes(modes).c_str());
			Log(LogCategory::Doctor, "full screen at %ux%u is not offered, using %ux%u instead (the report below says more)",
			    requested.width, requested.height, wanted.width, wanted.height);
			gScreenModeWarning = text;
			return;
		}
		sprintf_s(text, "SimCity 4 asks for full screen at %ux%u in %u-bit colour, which this screen does not offer, so "
		          "the game closes without a message. The screen offers %s. Change -r on the command line, or "
		          "WindowWidth, WindowHeight and ColorDepth in SC4GraphicsOptions.ini.", requested.width,
		          requested.height, requested.bitDepth, LargestSizes(modes).c_str());
		Log(LogCategory::Doctor, "problem: %s", text);
		if (Settings::Get().doctorDialog) MessageBoxA(nullptr, text, "SCD3D11", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
	}

	void Run(cIGZCmdLine *commandLine) {
		try {
			Environment const environment = Collect(commandLine);
			LogInventory(environment, Settings::Get().doctor);
			std::vector<Finding> const findings = Diagnose(environment);
			unsigned counts[3]{};
			for (Finding const &finding : findings) {
				static char const *const kLabels[] = {"note", "warning", "problem"};
				unsigned const index = static_cast<unsigned>(finding.severity);
				++counts[index];
				Log(LogCategory::Doctor, "%s: %s", kLabels[index], finding.text.c_str());
				if (finding.severity == Severity::Problem) gProblemDialog.AddProblem(finding.text);
			}
			cIGZMessageServer2Ptr messageServer;
			if (Settings::Get().doctorDialog && (counts[2] != 0 || Settings::Get().updateCheck) && messageServer) {
				messageServer->AddNotification(&gProblemDialog, kSC4MessagePostRegionInit);
			}
			Log(LogCategory::Doctor, "%u problems, %u warnings, %u notes%s", counts[2], counts[1], counts[0],
			    Settings::Get().doctor ? "" : " (Doctor=on also lists every DLL it looked at)");
			if (Settings::Get().updateCheck) std::thread(CheckForUpdate).detach();
		} catch (std::exception const &error) {
			Log(LogCategory::Doctor, "could not finish: %s", error.what());
		}
	}
}
