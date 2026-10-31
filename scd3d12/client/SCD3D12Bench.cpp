/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// What SCD3D12.dll costs SimCity 4's render thread. Plays a Record=<file> capture into a headless cGDriverClient
// alone, then through cGDriverWire into one, writing the stream into a shared ring that another thread empties as
// the server would. The difference is the recording.
//
//   SCD3D12Bench <capture.scdw> [--client] [--wire] [--save <stream file>]
//
// Without --client or --wire it runs both. --save writes the stream the wire run made to a file, to compare
// writers byte for byte.

#include "cGDriverClient.h"
#include "cGDriverWire.h"
#include "wire/SharedRing.h"
#include "wire/WireReplay.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
	struct Times {
		double wall = 0.0, thread = 0.0;
		uint64_t records = 0, frames = 0;
	};

	double ThreadSeconds() {
		FILETIME created, exited, kernel, user;
		GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
		auto const ticks = [](FILETIME const &t) { return static_cast<uint64_t>(t.dwHighDateTime) << 32 | t.dwLowDateTime; };
		return static_cast<double>(ticks(kernel) + ticks(user)) / 1.0e7;
	}

	bool Run(wchar_t const *capture, bool wire, std::wstring const &save, Times &times) {
		std::FILE *file = nullptr;
		if (_wfopen_s(&file, capture, L"rb") != 0 || file == nullptr) return false;
		auto *const client = new nSCD3D12::cGDriverClient();
		client->SetHeadless(true);
		client->AddRef();

		nSCDWire::SharedRing ring;
		std::thread drain;
		std::FILE *saved = nullptr;
		nSCD3D11::cGDriverWire *recorder = nullptr;
		if (wire) {
			std::wstring const name = L"Local\\SCD3D12Bench-" + std::to_wstring(GetCurrentProcessId());
			if (!ring.Create(name, 64ull << 20)) return false;
			if (!save.empty()) _wfopen_s(&saved, save.c_str(), L"wb");
			// Empties the ring as the server does, keeping the bytes only when asked to.
			drain = std::thread([name, saved] {
				nSCDWire::SharedRing side;
				if (!side.Open(name)) return;
				std::vector<uint8_t> chunk(1u << 16);
				for (;;) {
					nSCDWire::RingControl *const control = side.Control();
					uint64_t const available = control->written.load() - control->read.load();
					size_t const size = available >= chunk.size() ? chunk.size() : static_cast<size_t>(available);
					if (size == 0) {
						// Closed after its last bytes were published, so look once more once it says so.
						if (control->closed.load() != 0 && control->written.load() == control->read.load()) return;
						Sleep(0);
						continue;
					}
					if (!side.Read(chunk.data(), size, [] { return true; })) return;
					if (saved != nullptr) std::fwrite(chunk.data(), 1, size, saved);
				}
			});
			recorder = new nSCD3D11::cGDriverWire(client, std::make_unique<nSCDWire::RingOutput>(
				                                      ring, [] { return true; }, [] {}));
			recorder->AddRef();
		}
		cIGZGDriver &target = recorder != nullptr ? static_cast<cIGZGDriver &>(*recorder) : *client;
		nSCDWire::Reader reader([file](void *data, size_t size) { return std::fread(data, 1, size, file) == size; });
		nSCDWire::ReplayStats stats;
		char const *error = nullptr;
		LARGE_INTEGER frequency, start, end;
		QueryPerformanceFrequency(&frequency);
		double const threadStart = ThreadSeconds();
		QueryPerformanceCounter(&start);
		bool ok;
		if (recorder != nullptr) {
			nSCD3D11::cGDriverReplayHooks hooks(*recorder);
			ok = nSCDWire::Replay(reader, target, hooks, &stats, &error);
		} else {
			nSCD3D11::cGDriverReplayHooks hooks(*client);
			ok = nSCDWire::Replay(reader, target, hooks, &stats, &error);
		}
		QueryPerformanceCounter(&end);
		times.thread = ThreadSeconds() - threadStart;
		times.wall = static_cast<double>(end.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);
		times.records = stats.records;
		times.frames = stats.frames;
		if (recorder != nullptr) {
			recorder->Release();
			ring.Close();
			drain.join();
			if (saved != nullptr) std::fclose(saved);
		}
		client->Release();
		std::fclose(file);
		if (!ok) std::printf("replay stopped: %s\n", error != nullptr ? error : "?");
		return ok;
	}
}

int wmain(int argc, wchar_t **argv) {
	if (argc < 2) {
		std::puts("SCD3D12Bench <capture.scdw> [--client] [--wire] [--save <stream file>]");
		return 2;
	}
	bool client = false, wire = false;
	std::wstring save;
	for (int i = 2; i < argc; ++i) {
		std::wstring const argument = argv[i];
		if (argument == L"--client") client = true;
		else if (argument == L"--wire") wire = true;
		else if (argument == L"--save" && i + 1 < argc) save = argv[++i];
	}
	if (!client && !wire) client = wire = true;
	Times alone, recorded;
	if (client) {
		if (!Run(argv[1], false, {}, alone)) return 1;
		std::printf("client alone: %.2f s, render thread %.2f s CPU, %llu records, %llu frames\n", alone.wall,
		            alone.thread, static_cast<unsigned long long>(alone.records), static_cast<unsigned long long>(alone.frames));
	}
	if (wire) {
		if (!Run(argv[1], true, save, recorded)) return 1;
		std::printf("with recording: %.2f s, render thread %.2f s CPU\n", recorded.wall, recorded.thread);
	}
	if (client && wire) {
		std::printf("recording: %.2f s CPU, %.2f us per record, %.3f ms per frame\n", recorded.thread - alone.thread,
		            (recorded.thread - alone.thread) * 1.0e6 / static_cast<double>(recorded.records),
		            (recorded.thread - alone.thread) * 1.0e3 / static_cast<double>(recorded.frames));
	}
	return 0;
}
