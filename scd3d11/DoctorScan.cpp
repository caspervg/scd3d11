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

#include <GZServPtrs.h>
#include <cGZDisplayMetrics.h>
#include <cIGZCmdLine.h>
#include <cIGZGraphicSystem.h>
#include <cIGZGraphicSystem2.h>

#include <exception>
#include <filesystem>
#include <set>

#include <windows.h>
#include <tlhelp32.h>

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
				Log(LogCategory::Doctor, "loaded: %s (%s %s)", module.path.c_str(),
				    module.product.empty() ? "no version info" : module.product.c_str(), module.version.c_str());
			}
			for (Module const &file : environment.appsFiles) {
				Log(LogCategory::Doctor, "in Apps: %s (%s %s)", file.path.c_str(),
				    file.product.empty() ? "no version info" : file.product.c_str(), file.version.c_str());
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
			}
			Log(LogCategory::Doctor, "%u problems, %u warnings, %u notes%s", counts[2], counts[1], counts[0],
			    Settings::Get().doctor ? "" : " (Doctor=on also lists every DLL it looked at)");
		} catch (std::exception const &error) {
			Log(LogCategory::Doctor, "could not finish: %s", error.what());
		}
	}
}
