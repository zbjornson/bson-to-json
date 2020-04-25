//@ts-check
"use strict";

const assert = require("assert");
const bson = require("bson");

// Exercises all JSON types and nuances of JSON serialization.
const doc1 = {
	string: "string\tdata\n",
	"": "", // zero-length string
	objectId: new bson.ObjectId(),
	int: 1,
	int2: -2345718,
	number: Math.PI,
	date0: new Date(1575271655328), // varying number of 0s in millis
	date1: new Date(1575271655028),
	date2: new Date(1575271655008),
	date3: new Date(1575271655000),
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
	arrayOfUndef: [1, undefined] // undefined is preserved (as null)
};

// For using C++ debugger
global.describe = global.describe || function describe(label, fn) { fn(); };
global.it = global.it || function it(label, fn) { fn(); };

for (const [name, loc] of [["JS", "../src/bson-to-json.js"], ["C++", "../build/Release/bsonToJson.node"]]) {
	const {bsonToJson} = require(loc);

	describe(`bson2json - ${name}`, function () {

		it("deserializes all JSON types", function () {
			const bsonBuffer = bson.serialize(doc1);
			const jsonBuffer = bsonToJson(bsonBuffer, false);

			const expected = JSON.parse(JSON.stringify(doc1));
			// The JSON string will contain 1152921500580315135, which parses to
			// 1152921500580315100.
			expected.long = doc1.long.toNumber();

			assert.deepEqual(
				JSON.parse(jsonBuffer.toString()),
				expected
			);
		});

		it("escapes strings properly", function () {
			const str = Buffer.allocUnsafe(0x7e);
			for (let i = 0; i < 0x7e; i++) str[i] = i;
			const obj = {str: str.toString()};

			const bsonBuffer = bson.serialize(obj);
			const jsonBuffer = bsonToJson(bsonBuffer, false);
			
			assert.deepEqual(jsonBuffer, Buffer.from(JSON.stringify(obj)));
			assert.equal(jsonBuffer.toString(), JSON.stringify(bson.deserialize(bsonBuffer)));
		});

		it("writes multi-byte characters properly", function () {
			const s1 = "𝌆"; // three bytes
			const s2 = "\uD834\udf06"; // same as s1
			const s3 = "\uDF06\uD834"; // encoded in BSON as ef bf bd (�)
			const s4 = "\uDEAD"; // encoded in BSON as ef bf bd (�)
			const s5 = "漢"; // two bytes
			const obj = {s1, s2, s3, s4, s5};

			const bsonBuffer = bson.serialize(obj);
			const jsonBuffer = bsonToJson(bsonBuffer, false);

			// Unlike the previous test, this can't use JSON.stringify for the
			// expectation because the lone surrogates are encoded into the BSON
			// as replacement characters.
			assert.equal(jsonBuffer.toString(), JSON.stringify(bson.deserialize(bsonBuffer)));
		});

		it("handles non-buffer inputs", function () {
			assert.throws(() => bsonToJson(undefined),
				new Error("Input must be a buffer"));
		});

		it("handles invalid short input", function () {
			assert.throws(() => bsonToJson(Buffer.allocUnsafeSlow(2)),
				new Error("Input buffer must have length >= 5"));
		});

		it("handles invalid string lengths", function () {
			const inv = Buffer.from([
				12, 0, 0, 0, // 12 B document
				2, // string
				"0".charCodeAt(0), 0, // key
				24, 0, 0, 0,
				0 // fewer than 24 B
			]);

			assert.throws(() => bsonToJson(inv, true),
				new Error("Bad string length"));
		});

		it("handles buffers too short for ObjectId", function () {
			const inv = Buffer.from([
				12, 0, 0, 0, // 12 B document
				7, // ObjectId
				"0".charCodeAt(0), 0, // key
				0, 0, 0, 0,
				0 // fewer than 12 B
			]);

			assert.throws(() => bsonToJson(inv, true),
				new Error("Truncated BSON (in ObjectId)"));
		});
		
		it("handles invalid short embedded documents", function () {
			const inv = Buffer.from([
				12, 0, 0, 0, // x B document
				3, // object
				"0".charCodeAt(0), 0, // key
				12, 0, 0, 0, // 12 B subdoc
				0 // fewer than 12 B
			]);

			assert.throws(() => bsonToJson(inv, true),
				new Error("BSON size exceeds input length"));
		});

		it("handles invalid short ints", function () {
			const inv = Buffer.from([
				9, 0, 0, 0, // x B document
				16, // int
				"0".charCodeAt(0), 0, // key
				5, 0
			]);

			assert.throws(() => bsonToJson(inv, true),
				new Error("Truncated BSON (in Int)"));
		});

		it("handles invalid short longs", function () {
			const inv = Buffer.from([
				9, 0, 0, 0, // x B document
				18, // long
				"0".charCodeAt(0), 0, // key
				5, 0
			]);

			assert.throws(() => bsonToJson(inv, true),
				new Error("Truncated BSON (in Long)"));
		});
	});
}

// TODO setup mongodb in CI
if (!process.env.GITHUB_ACTIONS)
describe("send", function () {
	const {bsonToJson, send} = require("../index.js");
	const mongodb = require("mongodb");
	const N = process.argv.includes("--benchmark") ? 2000 : 10;

	before(async function () {
		const url = "mongodb://localhost:27017/bsontojson";
		const opts = {useUnifiedTopology: true};
		const client = this.client = await mongodb.MongoClient.connect(url, opts);
		const db = this.db = client.db();

		const doc = this.doc = require("../package.json");
		const docs = Array.from({length: N}, () => ({...doc}));
		await db.collection("test1").insertMany(docs);
	});

	after(async function () {
		await this.db.dropCollection("test1");
		await this.client.close();
	});

	class MockOstr {
		constructor() { this.buffs = []; }
		write(d) { this.buffs.push(d); return true; }
		end(d) { this.buffs.push(d); }
		once(event, cb) { process.nextTick(cb); }
		toJSON() { return JSON.parse(Buffer.concat(this.buffs).toString()); }
	}

	it("works", async function () {
		const cursor = await this.db.collection("test1").find({}, {
			raw: true,
			// Set to < N/2 so both the inner and outer loops run a few times.
			batchSize: 3
		});
		const ostr = new MockOstr();
		await send(cursor, ostr);
		const parsed = ostr.toJSON();
		for (const o of parsed) {
			delete o._id;
			assert.deepEqual(o, this.doc);
		}
		assert.equal(parsed.length, N);
	});

	if (process.argv.includes("--benchmark")) {
		const doProfile = process.argv.includes("--profile");
		const k = 10;
		if (doProfile) {
			before(function (done) {
				const inspector = require("inspector");
				const session = this.session = new inspector.Session();
				session.connect();
				session.post("Profiler.enable", done);
			});
			beforeEach(function (done) {
				console.time("bench");
				this.session.post("Profiler.start", done);
			});
			afterEach(function (done) {
				this.session.post("Profiler.stop", (err, {profile}) => {
					console.timeEnd("bench");
					require("fs").writeFileSync(`${this.currentTest.title}.cpuprofile`, JSON.stringify(profile));
					done(err);
				});
			});
		}

		it("this", async function () {
			for (let i = 0; i < k; i++) {
				const cursor = await this.db.collection("test1").find({}, {raw: true});
				const ostr = new MockOstr();
				await send(cursor, ostr);
				if (!doProfile)
					assert.equal(ostr.toJSON().length, N);
			}
		});

		// These remaining cases benchmark other ways to iterate a cursor.

		// forEach has a silly call stack.
		it("forEach", async function () {
			for (let i = 0; i < k; i++) {
				const cursor = await this.db.collection("test1").find({}, {raw: true});
				const ostr = new MockOstr();
				const comma = Buffer.from(",");
				ostr.write(Buffer.from("["));
				let rest = false;
				await cursor.forEach(doc => {
					if (rest)
						ostr.write(comma);
					else
						rest = true;
					ostr.write(bsonToJson(doc, false));
				});
				ostr.write(Buffer.from("]"));
				if (!doProfile)
					assert.equal(ostr.toJSON().length, N);
			}
		});

		// Allocates a promise/awaits a tick per document
		it("await next", async function () {
			for (let i = 0; i < k; i++) {
				const cursor = await this.db.collection("test1").find({}, {raw: true});
				const ostr = new MockOstr();
				const comma = Buffer.from(",");
				ostr.write(Buffer.from("["));
				let rest = false, next;
				while ((next = await cursor.next())) {
					if (rest)
						ostr.write(comma);
					else
						rest = true;
					ostr.write(bsonToJson(next, false));
				}
				ostr.write(Buffer.from("]"));
				if (!doProfile)
					assert.equal(ostr.toJSON().length, N);
			}
		});

		// Allocates several promises/awaits several ticks per document
		it("async iter", async function () {
			for (let i = 0; i < k; i++) {
				const cursor = await this.db.collection("test1").find({}, {raw: true});
				const ostr = new MockOstr();
				const comma = Buffer.from(",");
				ostr.write(Buffer.from("["));
				let rest = false;
				for await (const doc of cursor) {
					if (rest)
						ostr.write(comma);
					else
						rest = true;
					ostr.write(bsonToJson(doc, false));
				}
				ostr.write(Buffer.from("]"));
				if (!doProfile)
					assert.equal(ostr.toJSON().length, N);
			}
		});
	}
});
