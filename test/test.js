//@ts-check
"use strict";

const assert = require("assert");
const bson = require("bson");

// Exercises all JSON types and nuances of JSON serialization.
const doc1 = {
	string: "string\tdata\n",
	objectId: new bson.ObjectId(),
	int: 1,
	int2: -2345718,
	number: Math.PI,
	date: new Date(1575271655028), // has a 0 in the millis (028)
	boolean: false,
	object: {
		embedded: "document"
	},
	array: [-2, "hey", new bson.ObjectId()],
	null: null,
	long: new bson.Long(0xFFFFFFF, 0xFFFFFFF),
	undefined: undefined, // omitted from result
	nan: NaN, // serializes to null
	infinity: Infinity, // serializes to null
	negInfinity: -Infinity, // serializes to null
	arrayOfUndef: [1, undefined] // undefined is preserved
};

// For using C++ debugger
global.describe = global.describe || function describe(label, fn) { fn(); };
global.it = global.it || function it(label, fn) { fn(); };

// TODO {a: NaN, b: Infinity, c: -Infinity} -> {a: null, b: null, c: null}
// TODO {arr: [1, undefined]} -> '{"arr": [1, null]}'

describe("bson2json - JS", function () {
	const {bsonToJson} = require("../index.js");

	it("deserializes all JSON types", function () {
		const bsonBuffer = bson.serialize(doc1);
		const jsonBuffer = bsonToJson(bsonBuffer, false);

		const expected = JSON.parse(JSON.stringify(doc1));
		// The JSON string will contain 1152921500580315135, which parses to
		// 1152921500580315100.
		expected.long = doc1.long.toNumber();

		// Escaping:
		assert.ok(jsonBuffer.toString().startsWith('{"string":"string\\tdata\\n'));

		assert.deepEqual(
			JSON.parse(jsonBuffer.toString()),
			expected
		);
	});
});

describe("bson2json - C++", function () {
	const {bsonToJson} = require("../build/release/bson-to-json.node");

	it("deserializes all JSON types", function () {
		const bsonBuffer = bson.serialize(doc1);
		const jsonBuffer = bsonToJson(bsonBuffer, false);

		const expected = JSON.parse(JSON.stringify(doc1));
		// The JSON string will contain 1152921500580315135, which parses to
		// 1152921500580315100.
		expected.long = doc1.long.toNumber();

		// Escaping:
		assert.ok(jsonBuffer.toString().startsWith('{"string":"string\\tdata\\n'));

		assert.deepEqual(
			JSON.parse(jsonBuffer.toString()),
			expected
		);
	});
});
