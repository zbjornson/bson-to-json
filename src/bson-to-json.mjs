//@ts-check

import Long from "long";

const BSON_DATA_NUMBER = 1;
const BSON_DATA_STRING = 2;
const BSON_DATA_OBJECT = 3;
const BSON_DATA_ARRAY = 4;
const BSON_DATA_BINARY = 5;
const BSON_DATA_UNDEFINED = 6;
const BSON_DATA_OID = 7;
const BSON_DATA_BOOLEAN = 8;
const BSON_DATA_DATE = 9;
const BSON_DATA_NULL = 10;
const BSON_DATA_REGEXP = 11;
const BSON_DATA_DBPOINTER = 12;
const BSON_DATA_CODE = 13;
const BSON_DATA_SYMBOL = 14;
const BSON_DATA_CODE_W_SCOPE = 15;
const BSON_DATA_INT = 16;
const BSON_DATA_TIMESTAMP = 17;
const BSON_DATA_LONG = 18;
const BSON_DATA_DECIMAL128 = 19;
const BSON_DATA_MIN_KEY = 0xff;
const BSON_DATA_MAX_KEY = 0x7f;

const QUOTE = '"'.charCodeAt(0);
const COLON = ':'.charCodeAt(0);
const COMMA = ','.charCodeAt(0);
const OPENSQ = '['.charCodeAt(0);
const OPENCURL = '{'.charCodeAt(0);
const CLOSESQ = ']'.charCodeAt(0);
const CLOSECURL = '}'.charCodeAt(0);
const BACKSLASH = '\\'.charCodeAt(0);
const LOWERCASE_U = 'u'.charCodeAt(0);
const ZERO = '0'.charCodeAt(0);
const ONE = '1'.charCodeAt(0);

const TRUE = Buffer.from("true");
const FALSE = Buffer.from("false");
const NULL = Buffer.from("null");

// Returns the number of digits in a null-terminated string representation of v.
function nDigits(v) {
	if (v < 10) return 2;
	if (v < 100) return 3;
	if (v < 1000) return 4;
	if (v < 10000) return 5;
	if (v < 100000) return 6;
	if (v < 1000000) return 7;
	if (v < 10000000) return 8;
	if (v < 100000000) return 9;
	if (v < 1000000000) return 10;
	return 11;
}

// ECMA-262 Table 65 (sec. 24.5.2.2)
const ESCAPES = {
	0x08: 'b'.charCodeAt(0),
	0x09: 't'.charCodeAt(0),
	0x0a: 'n'.charCodeAt(0),
	0x0c: 'f'.charCodeAt(0),
	0x0d: 'r'.charCodeAt(0),
	0x22: '"'.charCodeAt(0),
	0x5c: '\\'.charCodeAt(0)
};

function readInt32LE(buffer, index) {
	return buffer[index] |
		(buffer[index + 1] << 8) |
		(buffer[index + 2] << 16) |
		(buffer[index + 3] << 24);
}

const tb = Buffer.allocUnsafeSlow(8);
const ta = new Float64Array(tb.buffer, tb.byteOffset, 1);
function readDoubleLE(buffer, index) {
	tb[0] = buffer[index];
	tb[1] = buffer[index + 1];
	tb[2] = buffer[index + 2];
	tb[3] = buffer[index + 3];
	tb[4] = buffer[index + 4];
	tb[5] = buffer[index + 5];
	tb[6] = buffer[index + 6];
	tb[7] = buffer[index + 7];
	return ta[0];
}

function hex(nibble) {
	return nibble + (nibble < 10 ? 48 : 87);
}

export class PopulateInfo {
	constructor() {
		/** @type {Map<string, Map<string, Uint8Array>>} */
		this.paths = new Map();
		/** @type {Record<string, Set<string>>} */
		this.missingIds = Object.create(null);
	}

	/**
	 * @param {string} path
	 * @param {Uint8Array[]} items
	 */
	addItems(path, items) {
		if (!this.paths.has(path))
			this.paths.set(path, new Map());
		const map = /** @type {Map<string, Uint8Array>} */ (this.paths.get(path));
		const mpSet = this.missingIds[path];
		for (const item of items) {
			const t = new Transcoder();
			const jsonBuf = t.transcode(item);
			map.set(t.docId, jsonBuf);
			mpSet?.delete(t.docId);
		}
	}

	/**
	 * @param {string} path1
	 * @param {string} path2
	 */
	repeatPath(path1, path2) {
		const p1Map = this.paths.get(path1);
		if (!p1Map)
			throw new Error("Path not found: " + path1);
		this.paths.set(path2, p1Map);
	}

	/** @param {string} path */
	getMissingIdsForPath(path) {
		const o = [];
		for (const id of this.missingIds[path] ?? [])
			o.push(Buffer.from(id, "hex"));
		return o;
	}
}

export class Transcoder {
	constructor(populateInfo) {
		/** @private */
		this.outIdx = 0;
		/** @private */
		this.currentPath = "";
		/** @type {Buffer} */
		// @ts-expect-error
		this.out = null;
		/** @type {string} */
		this.docId = "";
		this.populateInfo = populateInfo;
	}

	/**
	 * Finds missing IDs for paths in the `populateInfo` object. Query for
	 * them, then call `populateInfo.addItems` with the results, then
	 * `transcode()` the `input`.
	 * @param {Uint8Array} input BSON-encoded input.
	 * @param {number} inIdx Internal
	 * @param {boolean} isArray Internal
	 * @param {string | null} baseKey Internal
	 */
	getMissingIds(input, inIdx = 0, isArray = false, baseKey = null) {
		const inLen = input.length;
		const size = readInt32LE(input, inIdx);

		if (size < 5)
			throw new Error("BSON size must be >= 5");
		if (size + inIdx > inLen)
			throw new Error("BSON size exceeds input length");

		inIdx += 4;

		let arrIdx = 0;

		while (true) {
			const elementType = input[inIdx++];
			if (elementType === 0) break;

			if (isArray) {
				// Skip the number of digits in the key.
				inIdx += nDigits(arrIdx);
			} else {
				// Name is a null-terminated string.
				const nameStart = inIdx;
				let nameEnd = inIdx;
				while (input[nameEnd] !== 0 && nameEnd < inLen)
					nameEnd++;
	
				if (nameEnd >= inLen)
					throw new Error("Bad BSON Document: illegal CString");

				inIdx = nameEnd + 1; // +1 to skip null terminator
				const key = input.subarray(nameStart, nameEnd);
				this.currentPath = baseKey ? `${baseKey}.${key}` : `${key}`;
			}

			switch (elementType) {
			case BSON_DATA_STRING: {
				const size = readInt32LE(input, inIdx);
				inIdx += 4;
				if (size <= 0 || size > inLen - inIdx)
					throw new Error("Bad string length");
				inIdx += size;
				break;
			}
			case BSON_DATA_OID: {
				if (inIdx + 12 > inLen)
					throw new Error("Truncated BSON (in ObjectId)");
				const idMapForPath = this.populateInfo?.paths.get(this.currentPath);
				if (idMapForPath) {
					const id = this.readObjectId(input, inIdx);
					const doc = idMapForPath.get(id);
					if (!doc) {
						(this.populateInfo.missingIds[this.currentPath] ??= new Set()).add(id);
					}
				}
				inIdx += 12;
				break;
			}
			case BSON_DATA_INT: {
				inIdx += 4;
				if (inIdx > inLen)
					throw new Error("Truncated BSON (in Int)");
				break;
			}
			case BSON_DATA_NUMBER:
			case BSON_DATA_DATE:
			case BSON_DATA_LONG: {
				inIdx += 8;
				if (inIdx > inLen)
					throw new Error("Truncated BSON");
				break;
			}
			case BSON_DATA_BOOLEAN: {
				inIdx++;
				if (inIdx > inLen)
					throw new Error("Truncated BSON (in Boolean)");
				break;
			}
			case BSON_DATA_OBJECT: {
				const objectSize = readInt32LE(input, inIdx);
				this.getMissingIds(input, inIdx, false, this.currentPath);
				inIdx += objectSize;
				break;
			}
			case BSON_DATA_ARRAY: {
				const objectSize = readInt32LE(input, inIdx);
				this.getMissingIds(input, inIdx, true, this.currentPath);
				inIdx += objectSize;
				if (input[inIdx - 1] !== 0)
					throw new Error("Invalid array terminator byte");
				break;
			}
			case BSON_DATA_NULL:
			case BSON_DATA_UNDEFINED: {
				break;
			}
			case BSON_DATA_DECIMAL128:
			case BSON_DATA_BINARY:
			case BSON_DATA_REGEXP:
			case BSON_DATA_SYMBOL:
			case BSON_DATA_TIMESTAMP:
			case BSON_DATA_MIN_KEY:
			case BSON_DATA_MAX_KEY:
			case BSON_DATA_CODE:
			case BSON_DATA_CODE_W_SCOPE:
			case BSON_DATA_DBPOINTER:
				throw new Error("BSON type incompatible with JSON");
			default:
				throw new Error("Unknown BSON type " + elementType);
			}

			arrIdx++;
		}
	}

	/**
	 * @param {Uint8Array} input BSON-encoded input.
	 * @param {boolean} [isArray] BSON stores arrays and objects in the same
	 * format (arrays are objects with numerical keys stored as strings).
	 * @param {number} [chunkSize] Initial size of the output buffer. Setting to
	 * 0 uses 2.5x the inLen.
	 * @public
	 */
	transcode(input, isArray = false, chunkSize = 0) {
		if (!(input instanceof Uint8Array))
			throw new Error("Input must be a buffer");
		if (input.length < 5)
			throw new Error("Input buffer must have length >= 5");
		// Estimate outLen at 2.5x inLen. (See C++ for explanation.)
		chunkSize ||= (input.length * 10) >> 2;
		this.out = Buffer.alloc(chunkSize);
		this.outIdx = 0;
		this.transcodeObject(input, 0, isArray);
		const r = this.out.slice(0, this.outIdx);
		// @ts-expect-error
		this.out = null;
		this.outIdx = 0;
		return r;
	}

	/**
	 * @param {number} n
	 * @returns {boolean} true if reallocation happened.
	 */
	ensureSpace(n) {
		if (this.outIdx + n < this.out.length)
			return false;
		
		const oldOut = this.out;
		const m = Math.max(n, oldOut.length);
		const newOut = Buffer.alloc((m * 3) >> 1);
		oldOut.copy(newOut);
		this.out = newOut;
		return true;
	}

	/**
	 * Writes the bytes in `str` from `start` to `end` (exclusive) into `out`,
	 * escaping per ECMA-262 sec 24.5.2.2.
	 *
	 * Regarding [well-formed
	 * stringify](https://github.com/tc39/proposal-well-formed-stringify), the
	 * js-bson encoder uses Node.js' `buffer.write(value, index, "utf8")`, which
	 * converts unpaired surrogates to the byte sequence `ef bf bd`, which
	 * decodes to `0xfffd` (� REPLACEMENT CHARACTER, used to replace an unknown,
	 * unrecognized or unrepresentable character). Thus there's nothing we can
	 * do in the decoder to instead emit escape sequences.
	 *
	 * @param {Uint8Array} str
	 * @param {number} start Inclusive.
	 * @param {number} end Exclusive.
	 * @private
	 */
	writeStringRange(str, start, end) {
		for (let i = start; i < end; i++) {
			const c = str[i];
			let xc;
			if (c >= 0x20 /* & 0xe0*/ && c !== 0x22 && c !== 0x5c) { // no escape
				this.ensureSpace(1);
				this.out[this.outIdx++] = c;
			} else if ((xc = ESCAPES[c])) { // single char escape
				this.ensureSpace(2);
				const out = this.out;
				out[this.outIdx++] = BACKSLASH;
				out[this.outIdx++] = xc;
			} else { // c < 0x20, control
				this.ensureSpace(6);
				const out = this.out;
				out[this.outIdx++] = BACKSLASH;
				out[this.outIdx++] = LOWERCASE_U;
				out[this.outIdx++] = ZERO;
				out[this.outIdx++] = ZERO;
				out[this.outIdx++] = (c & 0xF0) ? ONE : ZERO;
				out[this.outIdx++] = hex(c & 0xF);
			}
		}
	}

	/**
	 * Reads an ObjectId as a string.
	 * @param {Uint8Array} buffer
	 * @param {Number} start
	 * @returns {string}
	 * @private
	 */
	readObjectId(buffer, start) {
		let str = "";
		for (let i = start; i < start + 12; i++) {
			// TODO(perf) optimize
			const byte = buffer[i];
			const hi = byte >>> 4;
			const lo = byte & 0xF;
			str += String.fromCharCode(hex(hi)) + String.fromCharCode(hex(lo));
		}
		return str;
	}

	/**
	 * @param {Uint8Array} buffer
	 * @param {Number} start
	 * @private
	 */
	writeObjectId(buffer, start) {
		// This is the same speed as a 16B LUT and a 512B LUT, and doesn't
		// pollute the cache. js-bson is still winning in the ObjectId benchmark
		// though, despite having extra copying and a call into C++.
		this.ensureSpace(26);
		const out = this.out;
		out[this.outIdx++] = QUOTE;
		for (let i = start; i < start + 12; i++) {
			const byte = buffer[i];
			const hi = byte >>> 4;
			const lo = byte & 0xF;
			out[this.outIdx++] = hex(hi);
			out[this.outIdx++] = hex(lo);
		}
		out[this.outIdx++] = QUOTE;
	}

	/**
	 * @param {Uint8Array} buffer
	 * @private
	 */
	writeBuffer(buffer) {
		this.ensureSpace(buffer.length);
		this.out.set(buffer, this.outIdx);
		this.outIdx += buffer.length;
	}

	/**
	 * @param {Uint8Array} val
	 * @private
	 */
	addQuotedVal(val) {
		this.ensureSpace(val.length + 2);
		const out = this.out;
		out[this.outIdx++] = QUOTE;
		for (let i = 0; i < val.length; i++)
			out[this.outIdx++] = val[i];
		out[this.outIdx++] = QUOTE;
	}

	/**
	 * @param {Uint8Array} val
	 * @private
	 */
	addVal(val) {
		this.ensureSpace(val.length);
		const out = this.out;
		for (let i = 0; i < val.length; i++)
			out[this.outIdx++] = val[i];
	}

	/**
	 * @param {Uint8Array} in_
	 * @param {number} inIdx
	 * @param {boolean} isArray
	 * @param {string} [baseKey]
	 * @private
	 */
	transcodeObject(in_, inIdx, isArray, baseKey) {
		const inLen = in_.length;
		const size = readInt32LE(in_, inIdx);

		if (size < 5)
			throw new Error("BSON size must be >= 5");
		if (size + inIdx > inLen)
			throw new Error("BSON size exceeds input length");

		inIdx += 4;

		let arrIdx = 0;

		this.ensureSpace(1);
		this.out[this.outIdx++] = isArray? OPENSQ : OPENCURL;

		while (true) {
			const elementType = in_[inIdx++];
			if (elementType === 0) break;

			if (arrIdx) {
				this.ensureSpace(1);
				this.out[this.outIdx++] = COMMA;
			}

			if (isArray) {
				// Skip the number of digits in the key.
				inIdx += nDigits(arrIdx);
			} else {
				// Name is a null-terminated string. TODO(perf) we can copy
				// bytes as we search.
				const nameStart = inIdx;
				let nameEnd = inIdx;
				while (in_[nameEnd] !== 0 && nameEnd < inLen)
					nameEnd++;
	
				if (nameEnd >= inLen)
					throw new Error("Bad BSON Document: illegal CString");

				this.ensureSpace(1);
				this.out[this.outIdx++] = QUOTE;
				this.writeStringRange(in_, nameStart, nameEnd);
				inIdx = nameEnd + 1; // +1 to skip null terminator
				this.ensureSpace(2);
				this.out[this.outIdx++] = QUOTE;
				this.out[this.outIdx++] = COLON;
				const key = in_.subarray(nameStart, nameEnd);
				this.currentPath = baseKey ? `${baseKey}.${key}` : `${key}`;
			}

			switch (elementType) {
			case BSON_DATA_STRING: {
				const size = readInt32LE(in_, inIdx);
				inIdx += 4;
				if (size <= 0 || size > inLen - inIdx)
					throw new Error("Bad string length");

				this.ensureSpace(1 + size);
				this.out[this.outIdx++] = QUOTE;
				this.writeStringRange(in_, inIdx, inIdx + size - 1);
				inIdx += size;
				this.ensureSpace(1);
				this.out[this.outIdx++] = QUOTE;
				break;
			}
			case BSON_DATA_OID: {
				if (inIdx + 12 > inLen)
					throw new Error("Truncated BSON (in ObjectId)");

				if (!baseKey && this.currentPath === "_id") {
					this.docId = this.readObjectId(in_, inIdx);
				}

				const idMapForPath = this.populateInfo?.paths.get(this.currentPath);
				if (idMapForPath) {
					const id = this.readObjectId(in_, inIdx);
					const doc = idMapForPath.get(id);
					if (doc) {
						this.writeBuffer(doc);
					} else {
						// doc missing, write ObjectId as fallback
						this.writeObjectId(in_, inIdx);
					}
				} else {
					this.writeObjectId(in_, inIdx);
				}
				inIdx += 12;
				break;
			}
			case BSON_DATA_INT: {
				if (4 + inIdx > inLen)
					throw new Error("Truncated BSON (in Int)");
				const value = readInt32LE(in_, inIdx);
				inIdx += 4;
				// JS impl of fast_itoa is slower than this.
				this.addVal(Buffer.from(value.toString()));
				break;
			}
			case BSON_DATA_NUMBER: {
				if (8 + inIdx > inLen)
					throw new Error("Truncated BSON (in Int)");
				// const value = in_.readDoubleLE(inIdx); // not sure which is faster TODO (perf)
				const value = readDoubleLE(in_, inIdx);
				inIdx += 8;
				if (Number.isFinite(value)) {
					this.addVal(Buffer.from(value.toString()));
				} else {
					this.addVal(NULL);
				}
				break;
			}
			case BSON_DATA_DATE: {
				if (8 + inIdx > inLen)
					throw new Error("Truncated BSON (in Date)");
				const lowBits = readInt32LE(in_, inIdx);
				inIdx += 4;
				const highBits = readInt32LE(in_, inIdx);
				inIdx += 4;
				const ms = new Long(lowBits, highBits).toNumber();
				const value = Buffer.from(new Date(ms).toISOString());
				this.addQuotedVal(value);
				break;
			}
			case BSON_DATA_BOOLEAN: {
				if (1 + inIdx > inLen)
					throw new Error("Truncated BSON (in Boolean)");
				const value = in_[inIdx++] === 1;
				this.addVal(value ? TRUE : FALSE);
				break;
			}
			case BSON_DATA_OBJECT: {
				const objectSize = readInt32LE(in_, inIdx);
				this.transcodeObject(in_, inIdx, false, this.currentPath);
				inIdx += objectSize;
				break;
			}
			case BSON_DATA_ARRAY: {
				const objectSize = readInt32LE(in_, inIdx);
				this.transcodeObject(in_, inIdx, true, this.currentPath);
				inIdx += objectSize;
				if (in_[inIdx - 1] !== 0)
					throw new Error("Invalid array terminator byte");
				break;
			}
			case BSON_DATA_NULL: {
				this.addVal(NULL);
				break;
			}
			case BSON_DATA_LONG: {
				if (8 + inIdx > inLen)
					throw new Error("Truncated BSON (in Long)");
				const lowBits = readInt32LE(in_, inIdx);
				inIdx += 4;
				const highBits = readInt32LE(in_, inIdx);
				inIdx += 4;
				let vx;
				if (highBits === 0) {
					vx = lowBits;
				} else {
					vx = new Long(lowBits, highBits);
				}
				const value = Buffer.from(vx.toString());
				this.addVal(value);
				break;
			}
			case BSON_DATA_UNDEFINED:
				// noop
				break;
			case BSON_DATA_DECIMAL128:
			case BSON_DATA_BINARY:
			case BSON_DATA_REGEXP:
			case BSON_DATA_SYMBOL:
			case BSON_DATA_TIMESTAMP:
			case BSON_DATA_MIN_KEY:
			case BSON_DATA_MAX_KEY:
			case BSON_DATA_CODE:
			case BSON_DATA_CODE_W_SCOPE:
			case BSON_DATA_DBPOINTER:
				throw new Error("BSON type incompatible with JSON");
			default:
				throw new Error("Unknown BSON type " + elementType);
			}

			arrIdx++;
		}

		this.ensureSpace(1);
		this.out[this.outIdx++] = isArray ? CLOSESQ : CLOSECURL;
	}
}

export const ISE = "JavaScript";
