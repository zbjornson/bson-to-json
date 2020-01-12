//@ts-check
"use strict";

/**
 * Exercises decoding an array of a single type. Source BSON fits in less than
 * half of L1 cache. Invoke with:
 *
 *     node ./benchmark/types.js space separated list of types
 * 
 * Where the list of types can be any of ObjectId, Date, Int, Number, Long,
 * Boolean, Null and/or String (case-insensitive).
 *
 * With String, also specify `--len=<int> --escape=<0 to 1>` to specify the
 * string length and fraction of characters that must be escaped.
 */

const benchmark = require("benchmark");
const benchmarks = require("beautify-benchmark");

const bson = require("bson");
const JS = require("../src/bson-to-json.js");
const CPP = require("../build/release/bsonToJson.node");

const types = process.argv.map(s => s.toLowerCase());

function addAndRun(name, buf) {
	const suite = new benchmark.Suite(name, {
		onCycle: e => benchmarks.add(e.target),
		onComplete: () => benchmarks.log()
	});

	console.log(name);
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
	const docs = Array.from({length: 2000}, (v, i) => i * 1000000);
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
	const docs = Array.from({length: 1000}, () => new Long(0x1fffff, 0xffffffff));
	const buf = bson.serialize(docs);

	// Large values in this test aren't fair; bsonToJson losslessly writes full
	// int64s, whereas js-bson writes `{low: number, high: number, unsigned:
	// boolean}`.
	// const docs = Array.from({length: 1000}, () => new Long(0xFFFFFFF, 0xFFFFFFF));

	addAndRun("Long", buf);
}

if (types.includes("string")) {
	const lenArg = process.argv.find(a => a.startsWith("--len="));
	const escArg = process.argv.find(a => a.startsWith("--escape="));
	if (!lenArg || !escArg) {
		console.log("Specify --len=<int> --escape=<0 to 1> when testing strings");
		process.exit(1);
	}
	const len = Number.parseInt(lenArg.split("=")[1], 10);
	const esc = Number.parseFloat(escArg.split("=")[1]);
	const str = Array.from({length: len}, () => {
		return Math.random() < esc ? "\t" : "a";
	}).join("");
	// Strings are 1B type header + 4B length + data
	const aSize = Math.ceil(16000 / (5 + len));
	const docs = Array.from({length: aSize}, () => str);
	const buf = bson.serialize(docs);
	addAndRun(`String<len=${len} escape=${esc}>`, buf);
}
