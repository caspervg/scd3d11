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
#include "mini/ini.h"

#include <exception>
#include <mutex>
#include <set>

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
		char const *Lookup(char const *name, std::string &value) {
			value.clear();
			if (FindSwitch(GetCommandLineA(), name, value)) return "command line";
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

		// Logs each setting that is set anywhere once, so a log shows what a
		// session ran with.
		void NoteSetting(char const *name, std::string const &value, char const *source) {
			static std::mutex mutex;
			static std::set<std::string> logged;
			std::lock_guard<std::mutex> const lock(mutex);
			if (!logged.insert(name).second) return;
			Log(LogCategory::Initialization, "setting %s = %s (%s)", name, value.empty() ? "on" : value.c_str(), source);
		}
	}

	bool FindSwitch(std::string_view commandLine, std::string_view name, std::string &value) {
		bool found = false;
		for (size_t index = 0; index + name.size() < commandLine.size(); ++index) {
			if (commandLine[index] != '-' || (index > 0 && !IsSpace(commandLine[index - 1]))) continue;
			size_t const end = index + 1 + name.size();
			bool same = true;
			for (size_t offset = 0; offset < name.size() && same; ++offset)
				same = Lower(commandLine[index + 1 + offset]) == Lower(name[offset]);
			if (!same || (end < commandLine.size() && commandLine[end] != ':' && !IsSpace(commandLine[end]))) continue;
			found = true;
			value.clear();
			if (end < commandLine.size() && commandLine[end] == ':') {
				for (size_t cursor = end + 1; cursor < commandLine.size() && !IsSpace(commandLine[cursor]); ++cursor)
					if (commandLine[cursor] != '"') value += Lower(commandLine[cursor]);
			}
		}
		return found;
	}

	bool ParseFlag(std::string_view value, bool fallback) {
		if (value.empty() || value == "on" || value == "true" || value == "yes" || value == "1") return true;
		if (value == "off" || value == "false" || value == "no" || value == "0") return false;
		return fallback;
	}

	bool Flag(char const *name, bool fallback) {
		std::string value;
		char const *const source = Lookup(name, value);
		if (source == nullptr) return fallback;
		NoteSetting(name, value, source);
		return ParseFlag(value, fallback);
	}

	std::string Value(char const *name) {
		std::string value;
		if (char const *const source = Lookup(name, value)) NoteSetting(name, value, source);
		return value;
	}
} // namespace nSCD3D11::Settings
