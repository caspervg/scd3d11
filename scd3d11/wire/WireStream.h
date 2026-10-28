/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include "Wire.h"

#include <cstddef>
#include <cstring>
#include <functional>
#include <list>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace nSCDWire {
	struct BlobKeyHash {
		size_t operator()(BlobKey const &key) const { return static_cast<size_t>(key.low ^ key.high); }
	};

	BlobKey HashBlob(void const *data, uint32_t size);

	// Builds records and hands finished bytes to a sink. Blobs the reader already holds are sent as their key
	// alone. Not thread safe: SC4 drives its graphics driver from one thread.
	class Writer {
	public:
		// Receives finished bytes, in order. Returns false when the stream can no longer be written.
		using Sink = std::function<bool(void const *data, size_t size)>;

		explicit Writer(Sink sink, uint64_t blobBudget = 512ull << 20);

		void Begin(Op op);

		template<typename T>
		void Put(T const &value) {
			static_assert(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>);
			PutBytes(&value, sizeof(T));
		}

		void PutBytes(void const *data, size_t size);
		// Writes the blob's key into the current record, sending the bytes first if the reader lacks them.
		void PutBlob(void const *data, uint32_t size);
		void End();

		// Sends everything buffered so far. Done at the end of every frame.
		bool Flush();
		bool Failed() const { return failed; }

		uint64_t BytesWritten() const { return bytesWritten; }
		uint64_t BlobBytesSent() const { return blobBytesSent; }

	private:
		void Append(Op op, void const *data, size_t size);

		Sink sink;
		uint64_t blobBudget;
		uint64_t blobBytesHeld = 0;
		uint64_t bytesWritten = 0;
		uint64_t blobBytesSent = 0;
		bool failed = false;
		Op current = Op::Count;
		std::vector<uint8_t> record;
		std::vector<uint8_t> pending;
		// Least recently used first.
		std::list<BlobKey> blobOrder;
		std::unordered_map<BlobKey, std::list<BlobKey>::iterator, BlobKeyHash> blobs;
	};

	// Reads fields back in the order the writer put them. A short or damaged record sets ok to false and
	// returns zeros from then on, so a decoder checks ok once after reading all fields.
	class Fields {
	public:
		Fields(uint8_t const *data, uint32_t size) : data(data), size(size) {}

		template<typename T>
		T Get() {
			static_assert(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>);
			T value{};
			if (uint8_t const *bytes = Bytes(sizeof(T))) std::memcpy(&value, bytes, sizeof(T));
			return value;
		}

		uint8_t const *Bytes(size_t count);
		bool ok = true;

	private:
		uint8_t const *data;
		uint32_t size;
		uint32_t offset = 0;
	};

	// Splits a stream into records. Blob and ForgetBlob are handled here, so Next only returns the others.
	class Reader {
	public:
		// Fills exactly `size` bytes, or returns false at the end of the stream.
		using Source = std::function<bool(void *data, size_t size)>;

		explicit Reader(Source source);

		// False with an empty error at a clean end of the stream.
		bool Next(Op &op, Fields &fields);
		std::vector<uint8_t> const *FindBlob(BlobKey const &key) const;

		char const *Error() const { return error; }

	private:
		Source source;
		bool started = false;
		char const *error = "";
		std::vector<uint8_t> payload;
		std::unordered_map<BlobKey, std::vector<uint8_t>, BlobKeyHash> blobs;
	};
}
