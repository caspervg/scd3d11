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
#include "mini/ini.h"

#include <exception>

#include <windows.h>

namespace nSCD3D11::Settings {
	namespace {
		char Lower(char character) {
			return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
		}

		bool IsSpace(char character) {
			return character == ' ' || character == '\t';
		}

		// The [SCD3D11] section of SCD3D11.ini beside the DLL, read once. Empty
		// when there is no file or it cannot be read.
		mINI::INIMap<std::string> const &IniSection() {
			static mINI::INIMap<std::string> const section = [] {
				mINI::INIStructure ini;
				try {
					std::filesystem::path const path = FileSystem::GetDllIniFilePath();
					if (mINI::INIFile(path).read(ini)) {
						Log(LogCategory::Initialization, "settings read from %s", path.string().c_str());
					}
				} catch (std::exception const &) {
					// A missing module path or an unreadable file leaves the defaults.
				}
				return ini.has("SCD3D11") ? ini.get("SCD3D11") : mINI::INIMap<std::string>();
			}();
			return section;
		}

		// Where a setting was found, for the log; null when it was not.
		char const *Lookup(cIGZCmdLine *commandLine, char const *name, std::string &value) {
			value.clear();
			cRZBaseString switchValue;
			if (commandLine != nullptr && commandLine->IsSwitchPresent(cRZBaseString(name), switchValue, true)) {
				for (char const character : std::string_view(switchValue.ToChar())) value += Lower(character);
				return "command line";
			}
			mINI::INIMap<std::string> const &section = IniSection();
			if (!section.has(name)) return nullptr;
			// mINI keeps an inline comment as part of the value; drop it and the
			// spaces before it.
			for (char const character : section.get(name)) {
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

		bool Flag(cIGZCmdLine *commandLine, char const *name, bool fallback);
		std::string Value(cIGZCmdLine *commandLine, char const *name);

		// Constant-initialised, so readable before Load.
		Values gValues;
	}

	bool ParseFlag(std::string_view value, bool fallback) {
		if (value.empty() || value == "on" || value == "true" || value == "yes" || value == "1") return true;
		if (value == "off" || value == "false" || value == "no" || value == "0") return false;
		return fallback;
	}

	namespace {
		bool Flag(cIGZCmdLine *commandLine, char const *name, bool fallback) {
			std::string value;
			char const *const source = Lookup(commandLine, name, value);
			if (source == nullptr) return fallback;
			NoteSetting(name, value, source);
			return ParseFlag(value, fallback);
		}

		std::string Value(cIGZCmdLine *commandLine, char const *name) {
			std::string value;
			if (char const *const source = Lookup(commandLine, name, value)) NoteSetting(name, value, source);
			return value;
		}
	}

	void Load(cIGZCmdLine *commandLine) {
		Values values;
		values.liveShadows = Flag(commandLine, "LiveShadows", values.liveShadows);
		values.networkShadowMasks = Flag(commandLine, "NetworkShadowMasks", values.networkShadowMasks);
		values.vsync = Flag(commandLine, "VSync", values.vsync);
		values.borderless = Flag(commandLine, "Borderless", values.borderless);
		values.flipModel = Flag(commandLine, "FlipModel", values.flipModel);
		values.defaultAdapter = Value(commandLine, "GPU") == "default";
		values.viewportCull = Flag(commandLine, "ViewportCull", values.viewportCull);
		values.reshade = Flag(commandLine, "ReShade", values.reshade);
		values.drawStats = Flag(commandLine, "DrawStats", values.drawStats);
		values.traceLog = Value(commandLine, "LogLevel") == "trace";
		gValues = values;
		SetDiagnostics(values.drawStats, values.traceLog);
	}

	Values const &Get() {
		return gValues;
	}
} // namespace nSCD3D11::Settings
