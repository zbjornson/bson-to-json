//@ts-check
'use strict';

const {Buffer} = require("buffer");
const Long = require("long");

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
	if (v < 1_000) return 4;
	if (v < 10_000) return 5;
	if (v < 100_000) return 6;
	if (v < 1_000_000) return 7;
	if (v < 10_000_000) return 8;
	if (v < 100_000_000) return 9;
	if (v < 1_000_000_000) return 10;
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

class Transcoder {
	constructor() {
		this.outIdx = 0;
	}

	/**
	 * @param {Buffer} input BSON-encoded input.
	 * @param {boolean} isArray BSON stores arrays and objects in the same
	 * format (arrrays are objects with numerical keys stored as strings).
	 * @public
	 */
	transcode(input, isArray = true) {
		// Estimate outLen at 2.5x inLen. (See C++ for explanation.)
		// TODO is it noticeable if this is the next multiple of 4096?
		this.out = Buffer.alloc((input.length * 10) >> 2);
		this.outIdx = 0;
		this.transcodeObject(input, 0, isArray);
		return this.out.slice(0, this.outIdx);
	}

	/**
	 * @param {number} n
	 * @returns {boolean} true if reallocation happened.
	 */
	ensureSpace(n) {
		if (this.outIdx + n < this.out.length)
			return false;
		
		const oldOut = this.out;
		const newOut = Buffer.alloc((oldOut.length * 3) >> 1);
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
	 * @param {Buffer} str
	 * @param {number} start Inclusive.
	 * @param {number} end Exclusive.
	 * @private
	 */
	writeStringRange(str, start, end) {
		this.ensureSpace(end - start);
		let out = this.out;
		for (let i = start; i < end; i++) {
			const c = str[i];
			let xc;
			if (c >= 0x20 /* & 0xe0*/ && c !== 0x22 && c !== 0x5c) { // no escape
				out[this.outIdx++] = c;
			} else if ((xc = ESCAPES[c])) { // single char escape
				this.ensureSpace(end - i + 1);
				out = this.out;
				out[this.outIdx++] = BACKSLASH;
				out[this.outIdx++] = xc;
			} else { // c < 0x20, control
				this.ensureSpace(end - i + 5);
				out = this.out;
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
	 * @param {Buffer} buffer
	 * @param {Number} start
	 * @private
	 */
	writeObjectId(buffer, start) {
		// This is the same speed as a 16B LUT and a 512B LUT, and doesn't
		// pollute the cache. js-bson is still winning in the ObjectId benchmark
		// though, despite having extra copying and a call into C++.
		this.ensureSpace(26);
		const out = this.out;
		this.out[this.outIdx++] = QUOTE;
		for (let i = start; i < start + 12; i++) {
			const byte = buffer[i];
			const hi = byte >>> 4;
			const lo = byte & 0xF;
			out[this.outIdx++] = hex(hi);
			out[this.outIdx++] = hex(lo);
		}
		this.out[this.outIdx++] = QUOTE;
	}

	/**
	 * @param {Buffer} val
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
	 * @param {Buffer} val
	 * @private
	 */
	addVal(val) {
		this.ensureSpace(val.length);
		const out = this.out;
		for (let i = 0; i < val.length; i++)
			out[this.outIdx++] = val[i];
	}

	/**
	 * @param {Buffer} in_
	 * @param {number} inIdx
	 * @param {boolean} isArray
	 * @private
	 */
	transcodeObject(in_, inIdx, isArray) {
		const inLen = in_.length;
		const size = readInt32LE(in_, inIdx);
		inIdx += 4;

		if (size < 5)
			throw new Error("BSON size must be >=5");
		if (size > inLen) // TODO + inIdx?
			throw new Error("BSON size exceeds input length.")

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
				// Name is a null-terminated string. TODO we can copy bytes as
				// we search.
				let nameStart = inIdx;
				let nameEnd = inIdx;
				while (in_[nameEnd] !== 0 && nameEnd < inLen)
					nameEnd++;
	
				if (nameEnd >= inLen)
					throw new Error('Bad BSON Document: illegal CString');

				this.ensureSpace(1);
				this.out[this.outIdx++] = QUOTE;
				this.writeStringRange(in_, nameStart, nameEnd);
				inIdx = nameEnd + 1; // +1 to skip null terminator
				this.ensureSpace(2);
				this.out[this.outIdx++] = QUOTE;
				this.out[this.outIdx++] = COLON;
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
				this.writeObjectId(in_, inIdx);
				inIdx += 12;
				break;
			}
			case BSON_DATA_INT: {
				const value = readInt32LE(in_, inIdx);
				inIdx += 4;
				// JS impl of fast_itoa is slower than this.
				this.addVal(Buffer.from(value.toString()));
				break;
			}
			case BSON_DATA_NUMBER: {
				// const value = in_.readDoubleLE(inIdx); // not sure which is faster TODO
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
				const value = in_[inIdx++] === 1;
				this.addVal(value ? TRUE : FALSE);
				break;
			}
			case BSON_DATA_OBJECT: {
				const objectSize = readInt32LE(in_, inIdx);
				this.transcodeObject(in_, inIdx, false);
				inIdx += objectSize;
				break;
			}
			case BSON_DATA_ARRAY: {
				const objectSize = readInt32LE(in_, inIdx);
				this.transcodeObject(in_, inIdx, true);
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
				throw new Error("Unknown BSON type");
			}

			arrIdx++;
		}

		this.ensureSpace(1);
		this.out[this.outIdx++] = isArray ? CLOSESQ : CLOSECURL;
	}
}

exports.bsonToJson = function bsonToJson(doc, isArray) {
	const t = new Transcoder();
	return t.transcode(doc, isArray);
};
