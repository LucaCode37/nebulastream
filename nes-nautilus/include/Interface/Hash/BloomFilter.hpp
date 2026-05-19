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

#include <cstddef>
#include <cstdint>
#include <vector>

namespace NES::Nautilus::Interface {

/// Simple Bloom filter for 64-bit keys.
/// See: https://en.wikipedia.org/wiki/Bloom_filter
/// Original paper: B. H. Bloom, "Space/Time Trade-offs in Hash Coding with Allowable Errors", 1970.
///
/// Core properties:
///  - No false negatives (if an inserted key is queried, mightContain() never returns false).
///  - False positives are possible and controlled via configuration.
class BloomFilter {
  public:
    using KeyType = std::uint64_t;

    /// Construct a Bloom filter with an expected number of entries and target false positive rate.
    ///
    /// @param expectedEntries Estimated number of keys that will be inserted.
    /// @param falsePositiveRate Target false positive probability in (0,1),
    ///                          for example 0.01 for 1%.
    BloomFilter(std::size_t expectedEntries, double falsePositiveRate);

    void add(KeyType key);

    /// Query if a key might be in the set.
    /// @return true if the key might be contained (false positive possible),
    ///         false if the key is definitely not contained.
    [[nodiscard]] bool mightContain(KeyType key) const;

    void clear();

    [[nodiscard]] std::size_t sizeInBits() const { return bitCount; }
    [[nodiscard]] std::size_t hashFunctionCount() const { return hashCount; }
    [[nodiscard]] const std::uint64_t* data() const { return bits.data(); }

  private:
    std::size_t bitCount;                 ///< m = number of bits in the filter
    std::size_t hashCount;                ///< k = number of hash functions
    std::vector<std::uint64_t> bits;      ///< bit array packed into 64-bit words

    void setBit(std::size_t bitIndex);
    [[nodiscard]] bool getBit(std::size_t bitIndex) const;

    /// Compute base hashes for key (used for double hashing).
    void computeBaseHashes(KeyType key, std::uint64_t& h1, std::uint64_t& h2) const;
};

} // namespace NES::Nautilus::Interface
