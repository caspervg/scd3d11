/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "Settings.h"

#include "Diagnostics.h"

#include "FileSystem.h"
#include "cIGZCmdLine.h"
#include "cRZBaseString.h"
#include "IniReader.h"
#include "StringViewUtil.h"

#include <exception>
#include <filesystem>
#include <optional>
#include <utility>

#include <windows.h>

namespace nSCD3D11::Settings {
	namespace {
		char Lower(char character) {
			return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
		}

		bool IsSpace(char character) {
			return character == ' ' || character == '\t';
		}

		// Where a setting can come from: SC4's command line, then the [SCD3D11]
		// section of SCD3D11.ini. Either may be missing.
		struct Sources {
			cIGZCmdLine *commandLine = nullptr;
			IniSection const *ini = nullptr;
		};

		// The [SCD3D11] section of SCD3D11.ini beside the DLL.
		std::optional<IniSection> ReadIni() {
			try {
				std::filesystem::path const path = FileSystem::GetDllIniFilePath();
				if (!std::filesystem::exists(path)) return std::nullopt;
				IniReader const reader(path);
				Log(LogCategory::Initialization, "settings read from %s", path.string().c_str());
				return reader.get_section_optional("SCD3D11");
			} catch (std::exception const &error) {
				Log(LogCategory::Initialization, "SCD3D11.ini ignored: %s", error.what());
				return std::nullopt;
			}
		}

		// One source's text for a setting in lower case, or nothing when that
		// source does not set it. A bare switch reads as "".
		std::optional<std::string> FromCommandLine(cIGZCmdLine *commandLine, char const *name) {
			if (commandLine == nullptr) return std::nullopt;
			// SC4's value lookup only matches -Name:value with something after the
			// colon, so a bare -Name needs the other lookup.
			cRZBaseString switchValue;
			if (!commandLine->IsSwitchPresent(cRZBaseString(name), switchValue, true))
				return commandLine->IsSwitchPresent(cRZBaseString(name)) ? std::optional<std::string>("") : std::nullopt;
			std::string value;
			for (char const character : std::string_view(switchValue.ToChar())) value += Lower(character);
			return value;
		}

		std::optional<std::string> FromIni(IniSection const *ini, char const *name) {
			if (ini == nullptr) return std::nullopt;
			auto const entry = ini->find(name);
			if (entry == ini->not_found()) return std::nullopt;
			// IniReader keeps an inline comment, starting with a semicolon or #,
			// as part of the value. Drop it and the spaces before it.
			std::string value;
			for (char const character : entry->second) {
				if (character == ';' || character == '#') break;
				value += Lower(character);
			}
			while (!value.empty() && IsSpace(value.back())) value.pop_back();
			if (value.empty()) return std::nullopt;
			return value;
		}

		// GPU=high-performance, GPU=default or GPU=<n>.
		bool ParseGpu(std::string_view value, GpuChoice &gpu) {
			gpu = GpuChoice{value == "default", 0};
			return gpu.systemDefault || value == "high-performance" || ParseNumber(value, 1, 16, gpu.number);
		}

		// LogLevel=trace or LogLevel=info.
		bool ParseLogLevel(std::string_view value, bool &trace) {
			trace = value == "trace";
			return trace || value == "info";
		}

		bool ParseVertexCacheSegments(std::string_view value, uint32_t &segments) {
			return ParseNumber(value, 1, 16, segments);
		}

		bool ParseVertexCacheSize(std::string_view value, uint32_t &megabytes) {
			return ParseNumber(value, 4, 64, megabytes);
		}

		std::vector<Rejection> gRejected;

		// The setting from the first source whose value parse accepts: the
		// command line, then the INI. A value it rejects is logged and skipped,
		// so a mistyped switch leaves the INI's setting in force.
		template <typename T>
		T Resolve(Sources const &sources, char const *name, bool (*parse)(std::string_view, T &), T fallback) {
			std::pair<char const *, std::optional<std::string>> const candidates[]{
				{"command line", FromCommandLine(sources.commandLine, name)},
				{"SCD3D11.ini", FromIni(sources.ini, name)}
			};
			for (auto const &[source, value] : candidates) {
				if (!value) continue;
				T result{};
				// Only a value that took effect is logged as the setting, so the
				// log shows what the session ran with.
				if (parse(*value, result)) {
					Log(LogCategory::Initialization, "setting %s = %s (%s)", name, value->empty() ? "on" : value->c_str(),
					    source);
					return result;
				}
				Log(LogCategory::Initialization, "setting %s: %s%s%s from the %s is not a valid value, ignored", name,
				    value->empty() ? "a bare switch" : "'", value->c_str(), value->empty() ? "" : "'", source);
				gRejected.push_back({name, *value, source});
			}
			return fallback;
		}


		// Constant-initialised, so readable before Load.
		Values gValues;
	}

	bool ParseFlag(std::string_view value, bool &flag) {
		if (value.empty()) {
			flag = true;
			return true;
		}
		return StringViewUtil::TryParse(value, flag);
	}

	bool ParseNumber(std::string_view value, uint32_t minimum, uint32_t maximum, uint32_t &number) {
		if (value.empty() || value.size() > 9) return false;
		uint32_t result = 0;
		for (char const character : value) {
			if (character < '0' || character > '9') return false;
			result = result * 10 + static_cast<uint32_t>(character - '0');
		}
		if (result < minimum || result > maximum) return false;
		number = result;
		return true;
	}

	void Load(cIGZCmdLine *commandLine) {
		// The arguments as SC4 split them, which is what the switches are matched against.
		if (commandLine != nullptr) {
			std::string arguments;
			for (uint32_t index = 0; index < commandLine->argc(); ++index) {
				char const *const argument = commandLine->argv(static_cast<int32_t>(index));
				arguments += std::string(index == 0 ? "" : " | ") + (argument != nullptr ? argument : "");
			}
			Log(LogCategory::Initialization, "command line as SC4 reads it: %s", arguments.c_str());
		}
		gRejected.clear();
		std::optional<IniSection> const ini = ReadIni();
		Sources const sources{commandLine, ini ? &*ini : nullptr};
		Values values;
		values.liveShadows = Resolve(sources, "LiveShadows", ParseFlag, values.liveShadows);
		values.networkShadowMasks = Resolve(sources, "NetworkShadowMasks", ParseFlag, values.networkShadowMasks);
		values.vsync = Resolve(sources, "VSync", ParseFlag, values.vsync);
		values.borderless = Resolve(sources, "Borderless", ParseFlag, values.borderless);
		// scvk's -b and -FullscreenMode:Borderless, the second SCD3D11's own before v0.3. -Borderless wins
		// when it is given too.
		if (!FromCommandLine(commandLine, "Borderless")) {
			std::optional<std::string> const shortSwitch = FromCommandLine(commandLine, "b");
			if (shortSwitch && ParseFlag(*shortSwitch, values.borderless)) {
				Log(LogCategory::Initialization, "setting Borderless = %s (command line, -b)", values.borderless ? "on" : "off");
			} else if (FromCommandLine(commandLine, "FullscreenMode") == std::optional<std::string>("borderless")) {
				values.borderless = true;
				Log(LogCategory::Initialization, "setting Borderless = on (command line, -FullscreenMode:Borderless)");
			}
		}
		values.flipModel = Resolve(sources, "FlipModel", ParseFlag, values.flipModel);
		values.gpu = Resolve(sources, "GPU", ParseGpu, values.gpu);
		values.viewportCull = Resolve(sources, "ViewportCull", ParseFlag, values.viewportCull);
		values.viewportClamp = Resolve(sources, "ViewportClamp", ParseFlag, values.viewportClamp);
		values.reshade = Resolve(sources, "ReShade", ParseFlag, values.reshade);
		values.drawStats = Resolve(sources, "DrawStats", ParseFlag, values.drawStats);
		values.traceLog = Resolve(sources, "LogLevel", ParseLogLevel, values.traceLog);
		values.doctor = Resolve(sources, "Doctor", ParseFlag, values.doctor);
		values.doctorDialog = Resolve(sources, "DoctorDialog", ParseFlag, values.doctorDialog);
		values.updateCheck = Resolve(sources, "UpdateCheck", ParseFlag, values.updateCheck);
		values.vertexCacheSegments =
			Resolve(sources, "VertexCacheSegments", ParseVertexCacheSegments, values.vertexCacheSegments);
		values.vertexCacheSizeMB = Resolve(sources, "VertexCacheSize", ParseVertexCacheSize, values.vertexCacheSizeMB);
		gValues = values;
		SetDiagnostics(values.drawStats, values.traceLog);
	}

	Values const &Get() {
		return gValues;
	}

	std::vector<Rejection> const &Rejected() {
		return gRejected;
	}
} // namespace nSCD3D11::Settings
