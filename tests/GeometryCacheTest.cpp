/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026  Casper Van Gheluwe
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// FlatGeometryCache against std::unordered_map under random inserts, overwrites and erases. Keys
// that differ only in format share a home slot, so the erases run through long probe chains.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <random>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "GeometryCache.h"

using nSCD3D11::FlatGeometryCache;
using nSCD3D11::GeometryCacheKey;

namespace {
    struct Value {
        uint8_t segment;
        uint32_t offset;
    };

    struct KeyHash {
        size_t operator()(GeometryCacheKey const &key) const {
            return std::hash<uint64_t>()(key.digest.low64 ^ (key.digest.high64 * 31) ^ key.count ^
                                         (static_cast<uint64_t>(key.format) << 40) ^
                                         static_cast<uint64_t>(key.generation));
        }
    };

    GeometryCacheKey Key(uint64_t low, uint64_t high, uint32_t count, uint32_t format, bool generation) {
        GeometryCacheKey key;
        key.digest.low64 = low;
        key.digest.high64 = high;
        key.count = count;
        key.format = format;
        key.generation = generation;
        return key;
    }

    void Check(FlatGeometryCache<Value> &flat, std::unordered_map<GeometryCacheKey, Value, KeyHash> const &reference,
               std::vector<GeometryCacheKey> const &universe) {
        assert(flat.size() == reference.size());
        assert(flat.empty() == reference.empty());
        for (GeometryCacheKey const &key: universe) {
            auto const expected = reference.find(key);
            auto *const found = flat.find(key);
            if (expected == reference.end()) {
                assert(found == flat.end());
                assert(flat.count(key) == 0);
            } else {
                assert(found != flat.end());
                assert(found->first == key);
                assert(found->second.segment == expected->second.segment && found->second.offset == expected->second.offset);
            }
        }
    }
}

int main() {
    std::mt19937_64 random(12345);
    std::vector<GeometryCacheKey> universe;
    for (uint32_t i = 0; i < 3000; ++i) universe.push_back(Key(random(), random(), 1 + i % 97, 3, false));
    for (uint32_t i = 0; i < 1500; ++i) universe.push_back(Key(i, 0, 4, 0, true));
    // Same digest and count, different format: one home slot, one long run.
    for (uint32_t format = 0; format < 400; ++format) universe.push_back(Key(0xABCDEF, 0x123456, 9, format, false));

    FlatGeometryCache<Value> flat;
    std::unordered_map<GeometryCacheKey, Value, KeyHash> reference;
    std::uniform_int_distribution<size_t> pick(0, universe.size() - 1);

    for (int step = 0; step < 300000; ++step) {
        GeometryCacheKey const &key = universe[pick(random)];
        if (random() % 3 != 0) {
            Value const value{static_cast<uint8_t>(random() % 8), static_cast<uint32_t>(random())};
            flat.insert_or_assign(key, value);
            reference.insert_or_assign(key, value);
        } else if (auto *const found = flat.find(key); found != flat.end()) {
            flat.erase(found);
            reference.erase(key);
        } else {
            assert(reference.find(key) == reference.end());
        }
        if (step % 20000 == 0) Check(flat, reference, universe);
    }
    Check(flat, reference, universe);

    // Erasing the whole collision run in both directions leaves nothing behind.
    for (int pass = 0; pass < 2; ++pass) {
        for (uint32_t format = 0; format < 400; ++format) flat.insert_or_assign(Key(0xABCDEF, 0x123456, 9, format, false), Value{1, format});
        for (uint32_t i = 0; i < 400; ++i) {
            uint32_t const format = pass == 0 ? i : 399 - i;
            auto *const found = flat.find(Key(0xABCDEF, 0x123456, 9, format, false));
            assert(found != flat.end() && found->second.offset == format);
            flat.erase(found);
            reference.erase(Key(0xABCDEF, 0x123456, 9, format, false));
        }
        Check(flat, reference, universe);
    }

    flat.clear();
    assert(flat.empty() && flat.size() == 0 && flat.find(universe[0]) == flat.end());
    flat.insert_or_assign(universe[0], Value{2, 7});
    assert(flat.size() == 1 && flat.find(universe[0])->second.offset == 7);

    std::puts("GeometryCacheTest passed");
    return 0;
}
