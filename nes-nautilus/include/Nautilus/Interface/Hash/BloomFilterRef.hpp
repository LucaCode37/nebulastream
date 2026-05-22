/*
    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        https://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#pragma once

#include <Nautilus/Interface/Hash/BloomFilter.hpp>
#include <function.hpp>
#include <val.hpp>

/// Forward-declaration of the Murmur-style hash used by BloomFilter::add().
namespace NES {
std::uint64_t hashBytes(void* data, std::uint64_t length);
}

namespace NES::Nautilus::Interface
{

/// Proxy functions called via nautilus::invoke().
/// Hash proxies (h1, h2, bit read) use invoke because >> on val<uint64_t> emits arith.shrsi
/// (signed shift) in the Nautilus COMPILER backend, producing wrong results for values with
/// the high bit set.

inline uint64_t getBitCountProxy(const BloomFilter* f) { return f->sizeInBits(); }
inline uint64_t getHashCountProxy(const BloomFilter* f) { return f->hashFunctionCount(); }

/// Mirrors the hashBytes() call in BloomFilter::add().
inline uint64_t computeH1Proxy(uint64_t key)
{
    return ::NES::hashBytes(&key, sizeof(uint64_t));
}

/// Mirrors the h2 derivation in BloomFilter::add().
inline uint64_t computeH2Proxy(uint64_t key)
{
    uint64_t h = ::NES::hashBytes(&key, sizeof(uint64_t));
    h ^= h >> 33;
    h *= UINT64_C(0xff51afd7ed558ccd);
    h ^= h >> 33;
    h *= UINT64_C(0xc4ceb9fe1a85ec53);
    h ^= h >> 33;
    if (h == 0) {
        h = UINT64_C(0x9e3779b97f4a7c15);
    }
    return h;
}

/// Reads one bit from the filter's bit array.
inline uint64_t getBitProxy(const BloomFilter* f, uint64_t bitIndex)
{
    return (f->data()[bitIndex / 64] >> (bitIndex % 64)) & UINT64_C(1);
}

/// Nautilus wrapper around BloomFilter with a JIT-compiled probe loop.
/// h1, h2, and the bit read use invoke (to work around the arith.shrsi bug).
/// The double-hashing loop itself — combined = h1 + i*h2, bitIndex = combined % bitCount —
/// runs as JIT code.
class BloomFilterRef
{
public:
    explicit BloomFilterRef(const nautilus::val<const BloomFilter*>& filter)
        : filterRef(filter)
        , bitCount(nautilus::invoke(getBitCountProxy, filter))
        , hashCount(nautilus::invoke(getHashCountProxy, filter))
    {
    }

    [[nodiscard]] nautilus::val<bool> mightContain(nautilus::val<uint64_t> key) const
    {
        nautilus::val<uint64_t> h1 = nautilus::invoke(computeH1Proxy, key);
        nautilus::val<uint64_t> h2 = nautilus::invoke(computeH2Proxy, key);

        nautilus::val<uint64_t> allPresent(1);
        for (nautilus::val<uint64_t> i(0); i < hashCount; i = i + nautilus::val<uint64_t>(1))
        {
            nautilus::val<uint64_t> combined = h1 + i * h2;
            nautilus::val<uint64_t> bitIndex = combined % bitCount;
            nautilus::val<uint64_t> bit      = nautilus::invoke(getBitProxy, filterRef, bitIndex);
            allPresent = allPresent & bit;
        }

        return allPresent != nautilus::val<uint64_t>(0);
    }

private:
    nautilus::val<const BloomFilter*> filterRef;
    nautilus::val<uint64_t>           bitCount;
    nautilus::val<uint64_t>           hashCount;
};

/// JIT version of BF with no invoke calls, for future

/// Once Nautilus emits arith.shrui for >> on val<uint64_t>, the invoke-based BloomFilterRef
/// above can be replaced with this version. It moves h1/h2 and the bit read entirely into

/// Additional proxy needed (called once at construction, not in the hot path):
//   inline uint64_t* getDataProxy(const BloomFilter* f) {
//       return const_cast<uint64_t*>(f->data());
//   }

// class BloomFilterRef {
// public:
//     explicit BloomFilterRef(const nautilus::val<const BloomFilter*>& filter)
//         : bitCount(nautilus::invoke(getBitCountProxy, filter))
//         , hashCount(nautilus::invoke(getHashCountProxy, filter))
//         , bitsData(nautilus::invoke(getDataProxy, filter))
//     {}

//     [[nodiscard]] nautilus::val<bool> mightContain(nautilus::val<uint64_t> key) const
//     {
//         const nautilus::val<uint64_t> m(UINT64_C(0xc6a4a7935bd1e995));
//         const nautilus::val<uint64_t> r(UINT64_C(47));
//         nautilus::val<uint64_t> h1 =
//             nautilus::val<uint64_t>(UINT64_C(0xe17a1465)) ^ (nautilus::val<uint64_t>(8ULL) * m);
//         nautilus::val<uint64_t> k = key;
//         k  = k * m; k = k ^ (k >> r); k = k * m;
//         h1 = h1 ^ k; h1 = h1 * m;
//         h1 = h1 ^ (h1 >> r); h1 = h1 * m; h1 = h1 ^ (h1 >> r);

//         const nautilus::val<uint64_t> c33(UINT64_C(33));
//         nautilus::val<uint64_t> h2 = h1;
//         h2 = h2 ^ (h2 >> c33); h2 = h2 * nautilus::val<uint64_t>(UINT64_C(0xff51afd7ed558ccd));
//         h2 = h2 ^ (h2 >> c33); h2 = h2 * nautilus::val<uint64_t>(UINT64_C(0xc4ceb9fe1a85ec53));
//         h2 = h2 ^ (h2 >> c33);

//         // no invoke calls
//         nautilus::val<uint64_t> allPresent(1ULL);
//         for (nautilus::val<uint64_t> i(0ULL); i < hashCount; i = i + nautilus::val<uint64_t>(1ULL))
//         {
//             nautilus::val<uint64_t> combined  = h1 + i * h2;
//             nautilus::val<uint64_t> bitIndex  = combined % bitCount;
//             nautilus::val<uint64_t> wordIndex = bitIndex / nautilus::val<uint64_t>(64ULL);
//             nautilus::val<uint64_t> bitOffset = bitIndex % nautilus::val<uint64_t>(64ULL);
//             nautilus::val<uint64_t> word      = *(bitsData + wordIndex);
//             nautilus::val<uint64_t> bit       = (word >> bitOffset) & nautilus::val<uint64_t>(1ULL);
//             allPresent = allPresent & bit;
//         }
//         return allPresent != nautilus::val<uint64_t>(0ULL);
//     }

// private:
//     nautilus::val<uint64_t>  bitCount;
//     nautilus::val<uint64_t>  hashCount;
//     nautilus::val<uint64_t*> bitsData;
// };

/// BloomFilter variant that always returns true — used when the filter is disabled.
class BloomFilterAlwaysTrue
{
public:
    [[nodiscard]] nautilus::val<bool> mightContain(nautilus::val<uint64_t> /*value*/) const
    {
        return nautilus::val<bool>(true);
    }
};

} // namespace NES::Nautilus::Interface
