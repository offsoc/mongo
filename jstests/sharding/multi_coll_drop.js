// Tests the dropping and re-adding of a collection
// @tags: [
//   # TODO (SERVER-88123): Re-enable this test.
//   # Test doesn't start enough mongods to have num_mongos routers
//   embedded_router_incompatible,
// ]
import {ShardingTest} from "jstests/libs/shardingtest.js";

var st = new ShardingTest({name: "multidrop", shards: 1, mongos: 2});

var mA = st.s0;
var mB = st.s1;

var coll = mA.getCollection('multidrop.coll');
var collB = mB.getCollection('multidrop.coll');

jsTestLog("Shard and split collection...");

var admin = mA.getDB("admin");
assert.commandWorked(admin.runCommand({enableSharding: coll.getDB() + ""}));
assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}));

for (var i = -100; i < 100; i++) {
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: i}}));
}

jsTestLog("Create versioned connection for each mongos...");

assert.eq(0, coll.find().itcount());
assert.eq(0, collB.find().itcount());

jsTestLog("Dropping sharded collection...");
assert(coll.drop());

jsTestLog("Recreating collection...");

assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}));
for (let i = -10; i < 10; i++) {
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: i}}));
}

jsTestLog("Retrying connections...");

assert.eq(0, coll.find().itcount());
assert.eq(0, collB.find().itcount());

st.stop();
