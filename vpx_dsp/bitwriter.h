/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef VPX_DSP_BITWRITER_H_
#define VPX_DSP_BITWRITER_H_

#include "vpx_ports/mem.h"

#include "vpx_dsp/prob.h"

#include "entenc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vpx_writer {
  unsigned int lowvalue;
  unsigned int range;
  int count;
  unsigned int pos;
  uint8_t *buffer;
#if DAALA_ENTROPY_CODER
  od_ec_enc ec;
#endif
} vpx_writer;

void vpx_start_encode(vpx_writer *bc, uint8_t *buffer);
void vpx_stop_encode(vpx_writer *bc);

#if DAALA_ENTROPY_CODER
static INLINE void vpx_write(vpx_writer *br, int bit, int probability) {
  if (probability == 128) {
    od_ec_enc_bits(&br->ec, bit, 1);
  } else {
    od_ec_encode_bool_q15(&br->ec, bit, probability * 128);
  }
}
#else
static INLINE void vpx_write(vpx_writer *br, int bit, int probability) {
  unsigned int split;
  int count = br->count;
  unsigned int range = br->range;
  unsigned int lowvalue = br->lowvalue;
  register int shift;

  split = 1 + (((range - 1) * probability) >> 8);

  range = split;

  if (bit) {
    lowvalue += split;
    range = br->range - split;
  }

  shift = vpx_norm[range];

  range <<= shift;
  count += shift;

  if (count >= 0) {
    int offset = shift - count;

    if ((lowvalue << (offset - 1)) & 0x80000000) {
      int x = br->pos - 1;

      while (x >= 0 && br->buffer[x] == 0xff) {
        br->buffer[x] = 0;
        x--;
      }

      br->buffer[x] += 1;
    }

    br->buffer[br->pos++] = (lowvalue >> (24 - offset));
    lowvalue <<= offset;
    shift = count;
    lowvalue &= 0xffffff;
    count -= 8;
  }

  lowvalue <<= shift;
  br->count = count;
  br->lowvalue = lowvalue;
  br->range = range;
}
#endif

static INLINE void vpx_write_bit(vpx_writer *w, int bit) {
  vpx_write(w, bit, 128);  // vpx_prob_half
}

static INLINE void vpx_write_literal(vpx_writer *w, int data, int bits) {
  int bit;

  for (bit = bits - 1; bit >= 0; bit--) vpx_write_bit(w, 1 & (data >> bit));
}

#define vpx_write_prob(w, v) vpx_write_literal((w), (v), 8)

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VPX_DSP_BITWRITER_H_
