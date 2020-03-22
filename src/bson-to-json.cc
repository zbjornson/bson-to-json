#include <cstdint>
#include <cstdlib>
#include <cstring> // memcpy
#include <ctime> // gmtime
#include <cmath> // isfinite
#include <thread>
#include <mutex>
#include <condition_variable>
#include "napi.h"
#include "../deps/double_conversion/double-to-string.h"
#include "cpu-detection.h"
#include "fast_itoa.h"

#ifdef _MSC_VER
# include <intrin.h>
#elif defined(__GNUC__)
# include <x86intrin.h>
#endif

#if defined(__AVX512F__) && defined(__GNUC__) && defined(B2J_USE_AVX512)
# if __GNUC__ < 8 || (__GNUC__ == 8 && __GNUC_MINOR__ < 2)
// https://godbolt.org/z/qNqVY5 uses movzx dst, r8 on the k reg
#  error "GCC < 8.2 has a bug in mask register handling. Please use a newer version."
# endif
#endif

using namespace double_conversion;

using std::size_t;
using std::uint8_t;
using std::int32_t;
using std::uint32_t;
using std::int64_t;
using std::memcpy;

#ifdef _MSC_VER
# define NOINLINE(fn) __declspec(noinline) fn
// MSVC does not support anything like likely/unlikely, so if/else statements
// need to be ordered by probability. They are planning to add [[likely]] and
// [[unlikely]] though.
// Maybe define these as `LIKELY(expr) (expr) [[likely]]` so upgrading is easier
# define LIKELY(expr) (expr)
# define UNLIKELY(expr) (expr)
#else
# define NOINLINE(fn) fn __attribute__((noinline))
// Only GCC10 supports C++20 [[likely]] and [[unlikely]]
# define LIKELY(expr) __builtin_expect((expr), 1)
# define UNLIKELY(expr) __builtin_expect((expr), 0)
#endif

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

// Returns the char to use to escape c if it requires escaping, else returns 0.
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

constexpr const char HEX_DIGITS[] = "0123456789abcdef";

inline static constexpr uint8_t hexNib(uint8_t nib) {
	// These appear equally fast.
	return HEX_DIGITS[nib];
	// return nib + (nib < 10 ? 48 : 87);
}

// Returns the number of base-10 digits in a null-terminated string
// representation of v. Assumes relatively small arrays. See
// https://stackoverflow.com/a/1489873/1218408 for a version that might be
// better for medium and large arrays.
inline static constexpr size_t nDigits(int32_t v) {
	if (v < 10) return 2;
	if (v < 100) return 3;
	if (v < 1'000) return 4;
	if (v < 10'000) return 5;
	if (v < 100'000) return 6;
	if (v < 1'000'000) return 7;
	if (v < 10'000'000) return 8;
	if (v < 100'000'000) return 9;
	if (v < 1'000'000'000) return 10;
	return 11;
}

// Except for MSVC and maybe some C++14-compliant compilers, class member fns
// can't be specialized, which I want to do for ISA specialization. This is sort
// of a hack that uses overload resolution instead, based on:
// https://stackoverflow.com/a/38283990/1218408
template<int isa> struct Enabler : Enabler<isa - 1> {};
template<> struct Enabler<0> {};

#define ENSURE_SPACE_OR_RETURN(n) if (UNLIKELY(ensureSpace<mode>(n))) return true
#define RETURN_ERR(msg) return err = (msg), true

[[gnu::target("sse2")]]
inline static __m128i _mm_set1_epu8(uint8_t v) {
	union {
		uint8_t u;
		int8_t i;
	} val;
	val.u = v;
	return _mm_set1_epi8(val.i);
}

[[gnu::target("avx")]]
inline static __m256i _mm256_set1_epu8(uint8_t v) {
	union {
		uint8_t u;
		int8_t i;
	} val;
	val.u = v;
	return _mm256_set1_epi8(val.i);
}

[[gnu::target("avx512f")]]
inline static __m512i _mm512_set1_epu8(uint8_t v) {
	union {
		uint8_t u;
		int8_t i;
	} val;
	val.u = v;
	return _mm512_set1_epi8(val.i);
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
	char const* err = nullptr;

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
	template <ISA isa, Mode mode>
	bool transcode(const uint8_t* in_, size_t inLen_, bool isArray = true,
			size_t chunkSize = 0, uint8_t* fixedOut = nullptr) {

		if (UNLIKELY(inLen_ < 5))
			RETURN_ERR("Input buffer must have length >= 5");

		in = in_;
		inLen = inLen_;
		inIdx = 0;

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

		if (transcodeObject<isa, mode>(isArray))
			return true;

		if (mode == Mode::PAUSE)
			cv.notify_one();
		
		return false;
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
	std::mutex m;
	std::condition_variable cv;

	template<typename T>
	inline T readLE() {
		T v;
		memcpy(&v, in + inIdx, sizeof(T));
		inIdx += sizeof(T);
		return v;
	}

	bool resize(size_t to) {
		// printf("resizing from %d to %d\n", outLen, to);
		uint8_t* oldOut = out;
		out = static_cast<uint8_t*>(std::realloc(out, to));
		if (out == nullptr) {
			std::free(oldOut);
			err = "Allocation failure";
			return true;
		}
		// printf("allocated %p\n", out);
		outLen = to;
		return false;
	}

	template<Mode mode>
	[[nodiscard]]
	inline bool ensureSpace(size_t n) {
		if (LIKELY(outIdx + n < outLen)) {
			return false;
		}

		if (mode == Mode::REALLOC) {
			// TODO assert(outIdx + n < (outLen * 3) >> 1)
			if (resize((outLen * 3) >> 1))
				return true;
		} else {
			cv.notify_one();
			std::unique_lock<std::mutex> lk(m);
			cv.wait(lk, [&] { return outIdx == 0; });
		}
		return false;
	}

	// The load/store methods must be small and inlinable in the fast case (not
	// at end of in or out, which is almost always). The slow case should be
	// reached with a `call` and there's not much point to optimize it.

	[[gnu::target("sse2")]]
	NOINLINE(__m128i load_partial_128i_slow(size_t n)) {
		// TODO compare against a right-aligned load + shuffle when possible.
		// TODO compare against AVX512VL+BW _mm_mask_loadu_epi8.
		uint8_t x[16];
		for (size_t i = 0; i < n; i++) x[i] = in[inIdx + i];
		return _mm_loadu_si128(reinterpret_cast<__m128i*>(x));
	}

	// Safely loads n bytes. The values in xmm beyond n are undefined.
	[[gnu::target("sse2")]]
	inline __m128i load_partial_128i(size_t n) {
		// Do a full load if we're 16B from the end of the input.
		// Other acceptable criteria:
		// n == 16
		// (reinterpret_cast<intptr_t>(in + inIdx) & 0xFFF) < 0xFF0 (16B from page boundary)
		if (LIKELY(n + inIdx < inLen)) {
			return _mm_loadu_si128(reinterpret_cast<__m128i const*>(&in[inIdx]));
		}

		return load_partial_128i_slow(n);
	}

	[[gnu::target("avx2")]]
	NOINLINE(__m256i load_partial_256i_slow(size_t n)) {
		if (n <= 16)
			return _mm256_castsi128_si256(load_partial_128i(n));

		__m128i lo = _mm_loadu_si128(reinterpret_cast<__m128i const*>(in + inIdx));;
		inIdx += 16;
		__m128i hi = load_partial_128i(n - 16);
		inIdx -= 16;
		return _mm256_inserti128_si256(_mm256_castsi128_si256(lo), hi, 1);
	}

	// Safely loads n bytes. The values in ymm beyond n are undefined.
	[[gnu::target("avx2")]]
	inline __m256i load_partial_256i(size_t n) {
		// debug assert n != 0 && n <= 32
		
		// Other acceptable criteria:
		// n == 32
		// (reinterpret_cast<intptr_t>(in + inIdx) & 0xFFF) < 0xFE0 (32B from page boundary)
		if (LIKELY(n + inIdx < inLen)) {
			return _mm256_loadu_si256(reinterpret_cast<__m256i const*>(&in[inIdx]));
		}

		return load_partial_256i_slow(n);
	}

	[[gnu::target("avx512f,avx512bw,bmi2")]]
	inline __m512i load_partial_512i(size_t n) {
		// if (LIKELY(n + inIdx < inLen)) {
		// 	return _mm512_loadu_si512(&in[inIdx]);
		// }

		__mmask64 mask = _bzhi_u64(-1, n); // TODO n needs to clamp at inLen
		return _mm512_maskz_loadu_epi8(mask, &in[inIdx]);
	}

	[[gnu::target("sse2")]]
	NOINLINE(void store_partial_128i_slow(__m128i v, size_t n)) {
		// maskmovdqu is implicitly NT.
		// Does AVX512BW have fast byte-granular store?
		// pblendvb for load+blend+store requires SSE4.1 and this has to work with SSE2.
		// TODO Try _mm_storeu_si128(temp, v), memcpy(out + outIdx, temp, n)?
		union {
			int8_t  i8[16];
			int16_t i16[8];
			int32_t i32[4];
			int64_t i64[2];
		} u;
		_mm_storeu_si128(reinterpret_cast<__m128i*>(u.i8), v);
		int j = 0;
		if (n & 8) {
			*reinterpret_cast<int64_t*>(out + outIdx) = u.i64[0];
			j += 8;
		}
		if (n & 4) {
			reinterpret_cast<int32_t*>(out + outIdx)[j >> 2] = u.i32[j >> 2];
			j += 4;
		}
		if (n & 2) {
			reinterpret_cast<int16_t*>(out + outIdx)[j >> 1] = u.i16[j >> 1];
			j += 2;
		}
		if (n & 1) {
			out[outIdx + j] = u.i8[j];
		}
	}

	// Safely stores n bytes. May write more than n bytes.
	[[gnu::target("sse2")]]
	inline void store_partial_128i(__m128i v, size_t n) {
		if (LIKELY(n + outIdx < outLen)) { // TODO try with (n == 16)
			return _mm_storeu_si128(reinterpret_cast<__m128i*>(out + outIdx), v);
		}
		store_partial_128i_slow(v, n);
	}

	[[gnu::target("avx2")]]
	NOINLINE(void store_partial_256i_slow(__m256i v, size_t n)) {
		store_partial_128i(_mm256_castsi256_si128(v), n);
		if (n > 16)
			store_partial_128i(_mm256_extracti128_si256(v, 1), n - 16);
	}

	// Safely stores n bytes. May write more than n bytes.
	[[gnu::target("avx2")]]
	inline void store_partial_256i(__m256i v, size_t n) {
		if (LIKELY(n + outIdx < outLen)) { // TODO try with (n == 32)
			return _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + outIdx), v);
		}
		store_partial_256i_slow(v, n);
	}

	// Safely stores n bytes. May write more than n bytes.
	[[gnu::target("avx512f,avx512bw,bmi2")]]
	inline void store_partial_512i(__m512i v, size_t n) {
		__mmask64 mask = _bzhi_u64(-1, n); // TODO n needs to clamp at outLen
		_mm512_mask_storeu_epi8(out + outIdx, mask, v);
	}

	// Writes the `\ u 0 0 ch cl` sequence
	inline void writeControlChar(uint8_t c) {
		memcpy(out + outIdx, "\\u00", 4);
		outIdx += 4;
		out[outIdx++] = (c & 0xf0) ? '1' : '0';
		out[outIdx++] = hexNib(c & 0xf);
	}

	// Writes n characters from in to out, escaping per ECMA-262 sec 24.5.2.2.
	template<Mode mode>
	bool writeEscapedChars(size_t n, Enabler<ISA::BASELINE>) {
		const size_t end = inIdx + n;
		// TODO the inner ensureSpace can be skipped when ensureSpace(n * 6) is
		// true (worst-case expansion is 6x).
		ENSURE_SPACE_OR_RETURN(n);
		while (inIdx < end) {
			uint8_t xc;
			const uint8_t c = in[inIdx++];
			if (LIKELY(c >= 0x20 && c != 0x22 && c != 0x5c)) {
				out[outIdx++] = c;
			} else if ((xc = getEscape(c))) { // single char escape
				ENSURE_SPACE_OR_RETURN(end - inIdx + 1);
				out[outIdx++] = '\\';
				out[outIdx++] = xc;
			} else { // c < 0x20, control
				ENSURE_SPACE_OR_RETURN(end - inIdx + 5);
				writeControlChar(c);
			}
		}
		return false;
	}

	template<Mode mode>
	[[gnu::target("sse2,bmi")]]
	bool writeEscapedChars(size_t n, Enabler<ISA::SSE2>) {
		const size_t end = inIdx + n;
		ENSURE_SPACE_OR_RETURN(n);

		// escape if (x < 0x20 || x == 0x22 || x == 0x5c)

		// xor 0x80 to get unsigned comparison. https://stackoverflow.com/q/32945410/1218408
		__m128i esch20 = _mm_set1_epu8(0x20 ^ 0x80);
		__m128i esch22 = _mm_set1_epu8(0x22);
		__m128i esch5c = _mm_set1_epu8(0x5c);

		while (inIdx < end) {
			const size_t clampedN = n > 16 ? 16 : n;
			__m128i chars = load_partial_128i(clampedN);

			// see above (unsigned comparison)
			__m128i iseq = _mm_cmpgt_epi8(esch20, _mm_xor_si128(chars, _mm_set1_epu8(0x80)));
			iseq = _mm_or_si128(iseq, _mm_cmpeq_epi8(chars, esch22));
			iseq = _mm_or_si128(iseq, _mm_cmpeq_epi8(chars, esch5c));

			uint32_t mask = _mm_movemask_epi8(iseq);
			uint32_t esRIdx = _tzcnt_u32(mask);

			if (esRIdx > clampedN) {
				// No chars need escaping.
				esRIdx = clampedN;
			}

			store_partial_128i(chars, esRIdx);
			n -= esRIdx;
			outIdx += esRIdx;
			inIdx += esRIdx;

			if (esRIdx < clampedN) {
				uint8_t xc;
				uint8_t c = in[inIdx++];
				n--;
				if ((xc = getEscape(c))) { // single char escape
					ENSURE_SPACE_OR_RETURN(end - inIdx + 1);
					out[outIdx++] = '\\';
					out[outIdx++] = xc;
				} else { // c < 0x20, control
					ENSURE_SPACE_OR_RETURN(end - inIdx + 5);
					writeControlChar(c);
				}
			}
		}
		return false;
	}

	template<Mode mode>
	[[gnu::target("sse4.2")]]
	bool writeEscapedChars(size_t n, Enabler<ISA::SSE42>) {
		const size_t end = inIdx + n;
		ENSURE_SPACE_OR_RETURN(n);

		const __m128i escapes = _mm_set_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x5c,0x5c, 0x22,0x22, 0x1f,0);

		while (inIdx < end) {
			const int clampedN = n > 16 ? 16 : n;
			__m128i chars = load_partial_128i(clampedN);
			int esRIdx = _mm_cmpestri(escapes, 6, chars, n,
				_SIDD_UBYTE_OPS | _SIDD_CMP_RANGES | _SIDD_POSITIVE_POLARITY | _SIDD_LEAST_SIGNIFICANT);

			if (esRIdx == 16) // No chars need escaping.
				esRIdx = clampedN;

			store_partial_128i(chars, esRIdx);
			n -= esRIdx;
			outIdx += esRIdx;
			inIdx += esRIdx;

			if (esRIdx < clampedN) {
				uint8_t xc;
				uint8_t c = in[inIdx++];
				n--;
				if ((xc = getEscape(c))) { // single char escape
					ENSURE_SPACE_OR_RETURN(end - inIdx + 1);
					out[outIdx++] = '\\';
					out[outIdx++] = xc;
				} else { // c < 0x20, control
					ENSURE_SPACE_OR_RETURN(end - inIdx + 5);
					writeControlChar(c);
				}
			}
		}
		return false;
	}

	template<Mode mode>
	[[gnu::target("avx2,bmi")]]
	bool writeEscapedChars(size_t n, Enabler<ISA::AVX2>) {
		const size_t end = inIdx + n;
		ENSURE_SPACE_OR_RETURN(n);

		// escape if (x < 0x20 || x == 0x22 || x == 0x5c)

		// xor 0x80 to get unsigned comparison. https://stackoverflow.com/q/32945410/1218408
		__m256i esch20 = _mm256_set1_epu8(0x20 ^ 0x80);
		__m256i esch22 = _mm256_set1_epu8(0x22);
		__m256i esch5c = _mm256_set1_epu8(0x5c);

		while (inIdx < end) {
			const size_t clampedN = n > 32 ? 32 : n;
			__m256i chars = load_partial_256i(clampedN);

			// see above (unsigned comparison)
			__m256i iseq = _mm256_cmpgt_epi8(esch20, _mm256_xor_si256(chars, _mm256_set1_epu8(0x80)));
			iseq = _mm256_or_si256(iseq, _mm256_cmpeq_epi8(chars, esch22));
			iseq = _mm256_or_si256(iseq, _mm256_cmpeq_epi8(chars, esch5c));

			uint32_t mask = _mm256_movemask_epi8(iseq);
			uint32_t esRIdx = _tzcnt_u32(mask);

			if (esRIdx > clampedN) // No chars need escaping.
				esRIdx = clampedN;

			store_partial_256i(chars, esRIdx);
			n -= esRIdx;
			outIdx += esRIdx;
			inIdx += esRIdx;

			if (esRIdx < clampedN) {
				uint8_t xc;
				uint8_t c = in[inIdx++];
				n--;
				if ((xc = getEscape(c))) { // single char escape
					ENSURE_SPACE_OR_RETURN(end - inIdx + 1);
					out[outIdx++] = '\\';
					out[outIdx++] = xc;
				} else { // c < 0x20, control
					ENSURE_SPACE_OR_RETURN(end - inIdx + 5);
					writeControlChar(c);
				}
			}
		}
		return false;
	}

	template<Mode mode>
	[[gnu::target("avx512f,avx512bw,bmi,bmi2")]]
	bool writeEscapedChars(size_t n, Enabler<ISA::AVX512F>) {
		const size_t end = inIdx + n;
		ENSURE_SPACE_OR_RETURN(n);

		// escape if (x < 0x20 || x == 0x22 || x == 0x5c)
		// allow if (x >= 0x20 && x != 0x22 && x != 0x5c)

		__m512i esch20 = _mm512_set1_epu8(0x20);
		__m512i esch22 = _mm512_set1_epu8(0x22);
		__m512i esch5c = _mm512_set1_epu8(0x5c);

		while (inIdx < end) {
			const size_t clampedN = n > 64 ? 64 : n;
			__m512i chars = load_partial_512i(clampedN);

			__mmask64 mask1 = _mm512_cmpge_epu8_mask(chars, esch20);
			mask1 = _mm512_mask_cmpneq_epu8_mask(mask1, chars, esch22);
			mask1 = _mm512_mask_cmpneq_epu8_mask(mask1, chars, esch5c);
			// TODO is it better to use two mask regs and & them later so
			// there's no dependency chain on the cmps?

			uint64_t esRIdx = _tzcnt_u64(~mask1);

			if (esRIdx > clampedN) // No chars need escaping.
				esRIdx = clampedN;

			store_partial_512i(chars, esRIdx);
			n -= esRIdx;
			outIdx += esRIdx;
			inIdx += esRIdx;

			if (esRIdx < clampedN) {
				uint8_t xc;
				uint8_t c = in[inIdx++];
				n--;
				if ((xc = getEscape(c))) { // single char escape
					ENSURE_SPACE_OR_RETURN(end - inIdx + 1);
					out[outIdx++] = '\\';
					out[outIdx++] = xc;
				} else { // c < 0x20, control
					ENSURE_SPACE_OR_RETURN(end - inIdx + 5);
					writeControlChar(c);
				}
			}
		}
		return false;
	}

	// Writes the null-terminated string from in to out, escaping per JSON spec.
	template<Mode mode>
	bool writeEscapedChars(Enabler<ISA::BASELINE>) {
		// TODO the inner ensureSpace can be skipped when ensureSpace(n * 6) is
		// true (worst-case expansion is 6x).
		uint8_t c;
		while ((c = in[inIdx++])) {
			uint8_t xc;
			if (LIKELY(c >= 0x20 && c != 0x22 && c != 0x5c)) {
				ENSURE_SPACE_OR_RETURN(1);
				out[outIdx++] = c;
			} else if ((xc = getEscape(c))) { // single char escape
				ENSURE_SPACE_OR_RETURN(2);
				out[outIdx++] = '\\';
				out[outIdx++] = xc;
			} else { // c < 0x20, control
				ENSURE_SPACE_OR_RETURN(6);
				writeControlChar(c);
			}
		}
		inIdx--;
		return false;
	}

	// TODO SSE2

	template<Mode mode>
	[[gnu::target("sse4.2")]]
	bool writeEscapedChars(Enabler<ISA::SSE42>) {
		// escape if (x < 0x20 || x == 0x22 || x == 0x5c)
		union {
			uint8_t u;
			int8_t i;
		} val;
		val.u = 0xff;
		const __m128i escapes = _mm_set_epi8(0,0, 0,0, 0,0, 0,0, 0,0, val.i,0x5d, 0x5b,0x23, 0x21,0x20);

		while (inIdx < inLen) {
			__m128i chars = _mm_loadu_si128(reinterpret_cast<__m128i const*>(&in[inIdx])); // TODO this can overrun
			int esRIdx = _mm_cmpistri(escapes, chars,
				_SIDD_UBYTE_OPS | _SIDD_CMP_RANGES | _SIDD_NEGATIVE_POLARITY | _SIDD_LEAST_SIGNIFICANT);

			ENSURE_SPACE_OR_RETURN(esRIdx);
			store_partial_128i(chars, esRIdx);
			outIdx += esRIdx;
			inIdx += esRIdx;

			if (esRIdx < 16) {
				if (in[inIdx] == 0)
					return false;

				uint8_t xc;
				uint8_t c = in[inIdx++];
				if ((xc = getEscape(c))) { // single char escape
					ENSURE_SPACE_OR_RETURN(2);
					out[outIdx++] = '\\';
					out[outIdx++] = xc;
				} else { // c < 0x20, control
					ENSURE_SPACE_OR_RETURN(6);
					writeControlChar(c);
				}
			}
		}
		return false;
	}

	template<Mode mode>
	[[gnu::target("avx2,bmi")]]
	bool writeEscapedChars(Enabler<ISA::AVX2>) {
		// escape if (x < 0x20 || x == 0x22 || x == 0x5c)

		// xor 0x80 to get unsigned comparison. https://stackoverflow.com/q/32945410/1218408
		__m256i esch20 = _mm256_set1_epu8(0x20 ^ 0x80);
		__m256i esch22 = _mm256_set1_epu8(0x22);
		__m256i esch5c = _mm256_set1_epu8(0x5c);

		while (inIdx < inLen) {
			__m256i chars = load_partial_256i(32);

			__m256i iseq = _mm256_cmpgt_epi8(esch20, _mm256_xor_si256(chars, _mm256_set1_epu8(0x80)));
			iseq = _mm256_or_si256(iseq, _mm256_cmpeq_epi8(chars, esch22));
			iseq = _mm256_or_si256(iseq, _mm256_cmpeq_epi8(chars, esch5c));

			uint32_t mask = _mm256_movemask_epi8(iseq);
			uint32_t esRIdx = _tzcnt_u32(mask); // position of 0 *or* a char that needs to be escaped

			ENSURE_SPACE_OR_RETURN(esRIdx);
			store_partial_256i(chars, esRIdx);
			outIdx += esRIdx;
			inIdx += esRIdx;

			if (esRIdx < 32) {
				if (in[inIdx] == 0)
					return false;
				
				uint8_t xc;
				uint8_t c = in[inIdx++];
				if ((xc = getEscape(c))) { // single char escape
					ENSURE_SPACE_OR_RETURN(2);
					out[outIdx++] = '\\';
					out[outIdx++] = xc;
				} else { // c < 0x20, control
					ENSURE_SPACE_OR_RETURN(6);
					writeControlChar(c);
				}
			}
		}
		return false;
	}

	template<Mode mode>
	[[gnu::target("avx512f,avx512bw,bmi,bmi2")]]
	bool writeEscapedChars(Enabler<ISA::AVX512F>) {
		__m512i esch20 = _mm512_set1_epu8(0x20);
		__m512i esch22 = _mm512_set1_epu8(0x22);
		__m512i esch5c = _mm512_set1_epu8(0x5c);

		while (inIdx < inLen) {
			__m512i chars = load_partial_512i(64);

			__mmask64 mask1 = _mm512_cmpge_epu8_mask(chars, esch20);
			mask1 = _mm512_mask_cmpneq_epu8_mask(mask1, chars, esch22);
			mask1 = _mm512_mask_cmpneq_epu8_mask(mask1, chars, esch5c);

			uint32_t esRIdx = static_cast<uint32_t>(_tzcnt_u64(~mask1));

			ENSURE_SPACE_OR_RETURN(esRIdx);
			store_partial_512i(chars, esRIdx);
			outIdx += esRIdx;
			inIdx += esRIdx;

			if (esRIdx < 64) {
				if (in[inIdx] == 0)
					return false;
				
				uint8_t xc;
				uint8_t c = in[inIdx++];
				if ((xc = getEscape(c))) { // single char escape
					ENSURE_SPACE_OR_RETURN(2);
					out[outIdx++] = '\\';
					out[outIdx++] = xc;
				} else { // c < 0x20, control
					ENSURE_SPACE_OR_RETURN(6);
					writeControlChar(c);
				}
			}
		}
		return false;
	}

	inline void transcodeObjectId(Enabler<ISA::BASELINE>) {
		out[outIdx++] = '"';
		const size_t end = inIdx + 12;
		while (inIdx < end) {
			uint8_t byte = in[inIdx++];
			out[outIdx++] = hexNib(byte >> 4);
			out[outIdx++] = hexNib(byte & 0xf);
		}
		out[outIdx++] = '"';
	}

	// TODO SSE2: arithmetic (nib + (nib < 10 ? 48 : 87))
	// TODO SSSE3: 128-bit pshufb

	[[gnu::target("avx2")]]
	inline void transcodeObjectId(Enabler<ISA::AVX2>) {
		__m128i a = load_partial_128i(12);
		inIdx += 12;

		// Technique from https://github.com/zbjornson/fast-hex
		const __m256i HEX_LUTR = _mm256_setr_epi8(
			'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
			'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f');
		const __m256i ROT2 = _mm256_setr_epi8(
			-1, 0, -1, 2, -1, 4, -1, 6, -1, 8, -1, 10, -1, 12, -1, 14,
			-1, 0, -1, 2, -1, 4, -1, 6, -1, 8, -1, 10, -1, 12, -1, 14
		);

		// Bytes to nibbles (a -> [a >> 4, a & 0b1111]):

		__m256i doubled = _mm256_cvtepu8_epi16(a);
		// Maybe 1-2 cycles lower latency than _mm256_cvtepu8_epi16 (3L1T).
		// __m256i aa = _mm256_broadcastsi128_si256(a);
		// const __m256i DUP_MASK = _mm256_setr_epi8(
		// 	0,0, 1,1, 2,2, 3,3, 4,4, 5,5, 6,6, 7,7,
		// 	8,8, 9,9, 10,10, 11,11, -1,-1,-1,-1,-1,-1,-1,-1
		// );
		// __m256i doubled = _mm256_shuffle_epi8(aa, DUP_MASK); // (1L1T)

		__m256i hi = _mm256_srli_epi16(doubled, 4);
#ifdef __AVX512VL__
		// This is ~2% faster than shuffle+or. Not worth a dispatch.
		__m256i bytes = _mm256_mask_shuffle_epi8(hi, 0xaaaaaaaa, doubled, ROT2);
#else
		__m256i lo = _mm256_shuffle_epi8(doubled, ROT2);
		__m256i bytes = _mm256_or_si256(hi, lo);
#endif
		bytes = _mm256_and_si256(bytes, _mm256_set1_epi8(0b1111));
		// Encode hex
		__m256i b = _mm256_shuffle_epi8(HEX_LUTR, bytes);

		out[outIdx++] = '"';
		store_partial_256i(b, 24);
		outIdx += 24;
		out[outIdx++] = '"';
	}

	template<ISA isa, Mode mode>
	bool transcodeObject(bool isArray) {
		const int32_t size = readLE<int32_t>();
		if (UNLIKELY(size < 5))
			RETURN_ERR("BSON size must be >= 5");

		if (UNLIKELY(size + inIdx - 4 > inLen))
			RETURN_ERR("BSON size exceeds input length");

		int32_t arrIdx = 0;

		ENSURE_SPACE_OR_RETURN(1);
		out[outIdx++] = isArray ? '[' : '{';

		while (true) {
			const uint8_t elementType = in[inIdx++];
			if (UNLIKELY(elementType == 0))
				break;

			if (LIKELY(arrIdx)) {
				ENSURE_SPACE_OR_RETURN(1);
				out[outIdx++] = ',';
			}

			// Write name
			if (isArray) {
				inIdx += nDigits(arrIdx);
			} else {
				ENSURE_SPACE_OR_RETURN(1);
				out[outIdx++] = '"';
				writeEscapedChars<mode>(Enabler<isa>{});
				inIdx++; // skip null terminator
				ENSURE_SPACE_OR_RETURN(2);
				memcpy(out + outIdx, "\":", 2);
				outIdx += 2;
			}

			switch (elementType) {
			case BSON_DATA_STRING: {
				const int32_t size = readLE<int32_t>();
				if (UNLIKELY(size <= 0 || static_cast<size_t>(size) > inLen - inIdx))
					RETURN_ERR("Bad string length");

				ENSURE_SPACE_OR_RETURN(1);
				out[outIdx++] = '"';
				writeEscapedChars<mode>(size - 1, Enabler<isa>{});
				inIdx++; // skip null terminator
				ENSURE_SPACE_OR_RETURN(1);
				out[outIdx++] = '"';
				break;
			}
			case BSON_DATA_OID: {
				if (LIKELY(inIdx + 12 <= inLen)) {
					ENSURE_SPACE_OR_RETURN(26);
					transcodeObjectId(Enabler<isa>{});
				} else
					RETURN_ERR("Truncated BSON (in ObjectId)");
				break;
			}
			case BSON_DATA_INT: {
				if (LIKELY(inIdx + 4 <= inLen)) {
					const int32_t value = readLE<int32_t>();
					uint8_t temp[INT_BUF_DIGS<int32_t>];
					uint8_t* temp_p = temp;
					size_t n = fast_itoa(temp_p, value);
					ENSURE_SPACE_OR_RETURN(n);
					memcpy(out + outIdx, temp_p, n);
					outIdx += n;
				} else
					RETURN_ERR("Truncated BSON (in Int)");
				break;
			}
			case BSON_DATA_NUMBER: {
				if (LIKELY(inIdx + 8 <= inLen)) {
					const double value = readLE<double>();
					if (std::isfinite(value)) {
						constexpr size_t kBufferSize = 128;
						ENSURE_SPACE_OR_RETURN(kBufferSize);
						StringBuilder sb(reinterpret_cast<char*>(out + outIdx), kBufferSize);
						auto& dc = DoubleToStringConverter::EcmaScriptConverter();
						dc.ToShortest(value, &sb);
						outIdx += sb.position();
					} else {
						ENSURE_SPACE_OR_RETURN(4);
						memcpy(out + outIdx, "null", 4);
						outIdx += 4;
					}
				} else
					RETURN_ERR("Truncated BSON (in Number)");
				break;
			}
			case BSON_DATA_DATE: {
				if (LIKELY(inIdx + 8 <= inLen)) {
					ENSURE_SPACE_OR_RETURN(26);
					const int64_t value = readLE<int64_t>(); // BSON encodes UTC ms since Unix epoch
					const time_t seconds = value / 1000;
					const int32_t millis = value % 1000;

					out[outIdx++] = '"';
					tm* gmt = gmtime(&seconds);

					uint8_t temp[INT_BUF_DIGS<int32_t>];
					uint8_t* temp_p = temp;
					size_t n;

					n = fast_itoa(temp_p, gmt->tm_year + 1900);
					memcpy(out + outIdx, temp_p, n);
					outIdx += n;
					temp_p = temp;

					out[outIdx++] = '-';
					memcpy(out + outIdx, digits + ((gmt->tm_mon + 1) * 2), 2);
					outIdx += 2;

					out[outIdx++] = '-';
					memcpy(out + outIdx, digits + (gmt->tm_mday) * 2, 2);
					outIdx += 2;

					out[outIdx++] = 'T';
					memcpy(out + outIdx, digits + (gmt->tm_hour) * 2, 2);
					outIdx += 2;

					out[outIdx++] = ':';
					memcpy(out + outIdx, digits + (gmt->tm_min) * 2, 2);
					outIdx += 2;

					out[outIdx++] = ':';
					memcpy(out + outIdx, digits + (gmt->tm_sec) * 2, 2);
					outIdx += 2;

					memcpy(out + outIdx, ".000Z\"", 6);
					n = fast_itoa(temp_p, millis);
					outIdx += 4 - n;
					// TODO benchmark specializing for the three possible n
					// values. GCC inlines if specialized.
					memcpy(out + outIdx, temp_p, n);
					// if (n == 3) memcpy(out + outIdx, temp_p, n);
					// if (n == 2) memcpy(out + outIdx, temp_p, n);
					// if (n == 1) memcpy(out + outIdx, temp_p, n);
					outIdx += n + 2;
				} else
					RETURN_ERR("Truncated BSON (in Date)");
				break;
			}
			case BSON_DATA_BOOLEAN: {
				if (LIKELY(inIdx + 1 <= inLen)) {
					const uint8_t val = in[inIdx++];
					if (val == 1) {
						ENSURE_SPACE_OR_RETURN(4);
						memcpy(out + outIdx, "true", 4);
						outIdx += 4;
					} else {
						ENSURE_SPACE_OR_RETURN(5);
						memcpy(out + outIdx, "false", 5);
						outIdx += 5;
					}
				} else
					RETURN_ERR("Truncated BSON (in Boolean)");
				break;
			}
			case BSON_DATA_OBJECT: {
				// Bounds check in head of this function.
				if (UNLIKELY((transcodeObject<isa, mode>(false))))
					return true;
				break;
			}
			case BSON_DATA_ARRAY: {
				// Bounds check in head of this function.
				if (UNLIKELY((transcodeObject<isa, mode>(true))))
					return true;
				if (UNLIKELY(in[inIdx - 1] != 0)) {
					err = "Invalid array terminator byte";
					return true;
				}
				break;
			}
			case BSON_DATA_NULL: {
				ENSURE_SPACE_OR_RETURN(4);
				memcpy(out + outIdx, "null", 4);
				outIdx += 4;
				break;
			}
			case BSON_DATA_LONG: {
				if (LIKELY(inIdx + 8 <= inLen)) {
					const int64_t value = readLE<int64_t>();
					uint8_t temp[INT_BUF_DIGS<int64_t>];
					uint8_t* temp_p = temp;
					size_t n = fast_itoa(temp_p, value);
					ENSURE_SPACE_OR_RETURN(n);
					memcpy(out + outIdx, temp_p, n);
					outIdx += n;
				} else
					RETURN_ERR("Truncated BSON (in Long)");
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
				RETURN_ERR("BSON type incompatible with JSON");
			default:
				RETURN_ERR("Unknown BSON type");
			}

			arrIdx++;
		}

		ENSURE_SPACE_OR_RETURN(1);
		out[outIdx++] = isArray ? ']' : '}';
		return false;
	}
};

Napi::FunctionReference constructor;

template<ISA isa>
class BJTrans : public Napi::ObjectWrap<BJTrans<isa> > {
public:

	static Napi::Object Init(Napi::Env env, Napi::Object exports) {
		Napi::HandleScope scope(env);
		// gcc 7 seems to need the explicit Napi::ObjectWrap<BJTrans<isa> >::
		Napi::Function func = Napi::ObjectWrap<BJTrans<isa> >::DefineClass(env, "BJTrans", {
			Napi::ObjectWrap<BJTrans<isa> >::InstanceMethod(Napi::Symbol::WellKnown(env, "iterator"), &BJTrans::Iterator)
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

		worker = std::thread { &Transcoder::transcode<isa, Transcoder::Mode::PAUSE>, &trans,
			arr.Data(), arr.ByteLength(), isArray, chunkSize, out };
	}

private:
	Transcoder trans;
	std::thread worker;
	bool fixedBuffer = false;
	size_t chunkSize = 0;
	bool final = false;

	Napi::Value Iterator(const Napi::CallbackInfo& info) {
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

template<ISA isa>
Napi::Value bsonToJson(const Napi::CallbackInfo& info) {
	Napi::Env env = info.Env();

	Napi::Uint8Array arr = info[0].As<Napi::Uint8Array>();
	bool isArray = info[1].ToBoolean().Value();

	Transcoder trans;
	bool status = trans.transcode<isa, Transcoder::Mode::REALLOC>(arr.Data(), arr.ByteLength(), isArray);
	if (status) {
		std::free(trans.out);
		Napi::Error::New(env, trans.err).ThrowAsJavaScriptException();
		return Napi::Value();
	}

	Napi::Buffer<uint8_t> buf = Napi::Buffer<uint8_t>::New(env, trans.out, trans.outIdx, [](Napi::Env, uint8_t* data) {
		// printf("freeing %p\n", data);
		std::free(data);
	});

	return buf;
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
	Napi::Function fn;
	char const* isa;
#ifdef B2J_USE_AVX512
	// This is about 25% slower than the AVX2 version.
	if (supports<ISA::AVX512F>()) {
		// (Actually uses AVX512F, AVX512BW, BMI1, BMI2)
		fn = Napi::Function::New(env, bsonToJson<ISA::AVX512F>);
		BJTrans<ISA::AVX512F>::Init(env, exports);
		isa = "AVX512";
	} else
#endif
	if (supports<ISA::AVX2>()) {
		fn = Napi::Function::New(env, bsonToJson<ISA::AVX2>);
		BJTrans<ISA::AVX2>::Init(env, exports);
		isa = "AVX2";
	} else if (supports<ISA::SSE42>()) {
		fn = Napi::Function::New(env, bsonToJson<ISA::SSE42>);
		BJTrans<ISA::SSE42>::Init(env, exports);
		isa = "SSE4.2";
	} else if (supports<ISA::SSE2>()) {
		fn = Napi::Function::New(env, bsonToJson<ISA::SSE2>);
		BJTrans<ISA::SSE2>::Init(env, exports);
		isa = "SSE2";
	} else {
		fn = Napi::Function::New(env, bsonToJson<ISA::BASELINE>);
		BJTrans<ISA::BASELINE>::Init(env, exports);
		isa = "Baseline";
	}

	exports.Set(Napi::String::New(env, "bsonToJson"), fn);
	exports.Set(Napi::String::New(env, "ISE"), isa);

	return exports;
}

NODE_API_MODULE(NODE_GYP_MODULE_NAME, Init)
