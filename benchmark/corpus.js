//@ts-check
"use strict";

const benchmark = require("benchmark");
const benchmarks = require("beautify-benchmark");

const bson = require("bson");
const JS = require("../src/bson-to-json.js");
const CPP = require("../build/Release/bsonToJson.node");

function addAndRun(name, buf) {
	const suite = new benchmark.Suite(name, {
		onCycle: e => benchmarks.add(e.target),
		onComplete: () => benchmarks.log()
	});

	// NB: this returns an object with numeric keys
	suite.add("js-bson", () => Buffer.from(JSON.stringify(bson.deserialize(buf))));
	const js = new JS.Transcoder();
	suite.add("bsonToJson JS", () => js.transcode(buf));
	const cpp = new CPP.Transcoder();
	suite.add("bsonToJson C++", () => cpp.transcode(buf));
	suite.run();
}

const buf = require("fs").readFileSync(__dirname + "/data.bson");
addAndRun("data.bson", buf);
