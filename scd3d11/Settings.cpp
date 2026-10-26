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

		// One source's text for a setting, lower-cased; nullopt when that source
		// does not set it. A bare switch reads as "".
		std::optional<std::string> FromCommandLine(cIGZCmdLine *commandLine, char const *name) {
			cRZBaseString switchValue;
			if (commandLine == nullptr || !commandLine->IsSwitchPresent(cRZBaseString(name), switchValue, true))
				return std::nullopt;
			std::string value;
			for (char const character : std::string_view(switchValue.ToChar())) value += Lower(character);
			return value;
		}

		std::optional<std::string> FromIni(IniSection const *ini, char const *name) {
			if (ini == nullptr) return std::nullopt;
			auto const entry = ini->find(name);
			if (entry == ini->not_found()) return std::nullopt;
			// IniReader keeps an inline comment as part of the value; drop it and
			// the spaces before it.
			std::string value;
			for (char const character : entry->second) {
				if (character == ';') break;
				value += Lower(character);
			}
			while (!value.empty() && IsSpace(value.back())) value.pop_back();
			if (value.empty()) return std::nullopt;
			return value;
		}

		// Logs each setting that is set anywhere, so a log shows what a session
		// ran with.
		void NoteSetting(char const *name, std::string const &value, char const *source) {
			Log(LogCategory::Initialization, "setting %s = %s (%s)", name, value.empty() ? "on" : value.c_str(), source);
		}

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
				if (parse(*value, result)) {
					NoteSetting(name, *value, source);
					return result;
				}
				Log(LogCategory::Initialization, "setting %s: '%s' from the %s is not a valid value, ignored", name,
				    value->c_str(), source);
			}
			return fallback;
		}

		std::string Value(Sources const &sources, char const *name);

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

	namespace {
		std::string Value(Sources const &sources, char const *name) {
			if (std::optional<std::string> const value = FromCommandLine(sources.commandLine, name)) {
				NoteSetting(name, *value, "command line");
				return *value;
			}
			if (std::optional<std::string> const value = FromIni(sources.ini, name)) {
				NoteSetting(name, *value, "SCD3D11.ini");
				return *value;
			}
			return {};
		}
	}

	void Load(cIGZCmdLine *commandLine) {
		std::optional<IniSection> const ini = ReadIni();
		Sources const sources{commandLine, ini ? &*ini : nullptr};
		Values values;
		values.liveShadows = Resolve(sources, "LiveShadows", ParseFlag, values.liveShadows);
		values.networkShadowMasks = Resolve(sources, "NetworkShadowMasks", ParseFlag, values.networkShadowMasks);
		values.vsync = Resolve(sources, "VSync", ParseFlag, values.vsync);
		values.borderless = Resolve(sources, "Borderless", ParseFlag, values.borderless);
		values.flipModel = Resolve(sources, "FlipModel", ParseFlag, values.flipModel);
		values.defaultAdapter = Value(sources, "GPU") == "default";
		values.viewportCull = Resolve(sources, "ViewportCull", ParseFlag, values.viewportCull);
		values.reshade = Resolve(sources, "ReShade", ParseFlag, values.reshade);
		values.drawStats = Resolve(sources, "DrawStats", ParseFlag, values.drawStats);
		values.traceLog = Value(sources, "LogLevel") == "trace";
		gValues = values;
		SetDiagnostics(values.drawStats, values.traceLog);
	}

	Values const &Get() {
		return gValues;
	}
} // namespace nSCD3D11::Settings
