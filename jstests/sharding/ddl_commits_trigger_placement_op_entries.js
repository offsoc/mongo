/**
 * Verifies that successful commits of Sharding DDL operations generate the expected op entry types
 * (following the format and rules defined in the design doc of PM-1939).
 * TODO SERVER-81138 remove multiversion_incompatible and fix comparison with 7.0 binaries
 * @tags: [
 *   does_not_support_stepdowns,
 *   requires_fcv_70,
 * ]
 */
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 3, chunkSize: 1});
const configDB = st.s.getDB('config');

function getExpectedOpEntriesOnNewDb(dbName, primaryShard, isImported = false) {
    // The creation of a database is matched by the generation of two op entries:
    return [
        // - One emitted before the metadata is committed on the sharding catalog
        {
            op: 'n',
            ns: dbName,
            o: {msg: {createDatabasePrepare: dbName}},
            o2: {createDatabasePrepare: dbName, primaryShard: primaryShard, isImported: isImported}
        },
        // - The second one emitted once the metadata is committed on the sharding catalog
        {
            op: 'n',
            ns: dbName,
            o: {msg: {createDatabase: dbName}},
            o2: {createDatabase: dbName, isImported: isImported}
        },
    ];
}

function verifyOpEntriesOnNodes(expectedOpEntryTemplates, nodes) {
    const namespaces = [...new Set(expectedOpEntryTemplates.map(t => t.ns))];
    for (const node of nodes) {
        const foundOpEntries = node.getCollection('local.oplog.rs')
                                   .find({ns: {$in: namespaces}, op: {$in: ['c', 'n']}})
                                   .sort({ts: -1})
                                   .limit(expectedOpEntryTemplates.length)
                                   .toArray()
                                   .reverse();

        assert.eq(expectedOpEntryTemplates.length, foundOpEntries.length);
        for (let i = 0; i < foundOpEntries.length; ++i) {
            // SERVER-83104: Remove 'numInitialChunks' check once 8.0 becomes last LTS.
            if ('numInitialChunks' in foundOpEntries[i].o2) {
                delete foundOpEntries[i].o2.numInitialChunks;
            }

            // SERVER-83104: Remove 'capped' check once 8.0 becomes last LTS.
            if (!('capped' in foundOpEntries[i].o2)) {
                delete expectedOpEntryTemplates[i].o2.capped;
            }

            assert.eq(expectedOpEntryTemplates[i].op, foundOpEntries[i].op);
            assert.eq(expectedOpEntryTemplates[i].ns, foundOpEntries[i].ns);
            assert.docEq(expectedOpEntryTemplates[i].o, foundOpEntries[i].o);
            assert.docEq(expectedOpEntryTemplates[i].o2, foundOpEntries[i].o2);
        }
    }
}

function testCreateDatabase(dbName = 'createDatabaseTestDB', primaryShardId = st.shard0.shardName) {
    jsTest.log('test createDatabase');

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: primaryShardId}));

    // Each shard of the cluster should have received the notifications about the database creation
    // (and generated the related op entries).
    const shardPrimaryNodes = Object.values(DiscoverTopology.findConnectedNodes(st.s).shards)
                                  .map(s => new Mongo(s.primary));
    const expectedOpEntries = getExpectedOpEntriesOnNewDb(dbName, primaryShardId);
    verifyOpEntriesOnNodes(expectedOpEntries, shardPrimaryNodes);
}

function testShardCollection() {
    jsTest.log('Testing placement entries added by shardCollection() (with implicit DB creation)');

    const dbName = 'shardCollectionTestDB';
    const collName = 'coll';
    const nss = dbName + '.' + collName;

    // Run shardCollection, with each shard hosting one chunk.
    const topology = DiscoverTopology.findConnectedNodes(st.s);
    assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {_id: 'hashed'}}));

    // Verify that the op entries for the creation of the parent DB have been generated on each
    // shard of the cluster.
    const primaryShard = configDB.databases.findOne({_id: dbName}).primary;
    const expectedEntriesForDbCreation = getExpectedOpEntriesOnNewDb(dbName, primaryShard);
    const shardPrimaryNodes = Object.values(topology.shards).map(s => new Mongo(s.primary));
    verifyOpEntriesOnNodes(expectedEntriesForDbCreation, shardPrimaryNodes);

    // Verify that the op entries for the sharded collection have been generated by the primary
    // shard.
    let allShardNames = Object.keys(topology.shards);
    // Shard names are sorted by the server.
    allShardNames.sort();
    const primaryShardPrimaryNode = new Mongo(topology.shards[primaryShard].primary);

    const expectedEntriesForCollSharded = [
        // One entry emitted before the metadata is committed on the sharding catalog
        {
            op: 'n',
            ns: nss,
            o: {msg: {shardCollectionPrepare: nss}},
            o2: {
                shardCollectionPrepare: nss,
                shards: allShardNames,
                shardKey: {_id: 'hashed'},
                unique: false,
                presplitHashedZones: false,
                capped: false
            }
        },
        // One entry emitted once the metadata is committed on the sharding catalog
        {
            op: 'n',
            ns: nss,
            o: {msg: {shardCollection: nss}},
            o2: {
                shardCollection: nss,
                shardKey: {_id: 'hashed'},
                unique: false,
                presplitHashedZones: false,
                capped: false
            }
        }
    ];

    verifyOpEntriesOnNodes(expectedEntriesForCollSharded, [primaryShardPrimaryNode]);

    jsTest.log('Testing placement entries added by shardCollection() of a timeseries collection');

    const timeseriesCollName = 'timeseriesColl';
    const timeseriesNss = dbName + '.' + timeseriesCollName;
    const bucketsNss = dbName + '.system.buckets.' + timeseriesCollName;

    // Create and shard a timeseries collection. The timeField is also used as shard key to verify
    // that its value gets correctly encoded within the 'o2' field of the oplog.
    const timeField = 'timestamp';
    const encodedTimeField = 'control.min.' + timeField;
    const metaField = 'metadata';
    const granularity = 'hours';
    const maxSpan = 2592000;
    assert.commandWorked(st.s.adminCommand({
        shardCollection: timeseriesNss,
        key: {[timeField]: 1},
        timeseries: {
            timeField: timeField,
            metaField: metaField,
            granularity: granularity,
            bucketMaxSpanSeconds: maxSpan
        }
    }));

    const expectedEntriesForTimeseriesCollSharded = [
        // One entry emitted before the DDL is committed on the sharding catalog
        {
            op: 'n',
            ns: bucketsNss,
            o: {msg: {shardCollectionPrepare: bucketsNss}},
            o2: {
                shardCollectionPrepare: bucketsNss,
                shards: [primaryShard],
                shardKey: {[encodedTimeField]: 1},
                unique: false,
                presplitHashedZones: false,
                timeseries: {
                    timeField: timeField,
                    metaField: metaField,
                    granularity: granularity,
                    bucketMaxSpanSeconds: maxSpan
                },
                capped: false

            }
        },
        // One entry emitted once the DDL is committed on the sharding catalog
        {
            op: 'n',
            ns: bucketsNss,
            o: {msg: {shardCollection: bucketsNss}},
            o2: {
                shardCollection: bucketsNss,
                shardKey: {[encodedTimeField]: 1},
                unique: false,
                presplitHashedZones: false,
                timeseries: {
                    timeField: timeField,
                    metaField: metaField,
                    granularity: granularity,
                    bucketMaxSpanSeconds: maxSpan
                },
                capped: false
            }
        }
    ];

    verifyOpEntriesOnNodes(expectedEntriesForTimeseriesCollSharded, [primaryShardPrimaryNode]);
}

function testAddShard() {
    jsTest.log('Test addShard');
    const shardPrimaryNodes = Object.values(DiscoverTopology.findConnectedNodes(st.s).shards)
                                  .map(s => new Mongo(s.primary));

    // Create a new replica set and populate it with two DBs
    const newReplicaSet = new ReplSetTest({name: 'addedShard', nodes: 1});
    const newShardName = 'addedShard';
    const preExistingCollName = 'preExistingColl';
    newReplicaSet.startSet({shardsvr: ''});
    newReplicaSet.initiate();
    const dbsOnNewReplicaSet = ['addShardTestDB1', 'addShardTestDB2'];
    for (const dbName of dbsOnNewReplicaSet) {
        const db = newReplicaSet.getPrimary().getDB(dbName);
        assert.commandWorked(db[preExistingCollName].save({value: 1}));
    }

    // Add the new replica set as a shard
    assert.commandWorked(st.s.adminCommand({addShard: newReplicaSet.getURL(), name: newShardName}));

    // Each already existing shard should contain the op entries for each database hosted by
    // newReplicaSet (that have been added to the catalog as part of addShard).
    for (let dbName of dbsOnNewReplicaSet) {
        const expectedOpEntries =
            getExpectedOpEntriesOnNewDb(dbName, newShardName, true /*isImported*/);
        verifyOpEntriesOnNodes(expectedOpEntries, shardPrimaryNodes);
    }

    // Execute the test case teardown
    for (const dbName of dbsOnNewReplicaSet) {
        assert.commandWorked(st.getDB(dbName).dropDatabase());
    }
    let res = assert.commandWorked(st.s.adminCommand({removeShard: newShardName}));
    assert.eq('started', res.state);
    res = assert.commandWorked(st.s.adminCommand({removeShard: newShardName}));
    assert.eq('completed', res.state);
    newReplicaSet.stopSet();
}

function testMovePrimary() {
    jsTest.log(
        'Testing placement entries added by movePrimary() over a new sharding-enabled DB with no data');

    // Set the initial state
    const dbName = 'movePrimaryTestDB';
    const fromPrimaryShard = st.shard0;
    const fromReplicaSet = st.rs0;
    testCreateDatabase(dbName, fromPrimaryShard.shardName);

    // Move the primary shard.
    const toPrimaryShard = st.shard1;
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: toPrimaryShard.shardName}));

    // Verify that the old shard generated the expected event.
    const expectedEntriesForPrimaryMoved = [{
        op: 'n',
        ns: dbName,
        o: {msg: {movePrimary: dbName}},
        o2: {movePrimary: dbName, from: fromPrimaryShard.shardName, to: toPrimaryShard.shardName},
    }];

    verifyOpEntriesOnNodes(expectedEntriesForPrimaryMoved, [fromReplicaSet.getPrimary()]);
}

testCreateDatabase();

testShardCollection();

testAddShard();

testMovePrimary();

st.stop();
