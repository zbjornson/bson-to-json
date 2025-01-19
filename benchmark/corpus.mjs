//@ts-check

import benchmark from "benchmark";
import benchmarks from "beautify-benchmark";
import {createRequire} from "node:module";
import * as bson from "bson";
const JS = await import ("../src/bson-to-json.mjs");
const require = createRequire(import.meta.url);
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
