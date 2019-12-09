Directly converts BSON to a JSON string (stored in a Buffer). Useful for quickly
sending MongoDB database query results to a client over JSON+HTTP.

Still a work in progress. This module is *not* safe to use yet as it has no
output bounds checking (I first wanted to see how fast I could get it before
investing time in correctness). Current benchmark with a ~2500-element array of
medium objects (9MB BSON):

| Method | Time (ms) |
| ------ | --------: |
| `JSON.stringify(BSON.deserialize(arr))`<sup>1</sup> | 196 |
| this, JS | 36 |
| this, portable C++ | 25 |
| this, SSE4.2 | 13 |
| this, AVX2 | 11 |

<sup>1</sup> `BSON.deserialize` is the [official MongoDB js-bson implementation](https://github.com/mongodb/js-bson).

Major reasons it's fast:
* No UTF8 string decoding. String bytes can be copied almost directly (with JSON
  escaping).
* No waste temporary objects created for the GC to clean up.
* SSE4.2 or AVX2-accelerated JSON string escaping.
* AVX2-accelerated ObjectId hex string encoding, using the technique from
  [zbjornson/fast-hex](https://github.com/zbjornson/fast-hex).
* Fast number encoding, using the methods from `{fmt}`

This module also writes full-precision (64-bit signed) BSON Longs to the JSON buffer. (JSON does not specify a maximum numeric precision.) That isn't possible with js-bson as far as I know; it writes an object with low and high bits.

TODO:
* Checking bounds on output buffer. Right now allocates a fixed buffer that
  works for my particular test case.
* Try to squash the remaining v8 deoptimizations.
* Portable (non-AVX2) C++ ObjectId decoding.
* Preprocessor directives or dispatch for SSE4.2, AVX2
* Try to make the `isArray` parameter optional.
* Error handling (uses C++ exceptions but Node.js disables C++ exceptions)
* Optimize number (double) (currently uses sprintf)
* Escape \\uxxx in strings

Benchmarks by BSON type (ops/sec):

| Type | js-bson | this, JS | this, CPP |
| ---- | ---: | ---: | ---: |
| long | 1,928 | 390 | 2,759
| int | 1,655 | 537 | 4,512
| ObjectId | 1,048 | 784 | 5,070
| date | 413 | 278 | 1,122
| number | 1,008 | 318 | 1,003
| boolean | 440 | 754 | 2,783
| null | 482 | 723 | 3,455
| string | (varies)

(The C++ benchmarks are slower than they will be because of a temporary extra memcpy.)
