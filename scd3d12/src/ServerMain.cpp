/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// SCD3D12Server: SimCity 4's renderer as a separate 64-bit process.
//
//   SCD3D12Server replay <capture.scdw> [--out <folder>] [--every <n>] [--frames <a,b,c>] [--last] [--warp]
//       Renders a Record=<file> capture offline and saves the chosen frames as PNG.
//   SCD3D12Server compare <a.png> <b.png> [--tolerance <n>]
//       Compares two images channel by channel. Exit code 0 when no channel differs by more than n.

#include "cGDriver12.h"
#include "wire/ImageFile.h"
#include "wire/WireReplay.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace {
	struct ReplayOptions {
		std::wstring capture;
		std::wstring out = L".";
		uint32_t every = 0;
		std::set<uint64_t> frames;
		bool last = false;
		bool warp = false;
	};

	bool ParseReplay(int argc, wchar_t **argv, ReplayOptions &options) {
		for (int i = 2; i < argc; ++i) {
			std::wstring const argument = argv[i];
			bool const hasValue = i + 1 < argc;
			if (argument == L"--out" && hasValue) options.out = argv[++i];
			else if (argument == L"--every" && hasValue) options.every = static_cast<uint32_t>(_wtoi(argv[++i]));
			else if (argument == L"--frames" && hasValue) {
				std::wstring const list = argv[++i];
				for (size_t start = 0; start <= list.size();) {
					size_t const end = list.find(L',', start);
					options.frames.insert(_wtoi64(list.substr(start, end - start).c_str()));
					if (end == std::wstring::npos) break;
					start = end + 1;
				}
			} else if (argument == L"--last") options.last = true;
			else if (argument == L"--warp") options.warp = true;
			else if (options.capture.empty()) options.capture = argument;
			else return false;
		}
		return !options.capture.empty();
	}

	// Saves the chosen frames as the replay reaches them. Frame numbers count Flush calls from zero, the
	// same way SCDReplay numbers them, so both tools' images of one capture line up.
	struct ReplayFrames : nSCDWire::ReplayHooks {
		nSCD3D12::cGDriver12 &driver;
		ReplayOptions const &options;
		uint64_t frame = 0;
		uint64_t saved = 0;

		ReplayFrames(nSCD3D12::cGDriver12 &driver, ReplayOptions const &options) : driver(driver), options(options) {}

		void VideoMode(bool show, uint32_t width, uint32_t height, bool fullscreen) override {
			driver.VideoMode(show, width, height, fullscreen);
		}
		void Vertices(uint32_t name, void const *data, uint32_t bytes) override { driver.Vertices(name, data, bytes); }
		bool Frame() override {
			driver.Frame();
			if (options.frames.count(frame) != 0 || (options.every != 0 && frame % options.every == 0)) {
				wchar_t name[64];
				swprintf_s(name, L"\\frame%06llu.png", static_cast<unsigned long long>(frame));
				Save(options.out + name);
			}
			++frame;
			return true;
		}
		void Save(std::wstring const &path) {
			std::vector<uint8_t> rgba;
			uint32_t width = 0, height = 0;
			if (driver.ReadBack(rgba, width, height) && nSCDWire::SavePng(path.c_str(), width, height, rgba.data())) {
				++saved;
			}
		}
	};

	int RunReplay(int argc, wchar_t **argv) {
		ReplayOptions options;
		if (!ParseReplay(argc, argv, options)) return 2;
		std::FILE *file = nullptr;
		if (_wfopen_s(&file, options.capture.c_str(), L"rb") != 0 || file == nullptr) {
			std::puts("cannot open the capture");
			return 1;
		}
		CreateDirectoryW(options.out.c_str(), nullptr);
		nSCD3D12::cGDriver12 driver;
		if (!driver.Create(options.warp)) {
			std::puts("cannot create a Direct3D 12 device");
			std::fclose(file);
			return 1;
		}
		ReplayFrames hooks(driver, options);
		nSCDWire::Reader reader([file](void *data, size_t size) { return std::fread(data, 1, size, file) == size; });
		nSCDWire::ReplayStats stats;
		char const *error = nullptr;
		bool const ok = nSCDWire::Replay(reader, driver, hooks, &stats, &error);
		if (options.last) hooks.Save(options.out + L"\\last.png");
		std::printf("%s: %llu frames, %llu records, %llu images saved\n", ok ? "done" : error,
		            static_cast<unsigned long long>(stats.frames), static_cast<unsigned long long>(stats.records),
		            static_cast<unsigned long long>(hooks.saved));
		std::fclose(file);
		return ok ? 0 : 1;
	}

	int RunCompare(int argc, wchar_t **argv) {
		if (argc < 4) return 2;
		int tolerance = 0;
		for (int i = 4; i + 1 < argc; ++i) {
			if (std::wstring(argv[i]) == L"--tolerance") tolerance = _wtoi(argv[i + 1]);
		}
		std::vector<uint8_t> a, b;
		uint32_t aw = 0, ah = 0, bw = 0, bh = 0;
		if (!nSCDWire::LoadPng(argv[2], aw, ah, a) || !nSCDWire::LoadPng(argv[3], bw, bh, b)) {
			std::puts("cannot read both images");
			return 2;
		}
		if (aw != bw || ah != bh) {
			std::printf("sizes differ: %ux%u and %ux%u\n", aw, ah, bw, bh);
			return 1;
		}
		uint64_t over = 0;
		int worst = 0;
		for (size_t i = 0; i < a.size(); ++i) {
			if (i % 4 == 3) continue;
			int const difference = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
			if (difference > worst) worst = difference;
			if (difference > tolerance) ++over;
		}
		std::printf("%llu of %zu channels differ by more than %d, worst %d\n", static_cast<unsigned long long>(over),
		            a.size() / 4 * 3, tolerance, worst);
		return over == 0 ? 0 : 1;
	}
}

int wmain(int argc, wchar_t **argv) {
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	int result = 2;
	std::wstring const command = argc > 1 ? argv[1] : L"";
	if (command == L"replay") result = RunReplay(argc, argv);
	else if (command == L"compare") result = RunCompare(argc, argv);
	if (result == 2) {
		std::puts("SCD3D12Server replay <capture.scdw> [--out <folder>] [--every <n>] [--frames <a,b,c>] [--last] [--warp]");
		std::puts("SCD3D12Server compare <a.png> <b.png> [--tolerance <n>]");
	}
	CoUninitialize();
	return result;
}
