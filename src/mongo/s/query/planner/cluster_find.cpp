/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/s/query/planner/cluster_find.h"

#include "mongo/db/query/query_stats/query_stats.h"
#include <algorithm>
#include <boost/optional.hpp>
#include <chrono>
#include <cstdint>
#include <fmt/format.h>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/change_stream_invalidation_info.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/sort_pattern.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/shard_id.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/num_hosts_targeted_metrics.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/collection_uuid_mismatch.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/exec/async_results_merger.h"
#include "mongo/s/query/exec/async_results_merger_params_gen.h"
#include "mongo/s/query/exec/cluster_client_cursor.h"
#include "mongo/s/query/exec/cluster_client_cursor_guard.h"
#include "mongo/s/query/exec/cluster_client_cursor_impl.h"
#include "mongo/s/query/exec/cluster_client_cursor_params.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/s/query/exec/cluster_query_result.h"
#include "mongo/s/query/exec/establish_cursors.h"
#include "mongo/s/query_analysis_sampler_util.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/transaction_router.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {
// Ticks for server-side Javascript deprecation log messages.
Rarely _samplerFunctionJs, _samplerWhereClause;

using namespace fmt::literals;

static const BSONObj kSortKeyMetaProjection = BSON("$meta"
                                                   << "sortKey");
static const BSONObj kGeoNearDistanceMetaProjection = BSON("$meta"
                                                           << "geoNearDistance");

const char kFindCmdName[] = "find";

std::unique_ptr<FindCommandRequest> makeFindCommandForShards(OperationContext* opCtx,
                                                             const std::set<ShardId>& shardIds,
                                                             const CanonicalQuery& query,
                                                             const boost::optional<UUID> sampleId,
                                                             bool requestQueryStatsFromRemotes,
                                                             const UUID& opKey) {
    std::unique_ptr<FindCommandRequest> findCommand;
    if (shardIds.size() > 1) {
        findCommand = uassertStatusOK(ClusterFind::transformQueryForShards(query));
    } else {
        // Forwards the FindCommandRequest as is to a single shard so that limit and skip can
        // be applied on mongod.
        findCommand = std::make_unique<FindCommandRequest>(query.getFindCommandRequest());
    }

    // Reset the input request's generic arguments and only set the ones needed for the query.
    // TODO: SERVER-90827 Only reset arguments not suitable for passing through to shards.
    GenericArguments args;
    std::swap(findCommand->getGenericArguments(), args);
    findCommand->setUnwrappedReadPref(std::move(args.getUnwrappedReadPref()));
    findCommand->setMaxTimeMS(args.getMaxTimeMS());
    findCommand->setReadConcern(std::move(args.getReadConcern()));

    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    if (readConcernArgs.wasAtClusterTimeSelected()) {
        // If mongos selected atClusterTime or received it from client, transmit it to shard.
        findCommand->setReadConcern(readConcernArgs);
    }

    query.getExpCtx()->initializeReferencedSystemVariables();

    // Replace the 'letParams' expressions with their values.
    if (auto letParams = findCommand->getLet()) {
        const auto& vars = query.getExpCtx()->variables;
        const auto& vps = query.getExpCtx()->variablesParseState;
        findCommand->setLet(vars.toBSON(vps, *letParams));
    }

    // ExpressionContext may contain query settings that were looked up in QuerySettingsManager.
    // Propagate it to the shards.
    if (!query.getExpCtx()->getQuerySettings().toBSON().isEmpty()) {
        findCommand->setQuerySettings(query.getExpCtx()->getQuerySettings());
    }

    // Request metrics if necessary.
    {
        // We'll set includeQueryStatsMetrics if our configuration (e.g., feature flag, sample
        // rate) dictates we should gather metrics, or the user sent the flag to us.
        auto origValue = query.getFindCommandRequest().getIncludeQueryStatsMetrics();
        if (origValue.value_or(false) || requestQueryStatsFromRemotes) {
            findCommand->setIncludeQueryStatsMetrics(true);
        }
    }

    // Only set lsid and txnNumber here. Other transaction-related arguments such as
    // startTransaction will be appended later as needed by the transaction machinery. We don't
    // necessarily want to forward all transaction arguments directly from the input request since
    // we may have already started a transaction for internal purposes (e.g. FLE does this).
    if (auto& lsid = opCtx->getLogicalSessionId()) {
        findCommand->setLsid(generic_argument_util::toLogicalSessionFromClient(*lsid));
    }
    findCommand->setTxnNumber(opCtx->getTxnNumber());
    findCommand->setClientOperationKey(opKey);

    return findCommand;
}

/**
 * Constructs the shard requests (ShardId, BSONObj) pairs for the find command by attaching the
 * shardVersion, txnNumber and sampleId if necessary.
 */
std::vector<AsyncRequestsSender::Request> constructRequestsForShards(
    OperationContext* opCtx,
    const CollectionRoutingInfo& cri,
    const std::set<ShardId>& shardIds,
    const CanonicalQuery& query,
    const boost::optional<UUID> sampleId,
    bool requestQueryStatsFromRemotes,
    const auto& opKey) {
    const auto& cm = cri.cm;

    // Choose the shard to sample the query on if needed.
    const auto sampleShardId = sampleId
        ? boost::make_optional(analyze_shard_key::getRandomShardId(shardIds))
        : boost::none;

    // Helper methods for appending additional attributes to the shard command.
    auto appendShardVersion = [&](const auto& shardId, auto& cmdBuilder) {
        if (cm.hasRoutingTable()) {
            cri.getShardVersion(shardId).serialize(ShardVersion::kShardVersionField, &cmdBuilder);
        } else if (!query.nss().isOnInternalDb()) {
            ShardVersion::UNSHARDED().serialize(ShardVersion::kShardVersionField, &cmdBuilder);
            cmdBuilder.append("databaseVersion", cm.dbVersion().toBSON());
        }
    };

    auto appendSampleId = [&](const auto& shardId, auto& cmdBuilder) {
        if (shardId == sampleShardId) {
            analyze_shard_key::appendSampleId(&cmdBuilder, *sampleId);
        }
    };

    // Constructs the shard request by appending additional attributes to the serialized
    // 'findCommandToForward'.
    const auto findCommandToForward = makeFindCommandForShards(
        opCtx, shardIds, query, sampleId, requestQueryStatsFromRemotes, opKey);

    auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    auto makeShardRequest = [&](const auto& shardId) {
        const auto shard = uassertStatusOK(shardRegistry->getShard(opCtx, shardId));
        invariant(!shard->isConfig() || shard->getConnString());

        BSONObjBuilder cmdBuilder;
        findCommandToForward->serialize(&cmdBuilder);
        appendShardVersion(shardId, cmdBuilder);
        appendSampleId(shardId, cmdBuilder);

        return AsyncRequestsSender::Request(shardId, cmdBuilder.obj(), std::move(shard));
    };

    std::vector<AsyncRequestsSender::Request> requests;
    requests.reserve(shardIds.size());
    std::transform(
        shardIds.begin(), shardIds.end(), std::back_inserter(requests), makeShardRequest);
    return requests;
}

void updateNumHostsTargetedMetrics(OperationContext* opCtx,
                                   const ChunkManager& cm,
                                   int nTargetedShards) {
    // Note: It is fine to use 'getAproxNShardsOwningChunks' here because the result is only used to
    // update stats.
    int nShardsOwningChunks = cm.hasRoutingTable() ? cm.getAproxNShardsOwningChunks() : 0;
    auto targetType = NumHostsTargetedMetrics::get(opCtx).parseTargetType(
        opCtx, nTargetedShards, nShardsOwningChunks, cm.isSharded());
    NumHostsTargetedMetrics::get(opCtx).addNumHostsTargeted(
        NumHostsTargetedMetrics::QueryType::kFindCmd, targetType);
}

CursorId runQueryWithoutRetrying(OperationContext* opCtx,
                                 const CanonicalQuery& query,
                                 const ReadPreferenceSetting& readPref,
                                 const boost::optional<UUID> sampleId,
                                 const CollectionRoutingInfo& cri,
                                 std::vector<BSONObj>* results,
                                 bool* partialResultsReturned) {
    const auto& cm = cri.cm;

    const auto& findCommand = query.getFindCommandRequest();
    // Get the set of shards on which we will run the query.
    auto shardIds = getTargetedShardsForCanonicalQuery(query, cm);

    bool requestQueryStatsFromRemotes =
        query_stats::shouldRequestRemoteMetrics(CurOp::get(opCtx)->debug());

    // Construct the query and parameters. Defer setting skip and limit here until
    // we determine if the query is targeting multi-shards or a single shard below.
    ClusterClientCursorParams params(
        query.nss(), APIParameters::get(opCtx), readPref, repl::ReadConcernArgs::get(opCtx), [&] {
            if (!opCtx->getLogicalSessionId())
                return OperationSessionInfoFromClient();

            OperationSessionInfoFromClient osi{*opCtx->getLogicalSessionId(),
                                               opCtx->getTxnNumber()};
            if (TransactionRouter::get(opCtx)) {
                osi.setAutocommit(false);
            }
            return osi;
        }());
    params.originatingCommandObj = CurOp::get(opCtx)->opDescription().getOwned();
    params.batchSize = findCommand.getBatchSize();
    params.tailableMode = query_request_helper::getTailableMode(findCommand);
    params.isAllowPartialResults = findCommand.getAllowPartialResults();
    params.originatingPrivileges = {
        Privilege(ResourcePattern::forExactNamespace(query.nss()), ActionType::find)};
    params.requestQueryStatsFromRemotes = requestQueryStatsFromRemotes;

    // This is the batchSize passed to each subsequent getMore command issued by the cursor. We
    // usually use the batchSize associated with the initial find, but as it is illegal to send a
    // getMore with a batchSize of 0, we set it to use the default batchSize logic.
    if (params.batchSize && *params.batchSize == 0) {
        params.batchSize = boost::none;
    }

    // $natural sort is actually a hint to use a collection scan, and shouldn't be treated like a
    // sort on mongos. Including a $natural anywhere in the sort spec results in the whole sort
    // being considered a hint to use a collection scan.
    BSONObj sortComparatorObj;
    if (query.getSortPattern() && !findCommand.getSort()[query_request_helper::kNaturalSortField]) {
        // We have already validated the input sort object. Serialize the raw sort spec into one
        // suitable for use as the ordering specification in BSONObj::woCompare(). In particular, we
        // want to eliminate sorts using expressions (like $meta) and replace them with a
        // placeholder. When mongos performs a merge-sort, any $meta expressions have already been
        // performed on the shards. Mongos just needs to know the length of the sort pattern and
        // whether each part of the sort pattern is ascending or descending.
        sortComparatorObj = query.getSortPattern()
                                ->serialize(SortPattern::SortKeySerialization::kForSortKeyMerging)
                                .toBson();
    }

    bool compareWholeSortKeyOnRouter = false;
    if (!query.getSortPattern() &&
        QueryPlannerCommon::hasNode(query.getPrimaryMatchExpression(), MatchExpression::GEO_NEAR)) {
        // There is no specified sort, and there is a GEO_NEAR node. This means we should merge sort
        // by the geoNearDistance. Indicate to the AsyncResultsMerger that it should extract the
        // sort key
        // {"$sortKey": <geoNearDistance>} and sort by the order {"$sortKey": 1}.
        sortComparatorObj = AsyncResultsMerger::kWholeSortKeySortPattern;
        compareWholeSortKeyOnRouter = true;
    }

    // Tailable cursors can't have a sort, which should have already been validated.
    tassert(4457013,
            "tailable cursor unexpectedly has a sort",
            sortComparatorObj.isEmpty() || !findCommand.getTailable());

    try {
        // Establish the cursors with a consistent shardVersion across shards.

        // If we have maxTimeMS and allowPartialResults, then leave some spare time in the opCtx
        // deadline so that we have time to return partial results before the opCtx is killed.
        auto deadline = opCtx->getDeadline();
        if (findCommand.getAllowPartialResults() && findCommand.getMaxTimeMS()) {
            // Reserve 25% of the time budget (up to 100,000 microseconds max) for processing
            // buffered partial results.
            deadline -=
                Microseconds{std::min(1000LL * findCommand.getMaxTimeMS().get() / 4, 100'000LL)};
            LOGV2_DEBUG(
                5746901,
                0,
                "Setting an earlier artificial deadline because the find allows partial results.",
                "deadline"_attr = deadline);
        }

        // Construct the requests that we will use to establish cursors on the targeted
        // shards, attaching the shardVersion and session info, if necessary. Attach our own
        // OperationKey as well so establishCursors won't copy each request.
        std::vector<OperationKey> opKeys{UUID::gen()};
        auto requests = constructRequestsForShards(
            opCtx, cri, shardIds, query, sampleId, requestQueryStatsFromRemotes, opKeys.front());

        // The call to establishCursors has its own timeout mechanism that is controlled by the
        // opCtx, so we don't expect runWithDeadline to throw a timeout at this level. We use
        // runWithDeadline because it has the side effect of pushing a temporary (artificial)
        // deadline onto the opCtx used by establishCursors.
        opCtx->runWithDeadline(deadline, ErrorCodes::MaxTimeMSExpired, [&]() -> void {
            params.remotes =
                establishCursors(opCtx,
                                 Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                                 query.nss(),
                                 readPref,
                                 std::move(requests),
                                 findCommand.getAllowPartialResults(),
                                 Shard::RetryPolicy::kIdempotent,
                                 std::move(opKeys));
        });
    } catch (const DBException& ex) {
        if (ex.code() == ErrorCodes::CollectionUUIDMismatch &&
            !ex.extraInfo<CollectionUUIDMismatchInfo>()->actualCollection() &&
            !shardIds.count(cm.dbPrimary())) {
            // We received CollectionUUIDMismatch but it does not contain the actual namespace, and
            // we did not attempt to establish a cursor on the primary shard.
            uassertStatusOK(populateCollectionUUIDMismatch(opCtx, ex.toStatus()));
            MONGO_UNREACHABLE;
        }
        throw;
    }

    // Determine whether the cursor we may eventually register will be single- or multi-target.
    const auto cursorType = params.remotes.size() > 1
        ? ClusterCursorManager::CursorType::MultiTarget
        : ClusterCursorManager::CursorType::SingleTarget;

    // Only set skip, limit and sort to be applied to on the router for the multi-shard case. For
    // the single-shard case skip/limit as well as sorts are appled on mongod.
    if (cursorType == ClusterCursorManager::CursorType::MultiTarget) {
        params.skipToApplyOnRouter = findCommand.getSkip();
        params.limit = findCommand.getLimit();
        params.sortToApplyOnRouter = sortComparatorObj;
        params.compareWholeSortKeyOnRouter = compareWholeSortKeyOnRouter;
    }

    // Transfer the established cursors to a ClusterClientCursor.
    auto ccc = ClusterClientCursorImpl::make(
        opCtx, Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(), std::move(params));

    // Retrieve enough data from the ClusterClientCursor for the first batch of results.

    FindCommon::waitInFindBeforeMakingBatch(opCtx, query, &routerWaitInFindBeforeMakingBatch);

    if (findCommand.getAllowPartialResults() &&
        opCtx->checkForInterruptNoAssert().code() == ErrorCodes::MaxTimeMSExpired) {
        // MaxTimeMS is expired in the router, but some remotes may still have outsanding requests.
        // Wait for all remotes to expire their requests.

        // Maximum number of 1ms sleeps to wait for remote cursors to be exhausted.
        constexpr int kMaxAttempts = 10;
        for (int remainingAttempts = kMaxAttempts; !ccc->remotesExhausted(); remainingAttempts--) {
            if (!remainingAttempts) {
                LOGV2_DEBUG(
                    5746900,
                    0,
                    "MaxTimeMSExpired error was seen on the router, but partial results cannot be "
                    "returned because the remotes did not give the expected MaxTimeMS error within "
                    "kMaxAttempts.");
                // Reveal the MaxTimeMSExpired error.
                opCtx->checkForInterrupt();
            }
            stdx::this_thread::sleep_for(stdx::chrono::milliseconds(1));
        }
    }

    auto cursorState = ClusterCursorManager::CursorState::NotExhausted;
    FindCommon::BSONArrayResponseSizeTracker responseSizeTracker;

    // This loop will load enough results from the shards for a full first batch.  At first, these
    // results come from the initial batches that were obtained when establishing cursors, but
    // ClusterClientCursor::next will fetch further results if necessary.
    while (!FindCommon::enoughForFirstBatch(findCommand, results->size())) {
        auto next = uassertStatusOK(ccc->next());
        if (findCommand.getAllowPartialResults()) {
            if (ccc->remotesExhausted()) {
                cursorState = ClusterCursorManager::CursorState::Exhausted;
            }
        }
        if (next.isEOF()) {
            // We reached end-of-stream. If the cursor is not tailable, then we mark it as
            // exhausted. If it is tailable, usually we keep it open (i.e. "NotExhausted") even
            // when we reach end-of-stream. However, if all the remote cursors are exhausted, there
            // is no hope of returning data and thus we need to close the mongos cursor as well.
            if (!ccc->isTailable() || ccc->remotesExhausted()) {
                cursorState = ClusterCursorManager::CursorState::Exhausted;
            }
            break;
        }

        auto nextObj = *next.getResult();

        // If adding this object will cause us to exceed the message size limit, then we stash it
        // for later.
        if (!responseSizeTracker.haveSpaceForNext(nextObj)) {
            ccc->queueResult(nextObj);
            break;
        }

        // Add doc to the batch.
        responseSizeTracker.add(nextObj);
        results->push_back(std::move(nextObj));
    }

    ccc->detachFromOperationContext();

    if (findCommand.getSingleBatch() && !ccc->isTailable()) {
        cursorState = ClusterCursorManager::CursorState::Exhausted;
    }

    auto&& opDebug = CurOp::get(opCtx)->debug();
    // Fill out query exec properties.
    opDebug.nShards = ccc->getNumRemotes();
    opDebug.additiveMetrics.nBatches = 1;

    // If the caller wants to know whether the cursor returned partial results, set it here.
    if (partialResultsReturned) {
        // Missing results can come either from the first batches or from the ccc's later batches.
        *partialResultsReturned = ccc->partialResultsReturned();
    }

    CurOp::get(opCtx)->setEndOfOpMetrics(results->size());
    // If the cursor is exhausted, then there are no more results to return and we don't need to
    // allocate a cursor id.
    if (cursorState == ClusterCursorManager::CursorState::Exhausted) {
        opDebug.cursorExhausted = true;

        if (shardIds.size() > 0) {
            updateNumHostsTargetedMetrics(opCtx, cm, shardIds.size());
        }
        if (const auto remoteMetrics = ccc->takeRemoteMetrics()) {
            opDebug.additiveMetrics.aggregateDataBearingNodeMetrics(*remoteMetrics);
        }
        collectQueryStatsMongos(opCtx, ccc->takeKey());
        return CursorId(0);
    }

    // Register the cursor with the cursor manager for subsequent getMore's.

    auto cursorManager = Grid::get(opCtx)->getCursorManager();
    const auto cursorLifetime = findCommand.getNoCursorTimeout()
        ? ClusterCursorManager::CursorLifetime::Immortal
        : ClusterCursorManager::CursorLifetime::Mortal;
    auto authUser = AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserName();
    collectQueryStatsMongos(opCtx, ccc);

    auto cursorId = uassertStatusOK(cursorManager->registerCursor(
        opCtx, ccc.releaseCursor(), query.nss(), cursorType, cursorLifetime, authUser));

    // Record the cursorID in CurOp.
    opDebug.cursorid = cursorId;

    if (shardIds.size() > 0) {
        updateNumHostsTargetedMetrics(opCtx, cm, shardIds.size());
    }

    return cursorId;
}

/**
 * Populates or re-populates some state of the OperationContext from what's stored on the cursor
 * and/or what's specified on the request.
 */
Status setUpOperationContextStateForGetMore(OperationContext* opCtx,
                                            const GetMoreCommandRequest& cmd,
                                            const ClusterCursorManager::PinnedCursor& cursor) {
    if (auto readPref = cursor->getReadPreference()) {
        ReadPreferenceSetting::get(opCtx) = *readPref;
    }

    if (auto readConcern = cursor->getReadConcern()) {
        // Used to return "atClusterTime" in cursor replies to clients for snapshot reads.
        repl::ReadConcernArgs::get(opCtx) = *readConcern;
    }

    auto apiParamsFromClient = APIParameters::get(opCtx);
    uassert(ErrorCodes::APIMismatchError,
            "API parameter mismatch: getMore used params {}, the cursor-creating command "
            "used {}"_format(apiParamsFromClient.toBSON().toString(),
                             cursor->getAPIParameters().toBSON().toString()),
            apiParamsFromClient == cursor->getAPIParameters());

    // If the originating command had a 'comment' field, we extract it and set it on opCtx. Note
    // that if the 'getMore' command itself has a 'comment' field, we give precedence to it.
    auto comment = cursor->getOriginatingCommand()["comment"];
    if (!opCtx->getComment() && comment) {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        opCtx->setComment(comment.wrap());
    }

    if (cursor->isTailableAndAwaitData()) {
        // For tailable + awaitData cursors, the request may have indicated a maximum amount of time
        // to wait for new data. If not, default it to 1 second.  We track the deadline instead via
        // the 'waitForInsertsDeadline' decoration.
        auto timeout = Milliseconds{cmd.getMaxTimeMS().value_or(1000)};
        awaitDataState(opCtx).waitForInsertsDeadline =
            opCtx->getServiceContext()->getPreciseClockSource()->now() + timeout;
        awaitDataState(opCtx).shouldWaitForInserts = true;
        invariant(cursor->setAwaitDataTimeout(timeout));
    } else if (cmd.getMaxTimeMS()) {
        return {ErrorCodes::BadValue,
                "maxTimeMS can only be used with getMore for tailable, awaitData cursors"};
    } else if (cursor->getLeftoverMaxTimeMicros() < Microseconds::max()) {
        // Be sure to do this only for non-tailable cursors.
        opCtx->setDeadlineAfterNowBy(cursor->getLeftoverMaxTimeMicros(),
                                     ErrorCodes::MaxTimeMSExpired);
    }
    return Status::OK();
}

CursorId earlyExitWithNoResults(OperationContext* opCtx,
                                const auto& query,
                                const auto& findCommand) {
    uassert(CollectionUUIDMismatchInfo(query.nss().dbName(),
                                       *findCommand.getCollectionUUID(),
                                       query.nss().coll().toString(),
                                       boost::none),
            "Database does not exist",
            !findCommand.getCollectionUUID());
    collectQueryStatsMongos(opCtx, std::move(CurOp::get(opCtx)->debug().queryStatsInfo.key));

    return CursorId(0);
}
}  // namespace

const size_t ClusterFind::kMaxRetries = 10;

CursorId ClusterFind::runQuery(OperationContext* opCtx,
                               const CanonicalQuery& query,
                               const ReadPreferenceSetting& readPref,
                               std::vector<BSONObj>* results,
                               bool* partialResultsReturned) {
    CurOp::get(opCtx)->debug().planCacheShapeHash = canonical_query_encoder::computeHash(
        /* Mongos doesn't know beforehand which execution engine will be used, so we use the classic
           encoding method by default. */
        canonical_query_encoder::encodeClassic(query));

    // If the user supplied a 'partialResultsReturned' out-parameter, default it to false here.
    if (partialResultsReturned) {
        *partialResultsReturned = false;
    }

    // We must always have a BSONObj vector into which to output our results.
    invariant(results);

    const auto& findCommand = query.getFindCommandRequest();
    // Projection on the reserved sort key field is illegal in mongos.
    if (findCommand.getProjection().hasField(AsyncResultsMerger::kSortKeyField)) {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "Projection contains illegal field '"
                                << AsyncResultsMerger::kSortKeyField
                                << "': " << findCommand.getProjection());
    }

    // Attempting to establish a resumable query through mongoS is illegal.
    uassert(ErrorCodes::BadValue,
            "Queries on mongoS may not request or provide a resume token",
            !findCommand.getRequestResumeToken() && findCommand.getResumeAfter().isEmpty());

    auto const catalogCache = Grid::get(opCtx)->catalogCache();
    // Try to generate a sample id for this query here instead of inside 'runQueryWithoutRetrying()'
    // since it is incorrect to generate multiple sample ids for a single query.
    const auto sampleId = analyze_shard_key::tryGenerateSampleId(
        opCtx, query.nss(), analyze_shard_key::SampledCommandNameEnum::kFind);

    // Evaluate let params once: not per shard, and not per retry.
    if (auto letParams = findCommand.getLet()) {
        auto* expCtx = query.getExpCtx().get();
        expCtx->variables.seedVariablesWithLetParameters(expCtx, *letParams);
    }

    if (query.getExpCtx()->hasServerSideJs.where && _samplerWhereClause.tick()) {
        LOGV2_WARNING(8996504,
                      "$where is deprecated. For more information, see "
                      "https://www.mongodb.com/docs/manual/reference/operator/query/where/");
    }

    if (query.getExpCtx()->hasServerSideJs.function && _samplerFunctionJs.tick()) {
        LOGV2_WARNING(
            8996505,
            "$function is deprecated. For more information, see "
            "https://www.mongodb.com/docs/manual/reference/operator/aggregation/function/");
    }

    // Re-target and re-send the initial find command to the shards until we have established the
    // shard version.
    for (size_t retries = 1; retries <= kMaxRetries; ++retries) {
        auto swCri = getCollectionRoutingInfoForTxnCmd(opCtx, query.nss());
        if (swCri == ErrorCodes::NamespaceNotFound) {
            // If the database doesn't exist, we successfully return an empty result set without
            // creating a cursor.
            return earlyExitWithNoResults(opCtx, query, findCommand);
        }

        const auto cri = uassertStatusOK(std::move(swCri));

        try {
            return runQueryWithoutRetrying(
                opCtx, query, readPref, sampleId, cri, results, partialResultsReturned);
        } catch (ExceptionFor<ErrorCodes::StaleDbVersion>& ex) {
            if (retries >= kMaxRetries) {
                // Check if there are no retries remaining, so the last received error can be
                // propagated to the caller.
                ex.addContext(str::stream()
                              << "Failed to run query after " << kMaxRetries << " retries");
                throw;
            }

            LOGV2_DEBUG(22839,
                        1,
                        "Received error status for query",
                        "query"_attr = redact(query.toStringShort()),
                        "attemptNumber"_attr = retries,
                        "maxRetries"_attr = kMaxRetries,
                        "error"_attr = redact(ex));

            // Mark database entry in cache as stale.
            Grid::get(opCtx)->catalogCache()->onStaleDatabaseVersion(ex->getDb(),
                                                                     ex->getVersionWanted());

            if (auto txnRouter = TransactionRouter::get(opCtx)) {
                if (!txnRouter.canContinueOnStaleShardOrDbError(kFindCmdName, ex.toStatus())) {
                    throw;
                }

                // Reset the default global read timestamp so the retry's routing table reflects the
                // chunk placement after the refresh (no-op if the transaction is not running with
                // snapshot read concern).
                txnRouter.onStaleShardOrDbError(opCtx, kFindCmdName, ex.toStatus());
                txnRouter.setDefaultAtClusterTime(opCtx);
            }

        } catch (DBException& ex) {
            if (retries >= kMaxRetries) {
                // Check if there are no retries remaining, so the last received error can be
                // propagated to the caller.
                ex.addContext(str::stream()
                              << "Failed to run query after " << kMaxRetries << " retries");
                throw;
            } else if (!ErrorCodes::isStaleShardVersionError(ex.code()) &&
                       ex.code() != ErrorCodes::ShardNotFound) {

                if (ErrorCodes::isRetriableError(ex.code())) {
                    ex.addContext("Encountered retryable error during query");
                } else {
                    // Errors other than stale metadata or from trying to reach a non existent shard
                    // are fatal to the operation. Network errors and replication retries happen at
                    // the level of the AsyncResultsMerger.
                    ex.addContext("Encountered non-retryable error during query");
                }
                throw;
            }

            LOGV2_DEBUG(22840,
                        1,
                        "Received error status for query",
                        "query"_attr = redact(query.toStringShort()),
                        "attemptNumber"_attr = retries,
                        "maxRetries"_attr = kMaxRetries,
                        "error"_attr = redact(ex));

            if (auto staleInfo = ex.extraInfo<StaleConfigInfo>()) {
                catalogCache->invalidateShardOrEntireCollectionEntryForShardedCollection(
                    query.nss(), staleInfo->getVersionWanted(), staleInfo->getShardId());
            } else {
                catalogCache->invalidateCollectionEntry_LINEARIZABLE(query.nss());
            }

            if (auto txnRouter = TransactionRouter::get(opCtx)) {
                if (!txnRouter.canContinueOnStaleShardOrDbError(kFindCmdName, ex.toStatus())) {
                    throw;
                }

                // Reset the default global read timestamp so the retry's routing table reflects the
                // chunk placement after the refresh (no-op if the transaction is not running with
                // snapshot read concern).
                txnRouter.onStaleShardOrDbError(opCtx, kFindCmdName, ex.toStatus());
                txnRouter.setDefaultAtClusterTime(opCtx);
            }
        }
    }

    MONGO_UNREACHABLE
}

/**
 * Validates that the lsid on the OperationContext matches that on the cursor, returning it to the
 * ClusterClusterCursor manager if it does not.
 */
void validateLSID(OperationContext* opCtx,
                  int64_t cursorId,
                  const ClusterCursorManager::PinnedCursor& cursor) {
    if (opCtx->getLogicalSessionId() && !cursor->getLsid()) {
        uasserted(50799,
                  str::stream() << "Cannot run getMore on cursor " << cursorId
                                << ", which was not created in a session, in session "
                                << *opCtx->getLogicalSessionId());
    }

    if (!opCtx->getLogicalSessionId() && cursor->getLsid()) {
        uasserted(50800,
                  str::stream() << "Cannot run getMore on cursor " << cursorId
                                << ", which was created in session " << *cursor->getLsid()
                                << ", without an lsid");
    }

    if (opCtx->getLogicalSessionId() && cursor->getLsid() &&
        (*opCtx->getLogicalSessionId() != *cursor->getLsid())) {
        uasserted(50801,
                  str::stream() << "Cannot run getMore on cursor " << cursorId
                                << ", which was created in session " << *cursor->getLsid()
                                << ", in session " << *opCtx->getLogicalSessionId());
    }
}

/**
 * Validates that the txnNumber on the OperationContext matches that on the cursor, returning it to
 * the ClusterClusterCursor manager if it does not.
 */
void validateTxnNumber(OperationContext* opCtx,
                       int64_t cursorId,
                       const ClusterCursorManager::PinnedCursor& cursor) {
    if (opCtx->getTxnNumber() && !cursor->getTxnNumber()) {
        uasserted(50802,
                  str::stream() << "Cannot run getMore on cursor " << cursorId
                                << ", which was not created in a transaction, in transaction "
                                << *opCtx->getTxnNumber());
    }

    if (!opCtx->getTxnNumber() && cursor->getTxnNumber()) {
        uasserted(50803,
                  str::stream() << "Cannot run getMore on cursor " << cursorId
                                << ", which was created in transaction " << *cursor->getTxnNumber()
                                << ", without a txnNumber");
    }

    if (opCtx->getTxnNumber() && cursor->getTxnNumber() &&
        (*opCtx->getTxnNumber() != *cursor->getTxnNumber())) {
        uasserted(50804,
                  str::stream() << "Cannot run getMore on cursor " << cursorId
                                << ", which was created in transaction " << *cursor->getTxnNumber()
                                << ", in transaction " << *opCtx->getTxnNumber());
    }
}

/**
 * Validates that the OperationSessionInfo (i.e. txnNumber and lsid) on the OperationContext match
 * that stored on the cursor. The cursor is returned to the ClusterCursorManager if it does not.
 */
void validateOperationSessionInfo(OperationContext* opCtx,
                                  int64_t cursorId,
                                  ClusterCursorManager::PinnedCursor* cursor) {
    ScopeGuard returnCursorGuard(
        [cursor] { cursor->returnCursor(ClusterCursorManager::CursorState::NotExhausted); });
    validateLSID(opCtx, cursorId, *cursor);
    validateTxnNumber(opCtx, cursorId, *cursor);
    returnCursorGuard.dismiss();
}

StatusWith<std::unique_ptr<FindCommandRequest>> ClusterFind::transformQueryForShards(
    const CanonicalQuery& query) {
    const FindCommandRequest& findCommand = query.getFindCommandRequest();

    // If there is a limit, we forward the sum of the limit and the skip.
    boost::optional<int64_t> newLimit;
    if (findCommand.getLimit()) {
        long long newLimitValue;
        if (overflow::add(
                *findCommand.getLimit(), findCommand.getSkip().value_or(0), &newLimitValue)) {
            return Status(
                ErrorCodes::Overflow,
                str::stream()
                    << "sum of limit and skip cannot be represented as a 64-bit integer, limit: "
                    << *findCommand.getLimit() << ", skip: " << findCommand.getSkip().value_or(0));
        }
        newLimit = newLimitValue;
    }

    // If there is a sort other than $natural, we send a sortKey meta-projection to the remote node.
    BSONObj newProjection = findCommand.getProjection();
    if (!findCommand.getSort().isEmpty() &&
        !findCommand.getSort()[query_request_helper::kNaturalSortField]) {
        BSONObjBuilder projectionBuilder;
        projectionBuilder.appendElements(findCommand.getProjection());
        projectionBuilder.append(AsyncResultsMerger::kSortKeyField, kSortKeyMetaProjection);
        newProjection = projectionBuilder.obj();
    }

    // There is no specified sort, and there is a GEO_NEAR node. This means we should merge sort
    // by the geoNearDistance. Request the projection {$sortKey: <geoNearDistance>} from the
    // shards.
    if (!query.getSortPattern() &&
        QueryPlannerCommon::hasNode(query.getPrimaryMatchExpression(), MatchExpression::GEO_NEAR)) {
        invariant(findCommand.getSort().isEmpty());
        BSONObjBuilder projectionBuilder;
        projectionBuilder.appendElements(findCommand.getProjection());
        projectionBuilder.append(AsyncResultsMerger::kSortKeyField, kGeoNearDistanceMetaProjection);
        newProjection = projectionBuilder.obj();
    }

    std::unique_ptr<FindCommandRequest> newQR = std::make_unique<FindCommandRequest>(findCommand);
    newQR->setProjection(newProjection);
    newQR->setSkip(boost::none);
    newQR->setLimit(newLimit);

    // Even if the client sends us singleBatch=true, we may need to retrieve
    // multiple batches from a shard in order to return the single requested batch to the client.
    // Therefore, we must always send singleBatch=false to the shards.
    newQR->setSingleBatch(false);

    // Any expansion of the 'showRecordId' flag should have already happened on mongos.
    if (newQR->getShowRecordId())
        newQR->setShowRecordId(false);

    uassertStatusOK(query_request_helper::validateFindCommandRequest(*newQR));
    return std::move(newQR);
}

StatusWith<CursorResponse> ClusterFind::runGetMore(OperationContext* opCtx,
                                                   const GetMoreCommandRequest& cmd) {
    auto cursorManager = Grid::get(opCtx)->getCursorManager();

    auto authzSession = AuthorizationSession::get(opCtx->getClient());
    auto authChecker = [&authzSession](const boost::optional<UserName>& userName) -> Status {
        return authzSession->isCoauthorizedWith(userName)
            ? Status::OK()
            : Status(ErrorCodes::Unauthorized, "User not authorized to access cursor");
    };

    NamespaceString nss(NamespaceStringUtil::deserialize(cmd.getDbName(), cmd.getCollection()));
    int64_t cursorId = cmd.getCommandParameter();

    auto pinnedCursor = cursorManager->checkOutCursor(cursorId, opCtx, authChecker);
    if (!pinnedCursor.isOK()) {
        return pinnedCursor.getStatus();
    }
    invariant(cursorId == pinnedCursor.getValue().getCursorId());

    validateOperationSessionInfo(opCtx, cursorId, &pinnedCursor.getValue());

    // Ensure that the client still has the privileges to run the originating command.
    if (!authzSession->isAuthorizedForPrivileges(
            pinnedCursor.getValue()->getOriginatingPrivileges())) {
        uasserted(ErrorCodes::Unauthorized,
                  str::stream() << "not authorized for getMore with cursor id " << cursorId);
    }

    // Set the originatingCommand object and the cursorID in CurOp.
    {
        CurOp::get(opCtx)->debug().nShards = pinnedCursor.getValue()->getNumRemotes();
        CurOp::get(opCtx)->debug().cursorid = cursorId;
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setShouldOmitDiagnosticInformation_inlock(
            lk, pinnedCursor.getValue()->shouldOmitDiagnosticInformation());
        CurOp::get(opCtx)->setOriginatingCommand_inlock(
            pinnedCursor.getValue()->getOriginatingCommand());
        CurOp::get(opCtx)->setGenericCursor_inlock(pinnedCursor.getValue().toGenericCursor());
    }

    // If the 'failGetMoreAfterCursorCheckout' failpoint is enabled, throw an exception with the
    // specified 'errorCode' value, or ErrorCodes::InternalError if 'errorCode' is omitted.
    failGetMoreAfterCursorCheckout.executeIf(
        [](const BSONObj& data) {
            auto errorCode = (data["errorCode"] ? data["errorCode"].safeNumberLong()
                                                : ErrorCodes::InternalError);
            uasserted(errorCode, "Hit the 'failGetMoreAfterCursorCheckout' failpoint");
        },
        [&opCtx, nss](const BSONObj& data) {
            auto dataForFailCommand =
                data.addField(BSON("failCommands" << BSON_ARRAY("getMore")).firstElement());
            auto* getMoreCommand = CommandHelpers::findCommand(opCtx, "getMore");
            return CommandHelpers::shouldActivateFailCommandFailPoint(
                dataForFailCommand, nss, getMoreCommand, opCtx->getClient());
        });

    // If the 'waitAfterPinningCursorBeforeGetMoreBatch' fail point is enabled, set the 'msg'
    // field of this operation's CurOp to signal that we've hit this point.
    if (MONGO_unlikely(waitAfterPinningCursorBeforeGetMoreBatch.shouldFail())) {
        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &waitAfterPinningCursorBeforeGetMoreBatch,
            opCtx,
            "waitAfterPinningCursorBeforeGetMoreBatch");
    }

    auto opCtxSetupStatus =
        setUpOperationContextStateForGetMore(opCtx, cmd, pinnedCursor.getValue());
    if (!opCtxSetupStatus.isOK()) {
        return opCtxSetupStatus;
    }

    std::vector<BSONObj> batch;
    FindCommon::BSONArrayResponseSizeTracker responseSizeTracker;
    long long batchSize = cmd.getBatchSize().value_or(0);
    auto cursorState = ClusterCursorManager::CursorState::NotExhausted;
    BSONObj postBatchResumeToken;
    bool stashedResult = false;

    // If the 'waitWithPinnedCursorDuringGetMoreBatch' fail point is enabled, set the 'msg'
    // field of this operation's CurOp to signal that we've hit this point.
    if (MONGO_unlikely(waitWithPinnedCursorDuringGetMoreBatch.shouldFail())) {
        CurOpFailpointHelpers::waitWhileFailPointEnabled(&waitWithPinnedCursorDuringGetMoreBatch,
                                                         opCtx,
                                                         "waitWithPinnedCursorDuringGetMoreBatch");
    }

    while (!FindCommon::enoughForGetMore(batchSize, batch.size())) {
        StatusWith<ClusterQueryResult> next =
            Status{ErrorCodes::InternalError, "uninitialized cluster query result"};
        try {
            next = pinnedCursor.getValue()->next();
        } catch (const ExceptionFor<ErrorCodes::CloseChangeStream>&) {
            // This exception is thrown when a $changeStream stage encounters an event
            // that invalidates the cursor. We should close the cursor and return without
            // error.
            cursorState = ClusterCursorManager::CursorState::Exhausted;
            break;
        } catch (const ExceptionFor<ErrorCodes::ChangeStreamInvalidated>& ex) {
            // This exception is thrown when a change-stream cursor is invalidated. Set the PBRT
            // to the resume token of the invalidating event, and mark the cursor response as
            // invalidated. We always expect to have ExtraInfo for this error code.
            const auto extraInfo = ex.extraInfo<ChangeStreamInvalidationInfo>();
            tassert(5493707, "Missing ChangeStreamInvalidationInfo on exception", extraInfo);

            postBatchResumeToken = extraInfo->getInvalidateResumeToken();
            cursorState = ClusterCursorManager::CursorState::Exhausted;
            break;
        }

        if (!next.isOK()) {
            if (next.getStatus() == ErrorCodes::MaxTimeMSExpired &&
                pinnedCursor.getValue()->partialResultsReturned()) {
                // Break to return partial results rather than return a MaxTimeMSExpired error
                cursorState = ClusterCursorManager::CursorState::Exhausted;
                LOGV2_DEBUG(5746903,
                            0,
                            "Attempting to return partial results because MaxTimeMS expired and "
                            "the query set AllowPartialResults.");
                break;
            }
            return next.getStatus();
        }

        if (next.getValue().isEOF()) {
            // We reached end-of-stream. If the cursor is not tailable, then we mark it as
            // exhausted. If it is tailable, usually we keep it open (i.e. "NotExhausted") even when
            // we reach end-of-stream. However, if all the remote cursors are exhausted, there is no
            // hope of returning data and thus we need to close the mongos cursor as well.
            if (!pinnedCursor.getValue()->isTailable() ||
                pinnedCursor.getValue()->remotesExhausted()) {
                cursorState = ClusterCursorManager::CursorState::Exhausted;
            }
            break;
        }

        if (!responseSizeTracker.haveSpaceForNext(*next.getValue().getResult())) {
            pinnedCursor.getValue()->queueResult(*next.getValue().getResult());
            stashedResult = true;
            break;
        }

        // As soon as we get a result, this operation no longer waits.
        awaitDataState(opCtx).shouldWaitForInserts = false;

        // Add doc to the batch.
        responseSizeTracker.add(*next.getValue().getResult());
        batch.push_back(std::move(*next.getValue().getResult()));

        // Update the postBatchResumeToken. For non-$changeStream aggregations, this will be empty.
        postBatchResumeToken = pinnedCursor.getValue()->getPostBatchResumeToken();
    }

    // If the cursor has been exhausted, we will communicate this by returning a CursorId of zero.
    auto idToReturn =
        (cursorState == ClusterCursorManager::CursorState::Exhausted ? CursorId(0) : cursorId);

    // For empty batches, or in the case where the final result was added to the batch rather than
    // being stashed, we update the PBRT here to ensure that it is the most recent available.
    if (idToReturn && !stashedResult) {
        postBatchResumeToken = pinnedCursor.getValue()->getPostBatchResumeToken();
    }

    auto&& opDebug = CurOp::get(opCtx)->debug();
    // Set nReturned and whether the cursor has been exhausted.
    opDebug.cursorExhausted = (idToReturn == 0);
    opDebug.additiveMetrics.nBatches = 1;
    CurOp::get(opCtx)->setEndOfOpMetrics(batch.size());

    const bool partialResultsReturned = pinnedCursor.getValue()->partialResultsReturned();
    pinnedCursor.getValue()->setLeftoverMaxTimeMicros(opCtx->getRemainingMaxTimeMicros());
    collectQueryStatsMongos(opCtx, pinnedCursor.getValue());

    // Upon successful completion, transfer ownership of the cursor back to the cursor manager. If
    // the cursor has been exhausted, the cursor manager will clean it up for us.
    pinnedCursor.getValue().returnCursor(cursorState);

    if (MONGO_unlikely(waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch.shouldFail())) {
        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch,
            opCtx,
            "waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch");
    }

    auto atClusterTime = !opCtx->inMultiDocumentTransaction()
        ? repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime()
        : boost::none;
    return CursorResponse(nss,
                          idToReturn,
                          std::move(batch),
                          atClusterTime ? atClusterTime->asTimestamp()
                                        : boost::optional<Timestamp>{},
                          postBatchResumeToken,
                          boost::none,
                          boost::none,
                          boost::none,
                          boost::none,
                          boost::none,
                          partialResultsReturned);
}

}  // namespace mongo
