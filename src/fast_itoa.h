#pragma once

#include <cstdint>
#include <cstdlib>

// Adaptetd from https://github.com/fmtlib/fmt/blob/master/include/fmt/format.h#L2818

// Unclear if MIT license (repository file) or the below (from the above-linked file):

/*
Copyright (c) 2012 - present, Victor Zverovich

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

--- Optional exception to the license ---

As an exception, if, as a result of your compiling your source code, portions of
this Software are embedded into a machine-executable object form of such source
code, you may redistribute such embedded portions in such object form without
including the above copyright and permission notices.

*/

constexpr const char digits[] =
	"0001020304050607080910111213141516171819"
	"2021222324252627282930313233343536373839"
	"4041424344454647484950515253545556575859"
	"6061626364656667686970717273747576777879"
	"8081828384858687888990919293949596979899";

template<typename T> constexpr std::size_t INT_BUF_DIGS = 0;
template<> constexpr std::size_t INT_BUF_DIGS<std::int32_t> = 11;
template<> constexpr std::size_t INT_BUF_DIGS<std::int64_t> = 20;

template<typename T>
std::size_t fast_itoa(std::uint8_t* &p, T val) {
	p += INT_BUF_DIGS<T>;
	std::size_t n = 0;

	const bool isNegative = val < 0;
	if (isNegative)
		val = 0 - val;

	while (val >= 100) {
		std::size_t index = static_cast<std::size_t>((val % 100) * 2);
		val /= 100;
		*--p = digits[index + 1];
		*--p = digits[index];
		n += 2;
	}

	if (val < 10) {
		*--p = static_cast<std::uint8_t>('0' + val);
		n++;
	} else {
		std::size_t index = static_cast<std::size_t>(val * 2);
		*--p = digits[index + 1];
		*--p = digits[index];
		n += 2;
	}

	if (isNegative) {
		*--p = '-';
		n++;
	}

	return n;
}
