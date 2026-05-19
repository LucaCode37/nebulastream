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

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <Nautilus/Interface/Hash/BloomFilter.hpp>
#include <Nautilus/Interface/Hash/BloomFilterRef.hpp>
#include <Util/ExecutionMode.hpp>
#include <Util/Logger/LogLevel.hpp>
#include <Util/Logger/Logger.hpp>
#include <Util/Logger/impl/NesLogger.hpp>
#include <gtest/gtest.h>
#include <magic_enum/magic_enum.hpp>

#include <BaseUnitTest.hpp>
#include <Engine.hpp>
#include <options.hpp>

namespace NES::Nautilus::Interface
{

/// Parameterized test fixture: runs each test with both INTERPRETER and COMPILER backends.
class BloomFilterTest : public Testing::BaseUnitTest,
                        public testing::WithParamInterface<ExecutionMode>
{
public:
    std::unique_ptr<nautilus::engine::NautilusEngine> nautilusEngine;

    static void SetUpTestSuite()
    {
        Logger::setupLogging("BloomFilterTest.log", LogLevel::LOG_DEBUG);
        NES_INFO("Setup BloomFilterTest class.");
    }

    void SetUp() override
    {
        BaseUnitTest::SetUp();
        const auto backend = GetParam();
        nautilus::engine::Options options;
        const bool compilation = (backend == ExecutionMode::COMPILER);
        NES_INFO("Backend: {} compilation: {}", magic_enum::enum_name(backend), compilation);
        options.setOption("engine.Compilation", compilation);
        options.setOption("mlir.enableMultithreading", false);
        nautilusEngine = std::make_unique<nautilus::engine::NautilusEngine>(options);
    }

    static void TearDownTestSuite() { NES_INFO("Tear down BloomFilterTest class."); }
};

TEST_P(BloomFilterTest, noFalseNegatives)
{
    constexpr std::size_t numKeys = 1000;
    constexpr double fpRate = 0.01;

    BloomFilter filter(numKeys, fpRate);
    for (std::uint64_t key = 0; key < numKeys; ++key)
    {
        filter.add(key);
    }

    /// Compile a function that checks mightContain via BloomFilterRef (runs through Nautilus JIT)
    auto mightContainFn = nautilusEngine->registerFunction(std::function(
        [](nautilus::val<const BloomFilter*> filterPtr, nautilus::val<uint64_t> key) -> nautilus::val<bool>
        {
            const BloomFilterRef bloomFilter(filterPtr);
            return bloomFilter.mightContain(key);
        }));

    for (std::uint64_t key = 0; key < numKeys; ++key)
    {
        ASSERT_TRUE(mightContainFn(&filter, key)) << "False negative for key " << key;
    }
}

TEST_P(BloomFilterTest, clearResetsFilter)
{
    constexpr std::size_t numKeys = 100;
    constexpr double fpRate = 0.01;

    BloomFilter filter(numKeys, fpRate);
    for (std::uint64_t key = 0; key < numKeys; ++key)
    {
        filter.add(key);
    }

    auto mightContainFn = nautilusEngine->registerFunction(std::function(
        [](nautilus::val<const BloomFilter*> filterPtr, nautilus::val<uint64_t> key) -> nautilus::val<bool>
        {
            const BloomFilterRef bloomFilter(filterPtr);
            return bloomFilter.mightContain(key);
        }));

    for (std::uint64_t key = 0; key < numKeys; ++key)
    {
        ASSERT_TRUE(mightContainFn(&filter, key)) << "Key " << key << " should be present before clear";
    }

    filter.clear();

    for (std::uint64_t key = 0; key < numKeys; ++key)
    {
        ASSERT_FALSE(mightContainFn(&filter, key)) << "Key " << key << " should not be present after clear";
    }
}

TEST_P(BloomFilterTest, falsePositiveRateSanity)
{
    constexpr std::size_t numInserted = 1000;
    constexpr std::size_t numQueries = 10000;
    constexpr double configuredFpRate = 0.01;
    constexpr double maxAcceptableFpRate = 0.05;

    BloomFilter filter(numInserted, configuredFpRate);
    for (std::uint64_t key = 0; key < numInserted; ++key)
    {
        filter.add(key);
    }

    auto mightContainFn = nautilusEngine->registerFunction(std::function(
        [](nautilus::val<const BloomFilter*> filterPtr, nautilus::val<uint64_t> key) -> nautilus::val<bool>
        {
            const BloomFilterRef bloomFilter(filterPtr);
            return bloomFilter.mightContain(key);
        }));

    std::size_t falsePositives = 0;
    constexpr std::uint64_t queryOffset = 1000000;

    for (std::uint64_t i = 0; i < numQueries; ++i)
    {
        std::uint64_t key = queryOffset + i;
        if (mightContainFn(&filter, key))
        {
            ++falsePositives;
        }
    }

    const double actualFpRate = static_cast<double>(falsePositives) / static_cast<double>(numQueries);

    ASSERT_LE(actualFpRate, maxAcceptableFpRate)
        << "False positive rate " << actualFpRate << " exceeds threshold " << maxAcceptableFpRate;

    NES_INFO("False positive rate: {} (configured: {})", actualFpRate, configuredFpRate);
}

TEST_P(BloomFilterTest, edgeCaseZeroExpectedEntries)
{
    BloomFilter filter(0, 0.01);

    ASSERT_GT(filter.sizeInBits(), 0) << "Filter should have non-zero size even with 0 expected entries";
    ASSERT_GT(filter.hashFunctionCount(), 0) << "Filter should have at least one hash function";

    auto mightContainFn = nautilusEngine->registerFunction(std::function(
        [](nautilus::val<const BloomFilter*> filterPtr, nautilus::val<uint64_t> key) -> nautilus::val<bool>
        {
            const BloomFilterRef bloomFilter(filterPtr);
            return bloomFilter.mightContain(key);
        }));

    filter.add(42);
    ASSERT_TRUE(mightContainFn(&filter, uint64_t{42}));
}

TEST_P(BloomFilterTest, sameKeyMultipleInserts)
{
    constexpr std::size_t numKeys = 100;
    constexpr double fpRate = 0.01;

    BloomFilter filter(numKeys, fpRate);

    constexpr std::uint64_t key = 42;
    filter.add(key);
    filter.add(key);
    filter.add(key);

    auto mightContainFn = nautilusEngine->registerFunction(std::function(
        [](nautilus::val<const BloomFilter*> filterPtr, nautilus::val<uint64_t> k) -> nautilus::val<bool>
        {
            const BloomFilterRef bloomFilter(filterPtr);
            return bloomFilter.mightContain(k);
        }));

    ASSERT_TRUE(mightContainFn(&filter, key)) << "Key should still be present after multiple inserts";
}

TEST_P(BloomFilterTest, bloomFilterAlwaysTrueNeverFilters)
{
    auto alwaysTrueFn = nautilusEngine->registerFunction(std::function(
        [](nautilus::val<uint64_t> key) -> nautilus::val<bool>
        {
            const BloomFilterAlwaysTrue noFilter;
            return noFilter.mightContain(key);
        }));

    for (std::uint64_t key = 0; key < 100; ++key)
    {
        ASSERT_TRUE(alwaysTrueFn(key)) << "BloomFilterAlwaysTrue should always return true";
    }
}

TEST_P(BloomFilterTest, gettersReturnReasonableValues)
{
    constexpr std::size_t numKeys = 1000;
    constexpr double fpRate = 0.01;

    BloomFilter filter(numKeys, fpRate);

    ASSERT_GT(filter.sizeInBits(), 0) << "Bit count should be positive";
    ASSERT_GT(filter.hashFunctionCount(), 0) << "Hash function count should be positive";

    NES_INFO("For {} entries with FP rate {}: m={} bits, k={} hashes",
             numKeys, fpRate, filter.sizeInBits(), filter.hashFunctionCount());
}

INSTANTIATE_TEST_CASE_P(
    BloomFilterTest,
    BloomFilterTest,
    ::testing::Values(ExecutionMode::INTERPRETER, ExecutionMode::COMPILER),
    [](const testing::TestParamInfo<BloomFilterTest::ParamType>& info)
    {
        std::stringstream ss;
        ss << magic_enum::enum_name(info.param);
        return ss.str();
    });

} // namespace NES::Nautilus::Interface
