/**
 * Basic tests for the $setUnion accumulator.
 * @tags: [featureFlagArrayAccumulators, requires_fcv_81]
 */
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

const coll = db["accumulators_set_union"];
assert(coll.drop());

// Test that $setUnion produces a single array containing all the unique values from the input
// arrays. As $setUnion does not provide guarantees on ordering, the result is sorted for easy
// comparison.
assert.commandWorked(coll.insert([{_id: 0, nums: [1, 2, 3]}, {_id: 1, nums: [4, 5, 6]}]));

let result = coll.aggregate([
                     {$group: {_id: null, n: {$setUnion: '$nums'}}},
                     {$project: {_id: 1, setUnionArr: {$sortArray: {input: '$n', sortBy: 1}}}}
                 ])
                 .toArray();

assert.eq(result, [{_id: null, setUnionArr: [1, 2, 3, 4, 5, 6]}]);

assert(coll.drop());

// Test that $setUnion deduplicates the values. That is, each unique value will only appear once in
// the ouput of the accumulation.
assert.commandWorked(coll.insert(
    [{_id: 0, nums: [1, 2, 3]}, {_id: 1, nums: [2, 3, 4]}, {_id: 2, nums: [3, 4, 5, 6]}]));

result = coll.aggregate([
                 {$group: {_id: null, n: {$setUnion: '$nums'}}},
                 {$project: {_id: 1, setUnionArr: {$sortArray: {input: '$n', sortBy: 1}}}}
             ])
             .toArray();

assert.eq(result, [{_id: null, setUnionArr: [1, 2, 3, 4, 5, 6]}]);

assert(coll.drop());

// Test that $setUnion deduplicates values that come from the same document. That is, each unique
// value will only appear once in the ouput of the accumulation.
assert.commandWorked(coll.insert([{_id: 0, nums: [1, 1, 2, 2, 3, 3]}]));

result = coll.aggregate([
                 {$group: {_id: null, n: {$setUnion: '$nums'}}},
                 {$project: {_id: 1, setUnionArr: {$sortArray: {input: '$n', sortBy: 1}}}}
             ])
             .toArray();

assert.eq(result, [{_id: null, setUnionArr: [1, 2, 3]}]);

assert(coll.drop());

// Nested arrays should still be arrays in the result of $setUnion.
assert.commandWorked(coll.insert([
    {_id: 0, vals: [["nested"], 1, 2]},
    {_id: 1, vals: [3, 4, ["nested"]]},
    {_id: 2, vals: [4, ["nested", "extra"]]}
]));

result = coll.aggregate([
                 {$group: {_id: null, n: {$setUnion: '$vals'}}},
                 {$project: {_id: 1, setUnionArr: {$sortArray: {input: '$n', sortBy: 1}}}}
             ])
             .toArray();

assert.eq(result, [{_id: null, setUnionArr: [1, 2, 3, 4, ["nested"], ["nested", "extra"]]}]);

assert(coll.drop());

// $setUnion should deduplicate objects as well. Note that documents which differ in field order are
// considered unique.
assert.commandWorked(coll.insert([
    {_id: 0, vals: [{a: 1, b: 2}]},
    {_id: 1, vals: [{b: 2, a: 1}]},
    {_id: 2, vals: [{a: 1, b: 2}]},
]));

result = coll.aggregate([
                 {$group: {_id: null, n: {$setUnion: '$vals'}}},
                 {$project: {_id: 1, setUnionArr: {$sortArray: {input: '$n', sortBy: 1}}}}
             ])
             .toArray();

assert.eq(result, [{_id: null, setUnionArr: [{a: 1, b: 2}, {b: 2, a: 1}]}]);

assert(coll.drop());

// $setUnion should 'skip over' documents that do not have the array field. Importantly, do not
// insert null for documents that do not have the referenced field.
assert.commandWorked(coll.insert([
    {_id: 1, author: "Nick", publisher: "Pub3", books: ["Smile :)"]},
    {_id: 2, author: "Santiago", publisher: "Pub3"},
    {_id: 3, author: "Matt", publisher: "Pub3", books: ["Happy!"]}
]));
result = coll.aggregate([
                 {$sort: {_id: 1}},
                 {$group: {_id: null, allBooks: {$setUnion: '$books'}}},
                 {$project: {_id: 1, setUnionArr: {$sortArray: {input: '$allBooks', sortBy: 1}}}}
             ])
             .toArray();

assert.eq(result, [{_id: null, setUnionArr: ["Happy!", "Smile :)"]}]);

assert(coll.drop());

// Test for errors on non-array types ($setUnion only supports arrays).
const notArrays = [1, "string", {object: "object"}, null];

for (const notAnArray of notArrays) {
    assert.commandWorked(coll.insert([{_id: "doesNotMatter", vals: notAnArray}]));

    assertErrorCode(
        coll, [{$group: {_id: null, v: {$setUnion: '$vals'}}}], ErrorCodes.TypeMismatch);

    assert.commandWorked(coll.deleteOne({_id: "doesNotMatter"}));
}

assert(coll.drop());

// Basic test of $setUnion with grouping.
assert.commandWorked(coll.insert([
    {_id: 1, author: "Kyra", publisher: "Pub1", books: ["Book 1"]},
    {_id: 2, author: "Nick", publisher: "Pub3", books: ["Book 2"]},
    {_id: 3, author: "Santiago", publisher: "Pub3"},
    {_id: 4, author: "Matt", publisher: "Pub3", books: ["Book 3"]}
]));

result =
    coll.aggregate([
            {$group: {_id: '$publisher', booksByPublisher: {$setUnion: '$books'}}},
            {$sort: {_id: 1}},
            {$project: {_id: 1, setUnionArr: {$sortArray: {input: '$booksByPublisher', sortBy: 1}}}}
        ])
        .toArray();

assert.eq(
    result,
    [{_id: "Pub1", setUnionArr: ["Book 1"]}, {_id: "Pub3", setUnionArr: ["Book 2", "Book 3"]}]);
