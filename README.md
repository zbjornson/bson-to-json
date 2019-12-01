Directly converts BSON to a JSON string (stored in a Buffer). Useful for quickly
sending MongoDB database query results to a client over JSON+HTTP.

Still a work in progress. This module is *not* safe to use yet as it has no
output bounds checking (I first wanted to see how fast I could get it before
investing time in correctness). Current benchmark with a ~2500-element array of
medium objects (9MB BSON):

| Method | Time (ms) |
| ------ | --------: |
| `JSON.stringify(BSON.deserialize(arr))`* | 196 |
| this, JS | 36 |
| this, portable C++ | 25 |
| this, SSE4.2 | 13 |
| this, AVX2 | 11 |

* `BSON.deserialize` is the [official MongoDB js-bson implementation](https://github.com/mongodb/js-bson).

Major reasons it's fast:
* No UTF8 string decoding. String bytes can be copied almost directly (with JSON
  escaping).
* No waste temporary objects created for the GC to clean up.
* (Planned) ObjectIds transcoded directly to hex strings.
* SSE4.2 or AVX2-accelerated JSON string escaping.
* AVX2-accelerated ObjectId hex string encoding, using the technique from
  [zbjornson/fast-hex](https://github.com/zbjornson/fast-hex).

TODO:
* Tests
* Checking bounds on output buffer
* Try to squash the remaining v8 deoptimizations.
