![Node.js CI](https://github.com/zbjornson/bson-to-json/workflows/Node.js%20CI/badge.svg)

Directly and quickly converts a BSON buffer to a JSON string stored in a Buffer.
Useful for sending MongoDB database query results to a client over JSON+HTTP.

Benchmark with a ~2500-element array of medium objects (9MB BSON):

| Method | Time (ms) |
| ------ | --------: |
| `JSON.stringify(BSON.deserialize(arr))`<sup>1</sup> | 226.0 |
| this, JS | 39.7 |
| this, portable C++ | 20.6 |
| this, SSE2 | 15.2 |
| this, SSE4.2 | 11.5 |
| this, AVX2 | 10.6 |

<sup>1</sup> `BSON.deserialize` is the [official MongoDB js-bson implementation](https://github.com/mongodb/js-bson).

## Installation

*The C++ implementations require a C++ compiler. See instructions [here](https://github.com/nodejs/node-gyp#on-unix). If you do not have a C++ compiler, the slower JS version will be used.*

```
yarn add zbjornson/bson-to-json
# or
npm install zbjornson/bson-to-json
```

> [!Warning]
> Please don't use the version on npm (ozonep-bson-to-json). Someone else
> published this module with changes that should not have been made.

## Usage

### `new Transcoder(p?: PopulateInfo)`

Constructs a new Transcoder.

`p` is an optional instance of the `PopulateInfo` class that is used for
client-side joins.

### `Transcoder#transcode(bson: Uint8Array): Buffer`

Transcodes a BSON document to a JSON string stored in a Buffer.

Note that Buffers extend Uint8Array, so `bson` can be a Buffer instance.

The output should be identical to `JSON.stringify(BSON.deserialize(v))`, with
two exceptions:

1. This module writes full-precision (64-bit signed) BSON Longs to the JSON
   buffer. This is valid because JSON does not specify a maximum numeric
   precision, but js-bson instead writes an object with low and high bits.
2. This module does more/better input bounds checking than js-bson, so this
   module may throw different errors. (js-bson seems to rely, intentionally or
   not, on indexing past the end of a typed array returning `undefined`.)

#### Example

> ```ts
> const {Transcoder, PopulateInfo} = require("bson-to-json");
> const p = new PopulateInfo();
> const t = new Transcoder(p);
> const buf = t.transcode(bson: Uint8Array);
> ```

### `send`

> ```ts
> const {send} = require("bson-to-json");
> send(cursor: MongoDbCursor, ostr: Stream.Writable): Promise<void>
> ```

**This function hasn't been updated recently and might not work (quickly).**
Efficiently sends the contents of a MongoDB cursor to a writable stream (e.g.
an HTTP response). The returned Promise resolves when the cursor is drained, or
rejects in case of an error.

#### Example usage in an HTTP handler

```js
const {send} = require("bson-to-json");
async function (req, res) {
  const cursor = await db.collection("mycol").find({name: "Zach"}, {raw: true});
  res.setHeader("Content-Type", "application/json");
  await send(cursor, res);
}
```

This is the fastest way to transfer results from MongoDB to a client. MongoDB's
`cursor.forEach` or `for await (const doc of cursor)` both have much higher CPU
and memory overhead.

### `ISE`

> ```ts
> const {ISE} = require("bson-to-json");
> ISE: string
> ```

A constant indicating what instruction set extension was used (based on your
CPU's available features). One of `"AVX512"`, `"AVX2"`, `"SSE4.2"`, `"SSE2"`,
`"Baseline"` (portable C) or `"JavaScript"`.

## Performance notes

### Major reasons it's fast

* No waste temporary objects created for the GC to clean up.
* Direct UTF8 to JSON-escaped string transcoding.
* SSE2, SSE4.2 or AVX2-accelerated JSON string escaping.
* AVX2-accelerated ObjectId hex string encoding, using the technique from
  [zbjornson/fast-hex](https://github.com/zbjornson/fast-hex).
* Fast integer encoding, using the method from [`fmtlib/fmt`](https://github.com/fmtlib/fmt).
* Fast double encoding, using the same [double-conversion library](https://github.com/google/double-conversion)
  used in V8.
* Native 64-bit operations used for Dates and Longs.
* Skips decoding array keys (which BSON stores as ASCII numbers) and instead
  advances by the known number of bytes in the key.
* The `send` method has a tight call stack and avoids allocating a Promise for
  each document (compared to `for await...of`).

### Benchmarks by BSON type (ops/sec):

| Type | js-bson | this, JS | this, CPP (AVX2) |
| ---- | ---: | ---: | ---: |
| long | 4,157 | 1,851 | 46,880
| int | 6,128 | 5,051 | 56,153
| ObjectId | 2,246 | 12,579 | 47,430
| date | 988 | 829 | 17,325
| number | 2,193 | 996 | 3,117
| boolean | 1,299 | 1,246 | 10,633
| null | 1,476 | 1,251 | 11,405
| string\<len=1000, esc=0.00><sup>1</sup> | 18,488 | 5,392 | 76,628
| string\<len=1000, esc=0.01> | 17,155 | 5,328 | 63,107
| string\<len=1000, esc=0.05> | 15,982 | 4,864 | 51,982

The JS version is not meant for production use. While it still reduces GC
overhead, it is typically slower than js-bson.

<sup>1</sup>String transcoding performance depends on the length of the string
(`len`) and the number of characters that must be escaped in the JSON output
(`esc`, a fraction from 0 to 1).

## Future Plans

- Consider adding an option to prepend a comma to the output so it can be used
  with MongoDB cursors more efficiently.
