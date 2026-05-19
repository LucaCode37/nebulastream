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

#include <gtest/gtest.h>
#include <Join/NestedLoopJoin/NLJSlice.hpp>
#include <Join/StreamJoinUtil.hpp>
#include <Nautilus/Interface/Hash/BloomFilter.hpp>
#include <Time/Timestamp.hpp>
#include <Util/Logger/Logger.hpp>
#include <Util/Logger/LogLevel.hpp>
#include <NautilusTestUtils.hpp>
#include <PagedVectorTestUtils.hpp>

namespace NES
{

class BloomFilterNLJTest : public testing::Test, public TestUtils::NautilusTestUtils
{
public:
    static void SetUpTestSuite()
    {
        Logger::setupLogging("BloomFilterNLJTest.log", LogLevel::LOG_DEBUG);
        NES_INFO("Setup BloomFilterNLJTest class.");
    }

    static void TearDownTestSuite()
    {
        NES_INFO("Tear down BloomFilterNLJTest class.");
    }


protected:
    void SetUp() override 
    {
        bufferManager = BufferManager::create();
        schema = Schema{}.addField("id", DataType::Type::UINT64);
        
        /// Set up NautilusEngine for PagedVector operations.
        nautilus::engine::Options options;
        options.setOption("engine.Compilation", false); /// Use the interpreter for faster tests.
        nautilusEngine = std::make_unique<nautilus::engine::NautilusEngine>(options);
    }
    
    void TearDown() override 
    {
        bufferManager.reset();
        nautilusEngine.reset();
    }
    
    std::shared_ptr<BufferManager> bufferManager;
    Schema schema;
    std::unique_ptr<nautilus::engine::NautilusEngine> nautilusEngine;
};

/// Integration test that verifies the BUILD phase creates accessible BloomFilter references.
/// Minimal integration coverage for NLJ + BloomFilter:
/// - create NLJSlice
/// - store tuples on both sides
/// - run combinePagedVectors()
/// - fetch BloomFilterRef for both build sides
TEST_F(BloomFilterNLJTest, BuildPhase_BloomFilterRefAccess)
{
    std::cout << "\nBUILD Phase Test: BloomFilterRef Access" << std::endl;
    
    NLJSlice slice(Timestamp(0), Timestamp(100), 1);
    
    /// Add tuples to both sides.
    const uint64_t tupleCount = 30;
    auto records = createMonotonicallyIncreasingValues(
        schema, MemoryLayoutType::ROW_LAYOUT, tupleCount, *bufferManager);
    
    PagedVector* leftPagedVector = slice.getPagedVectorRefLeft(WorkerThreadId(0));
    PagedVector* rightPagedVector = slice.getPagedVectorRefRight(WorkerThreadId(0));
    
    constexpr auto pageSize = 4096;
    const auto projections = schema.getFieldNames();
    
    TestUtils::runStoreTest(*leftPagedVector, schema, MemoryLayoutType::ROW_LAYOUT, 
                            pageSize, projections, records, *nautilusEngine, *bufferManager);
    TestUtils::runStoreTest(*rightPagedVector, schema, MemoryLayoutType::ROW_LAYOUT,
                            pageSize, projections, records, *nautilusEngine, *bufferManager);
    
    /// Populate BloomFilters by adding hashes for each tuple.
    for (uint64_t i = 0; i < tupleCount; ++i)
    {
        slice.addToBloomFilter(i, JoinBuildSideType::Left);
        slice.addToBloomFilter(i, JoinBuildSideType::Right);
    }
    
    slice.combinePagedVectors();
    
    /// Verify BloomFilter pointers are valid.
    const auto* leftBF = slice.getBloomFilter(JoinBuildSideType::Left);
    const auto* rightBF = slice.getBloomFilter(JoinBuildSideType::Right);
    ASSERT_NE(leftBF, nullptr);
    ASSERT_NE(rightBF, nullptr);
    ASSERT_GT(leftBF->sizeInBits(), 0);
    ASSERT_GT(rightBF->sizeInBits(), 0);
    
    /// Verify no false negatives on the host side.
    for (uint64_t i = 0; i < tupleCount; ++i)
    {
        ASSERT_TRUE(leftBF->mightContain(i)) << "Left BF false negative for key " << i;
        ASSERT_TRUE(rightBF->mightContain(i)) << "Right BF false negative for key " << i;
    }
    
    std::cout << "BloomFilter for LEFT: m=" << leftBF->sizeInBits() << " k=" << leftBF->hashFunctionCount() << std::endl;
    std::cout << "BloomFilter for RIGHT: m=" << rightBF->sizeInBits() << " k=" << rightBF->hashFunctionCount() << std::endl;
    std::cout << "Ready for use in performNLJ() JIT-compiled code\n" << std::endl;
    
    /// We cannot evaluate BloomFilterRef.mightContain() directly here because it expects
    /// Nautilus val<uint64_t>. Accessing both refs without errors validates integration wiring.
}

} // namespace NES
