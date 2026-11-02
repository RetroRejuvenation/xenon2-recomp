/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 the xenon2-recomp authors
 *
 * Regression tests for two setup-header defects in the vendored stb_vorbis (see vendor/audio/README.md, patches 5 to 7):
 *   - a mapping with 16 submaps wrote past submap_floor[15]/submap_residue[15] and left an unchecked residue index;
 *   - a sparse codebook with no codeword made DECODE read sorted_values[-1] of a NULL table;
 *   - a residue book with zero dimensions (decode_residue divides by it) is refused at setup (patch 7).
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

static stb_vorbis *open_in_arena(const unsigned char *file, size_t n, char **arena_out, int *err)
{
   size_t sz = (size_t)2 << 20;
   stb_vorbis_alloc a;
   char *arena = (char *)calloc(1, sz);
   a.alloc_buffer = arena;
   a.alloc_buffer_length_in_bytes = (int)sz;
   *arena_out = arena;
   return stb_vorbis_open_memory(file, (int)n, err, &a);
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

int main(void)
{
   test_16_submaps(1);
   test_16_submaps(63);
   test_sparse_without_codewords();
   test_zero_dimension_residue_book();
   if (failures) { printf("%d check(s) failed\n", failures); return 1; }
   printf("ok\n");
   return 0;
}
