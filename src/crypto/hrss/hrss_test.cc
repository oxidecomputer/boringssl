/* Copyright (c) 2018, Google Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

#include <gtest/gtest.h>

#include <openssl/cpu.h>
#include <openssl/hrss.h>
#include <openssl/rand.h>

#include "../test/abi_test.h"
#include "../test/test_util.h"
#include "internal.h"

// poly2_from_bits takes the least-significant bit from each byte of |in| and
// sets the bits of |*out| to match.
static void poly2_from_bits(struct poly2 *out, const uint8_t in[N]) {
  crypto_word_t *words = out->v;
  unsigned shift = 0;
  crypto_word_t word = 0;

  for (unsigned i = 0; i < N; i++) {
    word >>= 1;
    word |= (crypto_word_t)(in[i] & 1) << (BITS_PER_WORD - 1);
    shift++;

    if (shift == BITS_PER_WORD) {
      *words = word;
      words++;
      word = 0;
      shift = 0;
    }
  }

  word >>= BITS_PER_WORD - shift;
  *words = word;
}

TEST(HRSS, Poly2RotateRight) {
  uint8_t bits[N];
  RAND_bytes(bits, sizeof(bits));
  for (size_t i = 0; i < N; i++) {
    bits[i] &= 1;
  };

  struct poly2 p, orig, shifted;
  poly2_from_bits(&p, bits);
  OPENSSL_memcpy(&orig, &p, sizeof(orig));

  // Test |HRSS_poly2_rotr_consttime| by manually rotating |bits| step-by-step
  // and testing every possible shift to ensure that it produces the correct
  // answer.
  for (size_t shift = 0; shift <= N; shift++) {
    SCOPED_TRACE(shift);

    OPENSSL_memcpy(&p, &orig, sizeof(orig));
    HRSS_poly2_rotr_consttime(&p, shift);
    poly2_from_bits(&shifted, bits);
    ASSERT_EQ(
        Bytes(reinterpret_cast<const uint8_t *>(&shifted), sizeof(shifted)),
        Bytes(reinterpret_cast<const uint8_t *>(&p), sizeof(p)));

    const uint8_t least_significant_bit = bits[0];
    OPENSSL_memmove(bits, &bits[1], N-1);
    bits[N-1] = least_significant_bit;
  }
}

// poly3_rand sets |r| to a random value (albeit with bias).
static void poly3_rand(poly3 *p) {
  RAND_bytes(reinterpret_cast<uint8_t *>(p), sizeof(poly3));
  p->s.v[WORDS_PER_POLY - 1] &= (UINT64_C(1) << BITS_IN_LAST_WORD) - 1;
  p->a.v[WORDS_PER_POLY - 1] &= (UINT64_C(1) << BITS_IN_LAST_WORD) - 1;
  // (s, a) = (1, 0) is invalid. Map those to -1.
  for (size_t j = 0; j < WORDS_PER_POLY; j++) {
    p->a.v[j] |= p->s.v[j];
  }
}

// poly3_word_add sets (|s1|, |a1|) += (|s2|, |a2|).
static void poly3_word_add(crypto_word_t *s1, crypto_word_t *a1,
                           const crypto_word_t s2, const crypto_word_t a2) {
  const crypto_word_t t = *s1 ^ a2;
  *s1 = t & (s2 ^ *a1);
  *a1 = (*a1 ^ a2) | (t ^ s2);
}

TEST(HRSS, Poly3Invert) {
  poly3 p, inverse, result;
  memset(&p, 0, sizeof(p));
  memset(&inverse, 0, sizeof(inverse));
  memset(&result, 0, sizeof(result));

  // The inverse of -1 is -1.
  p.s.v[0] = 1;
  p.a.v[0] = 1;
  HRSS_poly3_invert(&inverse, &p);
  EXPECT_EQ(Bytes(reinterpret_cast<const uint8_t*>(&p), sizeof(p)),
            Bytes(reinterpret_cast<const uint8_t*>(&inverse), sizeof(inverse)));

  // The inverse of 1 is 1.
  p.s.v[0] = 0;
  p.a.v[0] = 1;
  HRSS_poly3_invert(&inverse, &p);
  EXPECT_EQ(Bytes(reinterpret_cast<const uint8_t*>(&p), sizeof(p)),
            Bytes(reinterpret_cast<const uint8_t*>(&inverse), sizeof(inverse)));

  for (size_t i = 0; i < 500; i++) {
    poly3 r;
    poly3_rand(&r);
    HRSS_poly3_invert(&inverse, &r);
    HRSS_poly3_mul(&result, &inverse, &r);
    // r×r⁻¹ = 1, and |p| contains 1.
    EXPECT_EQ(
        Bytes(reinterpret_cast<const uint8_t *>(&p), sizeof(p)),
        Bytes(reinterpret_cast<const uint8_t *>(&result), sizeof(result)));
  }
}

TEST(HRSS, Poly3UnreducedInput) {
  // Check that |poly3_mul| works correctly with inputs that aren't reduced mod
  // Φ(N).
  poly3 r, inverse, result, one;
  poly3_rand(&r);
  HRSS_poly3_invert(&inverse, &r);
  HRSS_poly3_mul(&result, &inverse, &r);

  memset(&one, 0, sizeof(one));
  one.a.v[0] = 1;
  EXPECT_EQ(Bytes(reinterpret_cast<const uint8_t *>(&one), sizeof(one)),
            Bytes(reinterpret_cast<const uint8_t *>(&result), sizeof(result)));

  // |r| is probably already not reduced mod Φ(N), but add x^701 - 1 and
  // recompute to ensure that we get the same answer. (Since (x^701 - 1) ≡ 0 mod
  // Φ(N).)
  poly3_word_add(&r.s.v[0], &r.a.v[0], 1, 1);
  poly3_word_add(&r.s.v[WORDS_PER_POLY - 1], &r.a.v[WORDS_PER_POLY - 1], 0,
                 UINT64_C(1) << BITS_IN_LAST_WORD);

  HRSS_poly3_mul(&result, &inverse, &r);
  EXPECT_EQ(Bytes(reinterpret_cast<const uint8_t *>(&one), sizeof(one)),
            Bytes(reinterpret_cast<const uint8_t *>(&result), sizeof(result)));

  // Check that x^700 × 1 gives -x^699 - x^698 … -1.
  poly3 x700;
  memset(&x700, 0, sizeof(x700));
  x700.a.v[WORDS_PER_POLY-1] = UINT64_C(1) << (BITS_IN_LAST_WORD - 1);
  HRSS_poly3_mul(&result, &one, &x700);

  for (size_t i = 0; i < WORDS_PER_POLY-1; i++) {
    EXPECT_EQ(CONSTTIME_TRUE_W, result.s.v[i]);
    EXPECT_EQ(CONSTTIME_TRUE_W, result.a.v[i]);
  }
  EXPECT_EQ((UINT64_C(1) << (BITS_IN_LAST_WORD - 1)) - 1,
            result.s.v[WORDS_PER_POLY - 1]);
  EXPECT_EQ(result.s.v[WORDS_PER_POLY - 1], result.a.v[WORDS_PER_POLY - 1]);
}

TEST(HRSS, Basic) {
  uint8_t generate_key_entropy[HRSS_GENERATE_KEY_BYTES];
  for (unsigned i = 0; i < sizeof(generate_key_entropy); i++) {
    generate_key_entropy[i] = i;
  }

  HRSS_public_key pub;
  HRSS_private_key priv;
  HRSS_generate_key(&pub, &priv, generate_key_entropy);

  uint8_t encap_entropy[HRSS_ENCAP_BYTES];
  for (unsigned i = 0; i < sizeof(encap_entropy); i++) {
    encap_entropy[i] = i;
  }

  HRSS_public_key pub2;
  uint8_t pub_bytes[HRSS_PUBLIC_KEY_BYTES];
  HRSS_marshal_public_key(pub_bytes, &pub);
  ASSERT_TRUE(HRSS_parse_public_key(&pub2, pub_bytes));

  uint8_t ciphertext[HRSS_CIPHERTEXT_BYTES];
  uint8_t shared_key[HRSS_KEY_BYTES];
  HRSS_encap(ciphertext, shared_key, &pub2, encap_entropy);

  uint8_t shared_key2[HRSS_KEY_BYTES];
  HRSS_decap(shared_key2, &priv, ciphertext, sizeof(ciphertext));

  EXPECT_EQ(Bytes(shared_key), Bytes(shared_key2));
}

TEST(HRSS, Random) {
  for (unsigned i = 0; i < 10; i++) {
    uint8_t generate_key_entropy[HRSS_GENERATE_KEY_BYTES];
    RAND_bytes(generate_key_entropy, sizeof(generate_key_entropy));
    SCOPED_TRACE(Bytes(generate_key_entropy));

    HRSS_public_key pub;
    HRSS_private_key priv;
    HRSS_generate_key(&pub, &priv, generate_key_entropy);

    for (unsigned j = 0; j < 10; j++) {
      uint8_t encap_entropy[HRSS_ENCAP_BYTES];
      RAND_bytes(encap_entropy, sizeof(encap_entropy));
      SCOPED_TRACE(Bytes(encap_entropy));

      uint8_t ciphertext[HRSS_CIPHERTEXT_BYTES];
      uint8_t shared_key[HRSS_KEY_BYTES];
      HRSS_encap(ciphertext, shared_key, &pub, encap_entropy);

      uint8_t shared_key2[HRSS_KEY_BYTES];
      HRSS_decap(shared_key2, &priv, ciphertext, sizeof(ciphertext));
      EXPECT_EQ(Bytes(shared_key), Bytes(shared_key2));

      uint32_t offset;
      RAND_bytes((uint8_t*) &offset, sizeof(offset));
      uint8_t bit;
      RAND_bytes(&bit, sizeof(bit));
      ciphertext[offset % sizeof(ciphertext)] ^= (1 << (bit & 7));
      HRSS_decap(shared_key2, &priv, ciphertext, sizeof(ciphertext));
      EXPECT_NE(Bytes(shared_key), Bytes(shared_key2));
    }
  }
}

TEST(HRSS, Golden) {
  uint8_t generate_key_entropy[HRSS_GENERATE_KEY_BYTES];
  for (unsigned i = 0; i < HRSS_SAMPLE_BYTES; i++) {
    generate_key_entropy[i] = i;
  }
  for (unsigned i = HRSS_SAMPLE_BYTES; i < 2 * HRSS_SAMPLE_BYTES; i++) {
    generate_key_entropy[i] = 2 + i;
  }
  for (unsigned i = 2 * HRSS_SAMPLE_BYTES; i < sizeof(generate_key_entropy);
       i++) {
    generate_key_entropy[i] = 4 + i;
  }

  HRSS_public_key pub;
  HRSS_private_key priv;
  OPENSSL_memset(&pub, 0, sizeof(pub));
  OPENSSL_memset(&priv, 0, sizeof(priv));
  HRSS_generate_key(&pub, &priv, generate_key_entropy);

  static const uint8_t kExpectedPub[HRSS_PUBLIC_KEY_BYTES] = {
      0xf8, 0x9f, 0xa0, 0xfc, 0xf1, 0xd4, 0xfa, 0x4d, 0x8f, 0x35, 0x28, 0x73,
      0x0e, 0x37, 0x18, 0x1d, 0x09, 0xf3, 0x9e, 0x16, 0x0d, 0x7f, 0x9c, 0x82,
      0x17, 0xa1, 0xa1, 0x88, 0x6b, 0x29, 0x5b, 0x3a, 0x30, 0xcd, 0x6f, 0x8e,
      0x0c, 0xd3, 0x38, 0x0c, 0x05, 0x68, 0x6e, 0x4c, 0xcc, 0x20, 0xd4, 0x06,
      0x77, 0x0c, 0xac, 0x1c, 0x49, 0x14, 0x00, 0xd6, 0x9b, 0x1c, 0xde, 0x43,
      0x0a, 0x59, 0x37, 0xd6, 0x46, 0x68, 0x1f, 0x04, 0xcb, 0x73, 0x92, 0x37,
      0x2d, 0x7f, 0x57, 0x70, 0x16, 0xe8, 0x06, 0x48, 0x3b, 0x66, 0xb3, 0x63,
      0x02, 0x5a, 0x71, 0x46, 0xdd, 0xa4, 0xee, 0xb8, 0x78, 0x44, 0xfd, 0x9e,
      0xd0, 0x71, 0x16, 0x00, 0xbd, 0x01, 0x1e, 0x27, 0x2e, 0xa0, 0xc6, 0x8d,
      0x55, 0x89, 0x7c, 0x2a, 0x01, 0x2b, 0x1b, 0x75, 0xa2, 0xc2, 0xd1, 0x5a,
      0x67, 0xfa, 0xdd, 0x3b, 0x70, 0x9d, 0xdb, 0xcd, 0x73, 0x32, 0x5e, 0x24,
      0xb1, 0xcf, 0x23, 0xbe, 0x3c, 0x56, 0xcc, 0xbe, 0x61, 0xdb, 0xe7, 0x3c,
      0xc7, 0xf5, 0x09, 0xe6, 0x87, 0xa0, 0x09, 0x52, 0x9d, 0x61, 0x5b, 0xc6,
      0xd4, 0xc5, 0x2e, 0xc2, 0x6c, 0x87, 0x30, 0x36, 0x49, 0x6f, 0x04, 0xaa,
      0xb3, 0x26, 0xd5, 0x63, 0xcf, 0xd4, 0x74, 0x1e, 0xc7, 0x79, 0xb3, 0xfc,
      0x8c, 0x41, 0x36, 0x79, 0xaa, 0xd5, 0xba, 0x64, 0x49, 0x48, 0xdb, 0xeb,
      0xe8, 0x33, 0x7d, 0xbe, 0x3b, 0x67, 0xd7, 0xfd, 0x93, 0x1e, 0x80, 0x8d,
      0x17, 0xab, 0x6f, 0xfd, 0x1c, 0x4b, 0x2d, 0x5b, 0x90, 0xf0, 0xf0, 0x5d,
      0xbe, 0x8f, 0x81, 0x18, 0x29, 0x08, 0x9a, 0x47, 0x1b, 0xc2, 0x2d, 0xa2,
      0x22, 0x5a, 0x4f, 0xe9, 0x81, 0x64, 0xdd, 0x53, 0x2e, 0x67, 0xe5, 0x07,
      0x1a, 0xf0, 0x0c, 0x54, 0x9b, 0xe2, 0xf8, 0xe6, 0xb3, 0xb6, 0xe0, 0x5a,
      0x74, 0xfa, 0x8d, 0x9c, 0xa5, 0x7c, 0x6e, 0x73, 0xba, 0xee, 0x6e, 0x6e,
      0x31, 0xcb, 0x59, 0xd7, 0xfd, 0x94, 0x1c, 0x4d, 0x62, 0xc6, 0x87, 0x0b,
      0x38, 0x54, 0xc6, 0x35, 0xac, 0xc8, 0x8c, 0xc0, 0xd9, 0x99, 0xee, 0xfc,
      0xa9, 0xde, 0xc4, 0x50, 0x88, 0x8e, 0x24, 0xf6, 0xd6, 0x04, 0x54, 0x3e,
      0x81, 0xc4, 0x96, 0x9a, 0x40, 0xe5, 0xef, 0x8b, 0xec, 0x41, 0x50, 0x1d,
      0x14, 0xae, 0xa4, 0x5a, 0xac, 0xd4, 0x73, 0x31, 0xc3, 0x1d, 0xc1, 0x96,
      0x89, 0xd8, 0x62, 0x97, 0x60, 0x3f, 0x58, 0x2a, 0x5f, 0xcf, 0xcb, 0x26,
      0x99, 0x69, 0x81, 0x13, 0x9c, 0xaf, 0x17, 0x91, 0xa8, 0xeb, 0x9a, 0xf9,
      0xd3, 0x83, 0x47, 0x66, 0xc7, 0xf8, 0xd8, 0xe3, 0xd2, 0x7e, 0x58, 0xa9,
      0xf5, 0xb2, 0x03, 0xbe, 0x7e, 0xa5, 0x29, 0x9d, 0xff, 0xd1, 0xd8, 0x55,
      0x39, 0xc7, 0x2c, 0xce, 0x03, 0x64, 0xdc, 0x18, 0xe7, 0xb0, 0x60, 0x46,
      0x26, 0xeb, 0xb7, 0x61, 0x4b, 0x91, 0x2c, 0xd8, 0xa2, 0xee, 0x63, 0x2e,
      0x15, 0x0a, 0x58, 0x88, 0x04, 0xb1, 0xed, 0x6d, 0xf1, 0x5c, 0xc7, 0xee,
      0x60, 0x38, 0x26, 0xc9, 0x31, 0x7e, 0x69, 0xe4, 0xac, 0x3c, 0x72, 0x09,
      0x3e, 0xe6, 0x24, 0x30, 0x44, 0x6e, 0x66, 0x83, 0xb9, 0x2a, 0x22, 0xaf,
      0x26, 0x1e, 0xaa, 0xa3, 0xf4, 0xb1, 0xa1, 0x5c, 0xfa, 0x5f, 0x0d, 0x71,
      0xac, 0xe3, 0xe0, 0xc3, 0xdd, 0x4f, 0x96, 0x57, 0x8b, 0x58, 0xac, 0xe3,
      0x42, 0x8e, 0x47, 0x72, 0xb1, 0xe4, 0x19, 0x68, 0x3e, 0xbb, 0x19, 0x14,
      0xdf, 0x16, 0xb5, 0xde, 0x7f, 0x37, 0xaf, 0xd8, 0xd3, 0x3d, 0x6a, 0x16,
      0x1b, 0x26, 0xd3, 0xcc, 0x53, 0x82, 0x57, 0x90, 0x89, 0xc5, 0x7e, 0x6d,
      0x7e, 0x99, 0x5b, 0xcd, 0xd3, 0x18, 0xbb, 0x89, 0xef, 0x76, 0xbd, 0xd2,
      0x62, 0xf0, 0xe8, 0x25, 0x2a, 0x8d, 0xe2, 0x21, 0xea, 0xde, 0x6e, 0xa5,
      0xa4, 0x3d, 0x58, 0xee, 0xdf, 0x90, 0xc1, 0xa1, 0x38, 0x5d, 0x11, 0x50,
      0xb5, 0xac, 0x9d, 0xb4, 0xfd, 0xef, 0x53, 0xe8, 0xc0, 0x17, 0x6c, 0x4f,
      0x31, 0xe0, 0xcc, 0x8f, 0x80, 0x7a, 0x84, 0x14, 0xde, 0xee, 0xec, 0xdd,
      0x6a, 0xad, 0x29, 0x65, 0xa5, 0x72, 0xc3, 0x73, 0x5f, 0xe3, 0x6f, 0x60,
      0xb1, 0xfb, 0x0f, 0xaa, 0xc6, 0xda, 0x53, 0x4a, 0xb1, 0x92, 0x2a, 0xb7,
      0x02, 0xbe, 0xf9, 0xdf, 0x37, 0x16, 0xe7, 0x5c, 0x38, 0x0b, 0x3c, 0xe2,
      0xdd, 0x90, 0xb8, 0x7b, 0x48, 0x69, 0x79, 0x81, 0xc5, 0xae, 0x9a, 0x0d,
      0x78, 0x95, 0x52, 0x63, 0x80, 0xda, 0x46, 0x69, 0x20, 0x57, 0x9b, 0x27,
      0xe2, 0xe8, 0xbd, 0x2f, 0x45, 0xe6, 0x46, 0x40, 0xae, 0x50, 0xd5, 0xa2,
      0x53, 0x93, 0xe1, 0x99, 0xfd, 0x13, 0x7c, 0xf6, 0x22, 0xc4, 0x6c, 0xab,
      0xe3, 0xc9, 0x55, 0x0a, 0x16, 0x67, 0x68, 0x26, 0x6b, 0xd6, 0x7d, 0xde,
      0xd3, 0xae, 0x71, 0x32, 0x02, 0xf1, 0x27, 0x67, 0x47, 0x74, 0xd9, 0x40,
      0x35, 0x1d, 0x25, 0x72, 0x32, 0xdf, 0x75, 0xd5, 0x60, 0x26, 0xab, 0x90,
      0xfa, 0xeb, 0x26, 0x11, 0x4b, 0xb4, 0xc5, 0xc2, 0x3e, 0xa9, 0x23, 0x3a,
      0x4e, 0x6a, 0xb1, 0xbb, 0xb3, 0xea, 0xf9, 0x1e, 0xe4, 0x10, 0xf5, 0xdc,
      0x35, 0xde, 0xb5, 0xee, 0xf0, 0xde, 0xa1, 0x18, 0x80, 0xc7, 0x13, 0x68,
      0x46, 0x94, 0x0e, 0x2a, 0x8e, 0xf8, 0xe9, 0x26, 0x84, 0x42, 0x0f, 0x56,
      0xed, 0x67, 0x7f, 0xeb, 0x7d, 0x35, 0x07, 0x01, 0x11, 0x81, 0x8b, 0x56,
      0x88, 0xc6, 0x58, 0x61, 0x65, 0x3c, 0x5d, 0x9c, 0x58, 0x25, 0xd6, 0xdf,
      0x4e, 0x3b, 0x93, 0xbf, 0x82, 0xe1, 0x19, 0xb8, 0xda, 0xde, 0x26, 0x38,
      0xf2, 0xd9, 0x95, 0x24, 0x98, 0xde, 0x58, 0xf7, 0x0c, 0xe9, 0x32, 0xbb,
      0xcc, 0xf7, 0x92, 0x69, 0xa2, 0xf0, 0xc3, 0xfa, 0xd2, 0x31, 0x8b, 0x43,
      0x4e, 0x03, 0xe2, 0x13, 0x79, 0x6e, 0x73, 0x63, 0x3b, 0x45, 0xde, 0x80,
      0xf4, 0x26, 0xb1, 0x38, 0xed, 0x62, 0x55, 0xc6, 0x6a, 0x67, 0x00, 0x2d,
      0xba, 0xb2, 0xc5, 0xb6, 0x97, 0x62, 0x28, 0x64, 0x30, 0xb9, 0xfb, 0x3f,
      0x94, 0x03, 0x48, 0x36, 0x2c, 0x5d, 0xfd, 0x08, 0x96, 0x40, 0xd1, 0x6c,
      0xe5, 0xd0, 0xf8, 0x99, 0x40, 0x82, 0x87, 0xd7, 0xdc, 0x2f, 0x8b, 0xaa,
      0x31, 0x96, 0x0a, 0x34, 0x33, 0xa6, 0xf1, 0x84, 0x6e, 0x33, 0x73, 0xc5,
      0xe3, 0x26, 0xad, 0xd0, 0xcb, 0x62, 0x71, 0x82, 0xab, 0xd1, 0x82, 0x33,
      0xe6, 0xca, 0xd0, 0x3e, 0xf5, 0x4d, 0x12, 0x6e, 0xf1, 0x83, 0xbd, 0xdc,
      0x4d, 0xdf, 0x49, 0xbc, 0x63, 0xae, 0x7e, 0x59, 0xe8, 0x3c, 0x0d, 0xd6,
      0x1d, 0x41, 0x89, 0x72, 0x52, 0xc0, 0xae, 0xd1, 0x2f, 0x0a, 0x8a, 0xce,
      0x26, 0xd0, 0x3e, 0x0c, 0x71, 0x32, 0x52, 0xb2, 0xe4, 0xee, 0xa2, 0xe5,
      0x28, 0xb6, 0x33, 0x69, 0x97, 0x5a, 0x53, 0xdb, 0x56, 0x63, 0xe9, 0xb3,
      0x6d, 0x60, 0xf4, 0x7a, 0xce, 0xec, 0x36, 0x65, 0xd5, 0xca, 0x63, 0x2a,
      0x19, 0x90, 0x14, 0x7b, 0x02, 0x33, 0xfa, 0x11, 0x58, 0x5a, 0xd9, 0xc5,
      0x54, 0xf3, 0x28, 0xd5, 0x6e, 0xea, 0x85, 0xf5, 0x09, 0xbb, 0x81, 0x44,
      0x1c, 0x63, 0x66, 0x81, 0xc5, 0x96, 0x2d, 0x7c, 0x0e, 0x75, 0x7b, 0xb4,
      0x7e, 0x4e, 0x0c, 0xfd, 0x3c, 0xc5, 0x5a, 0x22, 0x85, 0x5c, 0xc8, 0xf3,
      0x97, 0x98, 0x2c, 0xe9, 0x46, 0xb4, 0x02, 0xcf, 0x7d, 0xa4, 0xf2, 0x44,
      0x7a, 0x89, 0x71, 0xa0, 0xfa, 0xb6, 0xa3, 0xaf, 0x13, 0x25, 0x46, 0xe2,
      0x64, 0xe3, 0x69, 0xba, 0xf9, 0x68, 0x5c, 0xc0, 0xb7, 0xa8, 0xa6, 0x4b,
      0xe1, 0x42, 0xe9, 0xb5, 0xc7, 0x84, 0xbb, 0xa6, 0x4b, 0x10, 0x4e, 0xd4,
      0x68, 0x70, 0x0a, 0x75, 0x2a, 0xbb, 0x9d, 0xa0, 0xcb, 0xf0, 0x36, 0x4c,
      0x70, 0x6c, 0x60, 0x4d, 0xfe, 0xe8, 0xc8, 0x66, 0x80, 0x1b, 0xf7, 0xcc,
      0x1a, 0xdd, 0x6b, 0xa7, 0xa7, 0x25, 0x61, 0x0c, 0x31, 0xf0, 0x34, 0x63,
      0x00, 0x0e, 0x48, 0x6a, 0x5a, 0x8d, 0x47, 0x94, 0x3f, 0x14, 0x16, 0xa8,
      0x8a, 0x49, 0xbb, 0x0c, 0x43, 0x21, 0xda, 0xf2, 0xc5, 0xd0, 0xff, 0x19,
      0x3e, 0x36, 0x64, 0x20, 0xb3, 0x70, 0xae, 0x54, 0xca, 0x73, 0x05, 0x56,
      0x7a, 0x49, 0x45, 0xe9, 0x46, 0xbc, 0xc2, 0x61, 0x70, 0x40, 0x7c, 0xb0,
      0xf7, 0xea, 0xc0, 0xd1, 0xb0, 0x77, 0x2c, 0xc7, 0xdd, 0x88, 0xcb, 0x9d,
      0xea, 0x55, 0x6c, 0x5c, 0x28, 0xb8, 0x84, 0x1c, 0x2c, 0x06,
  };
  uint8_t pub_bytes[HRSS_PUBLIC_KEY_BYTES];
  HRSS_marshal_public_key(pub_bytes, &pub);
  EXPECT_EQ(Bytes(pub_bytes), Bytes(kExpectedPub));

  uint8_t ciphertext[HRSS_CIPHERTEXT_BYTES];
  uint8_t shared_key[HRSS_KEY_BYTES];
  OPENSSL_STATIC_ASSERT(
      sizeof(kExpectedPub) >= HRSS_ENCAP_BYTES,
      "Private key too small to use as input to HRSS encapsulation");
  HRSS_encap(ciphertext, shared_key, &pub, kExpectedPub);

  static const uint8_t kExpectedCiphertext[HRSS_CIPHERTEXT_BYTES] = {
      0x8e, 0x6b, 0x46, 0x9d, 0x4a, 0xef, 0xa6, 0x8c, 0x28, 0x7b, 0xec, 0x6f,
      0x13, 0x2d, 0x7f, 0x6c, 0xca, 0x7d, 0x9e, 0x6b, 0x54, 0x62, 0xa3, 0x13,
      0xe1, 0x1e, 0x8f, 0x5f, 0x71, 0x67, 0xc4, 0x85, 0xdf, 0xd5, 0x6b, 0xbd,
      0x86, 0x0f, 0x98, 0xec, 0xa5, 0x04, 0xf7, 0x7b, 0x2a, 0xbe, 0xcb, 0xac,
      0x29, 0xbe, 0xe1, 0x0f, 0xbc, 0x62, 0x87, 0x85, 0x7f, 0x05, 0xae, 0xe4,
      0x3f, 0x87, 0xfc, 0x1f, 0xf7, 0x45, 0x1e, 0xa3, 0xdb, 0xb1, 0xa0, 0x25,
      0xba, 0x82, 0xec, 0xca, 0x8d, 0xab, 0x7a, 0x20, 0x03, 0xeb, 0xe5, 0x5c,
      0x9f, 0xd0, 0x46, 0x78, 0xf1, 0x5a, 0xc7, 0x9e, 0xb4, 0x10, 0x6d, 0x37,
      0xc0, 0x75, 0x08, 0xfb, 0xeb, 0xcb, 0xd8, 0x35, 0x21, 0x9b, 0x89, 0xa0,
      0xaa, 0x87, 0x00, 0x66, 0x38, 0x37, 0x68, 0xa4, 0xa3, 0x93, 0x8e, 0x2b,
      0xca, 0xf7, 0x7a, 0x43, 0xb2, 0x15, 0x79, 0x81, 0xce, 0xa9, 0x09, 0xcb,
      0x29, 0xd4, 0xcc, 0xef, 0xf1, 0x9b, 0xbd, 0xe6, 0x63, 0xd5, 0x26, 0x0f,
      0xe8, 0x8b, 0xdf, 0xf1, 0xc3, 0xb4, 0x18, 0x0e, 0xf2, 0x1d, 0x5d, 0x82,
      0x9b, 0x1f, 0xf3, 0xca, 0x36, 0x2a, 0x26, 0x0a, 0x7f, 0xc4, 0x0d, 0xbd,
      0x5b, 0x15, 0x1c, 0x18, 0x6c, 0x11, 0x4e, 0xec, 0x36, 0x01, 0xc1, 0x15,
      0xab, 0xf7, 0x0b, 0x1a, 0xd3, 0xa1, 0xbd, 0x68, 0xc8, 0x59, 0xe7, 0x49,
      0x5c, 0xd5, 0x4b, 0x8c, 0x31, 0xdb, 0xb3, 0xea, 0x88, 0x09, 0x2f, 0xb9,
      0x8b, 0xfd, 0x96, 0x35, 0x88, 0x53, 0x72, 0x40, 0xcd, 0x89, 0x75, 0xb4,
      0x20, 0xf6, 0xf6, 0xe5, 0x74, 0x19, 0x48, 0xaf, 0x4b, 0xaa, 0x42, 0xa4,
      0xc8, 0x90, 0xee, 0xf3, 0x12, 0x04, 0x63, 0x90, 0x92, 0x8a, 0x89, 0xc3,
      0xa0, 0x7e, 0xfe, 0x19, 0xb3, 0x54, 0x53, 0x83, 0xe9, 0xc1, 0x6c, 0xe3,
      0x97, 0xa6, 0x27, 0xc3, 0x20, 0x9a, 0x79, 0x35, 0xc9, 0xb5, 0xc0, 0x90,
      0xe1, 0x56, 0x84, 0x69, 0xc2, 0x54, 0x77, 0x52, 0x48, 0x55, 0x71, 0x3e,
      0xcd, 0xa7, 0xd6, 0x25, 0x5d, 0x49, 0x13, 0xd2, 0x59, 0xd7, 0xe1, 0xd1,
      0x70, 0x46, 0xa0, 0xd4, 0xee, 0x59, 0x13, 0x1f, 0x1a, 0xd3, 0x39, 0x7d,
      0xb0, 0x79, 0xf7, 0xc0, 0x73, 0x5e, 0xbb, 0x08, 0xf7, 0x5c, 0xb0, 0x31,
      0x41, 0x3d, 0x7b, 0x1e, 0xf0, 0xe6, 0x47, 0x5c, 0x37, 0xd5, 0x54, 0xf1,
      0xbb, 0x64, 0xd7, 0x41, 0x8b, 0x34, 0x55, 0xaa, 0xc3, 0x5a, 0x9c, 0xa0,
      0xcc, 0x29, 0x8e, 0x5a, 0x1a, 0x93, 0x5a, 0x49, 0xd3, 0xd0, 0xa0, 0x56,
      0xda, 0x32, 0xa2, 0xa9, 0xa7, 0x13, 0x42, 0x93, 0x9b, 0x20, 0x32, 0x37,
      0x5c, 0x3e, 0x03, 0xa5, 0x28, 0x10, 0x93, 0xdd, 0xa0, 0x04, 0x7b, 0x2a,
      0xbd, 0x31, 0xc3, 0x6a, 0x89, 0x58, 0x6e, 0x55, 0x0e, 0xc9, 0x5c, 0x70,
      0x07, 0x10, 0xf1, 0x9a, 0xbd, 0xfb, 0xd2, 0xb7, 0x94, 0x5b, 0x4f, 0x8d,
      0x90, 0xfa, 0xee, 0xae, 0x37, 0x48, 0xc5, 0xf8, 0x16, 0xa1, 0x3b, 0x70,
      0x03, 0x1f, 0x0e, 0xb8, 0xbd, 0x8d, 0x30, 0x4f, 0x95, 0x31, 0x0b, 0x9f,
      0xfc, 0x80, 0xf8, 0xef, 0xa3, 0x3c, 0xbc, 0xe2, 0x23, 0x23, 0x3e, 0x2a,
      0x55, 0x11, 0xe8, 0x2c, 0x17, 0xea, 0x1c, 0xbd, 0x1d, 0x2d, 0x1b, 0xd5,
      0x16, 0x9e, 0x05, 0xfc, 0x89, 0x64, 0x50, 0x4d, 0x9a, 0x22, 0x50, 0xc6,
      0x5a, 0xd9, 0x58, 0x99, 0x8f, 0xbd, 0xf2, 0x4f, 0x2c, 0xdb, 0x51, 0x6a,
      0x86, 0xe2, 0xc6, 0x64, 0x8f, 0x54, 0x1a, 0xf2, 0xcb, 0x34, 0x88, 0x08,
      0xbd, 0x2a, 0x8f, 0xec, 0x29, 0xf5, 0x22, 0x36, 0x83, 0x99, 0xb9, 0x71,
      0x8c, 0x99, 0x5c, 0xec, 0x91, 0x78, 0xc1, 0xe2, 0x2d, 0xe9, 0xd1, 0x4d,
      0xf5, 0x15, 0x93, 0x4d, 0x93, 0x92, 0x9f, 0x0f, 0x33, 0x5e, 0xcd, 0x58,
      0x5f, 0x3d, 0x52, 0xb9, 0x38, 0x6a, 0x85, 0x63, 0x8b, 0x63, 0x29, 0xcb,
      0x67, 0x12, 0x25, 0xc2, 0x44, 0xd7, 0xab, 0x1a, 0x24, 0xca, 0x3d, 0xca,
      0x77, 0xce, 0x28, 0x68, 0x1a, 0x91, 0xed, 0x7b, 0xc9, 0x70, 0x84, 0xab,
      0xe2, 0xd4, 0xf4, 0xac, 0x58, 0xf6, 0x70, 0x99, 0xfc, 0x99, 0x4d, 0xbd,
      0xb4, 0x1b, 0x4f, 0x15, 0x86, 0x95, 0x08, 0xd1, 0x4e, 0x73, 0xa9, 0xbc,
      0x6a, 0x8c, 0xbc, 0xb5, 0x4b, 0xe0, 0xee, 0x35, 0x24, 0xf9, 0x12, 0xf5,
      0x88, 0x70, 0x50, 0x6c, 0xfe, 0x0d, 0x35, 0xbd, 0xf7, 0xc4, 0x2e, 0x39,
      0x16, 0x30, 0x6c, 0xf3, 0xb2, 0x19, 0x44, 0xaa, 0xcb, 0x4a, 0xf6, 0x75,
      0xb7, 0x09, 0xb9, 0xe1, 0x47, 0x71, 0x70, 0x5c, 0x05, 0x5f, 0x50, 0x50,
      0x9c, 0xd0, 0xe3, 0xc7, 0x91, 0xee, 0x6b, 0xc7, 0x0f, 0x71, 0x1b, 0xc3,
      0x48, 0x8b, 0xed, 0x15, 0x26, 0x8c, 0xc3, 0xd5, 0x54, 0x08, 0xcc, 0x33,
      0x79, 0xc0, 0x9f, 0x49, 0xc8, 0x75, 0xef, 0xb6, 0xf3, 0x29, 0x89, 0xfd,
      0x75, 0xd1, 0xda, 0x92, 0xc3, 0x13, 0xc6, 0x76, 0x51, 0x11, 0x40, 0x7b,
      0x82, 0xf7, 0x30, 0x79, 0x49, 0x04, 0xe3, 0xbb, 0x61, 0x34, 0xa6, 0x58,
      0x0b, 0x7d, 0xef, 0x3e, 0xf9, 0xb3, 0x8d, 0x2a, 0xba, 0xe9, 0xbc, 0xc0,
      0xa7, 0xe6, 0x6c, 0xda, 0xf8, 0x8c, 0xdf, 0x8d, 0x96, 0x83, 0x2d, 0x80,
      0x4f, 0x21, 0x81, 0xde, 0x57, 0x9d, 0x0a, 0x3c, 0xcc, 0xec, 0x3b, 0xb2,
      0x25, 0x96, 0x3c, 0xea, 0xfd, 0x46, 0x26, 0xbe, 0x1c, 0x79, 0x82, 0x1d,
      0xe0, 0x14, 0x22, 0x7c, 0x80, 0x3d, 0xbd, 0x05, 0x90, 0xfa, 0xaf, 0x7d,
      0x70, 0x13, 0x43, 0x0f, 0x3d, 0xa0, 0x7f, 0x92, 0x3a, 0x53, 0x69, 0xe4,
      0xb0, 0x10, 0x0d, 0xa7, 0x73, 0xa8, 0x8c, 0x74, 0xab, 0xd7, 0x78, 0x15,
      0x45, 0xec, 0x6e, 0xc8, 0x8b, 0xa0, 0xba, 0x21, 0x6f, 0xf3, 0x08, 0xb8,
      0xc7, 0x4f, 0x14, 0xf5, 0xcc, 0xfd, 0x39, 0xbc, 0x11, 0xf5, 0xb9, 0x11,
      0xba, 0xf3, 0x11, 0x24, 0x74, 0x3e, 0x0c, 0x07, 0x4f, 0xac, 0x2a, 0xb2,
      0xb1, 0x3c, 0x00, 0xfa, 0xbb, 0x8c, 0xd8, 0x7d, 0x17, 0x5b, 0x8d, 0x39,
      0xc6, 0x23, 0x31, 0x32, 0x7d, 0x6e, 0x20, 0x38, 0xd0, 0xc3, 0x58, 0xe2,
      0xb1, 0xfe, 0x53, 0x6b, 0xc7, 0x10, 0x13, 0x7e, 0xc6, 0x7c, 0x67, 0x59,
      0x43, 0x70, 0x4a, 0x2d, 0x7f, 0x76, 0xde, 0xbd, 0x45, 0x43, 0x56, 0x60,
      0xcd, 0xe9, 0x24, 0x7b, 0xb7, 0x41, 0xce, 0x56, 0xed, 0xd3, 0x74, 0x75,
      0xcc, 0x9d, 0x48, 0x61, 0xc8, 0x19, 0x66, 0x08, 0xfb, 0x28, 0x60, 0x1f,
      0x83, 0x11, 0xc0, 0x9b, 0xbd, 0x71, 0x53, 0x36, 0x01, 0x76, 0xa8, 0xc0,
      0xdc, 0x1d, 0x18, 0x85, 0x19, 0x65, 0xce, 0xcf, 0x14, 0x2e, 0x6c, 0x32,
      0x15, 0xbc, 0x2c, 0x5e, 0x8f, 0xfc, 0x3c, 0xf0, 0x2d, 0xf5, 0x5c, 0x04,
      0xc9, 0x22, 0xf4, 0xc3, 0xb8, 0x57, 0x79, 0x52, 0x41, 0xfd, 0xff, 0xcd,
      0x26, 0xa8, 0xc0, 0xd2, 0xe1, 0x71, 0xd6, 0xf1, 0xf4, 0x0c, 0xa8, 0xeb,
      0x0c, 0x33, 0x40, 0x25, 0x73, 0xbb, 0x31, 0xda, 0x0c, 0xa6, 0xee, 0x0c,
      0x41, 0x51, 0x94, 0x3c, 0x24, 0x27, 0x65, 0xe9, 0xb5, 0xc4, 0xe2, 0x88,
      0xc0, 0x82, 0xd0, 0x72, 0xd9, 0x10, 0x4d, 0x7f, 0xc0, 0x88, 0x94, 0x41,
      0x2d, 0x05, 0x09, 0xfb, 0x97, 0x31, 0x6e, 0xc1, 0xe9, 0xf4, 0x50, 0x70,
      0xdc, 0x3f, 0x0a, 0x90, 0x46, 0x37, 0x60, 0x8c, 0xfb, 0x06, 0x6e, 0xde,
      0x6f, 0xa7, 0x6b, 0xa3, 0x88, 0x18, 0x96, 0x93, 0x19, 0x87, 0xe7, 0x0a,
      0x98, 0xf0, 0x13, 0x01, 0xab, 0x7c, 0xeb, 0x25, 0xa5, 0xe2, 0x98, 0x44,
      0x7d, 0x09, 0xe2, 0x42, 0x33, 0xd4, 0xeb, 0xcc, 0x9b, 0x70, 0xf6, 0x0f,
      0xf0, 0xb2, 0x99, 0xcc, 0x4f, 0x64, 0xc4, 0x69, 0x12, 0xea, 0x56, 0xfe,
      0x50, 0x0e, 0x02, 0x1f, 0x6d, 0x7a, 0x79, 0x62, 0xaa, 0x2e, 0x52, 0xaf,
      0xa3, 0xed, 0xcd, 0xa7, 0x45, 0xe6, 0x86, 0xed, 0xa1, 0x73, 0x5b, 0x1e,
      0x49, 0x4f, 0x92, 0x50, 0x83, 0x99, 0x3c, 0xf4, 0xf6, 0xa8, 0x49, 0xd7,
      0x08, 0xf7, 0xdc, 0x28, 0x2c, 0xe6, 0x22, 0x6f, 0xf8, 0xfa, 0xba, 0x9e,
      0x0a, 0xcf, 0x72, 0x74, 0x76, 0x75, 0x99, 0x4d, 0x3d, 0x9a, 0x4c, 0x54,
      0xcd, 0xf8, 0x54, 0xf0, 0xbd, 0x73, 0xe9, 0x4f, 0x29, 0xd0, 0xe1, 0x24,
      0x94, 0x52, 0xd6, 0x60, 0x80, 0x71, 0x24, 0x95, 0x92, 0x01, 0x0e, 0xa9,
      0x7e, 0x64, 0x2e, 0xed, 0x51, 0xcc, 0xd2, 0xff, 0xfd, 0x0b,
  };
  EXPECT_EQ(Bytes(ciphertext), Bytes(kExpectedCiphertext));

  static const uint8_t kExpectedSharedKey[HRSS_KEY_BYTES] = {
      0xbc, 0x98, 0x9c, 0x9c, 0x1f, 0x57, 0x6f, 0x38, 0x0b, 0x5d, 0xc2,
      0x23, 0x7d, 0x01, 0xae, 0x63, 0x17, 0xe8, 0xe4, 0xb2, 0x02, 0xa7,
      0xc4, 0x3a, 0x1b, 0x5a, 0xf3, 0xf8, 0xb5, 0xea, 0x6e, 0x22,
  };
  EXPECT_EQ(Bytes(shared_key), Bytes(kExpectedSharedKey));

  HRSS_decap(shared_key, &priv, ciphertext, sizeof(ciphertext));
  EXPECT_EQ(Bytes(shared_key, sizeof(shared_key)),
            Bytes(kExpectedSharedKey, sizeof(kExpectedSharedKey)));

  // Corrupt the ciphertext and ensure that the failure key is constant.
  ciphertext[50] ^= 4;
  HRSS_decap(shared_key, &priv, ciphertext, sizeof(ciphertext));

  static const uint8_t kExpectedFailureKey[HRSS_KEY_BYTES] = {
      0x8e, 0x19, 0xfe, 0x2b, 0x12, 0x67, 0xef, 0x9a, 0x63, 0x4d, 0x79,
      0x33, 0x8c, 0xce, 0xbf, 0x03, 0xdb, 0x9c, 0xc4, 0xc1, 0x70, 0xe1,
      0x32, 0xa6, 0xb3, 0xd3, 0xa1, 0x43, 0x3c, 0xf1, 0x1f, 0x5a,
  };
  EXPECT_EQ(Bytes(shared_key), Bytes(kExpectedFailureKey));
}

#if defined(POLY_RQ_MUL_ASM) && defined(SUPPORTS_ABI_TEST)
TEST(HRSS, ABI) {
  const bool has_avx2 = (OPENSSL_ia32cap_P[2] & (1 << 5)) != 0;
  if (!has_avx2) {
    fprintf(stderr, "Skipping ABI test due to lack of AVX2 support.\n");
    return;
  }

  alignas(16) uint16_t r[N + 3];
  alignas(16) uint16_t a[N + 3] = {0};
  alignas(16) uint16_t b[N + 3] = {0};
  CHECK_ABI(poly_Rq_mul, r, a, b);
}
#endif  // POLY_RQ_MUL_ASM && SUPPORTS_ABI_TEST
