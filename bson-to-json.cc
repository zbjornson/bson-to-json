#include <nan.h>
#include <stdint.h>
#include <string>
#include <exception>
#include <cstdio> // sprintf

constexpr uint8_t BSON_DATA_NUMBER = 1;
constexpr uint8_t BSON_DATA_STRING = 2;
constexpr uint8_t BSON_DATA_OBJECT = 3;
constexpr uint8_t BSON_DATA_ARRAY = 4;
constexpr uint8_t BSON_DATA_BINARY = 5;
constexpr uint8_t BSON_DATA_UNDEFINED = 6;
constexpr uint8_t BSON_DATA_OID = 7;
constexpr uint8_t BSON_DATA_BOOLEAN = 8;
constexpr uint8_t BSON_DATA_DATE = 9;
constexpr uint8_t BSON_DATA_NULL = 10;
constexpr uint8_t BSON_DATA_REGEXP = 11;
constexpr uint8_t BSON_DATA_DBPOINTER = 12;
constexpr uint8_t BSON_DATA_CODE = 13;
constexpr uint8_t BSON_DATA_SYMBOL = 14;
constexpr uint8_t BSON_DATA_CODE_W_SCOPE = 15;
constexpr uint8_t BSON_DATA_INT = 16;
constexpr uint8_t BSON_DATA_TIMESTAMP = 17;
constexpr uint8_t BSON_DATA_LONG = 18;
constexpr uint8_t BSON_DATA_DECIMAL128 = 19;
constexpr uint8_t BSON_DATA_MIN_KEY = 0xff;
constexpr uint8_t BSON_DATA_MAX_KEY = 0x7f;

constexpr uint8_t QUOTE = '"';
constexpr uint8_t COLON = ':';
constexpr uint8_t COMMA = ',';
constexpr uint8_t CLOSESQ = ']';
constexpr uint8_t CLOSECURL = '}';
constexpr uint8_t BACKSLASH = '\\';

constexpr uint8_t NOESC = 91;
// Returns the char to use to escape c if it requires escaping, or returns
// NOESC if it does not require escaping. NOESC is a valid character in a JSON
// string and must have been handled already; see writeEscapedChars.
uint8_t getEscape(uint8_t c) {
	switch (c) {
	case 8: return 'b';
	case 9: return 't';
	case 10: return 'n';
	case 12: return 'f';
	case 13: return 'r';
	case 34: return c;
	case 47: return c;
	case 92: return c;
	default: return NOESC;
	}
}

class Transcoder {
public:
	uint8_t* out = nullptr;
	size_t outIdx = 0;

	void transcode(uint8_t* in_, size_t inLen_, bool isArray = true) {
		in = in_;
		inLen = inLen_;
		inIdx = 0;

		out = new uint8_t[1e8];
		outIdx = 0;

		transcodeObject(isArray);
	}

	void neuter() {
		delete[] out;
		out = nullptr;
		outIdx = 0;
	}

private:
	const uint8_t* in = nullptr;
	size_t inIdx = 0;
	size_t inLen = 0;

	int64_t readInt64LE() {
		int64_t v = reinterpret_cast<const int64_t*>(in + inIdx)[0]; // (UB, LE)
		inIdx += 8;
		return v;
	}

	int32_t readInt32LE(bool advance = true) {
		int32_t v = in[inIdx] | (in[inIdx + 1] << 8) |
			(in[inIdx + 2] << 16) | (in[inIdx + 3] << 24);
		if (advance)
			inIdx += 4;
		return v;
	}

	double readDoubleLE() {
		double v = reinterpret_cast<const double*>(in + inIdx)[0]; // (UB, LE)
		inIdx += 8;
		return v;
	}

	void writeEscapedChars(size_t n) {
		const size_t end = inIdx + n - 1;
		uint8_t xc;
		while (inIdx < end) {
			const uint8_t c = in[inIdx++];
			if (c > 47 && c != 92) {
				out[outIdx++] = c;
			} else if ((xc = getEscape(c)) != NOESC) {
				out[outIdx++] = '\\';
				out[outIdx++] = xc;
			} else {
				out[outIdx++] = c;
			}
		}
		inIdx++; // skip null byte
	}

	void transcodeObject(bool isArray) {
		size_t size = readInt32LE();
		if (size < 5 || size > inLen) // + inIdx?
			throw std::exception("BSON size exceeds input length.");

		bool first = true;

		out[outIdx++] = isArray ? '[' : '{';

		while (true) {
			const uint8_t elementType = in[inIdx++];
			if (elementType == 0) break;

			if (first) {
				first = false;
			} else {
				out[outIdx++] = ',';
			}

			{ // Write name
				size_t nameStart = inIdx;
				size_t strsz = inLen - inIdx;
				const size_t nameEnd = nameStart + strnlen_s(reinterpret_cast<const char*>(in + inIdx), strsz);
				if (nameEnd == strsz)
					throw std::exception("Name terminator not found");

				if (!isArray) {
					out[outIdx++] = '"';
					while (nameStart < nameEnd)
						out[outIdx++] = in[nameStart++];
					out[outIdx++] = '"';
					out[outIdx++] = ':';
				}

				inIdx = nameEnd + 1;
			}

			switch (elementType) {
			case BSON_DATA_STRING: {
				const int32_t size = readInt32LE();
				if (size <= 0 || size > inLen - inIdx)
					throw std::exception("Bad string length");
				out[outIdx++] = '"';
				writeEscapedChars(size);
				out[outIdx++] = '"';
				break;
			}
			case BSON_DATA_OID: {
				// TODO
				inIdx += 12;
				break;
			}
			case BSON_DATA_INT: {
				const int32_t value = readInt32LE();
				const int n = sprintf(reinterpret_cast<char*>(out + outIdx), "%d", value);
				// Safer:
				// snprintf(reinterpret_cast<char*>(out) + outIdx, nRemaining, "%d", value);
				outIdx += n;
				break;
			}
			case BSON_DATA_NUMBER: {
				const double value = readDoubleLE();
				const int n = sprintf(reinterpret_cast<char*>(out + outIdx), "%.*f", DBL_DIG, value);
				outIdx += n;
				break;
			}
			case BSON_DATA_DATE: {
				// TODO
				inIdx += 8;
				break;
			}
			case BSON_DATA_BOOLEAN: {
				const uint8_t val = in[inIdx++];
				if (val == 1) {
					out[outIdx++] = 't';
					out[outIdx++] = 'r';
					out[outIdx++] = 'u';
					out[outIdx++] = 'e';
				} else {
					out[outIdx++] = 'f';
					out[outIdx++] = 'a';
					out[outIdx++] = 'l';
					out[outIdx++] = 's';
					out[outIdx++] = 'e';
				}
				break;
			}
			case BSON_DATA_OBJECT: {
				const int32_t size = readInt32LE(false);
				if (size <= 0 || size > inLen - inIdx) // TODO this check is unnecessary
					throw std::exception("Bad embedded document length");
				transcodeObject(false);
				//inIdx += size;
				break;
			}
			case BSON_DATA_ARRAY: {
				const int32_t size = readInt32LE(false);
				if (size <= 0 || size > inLen - inIdx)
					throw std::exception("Bad embedded document length");
				transcodeObject(true);
				//inIdx += size;
				if (in[inIdx - 1] != 0)
					throw std::exception("Invalid array terminator byte");
				break;
			}
			case BSON_DATA_NULL: {
				out[outIdx++] = 'n';
				out[outIdx++] = 'u';
				out[outIdx++] = 'l';
				out[outIdx++] = 'l';
				break;
			}
			case BSON_DATA_LONG: {
				const int64_t value = readInt64LE();
				const int n = sprintf(reinterpret_cast<char*>(out + outIdx), "%I64d", value);
				outIdx += n;
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
				// incompatible JSON type
				break;
			default:
				throw std::exception("Unknown BSON type");
			}
		}

		out[outIdx++] = isArray ? ']' : '}';
	}
};

NAN_METHOD(bsonToJson) {
	v8::Isolate* iso = Nan::GetCurrentContext()->GetIsolate();
	v8::Local<v8::Uint8Array> arr = info[0].As<v8::Uint8Array>();
	Nan::TypedArrayContents<uint8_t> data(arr);

	Transcoder trans;
	trans.transcode(*data, data.length(), true);

	v8::Local<v8::Value> buf = node::Buffer::Copy(iso, reinterpret_cast<char*>(trans.out), trans.outIdx).ToLocalChecked();

	trans.neuter();

	info.GetReturnValue().Set(buf);
}

NAN_MODULE_INIT(Init) {
	NAN_EXPORT(target, bsonToJson);
}

NODE_MODULE(bsonToJson, Init);
