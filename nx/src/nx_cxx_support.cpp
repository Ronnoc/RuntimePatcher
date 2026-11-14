// SPDX-License-Identifier: MIT
// FEFW RuntimePatcher — freestanding C++ runtime support for the subsdk9 module.
//
// libc++ is used header-only (_LIBCPP_DISABLE_EXTERN_TEMPLATE), so only the handful of
// non-template runtime entry points it references are provided here. Heap memory comes from
// the game's own allocator (malloc/free/aligned_alloc exported by main / sdk).
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <bit>
#include <functional>
#include <new>
#include <string>

#include "nx_runtime.h"

// ------------------------------------------------------------------------------------------------
// operator new / delete (hidden: never interpose the game's allocator entry points)
// ------------------------------------------------------------------------------------------------
namespace {
[[noreturn]] void fefw_oom(size_t n) {
    fefw_nx::log("out of memory (%zu bytes)", n);
    __builtin_trap();
}
}  // namespace

void* operator new(size_t n) {
    if (n == 0) n = 1;
    void* p = malloc(n);
    if (!p) fefw_oom(n);
    return p;
}
void* operator new[](size_t n) { return operator new(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return malloc(n ? n : 1); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return malloc(n ? n : 1); }
void* operator new(size_t n, std::align_val_t a) {
    size_t al = static_cast<size_t>(a);
    if (al < sizeof(void*)) al = sizeof(void*);
    size_t sz = (n + al - 1) & ~(al - 1);
    void* p = aligned_alloc(al, sz ? sz : al);
    if (!p) fefw_oom(n);
    return p;
}
void* operator new[](size_t n, std::align_val_t a) { return operator new(n, a); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }
void operator delete(void* p, std::align_val_t) noexcept { free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { free(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { free(p); }

// ------------------------------------------------------------------------------------------------
// libc++ / Itanium ABI hooks
// ------------------------------------------------------------------------------------------------
namespace std {
inline namespace _LIBCPP_ABI_NAMESPACE {

[[noreturn]] void __libcpp_verbose_abort(const char* fmt, ...) noexcept {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fefw_nx::log("libc++ abort: %s", buf);
    __builtin_trap();
}

// Same algorithm contract as libc++'s src/hash.cpp: smallest prime >= n.
size_t __next_prime(size_t n) {
    if (n <= 2) return 2;
    if ((n & 1) == 0) ++n;
    for (;; n += 2) {
        bool prime = true;
        for (size_t d = 3; d * d <= n; d += 2) {
            if (n % d == 0) {
                prime = false;
                break;
            }
        }
        if (prime) return n;
    }
}

string to_string(int v) { char b[16]; snprintf(b, sizeof(b), "%d", v); return string(b); }
string to_string(unsigned v) { char b[16]; snprintf(b, sizeof(b), "%u", v); return string(b); }
string to_string(long v) { char b[24]; snprintf(b, sizeof(b), "%ld", v); return string(b); }
string to_string(unsigned long v) { char b[24]; snprintf(b, sizeof(b), "%lu", v); return string(b); }
string to_string(long long v) { char b[24]; snprintf(b, sizeof(b), "%lld", v); return string(b); }
string to_string(unsigned long long v) { char b[24]; snprintf(b, sizeof(b), "%llu", v); return string(b); }
string to_string(float v) { char b[64]; snprintf(b, sizeof(b), "%f", v); return string(b); }
string to_string(double v) { char b[64]; snprintf(b, sizeof(b), "%f", v); return string(b); }
string to_string(long double v) { char b[64]; snprintf(b, sizeof(b), "%Lf", v); return string(b); }

// Out-of-line instantiations that libc++ headers declare `extern template` and normally ship in
// libc++.so (src/string.cpp, src/algorithm.cpp, src/functional.cpp). Mirrors those files.
#define FEFW_EXTERN_TEMPLATE_DEFINE(...) template __VA_ARGS__;
#ifdef _LIBCPP_ABI_STRING_OPTIMIZED_EXTERNAL_INSTANTIATION
_LIBCPP_STRING_UNSTABLE_EXTERN_TEMPLATE_LIST(FEFW_EXTERN_TEMPLATE_DEFINE, char)
#else
_LIBCPP_STRING_V1_EXTERN_TEMPLATE_LIST(FEFW_EXTERN_TEMPLATE_DEFINE, char)
#endif
#undef FEFW_EXTERN_TEMPLATE_DEFINE
template string operator+ <char, char_traits<char>, allocator<char>>(char const*, string const&);

template <class Comp, class RandomAccessIterator>
void __sort(RandomAccessIterator first, RandomAccessIterator last, Comp) {  // = libc++ src/algorithm.cpp
    if (first == last) return;
    auto depth_limit = 2 * std::__bit_log2(static_cast<size_t>(last - first));
    std::__introsort<_ClassicAlgPolicy, ranges::less, RandomAccessIterator,
                     __use_branchless_sort<ranges::less, RandomAccessIterator>>(first, last, ranges::less{}, depth_limit);
}

template void __sort<__less<char>&, char*>(char*, char*, __less<char>&);
template void __sort<__less<signed char>&, signed char*>(signed char*, signed char*, __less<signed char>&);
template void __sort<__less<unsigned char>&, unsigned char*>(unsigned char*, unsigned char*, __less<unsigned char>&);
template void __sort<__less<short>&, short*>(short*, short*, __less<short>&);
template void __sort<__less<unsigned short>&, unsigned short*>(unsigned short*, unsigned short*, __less<unsigned short>&);
template void __sort<__less<int>&, int*>(int*, int*, __less<int>&);
template void __sort<__less<unsigned>&, unsigned*>(unsigned*, unsigned*, __less<unsigned>&);
template void __sort<__less<long>&, long*>(long*, long*, __less<long>&);
template void __sort<__less<unsigned long>&, unsigned long*>(unsigned long*, unsigned long*, __less<unsigned long>&);
template void __sort<__less<long long>&, long long*>(long long*, long long*, __less<long long>&);
template void __sort<__less<unsigned long long>&, unsigned long long*>(unsigned long long*, unsigned long long*, __less<unsigned long long>&);
template void __sort<__less<float>&, float*>(float*, float*, __less<float>&);
template void __sort<__less<double>&, double*>(double*, double*, __less<double>&);

size_t __hash_memory(_LIBCPP_NOESCAPE const void* ptr, size_t size) noexcept { return __murmur2_or_cityhash<size_t>()(ptr, size); }

}  // namespace _LIBCPP_ABI_NAMESPACE
}  // namespace std

extern "C" {

void* __dso_handle = &__dso_handle;

// Static destructors never run (module is never unloaded).
int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }

[[noreturn]] void __cxa_pure_virtual() {
    fefw_nx::log("pure virtual call");
    __builtin_trap();
}

}  // extern "C"
