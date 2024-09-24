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

#pragma once

#include <boost/optional.hpp>
#include <functional>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/tailable_mode.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/query/exec/async_results_merger_params_gen.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

/**
 * The resulting ClusterClientCursor will take ownership of the existing remote cursor, generating
 * results based on the cursor's current state.
 *
 * Note that any results already generated from this cursor will not be returned by the resulting
 * ClusterClientCursor. The caller is responsible for ensuring that results previously generated by
 * this cursor have been processed.
 */
struct ClusterClientCursorParams {
    ClusterClientCursorParams(NamespaceString nss,
                              APIParameters apiParameters,
                              boost::optional<ReadPreferenceSetting> readPreference,
                              boost::optional<repl::ReadConcernArgs> readConcern,
                              OperationSessionInfoFromClient osi);

    /**
     * Extracts the subset of fields here needed by the AsyncResultsMerger. The returned
     * AsyncResultsMergerParams will assume ownership of 'remotes'.
     */
    AsyncResultsMergerParams extractARMParams();

    // Namespace against which the cursors exist.
    NamespaceString nsString;

    // The original command object which generated this cursor. Must either be empty or owned.
    BSONObj originatingCommandObj;

    // The privileges required for the originatingCommand.
    PrivilegeVector originatingPrivileges;

    // Per-remote node data.
    std::vector<RemoteCursor> remotes;

    // The sort specification to be applied on router. Leave empty if there is no sort.
    BSONObj sortToApplyOnRouter;

    // When 'compareWholeSortKeyOnRouter' is true, $sortKey is a scalar value, rather than an
    // object. We extract the sort key {$sortKey: <value>}. The sort key pattern is verified to be
    // {$sortKey: 1}.
    bool compareWholeSortKeyOnRouter = false;

    // The number of results to skip on the router. Optional. Should not be forwarded to the remote
    // hosts in 'cmdObj'.
    boost::optional<long long> skipToApplyOnRouter;

    // The number of results per batch. Optional. If specified, will be specified as the batch for
    // each getMore.
    boost::optional<std::int64_t> batchSize;

    // Limits the number of results returned by the ClusterClientCursor to this many. Optional.
    // Should be forwarded to the remote hosts in 'cmdObj'.
    boost::optional<long long> limit;

    // Whether this cursor is tailing a capped collection, and whether it has the awaitData option
    // set.
    TailableModeEnum tailableMode = TailableModeEnum::kNormal;

    // The API parameters associated with the cursor.
    APIParameters apiParameters;

    // Set if a readPreference must be respected throughout the lifetime of the cursor.
    boost::optional<ReadPreferenceSetting> readPreference;

    // Set if a readConcern must be respected throughout the lifetime of the cursor.
    boost::optional<repl::ReadConcernArgs> readConcern;

    // Session/transaction information to attach with this request (if run under a session or
    // transaction)
    OperationSessionInfoFromClient osi;

    // Whether the client indicated that it is willing to receive partial results in the case of an
    // unreachable host.
    bool isAllowPartialResults = false;

    // Whether to request query stats when making requests to remote hosts.
    bool requestQueryStatsFromRemotes = false;
};

}  // namespace mongo
