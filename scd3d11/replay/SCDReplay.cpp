/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// Replays an SCDW capture (Record=<file>) into SCD3D11's own Direct3D 11 driver, outside the game, and saves
// chosen frames as PNG. These are the reference images other renderers of the same capture are compared to.
//
//   SCDReplay <capture.scdw> [--out <folder>] [--every <n>] [--frames <a,b,c>] [--last] [--rewrite <file>]
//
// --rewrite records the replay again through cGDriverWire, as the game would have recorded it with this
// build. Replaying that file must give the same images, which checks what the recorder leaves out.

#include "cGDriver.h"
#include "cGDriverWire.h"
#include "SCD3D11Service.h"
#include "wire/ImageFile.h"
#include "wire/WireReplay.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
	struct Options {
		std::wstring capture;
		std::wstring out = L".";
		uint32_t every = 0;
		std::set<uint64_t> frames;
		bool last = false;
		std::wstring rewrite;
	};

	struct State {
		Options options;
		uint64_t frame = 0;
		uint64_t saved = 0;
		std::vector<uint8_t> lastPixels;
		uint32_t lastWidth = 0;
		uint32_t lastHeight = 0;
	};

	bool Wanted(State const &state) {
		Options const &o = state.options;
		return o.frames.count(state.frame) != 0 || (o.every != 0 && state.frame % o.every == 0) || o.last;
	}

	void __stdcall OnFrame(SCD3D11FrameContext const *context, void *user) {
		auto &state = *static_cast<State *>(user);
		if (context->event != SCD3D11_EVENT_RENDER || !Wanted(state)) return;
		ComPtr<ID3D11Resource> resource;
		context->renderTargetView->GetResource(&resource);
		ComPtr<ID3D11Texture2D> texture;
		if (FAILED(resource.As(&texture))) return;
		D3D11_TEXTURE2D_DESC description{};
		texture->GetDesc(&description);
		description.Usage = D3D11_USAGE_STAGING;
		description.BindFlags = 0;
		description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		description.MiscFlags = 0;
		ComPtr<ID3D11Texture2D> staging;
		if (FAILED(context->device->CreateTexture2D(&description, nullptr, &staging))) return;
		context->context->CopyResource(staging.Get(), texture.Get());
		D3D11_MAPPED_SUBRESOURCE mapping{};
		if (FAILED(context->context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapping))) return;
		state.lastWidth = description.Width;
		state.lastHeight = description.Height;
		state.lastPixels.resize(static_cast<size_t>(description.Width) * description.Height * 4);
		for (UINT y = 0; y < description.Height; ++y) {
			std::memcpy(state.lastPixels.data() + static_cast<size_t>(y) * description.Width * 4,
			            static_cast<uint8_t const *>(mapping.pData) + static_cast<size_t>(y) * mapping.RowPitch,
			            static_cast<size_t>(description.Width) * 4);
		}
		context->context->Unmap(staging.Get(), 0);
		bool const explicitFrame = state.options.frames.count(state.frame) != 0 ||
		                           (state.options.every != 0 && state.frame % state.options.every == 0);
		if (explicitFrame) {
			wchar_t name[64];
			swprintf_s(name, L"\\frame%06llu.png", static_cast<unsigned long long>(state.frame));
			if (nSCDWire::SavePng((state.options.out + name).c_str(), description.Width, description.Height,
			                      state.lastPixels.data())) {
				++state.saved;
			}
		}
	}

	struct Hooks : nSCD3D11::cGDriverReplayHooks {
		State &state;
		template<typename Driver>
		Hooks(Driver &driver, State &state) : cGDriverReplayHooks(driver), state(state) {}
		bool Frame() override {
			++state.frame;
			return true;
		}
	};

	bool ParseOptions(int argc, wchar_t **argv, Options &options) {
		for (int i = 1; i < argc; ++i) {
			std::wstring const argument = argv[i];
			bool const hasValue = i + 1 < argc;
			if (argument == L"--out" && hasValue) options.out = argv[++i];
			else if (argument == L"--every" && hasValue) options.every = static_cast<uint32_t>(_wtoi(argv[++i]));
			else if (argument == L"--frames" && hasValue) {
				std::wstring list = argv[++i];
				for (size_t start = 0; start <= list.size();) {
					size_t const end = list.find(L',', start);
					options.frames.insert(_wtoi64(list.substr(start, end - start).c_str()));
					if (end == std::wstring::npos) break;
					start = end + 1;
				}
			} else if (argument == L"--last") options.last = true;
			else if (argument == L"--rewrite" && hasValue) options.rewrite = argv[++i];
			else if (options.capture.empty()) options.capture = argument;
			else return false;
		}
		return !options.capture.empty();
	}
}

int wmain(int argc, wchar_t **argv) {
	State state;
	if (!ParseOptions(argc, argv, state.options)) {
		std::puts("SCDReplay <capture.scdw> [--out <folder>] [--every <n>] [--frames <a,b,c>] [--last] "
		          "[--rewrite <file>]");
		return 2;
	}
	std::FILE *file = nullptr;
	if (_wfopen_s(&file, state.options.capture.c_str(), L"rb") != 0 || file == nullptr) {
		std::puts("cannot open the capture");
		return 1;
	}
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	CreateDirectoryW(state.options.out.c_str(), nullptr);

	auto *const driver = new nSCD3D11::cGDriver();
	driver->AddRef();
	SCD3D11RegisterFrameCallback(OnFrame, &state);
	nSCDWire::Reader reader([file](void *data, size_t size) { return std::fread(data, 1, size, file) == size; });
	nSCDWire::ReplayStats stats;
	char const *error = nullptr;
	bool ok = false;
	if (state.options.rewrite.empty()) {
		Hooks hooks(*driver, state);
		ok = nSCDWire::Replay(reader, *driver, hooks, &stats, &error);
	} else {
		std::FILE *output = nullptr;
		if (_wfopen_s(&output, state.options.rewrite.c_str(), L"wb") != 0 || output == nullptr) {
			std::puts("cannot write the rewritten capture");
			return 1;
		}
		auto *const wire = new nSCD3D11::cGDriverWire(driver, [output](void const *data, size_t size) {
			return std::fwrite(data, 1, size, output) == size;
		});
		wire->AddRef();
		Hooks hooks(*wire, state);
		ok = nSCDWire::Replay(reader, *wire, hooks, &stats, &error);
		wire->Release();
		std::fclose(output);
	}
	SCD3D11UnregisterFrameCallback(OnFrame, &state);

	if (state.options.last && !state.lastPixels.empty()) {
		std::wstring const path = state.options.out + L"\\last.png";
		if (nSCDWire::SavePng(path.c_str(), state.lastWidth, state.lastHeight, state.lastPixels.data())) ++state.saved;
	}
	std::printf("%s: %llu frames, %llu records, %llu images saved\n", ok ? "done" : error,
	            static_cast<unsigned long long>(stats.frames), static_cast<unsigned long long>(stats.records),
	            static_cast<unsigned long long>(state.saved));
	for (size_t op = 1; op < static_cast<size_t>(nSCDWire::Op::Count); ++op) {
		if (stats.opCounts[op] != 0) {
			std::printf("  %-24s %llu\n", nSCDWire::OpName(static_cast<nSCDWire::Op>(op)),
			            static_cast<unsigned long long>(stats.opCounts[op]));
		}
	}
	driver->Release();
	std::fclose(file);
	CoUninitialize();
	return ok ? 0 : 1;
}
