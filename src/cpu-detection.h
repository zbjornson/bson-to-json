#pragma once

#include <cstdint>

enum class ISA {
	BASELINE,
	SSE2,
	SSE3,
	SSSE3,
	SSE42,
	AVX,
	AVX2,
	AVX512F,
	AVX512VL,
	BMI1,
	BMI2
};

template<ISA isa> static bool supports() { return false; }
template<> static bool supports<ISA::BASELINE>() { return true; }

#if defined(__x86_64__) || defined(_M_X64)

#ifdef _MSC_VER
# include <intrin.h>
#endif

constexpr static std::uint8_t EAX = 0;
constexpr static std::uint8_t EBX = 1;
constexpr static std::uint8_t ECX = 2;
constexpr static std::uint8_t EDX = 3;

static bool cpuid(std::uint8_t outreg, std::uint8_t bit, std::uint32_t initEax,
		std::uint32_t initEcx = 0) {
	std::uint32_t regs[4];
#ifdef _MSC_VER
	__cpuidex(reinterpret_cast<std::int32_t*>(regs), initEax, initEcx);
#else
	asm volatile("cpuid"
		: "=a" (regs[0]), "=b" (regs[1]), "=c" (regs[2]), "=d" (regs[3])
		: "0" (initEax), "2" (initEcx));
#endif
	return regs[outreg] & (1 << bit);
}

template<> static bool supports<ISA::SSE2>() { return cpuid(EDX, 26, 1); }
template<> static bool supports<ISA::SSE3>() { return cpuid(ECX, 0, 1); }
template<> static bool supports<ISA::SSSE3>() { return cpuid(ECX, 9, 1); }
template<> static bool supports<ISA::SSE42>() { return cpuid(ECX, 20, 1); }
template<> static bool supports<ISA::AVX>() { return cpuid(ECX, 28, 1); }
template<> static bool supports<ISA::AVX2>() { return cpuid(EBX, 5, 7); }
template<> static bool supports<ISA::AVX512F>() { return cpuid(EBX, 16, 7); }
template<> static bool supports<ISA::AVX512VL>() { return cpuid(EBX, 31, 7); }
template<> static bool supports<ISA::BMI1>() { return cpuid(EBX, 3, 7); }
template<> static bool supports<ISA::BMI2>() { return cpuid(EBX, 8, 7); }

#endif // x86_64
