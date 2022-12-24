
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef _MSC_VER
#define IS_UTF8_VISUAL_STUDIO 1
/**
 * We want to differentiate carefully between
 * clang under visual studio and regular visual
 * studio.
 *
 * Under clang for Windows, we enable:
 *  * target pragmas so that part and only part of the
 *     code gets compiled for advanced instructions.
 *
 */
#ifdef __clang__
// clang under visual studio
#define IS_UTF8_CLANG_VISUAL_STUDIO 1
#else
// just regular visual studio (best guess)
#define IS_UTF8_REGULAR_VISUAL_STUDIO 1
#endif // __clang__
#endif // _MSC_VER

#if defined(__x86_64__) || defined(_M_AMD64)
#define IS_UTF8_IS_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#define IS_UTF8_IS_ARM64 1
#elif defined(__PPC64__) || defined(_M_PPC64)
#define IS_UTF8_IS_PPC64 1
#else // defined(__x86_64__) || defined(_M_AMD64)
#define IS_UTF8_IS_32BITS 1
// We do not support 32-bit platforms, but it can be
// handy to identify them.
#if defined(_M_IX86) || defined(__i386__)
#define IS_UTF8_IS_X86_32BITS 1
#elif defined(__arm__) || defined(_M_ARM)
#define IS_UTF8_IS_ARM_32BITS 1
#elif defined(__PPC__) || defined(_M_PPC)
#define IS_UTF8_IS_PPC_32BITS 1
#endif // defined(_M_IX86) || defined(__i386__)

#endif // defined(__x86_64__) || defined(_M_AMD64)

#if !IS_UTF8_IS_ARM64

namespace is_utf8_fallback {

static inline bool is_utf8(const char *buf, size_t len) {
  const uint8_t *data = reinterpret_cast<const uint8_t *>(buf);
  uint64_t pos = 0;
  uint32_t code_point = 0;
  while (pos < len) {
    // check of the next 8 bytes are ascii.
    uint64_t next_pos = pos + 16;
    if (next_pos <= len) { // if it is safe to read 8 more bytes, check that
                           // they are ascii
      uint64_t v1;
      ::memcpy(&v1, data + pos, sizeof(uint64_t));
      uint64_t v2;
      ::memcpy(&v2, data + pos + sizeof(uint64_t), sizeof(uint64_t));
      uint64_t v(v1 | v2);
      if ((v & 0x8080808080808080) == 0) {
        pos = next_pos;
        continue;
      }
    }
    unsigned char byte = data[pos];

    while (byte < 128) {
      if (++pos == len) {
        return true;
      }
      byte = data[pos];
    }

    if ((byte & 224) == 192) {
      next_pos = pos + 2;
      if (next_pos > len) {
        return false;
      }
      if ((data[pos + 1] & 192) != 128) {
        return false;
      }
      // range check
      code_point = (byte & 31) << 6 | (data[pos + 1] & 63);
      if ((code_point < 0x80) || (0x7ff < code_point)) {
        return false;
      }
    } else if ((byte & 240) == 224) {
      next_pos = pos + 3;
      if (next_pos > len) {
        return false;
      }
      if ((data[pos + 1] & 192) != 128) {
        return false;
      }
      if ((data[pos + 2] & 192) != 128) {
        return false;
      }
      // range check
      code_point =
          (byte & 15) << 12 | (data[pos + 1] & 63) << 6 | (data[pos + 2] & 63);
      if ((code_point < 0x800) || (0xffff < code_point) ||
          (0xd7ff < code_point && code_point < 0xe000)) {
        return false;
      }
    } else if ((byte & 248) == 240) { // 240
      next_pos = pos + 4;
      if (next_pos > len) {
        return false;
      }
      if ((data[pos + 1] & 192) != 128) {
        return false;
      }
      if ((data[pos + 2] & 192) != 128) {
        return false;
      }
      if ((data[pos + 3] & 192) != 128) {
        return false;
      }
      // range check
      code_point = (byte & 7) << 18 | (data[pos + 1] & 63) << 12 |
                   (data[pos + 2] & 63) << 6 | (data[pos + 3] & 63);
      if (code_point <= 0xffff || 0x10ffff < code_point) {
        return false;
      }
    } else {
      // we may have a continuation
      return false;
    }
    pos = next_pos;
  }
  return true;
}

} // namespace is_utf8_fallback
#endif
#if IS_UTF8_IS_ARM64
#include <arm_neon.h>

namespace is_utf8_neon {
// all byte values must be no larger than 0xF4
static inline void check_smaller_than_0xf4(int8x16_t current_bytes,
                                           int8x16_t *has_error) {
  // unsigned, saturates to 0 below max
  *has_error = vorrq_s8(
      *has_error, vreinterpretq_s8_u8(vqsubq_u8(
                      vreinterpretq_u8_s8(current_bytes), vdupq_n_u8(0xF4))));
}

static inline int8x16_t continuation_lengths(int8x16_t high_nibbles) {
  static const int8_t nibbles_array[] = {
      1, 1, 1, 1, 1, 1, 1, 1, // 0xxx (ASCII)
      0, 0, 0, 0,             // 10xx (continuation)
      2, 2,                   // 110x
      3,                      // 1110
      4,                      // 1111, next should be 0 (not checked here)
  };
  return vqtbl1q_s8(vld1q_s8(nibbles_array), vreinterpretq_u8_s8(high_nibbles));
}

static inline int8x16_t carry_continuations(int8x16_t initial_lengths,
                                            int8x16_t previous_carries) {

  int8x16_t right1 = vreinterpretq_s8_u8(vqsubq_u8(
      vreinterpretq_u8_s8(vextq_s8(previous_carries, initial_lengths, 16 - 1)),
      vdupq_n_u8(1)));
  int8x16_t sum = vaddq_s8(initial_lengths, right1);

  int8x16_t right2 = vreinterpretq_s8_u8(
      vqsubq_u8(vreinterpretq_u8_s8(vextq_s8(previous_carries, sum, 16 - 2)),
                vdupq_n_u8(2)));
  return vaddq_s8(sum, right2);
}

static inline void check_continuations(int8x16_t initial_lengths,
                                       int8x16_t carries,
                                       int8x16_t *has_error) {

  // overlap || underlap
  // carry > length && length > 0 || !(carry > length) && !(length > 0)
  // (carries > length) == (lengths > 0)
  uint8x16_t overunder = vceqq_u8(vcgtq_s8(carries, initial_lengths),
                                  vcgtq_s8(initial_lengths, vdupq_n_s8(0)));

  *has_error = vorrq_s8(*has_error, vreinterpretq_s8_u8(overunder));
}

// when 0xED is found, next byte must be no larger than 0x9F
// when 0xF4 is found, next byte must be no larger than 0x8F
// next byte must be continuation, ie sign bit is set, so signed < is ok
static inline void check_first_continuation_max(int8x16_t current_bytes,
                                                int8x16_t off1_current_bytes,
                                                int8x16_t *has_error) {
  uint8x16_t maskED = vceqq_s8(off1_current_bytes, vdupq_n_s8(0xED));
  uint8x16_t maskF4 = vceqq_s8(off1_current_bytes, vdupq_n_s8(0xF4));

  uint8x16_t badfollowED =
      vandq_u8(vcgtq_s8(current_bytes, vdupq_n_s8(0x9F)), maskED);
  uint8x16_t badfollowF4 =
      vandq_u8(vcgtq_s8(current_bytes, vdupq_n_s8(0x8F)), maskF4);

  *has_error = vorrq_s8(
      *has_error, vreinterpretq_s8_u8(vorrq_u8(badfollowED, badfollowF4)));
}

// map off1_hibits => error condition
// hibits     off1    cur
// C       => < C2 && true
// E       => < E1 && < A0
// F       => < F1 && < 90
// else      false && false
static inline void check_overlong(int8x16_t current_bytes,
                                  int8x16_t off1_current_bytes,
                                  int8x16_t hibits, int8x16_t previous_hibits,
                                  int8x16_t *has_error) {
  int8x16_t off1_hibits = vextq_s8(previous_hibits, hibits, 16 - 1);
  static const int8_t initial_mins_array[] = {
      -128,       -128, -128, -128, -128, -128,
      -128,       -128, -128, -128, -128, -128, // 10xx => false
      char(0xC2), -128,                         // 110x
      char(0xE1),                               // 1110
      char(0xF1),
  };
  int8x16_t initial_mins = vqtbl1q_s8(vld1q_s8(initial_mins_array),
                                      vreinterpretq_u8_s8(off1_hibits));

  uint8x16_t initial_under = vcgtq_s8(initial_mins, off1_current_bytes);

  static const int8_t second_mins_array[] = {
      -128,       -128, -128, -128, -128, -128,
      -128,       -128, -128, -128, -128, -128, // 10xx => false
      127,        127,                          // 110x => true
      char(0xA0),                               // 1110
      char(0x90),
  };
  int8x16_t second_mins =
      vqtbl1q_s8(vld1q_s8(second_mins_array), vreinterpretq_u8_s8(off1_hibits));
  uint8x16_t second_under = vcgtq_s8(second_mins, current_bytes);
  *has_error = vorrq_s8(
      *has_error, vreinterpretq_s8_u8(vandq_u8(initial_under, second_under)));
}

struct processed_utf_bytes {
  int8x16_t rawbytes;
  int8x16_t high_nibbles;
  int8x16_t carried_continuations;
};

static inline void count_nibbles(int8x16_t bytes,
                                 struct processed_utf_bytes *answer) {
  answer->rawbytes = bytes;
  answer->high_nibbles =
      vreinterpretq_s8_u8(vshrq_n_u8(vreinterpretq_u8_s8(bytes), 4));
}

// check whether the current bytes are valid UTF-8
// at the end of the function, previous gets updated
static inline struct processed_utf_bytes
check_utf8_bytes(int8x16_t current_bytes, struct processed_utf_bytes *previous,
                 int8x16_t *has_error) {
  struct processed_utf_bytes pb;
  count_nibbles(current_bytes, &pb);

  check_smaller_than_0xf4(current_bytes, has_error);

  int8x16_t initial_lengths = continuation_lengths(pb.high_nibbles);

  pb.carried_continuations =
      carry_continuations(initial_lengths, previous->carried_continuations);

  check_continuations(initial_lengths, pb.carried_continuations, has_error);

  int8x16_t off1_current_bytes =
      vextq_s8(previous->rawbytes, pb.rawbytes, 16 - 1);
  check_first_continuation_max(current_bytes, off1_current_bytes, has_error);

  check_overlong(current_bytes, off1_current_bytes, pb.high_nibbles,
                 previous->high_nibbles, has_error);
  return pb;
}

static inline bool is_utf8(const char *src, size_t len) {
  size_t i = 0;
  int8x16_t has_error = vdupq_n_s8(0);
  struct processed_utf_bytes previous;
  previous.rawbytes = vdupq_n_s8(0);
  previous.high_nibbles = vdupq_n_s8(0);
  previous.carried_continuations = vdupq_n_s8(0);
  if (len >= 16) {
    for (; i <= len - 16; i += 16) {
      int8x16_t current_bytes = vld1q_s8((int8_t *)(src + i));
      previous = check_utf8_bytes(current_bytes, &previous, &has_error);
    }
  }

  // last part
  if (i < len) {
    char buffer[16];
    memset(buffer, 0, 16);
    memcpy(buffer, src + i, len - i);
    int8x16_t current_bytes = vld1q_s8((int8_t *)buffer);
    previous = check_utf8_bytes(current_bytes, &previous, &has_error);
  } else {
    static const int8_t verror_array[] = {9, 9, 9, 9, 9, 9, 9, 9,
                                          9, 9, 9, 9, 9, 9, 9, 1};

    has_error =
        vorrq_s8(vreinterpretq_s8_u8(vcgtq_s8(previous.carried_continuations,
                                              vld1q_s8(verror_array))),
                 has_error);
  }

  return vmaxvq_u8(vreinterpretq_u8_s8(has_error)) == 0 ? true : false;
}
} // namespace is_utf8_neon

bool is_utf8(const char *src, size_t len) {
  return is_utf8_neon::is_utf8(src, len);
}
// We are done!

// #endif // IS_UTF8_IS_ARM64
#elif IS_UTF8_IS_X86_64

#ifndef SIMDUTF_TARGET_REGION // borrow directly from simdutf

#define SIMDUTF_STRINGIFY_IMPLEMENTATION_(a) #a
#define SIMDUTF_STRINGIFY(a) SIMDUTF_STRINGIFY_IMPLEMENTATION_(a)

#ifdef __clang__
// clang does not have GCC push pop
// warning: clang attribute push can't be used within a namespace in clang up
// til 8.0 so SIMDUTF_TARGET_REGION and SIMDUTF_UNTARGET_REGION must be
// *outside* of a namespace.
#define SIMDUTF_TARGET_REGION(T)                                               \
  _Pragma(SIMDUTF_STRINGIFY(                                                   \
      clang attribute push(__attribute__((target(T))), apply_to = function)))
#define SIMDUTF_UNTARGET_REGION _Pragma("clang attribute pop")
#elif defined(__GNUC__)
// GCC is easier
#define SIMDUTF_TARGET_REGION(T)                                               \
  _Pragma("GCC push_options") _Pragma(SIMDUTF_STRINGIFY(GCC target(T)))
#define SIMDUTF_UNTARGET_REGION _Pragma("GCC pop_options")
#endif // clang then gcc

// Default target region macros don't do anything.
#ifndef SIMDUTF_TARGET_REGION
#define SIMDUTF_TARGET_REGION(T)
#define SIMDUTF_UNTARGET_REGION
#endif

#endif // SIMDUTF_TARGET_REGION

#include <cstdint>
#include <cstdlib>
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(HAVE_GCC_GET_CPUID) && defined(USE_GCC_GET_CPUID)
#include <cpuid.h>
#endif // defined(_MSC_VER)

namespace is_utf8_core {

enum instruction_set {
  DEFAULT = 0x0,
  NEON = 0x1,
  AVX2 = 0x4,
  SSE42 = 0x8,
  PCLMULQDQ = 0x10,
  BMI1 = 0x20,
  BMI2 = 0x40,
  ALTIVEC = 0x80,
  AVX512F = 0x100,
  AVX512DQ = 0x200,
  AVX512IFMA = 0x400,
  AVX512PF = 0x800,
  AVX512ER = 0x1000,
  AVX512CD = 0x2000,
  AVX512BW = 0x4000,
  AVX512VL = 0x8000,
  AVX512VBMI2 = 0x10000
};
namespace cpuid_bit {
// Can be found on Intel ISA Reference for CPUID

// EAX = 0x01
constexpr uint32_t pclmulqdq = uint32_t(1)
                               << 1; ///< @private bit  1 of ECX for EAX=0x1
constexpr uint32_t sse42 = uint32_t(1)
                           << 20; ///< @private bit 20 of ECX for EAX=0x1

// EAX = 0x7f (Structured Extended Feature Flags), ECX = 0x00 (Sub-leaf)
// See: "Table 3-8. Information Returned by CPUID Instruction"
namespace ebx {
constexpr uint32_t bmi1 = uint32_t(1) << 3;
constexpr uint32_t avx2 = uint32_t(1) << 5;
constexpr uint32_t bmi2 = uint32_t(1) << 8;
constexpr uint32_t avx512f = uint32_t(1) << 16;
constexpr uint32_t avx512dq = uint32_t(1) << 17;
constexpr uint32_t avx512ifma = uint32_t(1) << 21;
constexpr uint32_t avx512cd = uint32_t(1) << 28;
constexpr uint32_t avx512bw = uint32_t(1) << 30;
constexpr uint32_t avx512vl = uint32_t(1) << 31;
} // namespace ebx

namespace ecx {
constexpr uint32_t avx512vbmi = uint32_t(1) << 1;
constexpr uint32_t avx512vbmi2 = uint32_t(1) << 6;
constexpr uint32_t avx512vnni = uint32_t(1) << 11;
constexpr uint32_t avx512bitalg = uint32_t(1) << 12;
constexpr uint32_t avx512vpopcnt = uint32_t(1) << 14;
} // namespace ecx
namespace edx {
constexpr uint32_t avx512vp2intersect = uint32_t(1) << 8;
}
} // namespace cpuid_bit

static inline void cpuid(uint32_t *eax, uint32_t *ebx, uint32_t *ecx,
                         uint32_t *edx) {
#if defined(_MSC_VER)
  int cpu_info[4];
  __cpuid(cpu_info, *eax);
  *eax = cpu_info[0];
  *ebx = cpu_info[1];
  *ecx = cpu_info[2];
  *edx = cpu_info[3];
#elif defined(HAVE_GCC_GET_CPUID) && defined(USE_GCC_GET_CPUID)
  uint32_t level = *eax;
  __get_cpuid(level, eax, ebx, ecx, edx);
#else
  uint32_t a = *eax, b, c = *ecx, d;
  asm volatile("cpuid\n\t" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
  *eax = a;
  *ebx = b;
  *ecx = c;
  *edx = d;
#endif
}

static inline uint32_t detect_supported_architectures() {
  uint32_t eax;
  uint32_t ebx = 0;
  uint32_t ecx = 0;
  uint32_t edx = 0;
  uint32_t host_isa = 0x0;

  // EBX for EAX=0x1
  eax = 0x1;
  cpuid(&eax, &ebx, &ecx, &edx);

  if (ecx & cpuid_bit::sse42) {
    host_isa |= instruction_set::SSE42;
  }

  if (ecx & cpuid_bit::pclmulqdq) {
    host_isa |= instruction_set::PCLMULQDQ;
  }

  // ECX for EAX=0x7
  eax = 0x7;
  ecx = 0x0; // Sub-leaf = 0
  cpuid(&eax, &ebx, &ecx, &edx);
  if (ebx & cpuid_bit::ebx::avx2) {
    host_isa |= instruction_set::AVX2;
  }
  if (ebx & cpuid_bit::ebx::bmi1) {
    host_isa |= instruction_set::BMI1;
  }
  if (ebx & cpuid_bit::ebx::bmi2) {
    host_isa |= instruction_set::BMI2;
  }
  if (ebx & cpuid_bit::ebx::avx512f) {
    host_isa |= instruction_set::AVX512F;
  }
  if (ebx & cpuid_bit::ebx::avx512bw) {
    host_isa |= instruction_set::AVX512BW;
  }
  if (ebx & cpuid_bit::ebx::avx512cd) {
    host_isa |= instruction_set::AVX512CD;
  }
  if (ebx & cpuid_bit::ebx::avx512dq) {
    host_isa |= instruction_set::AVX512DQ;
  }
  if (ebx & cpuid_bit::ebx::avx512vl) {
    host_isa |= instruction_set::AVX512VL;
  }
  if (ecx & cpuid_bit::ecx::avx512vbmi2) {
    host_isa |= instruction_set::AVX512VBMI2;
  }
  return host_isa;
}

enum { FALLBACK_KERNEL = 2, SSE_KERNEL = 1, AVX2_KERNEL = 0 };

static inline uint32_t which_kernel() {
  if ((detect_supported_architectures() & instruction_set::AVX2) ==
      instruction_set::AVX2) {
    return AVX2_KERNEL;
  }
  if ((detect_supported_architectures() & instruction_set::SSE42) ==
      instruction_set::SSE42) {
    return SSE_KERNEL;
  }
  return FALLBACK_KERNEL; // old
}
} // namespace is_utf8_core

#if IS_UTF8_VISUAL_STUDIO
#include <intrin.h> // visual studio or clang
#else
#include <x86intrin.h>
#endif

#define IS_UTF8_TARGET_SSE SIMDUTF_TARGET_REGION("sse4.2")

IS_UTF8_TARGET_SSE
namespace is_utf8_sse {
// all byte values must be no larger than 0xF4
static inline void check_smaller_than_0xf4(__m128i current_bytes,
                                           __m128i *has_error) {
  // unsigned, saturates to 0 below max
  *has_error = _mm_or_si128(
      *has_error, _mm_subs_epu8(current_bytes, _mm_set1_epi8(char(0xF4))));
}

static inline __m128i continuation_lengths(__m128i high_nibbles) {
  return _mm_shuffle_epi8(
      _mm_setr_epi8(1, 1, 1, 1, 1, 1, 1, 1, // 0xxx (ASCII)
                    0, 0, 0, 0,             // 10xx (continuation)
                    2, 2,                   // 110x
                    3,                      // 1110
                    4), // 1111, next should be 0 (not checked here)
      high_nibbles);
}

static inline __m128i carry_continuations(__m128i initial_lengths,
                                          __m128i previous_carries) {

  __m128i right1 =
      _mm_subs_epu8(_mm_alignr_epi8(initial_lengths, previous_carries, 16 - 1),
                    _mm_set1_epi8(1));
  __m128i sum = _mm_add_epi8(initial_lengths, right1);

  __m128i right2 = _mm_subs_epu8(_mm_alignr_epi8(sum, previous_carries, 16 - 2),
                                 _mm_set1_epi8(2));
  return _mm_add_epi8(sum, right2);
}

static inline void check_continuations(__m128i initial_lengths, __m128i carries,
                                       __m128i *has_error) {

  // overlap || underlap
  // carry > length && length > 0 || !(carry > length) && !(length > 0)
  // (carries > length) == (lengths > 0)
  __m128i overunder =
      _mm_cmpeq_epi8(_mm_cmpgt_epi8(carries, initial_lengths),
                     _mm_cmpgt_epi8(initial_lengths, _mm_setzero_si128()));

  *has_error = _mm_or_si128(*has_error, overunder);
}

// when 0xED is found, next byte must be no larger than 0x9F
// when 0xF4 is found, next byte must be no larger than 0x8F
// next byte must be continuation, ie sign bit is set, so signed < is ok
static inline void check_first_continuation_max(__m128i current_bytes,
                                                __m128i off1_current_bytes,
                                                __m128i *has_error) {
  __m128i maskED =
      _mm_cmpeq_epi8(off1_current_bytes, _mm_set1_epi8(char(0xED)));
  __m128i maskF4 =
      _mm_cmpeq_epi8(off1_current_bytes, _mm_set1_epi8(char(0xF4)));

  __m128i badfollowED = _mm_and_si128(
      _mm_cmpgt_epi8(current_bytes, _mm_set1_epi8(char(0x9F))), maskED);
  __m128i badfollowF4 = _mm_and_si128(
      _mm_cmpgt_epi8(current_bytes, _mm_set1_epi8(char(0x8F))), maskF4);

  *has_error = _mm_or_si128(*has_error, _mm_or_si128(badfollowED, badfollowF4));
}

// map off1_hibits => error condition
// hibits     off1    cur
// C       => < C2 && true
// E       => < E1 && < A0
// F       => < F1 && < 90
// else      false && false
static inline void check_overlong(__m128i current_bytes,
                                  __m128i off1_current_bytes, __m128i hibits,
                                  __m128i previous_hibits, __m128i *has_error) {
  __m128i off1_hibits = _mm_alignr_epi8(hibits, previous_hibits, 16 - 1);
  __m128i initial_mins = _mm_shuffle_epi8(
      _mm_setr_epi8(-128, -128, -128, -128, -128, -128, -128, -128, -128, -128,
                    -128, -128,       // 10xx => false
                    char(0xC2), -128, // 110x
                    char(0xE1),       // 1110
                    char(0xF1)),
      off1_hibits);

  __m128i initial_under = _mm_cmpgt_epi8(initial_mins, off1_current_bytes);

  __m128i second_mins = _mm_shuffle_epi8(
      _mm_setr_epi8(-128, -128, -128, -128, -128, -128, -128, -128, -128, -128,
                    -128, -128, // 10xx => false
                    127, 127,   // 110x => true
                    char(0xA0), // 1110
                    char(0x90)),
      off1_hibits);
  __m128i second_under = _mm_cmpgt_epi8(second_mins, current_bytes);
  *has_error =
      _mm_or_si128(*has_error, _mm_and_si128(initial_under, second_under));
}

struct processed_utf_bytes {
  __m128i rawbytes;
  __m128i high_nibbles;
  __m128i carried_continuations;
};

static inline void count_nibbles(__m128i bytes,
                                 struct processed_utf_bytes *answer) {
  answer->rawbytes = bytes;
  answer->high_nibbles =
      _mm_and_si128(_mm_srli_epi16(bytes, 4), _mm_set1_epi8(0x0F));
}

// check whether the current bytes are valid UTF-8
// at the end of the function, previous gets updated
static inline struct processed_utf_bytes
check_utf8_bytes(__m128i current_bytes, struct processed_utf_bytes *previous,
                 __m128i *has_error) {

  struct processed_utf_bytes pb;
  count_nibbles(current_bytes, &pb);

  check_smaller_than_0xf4(current_bytes, has_error);

  __m128i initial_lengths = continuation_lengths(pb.high_nibbles);

  pb.carried_continuations =
      carry_continuations(initial_lengths, previous->carried_continuations);

  check_continuations(initial_lengths, pb.carried_continuations, has_error);

  __m128i off1_current_bytes =
      _mm_alignr_epi8(pb.rawbytes, previous->rawbytes, 16 - 1);
  check_first_continuation_max(current_bytes, off1_current_bytes, has_error);

  check_overlong(current_bytes, off1_current_bytes, pb.high_nibbles,
                 previous->high_nibbles, has_error);
  return pb;
}

static inline bool is_utf8(const char *src, size_t len) {
  size_t i = 0;
  __m128i has_error = _mm_setzero_si128();
  struct processed_utf_bytes previous;
  previous.rawbytes = _mm_setzero_si128() previous.high_nibbles =
      _mm_setzero_si128();
  previous.carried_continuations = _mm_setzero_si128();
  if (len >= 16) {
    for (; i <= len - 16; i += 16) {
      __m128i current_bytes = _mm_loadu_si128((const __m128i *)(src + i));
      previous = check_utf8_bytes(current_bytes, &previous, &has_error);
    }
  }

  // last part
  if (i < len) {
    char buffer[16];
    memset(buffer, 0, 16);
    memcpy(buffer, src + i, len - i);
    __m128i current_bytes = _mm_loadu_si128((const __m128i *)(buffer));
    previous = check_utf8_bytes(current_bytes, &previous, &has_error);
  } else {
    has_error =
        _mm_or_si128(_mm_cmpgt_epi8(previous.carried_continuations,
                                    _mm_setr_epi8(9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
                                                  9, 9, 9, 9, 9, 1)),
                     has_error);
  }

  return _mm_testz_si128(has_error, has_error) ? true : false;
}
} // namespace is_utf8_sse
SIMDUTF_UNTARGET_REGION

#define IS_UTF8_TARGET_AVX2 SIMDUTF_TARGET_REGION("avx2,bmi,lzcnt")

IS_UTF8_TARGET_AVX2
namespace is_utf8_avx2 {

static inline __m256i push_last_byte_of_a_to_b(__m256i a, __m256i b) {
  return _mm256_alignr_epi8(b, _mm256_permute2x128_si256(a, b, 0x21), 15);
}

static inline __m256i push_last_2bytes_of_a_to_b(__m256i a, __m256i b) {
  return _mm256_alignr_epi8(b, _mm256_permute2x128_si256(a, b, 0x21), 14);
}

// all byte values must be no larger than 0xF4
static inline void check_smaller_than_0xf4(__m256i current_bytes,
                                           __m256i *has_error) {
  // unsigned, saturates to 0 below max
  *has_error = _mm256_or_si256(
      *has_error,
      _mm256_subs_epu8(current_bytes, _mm256_set1_epi8(char(0xF4))));
}

static inline __m256i continuation_lengths(__m256i high_nibbles) {
  return _mm256_shuffle_epi8(
      _mm256_setr_epi8(1, 1, 1, 1, 1, 1, 1, 1, // 0xxx (ASCII)
                       0, 0, 0, 0,             // 10xx (continuation)
                       2, 2,                   // 110x
                       3,                      // 1110
                       4, // 1111, next should be 0 (not checked here)
                       1, 1, 1, 1, 1, 1, 1, 1, // 0xxx (ASCII)
                       0, 0, 0, 0,             // 10xx (continuation)
                       2, 2,                   // 110x
                       3,                      // 1110
                       4 // 1111, next should be 0 (not checked here)
                       ),
      high_nibbles);
}

static inline __m256i carry_continuations(__m256i initial_lengths,
                                          __m256i previous_carries) {

  __m256i right1 = _mm256_subs_epu8(
      push_last_byte_of_a_to_b(previous_carries, initial_lengths),
      _mm256_set1_epi8(1));
  __m256i sum = _mm256_add_epi8(initial_lengths, right1);

  __m256i right2 = _mm256_subs_epu8(
      push_last_2bytes_of_a_to_b(previous_carries, sum), _mm256_set1_epi8(2));
  return _mm256_add_epi8(sum, right2);
}

static inline void check_continuations(__m256i initial_lengths, __m256i carries,
                                       __m256i *has_error) {

  // overlap || underlap
  // carry > length && length > 0 || !(carry > length) && !(length > 0)
  // (carries > length) == (lengths > 0)
  __m256i overunder = _mm256_cmpeq_epi8(
      _mm256_cmpgt_epi8(carries, initial_lengths),
      _mm256_cmpgt_epi8(initial_lengths, _mm256_setzero_si256()));

  *has_error = _mm256_or_si256(*has_error, overunder);
}

// when 0xED is found, next byte must be no larger than 0x9F
// when 0xF4 is found, next byte must be no larger than 0x8F
// next byte must be continuation, ie sign bit is set, so signed < is ok
static inline void check_first_continuation_max(__m256i current_bytes,
                                                __m256i off1_current_bytes,
                                                __m256i *has_error) {
  __m256i maskED =
      _mm256_cmpeq_epi8(off1_current_bytes, _mm256_set1_epi8(char(0xED)));
  __m256i maskF4 =
      _mm256_cmpeq_epi8(off1_current_bytes, _mm256_set1_epi8(char(0xF4)));

  __m256i badfollowED = _mm256_and_si256(
      _mm256_cmpgt_epi8(current_bytes, _mm256_set1_epi8(char(0x9F))), maskED);
  __m256i badfollowF4 = _mm256_and_si256(
      _mm256_cmpgt_epi8(current_bytes, _mm256_set1_epi8(char(0x8F))), maskF4);

  *has_error =
      _mm256_or_si256(*has_error, _mm256_or_si256(badfollowED, badfollowF4));
}

// map off1_hibits => error condition
// hibits     off1    cur
// C       => < C2 && true
// E       => < E1 && < A0
// F       => < F1 && < 90
// else      false && false
static inline void check_overlong(__m256i current_bytes,
                                  __m256i off1_current_bytes, __m256i hibits,
                                  __m256i previous_hibits, __m256i *has_error) {
  __m256i off1_hibits = push_last_byte_of_a_to_b(previous_hibits, hibits);
  __m256i initial_mins = _mm256_shuffle_epi8(
      _mm256_setr_epi8(-128, -128, -128, -128, -128, -128, -128, -128, -128,
                       -128, -128, -128, // 10xx => false
                       char(0xC2), -128, // 110x
                       char(0xE1),       // 1110
                       char(0xF1), -128, -128, -128, -128, -128, -128, -128,
                       -128, -128, -128, -128, -128, // 10xx => false
                       char(0xC2), -128,             // 110x
                       char(0xE1),                   // 1110
                       char(0xF1)),
      off1_hibits);

  __m256i initial_under = _mm256_cmpgt_epi8(initial_mins, off1_current_bytes);

  __m256i second_mins = _mm256_shuffle_epi8(
      _mm256_setr_epi8(-128, -128, -128, -128, -128, -128, -128, -128, -128,
                       -128, -128, -128, // 10xx => false
                       127, 127,         // 110x => true
                       char(0xA0),       // 1110
                       char(0x90), -128, -128, -128, -128, -128, -128, -128,
                       -128, -128, -128, -128, -128, // 10xx => false
                       127, 127,                     // 110x => true
                       char(0xA0),                   // 1110
                       char(0x90)),
      off1_hibits);
  __m256i second_under = _mm256_cmpgt_epi8(second_mins, current_bytes);
  *has_error = _mm256_or_si256(*has_error,
                               _mm256_and_si256(initial_under, second_under));
}

struct avx_processed_utf_bytes {
  __m256i rawbytes;
  __m256i high_nibbles;
  __m256i carried_continuations;
};

static inline void avx_count_nibbles(__m256i bytes,
                                     struct avx_processed_utf_bytes *answer) {
  answer->rawbytes = bytes;
  answer->high_nibbles =
      _mm256_and_si256(_mm256_srli_epi16(bytes, 4), _mm256_set1_epi8(0x0F));
}

// check whether the current bytes are valid UTF-8
// at the end of the function, previous gets updated
static struct avx_processed_utf_bytes
check_utf8_bytes(__m256i current_bytes,
                 struct avx_processed_utf_bytes *previous, __m256i *has_error) {
  struct avx_processed_utf_bytes pb;
  avx_count_nibbles(current_bytes, &pb);

  check_smaller_than_0xf4(current_bytes, has_error);

  __m256i initial_lengths = continuation_lengths(pb.high_nibbles);

  pb.carried_continuations =
      carry_continuations(initial_lengths, previous->carried_continuations);

  check_continuations(initial_lengths, pb.carried_continuations, has_error);

  __m256i off1_current_bytes =
      push_last_byte_of_a_to_b(previous->rawbytes, pb.rawbytes);
  check_first_continuation_max(current_bytes, off1_current_bytes, has_error);

  check_overlong(current_bytes, off1_current_bytes, pb.high_nibbles,
                 previous->high_nibbles, has_error);
  return pb;
}

static inline bool is_utf8(const char *src, size_t len) {
  size_t i = 0;
  __m256i has_error = _mm256_setzero_si256();
  struct avx_processed_utf_bytes previous;
  previous.rawbytes = _mm256_setzero_si256();
  previous.high_nibbles = _mm256_setzero_si256();
  previous.carried_continuations = _mm256_setzero_si256();
  if (len >= 32) {
    for (; i <= len - 32; i += 32) {
      __m256i current_bytes = _mm256_loadu_si256((const __m256i *)(src + i));
      previous = check_utf8_bytes(current_bytes, &previous, &has_error);
    }
  }

  // last part
  if (i < len) {
    char buffer[32];
    memset(buffer, 0, 32);
    memcpy(buffer, src + i, len - i);
    __m256i current_bytes = _mm256_loadu_si256((const __m256i *)(buffer));
    previous = check_utf8_bytes(current_bytes, &previous, &has_error);
  } else {
    has_error = _mm256_or_si256(
        _mm256_cmpgt_epi8(previous.carried_continuations,
                          _mm256_setr_epi8(9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
                                           9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
                                           9, 9, 9, 9, 9, 9, 9, 1)),
        has_error);
  }

  return _mm256_testz_si256(has_error, has_error) ? true : false;
}
} // namespace is_utf8_avx2
SIMDUTF_UNTARGET_REGION
bool is_utf8(const char *src, size_t len) {
  typedef bool (*checker_function)(const char *, size_t);
  static checker_function fnc =
      is_utf8_core::which_kernel() == is_utf8_core::AVX2_KERNEL
          ? is_utf8_avx2::is_utf8
          : (is_utf8_core::which_kernel() == is_utf8_core::SSE_KERNEL
                 ? is_utf8_sse::is_utf8
                 : is_utf8_fallback::is_utf8);
  return fnc(src, len);
}

// #endif // IS_UTF8_IS_X86_64
#else
bool is_utf8(const char *src, size_t len) {
  return is_utf8_fallback::is_utf8(src, len);
}
#endif // IS_UTF8_IS_X86_64
