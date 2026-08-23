/*
 *  SCGL - a free graphics driver for SimCity 4's SimGL interface
 *  Copyright (C) 2025  Nelson Gomez (nsgomez) <nelson@ngomez.me>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "Diagnostics.h"

#include <cstdarg>
#include <cstdio>
#include <unordered_set>

namespace nSCGL
{
	namespace
	{
		constexpr unsigned int kMessagesPerCategory = 128;
		unsigned int messageCounts[static_cast<unsigned int>(LogCategory::Count)]{};
		std::unordered_set<uint64_t> observed[static_cast<unsigned int>(ObservedCategory::Count)];

		char const* CategoryName(LogCategory category) {
			static char const* names[] = { "init", "caps", "swapchain", "resource", "grid", "unsupported" };
			return names[static_cast<unsigned int>(category)];
		}
	}

	void Log(LogCategory category, char const* format, ...) {
		unsigned int const index = static_cast<unsigned int>(category);
		if (index >= static_cast<unsigned int>(LogCategory::Count)) {
			return;
		}

		unsigned int const count = messageCounts[index]++;
		if (count > kMessagesPerCategory) {
			return;
		}

		char message[1024]{};
		if (count == kMessagesPerCategory) {
			sprintf_s(message, "[SC4D3D11][%s] further messages suppressed\n", CategoryName(category));
		}
		else {
			char detail[896]{};
			va_list arguments;
			va_start(arguments, format);
			vsnprintf_s(detail, sizeof(detail), _TRUNCATE, format, arguments);
			va_end(arguments);
			sprintf_s(message, "[SC4D3D11][%s] %s\n", CategoryName(category), detail);
		}

		OutputDebugStringA(message);
		FILE* file = nullptr;
		if (fopen_s(&file, "SC4D3D11.log", "a") == 0) {
			fputs(message, file);
			fclose(file);
		}
	}

	void LogHRESULT(LogCategory category, char const* operation, HRESULT result) {
		Log(category, "%s failed (HRESULT 0x%08lX)", operation, static_cast<unsigned long>(result));
	}

	void RecordEncountered(ObservedCategory category, uint64_t value) {
#ifndef NDEBUG
		char enabled[2]{};
		if (GetEnvironmentVariableA("SC4D3D11_RECORD_STATES", enabled, sizeof(enabled)) == 0 || enabled[0] == '0') return;

		unsigned int const index = static_cast<unsigned int>(category);
		if (index >= static_cast<unsigned int>(ObservedCategory::Count)) return;
		auto& values = observed[index];
		if (values.size() >= 4096 || !values.insert(value).second) return;

		static char const* names[] = { "render-state", "vertex-format", "texture-format" };
		char message[128]{};
		sprintf_s(message, "%s=0x%016llX\n", names[index], static_cast<unsigned long long>(value));
		FILE* file = nullptr;
		if (fopen_s(&file, "SC4D3D11-states.log", "a") == 0) {
			fputs(message, file);
			fclose(file);
		}
#else
		(void)category;
		(void)value;
#endif
	}
}
