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

		// Where a setting was found, for the log; null when it was not.
		char const *Lookup(Sources const &sources, char const *name, std::string &value) {
			value.clear();
			cRZBaseString switchValue;
			if (sources.commandLine != nullptr &&
			    sources.commandLine->IsSwitchPresent(cRZBaseString(name), switchValue, true)) {
				for (char const character : std::string_view(switchValue.ToChar())) value += Lower(character);
				return "command line";
			}
			if (sources.ini == nullptr) return nullptr;
			auto const entry = sources.ini->find(name);
			if (entry == sources.ini->not_found()) return nullptr;
			// IniReader keeps an inline comment as part of the value; drop it and
			// the spaces before it.
			for (char const character : entry->second) {
				if (character == ';') break;
				value += Lower(character);
			}
			while (!value.empty() && IsSpace(value.back())) value.pop_back();
			return value.empty() ? nullptr : "SCD3D11.ini";
		}

		// Logs each setting that is set anywhere, so a log shows what a session
		// ran with.
		void NoteSetting(char const *name, std::string const &value, char const *source) {
			Log(LogCategory::Initialization, "setting %s = %s (%s)", name, value.empty() ? "on" : value.c_str(), source);
		}

		bool Flag(Sources const &sources, char const *name, bool fallback);
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
		bool Flag(Sources const &sources, char const *name, bool fallback) {
			std::string value;
			char const *const source = Lookup(sources, name, value);
			if (source == nullptr) return fallback;
			NoteSetting(name, value, source);
			bool flag = fallback;
			return ParseFlag(value, flag) ? flag : fallback;
		}

		std::string Value(Sources const &sources, char const *name) {
			std::string value;
			if (char const *const source = Lookup(sources, name, value)) NoteSetting(name, value, source);
			return value;
		}
	}

	void Load(cIGZCmdLine *commandLine) {
		std::optional<IniSection> const ini = ReadIni();
		Sources const sources{commandLine, ini ? &*ini : nullptr};
		Values values;
		values.liveShadows = Flag(sources, "LiveShadows", values.liveShadows);
		values.networkShadowMasks = Flag(sources, "NetworkShadowMasks", values.networkShadowMasks);
		values.vsync = Flag(sources, "VSync", values.vsync);
		values.borderless = Flag(sources, "Borderless", values.borderless);
		values.flipModel = Flag(sources, "FlipModel", values.flipModel);
		values.defaultAdapter = Value(sources, "GPU") == "default";
		values.viewportCull = Flag(sources, "ViewportCull", values.viewportCull);
		values.reshade = Flag(sources, "ReShade", values.reshade);
		values.drawStats = Flag(sources, "DrawStats", values.drawStats);
		values.traceLog = Value(sources, "LogLevel") == "trace";
		gValues = values;
		SetDiagnostics(values.drawStats, values.traceLog);
	}

	Values const &Get() {
		return gValues;
	}
} // namespace nSCD3D11::Settings
