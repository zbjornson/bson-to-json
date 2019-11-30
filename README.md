Directly converts BSON to a JSON string. Useful for quickly sending MongoDB
database queries to a client over JSON+HTTP.

Current benchmark with a ~2500-element array of medium objects:

```
JSON.stringify(BSON.deserialize(obj)); // 1960 ms

new Transcoder().transcode(obj); // 380 ms
```

Major reasons it's faster:
* No UTF8 string decoding. String bytes can be copied directly (with JSON
  escaping).
* No waste temporary objects created for the GC to clean up.
* (Planned) ObjectIds can be transcoded directly to hex strings.

TODO:
* C++ version (using pcmpestri for JSON string escaping, pshufb for ObjectIds)
* Tests
* Checking bounds on output buffer
* Try to squash the remaining v8 deoptimizations.
