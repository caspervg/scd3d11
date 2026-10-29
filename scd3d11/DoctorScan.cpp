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
#include <cIGZGraphicSystem.h>
#include <cIGZGraphicSystem2.h>
#include <cIGZMessage2.h>
#include <cIGZMessageServer2.h>
#include <cIGZMessageTarget2.h>
#include <cRZBaseString.h>

#include <exception>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

#include <windows.h>
#include <tlhelp32.h>
#include <wininet.h>

namespace nSCD3D11::Doctor {
	namespace {
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
			}
			cIGZGraphicSystemPtr graphics;
			if (graphics) {
				cGZDisplayMetrics metrics{};
				environment.activeDriverClass = driver->GetGZCLSID();
				environment.activeDriverBuiltIn = owner != nullptr && owner == GetModuleHandleW(nullptr);
				graphics->GetGameMetrics(metrics);
				if (metrics.width != 0) environment.gameSize = Size{metrics.width, metrics.height};
				environment.gameFullScreen = graphics->IsFullScreenMode();
			}
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
			for (std::string const &argument : environment.arguments) {
				if (_strnicmp(argument.c_str(), "-UserDir:", 9) != 0) continue;
				// SC4's arguments are in the ANSI code page, which is what a narrow path expects.
				std::string folder = argument.substr(9);
				std::erase(folder, '"');
				std::error_code error;
				environment.userDirMissing = !folder.empty() &&
				                             !std::filesystem::exists(std::filesystem::path(folder), error);
			}

			environment.modules = LoadedModules();
			std::filesystem::path const apps = std::filesystem::path(ModulePath(nullptr)).parent_path();
			for (wchar_t const *name : {L"dxgi.dll", L"d3d11.dll", L"ddraw.dll", L"d3dimm.dll", L"d3d8.dll", L"d3d9.dll",
			                            L"opengl32.dll", L"imgui.dll"}) {
				std::error_code error;
				if (std::filesystem::exists(apps / name, error)) environment.appsFiles.push_back(Describe(apps / name));
			}

			wchar_t layer[512]{};
			if (GetEnvironmentVariableW(L"__COMPAT_LAYER", layer, 512) < 512) environment.compatibilityLayer = Utf8(layer);
			environment.graphicsOptions = ReadGraphicsOptions();

			DEVMODEW mode{};
			mode.dmSize = sizeof(mode);
			std::set<std::pair<uint32_t, uint32_t>> sizes;
			for (DWORD index = 0; EnumDisplaySettingsW(nullptr, index, &mode); ++index) {
				if (mode.dmBitsPerPel >= 24) sizes.emplace(mode.dmPelsWidth, mode.dmPelsHeight);
			}
			for (auto const &[width, height] : sizes) environment.displayModes.push_back(Size{width, height});
			environment.screen = Size{static_cast<uint32_t>(GetSystemMetrics(SM_CXSCREEN)),
			                          static_cast<uint32_t>(GetSystemMetrics(SM_CYSCREEN))};
			environment.gameVersion641 = IsGameVersion641();
			ReadDriver(environment);
			return environment;
		}

		constexpr uint32_t kSC4MessagePostRegionInit = 0xCBB5BB45;

		// Shows the doctor's problems in SC4's own dialog when the region view first opens. Players see it
		// in any window mode, and the game is not held up while it waits. It lives as long as the game, so
		// it does not count references.
		class ProblemDialog final : public cIGZMessageTarget2 {
		public:
			bool QueryInterface(uint32_t riid, void **object) override {
			wchar_t windowsFolder[MAX_PATH]{};
			if (GetWindowsDirectoryW(windowsFolder, MAX_PATH) != 0) environment.windowsFolder = Utf8(windowsFolder);
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
				std::string text;
				{
					std::lock_guard<std::mutex> const lock(mutex);
					if (shown || (problems.empty() && update.empty())) return true;
					shown = true;
					// SC4's dialog shows about five lines without scrolling, so this stays compact.
					for (std::string const &problem : problems) text += problem + "\n";
					if (!update.empty()) text += update + "\n";
				}
				text += "Details in Logs\\SCD3D11.log. DoctorDialog=off hides this.";
				SC4NotificationDialog::ShowDialog(cRZBaseString(text.c_str()), cRZBaseString("SCD3D11"));
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

		constexpr char kLatestReleaseUrl[] = "https://api.github.com/repos/caspervg/scd3d11/releases/latest";
		constexpr char kReleasesPage[] = "https://github.com/caspervg/scd3d11/releases";

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
					Log(LogCategory::Doctor, "update check: this is the latest release (%s)", latest.c_str());
					return;
				}
				// Version resources have four parts and release tags three.
				std::string shown = installed;
				if (std::count(shown.begin(), shown.end(), '.') == 3 && shown.ends_with(".0")) shown.resize(shown.size() - 2);
				std::string const notice = "SCD3D11 " + latest + " is out, this is " + shown + ". Get it from " +
				                           kReleasesPage + ".";
				Log(LogCategory::Doctor, "update check: %s", notice.c_str());
				gProblemDialog.SetUpdate(notice);
			} catch (std::exception const &error) {
				Log(LogCategory::Doctor, "update check failed: %s", error.what());
			}
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
		std::string Label(Module const &module) {
			if (module.product.empty() && module.version.empty()) return "no version info";
			return module.product + (module.product.empty() || module.version.empty() ? "" : " ") + module.version;
		}

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
