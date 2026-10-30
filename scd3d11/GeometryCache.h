/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026  Casper Van Gheluwe
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

#include "D3D11Conversions.h"

namespace nSCD3D11 {
    // The geometry cache's key to value table. Every draw looks its geometry up here, so it is an open
    // addressing table probed linearly in one array rather than a node per entry: a hit is usually one
    // cache line. Deletion shifts the following entries back instead of leaving tombstones, because
    // recycling a segment erases thousands of keys at once.
    //
    // The table stays at most half full, which keeps lookups that miss as short as hits. With the
    // default 8 segments the cache holds at most 8 * MAX_CACHE_ENTRIES_PER_SEGMENT (262144) keys, so it
    // never grows past 2^19 slots of 32 bytes, 16 MB, against about 17 MB for std::unordered_map at that
    // size. VertexCacheSegments=16 doubles that. A key's count is never 0, so an empty slot needs no flag.
    //
    // It offers the part of std::unordered_map the driver uses. find() returns a pointer to the slot,
    // with the key in first and the value in second, and end() is null. Inserting or erasing
    // invalidates every pointer into the table.
    template<typename Value>
    class FlatGeometryCache {
    public:
        struct Slot {
            GeometryCacheKey first{};
            Value second{};

            bool used() const { return first.count != 0; }
        };

        Slot *find(GeometryCacheKey const &key) {
            if (entries == 0) return nullptr;
            for (size_t index = Home(key);; index = (index + 1) & mask) {
                Slot &slot = slots[index];
                if (!slot.used()) return nullptr;
                if (slot.first == key) return &slot;
            }
        }

        Slot *end() { return nullptr; }

        size_t count(GeometryCacheKey const &key) { return find(key) != nullptr ? 1 : 0; }

        // May throw std::bad_alloc when the table grows; the table is unchanged if it does. The key's
        // count must not be 0.
        void insert_or_assign(GeometryCacheKey const &key, Value const &value) {
            if ((static_cast<uint64_t>(entries) + 1) * 2 > static_cast<uint64_t>(slots.size())) {
                Rehash(slots.empty() ? kInitialSlots : slots.size() * 2);
            }
            for (size_t index = Home(key);; index = (index + 1) & mask) {
                Slot &slot = slots[index];
                if (!slot.used()) {
                    slot.first = key;
                    slot.second = value;
                    ++entries;
                    return;
                }
                if (slot.first == key) {
                    slot.second = value;
                    return;
                }
            }
        }

        void erase(Slot *slot) {
            size_t hole = static_cast<size_t>(slot - slots.data());
            // Pull back every later entry of the run that may sit at the hole without being moved past
            // its home slot, so lookups never need to step over an empty slot.
            for (size_t next = (hole + 1) & mask; slots[next].used(); next = (next + 1) & mask) {
                size_t const home = Home(slots[next].first);
                if (((next - home) & mask) >= ((next - hole) & mask)) {
                    slots[hole] = slots[next];
                    hole = next;
                }
            }
            slots[hole] = Slot{};
            --entries;
        }

        size_t size() const { return entries; }
        bool empty() const { return entries == 0; }
        size_t bucket_count() const { return slots.size(); }

        void clear() {
            std::vector<Slot>().swap(slots);
            entries = 0;
            mask = 0;
            shift = 32;
        }

        // The table never shrinks as entries go, so a trimmed cache gives the slots back here. Keeps the
        // larger table if the smaller one cannot be allocated.
        void shrink_to_fit() {
            if (entries == 0) {
                clear();
                return;
            }
            size_t capacity = kInitialSlots;
            while (capacity < entries * 2) capacity *= 2;
            if (capacity >= slots.size()) return;
            try {
                Rehash(capacity);
            } catch (std::bad_alloc const &) {
            }
        }

    private:
        static constexpr size_t kInitialSlots = 1024;

        // Generation keys count up rather than being digests, so the bits are mixed before indexing.
        // Folded to 32 bits first: a 64-bit multiply is a library call on x86.
        size_t Home(GeometryCacheKey const &key) const {
            uint64_t const folded = key.digest.low64 ^ key.digest.high64;
            uint32_t const bits = (static_cast<uint32_t>(folded) ^ static_cast<uint32_t>(folded >> 32) ^ key.count) *
                                  0x9E3779B9u;
            return static_cast<size_t>(bits >> shift);
        }

        // capacity is a power of two at least twice the entries, so reinserting never rehashes again.
        void Rehash(size_t capacity) {
            std::vector<Slot> old(capacity);
            old.swap(slots);
            mask = capacity - 1;
            shift = 32;
            for (size_t bits = capacity; bits > 1; bits >>= 1) --shift;
            entries = 0;
            for (Slot const &slot: old) {
                if (slot.used()) insert_or_assign(slot.first, slot.second);
            }
        }

        std::vector<Slot> slots;
        size_t entries = 0;
        size_t mask = 0;
        unsigned shift = 32;
    };
}
