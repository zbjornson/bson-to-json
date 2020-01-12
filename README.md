Directly converts a BSON buffer to a JSON string stored in a Buffer. Useful for
quickly sending MongoDB database query results to a client over JSON+HTTP.

The output of this library should be identical to
`JSON.stringify(BSON.deserialize(v))`, with one exception: this module writes
full-precision (64-bit signed) BSON Longs to the JSON buffer. This is valid
because JSON does not specify a maximum numeric precision, but js-bson instead
writes an object with low and high bits.

Still a work in progress. Current benchmark with a ~2500-element array of
medium objects (9MB BSON):

| Method | Time (ms) |
| ------ | --------: |
| `JSON.stringify(BSON.deserialize(arr))`<sup>1</sup> | 226 |
| this, JS | 29 |
| this, portable C++ | 23 |
| this, SSE2 | 16 |
| this, SSE4.2 | 12 |
| this, AVX2 | 11 |

<sup>1</sup> `BSON.deserialize` is the [official MongoDB js-bson implementation](https://github.com/mongodb/js-bson).

Major reasons it's fast:
* Direct UTF8 to JSON-escaped string transcoding.
* No waste temporary objects created for the GC to clean up.
* SSE2, SSE4.2 or AVX2-accelerated JSON string escaping.
* AVX2-accelerated ObjectId hex string encoding, using the technique from
  [zbjornson/fast-hex](https://github.com/zbjornson/fast-hex).
* Fast integer encoding, using the methods from [`fmtlib/fmt`](https://github.com/fmtlib/fmt).
* Fast double encoding, using the same [double-conversion library](https://github.com/google/double-conversion)
  used in v8.

TODO:
- [ ] Fix read and write overruns.
- [ ] Fix crash when using iterator interface.
- [ ] Try to squash the remaining v8 deoptimizations in the JS implementation?
- [ ] Refactor so it's usable as a C++ library?

## Benchmarks by BSON type (ops/sec):

| Type | js-bson | this, JS | this, CPP (AVX2) |
| ---- | ---: | ---: | ---: |
| long | 1,928 | 390 | 36,918
| int | 1,655 | 537 | 16,348
| ObjectId | 1,048 | 784 | 33,403
| date | 413 | 278 | 1,455
| number | 1,008 | 318 | 2060
| boolean | 440 | 754 | 5,863
| null | 482 | 723 | 8,696
| string\<len=1000, esc=0.00><sup>1</sup> | 12,720 | 785 | 54,680
| string\<len=1000, esc=0.01> | 12,202 | 723 | 47,613
| string\<len=1000, esc=0.05> | 11,595 | 756 | 40,777

<sup>1</sup>String transcoding performance depends on the length of the string
(`len`) and the number of characters that must be escaped in the JSON output
(`esc`, a fraction from 0 to 1).

## Usage

### One-Shot

> ```ts
> bsonToJson(bson: Uint8Array, isArray?: boolean = true): Buffer
> // (note that Buffers extend Uint8Arrays, so `bson` can be a Buffer)
> ```

`isArray` specifies if the input is an array or not. BSON doesn't differentiate
between arrays and objects at the top level, so this must be provided.

* Pro: Easy to use.
* Con: May cause memory [reallocation](https://en.cppreference.com/w/c/memory/realloc)
  if the initial output buffer is too small. On Linux at least, this is usually
  an [efficient operation](http://blog.httrack.com/blog/2014/04/05/a-story-of-realloc-and-laziness/),
  however.
* Con: Entire output must fit in memory.

### Iterator (Streaming) (C++ only)

> ```ts
> new Transcoder(bson: Uint8Array, isArray?: boolean = true,
>     options?: ({chunkSize: number}|{fixedBuffer: ArrayBuffer})): Iterator<Buffer>
> // (note that Buffers extend Uint8Arrays, so `bson` can be a Buffer)
> ```

`isArray` specifies if the input is an array or not. BSON doesn't differentiate
between arrays and objects at the top level, so this must be provided.

* `chunkSize` can be specified to limit memory usage. The default value is
  estimated based on the input size and typical BSON expansion ratios such that
  a single output buffer will likely fit all of the data (i.e. the iterator will
  usually yield a value once or a few times).
* `fixedBuffer` if set to an instance of an ArrayBuffer, will decode into that
  memory in each iteration (the Buffer yielded in each iteration will be backed
  by that same ArrayBuffer). This limits memory usage and can improve
  performance (exactly one dynamic memory allocation).

```js
const iterator = new Transcoder(data);
for (const jsonBuf of iterator)
    res.write(jsonBuf); // res is an http server response or other writable stream
```
With a chunk size to limit memory usage:
```js
const iterator = new Transcoder(data, true, {chunkSize: 4096});
for (const jsonBuf of iterator)
    res.write(jsonBuf);
```
With a fixed buffer to limit memory usage and potentially improve performance:
```js
const iterator = new Transcoder(data, true, {fixedBuffer: new ArrayBuffer(4096)});
// jsonBuf is backed by the same memory in each iteration.
for (const jsonBuf of iterator) {
    // Wait for res to consume the output buffer.
    await new Promise(resolve => res.write(jsonBuf, resolve));
}
```

* Pro: Never causes memory reallocation. When the output is full, it's yielded.
* Pro: Can avoid all but one memory allocation by using a fixed buffer.
* Pro: Can specify a chunk size or fixed buffer if you want to limit memory usage.
* Con: Slightly harder to use.
