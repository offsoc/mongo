/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/db/query/search/mongot_cursor.h"

#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h"
#include "mongo/db/query/search/internal_search_cluster_parameters_gen.h"
#include "mongo/db/query/search/internal_search_mongot_remote_spec_gen.h"
#include "mongo/db/query/search/mongot_cursor_getmore_strategy.h"
#include "mongo/db/query/search/mongot_options.h"
#include "mongo/db/query/search/search_task_executors.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/server_parameter_with_storage.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::mongot_cursor {
MONGO_FAIL_POINT_DEFINE(shardedSearchOpCtxDisconnect);

namespace {
executor::RemoteCommandRequest getRemoteCommandRequestForSearchQuery(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const boost::optional<UUID>& uuid,
    const boost::optional<ExplainOptions::Verbosity>& explain,
    const BSONObj& query,
    const boost::optional<int> protocolVersion = boost::none,
    const boost::optional<long long> docsRequested = boost::none,
    const boost::optional<long long> batchSize = boost::none,
    const bool requiresSearchSequenceToken = false) {
    BSONObjBuilder cmdBob;
    cmdBob.append(kSearchField, nss.coll());
    uassert(
        6584801,
        str::stream() << "A uuid is required for a search query, but was missing. Got namespace "
                      << nss.toStringForErrorMsg(),
        uuid);
    uuid.value().appendToBuilder(&cmdBob, kCollectionUuidField);
    cmdBob.append(kQueryField, query);
    if (explain) {
        cmdBob.append(kExplainField,
                      BSON(kVerbosityField << ExplainOptions::verbosityString(*explain)));
    }
    if (protocolVersion) {
        cmdBob.append(kIntermediateField, *protocolVersion);
    }

    if (docsRequested.has_value() || batchSize.has_value() || requiresSearchSequenceToken) {
        tassert(
            8953001,
            "Only one of docsRequested or batchSize should be set on the initial mongot request.",
            !docsRequested.has_value() || !batchSize.has_value());

        BSONObjBuilder cursorOptionsBob(cmdBob.subobjStart(kCursorOptionsField));
        if (batchSize.has_value()) {
            cursorOptionsBob.append(kBatchSizeField, batchSize.get());
        }
        if (docsRequested.has_value()) {
            cursorOptionsBob.append(kDocsRequestedField, docsRequested.get());
        }
        if (requiresSearchSequenceToken) {
            // Indicate to mongot that the user wants to paginate so mongot returns pagination
            // tokens alongside the _id values.
            cursorOptionsBob.append(kRequiresSearchSequenceToken, true);
        }
        cursorOptionsBob.doneFast();
    }


    return getRemoteCommandRequest(opCtx, nss, cmdBob.obj());
}

void doThrowIfNotRunningWithMongotHostConfigured() {
    uassert(
        ErrorCodes::SearchNotEnabled,
        str::stream()
            << "Using $search and $vectorSearch aggregation stages requires additional "
            << "configuration. Please connect to Atlas or an AtlasCLI local deployment to enable."
            << "For more information on how to connect, see "
            << "https://dochub.mongodb.org/core/atlas-cli-deploy-local-reqs.",
        globalMongotParams.enabled);
}

long long computeInitialBatchSize(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  const DocsNeededBounds& bounds,
                                  const boost::optional<int64_t> userBatchSize,
                                  bool isStoredSource) {
    // TODO SERVER-63765 Allow cursor establishment for sharded clusters when userBatchSize is 0.
    double oversubscriptionFactor = 1;
    if (!isStoredSource) {
        oversubscriptionFactor =
            ServerParameterSet::getClusterParameterSet()
                ->get<ClusterParameterWithStorage<InternalSearchOptions>>("internalSearchOptions")
                ->getValue(expCtx->ns.tenantId())
                .getOversubscriptionFactor();
    }

    auto applyOversubscription = [oversubscriptionFactor](long long batchSize) -> long long {
        return std::ceil(batchSize * oversubscriptionFactor);
    };

    return visit(OverloadedVisitor{
                     [applyOversubscription](long long minVal, long long maxVal) {
                         return std::max(applyOversubscription(minVal), kMinimumMongotBatchSize);
                     },
                     [applyOversubscription](long long minVal, docs_needed_bounds::Unknown maxVal) {
                         // Since maxVal is Unknown, we don't have a strict extractable limit, so we
                         // should request at least the default batchSize.
                         return std::max(applyOversubscription(minVal), kDefaultMongotBatchSize);
                     },
                     [applyOversubscription](docs_needed_bounds::Unknown minVal, long long maxVal) {
                         return std::clamp(applyOversubscription(maxVal),
                                           kMinimumMongotBatchSize,
                                           kDefaultMongotBatchSize);
                     },
                     [applyOversubscription](auto& minVal, auto& maxVal) {
                         // This is the NeedAll case or the min and max are both Unknown case, so we
                         // use the default batchSize. Above, we don't apply the oversubscription to
                         // the default batchSize since it's used as an upper/lower constraint;
                         // here, it's used here as a true default, so we do apply oversubscription.
                         return applyOversubscription(kDefaultMongotBatchSize);
                     },
                 },
                 bounds.getMinBounds(),
                 bounds.getMaxBounds());
}
}  // namespace

executor::RemoteCommandRequest getRemoteCommandRequest(OperationContext* opCtx,
                                                       const NamespaceString& nss,
                                                       const BSONObj& cmdObj) {
    doThrowIfNotRunningWithMongotHostConfigured();
    auto swHostAndPort = HostAndPort::parse(globalMongotParams.host);
    // This host and port string is configured and validated at startup.
    invariant(swHostAndPort.getStatus().isOK());
    executor::RemoteCommandRequest rcr(
        executor::RemoteCommandRequest(swHostAndPort.getValue(), nss.dbName(), cmdObj, opCtx));
    rcr.sslMode = transport::ConnectSSLMode::kDisableSSL;
    return rcr;
}
// TODO SERVER-91594 makeTaskExecutorCursorForExplain() can be removed when mongot will always
// return a cursor.
std::unique_ptr<executor::TaskExecutorCursor> makeTaskExecutorCursorForExplain(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const executor::RemoteCommandRequest& command,
    std::shared_ptr<executor::TaskExecutor> taskExecutor,
    std::unique_ptr<executor::TaskExecutorCursorGetMoreStrategy> getMoreStrategy,
    std::unique_ptr<PlanYieldPolicy> yieldPolicy) {
    auto response =
        runSearchCommandWithRetries(expCtx, command.cmdObj, makeRetryOnNetworkErrorPolicy());
    auto responseData = response.data;
    // We may query a version of mongot that only returns the explain object. Since creating a
    // TaskExecutorCursor (TEC) with no cursor will error, we manually create a dummy cursor to
    // create the TEC here.
    if (responseData[CursorInitialReply::kCursorFieldName].type() != BSONType::Object) {
        auto nss = expCtx->ns;
        auto cursorBSON =
            BSON(AnyCursor::kCursorIdFieldName
                 << CursorId(0) << AnyCursor::kNsFieldName
                 << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())
                 << AnyCursor::kFirstBatchFieldName << BSONArray());
        BSONObjBuilder cursorBuilder(std::move(responseData));
        cursorBuilder << CursorInitialReply::kCursorFieldName << cursorBSON;
        responseData = cursorBuilder.obj();
    }
    auto cursorResponse = CursorResponse::parseFromBSON(std::move(responseData));
    executor::TaskExecutorCursorOptions options = {std::move(getMoreStrategy),
                                                   std::move(yieldPolicy)};
    return std::make_unique<executor::TaskExecutorCursor>(
        taskExecutor,
        nullptr,
        uassertStatusOK(std::move(cursorResponse)),
        command,
        std::move(options));
}

std::vector<std::unique_ptr<executor::TaskExecutorCursor>> establishCursors(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const executor::RemoteCommandRequest& command,
    std::shared_ptr<executor::TaskExecutor> taskExecutor,
    std::unique_ptr<executor::TaskExecutorCursorGetMoreStrategy> getMoreStrategy,
    std::unique_ptr<PlanYieldPolicy> yieldPolicy) {
    std::vector<std::unique_ptr<executor::TaskExecutorCursor>> cursors;

    std::unique_ptr<executor::TaskExecutorCursor> initialCursor;
    std::vector<std::unique_ptr<executor::TaskExecutorCursor>> additionalCursors;

    if (expCtx->explain) {
        tassert(8431800,
                "Sharded $search queries should not establishCursors() for explain.",
                !expCtx->needsMerge);
        initialCursor = makeTaskExecutorCursorForExplain(
            expCtx, command, taskExecutor, std::move(getMoreStrategy), std::move(yieldPolicy));

    } else {
        initialCursor = makeTaskExecutorCursor(expCtx->opCtx,
                                               taskExecutor,
                                               command,
                                               {std::move(getMoreStrategy), std::move(yieldPolicy)},
                                               makeRetryOnNetworkErrorPolicy());
        additionalCursors = initialCursor->releaseAdditionalCursors();
    }
    cursors.push_back(std::move(initialCursor));
    // Preserve cursor order. Expect cursors to be labeled, so this may not be necessary.
    for (auto& thisCursor : additionalCursors) {
        cursors.push_back(std::move(thisCursor));
    }

    return cursors;
}

std::vector<std::unique_ptr<executor::TaskExecutorCursor>> establishCursorsForSearchStage(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const InternalSearchMongotRemoteSpec& spec,
    std::shared_ptr<executor::TaskExecutor> taskExecutor,
    boost::optional<int64_t> userBatchSize,
    std::function<boost::optional<long long>()> calcDocsNeededFn,
    std::unique_ptr<PlanYieldPolicy> yieldPolicy,
    std::shared_ptr<DocumentSourceInternalSearchIdLookUp::SearchIdLookupMetrics>
        searchIdLookupMetrics) {
    // UUID is required for mongot queries. If not present, no results for the query as the
    // collection has not been created yet.
    if (!expCtx->uuid) {
        return {};
    }

    const auto& query = spec.getMongotQuery();

    auto bounds = spec.getDocsNeededBounds();
    boost::optional<long long> batchSize = boost::none;
    // We should only use batchSize if the batchSize feature flag (featureFlagSearchBatchSizeTuning)
    // is enabled and we've already computed min/max bounds.
    if (feature_flags::gFeatureFlagSearchBatchSizeTuning.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
        bounds.has_value()) {
        const auto storedSourceElem = query[kReturnStoredSourceArg];
        bool isStoredSource = !storedSourceElem.eoo() && storedSourceElem.Bool();
        batchSize = computeInitialBatchSize(expCtx, *bounds, userBatchSize, isStoredSource);
    }

    boost::optional<long long> docsRequested = spec.getMongotDocsRequested().has_value()
        ? boost::make_optional<long long>(*spec.getMongotDocsRequested())
        : boost::none;

    // We disable setting docsRequested if we're already setting batchSize or if
    // the docsRequested feature flag (featureFlagSearchBatchSizeLimit) is disabled.
    // TODO SERVER-74941 Remove docsRequested alongside featureFlagSearchBatchSizeLimit.
    // (Ignore FCV check): This feature is enabled on an earlier FCV.
    if (batchSize.has_value() ||
        !feature_flags::gFeatureFlagSearchBatchSizeLimit.isEnabledAndIgnoreFCVUnsafe()) {
        docsRequested = boost::none;
        // Set calcDocsNeededFn to disable docsRequested for getMore requests.
        calcDocsNeededFn = nullptr;
    } else {
        // If we're enabling the docsRequested option, min/max bounds can be set to the
        // docsRequested value.
        if (docsRequested.has_value()) {
            bounds = DocsNeededBounds(*docsRequested, *docsRequested);
        }
    }

    auto getMoreStrategy = std::make_unique<executor::MongotTaskExecutorCursorGetMoreStrategy>(
        calcDocsNeededFn,
        batchSize,
        bounds.value_or(
            DocsNeededBounds(docs_needed_bounds::Unknown(), docs_needed_bounds::Unknown())),
        expCtx->ns.tenantId(),
        searchIdLookupMetrics);

    // If it turns out that this stage is not running on a sharded collection, we don't want
    // to send the protocol version to mongot. If the protocol version is sent, mongot will
    // generate unmerged metadata documents that we won't be set up to merge.
    const auto& protocolVersion =
        expCtx->needsMerge ? spec.getMetadataMergeProtocolVersion() : boost::none;

    return establishCursors(expCtx,
                            getRemoteCommandRequestForSearchQuery(
                                expCtx->opCtx,
                                expCtx->ns,
                                expCtx->uuid,
                                expCtx->explain,
                                query,
                                protocolVersion,
                                docsRequested,
                                batchSize,
                                spec.getRequiresSearchSequenceToken().value_or(false)),
                            taskExecutor,
                            std::move(getMoreStrategy),
                            std::move(yieldPolicy));
}

std::vector<std::unique_ptr<executor::TaskExecutorCursor>> establishCursorsForSearchMetaStage(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const BSONObj& query,
    std::shared_ptr<executor::TaskExecutor> taskExecutor,
    const boost::optional<int>& protocolVersion,
    std::unique_ptr<PlanYieldPolicy> yieldPolicy) {
    // UUID is required for mongot queries. If not present, no results for the query as the
    // collection has not been created yet.
    if (!expCtx->uuid) {
        return {};
    }

    auto getMoreStrategy = std::make_unique<executor::DefaultTaskExecutorCursorGetMoreStrategy>(
        /*batchSize*/ boost::none,
        /*preFetchNextBatch*/ false);
    return establishCursors(
        expCtx,
        getRemoteCommandRequestForSearchQuery(
            expCtx->opCtx, expCtx->ns, expCtx->uuid, expCtx->explain, query, protocolVersion),
        taskExecutor,
        std::move(getMoreStrategy),
        std::move(yieldPolicy));
}


BSONObj getExplainResponse(const ExpressionContext* expCtx,
                           const executor::RemoteCommandRequest& request,
                           executor::TaskExecutor* taskExecutor) {
    auto [promise, future] = makePromiseFuture<executor::TaskExecutor::RemoteCommandCallbackArgs>();
    auto promisePtr = std::make_shared<Promise<executor::TaskExecutor::RemoteCommandCallbackArgs>>(
        std::move(promise));
    auto scheduleResult = taskExecutor->scheduleRemoteCommand(
        std::move(request), [promisePtr](const auto& args) { promisePtr->emplaceValue(args); });
    if (!scheduleResult.isOK()) {
        // Since the command failed to be scheduled, the callback above did not and will not run.
        // Thus, it is safe to fulfill the promise here without worrying about synchronizing access
        // with the executor's thread.
        promisePtr->setError(scheduleResult.getStatus());
    }
    auto response = future.getNoThrow(expCtx->opCtx);
    uassertStatusOK(response.getStatus());
    uassertStatusOK(response.getValue().response.status);
    BSONObj responseData = response.getValue().response.data;
    uassertStatusOK(getStatusFromCommandResult(responseData));
    auto explain = responseData["explain"];
    uassert(4895000,
            "Response must contain an 'explain' field that is of type 'Object'",
            explain.type() == BSONType::Object);
    return explain.embeddedObject().getOwned();
}

BSONObj getSearchExplainResponse(const ExpressionContext* expCtx,
                                 const BSONObj& query,
                                 executor::TaskExecutor* taskExecutor) {
    const auto request = getRemoteCommandRequestForSearchQuery(
        expCtx->opCtx, expCtx->ns, expCtx->uuid, expCtx->explain, query);
    return getExplainResponse(expCtx, request, taskExecutor);
}

executor::RemoteCommandResponse runSearchCommandWithRetries(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const BSONObj& cmdObj,
    std::function<bool(Status)> retryPolicy) {
    using namespace fmt::literals;
    auto taskExecutor = executor::getMongotTaskExecutor(expCtx->opCtx->getServiceContext());
    executor::RemoteCommandResponse response =
        Status(ErrorCodes::InternalError, "Internal error running search command");
    for (;;) {
        Status err = Status::OK();
        do {
            auto swCbHnd = taskExecutor->scheduleRemoteCommand(
                getRemoteCommandRequest(expCtx->opCtx, expCtx->ns, cmdObj),
                [&](const auto& args) { response = args.response; });
            err = swCbHnd.getStatus();
            if (!err.isOK()) {
                // scheduling error
                err.addContext("Failed to execute search command: {}"_format(cmdObj.toString()));
                break;
            }
            if (MONGO_likely(shardedSearchOpCtxDisconnect.shouldFail())) {
                expCtx->opCtx->markKilled();
            }
            // It is imperative to wrap the wait() call in a try/catch. If an exception is thrown
            // and not caught, planShardedSearch will exit and all stack-allocated variables will be
            // destroyed. Then later when the executor thread tries to run the callbackFn of
            // scheduleRemoteCommand (the lambda above), it will try to access the `response` var,
            // which had been captured by reference and thus lived on the stack and therefore
            // destroyed as part of stack unwinding, and the server will segfault.

            // By catching the exception and then wait-ing for the callbackFn to run, we
            // ensure that planShardedSearch isn't exited (and the `response` object isn't
            // destroyed) before the callbackFn (which has a reference to `response`) executes.
            try {
                taskExecutor->wait(swCbHnd.getValue(), expCtx->opCtx);
            } catch (const DBException& exception) {
                LOGV2_ERROR(8049900,
                            "An interruption occured while the MongotTaskExecutor was waiting for "
                            "a response",
                            "error"_attr = exception.toStatus());
                // If waiting for the response is interrupted, like by a ClientDisconnectError, then
                // we still have a callback-handle out and registered with the TaskExecutor to run
                // when the response finally does come back.

                // Since the callback-handle references local state, cbResponse, it would
                // be invalid for the callback-handle to run after leaving the this function.
                // Therefore, cancel() stops any work associated with the callback handle (eg
                // network work in the case of scheduleRemoteCommand).

                // The contract for executor::scheduleRemoteCommand(....., callbackFn) requires that
                // callbackFn (the lambda in our case) is always run. Thefore after the cancel(), we
                // wait() for the callbackFn to be run with a not-ok status to inform the executor
                // that the original callback handle call (scheduleRemoteCommand) was canceled.
                taskExecutor->cancel(swCbHnd.getValue());
                taskExecutor->wait(swCbHnd.getValue());
                throw;
            }
            err = response.status;
            if (!err.isOK()) {
                // Local error running the command.
                err.addContext("Failed to execute search command: {}"_format(cmdObj.toString()));
                break;
            }
            err = getStatusFromCommandResult(response.data);
            if (!err.isOK()) {
                // Mongot ran the command and returned an error.
                err.addContext("mongot returned an error");
                break;
            }
        } while (0);

        if (err.isOK())
            return response;
        if (!retryPolicy(err))
            uassertStatusOK(err);
    }
}

void throwIfNotRunningWithMongotHostConfigured(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    // We must validate if a mongot is configured. However, we might just be parsing or validating
    // the query without executing it. In this scenario, there is no reason to check if we are
    // running with a mongot configured, since we will never make a call to the mongot host. For
    // example, if we are in query analysis, performing pipeline-style updates, or creating query
    // shapes. Additionally, it would be an error to validate this inside query analysis, since
    // query analysis doesn't have access to the mongot host.
    //
    // This validation should occur before parsing so in the case of a parse and configuration
    // error, the configuration error is thrown.
    if (expCtx->mongoProcessInterface->isExpectedToExecuteQueries()) {
        doThrowIfNotRunningWithMongotHostConfigured();
    }
}
}  // namespace mongo::mongot_cursor
