/*
 *  Copyright (c) 2015 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <string.h>

#include "./vpx_scale_rtcd.h"
#include "vpx/vpx_integer.h"
#include "vp10/common/dering.h"
#include "vp10/common/onyxc_int.h"
#include "vp10/common/reconinter.h"
#include "od_dering.h"
#include <math.h>


int compute_level_from_index(int global_level, int gi) {
  static const double dering_gains[4] = {0, .7, 1, 1.4};
  int level;
  if (global_level == 0) return 0;
  level = (int)floor(.5 + global_level*dering_gains[gi]);
  return clamp(level, gi, MAX_DERING_LEVEL-1);
}

int sb_all_skip(const VP10_COMMON *const cm, int mi_row, int mi_col) {
  int r, c;
  int maxc, maxr;
  int skip = 1;
  maxc = cm->mi_cols - mi_col;
  maxr = cm->mi_rows - mi_row;
  if (maxr > MI_BLOCK_SIZE) maxr = MI_BLOCK_SIZE;
  if (maxc > MI_BLOCK_SIZE) maxc = MI_BLOCK_SIZE;
  for (r = 0; r < maxr; r++) {
    for (c = 0; c < maxc; c++) {
      skip = skip &&
          cm->mi_grid_visible[(mi_row + r) * cm->mi_stride + mi_col + c]->mbmi.skip;
    }
  }
  return skip;
}

void vp10_dering_frame(YV12_BUFFER_CONFIG *frame, VP10_COMMON *cm,
                       MACROBLOCKD *xd, int global_level) {
  int r, c;
  int sbr, sbc;
  int nhsb, nvsb;
  dering_in *src[3];
  unsigned char *bskip;
  int dir[OD_DERING_NBLOCKS][OD_DERING_NBLOCKS] = {{0}};
  int stride;
  int bsize[3];
  int dec[3];
  int pli;
  nvsb = (cm->mi_rows + MI_BLOCK_SIZE - 1)/MI_BLOCK_SIZE;
  nhsb = (cm->mi_cols + MI_BLOCK_SIZE - 1)/MI_BLOCK_SIZE;
  bskip = vpx_malloc(sizeof(*bskip)*cm->mi_rows*cm->mi_cols);
  vp10_setup_dst_planes(xd->plane, frame, 0, 0);
  for (pli = 0; pli < 3; pli++) {
    dec[pli] = xd->plane[pli].subsampling_x;
    bsize[pli] = 8 >> dec[pli];
  }
  stride = bsize[0]*cm->mi_cols;
  for (pli = 0; pli < 3; pli++) {
    src[pli] = vpx_malloc(sizeof(*src)*cm->mi_rows*cm->mi_cols*64);
    for (r = 0; r < bsize[pli]*cm->mi_rows; ++r) {
      for (c = 0; c < bsize[pli]*cm->mi_cols; ++c) {
        src[pli][r * stride + c] =
            xd->plane[pli].dst.buf[r * xd->plane[pli].dst.stride + c] <<
            OD_COEFF_SHIFT;
      }
    }
  }
  for (r = 0; r < cm->mi_rows; ++r) {
    for (c = 0; c < cm->mi_cols; ++c) {
      const MB_MODE_INFO *mbmi =
          &cm->mi_grid_visible[r * cm->mi_stride + c]->mbmi;
      bskip[r * cm->mi_cols + c] = mbmi->skip;
    }
  }
  for (sbr = 0; sbr < nvsb; sbr++) {
    for (sbc = 0; sbc < nhsb; sbc++) {
      int level;
      int nhb, nvb;
      nhb = VPXMIN(MI_BLOCK_SIZE, cm->mi_cols - MI_BLOCK_SIZE*sbc);
      nvb = VPXMIN(MI_BLOCK_SIZE, cm->mi_rows - MI_BLOCK_SIZE*sbr);
      for (pli = 0; pli < 3; pli++) {
        int16_t dst[MI_BLOCK_SIZE*MI_BLOCK_SIZE*8*8];
#if DERING_REFINEMENT
        level = compute_level_from_index(
            global_level,
            cm->mi_grid_visible[MI_BLOCK_SIZE*sbr*cm->mi_stride + MI_BLOCK_SIZE*sbc]->mbmi.dering_gain);
#else
        level = global_level;
#endif
        /* FIXME: This is a temporary hack that uses more conservative
           deringing for chroma. */
        if (pli) level = level*2/3;
        if (sb_all_skip(cm, sbr*MI_BLOCK_SIZE, sbc*MI_BLOCK_SIZE)) level = 0;
        od_dering(
            &OD_DERING_VTBL_C,
            dst,
            MI_BLOCK_SIZE*bsize[pli],
            &src[pli][sbr*stride*bsize[pli]*MI_BLOCK_SIZE + sbc*bsize[pli]*MI_BLOCK_SIZE],
            stride, nhb, nvb, sbc, sbr, nhsb, nvsb, dec[pli], dir, pli,
            &bskip[MI_BLOCK_SIZE*sbr*cm->mi_cols + MI_BLOCK_SIZE*sbc],
            cm->mi_cols, level<<OD_COEFF_SHIFT, OD_DERING_NO_CHECK_OVERLAP);
        for (r = 0; r < bsize[pli]*nvb; ++r) {
          for (c = 0; c < bsize[pli]*nhb; ++c) {
            xd->plane[pli].dst.buf[xd->plane[pli].dst.stride*(bsize[pli]*MI_BLOCK_SIZE*sbr + r) + sbc*bsize[pli]*MI_BLOCK_SIZE + c] =
                (dst[r * MI_BLOCK_SIZE * bsize[pli] + c] + (1<<OD_COEFF_SHIFT>>1)) >>
                OD_COEFF_SHIFT;
          }
        }
      }
    }
  }
  for (pli = 0; pli < 3; pli++) {
    vpx_free(src[pli]);
  }
  vpx_free(bskip);
}
