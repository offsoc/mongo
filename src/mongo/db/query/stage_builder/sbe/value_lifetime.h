/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo::stage_builder {

/**
 * Class encapsulating the logic for preserving the lifetime of local values in the SBE VM.
 */
class ValueLifetime {
public:
    /**
     * The possible types of value lifetime that we are interested into tracking.
     * GlobalValue: the value is stored and owned by a slot external to the VM
     * LocalValue: the value is produced by a VM instruction/builtin and it's
     *             going to be released when removed from the VM stack
     * Reference: the value is a non-owned copy of one of the two types above
     */
    enum class ValueType { GlobalValue, LocalValue, Reference };

    ValueLifetime() = default;

    // Recursively walk the provided node and insert the needed operators to ensure that the
    // ownership of the values is properly maintained.
    void validate(optimizer::ABT& node);

    template <typename T, size_t... I>
    void visitChildren(optimizer::ABT& n, T&& op, std::index_sequence<I...>) {
        (op.template get<I>().visit(*this), ...);
    }

    // The default visitor.
    template <int Arity>
    ValueType operator()(optimizer::ABT& n, optimizer::ABTOpFixedArity<Arity>& op) {
        visitChildren(n, op, std::make_index_sequence<Arity>{});
        return ValueType::LocalValue;
    }

    ValueType operator()(optimizer::ABT& node, optimizer::Constant& value);
    ValueType operator()(optimizer::ABT& n, optimizer::Variable& var);

    ValueType operator()(optimizer::ABT& n, optimizer::LambdaAbstraction& lambda);

    ValueType operator()(optimizer::ABT& n, optimizer::Let& let);

    ValueType operator()(optimizer::ABT& n, optimizer::BinaryOp& op);

    ValueType operator()(optimizer::ABT& n, optimizer::FunctionCall& op);

    ValueType operator()(optimizer::ABT& n, optimizer::If& op);

    bool modified() const {
        return _changed;
    }

private:
    // Replace the reference to a node with a reference to a makeOwn(node) call.
    void wrapNode(optimizer::ABT& node);

    using VariableTypes = stdx::
        unordered_map<optimizer::ProjectionName, ValueType, optimizer::ProjectionName::Hasher>;

    // Helper function that manipulates the tree.
    void swapAndUpdate(optimizer::ABT& n, optimizer::ABT newN);

    // Keep track of whether the tree was modified in place.
    bool _changed = false;

    // Keep track of the type of variables.
    VariableTypes _bindings;
};

}  // namespace mongo::stage_builder
