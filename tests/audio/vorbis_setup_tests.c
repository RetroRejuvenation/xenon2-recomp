/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 the xenon2-recomp authors
 *
 * Regression tests for the setup-header and seek defects in the vendored stb_vorbis (see vendor/audio/README.md, patches 5 to 10):
 *   - a mapping with 16 submaps wrote past submap_floor[15]/submap_residue[15] and left an unchecked residue index;
 *   - a sparse codebook with no codeword made DECODE read sorted_values[-1] of a NULL table;
 *   - a residue book with zero dimensions (decode_residue divides by it) is refused at setup (patch 7);
 *   - a codebook lookup table whose byte size overflows the allocators' int is refused at setup (patch 8);
 *   - a seek before the first read no longer leaves channel_buffer_start past channel_buffer_end (patch 9);
 *   - a type 2 residue's setup temp estimate covers the full block decode_residue uses (patch 10).
 * The streams are generated here (no file, no game data). The decoder is opened in memory with the same macros and an
 * arena allocation as runtime/audio/decode.cpp uses; the test includes stb_vorbis.c to look at its setup state.
 * Run it under AddressSanitizer/UBSan too (the sanitizer CI job does). */
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_INTEGER_CONVERSION
#include "stb_vorbis.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { unsigned char b[4096]; int nbits; } Bits;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static void put(Bits *w, unsigned v, int n) /* LSB first, as Vorbis packs bits */
{
   int i;
   for (i = 0; i < n; ++i, ++w->nbits)
      if ((v >> i) & 1) w->b[w->nbits >> 3] |= (unsigned char)(1u << (w->nbits & 7));
}

static int nbytes(const Bits *w) { return (w->nbits + 7) >> 3; }

static int vlog(int v) { int r = 0; while (v) { ++r; v >>= 1; } return r; }   /* ilog(), as the ordered-codebook runs use */

static unsigned ogg_crc(const unsigned char *p, size_t n)
{
   unsigned c = 0;
   size_t i;
   int k;
   for (i = 0; i < n; ++i) {
      c ^= (unsigned)p[i] << 24;
      for (k = 0; k < 8; ++k) c = (c << 1) ^ ((c & 0x80000000u) ? 0x04c11db7u : 0u);
   }
   return c;
}

/* append one page holding the given packets (each shorter than 255*255 bytes) */
static size_t page(unsigned char *out, size_t pos, int flags, unsigned seq, unsigned granule, const Bits *pk, int npk)
{
   unsigned char seg[255];
   int ns = 0, i;
   size_t o, body = 0;
   unsigned char *h = out + pos;
   unsigned c;
   for (i = 0; i < npk; ++i) {
      int len = nbytes(&pk[i]);
      while (len >= 255) { seg[ns++] = 255; len -= 255; }
      seg[ns++] = (unsigned char)len;
      body += (size_t)nbytes(&pk[i]);
   }
   memcpy(h, "OggS", 4); h[4] = 0; h[5] = (unsigned char)flags;
   memset(h + 6, 0, 8);
   for (i = 0; i < 4; ++i) h[6 + i] = (unsigned char)(granule >> (8 * i));
   for (i = 0; i < 4; ++i) h[14 + i] = (unsigned char)(0x1234u >> (8 * i));
   for (i = 0; i < 4; ++i) h[18 + i] = (unsigned char)(seq >> (8 * i));
   memset(h + 22, 0, 4); h[26] = (unsigned char)ns; memcpy(h + 27, seg, (size_t)ns);
   o = 27 + (size_t)ns;
   for (i = 0; i < npk; ++i) { memcpy(h + o, pk[i].b, (size_t)nbytes(&pk[i])); o += (size_t)nbytes(&pk[i]); }
   (void)body;
   c = ogg_crc(h, o);
   for (i = 0; i < 4; ++i) h[22 + i] = (unsigned char)(c >> (8 * i));
   return pos + o;
}

static void vorbis_hdr(Bits *w, int type)
{
   const char *s;
   put(w, (unsigned)type, 8);
   for (s = "vorbis"; *s; ++s) put(w, (unsigned char)*s, 8);
}

static void id_and_comment(Bits *id, Bits *com)
{
   memset(id, 0, sizeof *id); memset(com, 0, sizeof *com);
   vorbis_hdr(id, 1);
   put(id, 0, 32); put(id, 1, 8); put(id, 44100, 32); put(id, 0, 32); put(id, 0, 32); put(id, 0, 32);
   put(id, 8, 4); put(id, 11, 4); put(id, 1, 1);                    /* blocksizes 256/2048, framing */
   vorbis_hdr(com, 3);
   put(com, 0, 32); put(com, 0, 32); put(com, 1, 1);                /* empty vendor, no comments */
}

/* the same identification and comment headers, with the two block sizes chosen by the caller (exponents) */
static void id_and_comment_blocks(Bits *id, Bits *com, int log0, int log1)
{
   memset(id, 0, sizeof *id); memset(com, 0, sizeof *com);
   vorbis_hdr(id, 1);
   put(id, 0, 32); put(id, 1, 8); put(id, 44100, 32); put(id, 0, 32); put(id, 0, 32); put(id, 0, 32);
   put(id, (unsigned)log0, 4); put(id, (unsigned)log1, 4); put(id, 1, 1);
   vorbis_hdr(com, 3);
   put(com, 0, 32); put(com, 0, 32); put(com, 1, 1);
}

static size_t finish(unsigned char *out, const Bits *id, const Bits *com, const Bits *setup, const Bits *audio)
{
   size_t pos = 0;
   Bits cs[2];
   cs[0] = *com; cs[1] = *setup;
   pos = page(out, pos, 0x02, 0, 0, id, 1);
   pos = page(out, pos, 0x00, 1, 0, cs, 2);
   pos = page(out, pos, 0x04, 2, 1024, audio, 1);
   return pos;
}

/* One mono stream: a plain 2-entry codebook, floor_count = V+1 floor1 types, one residue, one mapping with 16 submaps whose
 * 16th entry names floor V and residue 0. */
static size_t build_16_submaps(unsigned char *out, int V)
{
   static Bits id, com, setup, audio;
   int i, j;
   id_and_comment(&id, &com);
   memset(&setup, 0, sizeof setup); memset(&audio, 0, sizeof audio);
   vorbis_hdr(&setup, 5);
   put(&setup, 0, 8);
   put(&setup, 0x564342, 24); put(&setup, 1, 16); put(&setup, 2, 24);
   put(&setup, 0, 1); put(&setup, 0, 1);
   put(&setup, 0, 5); put(&setup, 0, 5);                            /* both codeword lengths 1 */
   put(&setup, 0, 4);                                               /* lookup_type 0 */
   put(&setup, 0, 6); put(&setup, 0, 16);
   put(&setup, (unsigned)V, 6);                                     /* floor_count - 1 */
   for (i = 0; i <= V; ++i) { put(&setup, 1, 16); put(&setup, 0, 5); put(&setup, 0, 2); put(&setup, 4, 4); }
   put(&setup, 0, 6);                                               /* residue_count - 1 = 0 */
   put(&setup, 0, 16); put(&setup, 0, 24); put(&setup, 0, 24); put(&setup, 3, 24);
   put(&setup, 0, 6); put(&setup, 0, 8);
   put(&setup, 0, 3); put(&setup, 0, 1);
   put(&setup, 0, 6);                                               /* mapping_count - 1 */
   put(&setup, 0, 16);
   put(&setup, 1, 1); put(&setup, 15, 4);                           /* 16 submaps */
   put(&setup, 0, 1); put(&setup, 0, 2);
   put(&setup, 0, 4);                                               /* channel 0 mux 0 */
   for (j = 0; j < 16; ++j) {
      put(&setup, 0, 8);
      put(&setup, j == 15 ? (unsigned)V : 0u, 8);                   /* floor */
      put(&setup, 0, 8);                                            /* residue 0 */
   }
   put(&setup, 0, 6);
   put(&setup, 0, 1); put(&setup, 0, 16); put(&setup, 0, 16); put(&setup, 0, 8);
   put(&setup, 1, 1);
   audio.nbits = 8 * 8;
   return finish(out, &id, &com, &setup, &audio);
}

/* A mono stream whose only codebook has dimensions 1, entries 4, sparse, with `present` of the entries present (length 1
 * for the first), used as the classbook of a type 1 residue. With present == 0 the book has no codeword at all. */
static size_t build_sparse(unsigned char *out, int present)
{
   static Bits id, com, setup, audio;
   int i;
   id_and_comment(&id, &com);
   memset(&setup, 0, sizeof setup); memset(&audio, 0, sizeof audio);
   vorbis_hdr(&setup, 5);
   put(&setup, 0, 8);
   put(&setup, 0x564342, 24); put(&setup, 1, 16); put(&setup, 4, 24);
   put(&setup, 0, 1); put(&setup, 1, 1);                            /* not ordered, sparse */
   for (i = 0; i < 4; ++i) {
      if (present && i == 0) { put(&setup, 1, 1); put(&setup, 0, 5); }
      else put(&setup, 0, 1);
   }
   put(&setup, 0, 4);
   put(&setup, 0, 6); put(&setup, 0, 16);
   put(&setup, 0, 6);
   put(&setup, 1, 16); put(&setup, 0, 5); put(&setup, 0, 2); put(&setup, 4, 4);   /* one floor1, no partitions */
   put(&setup, 0, 6);
   put(&setup, 1, 16); put(&setup, 0, 24); put(&setup, 32, 24); put(&setup, 0, 24);  /* residue type 1, begin 0, end 32, part_size 1 */
   put(&setup, 0, 6); put(&setup, 0, 8);                            /* classifications 1, classbook 0 */
   put(&setup, 0, 3); put(&setup, 0, 1);
   put(&setup, 0, 6);
   put(&setup, 0, 16);
   put(&setup, 0, 1); put(&setup, 0, 1); put(&setup, 0, 2);
   put(&setup, 0, 8); put(&setup, 0, 8); put(&setup, 0, 8);
   put(&setup, 0, 6);
   put(&setup, 0, 1); put(&setup, 0, 16); put(&setup, 0, 16); put(&setup, 0, 8);
   put(&setup, 1, 1);
   audio.b[0] = 0x02;                                               /* packet type 0, floor present */
   audio.nbits = 16 * 8;
   return finish(out, &id, &com, &setup, &audio);
}

/* A mono stream whose only codebook has zero dimensions and zero entries and is the classbook of a residue of the given type. */
static size_t build_zero_dimension_book(unsigned char *out, int residue_type)
{
   static Bits id, com, setup, audio;
   id_and_comment(&id, &com);
   memset(&setup, 0, sizeof setup); memset(&audio, 0, sizeof audio);
   vorbis_hdr(&setup, 5);
   put(&setup, 0, 8);
   put(&setup, 0x564342, 24); put(&setup, 0, 16); put(&setup, 0, 24);   /* dimensions 0, entries 0 */
   put(&setup, 0, 1); put(&setup, 0, 1);                               /* not ordered, not sparse */
   put(&setup, 0, 4);                                                  /* lookup_type 0 */
   put(&setup, 0, 6); put(&setup, 0, 16);
   put(&setup, 0, 6);
   put(&setup, 1, 16); put(&setup, 0, 5); put(&setup, 0, 2); put(&setup, 4, 4);   /* one floor1, no partitions */
   put(&setup, 0, 6);
   put(&setup, (unsigned)residue_type, 16); put(&setup, 0, 24); put(&setup, 32, 24); put(&setup, 0, 24);
   put(&setup, 0, 6); put(&setup, 0, 8);                               /* classifications 1, classbook 0 */
   put(&setup, 0, 3); put(&setup, 0, 1);
   put(&setup, 0, 6);
   put(&setup, 0, 16);
   put(&setup, 0, 1); put(&setup, 0, 1); put(&setup, 0, 2);
   put(&setup, 0, 8); put(&setup, 0, 8); put(&setup, 0, 8);
   put(&setup, 0, 6);
   put(&setup, 0, 1); put(&setup, 0, 16); put(&setup, 0, 16); put(&setup, 0, 8);
   put(&setup, 1, 1);
   audio.b[0] = 0x02;
   audio.nbits = 16 * 8;
   return finish(out, &id, &com, &setup, &audio);
}

/* A mono 64/8192 stream with one audio packet per page: a long block with prev=0,next=0 (whose decode defers 2032 samples),
 * two short blocks and four long blocks. Seeking to sample 3000 before any read used to apply the leftover of that deferred
 * discard to a frame decoded after the jump and leave channel_buffer_start past channel_buffer_end. */
static size_t build_seek_discard(unsigned char *out)
{
   static Bits id, com, setup, audio[7], hs[2];
   static const unsigned char pkt[7] = { 0x02, 0x00, 0x00, 0x0E, 0x0E, 0x0E, 0x0E };
   static const unsigned granule[7] = { 0, 32, 64, 4160, 8256, 12352, 16448 };
   size_t pos;
   int i;

   id_and_comment_blocks(&id, &com, 6, 13);                         /* blocksizes 64/8192 */
   memset(&setup, 0, sizeof setup);
   vorbis_hdr(&setup, 5);

   put(&setup, 0, 8);                                               /* codebook_count - 1 = 0 */
   put(&setup, 0x42, 8); put(&setup, 0x43, 8); put(&setup, 0x56, 8);
   put(&setup, 1, 8); put(&setup, 0, 8);                            /* dimensions 1 */
   put(&setup, 2, 8); put(&setup, 0, 8); put(&setup, 0, 8);         /* entries 2 */
   put(&setup, 0, 1); put(&setup, 0, 1);                            /* not ordered, not sparse */
   put(&setup, 0, 5); put(&setup, 0, 5);                            /* both codeword lengths 1 */
   put(&setup, 0, 4);                                               /* lookup_type 0 */

   put(&setup, 0, 6); put(&setup, 0, 16);                           /* one time-domain transform */
   put(&setup, 0, 6); put(&setup, 1, 16); put(&setup, 0, 5); put(&setup, 0, 2); put(&setup, 4, 4);   /* floor1, no partitions */

   put(&setup, 0, 6);                                               /* residue_count - 1 = 0 */
   put(&setup, 1, 16); put(&setup, 0, 24); put(&setup, 4096, 24); put(&setup, 0, 24);   /* type 1, begin 0, end 4096, part_size 1 */
   put(&setup, 0, 6); put(&setup, 0, 8);                            /* classifications 1, classbook 0 */
   put(&setup, 0, 3); put(&setup, 0, 1);                            /* cascade 0 */

   put(&setup, 0, 6);                                               /* mapping_count - 1 = 0 */
   put(&setup, 0, 16); put(&setup, 0, 1); put(&setup, 0, 1); put(&setup, 0, 2);
   put(&setup, 0, 8); put(&setup, 0, 8); put(&setup, 0, 8);

   put(&setup, 1, 6);                                               /* mode_count - 1 = 1 */
   put(&setup, 0, 1); put(&setup, 0, 16); put(&setup, 0, 16); put(&setup, 0, 8);   /* short mode 0 */
   put(&setup, 1, 1); put(&setup, 0, 16); put(&setup, 0, 16); put(&setup, 0, 8);   /* long mode 1 */
   put(&setup, 1, 1);                                               /* framing */

   for (i = 0; i < 7; ++i) {
      memset(&audio[i], 0, sizeof audio[i]);
      audio[i].b[0] = pkt[i];
      audio[i].nbits = 8;                                           /* exactly one byte, as the format's small packets */
   }
   hs[0] = com; hs[1] = setup;
   pos = page(out, 0, 0x02, 0, 0, &id, 1);
   pos = page(out, pos, 0x00, 1, 0, hs, 2);
   for (i = 0; i < 7; ++i)
      pos = page(out, pos, i == 6 ? 0x04 : 0x00, (unsigned)(2 + i), granule[i], &audio[i], 1);
   return pos;
}

/* A mono 64/64 stream. With `bad` its only codebook has dimensions 65534 and entries 32769 with lookup_type 2, so
 * entries*dimensions = 2147483646 and sizeof(uint16) times that does not fit the int size of setup_temp_malloc.
 * Without `bad` the same lookup_type 2 path has a small valid table, followed by a complete setup. */
static size_t build_lookup_truncation(unsigned char *out, int bad)
{
   static Bits id, com, setup, audio, hs[2];
   size_t pos;

   id_and_comment_blocks(&id, &com, 6, 6);                          /* blocksizes 64/64 */
   memset(&setup, 0, sizeof setup); memset(&audio, 0, sizeof audio);
   vorbis_hdr(&setup, 5);

   put(&setup, 0, 8);                                               /* codebook_count - 1 = 0 */
   put(&setup, 0x42, 8); put(&setup, 0x43, 8); put(&setup, 0x56, 8);

   if (bad) {
      put(&setup, 0xFE, 8); put(&setup, 0xFF, 8);                   /* dimensions 0xFFFE */
      put(&setup, 0x01, 8); put(&setup, 0x80, 8); put(&setup, 0x00, 8);   /* entries 0x008001 */
      put(&setup, 1, 1);                                            /* ordered */
      put(&setup, 15, 5);                                           /* first codeword length 16 */
      put(&setup, 32769, 16);                                       /* one run of 32769 entries */
      put(&setup, 2, 4);                                            /* lookup_type 2 */
      put(&setup, 0, 32); put(&setup, 0, 32);                       /* minimum_value, delta_value */
      put(&setup, 0, 4); put(&setup, 0, 1);                         /* value_bits - 1 = 0, sequence_p = 0 */
   } else {
      put(&setup, 2, 8); put(&setup, 0, 8);                         /* dimensions 2 */
      put(&setup, 2, 8); put(&setup, 0, 8); put(&setup, 0, 8);      /* entries 2 */
      put(&setup, 0, 1); put(&setup, 0, 1);                         /* not ordered, not sparse */
      put(&setup, 0, 5); put(&setup, 0, 5);                         /* both codeword lengths 1 */
      put(&setup, 2, 4);                                            /* lookup_type 2 */
      put(&setup, 0, 32); put(&setup, 0, 32);
      put(&setup, 0, 4); put(&setup, 0, 1);
      put(&setup, 0, 1); put(&setup, 0, 1); put(&setup, 0, 1); put(&setup, 0, 1);   /* four multiplicands */

      put(&setup, 0, 6); put(&setup, 0, 16);                        /* one time-domain transform */
      put(&setup, 0, 6); put(&setup, 1, 16); put(&setup, 0, 5); put(&setup, 0, 2); put(&setup, 4, 4);
      put(&setup, 0, 6);                                            /* residue_count - 1 = 0 */
      put(&setup, 1, 16); put(&setup, 0, 24); put(&setup, 32, 24); put(&setup, 0, 24);
      put(&setup, 0, 6); put(&setup, 0, 8); put(&setup, 0, 3); put(&setup, 0, 1);
      put(&setup, 0, 6); put(&setup, 0, 16); put(&setup, 0, 1); put(&setup, 0, 1); put(&setup, 0, 2);
      put(&setup, 0, 8); put(&setup, 0, 8); put(&setup, 0, 8);
      put(&setup, 0, 6); put(&setup, 0, 1); put(&setup, 0, 16); put(&setup, 0, 16); put(&setup, 0, 8);
      put(&setup, 1, 1);                                            /* framing */
   }
   put(&setup, 0, 8);                                               /* padding so the lookup read has bytes */

   audio.nbits = 8 * 8;
   hs[0] = com; hs[1] = setup;
   pos = page(out, 0, 0x02, 0, 0, &id, 1);
   pos = page(out, pos, 0x00, 1, 0, hs, 2);
   pos = page(out, pos, 0x04, 2, 64, &audio, 1);
   return pos;
}

/* A mono 64/8192 stream whose one residue is type 2 over the whole long block (begin 0, end 8192, part_size 1). The extra
 * ordered codebook has E entries and consumes about 13 setup bytes each, so E places the free gap of a 2 MiB arena between
 * the half-block setup estimate and the full-block request decode_residue really makes. */
static size_t build_residue_type2(unsigned char *out, int E)
{
   static Bits id, com, setup, audio, hs[2];
   size_t pos;

   id_and_comment_blocks(&id, &com, 6, 13);                         /* blocksizes 64/8192 */
   memset(&setup, 0, sizeof setup); memset(&audio, 0, sizeof audio);
   vorbis_hdr(&setup, 5);

   put(&setup, 1, 8);                                               /* codebook_count - 1 = 1 */

   put(&setup, 0x42, 8); put(&setup, 0x43, 8); put(&setup, 0x56, 8);   /* codebook 0: classbook */
   put(&setup, 1, 8); put(&setup, 0, 8);
   put(&setup, 2, 8); put(&setup, 0, 8); put(&setup, 0, 8);
   put(&setup, 0, 1); put(&setup, 0, 1);
   put(&setup, 0, 5); put(&setup, 0, 5);
   put(&setup, 0, 4);

   put(&setup, 0x42, 8); put(&setup, 0x43, 8); put(&setup, 0x56, 8);   /* codebook 1: padding */
   put(&setup, 1, 8); put(&setup, 0, 8);                            /* dimensions 1 */
   put(&setup, E & 0xFF, 8);
   put(&setup, (E >> 8) & 0xFF, 8);
   put(&setup, (E >> 16) & 0xFF, 8);                                /* entries E (24 bit) */
   put(&setup, 1, 1);                                               /* ordered */
   put(&setup, 17, 5);                                              /* first codeword length 18 */
   put(&setup, (unsigned)E, vlog(E));                               /* one run: E entries at length 18 */
   put(&setup, 0, 4);                                               /* lookup_type 0 */

   put(&setup, 0, 6); put(&setup, 0, 16);                           /* one time-domain transform */
   put(&setup, 0, 6); put(&setup, 1, 16); put(&setup, 0, 5); put(&setup, 0, 2); put(&setup, 4, 4);

   put(&setup, 0, 6);                                               /* residue_count - 1 = 0 */
   put(&setup, 2, 16);                                              /* type 2 */
   put(&setup, 0, 24); put(&setup, 8192, 24);                       /* begin 0, end 8192 */
   put(&setup, 0, 24);                                              /* part_size - 1 = 0 -> 1 */
   put(&setup, 0, 6);                                               /* classifications - 1 = 0 */
   put(&setup, 0, 8);                                               /* classbook 0 */
   put(&setup, 0, 3); put(&setup, 0, 1);                            /* cascade 0 */

   put(&setup, 0, 6);                                               /* mapping_count - 1 = 0 */
   put(&setup, 0, 16); put(&setup, 0, 1); put(&setup, 0, 1); put(&setup, 0, 2);
   put(&setup, 0, 8); put(&setup, 0, 8); put(&setup, 0, 8);

   put(&setup, 1, 6);                                               /* mode_count - 1 = 1 */
   put(&setup, 0, 1); put(&setup, 0, 16); put(&setup, 0, 16); put(&setup, 0, 8);
   put(&setup, 1, 1); put(&setup, 0, 16); put(&setup, 0, 16); put(&setup, 0, 8);
   put(&setup, 1, 1);                                               /* framing */

   audio.b[0] = 0x02;                                               /* type 0, long mode, prev 0, next 0, floor absent */
   audio.nbits = 8 * 8;
   hs[0] = com; hs[1] = setup;
   pos = page(out, 0, 0x02, 0, 0, &id, 1);
   pos = page(out, pos, 0x00, 1, 0, hs, 2);
   pos = page(out, pos, 0x04, 2, 4096, &audio, 1);
   return pos;
}

static stb_vorbis *open_with_arena(const unsigned char *file, size_t n, size_t sz, char **arena_out, int *err)
{
   stb_vorbis_alloc a;
   char *arena = (char *)calloc(1, sz);
   *arena_out = arena;
   if (!arena) { if (err) *err = VORBIS_outofmem; return NULL; }
   a.alloc_buffer = arena;
   a.alloc_buffer_length_in_bytes = (int)sz;
   return stb_vorbis_open_memory(file, (int)n, err, &a);
}

static stb_vorbis *open_in_arena(const unsigned char *file, size_t n, char **arena_out, int *err)
{
   return open_with_arena(file, n, (size_t)2 << 20, arena_out, err);
}

static void test_16_submaps(int V)
{
   static unsigned char file[16384];
   size_t n = build_16_submaps(file, V);
   int err = 0;
   char *arena;
   stb_vorbis *v = open_in_arena(file, n, &arena, &err);
   printf("16 submaps, floor index %d: open %s (error %d)\n", V, v ? "ok" : "failed", err);
   CHECK(v != NULL);
   if (v) {
      const Mapping *m = &v->mapping[0];
      CHECK(m->submaps == 16);
      CHECK(m->submap_floor[15] == V);
      CHECK(m->submap_residue[0] == 0);   /* the 16th floor index must not land on the first residue index */
      CHECK(m->submap_residue[15] == 0);
      stb_vorbis_close(v);
   }
   free(arena);
   /* the same stream on the heap allocator, where AddressSanitizer can see an overrun */
   v = stb_vorbis_open_memory(file, (int)n, &err, NULL);
   CHECK(v != NULL);
   if (v) stb_vorbis_close(v);
}

static void test_sparse_without_codewords(void)
{
   static unsigned char file[16384];
   size_t n;
   int err = 0;
   char *arena;
   stb_vorbis *v;
   n = build_sparse(file, 0);
   v = open_in_arena(file, n, &arena, &err);
   printf("sparse book without codeword: open %s (error %d)\n", v ? "ok" : "failed", err);
   if (v) stb_vorbis_close(v);   /* either outcome is acceptable; reaching this line without a fault is the test */
   free(arena);
   v = stb_vorbis_open_memory(file, (int)n, &err, NULL);
   if (v) stb_vorbis_close(v);
   n = build_sparse(file, 1);
   v = open_in_arena(file, n, &arena, &err);
   printf("control (one codeword): open %s (error %d)\n", v ? "ok" : "failed", err);
   CHECK(v != NULL);
   if (v) stb_vorbis_close(v);
   free(arena);
}

static void test_zero_dimension_residue_book(void)
{
   static unsigned char file[16384];
   int type;
   for (type = 0; type <= 2; ++type) {
      size_t n = build_zero_dimension_book(file, type);
      int err = 0;
      char *arena;
      stb_vorbis *v = open_in_arena(file, n, &arena, &err);
      printf("zero-dimension classbook, residue type %d: open %s (error %d)\n", type, v ? "ok" : "refused", err);
      CHECK(v == NULL);
      CHECK(err == VORBIS_invalid_setup);
      if (v) stb_vorbis_close(v);
      free(arena);
   }
}

/* Seek before the first read: the first frame's deferred discard must not be applied after a mid-stream jump. The read
 * buffer has a guard on both sides so an overrun is caught without AddressSanitizer as well. */
static void test_seek_deferred_discard(void)
{
   enum { GUARD = 2048, FRAMES = 4096 };
   static unsigned char file[16384];
   size_t n = build_seek_discard(file);
   int err = 0;
   char *arena;
   stb_vorbis *v = open_in_arena(file, n, &arena, &err);
   printf("seek before first read: open %s (error %d)\n", v ? "ok" : "failed", err);
   CHECK(v != NULL);
   if (v) {
      float *buf = (float *)malloc((GUARD + FRAMES + GUARD) * sizeof(float));
      CHECK(buf != NULL);
      if (buf) {
         int i, seeked;
         for (i = 0; i < GUARD + FRAMES + GUARD; ++i) buf[i] = -12345.5f;
         CHECK(stb_vorbis_seek_start(v));
         seeked = stb_vorbis_seek(v, 3000);
         if (seeked) {
            CHECK(v->channel_buffer_start >= 0);
            CHECK(v->channel_buffer_start <= v->channel_buffer_end);
            CHECK(stb_vorbis_get_samples_float_interleaved(v, 1, buf + GUARD, FRAMES) <= FRAMES);
         } else {
            int e = stb_vorbis_get_error(v);
            printf("seek(3000) refused cleanly (error %d)\n", e);
            CHECK(e != VORBIS__no_error);
         }
         for (i = 0; i < GUARD; ++i) {
            CHECK(buf[i] == -12345.5f);
            CHECK(buf[GUARD + FRAMES + i] == -12345.5f);
         }
         free(buf);
      }
      stb_vorbis_close(v);
   }
   free(arena);
}

/* A lookup table whose byte size does not fit the int parameter of setup_temp_malloc must be refused at setup; the small
 * valid table on the same path still opens. */
static void test_lookup_size_truncation(void)
{
   static unsigned char file[16384];
   size_t n;
   int err = 0;
   char *arena;
   stb_vorbis *v;

   n = build_lookup_truncation(file, 1);
   v = open_in_arena(file, n, &arena, &err);
   printf("oversized lookup table: open %s (error %d)\n", v ? "ok" : "refused", err);
   CHECK(v == NULL);
   CHECK(err == VORBIS_invalid_setup);
   if (v) stb_vorbis_close(v);
   free(arena);

   err = 0;
   n = build_lookup_truncation(file, 0);
   v = open_in_arena(file, n, &arena, &err);
   printf("control (small lookup table): open %s (error %d)\n", v ? "ok" : "failed", err);
   CHECK(v != NULL);
   if (v) stb_vorbis_close(v);
   free(arena);
}

#define C3_ARENA_2M    ((size_t)2 << 20)
#define C3_ARENA_4M    ((size_t)4 << 20)
#define C3_ARENA_PROBE ((size_t)16 << 20)

/* A type 2 residue over the whole long block needs twice the half-block temp estimate. Tune the padding codebook so the
 * free gap of a 2 MiB arena lies between the old half-block estimate and the real full-block request: the setup must then
 * refuse that arena cleanly (before the fix decode_residue stored the block pointers through a NULL temp allocation),
 * while a 4 MiB arena still has room and opens. */
static void test_residue_type2_temp_estimate(void)
{
   static unsigned char file[16384];
   const long old_temp = (long)(sizeof(void *) + (8192 / 2) * sizeof(uint8 *));
   const long real_temp = (long)(sizeof(void *) + 8192 * sizeof(uint8 *));
   const long want = old_temp + (real_temp - old_temp) / 2;
   size_t n = 0;
   int E = 140000, iter, err = 0;
   unsigned setup = 0, temp_est = 0;
   char *arena;
   stb_vorbis *v;

   for (iter = 0; iter < 24; ++iter) {
      stb_vorbis_info info;
      long gap, dE;
      n = build_residue_type2(file, E);
      v = open_with_arena(file, n, C3_ARENA_PROBE, &arena, &err);
      CHECK(v != NULL);
      if (!v) { free(arena); return; }
      info = stb_vorbis_get_info(v);
      setup = info.setup_memory_required;
      temp_est = info.temp_memory_required;
      stb_vorbis_close(v);
      free(arena);
      gap = (long)C3_ARENA_2M - (long)setup;
      printf("type 2 residue, E=%d: setup=%u temp estimate=%u, 2 MiB gap=%ld\n", E, setup, temp_est, gap);
      if (gap >= old_temp && gap < real_temp) break;
      dE = (gap - want) / 13;                                      /* about 13 setup bytes per ordered entry */
      if (dE == 0) dE = gap > want ? 1 : -1;
      E += (int)dE;
      if (E < 1000) E = 1000;
      if (E > 250000) E = 250000;
   }
   CHECK(iter < 24);
   if (iter == 24) return;
   CHECK(temp_est >= (unsigned)real_temp);                         /* the estimate must cover what decode_residue asks for */

   n = build_residue_type2(file, E);
   err = 0;
   v = open_with_arena(file, n, C3_ARENA_2M, &arena, &err);
   printf("type 2 residue in a 2 MiB arena: open %s (error %d)\n", v ? "ok" : "refused", err);
   if (v) {
      stb_vorbis_close(v);                                        /* an open is acceptable; a crash is not */
   } else {
      CHECK(err == VORBIS_outofmem);
   }
   free(arena);

   err = 0;
   v = open_with_arena(file, n, C3_ARENA_4M, &arena, &err);
   printf("control (4 MiB arena): open %s (error %d)\n", v ? "ok" : "failed", err);
   CHECK(v != NULL);
   if (v) stb_vorbis_close(v);
   free(arena);
}

int main(void)
{
   test_16_submaps(1);
   test_16_submaps(63);
   test_sparse_without_codewords();
   test_zero_dimension_residue_book();
   test_seek_deferred_discard();
   test_lookup_size_truncation();
   test_residue_type2_temp_estimate();
   if (failures) { printf("%d check(s) failed\n", failures); return 1; }
   printf("ok\n");
   return 0;
}
