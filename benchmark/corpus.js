//@ts-check
"use strict";

const benchmark = require("benchmark");
const benchmarks = require("beautify-benchmark");

const bson = require("bson");
const JS = require("../src/bson-to-json.js");
const CPP = require("../build/release/bsonToJson.node");

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

const buf = require("fs").readFileSync("./data.bson");
addAndRun("data.bson", buf);
