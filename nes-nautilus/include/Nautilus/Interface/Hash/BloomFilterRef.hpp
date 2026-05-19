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

/// Proxy: returns the bit count of a BloomFilter (called once at construction time).
inline uint64_t getBitCountProxy(const BloomFilter* f) { return f->sizeInBits(); }
/// Proxy: returns the hash function count (called once at construction time).
inline uint64_t getHashCountProxy(const BloomFilter* f) { return f->hashFunctionCount(); }

/// Proxy: compute h1 for key — identical to the hashBytes call in BloomFilter::add().
///
/// Using invoke here (rather than inlining MurMur2-64A as val<> code) is necessary because
/// the Nautilus COMPILER backend generates arith.shrsi (arithmetic / signed right shift) for
/// all >> operations on val<> types, including val<uint64_t>.  For hash values whose top bit
/// is set this produces a sign-extended result instead of a zero-filled one, corrupting every
/// intermediate hash word and producing entirely wrong bit positions.
inline uint64_t computeH1Proxy(uint64_t key)
{
    return ::NES::hashBytes(&key, sizeof(uint64_t));
}

/// Proxy: compute h2 for key — identical to the h2 derivation in BloomFilter::add().
///
/// Computed on the host for the same reason as computeH1Proxy (arith.shrsi limitation).
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

/// Proxy: test one bit position inside the filter's bit array.
///
/// The host-side right shift (data[word] >> offset) cannot be expressed as val<> code
/// due to the arith.shrsi limitation — see computeH1Proxy for details.
inline uint64_t getBitProxy(const BloomFilter* f, uint64_t bitIndex)
{
    const uint64_t wordIndex = bitIndex / 64;
    const uint64_t bitOffset = bitIndex % 64;
    return (f->data()[wordIndex] >> bitOffset) & UINT64_C(1);
}

/// Nautilus-native wrapper around a BloomFilter with a JIT-compiled probe loop.
///
/// h1 and h2 are computed once via invoke so they exactly match BloomFilter::add().
/// The double-hashing probe loop — combined = h1 + i*h2, bitIndex = combined % bitCount —
/// runs entirely as JIT-generated code, giving the compiler full visibility over the
/// address arithmetic.  Only the bit read itself requires an invoke call due to the
/// arith.shrsi limitation in the Nautilus COMPILER backend.
class BloomFilterRef
{
public:
    explicit BloomFilterRef(const nautilus::val<const BloomFilter*>& filter)
        : filterRef(filter)
        , bitCount(nautilus::invoke(getBitCountProxy, filter))
        , hashCount(nautilus::invoke(getHashCountProxy, filter))
    {
    }

    /// Returns true if key might be in the set (false negatives are impossible).
    [[nodiscard]] nautilus::val<bool> mightContain(nautilus::val<uint64_t> key) const
    {
        /// Compute h1 and h2 via invoke to match BloomFilter::add() exactly and
        /// to avoid the arith.shrsi COMPILER limitation.
        nautilus::val<uint64_t> h1 = nautilus::invoke(computeH1Proxy, key);
        nautilus::val<uint64_t> h2 = nautilus::invoke(computeH2Proxy, key);

        /// Probe all k hash positions — this loop runs entirely as JIT code.
        nautilus::val<uint64_t> allPresent(1);
        for (nautilus::val<uint64_t> i(0); i < hashCount; i = i + nautilus::val<uint64_t>(1))
        {
            nautilus::val<uint64_t> combined   = h1 + i * h2;
            nautilus::val<uint64_t> bitIndex   = combined % bitCount;
            nautilus::val<uint64_t> bit        = nautilus::invoke(getBitProxy, filterRef, bitIndex);
            allPresent = allPresent & bit;
        }

        return allPresent != nautilus::val<uint64_t>(0);
    }

private:
    nautilus::val<const BloomFilter*> filterRef;  ///< passed to getBitProxy
    nautilus::val<uint64_t>           bitCount;
    nautilus::val<uint64_t>           hashCount;
};

/// A BloomFilter variant that always returns true (i.e., never filters anything).
/// Used when BloomFilter is disabled so we avoid null-pointer checks in JIT code.
class BloomFilterAlwaysTrue
{
public:
    [[nodiscard]] nautilus::val<bool> mightContain(nautilus::val<uint64_t> /*value*/) const
    {
        return nautilus::val<bool>(true);
    }
};

} // namespace NES::Nautilus::Interface
