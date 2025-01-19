//@ts-check

import benchmark from "benchmark";
import benchmarks from "beautify-benchmark";
import {createRequire} from "node:module";
import * as bson from "bson";
const JS = await import ("../src/bson-to-json.mjs");
const require = createRequire(import.meta.url);
const CPP = require("../build/Release/bsonToJson.node");

const ref1 = {
	_id: new bson.ObjectId(),
	prop1: "hello"
};

const doc1 = {
	k1: "hey",
	localKey: ref1._id,
	em1: {
		k2: "yo",
		em2: {
			k3: 123
		},
		arr1: [
			"hey",
			{k4: ref1._id}
		]
	},
	t1: 2
};

const populateInfo = new CPP.PopulateInfo();
populateInfo.addItems("localKey", [bson.serialize(ref1)]);
populateInfo.addItems("em1.arr1.k4", [bson.serialize(ref1)]);

const buf = bson.serialize(doc1);

const suite = new benchmark.Suite("Populate", {
	onCycle: e => benchmarks.add(e.target),
	onComplete: () => benchmarks.log()
});

const t = new CPP.Transcoder(populateInfo);
suite.add("bsonToJson C++", () => t.transcode(buf));
suite.run();
