/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// The stream writer against a plain encoding of the format: records with blobs that are new, known again and
// forgotten under a small budget, through a buffer and through a small shared ring that a reader thread
// drains while records wrap around its end.

#include "wire/SharedRing.h"
#include "wire/WireStream.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <list>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace nSCDWire;

namespace {
	constexpr uint64_t kBlobBudget = 300u << 10;
	constexpr uint64_t kRingBytes = 1u << 20;
	// What one frame may hold, so a frame always fits the ring.
	constexpr size_t kFrameBytes = 600u << 10;

	struct Field {
		bool blob = false;
		std::vector<uint8_t> bytes;
	};
	struct Record {
		Op op;
		std::vector<Field> fields;
		// A frame ends after it.
		bool flush = false;
	};

	// Writes the format by hand: blob records go in front of the record that first uses them, after the
	// ForgetBlob records of the least recently used blobs that make room.
	class Reference {
	public:
		std::vector<uint8_t> bytes;

		Reference() { Raw(StreamHeader{}); }

		void Add(Record const &record) {
			std::vector<uint8_t> payload;
			for (Field const &field: record.fields) {
				if (!field.blob) {
					payload.insert(payload.end(), field.bytes.begin(), field.bytes.end());
					continue;
				}
				uint32_t const size = static_cast<uint32_t>(field.bytes.size());
				BlobKey const key = size == 0 ? BlobKey{} : HashBlob(field.bytes.data(), size);
				if (size != 0) {
					auto found = std::find(order.begin(), order.end(), key);
					if (found != order.end()) {
						order.splice(order.end(), order, found);
					} else {
						while (held + size > kBlobBudget && !order.empty()) {
							BlobKey const victim = order.front();
							order.pop_front();
							held -= victim.size;
							Raw(RecordHeader{static_cast<uint16_t>(Op::ForgetBlob), 0, sizeof(victim)});
							Raw(victim);
						}
						Raw(RecordHeader{static_cast<uint16_t>(Op::Blob), 0, static_cast<uint32_t>(sizeof(key) + size)});
						Raw(key);
						bytes.insert(bytes.end(), field.bytes.begin(), field.bytes.end());
						order.push_back(key);
						held += size;
					}
				}
				uint8_t const *k = reinterpret_cast<uint8_t const *>(&key);
				payload.insert(payload.end(), k, k + sizeof(key));
			}
			Raw(RecordHeader{static_cast<uint16_t>(record.op), 0, static_cast<uint32_t>(payload.size())});
			bytes.insert(bytes.end(), payload.begin(), payload.end());
		}

	private:
		template<typename T>
		void Raw(T const &value) {
			uint8_t const *b = reinterpret_cast<uint8_t const *>(&value);
			bytes.insert(bytes.end(), b, b + sizeof(T));
		}

		std::list<BlobKey> order;
		uint64_t held = 0;
	};

	void Write(Writer &writer, std::vector<Record> const &records) {
		for (Record const &record: records) {
			writer.Begin(record.op);
			for (Field const &field: record.fields) {
				if (field.blob) writer.PutBlob(field.bytes.data(), static_cast<uint32_t>(field.bytes.size()));
				else writer.PutBytes(field.bytes.data(), field.bytes.size());
			}
			writer.End();
			if (record.flush) writer.Flush();
		}
		writer.Flush();
	}

	std::vector<Record> MakeRecords(uint32_t seed) {
		std::mt19937 random(seed);
		auto const below = [&](uint32_t n) { return static_cast<uint32_t>(random() % n); };
		// Blobs come from a small pool so they repeat, with a few large ones.
		std::vector<std::vector<uint8_t> > pool;
		for (int i = 0; i < 40; ++i) {
			std::vector<uint8_t> blob(i % 10 == 0 ? 40000 + below(60000) : below(3000));
			for (uint8_t &b: blob) b = static_cast<uint8_t>(random());
			pool.push_back(std::move(blob));
		}
		std::vector<Record> records;
		size_t frame = 0;
		for (int i = 0; i < 3000; ++i) {
			Record record{static_cast<Op>(3 + below(static_cast<uint32_t>(Op::Count) - 3)), {}, false};
			size_t size = 8;
			for (uint32_t f = below(5); f-- > 0;) {
				Field field;
				field.blob = below(3) == 0;
				if (field.blob) field.bytes = pool[below(static_cast<uint32_t>(pool.size()))];
				else field.bytes.resize(below(2) == 0 ? below(16) : below(5000));
				for (uint8_t &b: field.bytes) if (!field.blob) b = static_cast<uint8_t>(random());
				size += field.bytes.size() + 40;
				record.fields.push_back(std::move(field));
			}
			// A frame ends before it would no longer fit the ring.
			if (frame + size > kFrameBytes && !records.empty()) {
				records.back().flush = true;
				frame = 0;
			}
			frame += size;
			records.push_back(std::move(record));
		}
		return records;
	}

	void SameAsReference(uint32_t seed) {
		std::vector<Record> const records = MakeRecords(seed);
		Reference reference;
		for (Record const &record: records) reference.Add(record);

		std::vector<uint8_t> buffered;
		{
			Writer writer([&](void const *data, size_t size) {
				buffered.insert(buffered.end(), static_cast<uint8_t const *>(data),
				                static_cast<uint8_t const *>(data) + size);
				return true;
			}, kBlobBudget);
			Write(writer, records);
			assert(!writer.Failed());
			assert(writer.BytesWritten() == buffered.size());
		}
		if (buffered != reference.bytes) {
			size_t at = 0;
			while (at < buffered.size() && at < reference.bytes.size() && buffered[at] == reference.bytes[at]) ++at;
			std::fprintf(stderr, "seed %u: buffered %zu bytes, reference %zu, first difference at %zu\n", seed, buffered.size(),
			            reference.bytes.size(), at);
		}
		assert(buffered == reference.bytes);

		std::wstring const name = L"Local\\SCD3D11-WireStreamTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
		                          std::to_wstring(seed);
		SharedRing ring;
		bool const created = ring.Create(name, kRingBytes);
		assert(created);
		std::vector<uint8_t> drained(reference.bytes.size());
		std::thread reader([&] {
			SharedRing side;
			bool const opened = side.Open(name);
			assert(opened);
			bool const read = side.Read(drained.data(), drained.size(), [] { return true; });
			assert(read);
		});
		bool failed = false;
		{
			Writer writer(std::make_unique<RingOutput>(ring, [] { return true; }, [&] { failed = true; }), kBlobBudget);
			Write(writer, records);
			assert(!writer.Failed());
		}
		reader.join();
		assert(!failed);
		assert(drained == reference.bytes);
		std::printf("seed %u: %zu records, %zu bytes, the same three ways\n", seed, records.size(), drained.size());
	}

	// A record longer than the ring can never be read, so the ring output gives up instead of waiting.
	void RecordLongerThanTheRingFails() {
		std::wstring const name = L"Local\\SCD3D11-WireStreamTest-long-" + std::to_wstring(GetCurrentProcessId());
		SharedRing ring;
		bool const created = ring.Create(name, 4096);
		assert(created);
		bool failed = false;
		Writer writer(std::make_unique<RingOutput>(ring, [] { return true; }, [&] { failed = true; }));
		std::vector<uint8_t> bytes(5000, 7);
		writer.Begin(Op::Frame);
		writer.PutBytes(bytes.data(), bytes.size());
		writer.End();
		assert(writer.Failed() && failed);
		assert(!writer.Flush());
	}
}

int main() {
	for (uint32_t seed: {1u, 2u, 3u}) SameAsReference(seed);
	RecordLongerThanTheRingFails();
	std::puts("WireStreamTest passed");
	return 0;
}
