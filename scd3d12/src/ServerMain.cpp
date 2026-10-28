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
//                        [--relight <sun>] [--until <frame>] [--scene substitute|check --game <SimCity 4 folder>]
//       Renders a Record=<file> capture offline and saves the chosen frames as PNG. With --scene substitute the
//       server draws the game's models from its own copy of the game's data (SceneLane.h) and writes per frame
//       counts to scene.csv in the output folder. check also compares each such draw with the game's.
//   SCD3D12Server serve --ring <name> --parent <process id> [--log <file>] [--relight <sun>] [--stats on]
//       Draws the game live. SCD3D11 starts it this way with Renderer=server. --stats on logs the frame
//       rate and the CPU and GPU time per frame every second.
//
//   --relight relights every frame with another sun: an hour of the day from 0 to 24, or
//   <azimuth>,<elevation> in degrees (see cGDriver12::Sun).
//   SCD3D12Server compare <a.png> <b.png> [--tolerance <n>]
//       Compares two images channel by channel. Exit code 0 when no channel differs by more than n.

#include "AssetStore.h"
#include "cGDriver12.h"
#include "SceneLane.h"
#include "Serve.h"
#include "wire/ImageFile.h"
#include "wire/WireReplay.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {
	using Sun = nSCD3D12::cGDriver12::Sun;

	float SmoothStep(float edge0, float edge1, float x) {
		float const t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	// The sun's colour and the sky's light for the sun's elevation: white and high at noon, warm and low
	// near the horizon. Below it a dim blue moon takes over, opposite the sun.
	void LightSun(Sun &sun) {
		float const day = SmoothStep(-6.0f, 12.0f, sun.elevation);
		float const warmth = 1.0f - SmoothStep(5.0f, 35.0f, sun.elevation);
		float const strength = 0.68f * SmoothStep(-2.0f, 6.0f, sun.elevation);
		float const white[3]{1.0f, 0.97f, 0.92f}, warm[3]{1.0f, 0.55f, 0.28f};
		float const sky[3]{0.50f, 0.54f, 0.62f}, night[3]{0.09f, 0.11f, 0.21f}, moon[3]{0.12f, 0.14f, 0.22f};
		bool const moonlit = sun.elevation < -2.0f;
		for (int i = 0; i < 3; ++i) {
			sun.color[i] = moonlit ? moon[i] : strength * (white[i] + (warm[i] - white[i]) * warmth);
			sun.ambient[i] = night[i] + (sky[i] - night[i]) * day;
		}
		if (moonlit) {
			sun.azimuth += 180.0f;
			sun.elevation = 35.0f;
		}
	}

	// --relight: an hour of the day, or <azimuth>,<elevation> in degrees. The sun rises in front of the
	// camera at 6, stands at SC4's own sun's left at noon, 70 degrees up, and sets behind the scene at 18.
	bool ParseSun(std::wstring const &text, Sun &sun) {
		wchar_t *end = nullptr;
		float const first = std::wcstof(text.c_str(), &end);
		if (end == text.c_str()) return false;
		if (*end == L',') {
			wchar_t const *const second = end + 1;
			sun.azimuth = first;
			sun.elevation = std::wcstof(second, &end);
			if (end == second || *end != 0) return false;
		} else if (*end == 0 && first >= 0.0f && first <= 24.0f) {
			sun.azimuth = (first - 12.0f) * 15.0f;
			sun.elevation = 70.0f * std::sin((first - 6.0f) / 12.0f * 3.14159265f);
		} else {
			return false;
		}
		LightSun(sun);
		return true;
	}

	struct ReplayOptions {
		std::wstring capture;
		std::wstring out = L".";
		uint32_t every = 0;
		std::set<uint64_t> frames;
		bool last = false;
		bool warp = false;
		bool relight = false;
		Sun sun;
		uint64_t until = UINT64_MAX;
		bool substitute = false;
		bool check = false;
		std::wstring game;
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
			else if (argument == L"--relight" && hasValue) {
				options.relight = true;
				if (!ParseSun(argv[++i], options.sun)) return false;
			}
			else if (argument == L"--until" && hasValue) options.until = _wtoi64(argv[++i]);
			else if (argument == L"--scene" && hasValue) {
				std::wstring const mode = argv[++i];
				if (mode != L"substitute" && mode != L"check") return false;
				options.substitute = true;
				options.check = mode == L"check";
			} else if (argument == L"--game" && hasValue) options.game = argv[++i];
			else if (options.capture.empty()) options.capture = argument;
			else return false;
		}
		return !options.capture.empty() && (!options.substitute || !options.game.empty());
	}

	// Saves the chosen frames as the replay reaches them. Frame numbers count Flush calls from zero, the
	// same way SCDReplay numbers them, so both tools' images of one capture line up.
	struct ReplayFrames : nSCDWire::ReplayHooks {
		nSCD3D12::cGDriver12 &driver;
		ReplayOptions const &options;
		nSCD3D12::SceneLane *scene = nullptr;
		std::FILE *sceneCsv = nullptr;
		uint64_t frame = 0;
		uint64_t saved = 0;

		ReplayFrames(nSCD3D12::cGDriver12 &driver, ReplayOptions const &options) : driver(driver), options(options) {}

		void VideoMode(bool show, uint32_t width, uint32_t height, bool fullscreen) override {
			driver.VideoMode(show, width, height, fullscreen);
		}
		void Vertices(uint32_t name, void const *data, uint32_t bytes) override { driver.Vertices(name, data, bytes); }
		void OwnerBegin(nSCDWire::DrawOwner const &owner) override {
			driver.OwnerBegin(owner);
			if (scene != nullptr) scene->OwnerBegin(owner);
		}
		void OwnerEnd() override {
			driver.OwnerEnd();
			if (scene != nullptr) scene->OwnerEnd();
		}
		void ModelInstance(nSCDWire::ModelInstance const &model) override {
			if (scene != nullptr) scene->ModelInstance(model);
		}
		void DrawBlobs(nSCDWire::BlobKey const &vertices, nSCDWire::BlobKey const &indices) override {
			driver.DrawBlobs(vertices, indices);
		}
		bool Frame() override {
			driver.Frame();
			if (scene != nullptr) scene->EndFrame(frame, sceneCsv);
			if (options.frames.count(frame) != 0 || (options.every != 0 && frame % options.every == 0)) {
				wchar_t name[64];
				swprintf_s(name, L"\\frame%06llu.png", static_cast<unsigned long long>(frame));
				Save(options.out + name);
			}
			return ++frame <= options.until;
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
		if (options.relight) driver.Relight(options.sun);
		if (!driver.Create(options.warp)) {
			std::puts("cannot create a Direct3D 12 device");
			std::fclose(file);
			return 1;
		}
		ReplayFrames hooks(driver, options);
		nSCAssets::AssetStore store;
		std::unique_ptr<nSCD3D12::SceneLane> scene;
		if (options.substitute) {
			store.AddGameData(options.game);
			std::printf("scene: %zu packages, %zu entries\n", store.Files().size(), store.Index().size());
			scene = std::make_unique<nSCD3D12::SceneLane>(driver, store, options.check);
			hooks.scene = scene.get();
			_wfopen_s(&hooks.sceneCsv, (options.out + L"\\scene.csv").c_str(), L"w");
			if (hooks.sceneCsv != nullptr) nSCD3D12::SceneLane::WriteCsvHeader(hooks.sceneCsv);
		}
		auto const start = std::chrono::steady_clock::now();
		nSCDWire::Reader reader([file](void *data, size_t size) { return std::fread(data, 1, size, file) == size; });
		nSCDWire::ReplayStats stats;
		char const *error = nullptr;
		bool const ok = nSCDWire::Replay(reader, driver, hooks, &stats, &error);
		if (options.last) hooks.Save(options.out + L"\\last.png");
		double const seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		std::printf("%s: %llu frames, %llu records, %llu images saved, %.1f s\n", ok ? "done" : error,
		            static_cast<unsigned long long>(stats.frames), static_cast<unsigned long long>(stats.records),
		            static_cast<unsigned long long>(hooks.saved), seconds);
		for (size_t op = 1; op < static_cast<size_t>(nSCDWire::Op::Count); ++op) {
			if (stats.opCounts[op] != 0) {
				std::printf("  %-24s %llu\n", nSCDWire::OpName(static_cast<nSCDWire::Op>(op)),
				            static_cast<unsigned long long>(stats.opCounts[op]));
			}
		}
		if (scene != nullptr) scene->PrintTotals();
		if (hooks.sceneCsv != nullptr) std::fclose(hooks.sceneCsv);
		std::fclose(file);
		return ok ? 0 : 1;
	}

	int RunServeCommand(int argc, wchar_t **argv) {
		std::wstring ring;
		DWORD parent = 0;
		Sun sun;
		bool relight = false;
		bool stats = false;
		for (int i = 2; i + 1 < argc; i += 2) {
			std::wstring const argument = argv[i];
			if (argument == L"--ring") ring = argv[i + 1];
			else if (argument == L"--parent") parent = static_cast<DWORD>(_wtoi(argv[i + 1]));
			else if (argument == L"--relight") relight = ParseSun(argv[i + 1], sun);
			else if (argument == L"--stats") stats = std::wstring(argv[i + 1]) == L"on";
			else if (argument == L"--log") {
				std::FILE *log = nullptr;
				_wfreopen_s(&log, argv[i + 1], L"w", stdout);
			}
		}
		if (ring.empty() || parent == 0) return 2;
		return nSCD3D12::RunServe(ring, parent, relight ? &sun : nullptr, stats);
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
	else if (command == L"serve") result = RunServeCommand(argc, argv);
	else if (command == L"compare") result = RunCompare(argc, argv);
	if (result == 2) {
		std::puts("SCD3D12Server replay <capture.scdw> [--out <folder>] [--every <n>] [--frames <a,b,c>] [--last] [--warp]");
		std::puts("                     [--relight <hour or azimuth,elevation>] [--until <frame>]"
		          " [--scene substitute|check --game <SimCity 4 folder>]");
		std::puts("SCD3D12Server serve --ring <name> --parent <process id> [--log <file>] [--relight <sun>] [--stats on]");
		std::puts("SCD3D12Server compare <a.png> <b.png> [--tolerance <n>]");
	}
	CoUninitialize();
	return result;
}
