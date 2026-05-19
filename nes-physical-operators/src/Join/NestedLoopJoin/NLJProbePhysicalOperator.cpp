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

#include <Join/NestedLoopJoin/NLJProbePhysicalOperator.hpp>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>
#include <iostream>
#include <Functions/PhysicalFunction.hpp>
#include <Identifiers/Identifiers.hpp>
#include <Join/NestedLoopJoin/NLJOperatorHandler.hpp>
#include <Join/NestedLoopJoin/NLJSlice.hpp>
#include <Join/StreamJoinProbePhysicalOperator.hpp>
#include <Join/StreamJoinUtil.hpp>
#include <Nautilus/Interface/BufferRef/TupleBufferRef.hpp>
#include <Nautilus/Interface/Hash/BloomFilterRef.hpp>
#include <Nautilus/Interface/Hash/HashFunction.hpp>
#include <Nautilus/Interface/Hash/MurMur3HashFunction.hpp>
#include <Nautilus/Interface/NESStrongTypeRef.hpp>
#include <Nautilus/Interface/PagedVector/PagedVectorRef.hpp>
#include <Nautilus/Interface/Record.hpp>
#include <Nautilus/Interface/RecordBuffer.hpp>
#include <Nautilus/Interface/TimestampRef.hpp>
#include <Runtime/Execution/OperatorHandler.hpp>
#include <SliceStore/Slice.hpp>
#include <SliceStore/WindowSlicesStoreInterface.hpp>
#include <Time/Timestamp.hpp>
#include <Windowing/WindowMetaData.hpp>
#include <nautilus/val_enum.hpp>
#include <ErrorHandling.hpp>
#include <ExecutionContext.hpp>
#include <function.hpp>
#include <val.hpp>
#include <val_ptr.hpp>

namespace NES
{

namespace
{
NLJSlice* getNLJSliceRefFromEndProxy(OperatorHandler* ptrOpHandler, const SliceEnd sliceEnd)
{
    PRECONDITION(ptrOpHandler != nullptr, "op handler context should not be null");
    const auto* opHandler = dynamic_cast<NLJOperatorHandler*>(ptrOpHandler);

    auto slice = opHandler->getSliceAndWindowStore().getSliceBySliceEnd(sliceEnd);
    INVARIANT(slice.has_value(), "Could not find a slice for slice end {}", sliceEnd);

    return dynamic_cast<NLJSlice*>(slice.value().get());
}

Timestamp getNLJWindowStartProxy(const EmittedNLJWindowTrigger* nljWindowTriggerTask)
{
    PRECONDITION(nljWindowTriggerTask, "nljWindowTriggerTask should not be null");
    return nljWindowTriggerTask->windowInfo.windowStart;
}

Timestamp getNLJWindowEndProxy(const EmittedNLJWindowTrigger* nljWindowTriggerTask)
{
    PRECONDITION(nljWindowTriggerTask, "nljWindowTriggerTask should not be null");
    return nljWindowTriggerTask->windowInfo.windowEnd;
}

SliceEnd getNLJSliceEndProxy(const EmittedNLJWindowTrigger* nljWindowTriggerTask, const JoinBuildSideType joinBuildSideType)
{
    PRECONDITION(nljWindowTriggerTask != nullptr, "nljWindowTriggerTask should not be null");

    switch (joinBuildSideType)
    {
        case JoinBuildSideType::Left:
            return nljWindowTriggerTask->leftSliceEnd;
        case JoinBuildSideType::Right:
            return nljWindowTriggerTask->rightSliceEnd;
    }
    std::unreachable();
}

/// Proxy to get the BloomFilter pointer from a slice.
const Nautilus::Interface::BloomFilter* getBloomFilterProxy(const NLJSlice* slice, JoinBuildSideType joinBuildSide)
{
    PRECONDITION(slice != nullptr, "slice should not be null");
    return slice->getBloomFilter(joinBuildSide);
}
}

NLJProbePhysicalOperator::NLJProbePhysicalOperator(
    OperatorHandlerId operatorHandlerId,
    PhysicalFunction joinFunction,
    WindowMetaData windowMetaData,
    const JoinSchema& joinSchema,
    std::shared_ptr<TupleBufferRef> leftMemoryProvider,
    std::shared_ptr<TupleBufferRef> rightMemoryProvider,
    const std::vector<std::string>& leftKeyFieldNames,
    const std::vector<std::string>& rightKeyFieldNames,
    bool bloomFilterEnabled)
    : StreamJoinProbePhysicalOperator(operatorHandlerId, std::move(joinFunction), WindowMetaData(std::move(windowMetaData)), joinSchema)
    , leftMemoryProvider(std::move(leftMemoryProvider))
    , rightMemoryProvider(std::move(rightMemoryProvider))
    , leftKeyFieldNames(leftKeyFieldNames)
    , rightKeyFieldNames(rightKeyFieldNames)
    , bloomFilterEnabled(bloomFilterEnabled)
{
}

template<typename BloomFilterT>
void NLJProbePhysicalOperator::performNLJ(
    const PagedVectorRef& outerPagedVector,
    const PagedVectorRef& innerPagedVector,
    TupleBufferRef& outerMemoryProvider,
    TupleBufferRef& innerMemoryProvider,
    ExecutionContext& executionCtx,
    const nautilus::val<Timestamp>& windowStart,
    const nautilus::val<Timestamp>& windowEnd,
    const BloomFilterT& innerBloomFilter) const
{
    const auto outerFields = outerMemoryProvider.getAllFieldNames();
    const auto innerFields = innerMemoryProvider.getAllFieldNames();

    /// Determine which key field names to use for outer tuples.
    /// Check whether the outer side is left or right by comparing memory provider addresses.
    const auto& outerKeyFieldNames = (&outerMemoryProvider == leftMemoryProvider.get()) ? leftKeyFieldNames : rightKeyFieldNames;
    
    nautilus::val<uint64_t> outerItemPos(0);
    for (auto outerIt = outerPagedVector.begin(outerFields); outerIt != outerPagedVector.end(outerFields); ++outerIt)
    {
        /// Compute the hash of the outer tuple's join-key fields.
        std::vector<VarVal> outerJoinKeyValues;
        for (const auto& joinKeyFieldName : outerKeyFieldNames)
        {
            PRECONDITION((*outerIt).hasField(joinKeyFieldName), "Join key field should be present in outer record");
            outerJoinKeyValues.push_back((*outerIt).read(joinKeyFieldName));
        }
        
        MurMur3HashFunction murMur3HashFunction;
        const HashFunction& hashFunction = murMur3HashFunction;
        const auto outerJoinKeyHash = hashFunction.calculate(outerJoinKeyValues);
        
        /// Skip this outer tuple if the inner BloomFilter says it is definitely not present.
        /// When BloomFilterT is BloomFilterAlwaysTrue, this check is optimized away.
        if (!innerBloomFilter.mightContain(outerJoinKeyHash))
        {
            ++outerItemPos;
            continue;
        }
        
        /// This outer tuple might match, so probe the inner tuples.
        nautilus::val<uint64_t> innerItemPos(0);
        for (auto innerIt = innerPagedVector.begin(innerFields); innerIt != innerPagedVector.end(innerFields); ++innerIt)
        {
            const auto joinedKeyFields
                = createJoinedRecord(*outerIt, *innerIt, windowStart, windowEnd, outerFields, innerFields);
            if (joinFunction.execute(joinedKeyFields, executionCtx.pipelineMemoryProvider.arena))
            {
                auto outerRecord = outerPagedVector.readRecord(outerItemPos, outerFields);
                auto innerRecord = innerPagedVector.readRecord(innerItemPos, innerFields);
                auto joinedRecord = createJoinedRecord(outerRecord, innerRecord, windowStart, windowEnd, outerFields, innerFields);
                executeChild(executionCtx, joinedRecord);
            }

            innerItemPos = innerItemPos + nautilus::val<uint64_t>{1};
        }
        outerItemPos = outerItemPos + nautilus::val<uint64_t>{1};
    }
}

void NLJProbePhysicalOperator::open(ExecutionContext& executionCtx, RecordBuffer& recordBuffer) const
{
    /// As this operator functions as a scan, we have to set the execution context for this pipeline
    executionCtx.watermarkTs = recordBuffer.getWatermarkTs();
    executionCtx.currentTs = recordBuffer.getCreatingTs();
    executionCtx.sequenceNumber = recordBuffer.getSequenceNumber();
    executionCtx.chunkNumber = recordBuffer.getChunkNumber();
    executionCtx.lastChunk = recordBuffer.isLastChunk();
    executionCtx.originId = recordBuffer.getOriginId();
    openChild(executionCtx, recordBuffer);

    /// Getting all needed info from the recordBuffer
    const auto nljWindowTriggerTaskRef = static_cast<nautilus::val<EmittedNLJWindowTrigger*>>(recordBuffer.getMemArea());
    const auto sliceIdLeft
        = invoke(getNLJSliceEndProxy, nljWindowTriggerTaskRef, nautilus::val<JoinBuildSideType>(JoinBuildSideType::Left));
    const auto sliceIdRight
        = invoke(getNLJSliceEndProxy, nljWindowTriggerTaskRef, nautilus::val<JoinBuildSideType>(JoinBuildSideType::Right));
    const auto windowStart = invoke(getNLJWindowStartProxy, nljWindowTriggerTaskRef);
    const auto windowEnd = invoke(getNLJWindowEndProxy, nljWindowTriggerTaskRef);

    /// During triggering the slice, we append all pages of all local copies to a single PagedVector located at position 0
    const auto workerThreadIdForPages = nautilus::val<WorkerThreadId>(WorkerThreadId(0));

    /// Getting the left and right paged vector
    const auto operatorHandlerMemRef = executionCtx.getGlobalOperatorHandler(operatorHandlerId);
    const auto sliceRefLeft = invoke(getNLJSliceRefFromEndProxy, operatorHandlerMemRef, sliceIdLeft);
    const auto sliceRefRight = invoke(getNLJSliceRefFromEndProxy, operatorHandlerMemRef, sliceIdRight);

    const auto leftPagedVectorRef = invoke(
        +[](const NLJSlice* nljSlice, const WorkerThreadId workerThreadId, const JoinBuildSideType joinBuildSide)
        {
            PRECONDITION(nljSlice != nullptr, "nlj slice pointer should not be null!");
            return nljSlice->getPagedVectorRef(workerThreadId, joinBuildSide);
        },
        sliceRefLeft,
        workerThreadIdForPages,
        nautilus::val<JoinBuildSideType>(JoinBuildSideType::Left));
    const auto rightPagedVectorRef = invoke(
        +[](const NLJSlice* nljSlice, const WorkerThreadId workerThreadId, const JoinBuildSideType joinBuildSide)
        {
            PRECONDITION(nljSlice != nullptr, "nlj slice pointer should not be null!");
            return nljSlice->getPagedVectorRef(workerThreadId, joinBuildSide);
        },
        sliceRefRight,
        workerThreadIdForPages,
        nautilus::val<JoinBuildSideType>(JoinBuildSideType::Right));

    const PagedVectorRef leftPagedVector(leftPagedVectorRef, leftMemoryProvider);
    const PagedVectorRef rightPagedVector(rightPagedVectorRef, rightMemoryProvider);
    const auto numberOfTuplesLeft = leftPagedVector.getNumberOfTuples();
    const auto numberOfTuplesRight = rightPagedVector.getNumberOfTuples();

    /// Outer loop should have fewer tuples for better performance.
    /// Use BloomFilterRef when bloom filter is enabled, BloomFilterAlwaysTrue otherwise.
    if (bloomFilterEnabled)
    {
        if (numberOfTuplesLeft < numberOfTuplesRight)
        {
            auto rightBloomFilterPtr = invoke(getBloomFilterProxy, sliceRefRight, nautilus::val<JoinBuildSideType>(JoinBuildSideType::Right));
            const Nautilus::Interface::BloomFilterRef bloomFilter(rightBloomFilterPtr);
            performNLJ(leftPagedVector, rightPagedVector, *leftMemoryProvider, *rightMemoryProvider,
                       executionCtx, windowStart, windowEnd, bloomFilter);
        }
        else
        {
            auto leftBloomFilterPtr = invoke(getBloomFilterProxy, sliceRefLeft, nautilus::val<JoinBuildSideType>(JoinBuildSideType::Left));
            const Nautilus::Interface::BloomFilterRef bloomFilter(leftBloomFilterPtr);
            performNLJ(rightPagedVector, leftPagedVector, *rightMemoryProvider, *leftMemoryProvider,
                       executionCtx, windowStart, windowEnd, bloomFilter);
        }
    }
    else
    {
        const Nautilus::Interface::BloomFilterAlwaysTrue noFilter;
        if (numberOfTuplesLeft < numberOfTuplesRight)
        {
            performNLJ(leftPagedVector, rightPagedVector, *leftMemoryProvider, *rightMemoryProvider,
                       executionCtx, windowStart, windowEnd, noFilter);
        }
        else
        {
            performNLJ(rightPagedVector, leftPagedVector, *rightMemoryProvider, *leftMemoryProvider,
                       executionCtx, windowStart, windowEnd, noFilter);
        }
    }
}

/// Explicit template instantiations for the two bloom filter variants.
template void NLJProbePhysicalOperator::performNLJ<Nautilus::Interface::BloomFilterRef>(
    const PagedVectorRef&, const PagedVectorRef&, TupleBufferRef&, TupleBufferRef&,
    ExecutionContext&, const nautilus::val<Timestamp>&, const nautilus::val<Timestamp>&,
    const Nautilus::Interface::BloomFilterRef&) const;

template void NLJProbePhysicalOperator::performNLJ<Nautilus::Interface::BloomFilterAlwaysTrue>(
    const PagedVectorRef&, const PagedVectorRef&, TupleBufferRef&, TupleBufferRef&,
    ExecutionContext&, const nautilus::val<Timestamp>&, const nautilus::val<Timestamp>&,
    const Nautilus::Interface::BloomFilterAlwaysTrue&) const;

}
