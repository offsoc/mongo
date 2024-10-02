// Test $filter aggregation expression.

import "jstests/libs/query/sbe_assert_error_override.js";

import {assertArrayEq, assertErrorCode} from "jstests/aggregation/extras/utils.js";

function runAndAssert(filterSpec, expectedResult) {
    const actualResult = coll.aggregate([{$project: {b: filterSpec}}, {$sort: {_id: 1}}]).toArray();
    assertArrayEq({actual: actualResult, expected: expectedResult});
}

function runAndAssertThrows(filterSpec, expectedErrorCode) {
    const projectSpec = {$project: {b: {$filter: filterSpec}}};
    assertErrorCode(coll, projectSpec, expectedErrorCode);
}

let coll = db.agg_filter_expr;
coll.drop();

assert.commandWorked(coll.insert([
    {_id: 0, c: 1, d: 3, a: [1, 2, 3, 4, 5]},
    {_id: 1, c: 2, d: 4, a: [1, 2]},
    {_id: 2, c: 3, d: 5, a: []},
    {_id: 3, c: 4, d: 6, a: [4]},
    {_id: 4, c: 5, d: 7, a: null},
    {_id: 5, c: 6, d: 8, a: undefined},
    {_id: 6, c: 7, d: 9}
]));

// Create filter to only accept numbers greater than 2.
let filterDoc = {$filter: {input: '$a', as: 'x', cond: {$gt: ['$$x', 2]}}};
let expectedResults = [
    {_id: 0, b: [3, 4, 5]},
    {_id: 1, b: []},
    {_id: 2, b: []},
    {_id: 3, b: [4]},
    {_id: 4, b: null},
    {_id: 5, b: null},
    {_id: 6, b: null},
];
runAndAssert(filterDoc, expectedResults);

filterDoc = {
    $filter: {input: '$a', as: 'x', cond: {$gt: ['$$x', 1]}, limit: {$literal: 3}}
};
expectedResults = [
    {_id: 0, b: [2, 3, 4]},
    {_id: 1, b: [2]},
    {_id: 2, b: []},
    {_id: 3, b: [4]},
    {_id: 4, b: null},
    {_id: 5, b: null},
    {_id: 6, b: null},
];
runAndAssert(filterDoc, expectedResults);

// If we pass null to the 'limit' argument, we interpret the query as being "limit-less" and
// therefore expect to return all matching elements per doc.
filterDoc = {
    $filter: {input: '$a', as: 'x', cond: {$gt: ['$$x', 2]}, limit: null}
};
expectedResults = [
    {_id: 0, b: [3, 4, 5]},
    {_id: 1, b: []},
    {_id: 2, b: []},
    {_id: 3, b: [4]},
    {_id: 4, b: null},
    {_id: 5, b: null},
    {_id: 6, b: null},
];
runAndAssert(filterDoc, expectedResults);

// The 'limit' argument must be greater than zero and fit into int32 type. Otherwise, we throw an
// error.
filterDoc = {input: '$a', as: 'x', cond: true, limit: 0.5};
runAndAssertThrows(filterDoc, 327391);

filterDoc = {input: '$a', as: 'x', cond: true, limit: 0};
runAndAssertThrows(filterDoc, 327392);

filterDoc = {input: '$a', as: 'x', cond: true, limit: -100};
runAndAssertThrows(filterDoc, 327392);

// Passing 'maxInt32 + 1' value for the 'limit' argument should throw an exception
filterDoc = {
    input: '$a', as: 'x', cond: true, limit: 2147483648
};
runAndAssertThrows(filterDoc, 327391);

// Create filter that uses the default variable name in 'cond'.
filterDoc = {
    $filter: {input: '$a', cond: {$eq: [2, '$$this']}}
};
expectedResults = [
    {_id: 0, b: [2]},
    {_id: 1, b: [2]},
    {_id: 2, b: []},
    {_id: 3, b: []},
    {_id: 4, b: null},
    {_id: 5, b: null},
    {_id: 6, b: null},
];
runAndAssert(filterDoc, expectedResults);

// Create filter with path expressions inside $let expression.
filterDoc =
    {
                $let: {
                    vars: {
                        value: '$d'
                    },
                    in: {
                        $filter: {
                            input: '$a',
                            cond: {$gte: [{$add: ['$c', '$$this']}, '$$value']}
                        }
                    }
                }
            };

expectedResults = [
    {_id: 0, b: [2, 3, 4, 5]},
    {_id: 1, b: [2]},
    {_id: 2, b: []},
    {_id: 3, b: [4]},
    {_id: 4, b: null},
    {_id: 5, b: null},
    {_id: 6, b: null},
];
runAndAssert(filterDoc, expectedResults);

// Create filter that uses the $and and $or.
filterDoc = {
    $filter: {
        input: '$a',
        cond: {$or: [{$and: [{$gt: ['$$this', 1]}, {$lt: ['$$this', 3]}]}, {$eq: ['$$this', 5]}]}
    }
};
expectedResults = [
    {_id: 0, b: [2, 5]},
    {_id: 1, b: [2]},
    {_id: 2, b: []},
    {_id: 3, b: []},
    {_id: 4, b: null},
    {_id: 5, b: null},
    {_id: 6, b: null},
];
runAndAssert(filterDoc, expectedResults);

// Nested $filter expression. Queries below do not make sense from the user perspective, but allow
// us to test complex SBE trees generated by expressions like $and, $or, $cond and $switch with
// $filter inside them.

// Create filter as an argument to $and and $or expressions.
expectedResults = [
    {_id: 0, b: true},
    {_id: 1, b: true},
    {_id: 2, b: true},
    {_id: 3, b: true},
    {_id: 4, b: false},
    {_id: 5, b: false},
    {_id: 6, b: false},
];
filterDoc = {
    $or: [
        {
            $and: [
                {
                    $filter: {
                        input: '$a',
                        cond: {
                            $or: [
                                {$and: [{$gt: ['$$this', 1]}, {$lt: ['$$this', 3]}]},
                                {$eq: ['$$this', 5]}
                            ]
                        }
                    }
                },
                '$d'
            ]
        },
        {$filter: {input: '$a', cond: {$eq: ['$$this', 1]}}}
    ]
};
runAndAssert(filterDoc, expectedResults);

// Create filter as an argument to $cond expression.
expectedResults = [
    {_id: 0, b: [2]},
    {_id: 1, b: [2]},
    {_id: 2, b: []},
    {_id: 3, b: []},
    {_id: 4, b: null},
    {_id: 5, b: null},
    {_id: 6, b: null},
];
filterDoc = {
    $cond: {
        if: {$filter: {input: '$a', cond: {$eq: ['$$this', 1]}}},
        then: {$filter: {input: '$a', cond: {$eq: ['$$this', 2]}}},
        else: {$filter: {input: '$a', cond: {$eq: ['$$this', 3]}}}
    }
};

runAndAssert(filterDoc, expectedResults);

// Create filter as an argument to $switch expression.
expectedResults = [
    {_id: 0, b: [2]},
    {_id: 1, b: [2]},
    {_id: 2, b: []},
    {_id: 3, b: []},
    {_id: 4, b: null},
    {_id: 5, b: null},
    {_id: 6, b: null},
];
filterDoc = {
    $switch: {
        branches: [
            {
                case: {$filter: {input: '$a', cond: {$eq: ['$$this', 1]}}},
                then: {$filter: {input: '$a', cond: {$eq: ['$$this', 2]}}}
            },
            {
                case: {$filter: {input: '$a', cond: {$eq: ['$$this', 3]}}},
                then: {$filter: {input: '$a', cond: {$eq: ['$$this', 4]}}}
            }
        ],
        default: {$filter: {input: '$a', cond: {$eq: ['$$this', 5]}}}
    }
};
runAndAssert(filterDoc, expectedResults);

// Invalid filter expressions.

// '$filter' is not a document.
filterDoc = 'string';
runAndAssertThrows(filterDoc, 28646);

// Extra field(s).
filterDoc = {input: '$a', as: 'x', cond: true, extra: 1};
runAndAssertThrows(filterDoc, 28647);

// Missing 'input'.
filterDoc = {
    as: 'x',
    cond: true
};
runAndAssertThrows(filterDoc, 28648);

// Missing 'cond'.
filterDoc = {input: '$a', as: 'x'};
runAndAssertThrows(filterDoc, 28650);

// 'as' is not a valid variable name.
filterDoc = {input: '$a', as: '$x', cond: true};
runAndAssertThrows(filterDoc, ErrorCodes.FailedToParse);

// 'input' is not an array.
filterDoc = {input: 'string', as: 'x', cond: true};
runAndAssertThrows(filterDoc, 28651);

// 'cond' uses undefined variable name.
filterDoc = {
    input: '$a',
    cond: {$eq: [1, '$$var']}
};
runAndAssertThrows(filterDoc, 17276);

assert(coll.drop());
assert.commandWorked(coll.insert({a: 'string'}));
filterDoc = {input: '$a', as: 'x', cond: true};
runAndAssertThrows(filterDoc, 28651);

// Test $filter with a limit arg computed dynamically with an expression.
assert(coll.drop());
assert.commandWorked(coll.insert({
    _id: 0,
    store: "A",
    maxSaleItems: 4,
    items: [{price: 50}, {price: 500}, {price: 150}, {price: 200}, {price: 100}, {price: 200}]
}));
assert.commandWorked(coll.insert({
    _id: 1,
    store: "B",
    maxSaleItems: 2,
    items: [{price: 50}, {price: 500}, {price: 150}, {price: 200}, {price: 100}, {price: 200}]
}));
assert.commandWorked(coll.insert({_id: 2, store: "C", maxSaleItems: 4, items: []}));
assert.commandWorked(coll.insert({
    _id: 3,
    store: "D",
    maxSaleItems: 0,
    items: [{price: 50}, {price: 500}, {price: 150}, {price: 200}, {price: 100}, {price: 200}]
}));
assert.commandWorked(coll.insert({_id: 4, store: "E", maxSaleItems: 10, items: null}));
assert.commandWorked(coll.insert({_id: 5, store: "F", maxSaleItems: 10, items: undefined}));
assert.commandWorked(coll.insert({_id: 6, c: 7, d: 9}));

expectedResults = [
    {_id: 0, b: [{price: 500}, {price: 150}, {price: 200}, {price: 100}, {price: 200}]},
    {_id: 1, b: [{price: 500}, {price: 150}, {price: 200}]},
    {_id: 2, b: []},
    {_id: 3, b: [{price: 500}]},
    {_id: 4, b: null},
    {_id: 5, b: null},
    {_id: 6, b: null},
];

filterDoc = {$filter: {
    input: "$items",
    limit: {$add: ["$maxSaleItems", 1]},
    as: "item",
    cond: { $gte: [ "$$item.price", 100 ] }
 }};

runAndAssert(filterDoc, expectedResults);

// Create filter with non-bool predicate.
assert(coll.drop());
const date = new Date();
assert.commandWorked(
    coll.insert({_id: 0, a: [date, null, undefined, 0, false, NumberDecimal('1'), [], {c: 3}]}));
expectedResults = [
    {_id: 0, b: [date, NumberDecimal('1'), [], {c: 3}]},
];
filterDoc = {
    $filter: {input: '$a', as: 'x', cond: '$$x'}
};
runAndAssert(filterDoc, expectedResults);

// Create filter with deep path expressions.
assert(coll.drop());
assert.commandWorked(coll.insert({
    _id: 0,
    a: [
        {b: {c: {d: 1}}},
        {b: {c: {d: 2}}},
        {b: {c: {d: 3}}},
        {b: {c: {d: 4}}},
    ]
}));

filterDoc = {
    $filter: {input: '$a', cond: {$gt: ['$$this.b.c.d', 2]}}
};
expectedResults = [
    {_id: 0, b: [{b: {c: {d: 3}}}, {b: {c: {d: 4}}}]},
];
runAndAssert(filterDoc, expectedResults);

// Create nested filter expressions.
assert(coll.drop());
assert.commandWorked(coll.insert({_id: 0, a: [[1, 2, 3], null, [4, 5, 6]]}));

expectedResults = [
    {_id: 0, b: [[1, 2, 3], [4, 5, 6]]},
];
filterDoc = {
    $filter: {input: '$a', cond: {$filter: {input: '$$this', cond: {$gt: ['$$this', 3]}}}}
};
runAndAssert(filterDoc, expectedResults);

assert(coll.drop());
assert.commandWorked(
    coll.insert({_id: 0, a: [[-1000, 10, 20, 30, 40, 8000], [40, 50, 60], [70, 80, 90]]}));

expectedResults = [
    {_id: 0, b: [[-1000, 10, 20, 30, 40, 8000], [40, 50, 60]]},
];
filterDoc = {
    $filter: {
        input: '$a',
        limit: 2,
        cond: {$filter: {input: '$$this', limit: 3, cond: {$gt: [{$add: [0, '$$this']}, 3]}}}
    }
};
runAndAssert(filterDoc, expectedResults);

filterDoc = {
    $filter: {
        input: {$filter: {input: '$a', as: 'x', limit: 2, cond: {$gt: ['$$x', 3]}}},
        limit: 3,
        cond: {$gt: ['$$this', 20]}
    }
};
runAndAssert(filterDoc, expectedResults);

// Test short-circuiting in $and and $or inside $filter expression.
coll.drop();
assert.commandWorked(coll.insert({_id: 0, a: [-1, -2, -3, -4], zero: 0}));
// Create filter with $and expression containing $divide by zero operation in it.
expectedResults = [
    {_id: 0, b: []},
];
filterDoc = {
    $filter: {input: '$a', cond: {$and: [{$gt: ['$$this', 0]}, {$divide: [1, '$zero']}]}}
};
runAndAssert(filterDoc, expectedResults);

// Create filter with $or expression containing $divide by zero operation in it.
expectedResults = [
    {_id: 0, b: [-1, -2, -3, -4]},
];
filterDoc = {
    $filter: {input: '$a', cond: {$or: [{$lte: ['$$this', 0]}, {$divide: [1, '$zero']}]}}
};
runAndAssert(filterDoc, expectedResults);

// Create filter with $and expression containing invalid call to $ln in it.
expectedResults = [
    {_id: 0, b: []},
];
filterDoc = {
    $filter: {input: '$a', cond: {$and: [{$gt: ['$$this', 0]}, {$ln: '$$this'}]}}
};
runAndAssert(filterDoc, expectedResults);

// Create filter with $or expression containing invalid call to $ln in it.
expectedResults = [
    {_id: 0, b: [-1, -2, -3, -4]},
];
filterDoc = {
    $filter: {input: '$a', cond: {$or: [{$lt: ['$$this', 0]}, {$ln: '$$this'}]}}
};
runAndAssert(filterDoc, expectedResults);
