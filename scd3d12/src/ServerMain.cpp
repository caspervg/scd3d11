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
//                        [--relight <sun>] [--until <frame>]
//                        [--scene substitute|check|retained|owned [--game <SimCity 4 folder>]]
//                        [--dlss off|dlaa|quality|balanced|performance|ultra] [--frame-generation on|off]
//                        [--present window|composition|surface [--into <window>]] [--fps <n>]
//       Renders a Record=<file> capture offline and saves the chosen frames as PNG. With --scene substitute the
//       server draws the game's models, networks and lot textures from its own copy of the game's data
//       (SceneLane.h) and writes per frame counts to scene.csv in the output folder. check also compares each
//       such draw with the game's. The data comes from the folders the capture names, or from --game. retained
//       substitutes too, keeps the static models and draws them every frame with the game's camera
//       (RetainedScene.h), and saves the motion vectors of each chosen frame as motion<frame>.png.
//       --dlss runs NVIDIA DLSS through Streamline (Dlss.h) on the retained scene's frames: DLAA with the instances
//       drawn jittered, the other modes from a jittered render size copy of each frame (DlssTest.h). The saved
//       frames are then DLSS's. --present shows the replay in a window of its own through one of three kinds of
//       swap chain (--into presents into another process's window instead, given by its handle), --frame-generation
//       adds DLSS frame generation there, and --fps caps the frame rate with Reflex.
//       owned keeps the whole static view (OwnedScene.h) and draws it every frame without SC4's backing store,
//       moved by --jitter <x>,<y> pixels, and saves each chosen frame's city before the UI and its motion as
//       frame<n>-city.png and frame<n>-motion.png. --look <x>,<y> draws the chosen frames' owned view from a camera
//       moved that many pixels, to show what is kept where SC4 does not draw now.
//   SCD3D12Server serve --ring <name> --parent <process id> [--log <file>] [--relight <sun>] [--stats on]
//                       [--scene substitute|check|owned]
//       Draws the game live. SCD3D11 starts it this way with Renderer=server. --stats on logs the frame
//       rate and the CPU and GPU time per frame every second. --scene works as for replay, with the data
//       from the folders the game names, indexed while the game draws on.
//
//   --relight relights every frame with another sun: an hour of the day from 0 to 24, or
//   <azimuth>,<elevation> in degrees (see cGDriver12::Sun).
//   SCD3D12Server compare <a.png> <b.png> [--tolerance <n>]
//       Compares two images channel by channel. Exit code 0 when no channel differs by more than n.

#include "AssetStore.h"
#include "cGDriver12.h"
#include "Dlss.h"
#include "DlssTest.h"
#include "OwnedScene.h"
#include "RetainedScene.h"
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
		bool retained = false;
		bool owned = false;
		float jitter[2]{};
		float look[2]{};
		std::wstring game;
		nSCD3D12::DlssMode dlss = nSCD3D12::DlssMode::Off;
		bool frameGeneration = false;
		bool present = false;
		nSCD3D12::ReplayWindow::Kind window = nSCD3D12::ReplayWindow::Kind::Window;
		HWND into = nullptr;
		uint32_t fps = 0;
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
				if (mode != L"substitute" && mode != L"check" && mode != L"retained" && mode != L"owned") return false;
				options.substitute = true;
				options.check = mode == L"check";
				options.retained = mode == L"retained";
				options.owned = mode == L"owned";
			} else if (argument == L"--game" && hasValue) options.game = argv[++i];
			else if (argument == L"--dlss" && hasValue) {
				if (!nSCD3D12::ParseDlssMode(argv[++i], options.dlss)) return false;
			} else if (argument == L"--frame-generation" && hasValue) {
				options.frameGeneration = std::wstring(argv[++i]) == L"on";
			} else if (argument == L"--present" && hasValue) {
				options.present = true;
				if (!nSCD3D12::ParseReplayWindowKind(argv[++i], options.window)) return false;
			} else if (argument == L"--into" && hasValue) {
				options.into = reinterpret_cast<HWND>(static_cast<uintptr_t>(_wcstoui64(argv[++i], nullptr, 0)));
			} else if (argument == L"--fps" && hasValue) options.fps = static_cast<uint32_t>(_wtoi(argv[++i]));
			else if (argument == L"--look" && hasValue) {
				if (swscanf_s(argv[++i], L"%f,%f", &options.look[0], &options.look[1]) != 2) return false;
			}
			else if (argument == L"--jitter" && hasValue) {
				if (swscanf_s(argv[++i], L"%f,%f", &options.jitter[0], &options.jitter[1]) != 2) return false;
			}
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
		nSCAssets::AssetStore store;
		std::unique_ptr<nSCD3D12::SceneLane> scene;
		std::unique_ptr<nSCD3D12::RetainedScene> retained;
		std::unique_ptr<nSCD3D12::Dlss> dlss;
		std::unique_ptr<nSCD3D12::Downsampler> downsampler;
		std::unique_ptr<nSCD3D12::ReplayWindow> window;
		uint32_t renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
		uint64_t upscaled = 0;
		std::unique_ptr<nSCD3D12::OwnedScene> owned;
		std::FILE *sceneCsv = nullptr;
		uint64_t frame = 0;
		uint64_t saved = 0;

		ReplayFrames(nSCD3D12::cGDriver12 &driver, ReplayOptions const &options) : driver(driver), options(options) {}

		void VideoMode(bool show, uint32_t width, uint32_t height, bool fullscreen) override {
			driver.VideoMode(show, width, height, fullscreen);
			if (retained != nullptr) retained->VideoMode();
			if (owned != nullptr) owned->VideoMode();
		}
		void Vertices(uint32_t name, void const *data, uint32_t bytes) override { driver.Vertices(name, data, bytes); }
		void Resize(uint32_t width, uint32_t height) override { driver.Resize(width, height); }
		void OwnerBegin(nSCDWire::DrawOwner const &owner) override {
			if (retained != nullptr) retained->OwnerBegin(owner);
			if (owned != nullptr) owned->OwnerBegin(owner);
			driver.OwnerBegin(owner);
			if (scene != nullptr) scene->OwnerBegin(owner);
		}
		void OwnerEnd() override {
			driver.OwnerEnd();
			if (scene != nullptr) scene->OwnerEnd();
			if (owned != nullptr) owned->OwnerEnd();
		}
		void Occupant(nSCDWire::OccupantEvent const &event) override {
			if (retained != nullptr) retained->Occupant(event);
			if (owned != nullptr) owned->Occupant(event);
		}
		void ModelInstance(nSCDWire::ModelInstance const &model) override {
			if (scene != nullptr) scene->ModelInstance(model);
		}
		void NetworkPiece(nSCDWire::NetworkPiece const &piece, nSCDWire::NetworkPart const *parts) override {
			if (scene != nullptr) scene->NetworkPiece(piece, parts);
		}
		void CellTexture(nSCDWire::CellTexture const &cell) override {
			if (scene != nullptr) scene->CellTexture(cell);
		}
		// Without --game the store reads the folders the game named. The record comes before the city loads.
		void GameFolders(std::wstring const (&folders)[static_cast<size_t>(nSCDWire::GameFolder::Count)]) override {
			if (!options.substitute || scene != nullptr) return;
			store.AddFolders(folders[0], folders[1], folders[2], folders[3], folders[4]);
			StartScene();
		}
		void StartScene() {
			std::printf("scene: %zu packages, %zu entries\n", store.Files().size(), store.Index().size());
			scene = std::make_unique<nSCD3D12::SceneLane>(driver, store, options.check);
			_wfopen_s(&sceneCsv, (options.out + L"\\scene.csv").c_str(), L"w");
			if (sceneCsv != nullptr) nSCD3D12::SceneLane::WriteCsvHeader(sceneCsv);
			if (options.retained) {
				retained = std::make_unique<nSCD3D12::RetainedScene>(driver, *scene);
				if (Chosen(frame)) retained->KeepMotion();
			}
			if (options.owned) {
				owned = std::make_unique<nSCD3D12::OwnedScene>(driver, *scene);
				owned->SetJitter(options.jitter[0], options.jitter[1]);
				if (Chosen(frame)) owned->SetLook(options.look[0], options.look[1]);
			}
		}
		void DrawBlobs(nSCDWire::BlobKey const &vertices, nSCDWire::BlobKey const &indices) override {
			driver.DrawBlobs(vertices, indices);
		}
		bool Frame() override {
			driver.Frame();
			if (scene != nullptr) scene->EndFrame(frame, sceneCsv);
			bool generate = false;
			ID3D12Resource *const shown = Upscale(generate);
			if (window != nullptr && !window->Present(shown, generate)) return false;
			bool const chosen = Chosen(frame);
			if (chosen) {
				wchar_t name[64];
				swprintf_s(name, L"\\frame%06llu.png", static_cast<unsigned long long>(frame));
				Save(options.out + name, shown);
			}
			if (retained != nullptr) {
				wchar_t name[64];
				swprintf_s(name, L"\\motion%06llu.png", static_cast<unsigned long long>(frame));
				std::wstring const path = options.out + name;
				retained->EndFrame(chosen ? &path : nullptr);
				if (Chosen(frame + 1)) retained->KeepMotion();
			}
			if (owned != nullptr) {
				if (chosen) {
					wchar_t name[64];
					swprintf_s(name, L"\\frame%06llu-", static_cast<unsigned long long>(frame));
					owned->SaveInputs(options.out + name);
				}
				owned->EndFrame();
				// Only the chosen frames look elsewhere.
				bool const next = Chosen(frame + 1);
				owned->SetLook(next ? options.look[0] : 0.0f, next ? options.look[1] : 0.0f);
			}
			if (dlss != nullptr) dlss->BeginFrame(frame + 1);
			return ++frame <= options.until;
		}

		// The frame the replay shows: DLSS's from the retained scene's frame once there is one, else the driver's.
		// generate says whether the frame has the inputs frame generation needs.
		ID3D12Resource *Upscale(bool &generate) {
			ID3D12Resource *const output = driver.Output();
			nSCD3D12::UpscalerInputs inputs;
			if (dlss == nullptr || output == nullptr || retained == nullptr || !retained->UpscalerCamera(inputs)) {
				return output;
			}
			D3D12_RESOURCE_DESC const description = output->GetDesc();
			inputs.outputWidth = static_cast<uint32_t>(description.Width);
			inputs.outputHeight = description.Height;
			if (inputs.outputWidth != outputWidth || inputs.outputHeight != outputHeight) {
				outputWidth = inputs.outputWidth;
				outputHeight = inputs.outputHeight;
				dlss->RenderSize(outputWidth, outputHeight, renderWidth, renderHeight);
				std::printf("dlss: %ux%u from %ux%u\n", outputWidth, outputHeight, renderWidth, renderHeight);
				inputs.reset = true;
			}
			inputs.renderWidth = renderWidth;
			inputs.renderHeight = renderHeight;
			inputs.color = output;
			inputs.depth = driver.DepthBuffer();
			generate = true;
			++upscaled;
			bool const dlaa = options.dlss == nSCD3D12::DlssMode::Dlaa;
			if (options.dlss == nSCD3D12::DlssMode::Off || dlaa) {
				ID3D12Resource *const shown = dlss->Evaluate(inputs);
				// DLAA jitters the instances the retained scene draws next.
				if (dlaa) {
					float x, y;
					dlss->NextJitter(x, y);
					retained->SetJitter(x, y);
				}
				return shown;
			}
			// The retained scene draws at the output size, so a jittered copy at the render size stands in.
			if (downsampler == nullptr) downsampler = std::make_unique<nSCD3D12::Downsampler>(driver.Device());
			if (!downsampler->Resize(renderWidth, renderHeight)) return output;
			dlss->NextJitter(inputs.jitter[0], inputs.jitter[1]);
			ID3D12Resource *const depth = inputs.depth;
			inputs.color = downsampler->Color();
			inputs.depth = downsampler->Depth();
			inputs.depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			return dlss->Evaluate(inputs, [&](ID3D12GraphicsCommandList *list) {
				downsampler->Record(list, output, depth, inputs.jitter);
			});
		}
		bool Chosen(uint64_t number) const {
			return options.frames.count(number) != 0 || (options.every != 0 && number % options.every == 0);
		}
		void Save(std::wstring const &path, ID3D12Resource *texture = nullptr) {
			std::vector<uint8_t> rgba;
			uint32_t width = 0, height = 0;
			if (driver.ReadBack(rgba, width, height, texture) && nSCDWire::SavePng(path.c_str(), width, height, rgba.data())) {
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
		// Streamline starts before the device.
		std::unique_ptr<nSCD3D12::Dlss> dlss = nSCD3D12::Dlss::Start(options.dlss, options.frameGeneration);
		if (!driver.Create(options.warp)) {
			std::puts("cannot create a Direct3D 12 device");
			std::fclose(file);
			return 1;
		}
		if (dlss != nullptr && !dlss->SetDevice(driver.Device(), driver.Queue())) {
			std::puts("dlss: not available on this device, drawing without it");
			dlss.reset();
		}
		ReplayFrames hooks(driver, options);
		if (dlss != nullptr) {
			dlss->LimitFrameRate(options.fps);
			dlss->BeginFrame(0);
		}
		hooks.dlss = std::move(dlss);
		if (options.present) {
			hooks.window = std::make_unique<nSCD3D12::ReplayWindow>(options.window, options.into, driver.Device(),
			                                                        driver.Queue(), hooks.dlss.get());
		}
		if (options.substitute && !options.game.empty()) {
			hooks.store.AddGameData(options.game);
			hooks.StartScene();
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
		if (hooks.scene != nullptr) hooks.scene->PrintTotals();
		if (hooks.retained != nullptr) hooks.retained->PrintTotals();
		if (hooks.owned != nullptr) hooks.owned->PrintTotals();
		if (hooks.scene == nullptr && options.substitute) std::puts("scene: the capture names no game folders, pass --game");
		if (hooks.dlss != nullptr) {
			double milliseconds = 0.0;
			uint32_t frames = 0;
			hooks.dlss->TakeGpuTime(milliseconds, frames);
			std::printf("dlss: %llu frames upscaled, the DLSS pass took %.3f ms of GPU per frame over %u timed\n",
						static_cast<unsigned long long>(hooks.upscaled), frames != 0 ? milliseconds / frames : 0.0,
						frames);
		}
		if (hooks.window != nullptr) hooks.window->PrintTotals();
		// Streamline goes before the device it was handed.
		hooks.window.reset();
		hooks.downsampler.reset();
		hooks.dlss.reset();
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
		nSCD3D12::SceneMode scene = nSCD3D12::SceneMode::Off;
		for (int i = 2; i + 1 < argc; i += 2) {
			std::wstring const argument = argv[i];
			if (argument == L"--ring") ring = argv[i + 1];
			else if (argument == L"--parent") parent = static_cast<DWORD>(_wtoi(argv[i + 1]));
			else if (argument == L"--relight") relight = ParseSun(argv[i + 1], sun);
			else if (argument == L"--stats") stats = std::wstring(argv[i + 1]) == L"on";
			else if (argument == L"--scene") {
				std::wstring const mode = argv[i + 1];
				if (mode == L"substitute") scene = nSCD3D12::SceneMode::Substitute;
				else if (mode == L"check") scene = nSCD3D12::SceneMode::Check;
				else if (mode == L"owned") scene = nSCD3D12::SceneMode::Owned;
			}
			else if (argument == L"--log") {
				std::FILE *log = nullptr;
				_wfreopen_s(&log, argv[i + 1], L"w", stdout);
			}
		}
		if (ring.empty() || parent == 0) return 2;
		return nSCD3D12::RunServe(ring, parent, relight ? &sun : nullptr, stats, scene);
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
		          " [--scene substitute|check|retained|owned [--game <SimCity 4 folder>]]");
		std::puts("                     [--dlss off|dlaa|quality|balanced|performance|ultra] [--frame-generation on|off]"
		          " [--present window|composition|surface [--into <window>]] [--fps <n>]");
		std::puts("SCD3D12Server serve --ring <name> --parent <process id> [--log <file>] [--relight <sun>] [--stats on]"
		          " [--scene substitute|check|owned]");
		std::puts("SCD3D12Server compare <a.png> <b.png> [--tolerance <n>]");
	}
	CoUninitialize();
	return result;
}
