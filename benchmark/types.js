//@ts-check
"use strict";

/**
 * Exercises decoding an array of a single type. Source BSON fits in less than
 * half of L1 cache. Invoke with:
 *
 *     node ./benchmark/types.js space separated list of types
 * 
 * Where the list of types can be any of ObjectId, Date, Int, Number, Long,
 * Boolean or Null (case-insensitive). TODO String.
 */

const benchmark = require("benchmark");
const benchmarks = require("beautify-benchmark");

const bson = require("bson");
const JS = require("../index.js");
const CPP = require("../build/release/bson-to-json.node");

const types = process.argv.map(s => s.toLowerCase());

function addAndRun(name, buf) {
	const suite = new benchmark.Suite(name, {
		onCycle: e => benchmarks.add(e.target),
		onComplete: () => benchmarks.log()
	});

	// NB: this returns an object with numeric keys
	suite.add("js-bson", () => Buffer.from(JSON.stringify(bson.deserialize(buf))));
	suite.add("bsonToJson JS", () => JS.bsonToJson(buf, true));
	suite.add("bsonToJson C++", () => CPP.bsonToJson(buf, true));
	suite.run();
}

if (types.includes("objectid")) {
	// ObjectIds are 12B + 1B type header = 13B each = ~2461 values in L1$
	const docs = Array.from({length: 1000}, () => new bson.ObjectId());
	const buf = bson.serialize(docs);
	addAndRun("ObjectId", buf);
}

if (types.includes("date")) {
	// Dates are 8B + 1B type header = 9B each = ~3555 values in L1$
	const docs = Array.from({length: 1000}, () => new Date());
	const buf = bson.serialize(docs);
	addAndRun("Date", buf);
}

if (types.includes("int")) {
	// Ints are 4B + 1B type header = 5B each = ~6400 values in L1$
	const docs = Array.from({length: 2000}, (v, i) => i);
	const buf = bson.serialize(docs);
	addAndRun("Int", buf);
}

if (types.includes("number")) {
	// Numbers are 4B + 1B type header = 5B each = ~6400 values in L1$
	const docs = Array.from({length: 2000}, Math.random);
	const buf = bson.serialize(docs);
	addAndRun("Number", buf);
}

if (types.includes("boolean")) {
	// Booleans are 1B + 1B type header = 2B each = ~16000 values in L1$
	const docs = Array.from({length: 8000}, () => Math.random() > 0.5);
	const buf = bson.serialize(docs);
	addAndRun("Boolean", buf);
}

if (types.includes("null")) {
	// Nulls are 1B type header = 1B each = ~32000 values in L1$
	const docs = Array.from({length: 8000}, () => null);
	const buf = bson.serialize(docs);
	addAndRun("Null", buf);
}

if (types.includes("long")) {
	const Long = bson.Long;

	// Longs are 8B + 1B type header = 9B each = ~3555 values in L1$
	const docs = Array.from({length: 1000}, () => new Long(0xFFFFFFF, 0xFFFFFFF));
	const buf = bson.serialize(docs);
	// This test isn't fair; bsonToJson losslessly writes full int64s, whereas
	// js-bson writes `{low: number, high: number, unsigned: boolean}`.
	addAndRun("Long", buf);
}
