
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __aarch64__

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
      -128, -128, -128, -128, -128, -128,
      -128, -128, -128, -128, -128, -128, // 10xx => false
      0xC2, -128,                         // 110x
      0xE1,                               // 1110
      0xF1,
  };
  int8x16_t initial_mins = vqtbl1q_s8(vld1q_s8(initial_mins_array),
                                      vreinterpretq_u8_s8(off1_hibits));

  uint8x16_t initial_under = vcgtq_s8(initial_mins, off1_current_bytes);

  static const int8_t second_mins_array[] = {
      -128, -128, -128, -128, -128, -128,
      -128, -128, -128, -128, -128, -128, // 10xx => false
      127,  127,                          // 110x => true
      0xA0,                               // 1110
      0x90,
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

bool is_utf8(const unsigned char *src, size_t len) {
  size_t i = 0;
  int8x16_t has_error = vdupq_n_s8(0);
  struct processed_utf_bytes previous = {.rawbytes = vdupq_n_s8(0),
                                         .high_nibbles = vdupq_n_s8(0),
                                         .carried_continuations =
                                             vdupq_n_s8(0)};
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
#endif

#ifdef __x86_64__

#include <x86intrin.h>

namespace is_utf8_sse {
// all byte values must be no larger than 0xF4
static inline void check_smaller_than_0xf4(__m128i current_bytes,
                                           __m128i *has_error) {
  // unsigned, saturates to 0 below max
  *has_error = _mm_or_si128(*has_error,
                            _mm_subs_epu8(current_bytes, _mm_set1_epi8(0xF4)));
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
  __m128i maskED = _mm_cmpeq_epi8(off1_current_bytes, _mm_set1_epi8(0xED));
  __m128i maskF4 = _mm_cmpeq_epi8(off1_current_bytes, _mm_set1_epi8(0xF4));

  __m128i badfollowED =
      _mm_and_si128(_mm_cmpgt_epi8(current_bytes, _mm_set1_epi8(0x9F)), maskED);
  __m128i badfollowF4 =
      _mm_and_si128(_mm_cmpgt_epi8(current_bytes, _mm_set1_epi8(0x8F)), maskF4);

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
                    -128, -128, // 10xx => false
                    0xC2, -128, // 110x
                    0xE1,       // 1110
                    0xF1),
      off1_hibits);

  __m128i initial_under = _mm_cmpgt_epi8(initial_mins, off1_current_bytes);

  __m128i second_mins = _mm_shuffle_epi8(
      _mm_setr_epi8(-128, -128, -128, -128, -128, -128, -128, -128, -128, -128,
                    -128, -128, // 10xx => false
                    127, 127,   // 110x => true
                    0xA0,       // 1110
                    0x90),
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

bool is_utf8(const unsigned char *src, size_t len) {
  size_t i = 0;
  __m128i has_error = _mm_setzero_si128();
  struct processed_utf_bytes previous = {.rawbytes = _mm_setzero_si128(),
                                         .high_nibbles = _mm_setzero_si128(),
                                         .carried_continuations =
                                             _mm_setzero_si128()};
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

#endif

#ifdef __AVX2__

#include <x86intrin.h>

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
      *has_error, _mm256_subs_epu8(current_bytes, _mm256_set1_epi8(0xF4)));
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
static inline void \wheck_first_continuation_max(__m256i current_bytes,
                                                 __m256i off1_current_bytes,
                                                 __m256i *has_error) {
  __m256i maskED =
      _mm256_cmpeq_epi8(off1_current_bytes, _mm256_set1_epi8(0xED));
  __m256i maskF4 =
      _mm256_cmpeq_epi8(off1_current_bytes, _mm256_set1_epi8(0xF4));

  __m256i badfollowED = _mm256_and_si256(
      _mm256_cmpgt_epi8(current_bytes, _mm256_set1_epi8(0x9F)), maskED);
  __m256i badfollowF4 = _mm256_and_si256(
      _mm256_cmpgt_epi8(current_bytes, _mm256_set1_epi8(0x8F)), maskF4);

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
                       0xC2, -128,       // 110x
                       0xE1,             // 1110
                       0xF1, -128, -128, -128, -128, -128, -128, -128, -128,
                       -128, -128, -128, -128, // 10xx => false
                       0xC2, -128,             // 110x
                       0xE1,                   // 1110
                       0xF1),
      off1_hibits);

  __m256i initial_under = _mm256_cmpgt_epi8(initial_mins, off1_current_bytes);

  __m256i second_mins = _mm256_shuffle_epi8(
      _mm256_setr_epi8(-128, -128, -128, -128, -128, -128, -128, -128, -128,
                       -128, -128, -128, // 10xx => false
                       127, 127,         // 110x => true
                       0xA0,             // 1110
                       0x90, -128, -128, -128, -128, -128, -128, -128, -128,
                       -128, -128, -128, -128, // 10xx => false
                       127, 127,               // 110x => true
                       0xA0,                   // 1110
                       0x90),
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

int is_utf8(const unsigned char *src, size_t len) {
  size_t i = 0;
  __m256i has_error = _mm256_setzero_si256();
  struct avx_processed_utf_bytes previous = {
      .rawbytes = _mm256_setzero_si256(),
      .high_nibbles = _mm256_setzero_si256(),
      .carried_continuations = _mm256_setzero_si256()};
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

#endif
