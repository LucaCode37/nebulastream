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
#include <Join/NestedLoopJoin/NLJBuildPhysicalOperator.hpp>

#include <memory>
#include <string>
#include <utility>
#include <Identifiers/Identifiers.hpp>
#include <Join/NestedLoopJoin/NLJOperatorHandler.hpp>
#include <Join/NestedLoopJoin/NLJSlice.hpp>
#include <Join/StreamJoinBuildPhysicalOperator.hpp>
#include <Join/StreamJoinUtil.hpp>
#include <Nautilus/Interface/BufferRef/TupleBufferRef.hpp>
#include <Nautilus/Interface/Hash/HashFunction.hpp>
#include <Nautilus/Interface/Hash/MurMur3HashFunction.hpp>
#include <Nautilus/Interface/PagedVector/PagedVectorRef.hpp>
#include <Nautilus/Interface/Record.hpp>
#include <Runtime/Execution/OperatorHandler.hpp>
#include <SliceStore/Slice.hpp>
#include <SliceStore/SliceStoreRef.hpp>
#include <Time/Timestamp.hpp>
#include <Watermark/TimeFunction.hpp>
#include <ErrorHandling.hpp>
#include <ExecutionContext.hpp>
#include <WindowBuildPhysicalOperator.hpp>

namespace NES
{
SliceStart getNLJSliceStartProxy(const NLJSlice* nljSlice)
{
    PRECONDITION(nljSlice != nullptr, "nlj slice pointer should not be null!");
    return nljSlice->getSliceStart();
}

SliceEnd getNLJSliceEndProxy(const NLJSlice* nljSlice)
{
    PRECONDITION(nljSlice != nullptr, "nlj slice pointer should not be null!");
    return nljSlice->getSliceEnd();
}

void addJoinKeyHashToSliceProxy(NLJSlice* nljSlice, const uint64_t hash, const JoinBuildSideType joinBuildSide)
{
    PRECONDITION(nljSlice != nullptr, "nlj slice pointer should not be null!");
    nljSlice->addToBloomFilter(hash, joinBuildSide);
}

/// Proxy to retrieve the NLJSlice* for a given timestamp.
/// Slices already exist at this point (created by sliceStoreRef->getDataStructureRef above).
NLJSlice* getNLJSliceForTimestampProxy(OperatorHandler* ptrOpHandler, const Timestamp timestamp)
{
    PRECONDITION(ptrOpHandler != nullptr, "Operator handler pointer should not be null!");
    auto* handler = dynamic_cast<NLJOperatorHandler*>(ptrOpHandler);
    PRECONDITION(handler != nullptr, "Operator handler must be an NLJOperatorHandler!");
    auto& sliceStore = handler->getSliceAndWindowStore();
    const auto slices = sliceStore.getSlicesOrCreate(timestamp, handler->getCreateNewSlicesFunction({}));
    for (const auto& slice : slices)
    {
        if (slice->getSliceStart() <= timestamp && timestamp < slice->getSliceEnd())
        {
            return dynamic_cast<NLJSlice*>(slice.get());
        }
    }
    PRECONDITION(false, "No NLJSlice found for timestamp " + std::to_string(timestamp.getRawValue()));
    return nullptr;
}

NLJBuildPhysicalOperator::NLJBuildPhysicalOperator(
    const OperatorHandlerId operatorHandlerId,
    const JoinBuildSideType joinBuildSide,
    std::unique_ptr<TimeFunction> timeFunction,
    std::shared_ptr<TupleBufferRef> bufferRef,
    std::unique_ptr<SliceStoreRef> sliceStoreRef,
    const std::vector<std::string>& joinKeyFieldNames,
    const bool bloomFilterEnabled)
    : StreamJoinBuildPhysicalOperator{
          operatorHandlerId, joinBuildSide, std::move(timeFunction), std::move(bufferRef), std::move(sliceStoreRef)},
      joinKeyFieldNames(joinKeyFieldNames),
      bloomFilterEnabled(bloomFilterEnabled)
{
}

void NLJBuildPhysicalOperator::execute(ExecutionContext& executionCtx, Record& record) const
{
    /// Getting the operator handler from the local state
    auto* const localState = dynamic_cast<WindowOperatorBuildLocalState*>(executionCtx.getLocalState(id));
    auto operatorHandler = localState->getOperatorHandler();

    /// Get the current slice / pagedVector that we have to insert the tuple into
    const auto timestamp = timeFunction->getTs(executionCtx, record);
    const auto nljPagedVectorMemRef = sliceStoreRef->getDataStructureRef(timestamp, executionCtx.workerThreadId, operatorHandler);

    /// Write record to the pagedVector
    const PagedVectorRef pagedVectorRef(nljPagedVectorMemRef, bufferRef);
    pagedVectorRef.writeRecord(record, executionCtx.pipelineMemoryProvider.bufferProvider);

    /// Compute join-key hash for BloomFilter integration and store it in the slice.
    /// Only create BloomFilter if enabled to avoid unnecessary overhead.
    if (bloomFilterEnabled && !joinKeyFieldNames.empty())
    {
        std::vector<VarVal> joinKeyValues;
        joinKeyValues.reserve(joinKeyFieldNames.size());

        for (const auto& joinKeyFieldName : joinKeyFieldNames)
        {
            PRECONDITION(record.hasField(joinKeyFieldName), "Join key field should be present in record");
            joinKeyValues.push_back(record.read(joinKeyFieldName));
        }

        MurMur3HashFunction murMur3HashFunction;
        const HashFunction& hashFunction = murMur3HashFunction;
        const auto joinKeyHash = hashFunction.calculate(joinKeyValues);

        const auto sliceReference = invoke(getNLJSliceForTimestampProxy, operatorHandler, timestamp);
        invoke(addJoinKeyHashToSliceProxy, sliceReference, joinKeyHash, nautilus::val<JoinBuildSideType>(joinBuildSide));
    }
}
}
