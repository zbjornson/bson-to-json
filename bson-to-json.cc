#include "napi.h"
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string.h>
#include <stdexcept>
#include <cstdio> // sprintf
#include <cfloat> // DBL_DIG
#include <ctime> // strftime
#include <thread>
#include <mutex>
#include <condition_variable>

#include <iostream>

#ifdef _MSC_VER
# include <intrin.h>
#elif defined(__GNUC__)
# include <x86intrin.h>
#endif

#ifdef _MSC_VER
// long is 4B in MSVC, 8 Clang and GCC
constexpr const char* int64spec = "%I64d";
#else
constexpr const char* int64spec = "%ld";
#endif

using std::size_t;
using std::uint8_t;
using std::int32_t;
using std::uint32_t;
using std::int64_t;

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

// Adaptetd from https://github.com/fmtlib/fmt/blob/master/include/fmt/format.h#L2818,
// MIT license
const char digits[] =
	"0001020304050607080910111213141516171819"
	"2021222324252627282930313233343536373839"
	"4041424344454647484950515253545556575859"
	"6061626364656667686970717273747576777879"
	"8081828384858687888990919293949596979899";

template<typename T>
size_t fast_itoa(uint8_t* buf, T val) {
	char temp[20]; // largest int32 is 10 digits + sign, largest int64 is 19+sign
	char* p = temp + 10;
	size_t n = 0;

	const bool isNegative = val < 0;
	if (isNegative)
		val = 0 - val;

	while (val >= 100) {
		size_t index = static_cast<size_t>((val % 100) * 2);
		val /= 100;
		*--p = digits[index + 1];
		*--p = digits[index];
		n += 2;
	}

	if (val < 10) {
		*--p = static_cast<uint8_t>('0' + val);
		n++;
	} else {
		size_t index = static_cast<size_t>(val * 2);
		*--p = digits[index + 1];
		*--p = digits[index];
		n += 2;
	}

	if (isNegative) {
		*--p = '-';
		n++;
	}

	memcpy(buf, p, n);
	return n;
}

// Technique from https://github.com/zbjornson/fast-hex
inline static __m256i encodeHex(__m128i val) {
	const __m256i HEX_LUTR = _mm256_setr_epi8(
		'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
		'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f');
	const __m256i ROT2 = _mm256_setr_epi8(
		-1, 0, -1, 2, -1, 4, -1, 6, -1, 8, -1, 10, -1, 12, -1, 14,
		-1, 0, -1, 2, -1, 4, -1, 6, -1, 8, -1, 10, -1, 12, -1, 14
	);
	// Bytes to nibbles (a -> [a >> 4, a & 0b1111]):
	__m256i doubled = _mm256_cvtepu8_epi16(val);
	__m256i hi = _mm256_srli_epi16(doubled, 4);
	__m256i lo = _mm256_shuffle_epi8(doubled, ROT2);
	__m256i bytes = _mm256_or_si256(hi, lo); // avx512: mask_shuffle to avoid the OR
	bytes = _mm256_and_si256(bytes, _mm256_set1_epi8(0b1111));
	// Encode hex
	return _mm256_shuffle_epi8(HEX_LUTR, bytes);
}

// Returns the char to use to escape c if it requires escaping, or returns
// NOESC if it does not require escaping. NOESC is a valid character in a JSON
// string and must have been handled already; see writeEscapedChars.
inline static uint8_t getEscape(uint8_t c) {
	switch (c) {
	case 0x08: return 'b';
	case 0x09: return 't';
	case 0x0a: return 'n';
	case 0x0c: return 'f';
	case 0x0d: return 'r';
	case 0x22: return c; // "
	case 0x5c: return c; /* \ */
	default: return 0;
	}
}

class Transcoder {
public:
	enum Mode {
		/* Reallocate the output buffer as necessary to fit entire contents. */
		REALLOC,
		/* Pause when output buffer is full. Adjust `out`, `outIdx` and
		 * `outLen`, then call `resume` when ready to continue.
		 */
		PAUSE
	};

	uint8_t* out = nullptr;
	size_t outIdx = 0;
	size_t outLen = 0;

	/// <summary>
	/// Transcodes the BSON document to JSON. Call once per lifetime of the
	/// Transcoder instance.
	/// </summary>
	/// <param name="in_">BSON document</param>
	/// <param name="inLen_">Length of input</param>
	/// <param name="isArray">Whether or not the document is an array.</param>
	/// <param name="chunkSize">
	/// Initial size of the output buffer. Setting to 0 currently uses 2.5x
	/// the inLen.
	/// </param>
	/// <param name="mode_">What to do when the output buffer is full.</param>
	/// <param name="fixedOut">With Mode::PAUSE, output buffer to reuse.</param>
	void transcode(const uint8_t* in_, size_t inLen_, bool isArray = true,
			size_t chunkSize = 0, Mode mode_ = Mode::REALLOC, uint8_t* fixedOut = nullptr) {

		in = in_;
		inLen = inLen_;
		inIdx = 0;
		mode = mode_;

		if (chunkSize == 0 && fixedOut == nullptr) {
			// Estimate outLen at 2.5x inLen. Expansion rates for values:
			// ObjectId: 12B -> 24B plus 2 for quotes
			// String: 5 for header + 1 per char -> 1 or 2 per char + 2 for quotes
			// Int: 1+4 -> up to 11
			// Long: 1+8 -> up to 20
			// Number: 1+8 -> up to ???
			// Date: 1+8 -> 24 plus 2 for quotes
			// Boolean: 1+1 -> 4 or 5
			// Null: 1+0 -> 4
			// The maximum expansion ratio is 1:5 (for null), but averages ~2.3x
			// for mixed data or ~1x for string-heavy data.
			chunkSize = (inLen * 10) >> 2;
		}

		if (fixedOut == nullptr) {
			resize(chunkSize);
		} else {
			out = fixedOut;
			outLen = chunkSize;
		}

		if (mode == Mode::PAUSE) {
			std::unique_lock<std::mutex> lk(m);
			outIdx = outLen + 1;
			cv.wait(lk, [&] { return outIdx == 0; });
		}

		transcodeObject(isArray);

		if (mode == Mode::PAUSE)
			cv.notify_one();
	}

	void setOutIdx(size_t to) {
		std::unique_lock<std::mutex> lk(m);
		outIdx = to;
	}

	void resume() {
		cv.notify_one();
		std::unique_lock<std::mutex> lk(m);
		cv.wait(lk, [&] { return outIdx > 0 || isDone(); });
	}

	bool isDone() {
		//std::cout << "remaining: " << (inLen - inIdx) << std::endl;
		return inIdx == inLen;
	}

	void destroy() {
		std::free(out);
		out = nullptr;
		outIdx = 0;
	}

private:
	const uint8_t* in = nullptr;
	size_t inIdx = 0;
	size_t inLen = 0;
	Mode mode;
	std::mutex m;
	std::condition_variable cv;

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

	void resize(size_t to) {
		// printf("resizing from %d to %d\n", outLen, to);
		uint8_t* oldOut = out;
		out = static_cast<uint8_t*>(std::realloc(out, to));
		if (out == nullptr) {
			std::free(oldOut);
			throw std::runtime_error("Failed to reallocate output.");
		}
		printf("allocated %p\n", out);
		outLen = to;
	}

	void ensureSpace(size_t n) {
		if (outIdx + n < outLen) [[likely]] {
			return;
		}

		if (mode == Mode::REALLOC) {
			resize((outLen * 3) >> 1);
		} else {
			cv.notify_one();
			std::unique_lock<std::mutex> lk(m);
			cv.wait(lk, [&] { return outIdx == 0; });
		}
	}

	// Writes n characters from in to out, escaping per ECMA-262 sec 24.5.2.2.
	// Portable/scalar
	void writeEscapedCharsBaseline(size_t n) {
		const size_t end = inIdx + n;
		// TODO the inner ensureSpace can be skipped when ensureSpace(n * 6) is
		// true (worst-case expansion is 6x).
		ensureSpace(n);
		while (inIdx < end) {
			uint8_t xc;
			const uint8_t c = in[inIdx++];
			if (c >= 0x20 && c != 0x22 && c != 0x5c) {
				out[outIdx++] = c;
			} else if ((xc = getEscape(c))) { // single char escape
				ensureSpace(end - inIdx + 1);
				out[outIdx++] = '\\';
				out[outIdx++] = xc;
			} else { // c < 0x20, control
				ensureSpace(end - inIdx + 5);
				out[outIdx++] = '\\';
				out[outIdx++] = 'u';
				out[outIdx++] = '0';
				out[outIdx++] = '0';
				out[outIdx++] = (c & 0xf0) ? '1' : '0';
				out[outIdx++] = 'h'; // TODO
			}
		}
	}
	
	// SSE4.2
	void writeEscapedCharsSSE42(size_t n) {
		const size_t end = inIdx + n;
		ensureSpace(n);

		const __m128i escapes = _mm_set_epi8(0, 0, 0, 0, 0, 0, 0, 0, '\b', '\t', '\n', '\f', '\r', '"', '/', '\\');

		while (inIdx < end) {
			const int clampedN = n > 16 ? 16 : n;
			__m128i chars = _mm_loadu_si128(reinterpret_cast<__m128i const*>(&in[inIdx])); // TODO this can overrun
			int esRIdx = _mm_cmpestri(escapes, 8, chars, n,
				_SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ANY | _SIDD_POSITIVE_POLARITY | _SIDD_LEAST_SIGNIFICANT);

			if (esRIdx == 16) {
				// No chars need escaping.
				esRIdx = clampedN;
			}

			_mm_storeu_si128(reinterpret_cast<__m128i*>(&out[outIdx]), chars); // TODO this overruns (okay except at end)
			//memcpy(&out[outIdx], &in[inIdx], esRIdx);
			n -= esRIdx;
			outIdx += esRIdx;
			inIdx += esRIdx;

			if (esRIdx < clampedN) {
				ensureSpace(end - inIdx + 1);
				out[outIdx++] = '\\';
				out[outIdx++] = getEscape(in[inIdx++]);
				n--;
			}
		}
	}

	// AVX2
	void writeEscapedChars(size_t n) {
		const size_t end = inIdx + n;
		ensureSpace(n);

		// x == 8 || x == 9 || x == 10 || x == 12 || x == 13 || x == 34 || x == 47 || x == 92
		// (x > 7 && 14 > x &! x == 11) || x == 34 || x == 47 || x == 92
		__m256i esc7 = _mm256_set1_epi8(7);
		__m256i esc11 = _mm256_set1_epi8(11);
		__m256i esc14 = _mm256_set1_epi8(14);
		__m256i esc34 = _mm256_set1_epi8('"');
		__m256i esc47 = _mm256_set1_epi8('/');
		__m256i esc92 = _mm256_set1_epi8('\\');

		while (inIdx < end) {
			const size_t clampedN = n > 32 ? 32 : n;
			__m256i chars = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(&in[inIdx])); // TODO this can overrun (okay except at end)

			__m256i iseq = _mm256_cmpgt_epi8(chars, esc7);
			iseq = _mm256_and_si256(iseq, _mm256_cmpgt_epi8(esc14, chars));
			iseq = _mm256_andnot_si256(_mm256_cmpeq_epi8(chars, esc11), iseq);
			iseq = _mm256_or_si256(iseq, _mm256_cmpeq_epi8(chars, esc34));
			iseq = _mm256_or_si256(iseq, _mm256_cmpeq_epi8(chars, esc47));
			iseq = _mm256_or_si256(iseq, _mm256_cmpeq_epi8(chars, esc92));

			uint32_t mask = _mm256_movemask_epi8(iseq);
			uint32_t esRIdx = _tzcnt_u32(mask);

			if (esRIdx > clampedN) {
				// No chars need escaping.
				esRIdx = clampedN;
			}

			_mm256_storeu_si256(reinterpret_cast<__m256i*>(&out[outIdx]), chars); // TODO this overruns (okay except at end)
			//memcpy(&out[outIdx], &in[inIdx], esRIdx);
			n -= esRIdx;
			outIdx += esRIdx;
			inIdx += esRIdx;

			if (esRIdx < clampedN) {
				ensureSpace(end - inIdx + 1);
				out[outIdx++] = '\\';
				out[outIdx++] = getEscape(in[inIdx++]);
				n--;
			}
		}
	}

	// Writes the null-terminated string from in to out, escaping per JSON spec.
	// SSE4.2
	void writeEscapedChars() {
		const __m128i escapes = _mm_set_epi8(0, 0, 0, 0, 0xff,93, 91,48, 46,35, 33,14, 11,11, 7,1);

		while (inIdx < inLen) {
			__m128i chars = _mm_loadu_si128(reinterpret_cast<__m128i const*>(&in[inIdx])); // TODO this can overrun
			int esRIdx = _mm_cmpistri(escapes, chars,
				_SIDD_UBYTE_OPS | _SIDD_CMP_RANGES | _SIDD_NEGATIVE_POLARITY | _SIDD_LEAST_SIGNIFICANT);

			ensureSpace(esRIdx);
			_mm_storeu_si128(reinterpret_cast<__m128i*>(&out[outIdx]), chars); // TODO this overruns (okay except at end)
			//memcpy(&out[outIdx], &in[inIdx], esRIdx);
			outIdx += esRIdx;
			inIdx += esRIdx;

			if (esRIdx < 16) {
				if (in[inIdx] == 0) {
					return;
				} else {
					out[outIdx++] = '\\';
					out[outIdx++] = getEscape(in[inIdx++]);
				}
			}
		}
	}

	// AVX2 - For our use case with usually short keys, this is slightly slower than SSE4.2 ver
	void writeEscapedCharsAVX2() {
		// x == 8 || x == 9 || x == 10 || x == 12 || x == 13 || x == 34 || x == 47 || x == 92 || [x == 0]
		// (x > 7 && 14 > x &! x == 11) || x == 34 || x == 47 || x == 92 || [x == 0]
		__m256i esc7 = _mm256_set1_epi8(7);
		__m256i esc11 = _mm256_set1_epi8(11);
		__m256i esc14 = _mm256_set1_epi8(14);
		__m256i esc34 = _mm256_set1_epi8('"');
		__m256i esc47 = _mm256_set1_epi8('/');
		__m256i esc92 = _mm256_set1_epi8('\\');
		__m256i nul0 = _mm256_setzero_si256();

		while (inIdx < inLen) {
			__m256i chars = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(&in[inIdx])); // TODO this can overrun

			__m256i iseq = _mm256_cmpgt_epi8(chars, esc7);
			iseq = _mm256_and_si256(iseq, _mm256_cmpgt_epi8(esc14, chars));
			iseq = _mm256_andnot_si256(_mm256_cmpeq_epi8(chars, esc11), iseq);
			iseq = _mm256_or_si256(iseq, _mm256_cmpeq_epi8(chars, esc34));
			iseq = _mm256_or_si256(iseq, _mm256_cmpeq_epi8(chars, esc47));
			iseq = _mm256_or_si256(iseq, _mm256_cmpeq_epi8(chars, esc92));
			iseq = _mm256_or_si256(iseq, _mm256_cmpeq_epi8(chars, nul0));

			uint32_t mask = _mm256_movemask_epi8(iseq);
			uint32_t esRIdx = _tzcnt_u32(mask); // position of 0 *or* a char that needs to be escaped

			ensureSpace(esRIdx);
			_mm256_storeu_si256(reinterpret_cast<__m256i*>(&out[outIdx]), chars); // TODO this overruns (okay except at end)
			//memcpy(&out[outIdx], &in[inIdx], esRIdx);
			outIdx += esRIdx;
			inIdx += esRIdx;

			if (esRIdx < 32) {
				if (in[inIdx] == 0) {
					return;
				} else {
					out[outIdx++] = '\\';
					out[outIdx++] = getEscape(in[inIdx++]);
				}
			}
		}
	}

	void transcodeObject(bool isArray) {
		const int32_t size = readInt32LE();
		if (size < 5 || size > inLen) // + inIdx?
			throw std::runtime_error("BSON size exceeds input length.");

		bool first = true;

		ensureSpace(1);
		out[outIdx++] = isArray ? '[' : '{';

		while (true) {
			const uint8_t elementType = in[inIdx++];
			if (elementType == 0) break;

			if (first) {
				first = false;
			} else {
				ensureSpace(1);
				out[outIdx++] = ',';
			}

			{ // Write name
				if (!isArray) {
					ensureSpace(1);
					out[outIdx++] = '"';
					writeEscapedChars();
					inIdx++; // skip null terminator
					ensureSpace(2);
					out[outIdx++] = '"';
					out[outIdx++] = ':';
				} else {
					size_t strsz = inLen - inIdx;
					const size_t namelen = strnlen(reinterpret_cast<const char*>(in + inIdx), strsz);
					if (namelen == strsz)
						throw std::runtime_error("Name terminator not found");
					inIdx += namelen + 1;
				}
			}

			switch (elementType) {
			case BSON_DATA_STRING: {
				const int32_t size = readInt32LE();
				if (size <= 0 || size > inLen - inIdx)
					throw std::runtime_error("Bad string length");
				ensureSpace(1);
				out[outIdx++] = '"';
				writeEscapedChars(size - 1);
				inIdx++; // skip null terminator
				ensureSpace(1);
				out[outIdx++] = '"';
				break;
			}
			case BSON_DATA_OID: {
				ensureSpace(26);
				// TODO mask load is 4,0.5, load is 2,0.5. Overrun the load where possible:
				// if (not at end of buffer) {
				//	 __m128i a = _mm_loadu_si128(reinterpret_cast<__m128i const*>(in + inIdx)); // 12B = 3x4B
				// } else {
				__m128i loadmask = _mm_set_epi32(0, 0x80000000, 0x80000000, 0x80000000); // 12B = 3x4B
				__m128i a = _mm_maskload_epi32(reinterpret_cast<int const*>(in + inIdx), loadmask);
				// }
				inIdx += 12;
				__m256i b = encodeHex(a);
				out[outIdx++] = '"';
				// TODO mask store is 14,1, store is 3,1. Overrun the store where possible:
				// if (not at end of buffer) {
				//   _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + outIdx), b);
				// } else {
				// Else use a 16B and an 8B
				__m256i storemask = _mm256_set_epi32(0, 0, 0x80000000, 0x80000000,
					0x80000000, 0x80000000, 0x80000000, 0x80000000); // 24B = 6x4B
				_mm256_maskstore_epi32(reinterpret_cast<int*>(out + outIdx), storemask, b);
				// }
				outIdx += 24;
				out[outIdx++] = '"';
				break;
			}
			case BSON_DATA_INT: {
				ensureSpace(11); // TODO use exact sizing in fast_itoa
				const int32_t value = readInt32LE();
				outIdx += fast_itoa(out + outIdx, value);
				break;
			}
			case BSON_DATA_NUMBER: {
				const double value = readDoubleLE();
				if (std::isfinite(value)) {
					ensureSpace(20); // TODO
					const int n = sprintf(reinterpret_cast<char*>(out + outIdx), "%.*f", DBL_DIG, value);
					outIdx += n;
				} else {
					ensureSpace(4);
					out[outIdx++] = 'n';
					out[outIdx++] = 'u';
					out[outIdx++] = 'l';
					out[outIdx++] = 'l';
				}
				break;
			}
			case BSON_DATA_DATE: {
				ensureSpace(26);
				const int64_t value = readInt64LE(); // BSON encodes UTC ms since Unix epoch
				const time_t seconds = value / 1000;
				const int32_t millis = value % 1000;
				out[outIdx++] = '"';
				size_t n = strftime(reinterpret_cast<char*>(out + outIdx), 24, "%FT%T", gmtime(&seconds));
				outIdx += n;
				n = sprintf(reinterpret_cast<char*>(out + outIdx), ".%03dZ", millis);
				outIdx += n;
				out[outIdx++] = '"';
				break;
			}
			case BSON_DATA_BOOLEAN: {
				const uint8_t val = in[inIdx++];
				if (val == 1) {
					ensureSpace(4);
					out[outIdx++] = 't';
					out[outIdx++] = 'r';
					out[outIdx++] = 'u';
					out[outIdx++] = 'e';
				} else {
					ensureSpace(5);
					out[outIdx++] = 'f';
					out[outIdx++] = 'a';
					out[outIdx++] = 'l';
					out[outIdx++] = 's';
					out[outIdx++] = 'e';
				}
				break;
			}
			case BSON_DATA_OBJECT: {
				transcodeObject(false);
				break;
			}
			case BSON_DATA_ARRAY: {
				const int32_t size = readInt32LE(false);
				transcodeObject(true);
				if (in[inIdx - 1] != 0)
					throw std::runtime_error("Invalid array terminator byte");
				break;
			}
			case BSON_DATA_NULL: {
				ensureSpace(4);
				out[outIdx++] = 'n';
				out[outIdx++] = 'u';
				out[outIdx++] = 'l';
				out[outIdx++] = 'l';
				break;
			}
			case BSON_DATA_LONG: {
				ensureSpace(20); // TODO use exact sizing in fast_itoa
				const int64_t value = readInt64LE();
				outIdx += fast_itoa(out + outIdx, value);
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
				// incompatible JSON type - TODO still need to at least skip these parts of the doc
				break;
			default:
				throw std::runtime_error("Unknown BSON type");
			}
		}

		ensureSpace(1);
		out[outIdx++] = isArray ? ']' : '}';
	}
};

Napi::FunctionReference constructor;

class BJTrans : public Napi::ObjectWrap<BJTrans> {
public:

	static Napi::Object Init(Napi::Env env, Napi::Object exports) {
		Napi::HandleScope scope(env);
		Napi::Function func = DefineClass(env, "BJTrans", {
			InstanceMethod(Napi::Symbol::WellKnown(env, "iterator"), &BJTrans::Iterator)
			});
		
		constructor = Napi::Persistent(func);
		constructor.SuppressDestruct();

		exports.Set("BJTrans", func);
		return exports;
	}

	BJTrans(const Napi::CallbackInfo& info) : Napi::ObjectWrap<BJTrans>(info) {
		Napi::Env env = info.Env();
		Napi::HandleScope scope(env);
		Napi::Uint8Array arr = info[0].As<Napi::Uint8Array>();
		bool isArray = info[1].ToBoolean().Value();
		uint8_t* out = nullptr;

		if (info.Length() >= 2 && info[2].IsObject()) {
			Napi::Object options = info[2].ToObject();

			if (options.Has("chunkSize")) {
				Napi::Value csOpt = options.Get("chunkSize");
				if (csOpt.IsNumber()) {
					chunkSize = csOpt.As<Napi::Number>().Uint32Value();
				} else {
					Napi::TypeError::New(env, "chunkSize must be a number").ThrowAsJavaScriptException();
				}
			}

			if (options.Has("fixedBuffer")) {
				auto val = options.Get("fixedBuffer");
				if (val.IsArrayBuffer()) {
					auto ab = options.Get("fixedBuffer").As<Napi::ArrayBuffer>();
					chunkSize = ab.ByteLength();
					out = static_cast<uint8_t*>(ab.Data());
					fixedBuffer = true;
				} else {
					Napi::TypeError::New(env, "fixedBuffer must be an ArrayBuffer").ThrowAsJavaScriptException();
				}
			}
		}

		worker = std::thread { &Transcoder::transcode, &trans, arr.Data(), arr.ByteLength(), isArray, chunkSize, Transcoder::Mode::PAUSE, out };
	}

private:
	Transcoder trans;
	std::thread worker;
	bool fixedBuffer = false;
	size_t chunkSize = 0;
	bool final = false;

	Napi::Value BJTrans::Iterator(const Napi::CallbackInfo& info) {
		Napi::Env env = info.Env();

		Napi::Object iter = Napi::Object::New(env);

		iter.Set("next", Napi::Function::New(env, [&](const Napi::CallbackInfo& info) -> Napi::Object {
			Napi::Env env = info.Env();

			Napi::Object rv = Napi::Object::New(env);

			if (final) {
				rv.Set("done", true);
				worker.join();
			} else {
				trans.setOutIdx(0);
				trans.resume();

				if (trans.isDone())
					final = true;

				Napi::Buffer<uint8_t> buf;
				if (fixedBuffer) {
					buf = Napi::Buffer<uint8_t>::New(env, trans.out, trans.outIdx, [](Napi::Env, uint8_t* data) {});
				} else {
					buf = Napi::Buffer<uint8_t>::Copy(env, trans.out, trans.outIdx);
				}
				rv.Set("value", buf);
				rv.Set("done", false);
			}

			return rv;
		}, "next"));

		return iter;
	}
};

Napi::Buffer<uint8_t> bsonToJson(const Napi::CallbackInfo& info) {
	Napi::Env env = info.Env();

	Napi::Uint8Array arr = info[0].As<Napi::Uint8Array>();
	bool isArray = info[1].ToBoolean().Value();

	Transcoder trans;
	trans.transcode(arr.Data(), arr.ByteLength(), isArray);

	Napi::Buffer<uint8_t> buf = Napi::Buffer<uint8_t>::New(env, trans.out, trans.outIdx, [](Napi::Env, uint8_t* data) {
		printf("freeing %p\n", data);
		std::free(data);
	});

	return buf;
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
	exports.Set(Napi::String::New(env, "bsonToJson"), Napi::Function::New(env, bsonToJson));
	BJTrans::Init(env, exports);

	return exports;
}

NODE_API_MODULE(NODE_GYP_MODULE_NAME, Init)
