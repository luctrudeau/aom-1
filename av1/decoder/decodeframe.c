/*
 * Copyright (c) 2016, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#include <assert.h>
#include <stdlib.h>  // qsort()

#include "./aom_config.h"
#include "./aom_dsp_rtcd.h"
#include "./aom_scale_rtcd.h"
#include "./av1_rtcd.h"

#include "aom/aom_codec.h"
#include "aom_dsp/aom_dsp_common.h"
#include "aom_dsp/binary_codes_reader.h"
#include "aom_dsp/bitreader.h"
#include "aom_dsp/bitreader_buffer.h"
#include "aom_mem/aom_mem.h"
#include "aom_ports/aom_timer.h"
#include "aom_ports/mem.h"
#include "aom_ports/mem_ops.h"
#include "aom_scale/aom_scale.h"
#include "aom_util/aom_thread.h"

#if CONFIG_BITSTREAM_DEBUG
#include "aom_util/debug_util.h"
#endif  // CONFIG_BITSTREAM_DEBUG

#include "av1/common/alloccommon.h"
#include "av1/common/cdef.h"
#if CONFIG_INSPECTION
#include "av1/decoder/inspection.h"
#endif
#include "av1/common/common.h"
#include "av1/common/entropy.h"
#include "av1/common/entropymode.h"
#include "av1/common/entropymv.h"
#include "av1/common/idct.h"
#include "av1/common/mvref_common.h"
#include "av1/common/pred_common.h"
#include "av1/common/quant_common.h"
#include "av1/common/reconinter.h"
#include "av1/common/reconintra.h"
#if CONFIG_FRAME_SUPERRES
#include "av1/common/resize.h"
#endif  // CONFIG_FRAME_SUPERRES
#include "av1/common/seg_common.h"
#include "av1/common/thread_common.h"
#include "av1/common/tile_common.h"

#include "av1/decoder/decodeframe.h"
#include "av1/decoder/decodemv.h"
#include "av1/decoder/decoder.h"
#if CONFIG_LV_MAP
#include "av1/decoder/decodetxb.h"
#endif
#include "av1/decoder/detokenize.h"
#include "av1/decoder/dsubexp.h"
#include "av1/decoder/symbolrate.h"

#include "av1/common/warped_motion.h"

#define MAX_AV1_HEADER_SIZE 80
#define ACCT_STR __func__

#if CONFIG_CFL
#include "av1/common/cfl.h"
#endif

#if CONFIG_STRIPED_LOOP_RESTORATION && !CONFIG_LOOP_RESTORATION
#error "striped_loop_restoration requires loop_restoration"
#endif

#if CONFIG_LOOP_RESTORATION
static void loop_restoration_read_sb_coeffs(const AV1_COMMON *const cm,
                                            MACROBLOCKD *xd,
                                            aom_reader *const r, int plane,
                                            int rtile_idx);
#endif

static struct aom_read_bit_buffer *init_read_bit_buffer(
    AV1Decoder *pbi, struct aom_read_bit_buffer *rb, const uint8_t *data,
    const uint8_t *data_end, uint8_t clear_data[MAX_AV1_HEADER_SIZE]);
static int read_compressed_header(AV1Decoder *pbi, const uint8_t *data,
                                  size_t partition_size);
static size_t read_uncompressed_header(AV1Decoder *pbi,
                                       struct aom_read_bit_buffer *rb);

static int is_compound_reference_allowed(const AV1_COMMON *cm) {
  return !frame_is_intra_only(cm);
}

static void setup_compound_reference_mode(AV1_COMMON *cm) {
  cm->comp_fwd_ref[0] = LAST_FRAME;
  cm->comp_fwd_ref[1] = LAST2_FRAME;
  cm->comp_fwd_ref[2] = LAST3_FRAME;
  cm->comp_fwd_ref[3] = GOLDEN_FRAME;

  cm->comp_bwd_ref[0] = BWDREF_FRAME;
  cm->comp_bwd_ref[1] = ALTREF2_FRAME;
  cm->comp_bwd_ref[2] = ALTREF_FRAME;
}

static int read_is_valid(const uint8_t *start, size_t len, const uint8_t *end) {
  return len != 0 && len <= (size_t)(end - start);
}

static int decode_unsigned_max(struct aom_read_bit_buffer *rb, int max) {
  const int data = aom_rb_read_literal(rb, get_unsigned_bits(max));
  return data > max ? max : data;
}

#if CONFIG_SIMPLIFY_TX_MODE
static TX_MODE read_tx_mode(AV1_COMMON *cm, struct aom_read_bit_buffer *rb) {
  if (cm->all_lossless) return ONLY_4X4;
  return aom_rb_read_bit(rb) ? TX_MODE_SELECT : TX_MODE_LARGEST;
}
#else
static TX_MODE read_tx_mode(AV1_COMMON *cm, struct aom_read_bit_buffer *rb) {
  if (cm->all_lossless) return ONLY_4X4;
#if CONFIG_TX64X64
  TX_MODE tx_mode =
      aom_rb_read_bit(rb) ? TX_MODE_SELECT : aom_rb_read_literal(rb, 2);
  if (tx_mode == ALLOW_32X32) tx_mode += aom_rb_read_bit(rb);
  return tx_mode;
#else
  return aom_rb_read_bit(rb) ? TX_MODE_SELECT : aom_rb_read_literal(rb, 2);
#endif  // CONFIG_TX64X64
}
#endif  // CONFIG_SIMPLIFY_TX_MODE

static REFERENCE_MODE read_frame_reference_mode(
    const AV1_COMMON *cm, struct aom_read_bit_buffer *rb) {
  if (is_compound_reference_allowed(cm)) {
#if CONFIG_REF_ADAPT
    return aom_rb_read_bit(rb) ? REFERENCE_MODE_SELECT : SINGLE_REFERENCE;
#else
    return aom_rb_read_bit(rb)
               ? REFERENCE_MODE_SELECT
               : (aom_rb_read_bit(rb) ? COMPOUND_REFERENCE : SINGLE_REFERENCE);
#endif  // CONFIG_REF_ADAPT
  } else {
    return SINGLE_REFERENCE;
  }
}

static void inverse_transform_block(MACROBLOCKD *xd, int plane,
                                    const TX_TYPE tx_type,
                                    const TX_SIZE tx_size, uint8_t *dst,
                                    int stride, int16_t scan_line, int eob,
                                    int reduced_tx_set) {
  struct macroblockd_plane *const pd = &xd->plane[plane];
  tran_low_t *const dqcoeff = pd->dqcoeff;
  av1_inverse_transform_block(xd, dqcoeff, plane, tx_type, tx_size, dst, stride,
                              eob, reduced_tx_set);
  memset(dqcoeff, 0, (scan_line + 1) * sizeof(dqcoeff[0]));
}

static int get_block_idx(const MACROBLOCKD *xd, int plane, int row, int col) {
  const int bsize = xd->mi[0]->mbmi.sb_type;
  const struct macroblockd_plane *pd = &xd->plane[plane];
  const BLOCK_SIZE plane_bsize =
      AOMMAX(BLOCK_4X4, get_plane_block_size(bsize, pd));
  const int max_blocks_wide = max_block_wide(xd, plane_bsize, plane);
  const TX_SIZE tx_size = av1_get_tx_size(plane, xd);
  const uint8_t txh_unit = tx_size_high_unit[tx_size];
  return row * max_blocks_wide + col * txh_unit;
}

static void predict_and_reconstruct_intra_block(
    AV1_COMMON *cm, MACROBLOCKD *const xd, aom_reader *const r,
    MB_MODE_INFO *const mbmi, int plane, int row, int col, TX_SIZE tx_size) {
  PLANE_TYPE plane_type = get_plane_type(plane);
  const int block_idx = get_block_idx(xd, plane, row, col);
  av1_predict_intra_block_facade(cm, xd, plane, block_idx, col, row, tx_size);

  if (!mbmi->skip) {
    struct macroblockd_plane *const pd = &xd->plane[plane];
#if TXCOEFF_TIMER
    struct aom_usec_timer timer;
    aom_usec_timer_start(&timer);
#endif
#if CONFIG_LV_MAP
    int16_t max_scan_line = 0;
    int eob;
    av1_read_coeffs_txb_facade(cm, xd, r, row, col, block_idx, plane,
                               pd->dqcoeff, tx_size, &max_scan_line, &eob);
    // tx_type will be read out in av1_read_coeffs_txb_facade
    const TX_TYPE tx_type =
        av1_get_tx_type(plane_type, xd, row, col, block_idx, tx_size);
#else   // CONFIG_LV_MAP
    const TX_TYPE tx_type =
        av1_get_tx_type(plane_type, xd, row, col, block_idx, tx_size);
    const SCAN_ORDER *scan_order = get_scan(cm, tx_size, tx_type, mbmi);
    int16_t max_scan_line = 0;
    const int eob =
        av1_decode_block_tokens(cm, xd, plane, scan_order, col, row, tx_size,
                                tx_type, &max_scan_line, r, mbmi->segment_id);
#endif  // CONFIG_LV_MAP

#if TXCOEFF_TIMER
    aom_usec_timer_mark(&timer);
    const int64_t elapsed_time = aom_usec_timer_elapsed(&timer);
    cm->txcoeff_timer += elapsed_time;
    ++cm->txb_count;
#endif
    if (eob) {
      uint8_t *dst =
          &pd->dst.buf[(row * pd->dst.stride + col) << tx_size_wide_log2[0]];
      inverse_transform_block(xd, plane, tx_type, tx_size, dst, pd->dst.stride,
                              max_scan_line, eob, cm->reduced_tx_set_used);
    }
  }
#if CONFIG_CFL
  if (plane == AOM_PLANE_Y && xd->cfl.store_y) {
    cfl_store_tx(xd, row, col, tx_size, mbmi->sb_type);
  }
#endif  // CONFIG_CFL
}

static void decode_reconstruct_tx(AV1_COMMON *cm, MACROBLOCKD *const xd,
                                  aom_reader *r, MB_MODE_INFO *const mbmi,
                                  int plane, BLOCK_SIZE plane_bsize,
                                  int blk_row, int blk_col, int block,
                                  TX_SIZE tx_size, int *eob_total) {
  const struct macroblockd_plane *const pd = &xd->plane[plane];
  const BLOCK_SIZE bsize = txsize_to_bsize[tx_size];
  const int tx_row = blk_row >> (1 - pd->subsampling_y);
  const int tx_col = blk_col >> (1 - pd->subsampling_x);
  const TX_SIZE plane_tx_size =
      plane ? uv_txsize_lookup[bsize][mbmi->inter_tx_size[tx_row][tx_col]][0][0]
            : mbmi->inter_tx_size[tx_row][tx_col];
  // Scale to match transform block unit.
  const int max_blocks_high = max_block_high(xd, plane_bsize, plane);
  const int max_blocks_wide = max_block_wide(xd, plane_bsize, plane);

  if (blk_row >= max_blocks_high || blk_col >= max_blocks_wide) return;

  if (tx_size == plane_tx_size
#if DISABLE_VARTX_FOR_CHROMA
      || pd->subsampling_x || pd->subsampling_y
#endif  // DISABLE_VARTX_FOR_CHROMA
      ) {
    PLANE_TYPE plane_type = get_plane_type(plane);
#if TXCOEFF_TIMER
    struct aom_usec_timer timer;
    aom_usec_timer_start(&timer);
#endif
#if CONFIG_LV_MAP
    int16_t max_scan_line = 0;
    int eob;
    av1_read_coeffs_txb_facade(cm, xd, r, blk_row, blk_col, block, plane,
                               pd->dqcoeff, tx_size, &max_scan_line, &eob);
    // tx_type will be read out in av1_read_coeffs_txb_facade
    const TX_TYPE tx_type =
        av1_get_tx_type(plane_type, xd, blk_row, blk_col, block, tx_size);
#else   // CONFIG_LV_MAP
    const TX_TYPE tx_type =
        av1_get_tx_type(plane_type, xd, blk_row, blk_col, block, tx_size);
    const SCAN_ORDER *sc = get_scan(cm, tx_size, tx_type, mbmi);
    int16_t max_scan_line = 0;
    const int eob =
        av1_decode_block_tokens(cm, xd, plane, sc, blk_col, blk_row, tx_size,
                                tx_type, &max_scan_line, r, mbmi->segment_id);
#endif  // CONFIG_LV_MAP

#if TXCOEFF_TIMER
    aom_usec_timer_mark(&timer);
    const int64_t elapsed_time = aom_usec_timer_elapsed(&timer);
    cm->txcoeff_timer += elapsed_time;
    ++cm->txb_count;
#endif

    inverse_transform_block(
        xd, plane, tx_type, tx_size,
        &pd->dst
             .buf[(blk_row * pd->dst.stride + blk_col) << tx_size_wide_log2[0]],
        pd->dst.stride, max_scan_line, eob, cm->reduced_tx_set_used);
    *eob_total += eob;
  } else {
    const TX_SIZE sub_txs = sub_tx_size_map[tx_size];
    assert(IMPLIES(tx_size <= TX_4X4, sub_txs == tx_size));
    assert(IMPLIES(tx_size > TX_4X4, sub_txs < tx_size));
    const int bsw = tx_size_wide_unit[sub_txs];
    const int bsh = tx_size_high_unit[sub_txs];
    const int sub_step = bsw * bsh;

    assert(bsw > 0 && bsh > 0);

    for (int row = 0; row < tx_size_high_unit[tx_size]; row += bsh) {
      for (int col = 0; col < tx_size_wide_unit[tx_size]; col += bsw) {
        const int offsetr = blk_row + row;
        const int offsetc = blk_col + col;

        if (offsetr >= max_blocks_high || offsetc >= max_blocks_wide) continue;

        decode_reconstruct_tx(cm, xd, r, mbmi, plane, plane_bsize, offsetr,
                              offsetc, block, sub_txs, eob_total);
        block += sub_step;
      }
    }
  }
}

static void set_offsets(AV1_COMMON *const cm, MACROBLOCKD *const xd,
                        BLOCK_SIZE bsize, int mi_row, int mi_col, int bw,
                        int bh, int x_mis, int y_mis) {
  const int offset = mi_row * cm->mi_stride + mi_col;
  const TileInfo *const tile = &xd->tile;

  xd->mi = cm->mi_grid_visible + offset;
  xd->mi[0] = &cm->mi[offset];
  // TODO(slavarnway): Generate sb_type based on bwl and bhl, instead of
  // passing bsize from decode_partition().
  xd->mi[0]->mbmi.sb_type = bsize;
#if CONFIG_RD_DEBUG
  xd->mi[0]->mbmi.mi_row = mi_row;
  xd->mi[0]->mbmi.mi_col = mi_col;
#endif
#if CONFIG_CFL
  xd->cfl.mi_row = mi_row;
  xd->cfl.mi_col = mi_col;
#endif

  assert(x_mis && y_mis);
  for (int x = 1; x < x_mis; ++x) xd->mi[x] = xd->mi[0];
  int idx = cm->mi_stride;
  for (int y = 1; y < y_mis; ++y) {
    memcpy(&xd->mi[idx], &xd->mi[0], x_mis * sizeof(xd->mi[0]));
    idx += cm->mi_stride;
  }

  set_plane_n4(xd, bw, bh);
  set_skip_context(xd, mi_row, mi_col);

  // Distance of Mb to the various image edges. These are specified to 8th pel
  // as they are always compared to values that are in 1/8th pel units
  set_mi_row_col(xd, tile, mi_row, bh, mi_col, bw,
#if CONFIG_DEPENDENT_HORZTILES
                 cm->dependent_horz_tiles,
#endif  // CONFIG_DEPENDENT_HORZTILES
                 cm->mi_rows, cm->mi_cols);

  av1_setup_dst_planes(xd->plane, bsize, get_frame_new_buffer(cm), mi_row,
                       mi_col);
}

static void decode_mbmi_block(AV1Decoder *const pbi, MACROBLOCKD *const xd,
                              int mi_row, int mi_col, aom_reader *r,
#if CONFIG_EXT_PARTITION_TYPES
                              PARTITION_TYPE partition,
#endif  // CONFIG_EXT_PARTITION_TYPES
                              BLOCK_SIZE bsize) {
  AV1_COMMON *const cm = &pbi->common;
  const int bw = mi_size_wide[bsize];
  const int bh = mi_size_high[bsize];
  const int x_mis = AOMMIN(bw, cm->mi_cols - mi_col);
  const int y_mis = AOMMIN(bh, cm->mi_rows - mi_row);

#if CONFIG_ACCOUNTING
  aom_accounting_set_context(&pbi->accounting, mi_col, mi_row);
#endif
  set_offsets(cm, xd, bsize, mi_row, mi_col, bw, bh, x_mis, y_mis);
#if CONFIG_EXT_PARTITION_TYPES
  xd->mi[0]->mbmi.partition = partition;
#endif
  av1_read_mode_info(pbi, xd, mi_row, mi_col, r, x_mis, y_mis);
  if (bsize >= BLOCK_8X8 && (cm->subsampling_x || cm->subsampling_y)) {
    const BLOCK_SIZE uv_subsize =
        ss_size_lookup[bsize][cm->subsampling_x][cm->subsampling_y];
    if (uv_subsize == BLOCK_INVALID)
      aom_internal_error(xd->error_info, AOM_CODEC_CORRUPT_FRAME,
                         "Invalid block size.");
  }

  int reader_corrupted_flag = aom_reader_has_error(r);
  aom_merge_corrupted_flag(&xd->corrupted, reader_corrupted_flag);
}

static void decode_token_and_recon_block(AV1Decoder *const pbi,
                                         MACROBLOCKD *const xd, int mi_row,
                                         int mi_col, aom_reader *r,
                                         BLOCK_SIZE bsize) {
  AV1_COMMON *const cm = &pbi->common;
  const int bw = mi_size_wide[bsize];
  const int bh = mi_size_high[bsize];
  const int x_mis = AOMMIN(bw, cm->mi_cols - mi_col);
  const int y_mis = AOMMIN(bh, cm->mi_rows - mi_row);

  set_offsets(cm, xd, bsize, mi_row, mi_col, bw, bh, x_mis, y_mis);
  MB_MODE_INFO *mbmi = &xd->mi[0]->mbmi;
#if CONFIG_CFL
  CFL_CTX *const cfl = &xd->cfl;
  cfl->is_chroma_reference = is_chroma_reference(
      mi_row, mi_col, bsize, cfl->subsampling_x, cfl->subsampling_y);
#endif  // CONFIG_CFL

  if (cm->delta_q_present_flag) {
    for (int i = 0; i < MAX_SEGMENTS; i++) {
#if CONFIG_EXT_DELTA_Q
      const int current_qindex =
#if CONFIG_Q_SEGMENTATION
          av1_get_qindex(&cm->seg, i, i, xd->current_qindex);
#else
          av1_get_qindex(&cm->seg, i, xd->current_qindex);
#endif
#else
      const int current_qindex = xd->current_qindex;
#endif  // CONFIG_EXT_DELTA_Q
      for (int j = 0; j < av1_num_planes(cm); ++j) {
        const int dc_delta_q =
            j == 0 ? cm->y_dc_delta_q
                   : (j == 1 ? cm->u_dc_delta_q : cm->v_dc_delta_q);
        const int ac_delta_q =
            j == 0 ? 0 : (j == 1 ? cm->u_ac_delta_q : cm->v_ac_delta_q);
        xd->plane[j].seg_dequant_QTX[i][0] =
            av1_dc_quant_QTX(current_qindex, dc_delta_q, cm->bit_depth);
        xd->plane[j].seg_dequant_QTX[i][1] =
            av1_ac_quant_QTX(current_qindex, ac_delta_q, cm->bit_depth);
      }
    }
  }
  if (mbmi->skip) av1_reset_skip_context(xd, mi_row, mi_col, bsize);

  if (!is_inter_block(mbmi)) {
    const int num_planes = av1_num_planes(cm);
    for (int plane = 0; plane < AOMMIN(2, num_planes); ++plane) {
      if (mbmi->palette_mode_info.palette_size[plane])
        av1_decode_palette_tokens(xd, plane, r);
    }

    for (int plane = 0; plane < num_planes; ++plane) {
      const struct macroblockd_plane *const pd = &xd->plane[plane];
      const TX_SIZE tx_size = av1_get_tx_size(plane, xd);
      const int stepr = tx_size_high_unit[tx_size];
      const int stepc = tx_size_wide_unit[tx_size];
      const BLOCK_SIZE plane_bsize =
          AOMMAX(BLOCK_4X4, get_plane_block_size(bsize, pd));
      int row, col;
      const int max_blocks_wide = max_block_wide(xd, plane_bsize, plane);
      const int max_blocks_high = max_block_high(xd, plane_bsize, plane);
      if (!is_chroma_reference(mi_row, mi_col, bsize, pd->subsampling_x,
                               pd->subsampling_y))
        continue;
      int blk_row, blk_col;
      const BLOCK_SIZE max_unit_bsize = get_plane_block_size(BLOCK_64X64, pd);
      int mu_blocks_wide =
          block_size_wide[max_unit_bsize] >> tx_size_wide_log2[0];
      int mu_blocks_high =
          block_size_high[max_unit_bsize] >> tx_size_high_log2[0];
      mu_blocks_wide = AOMMIN(max_blocks_wide, mu_blocks_wide);
      mu_blocks_high = AOMMIN(max_blocks_high, mu_blocks_high);

      for (row = 0; row < max_blocks_high; row += mu_blocks_high) {
        const int unit_height = AOMMIN(mu_blocks_high + row, max_blocks_high);
        for (col = 0; col < max_blocks_wide; col += mu_blocks_wide) {
          const int unit_width = AOMMIN(mu_blocks_wide + col, max_blocks_wide);

          for (blk_row = row; blk_row < unit_height; blk_row += stepr)
            for (blk_col = col; blk_col < unit_width; blk_col += stepc)
              predict_and_reconstruct_intra_block(cm, xd, r, mbmi, plane,
                                                  blk_row, blk_col, tx_size);
        }
      }
    }
  } else {
    int ref;

    for (ref = 0; ref < 1 + has_second_ref(mbmi); ++ref) {
      const MV_REFERENCE_FRAME frame = mbmi->ref_frame[ref];
      if (frame < LAST_FRAME) {
#if CONFIG_INTRABC
        assert(is_intrabc_block(mbmi));
        assert(frame == INTRA_FRAME);
        assert(ref == 0);
#else
        assert(0);
#endif  // CONFIG_INTRABC
      } else {
        RefBuffer *ref_buf = &cm->frame_refs[frame - LAST_FRAME];

        xd->block_refs[ref] = ref_buf;
        if ((!av1_is_valid_scale(&ref_buf->sf)))
          aom_internal_error(xd->error_info, AOM_CODEC_UNSUP_BITSTREAM,
                             "Reference frame has invalid dimensions");
        av1_setup_pre_planes(xd, ref, ref_buf->buf, mi_row, mi_col,
                             &ref_buf->sf);
      }
    }

    av1_build_inter_predictors_sb(cm, xd, mi_row, mi_col, NULL, bsize);

    if (mbmi->motion_mode == OBMC_CAUSAL) {
#if CONFIG_NCOBMC
      av1_build_ncobmc_inter_predictors_sb(cm, xd, mi_row, mi_col);
#else
      av1_build_obmc_inter_predictors_sb(cm, xd, mi_row, mi_col);
#endif
    }
    // Reconstruction
    if (!mbmi->skip) {
      int eobtotal = 0;
      for (int plane = 0; plane < av1_num_planes(cm); ++plane) {
        const struct macroblockd_plane *const pd = &xd->plane[plane];
        if (!is_chroma_reference(mi_row, mi_col, bsize, pd->subsampling_x,
                                 pd->subsampling_y))
          continue;
        const BLOCK_SIZE bsizec =
            scale_chroma_bsize(bsize, pd->subsampling_x, pd->subsampling_y);
        const BLOCK_SIZE plane_bsize =
            AOMMAX(BLOCK_4X4, get_plane_block_size(bsizec, pd));
        const int max_blocks_wide = max_block_wide(xd, plane_bsize, plane);
        const int max_blocks_high = max_block_high(xd, plane_bsize, plane);
        int row, col;

        const BLOCK_SIZE max_unit_bsize = get_plane_block_size(BLOCK_64X64, pd);
        int mu_blocks_wide =
            block_size_wide[max_unit_bsize] >> tx_size_wide_log2[0];
        int mu_blocks_high =
            block_size_high[max_unit_bsize] >> tx_size_high_log2[0];

        mu_blocks_wide = AOMMIN(max_blocks_wide, mu_blocks_wide);
        mu_blocks_high = AOMMIN(max_blocks_high, mu_blocks_high);

        TX_SIZE max_tx_size = get_vartx_max_txsize(
            xd, plane_bsize, pd->subsampling_x || pd->subsampling_y);
#if DISABLE_VARTX_FOR_CHROMA == 2
        // If the luma transform size is split at least one level, split the
        // chroma by one level. Otherwise use the  largest possible trasnform
        // size for chroma.
        if (plane && (pd->subsampling_x || pd->subsampling_y)) {
          const TX_SIZE l_max_tx_size = get_vartx_max_txsize(xd, bsizec, 0);
          const int is_split =
              (l_max_tx_size != mbmi->inter_tx_size[0][0] && bsize == bsizec &&
               txsize_to_bsize[l_max_tx_size] == bsizec);
          if (is_split) max_tx_size = sub_tx_size_map[max_tx_size];
        }
#endif  // DISABLE_VARTX_FOR_CHROMA == 2
        const int bh_var_tx = tx_size_high_unit[max_tx_size];
        const int bw_var_tx = tx_size_wide_unit[max_tx_size];
        int block = 0;
        int step =
            tx_size_wide_unit[max_tx_size] * tx_size_high_unit[max_tx_size];

        for (row = 0; row < max_blocks_high; row += mu_blocks_high) {
          for (col = 0; col < max_blocks_wide; col += mu_blocks_wide) {
            int blk_row, blk_col;
            const int unit_height =
                AOMMIN(mu_blocks_high + row, max_blocks_high);
            const int unit_width =
                AOMMIN(mu_blocks_wide + col, max_blocks_wide);
            for (blk_row = row; blk_row < unit_height; blk_row += bh_var_tx) {
              for (blk_col = col; blk_col < unit_width; blk_col += bw_var_tx) {
                decode_reconstruct_tx(cm, xd, r, mbmi, plane, plane_bsize,
                                      blk_row, blk_col, block, max_tx_size,
                                      &eobtotal);
                block += step;
              }
            }
          }
        }
      }
    }
  }
#if CONFIG_CFL
  if (mbmi->uv_mode != UV_CFL_PRED) {
    if (!cfl->is_chroma_reference && is_inter_block(mbmi)) {
      cfl_store_block(xd, mbmi->sb_type, mbmi->tx_size);
    }
  }
#endif  // CONFIG_CFL

  int reader_corrupted_flag = aom_reader_has_error(r);
  aom_merge_corrupted_flag(&xd->corrupted, reader_corrupted_flag);
}

#if NC_MODE_INFO
static void detoken_and_recon_sb(AV1Decoder *const pbi, MACROBLOCKD *const xd,
                                 int mi_row, int mi_col, aom_reader *r,
                                 BLOCK_SIZE bsize) {
  AV1_COMMON *const cm = &pbi->common;
  const int hbs = mi_size_wide[bsize] >> 1;
#if CONFIG_EXT_PARTITION_TYPES
  BLOCK_SIZE bsize2 = get_subsize(bsize, PARTITION_SPLIT);
#endif
  PARTITION_TYPE partition;
  BLOCK_SIZE subsize;
  const int has_rows = (mi_row + hbs) < cm->mi_rows;
  const int has_cols = (mi_col + hbs) < cm->mi_cols;

  if (mi_row >= cm->mi_rows || mi_col >= cm->mi_cols) return;

  partition = get_partition(cm, mi_row, mi_col, bsize);
  subsize = subsize_lookup[partition][bsize];

  switch (partition) {
    case PARTITION_NONE:
      decode_token_and_recon_block(pbi, xd, mi_row, mi_col, r, bsize);
      break;
    case PARTITION_HORZ:
      decode_token_and_recon_block(pbi, xd, mi_row, mi_col, r, subsize);
      if (has_rows)
        decode_token_and_recon_block(pbi, xd, mi_row + hbs, mi_col, r, subsize);
      break;
    case PARTITION_VERT:
      decode_token_and_recon_block(pbi, xd, mi_row, mi_col, r, subsize);
      if (has_cols)
        decode_token_and_recon_block(pbi, xd, mi_row, mi_col + hbs, r, subsize);
      break;
    case PARTITION_SPLIT:
      detoken_and_recon_sb(pbi, xd, mi_row, mi_col, r, subsize);
      detoken_and_recon_sb(pbi, xd, mi_row, mi_col + hbs, r, subsize);
      detoken_and_recon_sb(pbi, xd, mi_row + hbs, mi_col, r, subsize);
      detoken_and_recon_sb(pbi, xd, mi_row + hbs, mi_col + hbs, r, subsize);
      break;
#if CONFIG_EXT_PARTITION_TYPES
#if CONFIG_EXT_PARTITION_TYPES_AB
#error NC_MODE_INFO+MOTION_VAR not yet supported for new HORZ/VERT_AB partitions
#endif
    case PARTITION_HORZ_A:
      decode_token_and_recon_block(pbi, xd, mi_row, mi_col, r, bsize2);
      decode_token_and_recon_block(pbi, xd, mi_row, mi_col + hbs, r, bsize2);
      decode_token_and_recon_block(pbi, xd, mi_row + hbs, mi_col, r, subsize);
      break;
    case PARTITION_HORZ_B:
      decode_token_and_recon_block(pbi, xd, mi_row, mi_col, r, subsize);
      decode_token_and_recon_block(pbi, xd, mi_row + hbs, mi_col, r, bsize2);
      decode_token_and_recon_block(pbi, xd, mi_row + hbs, mi_col + hbs, r,
                                   bsize2);
      break;
    case PARTITION_VERT_A:
      decode_token_and_recon_block(pbi, xd, mi_row, mi_col, r, bsize2);
      decode_token_and_recon_block(pbi, xd, mi_row + hbs, mi_col, r, bsize2);
      decode_token_and_recon_block(pbi, xd, mi_row, mi_col + hbs, r, subsize);
      break;
    case PARTITION_VERT_B:
      decode_token_and_recon_block(pbi, xd, mi_row, mi_col, r, subsize);
      decode_token_and_recon_block(pbi, xd, mi_row, mi_col + hbs, r, bsize2);
      decode_token_and_recon_block(pbi, xd, mi_row + hbs, mi_col + hbs, r,
                                   bsize2);
      break;
#endif
    default: assert(0 && "Invalid partition type");
  }
}
#endif

static void decode_block(AV1Decoder *const pbi, MACROBLOCKD *const xd,
                         int mi_row, int mi_col, aom_reader *r,
#if CONFIG_EXT_PARTITION_TYPES
                         PARTITION_TYPE partition,
#endif  // CONFIG_EXT_PARTITION_TYPES
                         BLOCK_SIZE bsize) {
  decode_mbmi_block(pbi, xd, mi_row, mi_col, r,
#if CONFIG_EXT_PARTITION_TYPES
                    partition,
#endif
                    bsize);

#if !(NC_MODE_INFO)
  decode_token_and_recon_block(pbi, xd, mi_row, mi_col, r, bsize);
#endif
}

static PARTITION_TYPE read_partition(MACROBLOCKD *xd, int mi_row, int mi_col,
                                     aom_reader *r, int has_rows, int has_cols,
                                     BLOCK_SIZE bsize) {
  const int ctx = partition_plane_context(xd, mi_row, mi_col, bsize);
  FRAME_CONTEXT *ec_ctx = xd->tile_ctx;

  if (!has_rows && !has_cols) return PARTITION_SPLIT;

  assert(ctx >= 0);
  aom_cdf_prob *partition_cdf = ec_ctx->partition_cdf[ctx];
  if (has_rows && has_cols) {
    return (PARTITION_TYPE)aom_read_symbol(
        r, partition_cdf, partition_cdf_length(bsize), ACCT_STR);
  } else if (!has_rows && has_cols) {
    assert(bsize > BLOCK_8X8);
    aom_cdf_prob cdf[2];
    partition_gather_vert_alike(cdf, partition_cdf, bsize);
    assert(cdf[1] == AOM_ICDF(CDF_PROB_TOP));
    return aom_read_cdf(r, cdf, 2, ACCT_STR) ? PARTITION_SPLIT : PARTITION_HORZ;
  } else {
    assert(has_rows && !has_cols);
    assert(bsize > BLOCK_8X8);
    aom_cdf_prob cdf[2];
    partition_gather_horz_alike(cdf, partition_cdf, bsize);
    assert(cdf[1] == AOM_ICDF(CDF_PROB_TOP));
    return aom_read_cdf(r, cdf, 2, ACCT_STR) ? PARTITION_SPLIT : PARTITION_VERT;
  }
}

// TODO(slavarnway): eliminate bsize and subsize in future commits
static void decode_partition(AV1Decoder *const pbi, MACROBLOCKD *const xd,
                             int mi_row, int mi_col, aom_reader *r,
                             BLOCK_SIZE bsize) {
  AV1_COMMON *const cm = &pbi->common;
  const int num_8x8_wh = mi_size_wide[bsize];
  const int hbs = num_8x8_wh >> 1;
#if CONFIG_EXT_PARTITION_TYPES && CONFIG_EXT_PARTITION_TYPES_AB
  const int qbs = num_8x8_wh >> 2;
#endif
  PARTITION_TYPE partition;
  BLOCK_SIZE subsize;
#if CONFIG_EXT_PARTITION_TYPES
  const int quarter_step = num_8x8_wh / 4;
#if !CONFIG_EXT_PARTITION_TYPES_AB
  BLOCK_SIZE bsize2 = get_subsize(bsize, PARTITION_SPLIT);
#endif
#endif
  const int has_rows = (mi_row + hbs) < cm->mi_rows;
  const int has_cols = (mi_col + hbs) < cm->mi_cols;

  if (mi_row >= cm->mi_rows || mi_col >= cm->mi_cols) return;

  partition = (bsize < BLOCK_8X8) ? PARTITION_NONE
                                  : read_partition(xd, mi_row, mi_col, r,
                                                   has_rows, has_cols, bsize);
  subsize = subsize_lookup[partition][bsize];  // get_subsize(bsize, partition);

  // Check the bitstream is conformant: if there is subsampling on the
  // chroma planes, subsize must subsample to a valid block size.
  const struct macroblockd_plane *const pd_u = &xd->plane[1];
  if (get_plane_block_size(subsize, pd_u) == BLOCK_INVALID) {
    aom_internal_error(&cm->error, AOM_CODEC_CORRUPT_FRAME,
                       "Block size %dx%d invalid with this subsampling mode",
                       block_size_wide[subsize], block_size_high[subsize]);
  }

#define DEC_BLOCK_STX_ARG
#if CONFIG_EXT_PARTITION_TYPES
#define DEC_BLOCK_EPT_ARG partition,
#else
#define DEC_BLOCK_EPT_ARG
#endif
#define DEC_BLOCK(db_r, db_c, db_subsize)                   \
  decode_block(pbi, xd, DEC_BLOCK_STX_ARG(db_r), (db_c), r, \
               DEC_BLOCK_EPT_ARG(db_subsize))
#define DEC_PARTITION(db_r, db_c, db_subsize) \
  decode_partition(pbi, xd, DEC_BLOCK_STX_ARG(db_r), (db_c), r, (db_subsize))

  switch (partition) {
    case PARTITION_NONE: DEC_BLOCK(mi_row, mi_col, subsize); break;
    case PARTITION_HORZ:
      DEC_BLOCK(mi_row, mi_col, subsize);
      if (has_rows) DEC_BLOCK(mi_row + hbs, mi_col, subsize);
      break;
    case PARTITION_VERT:
      DEC_BLOCK(mi_row, mi_col, subsize);
      if (has_cols) DEC_BLOCK(mi_row, mi_col + hbs, subsize);
      break;
    case PARTITION_SPLIT:
      DEC_PARTITION(mi_row, mi_col, subsize);
      DEC_PARTITION(mi_row, mi_col + hbs, subsize);
      DEC_PARTITION(mi_row + hbs, mi_col, subsize);
      DEC_PARTITION(mi_row + hbs, mi_col + hbs, subsize);
      break;
#if CONFIG_EXT_PARTITION_TYPES
#if CONFIG_EXT_PARTITION_TYPES_AB
    case PARTITION_HORZ_A:
      DEC_BLOCK(mi_row, mi_col, get_subsize(bsize, PARTITION_HORZ_4));
      DEC_BLOCK(mi_row + qbs, mi_col, get_subsize(bsize, PARTITION_HORZ_4));
      DEC_BLOCK(mi_row + hbs, mi_col, subsize);
      break;
    case PARTITION_HORZ_B:
      DEC_BLOCK(mi_row, mi_col, subsize);
      DEC_BLOCK(mi_row + hbs, mi_col, get_subsize(bsize, PARTITION_HORZ_4));
      if (mi_row + 3 * qbs < cm->mi_rows)
        DEC_BLOCK(mi_row + 3 * qbs, mi_col,
                  get_subsize(bsize, PARTITION_HORZ_4));
      break;
    case PARTITION_VERT_A:
      DEC_BLOCK(mi_row, mi_col, get_subsize(bsize, PARTITION_VERT_4));
      DEC_BLOCK(mi_row, mi_col + qbs, get_subsize(bsize, PARTITION_VERT_4));
      DEC_BLOCK(mi_row, mi_col + hbs, subsize);
      break;
    case PARTITION_VERT_B:
      DEC_BLOCK(mi_row, mi_col, subsize);
      DEC_BLOCK(mi_row, mi_col + hbs, get_subsize(bsize, PARTITION_VERT_4));
      if (mi_col + 3 * qbs < cm->mi_cols)
        DEC_BLOCK(mi_row, mi_col + 3 * qbs,
                  get_subsize(bsize, PARTITION_VERT_4));
      break;
#else
    case PARTITION_HORZ_A:
      DEC_BLOCK(mi_row, mi_col, bsize2);
      DEC_BLOCK(mi_row, mi_col + hbs, bsize2);
      DEC_BLOCK(mi_row + hbs, mi_col, subsize);
      break;
    case PARTITION_HORZ_B:
      DEC_BLOCK(mi_row, mi_col, subsize);
      DEC_BLOCK(mi_row + hbs, mi_col, bsize2);
      DEC_BLOCK(mi_row + hbs, mi_col + hbs, bsize2);
      break;
    case PARTITION_VERT_A:
      DEC_BLOCK(mi_row, mi_col, bsize2);
      DEC_BLOCK(mi_row + hbs, mi_col, bsize2);
      DEC_BLOCK(mi_row, mi_col + hbs, subsize);
      break;
    case PARTITION_VERT_B:
      DEC_BLOCK(mi_row, mi_col, subsize);
      DEC_BLOCK(mi_row, mi_col + hbs, bsize2);
      DEC_BLOCK(mi_row + hbs, mi_col + hbs, bsize2);
      break;
#endif
    case PARTITION_HORZ_4:
      for (int i = 0; i < 4; ++i) {
        int this_mi_row = mi_row + i * quarter_step;
        if (i > 0 && this_mi_row >= cm->mi_rows) break;
        DEC_BLOCK(this_mi_row, mi_col, subsize);
      }
      break;
    case PARTITION_VERT_4:
      for (int i = 0; i < 4; ++i) {
        int this_mi_col = mi_col + i * quarter_step;
        if (i > 0 && this_mi_col >= cm->mi_cols) break;
        DEC_BLOCK(mi_row, this_mi_col, subsize);
      }
      break;
#endif  // CONFIG_EXT_PARTITION_TYPES
    default: assert(0 && "Invalid partition type");
  }

#undef DEC_PARTITION
#undef DEC_BLOCK
#undef DEC_BLOCK_EPT_ARG
#undef DEC_BLOCK_STX_ARG

#if CONFIG_EXT_PARTITION_TYPES
  update_ext_partition_context(xd, mi_row, mi_col, subsize, bsize, partition);
#else
  // update partition context
  if (bsize >= BLOCK_8X8 &&
      (bsize == BLOCK_8X8 || partition != PARTITION_SPLIT))
    update_partition_context(xd, mi_row, mi_col, subsize, bsize);
#endif  // CONFIG_EXT_PARTITION_TYPES

#if CONFIG_LPF_SB
  if (bsize == cm->sb_size) {
    int filt_lvl;
    if (mi_row == 0 && mi_col == 0) {
      filt_lvl = aom_read_literal(r, 6, ACCT_STR);
      cm->mi_grid_visible[0]->mbmi.reuse_sb_lvl = 0;
      cm->mi_grid_visible[0]->mbmi.delta = 0;
      cm->mi_grid_visible[0]->mbmi.sign = 0;
    } else {
      int prev_mi_row, prev_mi_col;
      if (mi_col - MAX_MIB_SIZE < 0) {
        prev_mi_row = mi_row - MAX_MIB_SIZE;
        prev_mi_col = mi_col;
      } else {
        prev_mi_row = mi_row;
        prev_mi_col = mi_col - MAX_MIB_SIZE;
      }

      MB_MODE_INFO *curr_mbmi =
          &cm->mi_grid_visible[mi_row * cm->mi_stride + mi_col]->mbmi;
      MB_MODE_INFO *prev_mbmi =
          &cm->mi_grid_visible[prev_mi_row * cm->mi_stride + prev_mi_col]->mbmi;
      const uint8_t prev_lvl = prev_mbmi->filt_lvl;

      const int reuse_ctx = prev_mbmi->reuse_sb_lvl;
      const int reuse_prev_lvl = aom_read_symbol(
          r, xd->tile_ctx->lpf_reuse_cdf[reuse_ctx], 2, ACCT_STR);
      curr_mbmi->reuse_sb_lvl = reuse_prev_lvl;

      if (reuse_prev_lvl) {
        filt_lvl = prev_lvl;
        curr_mbmi->delta = 0;
        curr_mbmi->sign = 0;
      } else {
        const int delta_ctx = prev_mbmi->delta;
        unsigned int delta = aom_read_symbol(
            r, xd->tile_ctx->lpf_delta_cdf[delta_ctx], DELTA_RANGE, ACCT_STR);
        curr_mbmi->delta = delta;
        delta *= LPF_STEP;

        if (delta) {
          const int sign_ctx = prev_mbmi->sign;
          const int sign = aom_read_symbol(
              r, xd->tile_ctx->lpf_sign_cdf[reuse_ctx][sign_ctx], 2, ACCT_STR);
          curr_mbmi->sign = sign;
          filt_lvl = sign ? prev_lvl + delta : prev_lvl - delta;
        } else {
          filt_lvl = prev_lvl;
          curr_mbmi->sign = 0;
        }
      }
    }

    av1_loop_filter_sb_level_init(cm, mi_row, mi_col, filt_lvl);
  }
#endif

#if CONFIG_LOOP_RESTORATION
  for (int plane = 0; plane < av1_num_planes(cm); ++plane) {
    int rcol0, rcol1, rrow0, rrow1, tile_tl_idx;
    if (av1_loop_restoration_corners_in_sb(cm, plane, mi_row, mi_col, bsize,
                                           &rcol0, &rcol1, &rrow0, &rrow1,
                                           &tile_tl_idx)) {
      const int rstride = cm->rst_info[plane].horz_units_per_tile;
      for (int rrow = rrow0; rrow < rrow1; ++rrow) {
        for (int rcol = rcol0; rcol < rcol1; ++rcol) {
          const int rtile_idx = tile_tl_idx + rcol + rrow * rstride;
          loop_restoration_read_sb_coeffs(cm, xd, r, plane, rtile_idx);
        }
      }
    }
  }
#endif
}

static void setup_bool_decoder(const uint8_t *data, const uint8_t *data_end,
                               const size_t read_size,
                               struct aom_internal_error_info *error_info,
                               aom_reader *r, uint8_t allow_update_cdf,
#if CONFIG_ANS && ANS_MAX_SYMBOLS
                               int window_size,
#endif  // CONFIG_ANS && ANS_MAX_SYMBOLS
                               aom_decrypt_cb decrypt_cb, void *decrypt_state) {
  // Validate the calculated partition length. If the buffer
  // described by the partition can't be fully read, then restrict
  // it to the portion that can be (for EC mode) or throw an error.
  if (!read_is_valid(data, read_size, data_end))
    aom_internal_error(error_info, AOM_CODEC_CORRUPT_FRAME,
                       "Truncated packet or corrupt tile length");

#if CONFIG_ANS && ANS_MAX_SYMBOLS
  r->window_size = window_size;
#endif
  if (aom_reader_init(r, data, read_size, decrypt_cb, decrypt_state))
    aom_internal_error(error_info, AOM_CODEC_MEM_ERROR,
                       "Failed to allocate bool decoder %d", 1);

  r->allow_update_cdf = allow_update_cdf;
}

static void setup_segmentation(AV1_COMMON *const cm,
                               struct aom_read_bit_buffer *rb) {
  struct segmentation *const seg = &cm->seg;

  seg->update_map = 0;
  seg->update_data = 0;
  seg->temporal_update = 0;

  seg->enabled = aom_rb_read_bit(rb);
  if (!seg->enabled) return;

  // Segmentation map update
  if (frame_is_intra_only(cm) || cm->error_resilient_mode) {
    seg->update_map = 1;
  } else {
    seg->update_map = aom_rb_read_bit(rb);
  }
  if (seg->update_map) {
    if (frame_is_intra_only(cm) || cm->error_resilient_mode) {
      seg->temporal_update = 0;
    } else {
      seg->temporal_update = aom_rb_read_bit(rb);
    }
  }

  // Segmentation data update
  seg->update_data = aom_rb_read_bit(rb);
  if (seg->update_data) {
    av1_clearall_segfeatures(seg);

    for (int i = 0; i < MAX_SEGMENTS; i++) {
      for (int j = 0; j < SEG_LVL_MAX; j++) {
        int data = 0;
        const int feature_enabled = aom_rb_read_bit(rb);
        if (feature_enabled) {
          av1_enable_segfeature(seg, i, j);
          data = decode_unsigned_max(rb, av1_seg_feature_data_max(j));
          if (av1_is_segfeature_signed(j))
            data = aom_rb_read_bit(rb) ? -data : data;
        }
        av1_set_segdata(seg, i, j, data);
      }
    }
  }
}

#if CONFIG_Q_SEGMENTATION
static void setup_q_segmentation(AV1_COMMON *const cm,
                                 struct aom_read_bit_buffer *rb) {
  struct segmentation *const seg = &cm->seg;

  for (int i = 0; i < MAX_SEGMENTS; i++) {
    if (segfeature_active(seg, i, SEG_LVL_ALT_Q)) {
      seg->q_lvls = 0;
      return;
    }
  }

  if (!aom_rb_read_bit(rb)) {
    seg->q_lvls = 0;
    return;
  }

  seg->q_lvls = decode_unsigned_max(rb, MAX_SEGMENTS);

  for (int i = 0; i < seg->q_lvls; i++) {
    int val = decode_unsigned_max(rb, MAXQ);
    val *= 1 - 2 * aom_rb_read_bit(rb);
    seg->q_delta[i] = val;
  }
}
#endif

#if CONFIG_LOOP_RESTORATION
static void decode_restoration_mode(AV1_COMMON *cm,
                                    struct aom_read_bit_buffer *rb) {
#if CONFIG_INTRABC
  if (cm->allow_intrabc && NO_FILTER_FOR_IBC) return;
#endif  // CONFIG_INTRABC
  int all_none = 1, chroma_none = 1;
  for (int p = 0; p < av1_num_planes(cm); ++p) {
    RestorationInfo *rsi = &cm->rst_info[p];
    if (aom_rb_read_bit(rb)) {
      rsi->frame_restoration_type =
          aom_rb_read_bit(rb) ? RESTORE_SGRPROJ : RESTORE_WIENER;
    } else {
      rsi->frame_restoration_type =
          aom_rb_read_bit(rb) ? RESTORE_SWITCHABLE : RESTORE_NONE;
    }
    if (rsi->frame_restoration_type != RESTORE_NONE) {
      all_none = 0;
      chroma_none &= p == 0;
    }
  }
  if (!all_none) {
    const int qsize = RESTORATION_TILESIZE_MAX >> 2;
    for (int p = 0; p < MAX_MB_PLANE; ++p)
      cm->rst_info[p].restoration_unit_size = qsize;

    RestorationInfo *rsi = &cm->rst_info[0];
    rsi->restoration_unit_size <<= aom_rb_read_bit(rb);
    if (rsi->restoration_unit_size != qsize) {
      rsi->restoration_unit_size <<= aom_rb_read_bit(rb);
    }
  } else {
    const int size = RESTORATION_TILESIZE_MAX;
    for (int p = 0; p < MAX_MB_PLANE; ++p)
      cm->rst_info[p].restoration_unit_size = size;
  }

  if (av1_num_planes(cm) > 1) {
    int s = AOMMIN(cm->subsampling_x, cm->subsampling_y);
    if (s && !chroma_none) {
      cm->rst_info[1].restoration_unit_size =
          cm->rst_info[0].restoration_unit_size >> (aom_rb_read_bit(rb) * s);
    } else {
      cm->rst_info[1].restoration_unit_size =
          cm->rst_info[0].restoration_unit_size;
    }
    cm->rst_info[2].restoration_unit_size =
        cm->rst_info[1].restoration_unit_size;
  }
}

static void read_wiener_filter(int wiener_win, WienerInfo *wiener_info,
                               WienerInfo *ref_wiener_info, aom_reader *rb) {
  memset(wiener_info->vfilter, 0, sizeof(wiener_info->vfilter));
  memset(wiener_info->hfilter, 0, sizeof(wiener_info->hfilter));

  if (wiener_win == WIENER_WIN)
    wiener_info->vfilter[0] = wiener_info->vfilter[WIENER_WIN - 1] =
        aom_read_primitive_refsubexpfin(
            rb, WIENER_FILT_TAP0_MAXV - WIENER_FILT_TAP0_MINV + 1,
            WIENER_FILT_TAP0_SUBEXP_K,
            ref_wiener_info->vfilter[0] - WIENER_FILT_TAP0_MINV, ACCT_STR) +
        WIENER_FILT_TAP0_MINV;
  else
    wiener_info->vfilter[0] = wiener_info->vfilter[WIENER_WIN - 1] = 0;
  wiener_info->vfilter[1] = wiener_info->vfilter[WIENER_WIN - 2] =
      aom_read_primitive_refsubexpfin(
          rb, WIENER_FILT_TAP1_MAXV - WIENER_FILT_TAP1_MINV + 1,
          WIENER_FILT_TAP1_SUBEXP_K,
          ref_wiener_info->vfilter[1] - WIENER_FILT_TAP1_MINV, ACCT_STR) +
      WIENER_FILT_TAP1_MINV;
  wiener_info->vfilter[2] = wiener_info->vfilter[WIENER_WIN - 3] =
      aom_read_primitive_refsubexpfin(
          rb, WIENER_FILT_TAP2_MAXV - WIENER_FILT_TAP2_MINV + 1,
          WIENER_FILT_TAP2_SUBEXP_K,
          ref_wiener_info->vfilter[2] - WIENER_FILT_TAP2_MINV, ACCT_STR) +
      WIENER_FILT_TAP2_MINV;
  // The central element has an implicit +WIENER_FILT_STEP
  wiener_info->vfilter[WIENER_HALFWIN] =
      -2 * (wiener_info->vfilter[0] + wiener_info->vfilter[1] +
            wiener_info->vfilter[2]);

  if (wiener_win == WIENER_WIN)
    wiener_info->hfilter[0] = wiener_info->hfilter[WIENER_WIN - 1] =
        aom_read_primitive_refsubexpfin(
            rb, WIENER_FILT_TAP0_MAXV - WIENER_FILT_TAP0_MINV + 1,
            WIENER_FILT_TAP0_SUBEXP_K,
            ref_wiener_info->hfilter[0] - WIENER_FILT_TAP0_MINV, ACCT_STR) +
        WIENER_FILT_TAP0_MINV;
  else
    wiener_info->hfilter[0] = wiener_info->hfilter[WIENER_WIN - 1] = 0;
  wiener_info->hfilter[1] = wiener_info->hfilter[WIENER_WIN - 2] =
      aom_read_primitive_refsubexpfin(
          rb, WIENER_FILT_TAP1_MAXV - WIENER_FILT_TAP1_MINV + 1,
          WIENER_FILT_TAP1_SUBEXP_K,
          ref_wiener_info->hfilter[1] - WIENER_FILT_TAP1_MINV, ACCT_STR) +
      WIENER_FILT_TAP1_MINV;
  wiener_info->hfilter[2] = wiener_info->hfilter[WIENER_WIN - 3] =
      aom_read_primitive_refsubexpfin(
          rb, WIENER_FILT_TAP2_MAXV - WIENER_FILT_TAP2_MINV + 1,
          WIENER_FILT_TAP2_SUBEXP_K,
          ref_wiener_info->hfilter[2] - WIENER_FILT_TAP2_MINV, ACCT_STR) +
      WIENER_FILT_TAP2_MINV;
  // The central element has an implicit +WIENER_FILT_STEP
  wiener_info->hfilter[WIENER_HALFWIN] =
      -2 * (wiener_info->hfilter[0] + wiener_info->hfilter[1] +
            wiener_info->hfilter[2]);
  memcpy(ref_wiener_info, wiener_info, sizeof(*wiener_info));
}

static void read_sgrproj_filter(SgrprojInfo *sgrproj_info,
                                SgrprojInfo *ref_sgrproj_info, aom_reader *rb) {
  sgrproj_info->ep = aom_read_literal(rb, SGRPROJ_PARAMS_BITS, ACCT_STR);
  sgrproj_info->xqd[0] =
      aom_read_primitive_refsubexpfin(
          rb, SGRPROJ_PRJ_MAX0 - SGRPROJ_PRJ_MIN0 + 1, SGRPROJ_PRJ_SUBEXP_K,
          ref_sgrproj_info->xqd[0] - SGRPROJ_PRJ_MIN0, ACCT_STR) +
      SGRPROJ_PRJ_MIN0;
  sgrproj_info->xqd[1] =
      aom_read_primitive_refsubexpfin(
          rb, SGRPROJ_PRJ_MAX1 - SGRPROJ_PRJ_MIN1 + 1, SGRPROJ_PRJ_SUBEXP_K,
          ref_sgrproj_info->xqd[1] - SGRPROJ_PRJ_MIN1, ACCT_STR) +
      SGRPROJ_PRJ_MIN1;
  memcpy(ref_sgrproj_info, sgrproj_info, sizeof(*sgrproj_info));
}

static void loop_restoration_read_sb_coeffs(const AV1_COMMON *const cm,
                                            MACROBLOCKD *xd,
                                            aom_reader *const r, int plane,
                                            int rtile_idx) {
  const RestorationInfo *rsi = &cm->rst_info[plane];
  RestorationUnitInfo *rui = &rsi->unit_info[rtile_idx];
  if (rsi->frame_restoration_type == RESTORE_NONE) return;

  const int wiener_win = (plane > 0) ? WIENER_WIN_CHROMA : WIENER_WIN;
  WienerInfo *wiener_info = xd->wiener_info + plane;
  SgrprojInfo *sgrproj_info = xd->sgrproj_info + plane;

  if (rsi->frame_restoration_type == RESTORE_SWITCHABLE) {
    rui->restoration_type =
        aom_read_symbol(r, xd->tile_ctx->switchable_restore_cdf,
                        RESTORE_SWITCHABLE_TYPES, ACCT_STR);
    switch (rui->restoration_type) {
      case RESTORE_WIENER:
        read_wiener_filter(wiener_win, &rui->wiener_info, wiener_info, r);
        break;
      case RESTORE_SGRPROJ:
        read_sgrproj_filter(&rui->sgrproj_info, sgrproj_info, r);
        break;
      default: assert(rui->restoration_type == RESTORE_NONE); break;
    }
  } else if (rsi->frame_restoration_type == RESTORE_WIENER) {
    if (aom_read_symbol(r, xd->tile_ctx->wiener_restore_cdf, 2, ACCT_STR)) {
      rui->restoration_type = RESTORE_WIENER;
      read_wiener_filter(wiener_win, &rui->wiener_info, wiener_info, r);
    } else {
      rui->restoration_type = RESTORE_NONE;
    }
  } else if (rsi->frame_restoration_type == RESTORE_SGRPROJ) {
    if (aom_read_symbol(r, xd->tile_ctx->sgrproj_restore_cdf, 2, ACCT_STR)) {
      rui->restoration_type = RESTORE_SGRPROJ;
      read_sgrproj_filter(&rui->sgrproj_info, sgrproj_info, r);
    } else {
      rui->restoration_type = RESTORE_NONE;
    }
  }
}
#endif  // CONFIG_LOOP_RESTORATION

static void setup_loopfilter(AV1_COMMON *cm, struct aom_read_bit_buffer *rb) {
#if CONFIG_INTRABC
  if (cm->allow_intrabc && NO_FILTER_FOR_IBC) return;
#endif  // CONFIG_INTRABC
  struct loopfilter *lf = &cm->lf;
#if !CONFIG_LPF_SB
#if CONFIG_LOOPFILTER_LEVEL
  lf->filter_level[0] = aom_rb_read_literal(rb, 6);
  lf->filter_level[1] = aom_rb_read_literal(rb, 6);
  if (lf->filter_level[0] || lf->filter_level[1]) {
    lf->filter_level_u = aom_rb_read_literal(rb, 6);
    lf->filter_level_v = aom_rb_read_literal(rb, 6);
  }
#else
  lf->filter_level = aom_rb_read_literal(rb, 6);
#endif
#endif  // CONFIG_LPF_SB
  lf->sharpness_level = aom_rb_read_literal(rb, 3);

  // Read in loop filter deltas applied at the MB level based on mode or ref
  // frame.
  lf->mode_ref_delta_update = 0;

  lf->mode_ref_delta_enabled = aom_rb_read_bit(rb);
  if (lf->mode_ref_delta_enabled) {
    lf->mode_ref_delta_update = aom_rb_read_bit(rb);
    if (lf->mode_ref_delta_update) {
      for (int i = 0; i < TOTAL_REFS_PER_FRAME; i++)
        if (aom_rb_read_bit(rb))
          lf->ref_deltas[i] = aom_rb_read_inv_signed_literal(rb, 6);

      for (int i = 0; i < MAX_MODE_LF_DELTAS; i++)
        if (aom_rb_read_bit(rb))
          lf->mode_deltas[i] = aom_rb_read_inv_signed_literal(rb, 6);
    }
  }
}

static void setup_cdef(AV1_COMMON *cm, struct aom_read_bit_buffer *rb) {
#if CONFIG_INTRABC
  if (cm->allow_intrabc && NO_FILTER_FOR_IBC) return;
#endif  // CONFIG_INTRABC
#if CONFIG_CDEF_SINGLEPASS
  cm->cdef_pri_damping = cm->cdef_sec_damping = aom_rb_read_literal(rb, 2) + 3;
#else
  cm->cdef_pri_damping = aom_rb_read_literal(rb, 1) + 5;
  cm->cdef_sec_damping = aom_rb_read_literal(rb, 2) + 3;
#endif
  cm->cdef_bits = aom_rb_read_literal(rb, 2);
  cm->nb_cdef_strengths = 1 << cm->cdef_bits;
  for (int i = 0; i < cm->nb_cdef_strengths; i++) {
    cm->cdef_strengths[i] = aom_rb_read_literal(rb, CDEF_STRENGTH_BITS);
    cm->cdef_uv_strengths[i] = cm->subsampling_x == cm->subsampling_y
                                   ? aom_rb_read_literal(rb, CDEF_STRENGTH_BITS)
                                   : 0;
  }
}

static INLINE int read_delta_q(struct aom_read_bit_buffer *rb) {
  return aom_rb_read_bit(rb) ? aom_rb_read_inv_signed_literal(rb, 6) : 0;
}

static void setup_quantization(AV1_COMMON *const cm,
                               struct aom_read_bit_buffer *rb) {
  cm->base_qindex = aom_rb_read_literal(rb, QINDEX_BITS);
  cm->y_dc_delta_q = read_delta_q(rb);
  cm->u_dc_delta_q = read_delta_q(rb);
  cm->u_ac_delta_q = read_delta_q(rb);
  cm->v_dc_delta_q = cm->u_dc_delta_q;
  cm->v_ac_delta_q = cm->u_ac_delta_q;
  cm->dequant_bit_depth = cm->bit_depth;
#if CONFIG_AOM_QM
  cm->using_qmatrix = aom_rb_read_bit(rb);
  if (cm->using_qmatrix) {
    cm->min_qmlevel = aom_rb_read_literal(rb, QM_LEVEL_BITS);
    cm->max_qmlevel = aom_rb_read_literal(rb, QM_LEVEL_BITS);
  } else {
    cm->min_qmlevel = 0;
    cm->max_qmlevel = 0;
  }
#endif
}

// Build y/uv dequant values based on segmentation.
static void setup_segmentation_dequant(AV1_COMMON *const cm) {
#if CONFIG_AOM_QM
  const int using_qm = cm->using_qmatrix;
  const int minqm = cm->min_qmlevel;
  const int maxqm = cm->max_qmlevel;
#endif
  // When segmentation is disabled, only the first value is used.  The
  // remaining are don't cares.
  const int max_segments = cm->seg.enabled ? MAX_SEGMENTS : 1;
  for (int i = 0; i < max_segments; ++i) {
#if CONFIG_Q_SEGMENTATION
    const int qindex = av1_get_qindex(&cm->seg, i, i, cm->base_qindex);
#else
    const int qindex = av1_get_qindex(&cm->seg, i, cm->base_qindex);
#endif
    cm->y_dequant_QTX[i][0] =
        av1_dc_quant_QTX(qindex, cm->y_dc_delta_q, cm->bit_depth);
    cm->y_dequant_QTX[i][1] = av1_ac_quant_QTX(qindex, 0, cm->bit_depth);
    cm->u_dequant_QTX[i][0] =
        av1_dc_quant_QTX(qindex, cm->u_dc_delta_q, cm->bit_depth);
    cm->u_dequant_QTX[i][1] =
        av1_ac_quant_QTX(qindex, cm->u_ac_delta_q, cm->bit_depth);
    cm->v_dequant_QTX[i][0] =
        av1_dc_quant_QTX(qindex, cm->v_dc_delta_q, cm->bit_depth);
    cm->v_dequant_QTX[i][1] =
        av1_ac_quant_QTX(qindex, cm->v_ac_delta_q, cm->bit_depth);
#if CONFIG_AOM_QM
    const int lossless = qindex == 0 && cm->y_dc_delta_q == 0 &&
                         cm->u_dc_delta_q == 0 && cm->u_ac_delta_q == 0 &&
                         cm->v_dc_delta_q == 0 && cm->v_ac_delta_q == 0;
    // NB: depends on base index so there is only 1 set per frame
    // No quant weighting when lossless or signalled not using QM
    const int qmlevel = (lossless || using_qm == 0)
                            ? NUM_QM_LEVELS - 1
                            : aom_get_qmlevel(cm->base_qindex, minqm, maxqm);
    for (int j = 0; j < TX_SIZES_ALL; ++j) {
      cm->y_iqmatrix[i][j] = aom_iqmatrix(cm, qmlevel, AOM_PLANE_Y, j);
      cm->u_iqmatrix[i][j] = aom_iqmatrix(cm, qmlevel, AOM_PLANE_U, j);
      cm->v_iqmatrix[i][j] = aom_iqmatrix(cm, qmlevel, AOM_PLANE_V, j);
    }
#endif  // CONFIG_AOM_QM
#if CONFIG_NEW_QUANT
    for (int dq = 0; dq < QUANT_PROFILES; dq++) {
      for (int b = 0; b < COEF_BANDS; ++b) {
        av1_get_dequant_val_nuq(cm->y_dequant_QTX[i][b != 0], b,
                                cm->y_dequant_nuq_QTX[i][dq][b], NULL, dq);
        av1_get_dequant_val_nuq(cm->u_dequant_QTX[i][b != 0], b,
                                cm->u_dequant_nuq_QTX[i][dq][b], NULL, dq);
        av1_get_dequant_val_nuq(cm->v_dequant_QTX[i][b != 0], b,
                                cm->v_dequant_nuq_QTX[i][dq][b], NULL, dq);
      }
    }
#endif  //  CONFIG_NEW_QUANT
  }
}

static InterpFilter read_frame_interp_filter(struct aom_read_bit_buffer *rb) {
  return aom_rb_read_bit(rb) ? SWITCHABLE
                             : aom_rb_read_literal(rb, LOG_SWITCHABLE_FILTERS);
}

static void setup_render_size(AV1_COMMON *cm, struct aom_read_bit_buffer *rb) {
#if CONFIG_FRAME_SUPERRES
  cm->render_width = cm->superres_upscaled_width;
  cm->render_height = cm->superres_upscaled_height;
#else
  cm->render_width = cm->width;
  cm->render_height = cm->height;
#endif  // CONFIG_FRAME_SUPERRES
  if (aom_rb_read_bit(rb))
#if CONFIG_FRAME_SIZE
    av1_read_frame_size(rb, 16, 16, &cm->render_width, &cm->render_height);
#else
    av1_read_frame_size(rb, &cm->render_width, &cm->render_height);
#endif
}

#if CONFIG_FRAME_SUPERRES
// TODO(afergs): make "struct aom_read_bit_buffer *const rb"?
static void setup_superres(AV1_COMMON *const cm, struct aom_read_bit_buffer *rb,
                           int *width, int *height) {
  cm->superres_upscaled_width = *width;
  cm->superres_upscaled_height = *height;
  if (aom_rb_read_bit(rb)) {
    cm->superres_scale_denominator =
        (uint8_t)aom_rb_read_literal(rb, SUPERRES_SCALE_BITS);
    cm->superres_scale_denominator += SUPERRES_SCALE_DENOMINATOR_MIN;
    // Don't edit cm->width or cm->height directly, or the buffers won't get
    // resized correctly
    av1_calculate_scaled_superres_size(width, height,
                                       cm->superres_scale_denominator);
  } else {
    // 1:1 scaling - ie. no scaling, scale not provided
    cm->superres_scale_denominator = SCALE_NUMERATOR;
  }
}
#endif  // CONFIG_FRAME_SUPERRES
#if CONFIG_SEGMENT_PRED_LAST
static void resize_segmap_buffer(AV1_COMMON *cm) {
  aom_free(cm->cur_frame->seg_map);
  cm->cur_frame->mi_rows = cm->mi_rows;
  cm->cur_frame->mi_cols = cm->mi_cols;
  CHECK_MEM_ERROR(cm, cm->cur_frame->seg_map,
                  (uint8_t *)aom_calloc(cm->mi_rows * cm->mi_cols,
                                        sizeof(*cm->cur_frame->seg_map)));
}
#endif
static void resize_context_buffers(AV1_COMMON *cm, int width, int height) {
#if CONFIG_SIZE_LIMIT
  if (width > DECODE_WIDTH_LIMIT || height > DECODE_HEIGHT_LIMIT)
    aom_internal_error(&cm->error, AOM_CODEC_CORRUPT_FRAME,
                       "Dimensions of %dx%d beyond allowed size of %dx%d.",
                       width, height, DECODE_WIDTH_LIMIT, DECODE_HEIGHT_LIMIT);
#endif
  if (cm->width != width || cm->height != height) {
    const int new_mi_rows =
        ALIGN_POWER_OF_TWO(height, MI_SIZE_LOG2) >> MI_SIZE_LOG2;
    const int new_mi_cols =
        ALIGN_POWER_OF_TWO(width, MI_SIZE_LOG2) >> MI_SIZE_LOG2;

    // Allocations in av1_alloc_context_buffers() depend on individual
    // dimensions as well as the overall size.
    if (new_mi_cols > cm->mi_cols || new_mi_rows > cm->mi_rows) {
      if (av1_alloc_context_buffers(cm, width, height))
        aom_internal_error(&cm->error, AOM_CODEC_MEM_ERROR,
                           "Failed to allocate context buffers");
    } else {
      av1_set_mb_mi(cm, width, height);
    }
    av1_init_context_buffers(cm);
    cm->width = width;
    cm->height = height;
  }

  ensure_mv_buffer(cm->cur_frame, cm);
#if CONFIG_SEGMENT_PRED_LAST
  if (cm->cur_frame->seg_map == NULL || cm->mi_rows > cm->cur_frame->mi_rows ||
      cm->mi_cols > cm->cur_frame->mi_cols) {
    resize_segmap_buffer(cm);
  }
#endif
  cm->cur_frame->width = cm->width;
  cm->cur_frame->height = cm->height;
}

#if CONFIG_FRAME_SIZE
static void setup_frame_size(AV1_COMMON *cm, int frame_size_override_flag,
                             struct aom_read_bit_buffer *rb) {
#else
static void setup_frame_size(AV1_COMMON *cm, struct aom_read_bit_buffer *rb) {
#endif
  int width, height;
  BufferPool *const pool = cm->buffer_pool;
#if CONFIG_FRAME_SIZE
  if (frame_size_override_flag) {
    int num_bits_width = cm->seq_params.num_bits_width;
    int num_bits_height = cm->seq_params.num_bits_height;
    av1_read_frame_size(rb, num_bits_width, num_bits_height, &width, &height);
    if (width > cm->seq_params.max_frame_width ||
        height > cm->seq_params.max_frame_height) {
      aom_internal_error(&cm->error, AOM_CODEC_CORRUPT_FRAME,
                         "Frame dimensions are larger than the maximum values");
    }
  } else {
    width = cm->seq_params.max_frame_width;
    height = cm->seq_params.max_frame_height;
  }
#else
  av1_read_frame_size(rb, &width, &height);
#endif
#if CONFIG_FRAME_SUPERRES
  setup_superres(cm, rb, &width, &height);
#endif  // CONFIG_FRAME_SUPERRES
  resize_context_buffers(cm, width, height);
  setup_render_size(cm, rb);

  lock_buffer_pool(pool);
  if (aom_realloc_frame_buffer(
          get_frame_new_buffer(cm), cm->width, cm->height, cm->subsampling_x,
          cm->subsampling_y,
#if CONFIG_HIGHBITDEPTH
          cm->use_highbitdepth,
#endif
          AOM_BORDER_IN_PIXELS, cm->byte_alignment,
          &pool->frame_bufs[cm->new_fb_idx].raw_frame_buffer, pool->get_fb_cb,
          pool->cb_priv)) {
    unlock_buffer_pool(pool);
    aom_internal_error(&cm->error, AOM_CODEC_MEM_ERROR,
                       "Failed to allocate frame buffer");
  }
  unlock_buffer_pool(pool);

  pool->frame_bufs[cm->new_fb_idx].buf.subsampling_x = cm->subsampling_x;
  pool->frame_bufs[cm->new_fb_idx].buf.subsampling_y = cm->subsampling_y;
  pool->frame_bufs[cm->new_fb_idx].buf.bit_depth = (unsigned int)cm->bit_depth;
  pool->frame_bufs[cm->new_fb_idx].buf.color_space = cm->color_space;
#if CONFIG_COLORSPACE_HEADERS
  pool->frame_bufs[cm->new_fb_idx].buf.transfer_function =
      cm->transfer_function;
  pool->frame_bufs[cm->new_fb_idx].buf.chroma_sample_position =
      cm->chroma_sample_position;
#endif
  pool->frame_bufs[cm->new_fb_idx].buf.color_range = cm->color_range;
  pool->frame_bufs[cm->new_fb_idx].buf.render_width = cm->render_width;
  pool->frame_bufs[cm->new_fb_idx].buf.render_height = cm->render_height;
}

static void setup_sb_size(AV1_COMMON *cm, struct aom_read_bit_buffer *rb) {
  (void)rb;
#if CONFIG_EXT_PARTITION
  set_sb_size(cm, aom_rb_read_bit(rb) ? BLOCK_128X128 : BLOCK_64X64);
#else
  set_sb_size(cm, BLOCK_64X64);
#endif  // CONFIG_EXT_PARTITION
}

static INLINE int valid_ref_frame_img_fmt(aom_bit_depth_t ref_bit_depth,
                                          int ref_xss, int ref_yss,
                                          aom_bit_depth_t this_bit_depth,
                                          int this_xss, int this_yss) {
  return ref_bit_depth == this_bit_depth && ref_xss == this_xss &&
         ref_yss == this_yss;
}

static void setup_frame_size_with_refs(AV1_COMMON *cm,
                                       struct aom_read_bit_buffer *rb) {
  int width, height;
  int found = 0;
  int has_valid_ref_frame = 0;
  BufferPool *const pool = cm->buffer_pool;
  for (int i = 0; i < INTER_REFS_PER_FRAME; ++i) {
    if (aom_rb_read_bit(rb)) {
      YV12_BUFFER_CONFIG *const buf = cm->frame_refs[i].buf;
      width = buf->y_crop_width;
      height = buf->y_crop_height;
      cm->render_width = buf->render_width;
      cm->render_height = buf->render_height;
#if CONFIG_FRAME_SUPERRES
      setup_superres(cm, rb, &width, &height);
#endif  // CONFIG_FRAME_SUPERRES
      resize_context_buffers(cm, width, height);
      found = 1;
      break;
    }
  }

  if (!found) {
#if CONFIG_FRAME_SIZE
    int num_bits_width = cm->seq_params.num_bits_width;
    int num_bits_height = cm->seq_params.num_bits_height;
    av1_read_frame_size(rb, num_bits_width, num_bits_height, &width, &height);
#else
    av1_read_frame_size(rb, &width, &height);
#endif
#if CONFIG_FRAME_SUPERRES
    setup_superres(cm, rb, &width, &height);
#endif  // CONFIG_FRAME_SUPERRES
    resize_context_buffers(cm, width, height);
    setup_render_size(cm, rb);
  }

  if (width <= 0 || height <= 0)
    aom_internal_error(&cm->error, AOM_CODEC_CORRUPT_FRAME,
                       "Invalid frame size");

  // Check to make sure at least one of frames that this frame references
  // has valid dimensions.
  for (int i = 0; i < INTER_REFS_PER_FRAME; ++i) {
    RefBuffer *const ref_frame = &cm->frame_refs[i];
    has_valid_ref_frame |=
        valid_ref_frame_size(ref_frame->buf->y_crop_width,
                             ref_frame->buf->y_crop_height, width, height);
  }
  if (!has_valid_ref_frame)
    aom_internal_error(&cm->error, AOM_CODEC_CORRUPT_FRAME,
                       "Referenced frame has invalid size");
  for (int i = 0; i < INTER_REFS_PER_FRAME; ++i) {
    RefBuffer *const ref_frame = &cm->frame_refs[i];
    if (!valid_ref_frame_img_fmt(ref_frame->buf->bit_depth,
                                 ref_frame->buf->subsampling_x,
                                 ref_frame->buf->subsampling_y, cm->bit_depth,
                                 cm->subsampling_x, cm->subsampling_y))
      aom_internal_error(&cm->error, AOM_CODEC_CORRUPT_FRAME,
                         "Referenced frame has incompatible color format");
  }

  lock_buffer_pool(pool);
  if (aom_realloc_frame_buffer(
          get_frame_new_buffer(cm), cm->width, cm->height, cm->subsampling_x,
          cm->subsampling_y,
#if CONFIG_HIGHBITDEPTH
          cm->use_highbitdepth,
#endif
          AOM_BORDER_IN_PIXELS, cm->byte_alignment,
          &pool->frame_bufs[cm->new_fb_idx].raw_frame_buffer, pool->get_fb_cb,
          pool->cb_priv)) {
    unlock_buffer_pool(pool);
    aom_internal_error(&cm->error, AOM_CODEC_MEM_ERROR,
                       "Failed to allocate frame buffer");
  }
  unlock_buffer_pool(pool);

  pool->frame_bufs[cm->new_fb_idx].buf.subsampling_x = cm->subsampling_x;
  pool->frame_bufs[cm->new_fb_idx].buf.subsampling_y = cm->subsampling_y;
  pool->frame_bufs[cm->new_fb_idx].buf.bit_depth = (unsigned int)cm->bit_depth;
  pool->frame_bufs[cm->new_fb_idx].buf.color_space = cm->color_space;
#if CONFIG_COLORSPACE_HEADERS
  pool->frame_bufs[cm->new_fb_idx].buf.transfer_function =
      cm->transfer_function;
  pool->frame_bufs[cm->new_fb_idx].buf.chroma_sample_position =
      cm->chroma_sample_position;
#endif
  pool->frame_bufs[cm->new_fb_idx].buf.color_range = cm->color_range;
  pool->frame_bufs[cm->new_fb_idx].buf.render_width = cm->render_width;
  pool->frame_bufs[cm->new_fb_idx].buf.render_height = cm->render_height;
}

#if !CONFIG_OBU
static void read_tile_group_range(AV1Decoder *pbi,
                                  struct aom_read_bit_buffer *const rb) {
  AV1_COMMON *const cm = &pbi->common;
  const int num_bits = cm->log2_tile_rows + cm->log2_tile_cols;
  const int num_tiles =
      cm->tile_rows * cm->tile_cols;  // Note: May be < (1<<num_bits)
  pbi->tg_start = aom_rb_read_literal(rb, num_bits);
  pbi->tg_size = 1 + aom_rb_read_literal(rb, num_bits);
  if (pbi->tg_start + pbi->tg_size > num_tiles)
    aom_internal_error(&cm->error, AOM_CODEC_CORRUPT_FRAME,
                       "Tile group extends past last tile in frame");
}
#endif  // !CONFIG_OBU

#if CONFIG_MAX_TILE

// Same function as av1_read_uniform but reading from uncompresses header wb
static int rb_read_uniform(struct aom_read_bit_buffer *const rb, int n) {
  const int l = get_unsigned_bits(n);
  const int m = (1 << l) - n;
  const int v = aom_rb_read_literal(rb, l - 1);
  assert(l != 0);
  if (v < m)
    return v;
  else
    return (v << 1) - m + aom_rb_read_literal(rb, 1);
}

static void read_tile_info_max_tile(AV1_COMMON *const cm,
                                    struct aom_read_bit_buffer *const rb) {
  int width_mi = ALIGN_POWER_OF_TWO(cm->mi_cols, MAX_MIB_SIZE_LOG2);
  int height_mi = ALIGN_POWER_OF_TWO(cm->mi_rows, MAX_MIB_SIZE_LOG2);
  int width_sb = width_mi >> MAX_MIB_SIZE_LOG2;
  int height_sb = height_mi >> MAX_MIB_SIZE_LOG2;

  av1_get_tile_limits(cm);
  cm->uniform_tile_spacing_flag = aom_rb_read_bit(rb);

  // Read tile columns
  if (cm->uniform_tile_spacing_flag) {
    cm->log2_tile_cols = cm->min_log2_tile_cols;
    while (cm->log2_tile_cols < cm->max_log2_tile_cols) {
      if (!aom_rb_read_bit(rb)) {
        break;
      }
      cm->log2_tile_cols++;
    }
  } else {
    int i;
    int start_sb;
    for (i = 0, start_sb = 0; width_sb > 0 && i < MAX_TILE_COLS; i++) {
      const int size_sb =
          1 + rb_read_uniform(rb, AOMMIN(width_sb, MAX_TILE_WIDTH_SB));
      cm->tile_col_start_sb[i] = start_sb;
      start_sb += size_sb;
      width_sb -= size_sb;
    }
    cm->tile_cols = i;
    cm->tile_col_start_sb[i] = start_sb + width_sb;
  }
  av1_calculate_tile_cols(cm);

  // Read tile rows
  if (cm->uniform_tile_spacing_flag) {
    cm->log2_tile_rows = cm->min_log2_tile_rows;
    while (cm->log2_tile_rows < cm->max_log2_tile_rows) {
      if (!aom_rb_read_bit(rb)) {
        break;
      }
      cm->log2_tile_rows++;
    }
  } else {
    int i;
    int start_sb;
    for (i = 0, start_sb = 0; height_sb > 0 && i < MAX_TILE_ROWS; i++) {
      const int size_sb =
          1 + rb_read_uniform(rb, AOMMIN(height_sb, cm->max_tile_height_sb));
      cm->tile_row_start_sb[i] = start_sb;
      start_sb += size_sb;
      height_sb -= size_sb;
    }
    cm->tile_rows = i;
    cm->tile_row_start_sb[i] = start_sb + height_sb;
  }
  av1_calculate_tile_rows(cm);
}
#endif

static void read_tile_info(AV1Decoder *const pbi,
                           struct aom_read_bit_buffer *const rb) {
  AV1_COMMON *const cm = &pbi->common;
#if CONFIG_EXT_TILE
  cm->single_tile_decoding = 0;
  if (cm->large_scale_tile) {
    struct loopfilter *lf = &cm->lf;

// Figure out single_tile_decoding by loopfilter_level.
#if CONFIG_LOOPFILTER_LEVEL
    const int no_loopfilter = !(lf->filter_level[0] || lf->filter_level[1]);
#else
    const int no_loopfilter = !lf->filter_level;
#endif
    const int no_cdef = cm->cdef_bits == 0 && cm->cdef_strengths[0] == 0 &&
                        cm->nb_cdef_strengths == 1;
#if CONFIG_LOOP_RESTORATION
    const int no_restoration =
        cm->rst_info[0].frame_restoration_type == RESTORE_NONE &&
        cm->rst_info[1].frame_restoration_type == RESTORE_NONE &&
        cm->rst_info[2].frame_restoration_type == RESTORE_NONE;
#endif
    cm->single_tile_decoding = no_loopfilter && no_cdef
#if CONFIG_LOOP_RESTORATION
                               && no_restoration
#endif
        ;
// Read the tile width/height
#if CONFIG_EXT_PARTITION
    if (cm->sb_size == BLOCK_128X128) {
      cm->tile_width = aom_rb_read_literal(rb, 5) + 1;
      cm->tile_height = aom_rb_read_literal(rb, 5) + 1;
    } else {
#endif  // CONFIG_EXT_PARTITION
      cm->tile_width = aom_rb_read_literal(rb, 6) + 1;
      cm->tile_height = aom_rb_read_literal(rb, 6) + 1;
#if CONFIG_EXT_PARTITION
    }
#endif  // CONFIG_EXT_PARTITION

    cm->tile_width <<= cm->mib_size_log2;
    cm->tile_height <<= cm->mib_size_log2;

    cm->tile_width = AOMMIN(cm->tile_width, cm->mi_cols);
    cm->tile_height = AOMMIN(cm->tile_height, cm->mi_rows);

    // Get the number of tiles
    cm->tile_cols = 1;
    while (cm->tile_cols * cm->tile_width < cm->mi_cols) ++cm->tile_cols;

    cm->tile_rows = 1;
    while (cm->tile_rows * cm->tile_height < cm->mi_rows) ++cm->tile_rows;

#if CONFIG_DEPENDENT_HORZTILES
    cm->dependent_horz_tiles = 0;
#endif
#if CONFIG_LOOPFILTERING_ACROSS_TILES
    if (cm->tile_cols * cm->tile_rows > 1)
      cm->loop_filter_across_tiles_enabled = aom_rb_read_bit(rb);
    else
      cm->loop_filter_across_tiles_enabled = 1;
#endif  // CONFIG_LOOPFILTERING_ACROSS_TILES

    if (cm->tile_cols * cm->tile_rows > 1) {
      // Read the number of bytes used to store tile size
      pbi->tile_col_size_bytes = aom_rb_read_literal(rb, 2) + 1;
      pbi->tile_size_bytes = aom_rb_read_literal(rb, 2) + 1;
    }
  } else {
#endif  // CONFIG_EXT_TILE

#if CONFIG_MAX_TILE
    read_tile_info_max_tile(cm, rb);
#else
  int min_log2_tile_cols, max_log2_tile_cols, max_ones;
  av1_get_tile_n_bits(cm->mi_cols, &min_log2_tile_cols, &max_log2_tile_cols);

  // columns
  max_ones = max_log2_tile_cols - min_log2_tile_cols;
  cm->log2_tile_cols = min_log2_tile_cols;
  while (max_ones-- && aom_rb_read_bit(rb)) cm->log2_tile_cols++;

  if (cm->log2_tile_cols > 6)
    aom_internal_error(&cm->error, AOM_CODEC_CORRUPT_FRAME,
                       "Invalid number of tile columns");

  // rows
  cm->log2_tile_rows = aom_rb_read_bit(rb);
  if (cm->log2_tile_rows) cm->log2_tile_rows += aom_rb_read_bit(rb);

  cm->tile_width =
      get_tile_size(cm->mi_cols, cm->log2_tile_cols, &cm->tile_cols);
  cm->tile_height =
      get_tile_size(cm->mi_rows, cm->log2_tile_rows, &cm->tile_rows);

#endif  // CONFIG_MAX_TILE
#if CONFIG_DEPENDENT_HORZTILES
    if (cm->tile_rows > 1)
      cm->dependent_horz_tiles = aom_rb_read_bit(rb);
    else
      cm->dependent_horz_tiles = 0;
#endif
#if CONFIG_LOOPFILTERING_ACROSS_TILES
    if (cm->tile_cols * cm->tile_rows > 1)
      cm->loop_filter_across_tiles_enabled = aom_rb_read_bit(rb);
    else
      cm->loop_filter_across_tiles_enabled = 1;
#endif  // CONFIG_LOOPFILTERING_ACROSS_TILES

    // tile size magnitude
    pbi->tile_size_bytes = aom_rb_read_literal(rb, 2) + 1;
#if CONFIG_EXT_TILE
  }
#endif  // CONFIG_EXT_TILE

// each tile group header is in its own tile group OBU
#if !CONFIG_OBU
  // Store an index to the location of the tile group information
  pbi->tg_size_bit_offset = rb->bit_offset;
  read_tile_group_range(pbi, rb);
#endif
}

static int mem_get_varsize(const uint8_t *src, int sz) {
  switch (sz) {
    case 1: return src[0];
    case 2: return mem_get_le16(src);
    case 3: return mem_get_le24(src);
    case 4: return mem_get_le32(src);
    default: assert(0 && "Invalid size"); return -1;
  }
}

#if CONFIG_EXT_TILE
// Reads the next tile returning its size and adjusting '*data' accordingly
// based on 'is_last'.
static void get_ls_tile_buffer(
    const uint8_t *const data_end, struct aom_internal_error_info *error_info,
    const uint8_t **data, aom_decrypt_cb decrypt_cb, void *decrypt_state,
    TileBufferDec (*const tile_buffers)[MAX_TILE_COLS], int tile_size_bytes,
    int col, int row, int tile_copy_mode) {
  size_t size;

  size_t copy_size = 0;
  const uint8_t *copy_data = NULL;

  if (!read_is_valid(*data, tile_size_bytes, data_end))
    aom_internal_error(error_info, AOM_CODEC_CORRUPT_FRAME,
                       "Truncated packet or corrupt tile length");
  if (decrypt_cb) {
    uint8_t be_data[4];
    decrypt_cb(decrypt_state, *data, be_data, tile_size_bytes);

    // Only read number of bytes in cm->tile_size_bytes.
    size = mem_get_varsize(be_data, tile_size_bytes);
  } else {
    size = mem_get_varsize(*data, tile_size_bytes);
  }

  // If tile_copy_mode = 1, then the top bit of the tile header indicates copy
  // mode.
  if (tile_copy_mode && (size >> (tile_size_bytes * 8 - 1)) == 1) {
    // The remaining bits in the top byte signal the row offset
    int offset = (size >> (tile_size_bytes - 1) * 8) & 0x7f;

    // Currently, only use tiles in same column as reference tiles.
    copy_data = tile_buffers[row - offset][col].data;
    copy_size = tile_buffers[row - offset][col].size;
    size = 0;
  }

  *data += tile_size_bytes;

  if (size > (size_t)(data_end - *data))
    aom_internal_error(error_info, AOM_CODEC_CORRUPT_FRAME,
                       "Truncated packet or corrupt tile size");

  if (size > 0) {
    tile_buffers[row][col].data = *data;
    tile_buffers[row][col].size = size;
  } else {
    tile_buffers[row][col].data = copy_data;
    tile_buffers[row][col].size = copy_size;
  }

  *data += size;

  tile_buffers[row][col].raw_data_end = *data;
}

static void get_ls_tile_buffers(
    AV1Decoder *pbi, const uint8_t *data, const uint8_t *data_end,
    TileBufferDec (*const tile_buffers)[MAX_TILE_COLS]) {
  AV1_COMMON *const cm = &pbi->common;
  const int tile_cols = cm->tile_cols;
  const int tile_rows = cm->tile_rows;
  const int have_tiles = tile_cols * tile_rows > 1;

  if (!have_tiles) {
    const size_t tile_size = data_end - data;
    tile_buffers[0][0].data = data;
    tile_buffers[0][0].size = tile_size;
    tile_buffers[0][0].raw_data_end = NULL;
  } else {
    // We locate only the tile buffers that are required, which are the ones
    // specified by pbi->dec_tile_col and pbi->dec_tile_row. Also, we always
    // need the last (bottom right) tile buffer, as we need to know where the
    // end of the compressed frame buffer is for proper superframe decoding.

    const uint8_t *tile_col_data_end[MAX_TILE_COLS];
    const uint8_t *const data_start = data;

    const int dec_tile_row = AOMMIN(pbi->dec_tile_row, tile_rows);
    const int single_row = pbi->dec_tile_row >= 0;
    const int tile_rows_start = single_row ? dec_tile_row : 0;
    const int tile_rows_end = single_row ? tile_rows_start + 1 : tile_rows;
    const int dec_tile_col = AOMMIN(pbi->dec_tile_col, tile_cols);
    const int single_col = pbi->dec_tile_col >= 0;
    const int tile_cols_start = single_col ? dec_tile_col : 0;
    const int tile_cols_end = single_col ? tile_cols_start + 1 : tile_cols;

    const int tile_col_size_bytes = pbi->tile_col_size_bytes;
    const int tile_size_bytes = pbi->tile_size_bytes;
    const int tile_copy_mode =
        ((AOMMAX(cm->tile_width, cm->tile_height) << MI_SIZE_LOG2) <= 256) ? 1
                                                                           : 0;
    // Read tile column sizes for all columns (we need the last tile buffer)
    for (int c = 0; c < tile_cols; ++c) {
      const int is_last = c == tile_cols - 1;
      size_t tile_col_size;

      if (!is_last) {
        tile_col_size = mem_get_varsize(data, tile_col_size_bytes);
        data += tile_col_size_bytes;
        tile_col_data_end[c] = data + tile_col_size;
      } else {
        tile_col_size = data_end - data;
        tile_col_data_end[c] = data_end;
      }
      data += tile_col_size;
    }

    data = data_start;

    // Read the required tile sizes.
    for (int c = tile_cols_start; c < tile_cols_end; ++c) {
      const int is_last = c == tile_cols - 1;

      if (c > 0) data = tile_col_data_end[c - 1];

      if (!is_last) data += tile_col_size_bytes;

      // Get the whole of the last column, otherwise stop at the required tile.
      for (int r = 0; r < (is_last ? tile_rows : tile_rows_end); ++r) {
        tile_buffers[r][c].col = c;

        get_ls_tile_buffer(tile_col_data_end[c], &pbi->common.error, &data,
                           pbi->decrypt_cb, pbi->decrypt_state, tile_buffers,
                           tile_size_bytes, c, r, tile_copy_mode);
      }
    }

    // If we have not read the last column, then read it to get the last tile.
    if (tile_cols_end != tile_cols) {
      int c = tile_cols - 1;

      data = tile_col_data_end[c - 1];

      for (int r = 0; r < tile_rows; ++r) {
        tile_buffers[r][c].col = c;

        get_ls_tile_buffer(tile_col_data_end[c], &pbi->common.error, &data,
                           pbi->decrypt_cb, pbi->decrypt_state, tile_buffers,
                           tile_size_bytes, c, r, tile_copy_mode);
      }
    }
  }
}
#endif  // CONFIG_EXT_TILE

// Reads the next tile returning its size and adjusting '*data' accordingly
// based on 'is_last'.
static void get_tile_buffer(const uint8_t *const data_end,
                            const int tile_size_bytes, int is_last,
                            struct aom_internal_error_info *error_info,
                            const uint8_t **data, aom_decrypt_cb decrypt_cb,
                            void *decrypt_state, TileBufferDec *const buf) {
  size_t size;

  if (!is_last) {
    if (!read_is_valid(*data, tile_size_bytes, data_end))
      aom_internal_error(error_info, AOM_CODEC_CORRUPT_FRAME,
                         "Truncated packet or corrupt tile length");

    if (decrypt_cb) {
      uint8_t be_data[4];
      decrypt_cb(decrypt_state, *data, be_data, tile_size_bytes);
      size = mem_get_varsize(be_data, tile_size_bytes);
    } else {
      size = mem_get_varsize(*data, tile_size_bytes);
    }
    *data += tile_size_bytes;

    if (size > (size_t)(data_end - *data))
      aom_internal_error(error_info, AOM_CODEC_CORRUPT_FRAME,
                         "Truncated packet or corrupt tile size");
  } else {
#if !CONFIG_OBU || CONFIG_ADD_4BYTES_OBUSIZE
    size = data_end - *data;
#else
    size = mem_get_varsize(*data, tile_size_bytes);
    *data += tile_size_bytes;
#endif
  }

  buf->data = *data;
  buf->size = size;

  *data += size;
}

static void get_tile_buffers(AV1Decoder *pbi, const uint8_t *data,
                             const uint8_t *data_end,
                             TileBufferDec (*const tile_buffers)[MAX_TILE_COLS],
                             int startTile, int endTile) {
  AV1_COMMON *const cm = &pbi->common;
  const int tile_cols = cm->tile_cols;
  const int tile_rows = cm->tile_rows;
  int tc = 0;
  int first_tile_in_tg = 0;
#if !CONFIG_OBU
  struct aom_read_bit_buffer rb_tg_hdr;
  uint8_t clear_data[MAX_AV1_HEADER_SIZE];
  const size_t hdr_size = pbi->uncomp_hdr_size + pbi->first_partition_size;
  const int tg_size_bit_offset = pbi->tg_size_bit_offset;
#endif

#if CONFIG_DEPENDENT_HORZTILES
  int tile_group_start_col = 0;
  int tile_group_start_row = 0;
#endif

#if CONFIG_SIMPLE_BWD_ADAPT
  size_t max_tile_size = 0;
  cm->largest_tile_id = 0;
#endif
  for (int r = 0; r < tile_rows; ++r) {
    for (int c = 0; c < tile_cols; ++c, ++tc) {
      TileBufferDec *const buf = &tile_buffers[r][c];
#if CONFIG_OBU
      const int is_last = (tc == endTile);
      const size_t hdr_offset = 0;
#else
      const int is_last = (r == tile_rows - 1) && (c == tile_cols - 1);
      const size_t hdr_offset = (tc && tc == first_tile_in_tg) ? hdr_size : 0;
#endif

      if (tc < startTile || tc > endTile) continue;

      if (data + hdr_offset >= data_end)
        aom_internal_error(&cm->error, AOM_CODEC_CORRUPT_FRAME,
                           "Data ended before all tiles were read.");
      buf->col = c;
#if CONFIG_OBU
#if CONFIG_DEPENDENT_HORZTILES
      if (tc == startTile) {
        tile_group_start_row = r;
        tile_group_start_col = c;
      }
#endif  // CONFIG_DEPENDENT_HORZTILES
#else   // CONFIG_OBU
      if (hdr_offset) {
        init_read_bit_buffer(pbi, &rb_tg_hdr, data, data_end, clear_data);
        rb_tg_hdr.bit_offset = tg_size_bit_offset;
        read_tile_group_range(pbi, &rb_tg_hdr);
#if CONFIG_DEPENDENT_HORZTILES
        tile_group_start_row = r;
        tile_group_start_col = c;
#endif
      }
#endif  // CONFIG_OBU
      first_tile_in_tg += tc == first_tile_in_tg ? pbi->tg_size : 0;
      data += hdr_offset;
      get_tile_buffer(data_end, pbi->tile_size_bytes, is_last,
                      &pbi->common.error, &data, pbi->decrypt_cb,
                      pbi->decrypt_state, buf);
#if CONFIG_DEPENDENT_HORZTILES
      cm->tile_group_start_row[r][c] = tile_group_start_row;
      cm->tile_group_start_col[r][c] = tile_group_start_col;
#endif
#if CONFIG_SIMPLE_BWD_ADAPT
      if (buf->size > max_tile_size) {
        max_tile_size = buf->size;
        cm->largest_tile_id = r * tile_cols + c;
      }
#endif
    }
  }
}

#if CONFIG_LOOPFILTERING_ACROSS_TILES
static void dec_setup_across_tile_boundary_info(
    const AV1_COMMON *const cm, const TileInfo *const tile_info) {
  if (tile_info->mi_row_start >= tile_info->mi_row_end ||
      tile_info->mi_col_start >= tile_info->mi_col_end)
    return;

  if (!cm->loop_filter_across_tiles_enabled) {
    av1_setup_across_tile_boundary_info(cm, tile_info);
  }
}
#endif  // CONFIG_LOOPFILTERING_ACROSS_TILES

static const uint8_t *decode_tiles(AV1Decoder *pbi, const uint8_t *data,
                                   const uint8_t *data_end, int startTile,
                                   int endTile) {
  AV1_COMMON *const cm = &pbi->common;
#if !CONFIG_LOOPFILTER_LEVEL
  const AVxWorkerInterface *const winterface = aom_get_worker_interface();
#endif
  const int tile_cols = cm->tile_cols;
  const int tile_rows = cm->tile_rows;
  const int n_tiles = tile_cols * tile_rows;
  TileBufferDec(*const tile_buffers)[MAX_TILE_COLS] = pbi->tile_buffers;
#if CONFIG_EXT_TILE
  const int dec_tile_row = AOMMIN(pbi->dec_tile_row, tile_rows);
  const int single_row = pbi->dec_tile_row >= 0;
  const int dec_tile_col = AOMMIN(pbi->dec_tile_col, tile_cols);
  const int single_col = pbi->dec_tile_col >= 0;
#endif  // CONFIG_EXT_TILE
  int tile_rows_start;
  int tile_rows_end;
  int tile_cols_start;
  int tile_cols_end;
  int inv_col_order;
  int inv_row_order;
  int tile_row, tile_col;
  uint8_t allow_update_cdf;

#if CONFIG_EXT_TILE
  if (cm->large_scale_tile) {
    tile_rows_start = single_row ? dec_tile_row : 0;
    tile_rows_end = single_row ? dec_tile_row + 1 : tile_rows;
    tile_cols_start = single_col ? dec_tile_col : 0;
    tile_cols_end = single_col ? tile_cols_start + 1 : tile_cols;
    inv_col_order = pbi->inv_tile_order && !single_col;
    inv_row_order = pbi->inv_tile_order && !single_row;
    allow_update_cdf = 0;
  } else {
#endif  // CONFIG_EXT_TILE
    tile_rows_start = 0;
    tile_rows_end = tile_rows;
    tile_cols_start = 0;
    tile_cols_end = tile_cols;
    inv_col_order = pbi->inv_tile_order;
    inv_row_order = pbi->inv_tile_order;
    allow_update_cdf = 1;
#if CONFIG_EXT_TILE
  }
#endif  // CONFIG_EXT_TILE

#if !CONFIG_LOOPFILTER_LEVEL
  if (cm->lf.filter_level && !cm->skip_loop_filter &&
      pbi->lf_worker.data1 == NULL) {
    CHECK_MEM_ERROR(cm, pbi->lf_worker.data1,
                    aom_memalign(32, sizeof(LFWorkerData)));
    pbi->lf_worker.hook = (AVxWorkerHook)av1_loop_filter_worker;
    if (pbi->max_threads > 1 && !winterface->reset(&pbi->lf_worker)) {
      aom_internal_error(&cm->error, AOM_CODEC_ERROR,
                         "Loop filter thread creation failed");
    }
  }

  if (cm->lf.filter_level && !cm->skip_loop_filter) {
    LFWorkerData *const lf_data = (LFWorkerData *)pbi->lf_worker.data1;
    // Be sure to sync as we might be resuming after a failed frame decode.
    winterface->sync(&pbi->lf_worker);
    av1_loop_filter_data_reset(lf_data, get_frame_new_buffer(cm), cm,
                               pbi->mb.plane);
  }
#endif  // CONFIG_LOOPFILTER_LEVEL

  assert(tile_rows <= MAX_TILE_ROWS);
  assert(tile_cols <= MAX_TILE_COLS);

#if CONFIG_EXT_TILE
  if (cm->large_scale_tile)
    get_ls_tile_buffers(pbi, data, data_end, tile_buffers);
  else
#endif  // CONFIG_EXT_TILE
    get_tile_buffers(pbi, data, data_end, tile_buffers, startTile, endTile);

  if (pbi->tile_data == NULL || n_tiles != pbi->allocated_tiles) {
    aom_free(pbi->tile_data);
    CHECK_MEM_ERROR(cm, pbi->tile_data,
                    aom_memalign(32, n_tiles * (sizeof(*pbi->tile_data))));
    pbi->allocated_tiles = n_tiles;
  }
#if CONFIG_ACCOUNTING
  if (pbi->acct_enabled) {
    aom_accounting_reset(&pbi->accounting);
  }
#endif
  // Load all tile information into tile_data.
  for (tile_row = tile_rows_start; tile_row < tile_rows_end; ++tile_row) {
    for (tile_col = tile_cols_start; tile_col < tile_cols_end; ++tile_col) {
      const TileBufferDec *const buf = &tile_buffers[tile_row][tile_col];
      TileData *const td = pbi->tile_data + tile_cols * tile_row + tile_col;

      if (tile_row * cm->tile_cols + tile_col < startTile ||
          tile_row * cm->tile_cols + tile_col > endTile)
        continue;

      td->cm = cm;
      td->xd = pbi->mb;
      td->xd.corrupted = 0;
      td->xd.counts =
          cm->refresh_frame_context == REFRESH_FRAME_CONTEXT_BACKWARD
              ? &cm->counts
              : NULL;
      av1_zero(td->dqcoeff);
      av1_tile_init(&td->xd.tile, td->cm, tile_row, tile_col);
      setup_bool_decoder(buf->data, data_end, buf->size, &cm->error,
                         &td->bit_reader, allow_update_cdf,
#if CONFIG_ANS && ANS_MAX_SYMBOLS
                         1 << cm->ans_window_size_log2,
#endif  // CONFIG_ANS && ANS_MAX_SYMBOLS
                         pbi->decrypt_cb, pbi->decrypt_state);
#if CONFIG_ACCOUNTING
      if (pbi->acct_enabled) {
        td->bit_reader.accounting = &pbi->accounting;
      } else {
        td->bit_reader.accounting = NULL;
      }
#endif
      av1_init_macroblockd(cm, &td->xd, td->dqcoeff);

      // Initialise the tile context from the frame context
      td->tctx = *cm->fc;
      td->xd.tile_ctx = &td->tctx;
      td->xd.plane[0].color_index_map = td->color_index_map[0];
      td->xd.plane[1].color_index_map = td->color_index_map[1];
    }
  }

  for (tile_row = tile_rows_start; tile_row < tile_rows_end; ++tile_row) {
    const int row = inv_row_order ? tile_rows - 1 - tile_row : tile_row;
    int mi_row = 0;
    TileInfo tile_info;

    av1_tile_set_row(&tile_info, cm, row);

    for (tile_col = tile_cols_start; tile_col < tile_cols_end; ++tile_col) {
      const int col = inv_col_order ? tile_cols - 1 - tile_col : tile_col;
      TileData *const td = pbi->tile_data + tile_cols * row + col;

      if (tile_row * cm->tile_cols + tile_col < startTile ||
          tile_row * cm->tile_cols + tile_col > endTile)
        continue;

#if CONFIG_ACCOUNTING
      if (pbi->acct_enabled) {
        td->bit_reader.accounting->last_tell_frac =
            aom_reader_tell_frac(&td->bit_reader);
      }
#endif

      av1_tile_set_col(&tile_info, cm, col);

#if CONFIG_DEPENDENT_HORZTILES
      av1_tile_set_tg_boundary(&tile_info, cm, tile_row, tile_col);
      if (!cm->dependent_horz_tiles || tile_row == 0 ||
          tile_info.tg_horz_boundary) {
        av1_zero_above_context(cm, tile_info.mi_col_start,
                               tile_info.mi_col_end);
      }
#else
      av1_zero_above_context(cm, tile_info.mi_col_start, tile_info.mi_col_end);
#endif
#if CONFIG_LOOP_RESTORATION
      av1_reset_loop_restoration(&td->xd);
#endif  // CONFIG_LOOP_RESTORATION

#if CONFIG_LOOPFILTERING_ACROSS_TILES
      dec_setup_across_tile_boundary_info(cm, &tile_info);
#endif  // CONFIG_LOOPFILTERING_ACROSS_TILES

      for (mi_row = tile_info.mi_row_start; mi_row < tile_info.mi_row_end;
           mi_row += cm->mib_size) {
        int mi_col;

        av1_zero_left_context(&td->xd);

        for (mi_col = tile_info.mi_col_start; mi_col < tile_info.mi_col_end;
             mi_col += cm->mib_size) {
#if CONFIG_SYMBOLRATE
          av1_record_superblock(td->xd.counts);
#endif
          decode_partition(pbi, &td->xd, mi_row, mi_col, &td->bit_reader,
                           cm->sb_size);
#if NC_MODE_INFO
          detoken_and_recon_sb(pbi, &td->xd, mi_row, mi_col, &td->bit_reader,
                               cm->sb_size);
#endif
#if CONFIG_LPF_SB
          if (USE_LOOP_FILTER_SUPERBLOCK) {
            // apply deblocking filtering right after each superblock is decoded
            const int guess_filter_lvl = FAKE_FILTER_LEVEL;
            av1_loop_filter_frame(get_frame_new_buffer(cm), cm, &pbi->mb,
                                  guess_filter_lvl, 0, 1, mi_row, mi_col);
          }
#endif  // CONFIG_LPF_SB
        }
        aom_merge_corrupted_flag(&pbi->mb.corrupted, td->xd.corrupted);
        if (pbi->mb.corrupted)
          aom_internal_error(&cm->error, AOM_CODEC_CORRUPT_FRAME,
                             "Failed to decode tile data");
      }
    }

#if !CONFIG_OBU
    assert(mi_row > 0);
#endif

    // After loopfiltering, the last 7 row pixels in each superblock row may
    // still be changed by the longest loopfilter of the next superblock row.
    if (cm->frame_parallel_decode)
      av1_frameworker_broadcast(pbi->cur_buf, mi_row << cm->mib_size_log2);
  }

#if CONFIG_INTRABC
  if (!(cm->allow_intrabc && NO_FILTER_FOR_IBC))
#endif  // CONFIG_INTRABC
  {
// Loopfilter the whole frame.
#if CONFIG_LPF_SB
    av1_loop_filter_frame(get_frame_new_buffer(cm), cm, &pbi->mb,
                          cm->lf.filter_level, 0, 0, 0, 0);
#else
#if CONFIG_OBU
    if (endTile == cm->tile_rows * cm->tile_cols - 1)
#endif
#if CONFIG_LOOPFILTER_LEVEL
      if (cm->lf.filter_level[0] || cm->lf.filter_level[1]) {
        av1_loop_filter_frame(get_frame_new_buffer(cm), cm, &pbi->mb,
                              cm->lf.filter_level[0], cm->lf.filter_level[1], 0,
                              0);
        av1_loop_filter_frame(get_frame_new_buffer(cm), cm, &pbi->mb,
                              cm->lf.filter_level_u, cm->lf.filter_level_u, 1,
                              0);
        av1_loop_filter_frame(get_frame_new_buffer(cm), cm, &pbi->mb,
                              cm->lf.filter_level_v, cm->lf.filter_level_v, 2,
                              0);
      }
#else
    av1_loop_filter_frame(get_frame_new_buffer(cm), cm, &pbi->mb,
                          cm->lf.filter_level, 0, 0);
#endif  // CONFIG_LOOPFILTER_LEVEL
#endif  // CONFIG_LPF_SB
  }
  if (cm->frame_parallel_decode)
    av1_frameworker_broadcast(pbi->cur_buf, INT_MAX);

#if CONFIG_EXT_TILE
  if (cm->large_scale_tile) {
    if (n_tiles == 1) {
#if CONFIG_ANS
      return data_end;
#else
      // Find the end of the single tile buffer
      return aom_reader_find_end(&pbi->tile_data->bit_reader);
#endif  // CONFIG_ANS
    } else {
      // Return the end of the last tile buffer
      return tile_buffers[tile_rows - 1][tile_cols - 1].raw_data_end;
    }
  } else {
#endif  // CONFIG_EXT_TILE
#if CONFIG_ANS
    return data_end;
#else
#if !CONFIG_OBU
  {
    // Get last tile data.
    TileData *const td = pbi->tile_data + tile_cols * tile_rows - 1;
    return aom_reader_find_end(&td->bit_reader);
  }
#else
  TileData *const td = pbi->tile_data + endTile;
  return aom_reader_find_end(&td->bit_reader);
#endif
#endif  // CONFIG_ANS
#if CONFIG_EXT_TILE
  }
#endif  // CONFIG_EXT_TILE
}

static void error_handler(void *data) {
  AV1_COMMON *const cm = (AV1_COMMON *)data;
  aom_internal_error(&cm->error, AOM_CODEC_CORRUPT_FRAME, "Truncated packet");
}

static void read_bitdepth_colorspace_sampling(AV1_COMMON *cm,
                                              struct aom_read_bit_buffer *rb,
                                              int allow_lowbitdepth) {
  if (cm->profile >= PROFILE_2) {
    cm->bit_depth = aom_rb_read_bit(rb) ? AOM_BITS_12 : AOM_BITS_10;
  } else {
    cm->bit_depth = AOM_BITS_8;
  }

#if CONFIG_HIGHBITDEPTH
  cm->use_highbitdepth = cm->bit_depth > AOM_BITS_8 || !allow_lowbitdepth;
#else
  (void)allow_lowbitdepth;
#endif
#if CONFIG_COLORSPACE_HEADERS
  cm->color_space = aom_rb_read_literal(rb, 5);
  cm->transfer_function = aom_rb_read_literal(rb, 5);
#else
  cm->color_space = aom_rb_read_literal(rb, 3);
#endif
  if (cm->color_space != AOM_CS_SRGB) {
    // [16,235] (including xvycc) vs [0,255] range
    cm->color_range = aom_rb_read_bit(rb);
    if (cm->profile == PROFILE_1 || cm->profile == PROFILE_3) {
      cm->subsampling_x = aom_rb_read_bit(rb);
      cm->subsampling_y = aom_rb_read_bit(rb);
      if (cm->subsampling_x == 1 && cm->subsampling_y == 1)
        aom_internal_error(&cm->error, AOM_CODEC_UNSUP_BITSTREAM,
                           "4:2:0 color not supported in profile 1 or 3");
      if (aom_rb_read_bit(rb))
        aom_internal_error(&cm->error, AOM_CODEC_UNSUP_BITSTREAM,
                           "Reserved bit set");
    } else {
      cm->subsampling_y = cm->subsampling_x = 1;
    }
#if CONFIG_COLORSPACE_HEADERS
    if (cm->subsampling_x == 1 && cm->subsampling_y == 1) {
      cm->chroma_sample_position = aom_rb_read_literal(rb, 2);
    }
#endif
  } else {
    if (cm->profile == PROFILE_1 || cm->profile == PROFILE_3) {
      // Note if colorspace is SRGB then 4:4:4 chroma sampling is assumed.
      // 4:2:2 or 4:4:0 chroma sampling is not allowed.
      cm->subsampling_y = cm->subsampling_x = 0;
      if (aom_rb_read_bit(rb))
        aom_internal_error(&cm->error, AOM_CODEC_UNSUP_BITSTREAM,
                           "Reserved bit set");
    } else {
      aom_internal_error(&cm->error, AOM_CODEC_UNSUP_BITSTREAM,
                         "4:4:4 color not supported in profile 0 or 2");
    }
  }
}

#if CONFIG_REFERENCE_BUFFER || CONFIG_OBU
void read_sequence_header(SequenceHeader *seq_params,
                          struct aom_read_bit_buffer *rb) {
#if CONFIG_FRAME_SIZE
  int num_bits_width = aom_rb_read_literal(rb, 4) + 1;
  int num_bits_height = aom_rb_read_literal(rb, 4) + 1;
  int max_frame_width = aom_rb_read_literal(rb, num_bits_width) + 1;
  int max_frame_height = aom_rb_read_literal(rb, num_bits_height) + 1;

  seq_params->num_bits_width = num_bits_width;
  seq_params->num_bits_height = num_bits_height;
  seq_params->max_frame_width = max_frame_width;
  seq_params->max_frame_height = max_frame_height;
#endif

  /* Placeholder for actually reading from the bitstream */
  seq_params->frame_id_numbers_present_flag = aom_rb_read_bit(rb);
  if (seq_params->frame_id_numbers_present_flag) {
    // We must always have delta_frame_id_length < frame_id_length,
    // in order for a frame to be referenced with a unique delta.
    // Avoid wasting bits by using a coding that enforces this restriction.
    seq_params->delta_frame_id_length = aom_rb_read_literal(rb, 4) + 2;
    seq_params->frame_id_length =
        aom_rb_read_literal(rb, 3) + seq_params->delta_frame_id_length + 1;
  }

#if CONFIG_MONO_VIDEO
  seq_params->monochrome = aom_rb_read_bit(rb);
#endif  // CONFIG_MONO_VIDEO
}
#endif  // CONFIG_REFERENCE_BUFFER || CONFIG_OBU

static void read_compound_tools(AV1_COMMON *cm,
                                struct aom_read_bit_buffer *rb) {
  if (!frame_is_intra_only(cm) && cm->reference_mode != COMPOUND_REFERENCE) {
    cm->allow_interintra_compound = aom_rb_read_bit(rb);
  } else {
    cm->allow_interintra_compound = 0;
  }
  if (!frame_is_intra_only(cm) && cm->reference_mode != SINGLE_REFERENCE) {
    cm->allow_masked_compound = aom_rb_read_bit(rb);
  } else {
    cm->allow_masked_compound = 0;
  }
}

static int read_global_motion_params(WarpedMotionParams *params,
                                     const WarpedMotionParams *ref_params,
                                     struct aom_read_bit_buffer *rb,
                                     int allow_hp) {
  TransformationType type = aom_rb_read_bit(rb);
  if (type != IDENTITY) {
#if GLOBAL_TRANS_TYPES > 4
    type += aom_rb_read_literal(rb, GLOBAL_TYPE_BITS);
#else
    if (aom_rb_read_bit(rb))
      type = ROTZOOM;
    else
      type = aom_rb_read_bit(rb) ? TRANSLATION : AFFINE;
#endif  // GLOBAL_TRANS_TYPES > 4
  }

  *params = default_warp_params;
  params->wmtype = type;

  if (type >= ROTZOOM) {
    params->wmmat[2] = aom_rb_read_signed_primitive_refsubexpfin(
                           rb, GM_ALPHA_MAX + 1, SUBEXPFIN_K,
                           (ref_params->wmmat[2] >> GM_ALPHA_PREC_DIFF) -
                               (1 << GM_ALPHA_PREC_BITS)) *
                           GM_ALPHA_DECODE_FACTOR +
                       (1 << WARPEDMODEL_PREC_BITS);
    params->wmmat[3] = aom_rb_read_signed_primitive_refsubexpfin(
                           rb, GM_ALPHA_MAX + 1, SUBEXPFIN_K,
                           (ref_params->wmmat[3] >> GM_ALPHA_PREC_DIFF)) *
                       GM_ALPHA_DECODE_FACTOR;
  }

  if (type >= AFFINE) {
    params->wmmat[4] = aom_rb_read_signed_primitive_refsubexpfin(
                           rb, GM_ALPHA_MAX + 1, SUBEXPFIN_K,
                           (ref_params->wmmat[4] >> GM_ALPHA_PREC_DIFF)) *
                       GM_ALPHA_DECODE_FACTOR;
    params->wmmat[5] = aom_rb_read_signed_primitive_refsubexpfin(
                           rb, GM_ALPHA_MAX + 1, SUBEXPFIN_K,
                           (ref_params->wmmat[5] >> GM_ALPHA_PREC_DIFF) -
                               (1 << GM_ALPHA_PREC_BITS)) *
                           GM_ALPHA_DECODE_FACTOR +
                       (1 << WARPEDMODEL_PREC_BITS);
  } else {
    params->wmmat[4] = -params->wmmat[3];
    params->wmmat[5] = params->wmmat[2];
  }

  if (type >= TRANSLATION) {
    const int trans_bits = (type == TRANSLATION)
                               ? GM_ABS_TRANS_ONLY_BITS - !allow_hp
                               : GM_ABS_TRANS_BITS;
    const int trans_dec_factor =
        (type == TRANSLATION) ? GM_TRANS_ONLY_DECODE_FACTOR * (1 << !allow_hp)
                              : GM_TRANS_DECODE_FACTOR;
    const int trans_prec_diff = (type == TRANSLATION)
                                    ? GM_TRANS_ONLY_PREC_DIFF + !allow_hp
                                    : GM_TRANS_PREC_DIFF;
    params->wmmat[0] = aom_rb_read_signed_primitive_refsubexpfin(
                           rb, (1 << trans_bits) + 1, SUBEXPFIN_K,
                           (ref_params->wmmat[0] >> trans_prec_diff)) *
                       trans_dec_factor;
    params->wmmat[1] = aom_rb_read_signed_primitive_refsubexpfin(
                           rb, (1 << trans_bits) + 1, SUBEXPFIN_K,
                           (ref_params->wmmat[1] >> trans_prec_diff)) *
                       trans_dec_factor;
  }

  if (params->wmtype <= AFFINE) {
    int good_shear_params = get_shear_params(params);
    if (!good_shear_params) return 0;
  }

  return 1;
}

static void read_global_motion(AV1_COMMON *cm, struct aom_read_bit_buffer *rb) {
  int frame;
  for (frame = LAST_FRAME; frame <= ALTREF_FRAME; ++frame) {
    const WarpedMotionParams *ref_params =
        cm->error_resilient_mode ? &default_warp_params
                                 : &cm->prev_frame->global_motion[frame];
    int good_params = read_global_motion_params(
        &cm->global_motion[frame], ref_params, rb, cm->allow_high_precision_mv);
    if (!good_params) {
#if WARPED_MOTION_DEBUG
      printf("Warning: unexpected global motion shear params from aomenc\n");
#endif
      cm->global_motion[frame].invalid = 1;
    }

    // TODO(sarahparker, debargha): The logic in the commented out code below
    // does not work currently and causes mismatches when resize is on. Fix it
    // before turning the optimization back on.
    /*
    YV12_BUFFER_CONFIG *ref_buf = get_ref_frame(cm, frame);
    if (cm->width == ref_buf->y_crop_width &&
        cm->height == ref_buf->y_crop_height) {
      read_global_motion_params(&cm->global_motion[frame],
                                &cm->prev_frame->global_motion[frame], rb,
                                cm->allow_high_precision_mv);
    } else {
      cm->global_motion[frame] = default_warp_params;
    }
    */
    /*
    printf("Dec Ref %d [%d/%d]: %d %d %d %d\n",
           frame, cm->current_video_frame, cm->show_frame,
           cm->global_motion[frame].wmmat[0],
           cm->global_motion[frame].wmmat[1],
           cm->global_motion[frame].wmmat[2],
           cm->global_motion[frame].wmmat[3]);
           */
  }
  memcpy(cm->cur_frame->global_motion, cm->global_motion,
         TOTAL_REFS_PER_FRAME * sizeof(WarpedMotionParams));
}

static size_t read_uncompressed_header(AV1Decoder *pbi,
                                       struct aom_read_bit_buffer *rb) {
  AV1_COMMON *const cm = &pbi->common;
  MACROBLOCKD *const xd = &pbi->mb;
  BufferPool *const pool = cm->buffer_pool;
  RefCntBuffer *const frame_bufs = pool->frame_bufs;

  cm->last_frame_type = cm->frame_type;
  cm->last_intra_only = cm->intra_only;

  // NOTE: By default all coded frames to be used as a reference
  cm->is_reference_frame = 1;

#if !CONFIG_OBU
  if (aom_rb_read_literal(rb, 2) != AOM_FRAME_MARKER)
    aom_internal_error(&cm->error, AOM_CODEC_UNSUP_BITSTREAM,
                       "Invalid frame marker");

  cm->profile = av1_read_profile(rb);

  const BITSTREAM_PROFILE MAX_SUPPORTED_PROFILE =
      CONFIG_HIGHBITDEPTH ? MAX_PROFILES : PROFILE_2;

  if (cm->profile >= MAX_SUPPORTED_PROFILE)
    aom_internal_error(&cm->error, AOM_CODEC_UNSUP_BITSTREAM,
                       "Unsupported bitstream profile");
#endif

#if CONFIG_EXT_TILE
  cm->large_scale_tile = aom_rb_read_literal(rb, 1);
#if CONFIG_REFERENCE_BUFFER
  if (cm->large_scale_tile) cm->seq_params.frame_id_numbers_present_flag = 0;
#endif  // CONFIG_REFERENCE_BUFFER
#endif  // CONFIG_EXT_TILE

  cm->show_existing_frame = aom_rb_read_bit(rb);

  if (cm->show_existing_frame) {
    // Show an existing frame directly.
    const int existing_frame_idx = aom_rb_read_literal(rb, 3);
    const int frame_to_show = cm->ref_frame_map[existing_frame_idx];
#if CONFIG_REFERENCE_BUFFER
    if (cm->seq_params.frame_id_numbers_present_flag) {
      int frame_id_length = cm->seq_params.frame_id_length;
      int display_frame_id = aom_rb_read_literal(rb, frame_id_length);
      /* Compare display_frame_id with ref_frame_id and check valid for
       * referencing */
      if (display_frame_id != cm->ref_frame_id[existing_frame_idx] ||
          cm->valid_for_referencing[existing_frame_idx] == 0)
        aom_internal_error(&cm->error, AOM_CODEC_CORRUPT_FRAME,
                           "Reference buffer frame ID mismatch");
    }
#endif
    lock_buffer_pool(pool);
    if (frame_to_show < 0 || frame_bufs[frame_to_show].ref_count < 1) {
      unlock_buffer_pool(pool);
      aom_internal_error(&cm->error, AOM_CODEC_UNSUP_BITSTREAM,
                         "Buffer %d does not contain a decoded frame",
                         frame_to_show);
    }
    ref_cnt_fb(frame_bufs, &cm->new_fb_idx, frame_to_show);
    unlock_buffer_pool(pool);

#if CONFIG_LOOPFILTER_LEVEL
    cm->lf.filter_level[0] = 0;
    cm->lf.filter_level[1] = 0;
#else
    cm->lf.filter_level = 0;
#endif
    cm->show_frame = 1;
    pbi->refresh_frame_flags = 0;

    if (cm->frame_parallel_decode) {
      for (int i = 0; i < REF_FRAMES; ++i)
        cm->next_ref_frame_map[i] = cm->ref_frame_map[i];
    }

    return 0;
  }

#if !CONFIG_OBU
  cm->frame_type = (FRAME_TYPE)aom_rb_read_bit(rb);
  cm->show_frame = aom_rb_read_bit(rb);
  if (cm->frame_type != KEY_FRAME)
    cm->intra_only = cm->show_frame ? 0 : aom_rb_read_bit(rb);
#else
  cm->frame_type = (FRAME_TYPE)aom_rb_read_literal(rb, 2);  // 2 bits
  cm->show_frame = aom_rb_read_bit(rb);
  cm->intra_only = cm->frame_type == INTRA_ONLY_FRAME;
#endif
  cm->error_resilient_mode = aom_rb_read_bit(rb);
#if CONFIG_REFERENCE_BUFFER
#if !CONFIG_OBU
  if (frame_is_intra_only(cm)) read_sequence_header(&cm->seq_params, rb);
#endif  // !CONFIG_OBU
  if (cm->seq_params.frame_id_numbers_present_flag) {
    int frame_id_length = cm->seq_params.frame_id_length;
    int diff_len = cm->seq_params.delta_frame_id_length;
    int prev_frame_id = 0;
    if (cm->frame_type != KEY_FRAME) {
      prev_frame_id = cm->current_frame_id;
    }
    cm->current_frame_id = aom_rb_read_literal(rb, frame_id_length);

    if (cm->frame_type != KEY_FRAME) {
      int diff_frame_id;
      if (cm->current_frame_id > prev_frame_id) {
        diff_frame_id = cm->current_frame_id - prev_frame_id;
      } else {
        diff_frame_id =
            (1 << frame_id_length) + cm->current_frame_id - prev_frame_id;
      }
      /* Check current_frame_id for conformance */
      if (prev_frame_id == cm->current_frame_id ||
          diff_frame_id >= (1 << (frame_id_length - 1))) {
        aom_internal_error(&cm->error, AOM_CODEC_CORRUPT_FRAME,
                           "Invalid value of current_frame_id");
      }
    }
    /* Check if some frames need to be marked as not valid for referencing */
    for (int i = 0; i < REF_FRAMES; i++) {
      if (cm->frame_type == KEY_FRAME) {
        cm->valid_for_referencing[i] = 0;
      } else if (cm->current_frame_id - (1 << diff_len) > 0) {
        if (cm->ref_frame_id[i] > cm->current_frame_id ||
            cm->ref_frame_id[i] < cm->current_frame_id - (1 << diff_len))
          cm->valid_for_referencing[i] = 0;
      } else {
        if (cm->ref_frame_id[i] > cm->current_frame_id &&
            cm->ref_frame_id[i] <
                (1 << frame_id_length) + cm->current_frame_id - (1 << diff_len))
          cm->valid_for_referencing[i] = 0;
      }
    }
  }
#endif  // CONFIG_REFERENCE_BUFFER

#if CONFIG_FRAME_SIZE
  int frame_size_override_flag = aom_rb_read_literal(rb, 1);
#endif

#if CONFIG_INTRABC
  cm->allow_intrabc = 0;
#endif  // CONFIG_INTRABC

  if (cm->frame_type == KEY_FRAME) {
    cm->current_video_frame = 0;
#if !CONFIG_OBU
    read_bitdepth_colorspace_sampling(cm, rb, pbi->allow_lowbitdepth);
#endif
    pbi->refresh_frame_flags = (1 << REF_FRAMES) - 1;

    for (int i = 0; i < INTER_REFS_PER_FRAME; ++i) {
      cm->frame_refs[i].idx = INVALID_IDX;
      cm->frame_refs[i].buf = NULL;
    }

#if CONFIG_FRAME_SIZE
    setup_frame_size(cm, frame_size_override_flag, rb);
#else
    setup_frame_size(cm, rb);
#endif
    setup_sb_size(cm, rb);

    if (pbi->need_resync) {
      memset(&cm->ref_frame_map, -1, sizeof(cm->ref_frame_map));
      pbi->need_resync = 0;
    }
#if CONFIG_ANS && ANS_MAX_SYMBOLS
    cm->ans_window_size_log2 = aom_rb_read_literal(rb, 4) + 8;
#endif  // CONFIG_ANS && ANS_MAX_SYMBOLS
    cm->allow_screen_content_tools = aom_rb_read_bit(rb);
#if CONFIG_INTRABC
    if (cm->allow_screen_content_tools) cm->allow_intrabc = aom_rb_read_bit(rb);
#endif  // CONFIG_INTRABC
#if CONFIG_AMVR
    if (cm->allow_screen_content_tools) {
      if (aom_rb_read_bit(rb)) {
        cm->seq_force_integer_mv = 2;
      } else {
        cm->seq_force_integer_mv = aom_rb_read_bit(rb);
      }
    } else {
      cm->seq_force_integer_mv = 0;
    }
#endif
#if CONFIG_TEMPMV_SIGNALING
    cm->use_prev_frame_mvs = 0;
#endif
  } else {
#if CONFIG_TEMPMV_SIGNALING
    if (cm->intra_only || cm->error_resilient_mode) cm->use_prev_frame_mvs = 0;
#endif
#if CONFIG_NO_FRAME_CONTEXT_SIGNALING
// The only way to reset all frame contexts to their default values is with a
// keyframe.
#else
    if (cm->error_resilient_mode) {
      cm->reset_frame_context = RESET_FRAME_CONTEXT_ALL;
    } else {
      if (cm->intra_only) {
        cm->reset_frame_context = aom_rb_read_bit(rb)
                                      ? RESET_FRAME_CONTEXT_ALL
                                      : RESET_FRAME_CONTEXT_CURRENT;
      } else {
        cm->reset_frame_context = aom_rb_read_bit(rb)
                                      ? RESET_FRAME_CONTEXT_CURRENT
                                      : RESET_FRAME_CONTEXT_NONE;
        if (cm->reset_frame_context == RESET_FRAME_CONTEXT_CURRENT)
          cm->reset_frame_context = aom_rb_read_bit(rb)
                                        ? RESET_FRAME_CONTEXT_ALL
                                        : RESET_FRAME_CONTEXT_CURRENT;
      }
    }
#endif

    if (cm->intra_only) {
#if !CONFIG_OBU
      read_bitdepth_colorspace_sampling(cm, rb, pbi->allow_lowbitdepth);
#endif

      pbi->refresh_frame_flags = aom_rb_read_literal(rb, REF_FRAMES);
#if CONFIG_FRAME_SIZE
      setup_frame_size(cm, frame_size_override_flag, rb);
#else
      setup_frame_size(cm, rb);
#endif
      setup_sb_size(cm, rb);
      if (pbi->need_resync) {
        memset(&cm->ref_frame_map, -1, sizeof(cm->ref_frame_map));
        pbi->need_resync = 0;
      }
#if CONFIG_ANS && ANS_MAX_SYMBOLS
      cm->ans_window_size_log2 = aom_rb_read_literal(rb, 4) + 8;
#endif
      cm->allow_screen_content_tools = aom_rb_read_bit(rb);
#if CONFIG_INTRABC
      if (cm->allow_screen_content_tools)
        cm->allow_intrabc = aom_rb_read_bit(rb);
#endif                                  // CONFIG_INTRABC
    } else if (pbi->need_resync != 1) { /* Skip if need resync */
#if CONFIG_OBU
      pbi->refresh_frame_flags = (cm->frame_type == S_FRAME)
                                     ? ~(1 << REF_FRAMES)
                                     : aom_rb_read_literal(rb, REF_FRAMES);
#else
      pbi->refresh_frame_flags = aom_rb_read_literal(rb, REF_FRAMES);
#endif

      if (!pbi->refresh_frame_flags) {
        // NOTE: "pbi->refresh_frame_flags == 0" indicates that the coded frame
        //       will not be used as a reference
        cm->is_reference_frame = 0;
      }

      for (int i = 0; i < INTER_REFS_PER_FRAME; ++i) {
        const int ref = aom_rb_read_literal(rb, REF_FRAMES_LOG2);
        const int idx = cm->ref_frame_map[ref];

        // Most of the time, streams start with a keyframe. In that case,
        // ref_frame_map will have been filled in at that point and will not
        // contain any -1's. However, streams are explicitly allowed to start
        // with an intra-only frame, so long as they don't then signal a
        // reference to a slot that hasn't been set yet. That's what we are
        // checking here.
        if (idx == -1)
          aom_internal_error(&cm->error, AOM_CODEC_CORRUPT_FRAME,
                             "Inter frame requests nonexistent reference");

        RefBuffer *const ref_frame = &cm->frame_refs[i];
        ref_frame->idx = idx;
        ref_frame->buf = &frame_bufs[idx].buf;
#if CONFIG_FRAME_SIGN_BIAS
#if CONFIG_OBU
        // NOTE: For the scenario of (cm->frame_type != S_FRAME),
        // ref_frame_sign_bias will be reset based on frame offsets.
        cm->ref_frame_sign_bias[LAST_FRAME + i] = 0;
#endif  // CONFIG_OBU
#else   // !CONFIG_FRAME_SIGN_BIAS
#if CONFIG_OBU
        cm->ref_frame_sign_bias[LAST_FRAME + i] =
            (cm->frame_type == S_FRAME) ? 0 : aom_rb_read_bit(rb);
#else   // !CONFIG_OBU
        cm->ref_frame_sign_bias[LAST_FRAME + i] = aom_rb_read_bit(rb);
#endif  // CONFIG_OBU
#endif  // CONFIG_FRAME_SIGN_BIAS
#if CONFIG_REFERENCE_BUFFER
        if (cm->seq_params.frame_id_numbers_present_flag) {
          int frame_id_length = cm->seq_params.frame_id_length;
          int diff_len = cm->seq_params.delta_frame_id_length;
          int delta_frame_id_minus1 = aom_rb_read_literal(rb, diff_len);
          int ref_frame_id =
              ((cm->current_frame_id - (delta_frame_id_minus1 + 1) +
                (1 << frame_id_length)) %
               (1 << frame_id_length));
          /* Compare values derived from delta_frame_id_minus1 and
           * refresh_frame_flags. Also, check valid for referencing */
          if (ref_frame_id != cm->ref_frame_id[ref] ||
              cm->valid_for_referencing[ref] == 0)
            aom_internal_error(&cm->error, AOM_CODEC_CORRUPT_FRAME,
                               "Reference buffer frame ID mismatch");
        }
#endif  // CONFIG_REFERENCE_BUFFER
      }

#if CONFIG_FRAME_SIZE
      if (cm->error_resilient_mode == 0 && frame_size_override_flag) {
        setup_frame_size_with_refs(cm, rb);
      } else {
        setup_frame_size(cm, frame_size_override_flag, rb);
      }
#else
      setup_frame_size_with_refs(cm, rb);
#endif

#if CONFIG_AMVR
      if (cm->seq_force_integer_mv == 2) {
        cm->cur_frame_force_integer_mv = aom_rb_read_bit(rb);
      } else {
        cm->cur_frame_force_integer_mv = cm->seq_force_integer_mv;
      }

      if (cm->cur_frame_force_integer_mv) {
        cm->allow_high_precision_mv = 0;
      } else {
#if CONFIG_EIGHTH_PEL_MV_ONLY
        cm->allow_high_precision_mv = 1;
#else
        cm->allow_high_precision_mv = aom_rb_read_bit(rb);
#endif  // CONFIG_EIGHTH_PEL_MV_ONLY
      }
#else
#if CONFIG_EIGHTH_PEL_MV_ONLY
      cm->allow_high_precision_mv = 1;
#else
      cm->allow_high_precision_mv = aom_rb_read_bit(rb);
#endif  // CONFIG_EIGHTH_PEL_MV_ONLY
#endif
      cm->interp_filter = read_frame_interp_filter(rb);
#if CONFIG_TEMPMV_SIGNALING
      if (frame_might_use_prev_frame_mvs(cm))
        cm->use_ref_frame_mvs = aom_rb_read_bit(rb);
      else
        cm->use_ref_frame_mvs = 0;

      cm->prev_frame =
          cm->frame_refs[LAST_FRAME - LAST_FRAME].idx != INVALID_IDX
              ? &cm->buffer_pool
                     ->frame_bufs[cm->frame_refs[LAST_FRAME - LAST_FRAME].idx]
              : NULL;
      cm->use_prev_frame_mvs =
          cm->use_ref_frame_mvs && frame_can_use_prev_frame_mvs(cm);
#endif
      for (int i = 0; i < INTER_REFS_PER_FRAME; ++i) {
        RefBuffer *const ref_buf = &cm->frame_refs[i];
#if CONFIG_HIGHBITDEPTH
        av1_setup_scale_factors_for_frame(
            &ref_buf->sf, ref_buf->buf->y_crop_width,
            ref_buf->buf->y_crop_height, cm->width, cm->height,
            cm->use_highbitdepth);
#else
        av1_setup_scale_factors_for_frame(
            &ref_buf->sf, ref_buf->buf->y_crop_width,
            ref_buf->buf->y_crop_height, cm->width, cm->height);
#endif
      }
    }
  }

#if CONFIG_FRAME_MARKER
  if (cm->show_frame == 0) {
    cm->frame_offset =
        cm->current_video_frame + aom_rb_read_literal(rb, FRAME_OFFSET_BITS);
  } else {
    cm->frame_offset = cm->current_video_frame;
  }
  av1_setup_frame_buf_refs(cm);

#if CONFIG_EXT_SKIP
  av1_setup_skip_mode_allowed(cm);
  cm->skip_mode_flag = cm->is_skip_mode_allowed ? aom_rb_read_bit(rb) : 0;
#if 0
  printf(
      "DECODER: Frame=%d, frame_offset=%d, show_frame=%d, "
      "show_existing_frame=%d, is_skip_mode_allowed=%d, "
      "ref_frame_idx=(%d,%d), frame_reference_mode=%d, "
      "tpl_frame_ref0_idx=%d, skip_mode_flag=%d\n\n",
      cm->current_video_frame, cm->frame_offset, cm->show_frame,
      cm->show_existing_frame, cm->is_skip_mode_allowed, cm->ref_frame_idx_0,
      cm->ref_frame_idx_1, cm->reference_mode, cm->tpl_frame_ref0_idx,
      cm->skip_mode_flag);
#endif  // 0
#endif  // CONFIG_EXT_SKIP

#if CONFIG_FRAME_SIGN_BIAS
#if CONFIG_OBU
  if (cm->frame_type != S_FRAME)
#endif  // CONFIG_OBU
    av1_setup_frame_sign_bias(cm);
#endif  // CONFIG_FRAME_SIGN_BIAS
#endif  // CONFIG_FRAME_MARKER

#if CONFIG_TEMPMV_SIGNALING
  cm->cur_frame->intra_only = cm->frame_type == KEY_FRAME || cm->intra_only;
#endif

#if CONFIG_REFERENCE_BUFFER
  if (cm->seq_params.frame_id_numbers_present_flag) {
    /* If bitmask is set, update reference frame id values and
       mark frames as valid for reference */
    int refresh_frame_flags =
        cm->frame_type == KEY_FRAME ? 0xFF : pbi->refresh_frame_flags;
    for (int i = 0; i < REF_FRAMES; i++) {
      if ((refresh_frame_flags >> i) & 1) {
        cm->ref_frame_id[i] = cm->current_frame_id;
        cm->valid_for_referencing[i] = 1;
      }
    }
  }
#endif  // CONFIG_REFERENCE_BUFFER

  get_frame_new_buffer(cm)->bit_depth = cm->bit_depth;
  get_frame_new_buffer(cm)->color_space = cm->color_space;
#if CONFIG_COLORSPACE_HEADERS
  get_frame_new_buffer(cm)->transfer_function = cm->transfer_function;
  get_frame_new_buffer(cm)->chroma_sample_position = cm->chroma_sample_position;
#endif
  get_frame_new_buffer(cm)->color_range = cm->color_range;
  get_frame_new_buffer(cm)->render_width = cm->render_width;
  get_frame_new_buffer(cm)->render_height = cm->render_height;

  if (pbi->need_resync) {
    aom_internal_error(&cm->error, AOM_CODEC_CORRUPT_FRAME,
                       "Keyframe / intra-only frame required to reset decoder"
                       " state");
  }

#if CONFIG_EXT_TILE
  const int might_bwd_adapt =
      !(cm->error_resilient_mode || cm->large_scale_tile);
#else
  const int might_bwd_adapt = !cm->error_resilient_mode;
#endif  // CONFIG_EXT_TILE
  if (might_bwd_adapt) {
    cm->refresh_frame_context = aom_rb_read_bit(rb)
                                    ? REFRESH_FRAME_CONTEXT_FORWARD
                                    : REFRESH_FRAME_CONTEXT_BACKWARD;
  } else {
    cm->refresh_frame_context = REFRESH_FRAME_CONTEXT_FORWARD;
  }
#if !CONFIG_NO_FRAME_CONTEXT_SIGNALING
  // This flag will be overridden by the call to av1_setup_past_independence
  // below, forcing the use of context 0 for those frame types.
  cm->frame_context_idx = aom_rb_read_literal(rb, FRAME_CONTEXTS_LOG2);
#else
  cm->primary_ref_frame = aom_rb_read_literal(rb, 3);
#endif

  // Generate next_ref_frame_map.
  lock_buffer_pool(pool);
  int ref_index = 0;
  for (int mask = pbi->refresh_frame_flags; mask; mask >>= 1) {
    if (mask & 1) {
      cm->next_ref_frame_map[ref_index] = cm->new_fb_idx;
      ++frame_bufs[cm->new_fb_idx].ref_count;
    } else {
      cm->next_ref_frame_map[ref_index] = cm->ref_frame_map[ref_index];
    }
    // Current thread holds the reference frame.
    if (cm->ref_frame_map[ref_index] >= 0)
      ++frame_bufs[cm->ref_frame_map[ref_index]].ref_count;
    ++ref_index;
  }

  for (; ref_index < REF_FRAMES; ++ref_index) {
    cm->next_ref_frame_map[ref_index] = cm->ref_frame_map[ref_index];

    // Current thread holds the reference frame.
    if (cm->ref_frame_map[ref_index] >= 0)
      ++frame_bufs[cm->ref_frame_map[ref_index]].ref_count;
  }
  unlock_buffer_pool(pool);
  pbi->hold_ref_buf = 1;

  if (frame_is_intra_only(cm) || cm->error_resilient_mode)
    av1_setup_past_independence(cm);

#if CONFIG_INTRABC
  if (cm->allow_intrabc && NO_FILTER_FOR_IBC) {
    // Set parameters corresponding to no filtering.
    struct loopfilter *lf = &cm->lf;
#if CONFIG_LOOPFILTER_LEVEL
    lf->filter_level[0] = 0;
    lf->filter_level[1] = 0;
#else
    lf->filter_level = 0;
#endif
    cm->cdef_bits = 0;
    cm->cdef_strengths[0] = 0;
    cm->nb_cdef_strengths = 1;
#if CONFIG_LOOP_RESTORATION
    cm->rst_info[0].frame_restoration_type = RESTORE_NONE;
    cm->rst_info[1].frame_restoration_type = RESTORE_NONE;
    cm->rst_info[2].frame_restoration_type = RESTORE_NONE;
#endif  // CONFIG_LOOP_RESTORATION
  }
#endif  // CONFIG_INTRABC

  setup_loopfilter(cm, rb);
  setup_quantization(cm, rb);
  xd->bd = (int)cm->bit_depth;

#if CONFIG_Q_ADAPT_PROBS
  if (frame_is_intra_only(cm) || cm->error_resilient_mode) {
    av1_default_coef_probs(cm);
    av1_setup_frame_contexts(cm);
  }
#endif  // CONFIG_Q_ADAPT_PROBS

  setup_segmentation(cm, rb);
#if CONFIG_Q_SEGMENTATION
  setup_q_segmentation(cm, rb);
#endif

  {
    int delta_q_allowed = 1;
#if !CONFIG_EXT_DELTA_Q
    struct segmentation *const seg = &cm->seg;
    int segment_quantizer_active = 0;
    for (int i = 0; i < MAX_SEGMENTS; i++) {
      if (segfeature_active(seg, i, SEG_LVL_ALT_Q)) {
        segment_quantizer_active = 1;
      }
#if CONFIG_Q_SEGMENTATION
      if (seg->q_lvls) segment_quantizer_active = 1;
#endif
    }
    delta_q_allowed = !segment_quantizer_active;
#endif

    cm->delta_q_res = 1;
#if CONFIG_EXT_DELTA_Q
    cm->delta_lf_res = 1;
    cm->delta_lf_present_flag = 0;
#if CONFIG_LOOPFILTER_LEVEL
    cm->delta_lf_multi = 0;
#endif  // CONFIG_LOOPFILTER_LEVEL
#endif
    if (delta_q_allowed == 1 && cm->base_qindex > 0) {
      cm->delta_q_present_flag = aom_rb_read_bit(rb);
    } else {
      cm->delta_q_present_flag = 0;
    }
    if (cm->delta_q_present_flag) {
      xd->prev_qindex = cm->base_qindex;
      cm->delta_q_res = 1 << aom_rb_read_literal(rb, 2);
#if CONFIG_EXT_DELTA_Q
      cm->delta_lf_present_flag = aom_rb_read_bit(rb);
      if (cm->delta_lf_present_flag) {
        xd->prev_delta_lf_from_base = 0;
        cm->delta_lf_res = 1 << aom_rb_read_literal(rb, 2);
#if CONFIG_LOOPFILTER_LEVEL
        cm->delta_lf_multi = aom_rb_read_bit(rb);
        for (int lf_id = 0; lf_id < FRAME_LF_COUNT; ++lf_id)
          xd->prev_delta_lf[lf_id] = 0;
#endif  // CONFIG_LOOPFILTER_LEVEL
      }
#endif  // CONFIG_EXT_DELTA_Q
    }
  }
#if CONFIG_AMVR
  xd->cur_frame_force_integer_mv = cm->cur_frame_force_integer_mv;
#endif

  for (int i = 0; i < MAX_SEGMENTS; ++i) {
    const int qindex = cm->seg.enabled
#if CONFIG_Q_SEGMENTATION
                           ? av1_get_qindex(&cm->seg, i, i, cm->base_qindex)
#else
                           ? av1_get_qindex(&cm->seg, i, cm->base_qindex)
#endif
                           : cm->base_qindex;
    xd->lossless[i] = qindex == 0 && cm->y_dc_delta_q == 0 &&
                      cm->u_dc_delta_q == 0 && cm->u_ac_delta_q == 0 &&
                      cm->v_dc_delta_q == 0 && cm->v_ac_delta_q == 0;
    xd->qindex[i] = qindex;
  }
  cm->all_lossless = all_lossless(cm, xd);
  setup_segmentation_dequant(cm);
  if (!cm->all_lossless) {
    setup_cdef(cm, rb);
  }
#if CONFIG_LOOP_RESTORATION
  decode_restoration_mode(cm, rb);
#endif  // CONFIG_LOOP_RESTORATION
  cm->tx_mode = read_tx_mode(cm, rb);
  cm->reference_mode = read_frame_reference_mode(cm, rb);
  if (cm->reference_mode != SINGLE_REFERENCE) setup_compound_reference_mode(cm);
  read_compound_tools(cm, rb);

  cm->reduced_tx_set_used = aom_rb_read_bit(rb);

#if CONFIG_ADAPT_SCAN
#if CONFIG_EXT_TILE
  if (cm->large_scale_tile)
    cm->use_adapt_scan = 0;
  else
#endif  // CONFIG_EXT_TILE
    cm->use_adapt_scan = aom_rb_read_bit(rb);
  // TODO(angiebird): call av1_init_scan_order only when use_adapt_scan
  // switches from 1 to 0
  if (cm->use_adapt_scan == 0) av1_init_scan_order(cm);
#endif  // CONFIG_ADAPT_SCAN

#if !CONFIG_TEMPMV_SIGNALING
  // NOTE(zoeliu): As cm->prev_frame can take neither a frame of
  //               show_exisiting_frame=1, nor can it take a frame not used as
  //               a reference, it is probable that by the time it is being
  //               referred to, the frame buffer it originally points to may
  //               already get expired and have been reassigned to the current
  //               newly coded frame. Hence, we need to check whether this is
  //               the case, and if yes, we have 2 choices:
  //               (1) Simply disable the use of previous frame mvs; or
  //               (2) Have cm->prev_frame point to one reference frame buffer,
  //                   e.g. LAST_FRAME.
  if (!dec_is_ref_frame_buf(pbi, cm->prev_frame)) {
    // Reassign the LAST_FRAME buffer to cm->prev_frame.
    cm->prev_frame =
        cm->frame_refs[LAST_FRAME - LAST_FRAME].idx != INVALID_IDX
            ? &cm->buffer_pool
                   ->frame_bufs[cm->frame_refs[LAST_FRAME - LAST_FRAME].idx]
            : NULL;
  }
#endif

#if CONFIG_TEMPMV_SIGNALING
  if (cm->use_prev_frame_mvs && !frame_can_use_prev_frame_mvs(cm)) {
    aom_internal_error(&cm->error, AOM_CODEC_CORRUPT_FRAME,
                       "Frame wrongly requests previous frame MVs");
  }
#else
  cm->use_prev_frame_mvs = !cm->error_resilient_mode && cm->prev_frame &&
#if CONFIG_FRAME_SUPERRES
                           cm->width == cm->last_width &&
                           cm->height == cm->last_height &&
#else
                           cm->width == cm->prev_frame->buf.y_crop_width &&
                           cm->height == cm->prev_frame->buf.y_crop_height &&
#endif  // CONFIG_FRAME_SUPERRES
                           !cm->last_intra_only && cm->last_show_frame &&
                           (cm->last_frame_type != KEY_FRAME);
#endif  // CONFIG_TEMPMV_SIGNALING

  if (!frame_is_intra_only(cm)) read_global_motion(cm, rb);

  read_tile_info(pbi, rb);

  size_t sz;
  if (use_compressed_header(cm)) {
    sz = aom_rb_read_literal(rb, 16);
    if (sz == 0)
      aom_internal_error(&cm->error, AOM_CODEC_CORRUPT_FRAME,
                         "Invalid header size");
  } else {
    sz = 0;
  }
  return sz;
}

static int read_compressed_header(AV1Decoder *pbi, const uint8_t *data,
                                  size_t partition_size) {
  (void)pbi;
  (void)data;
  (void)partition_size;
  return 0;
}

#ifdef NDEBUG
#define debug_check_frame_counts(cm) (void)0
#else  // !NDEBUG
// Counts should only be incremented when frame_parallel_decoding_mode and
// error_resilient_mode are disabled.
static void debug_check_frame_counts(const AV1_COMMON *const cm) {
  FRAME_COUNTS zero_counts;
  av1_zero(zero_counts);
  assert(cm->refresh_frame_context != REFRESH_FRAME_CONTEXT_BACKWARD ||
         cm->error_resilient_mode);
  assert(!memcmp(cm->counts.partition, zero_counts.partition,
                 sizeof(cm->counts.partition)));
  assert(!memcmp(cm->counts.switchable_interp, zero_counts.switchable_interp,
                 sizeof(cm->counts.switchable_interp)));
  assert(!memcmp(cm->counts.inter_compound_mode,
                 zero_counts.inter_compound_mode,
                 sizeof(cm->counts.inter_compound_mode)));
  assert(!memcmp(cm->counts.interintra, zero_counts.interintra,
                 sizeof(cm->counts.interintra)));
  assert(!memcmp(cm->counts.wedge_interintra, zero_counts.wedge_interintra,
                 sizeof(cm->counts.wedge_interintra)));
  assert(!memcmp(cm->counts.compound_interinter,
                 zero_counts.compound_interinter,
                 sizeof(cm->counts.compound_interinter)));
  assert(!memcmp(cm->counts.motion_mode, zero_counts.motion_mode,
                 sizeof(cm->counts.motion_mode)));
  assert(!memcmp(cm->counts.intra_inter, zero_counts.intra_inter,
                 sizeof(cm->counts.intra_inter)));
  assert(!memcmp(cm->counts.comp_inter, zero_counts.comp_inter,
                 sizeof(cm->counts.comp_inter)));
#if CONFIG_EXT_COMP_REFS
  assert(!memcmp(cm->counts.comp_ref_type, zero_counts.comp_ref_type,
                 sizeof(cm->counts.comp_ref_type)));
  assert(!memcmp(cm->counts.uni_comp_ref, zero_counts.uni_comp_ref,
                 sizeof(cm->counts.uni_comp_ref)));
#endif  // CONFIG_EXT_COMP_REFS
  assert(!memcmp(cm->counts.single_ref, zero_counts.single_ref,
                 sizeof(cm->counts.single_ref)));
  assert(!memcmp(cm->counts.comp_ref, zero_counts.comp_ref,
                 sizeof(cm->counts.comp_ref)));
  assert(!memcmp(cm->counts.comp_bwdref, zero_counts.comp_bwdref,
                 sizeof(cm->counts.comp_bwdref)));
  assert(!memcmp(cm->counts.skip, zero_counts.skip, sizeof(cm->counts.skip)));
}
#endif  // NDEBUG

static struct aom_read_bit_buffer *init_read_bit_buffer(
    AV1Decoder *pbi, struct aom_read_bit_buffer *rb, const uint8_t *data,
    const uint8_t *data_end, uint8_t clear_data[MAX_AV1_HEADER_SIZE]) {
  rb->bit_offset = 0;
  rb->error_handler = error_handler;
  rb->error_handler_data = &pbi->common;
  if (pbi->decrypt_cb) {
    const int n = (int)AOMMIN(MAX_AV1_HEADER_SIZE, data_end - data);
    pbi->decrypt_cb(pbi->decrypt_state, data, clear_data, n);
    rb->bit_buffer = clear_data;
    rb->bit_buffer_end = clear_data + n;
  } else {
    rb->bit_buffer = data;
    rb->bit_buffer_end = data_end;
  }
  return rb;
}

//------------------------------------------------------------------------------

#if CONFIG_FRAME_SIZE
void av1_read_frame_size(struct aom_read_bit_buffer *rb, int num_bits_width,
                         int num_bits_height, int *width, int *height) {
  *width = aom_rb_read_literal(rb, num_bits_width) + 1;
  *height = aom_rb_read_literal(rb, num_bits_height) + 1;
#else
void av1_read_frame_size(struct aom_read_bit_buffer *rb, int *width,
                         int *height) {
  *width = aom_rb_read_literal(rb, 16) + 1;
  *height = aom_rb_read_literal(rb, 16) + 1;
#endif
}

BITSTREAM_PROFILE av1_read_profile(struct aom_read_bit_buffer *rb) {
  int profile = aom_rb_read_bit(rb);
  profile |= aom_rb_read_bit(rb) << 1;
  if (profile > 2) profile += aom_rb_read_bit(rb);
  return (BITSTREAM_PROFILE)profile;
}

static void make_update_tile_list_dec(AV1Decoder *pbi, int start_tile,
                                      int num_tile, FRAME_CONTEXT *ec_ctxs[]) {
  for (int i = start_tile; i < start_tile + num_tile; ++i)
    ec_ctxs[i - start_tile] = &pbi->tile_data[i].tctx;
}

#if CONFIG_FRAME_SUPERRES
void superres_post_decode(AV1Decoder *pbi) {
  AV1_COMMON *const cm = &pbi->common;
  BufferPool *const pool = cm->buffer_pool;

  if (av1_superres_unscaled(cm)) return;

  lock_buffer_pool(pool);
  av1_superres_upscale(cm, pool);
  unlock_buffer_pool(pool);
}
#endif  // CONFIG_FRAME_SUPERRES

static void dec_setup_frame_boundary_info(AV1_COMMON *const cm) {
// Note: When LOOPFILTERING_ACROSS_TILES is enabled, we need to clear the
// boundary information every frame, since the tile boundaries may
// change every frame (particularly when dependent-horztiles is also
// enabled); when it is disabled, the only information stored is the frame
// boundaries, which only depend on the frame size.
#if !CONFIG_LOOPFILTERING_ACROSS_TILES
  if (cm->width != cm->last_width || cm->height != cm->last_height)
#endif  // CONFIG_LOOPFILTERING_ACROSS_TILES
  {
    int row, col;
    for (row = 0; row < cm->mi_rows; ++row) {
      MODE_INFO *mi = cm->mi + row * cm->mi_stride;
      for (col = 0; col < cm->mi_cols; ++col) {
        mi->mbmi.boundary_info = 0;
        mi++;
      }
    }
    av1_setup_frame_boundary_info(cm);
  }
}

size_t av1_decode_frame_headers_and_setup(AV1Decoder *pbi, const uint8_t *data,
                                          const uint8_t *data_end,
                                          const uint8_t **p_data_end) {
  AV1_COMMON *const cm = &pbi->common;
  MACROBLOCKD *const xd = &pbi->mb;
#if !CONFIG_TEMPMV_SIGNALING
  RefBuffer *last_fb_ref_buf = &cm->frame_refs[LAST_FRAME - LAST_FRAME];
#endif

#if CONFIG_ADAPT_SCAN
  av1_deliver_eob_threshold(cm, xd);
#endif
#if CONFIG_BITSTREAM_DEBUG
  bitstream_queue_set_frame_read(cm->current_video_frame * 2 + cm->show_frame);
#endif

  for (int i = LAST_FRAME; i <= ALTREF_FRAME; ++i) {
    cm->global_motion[i] = default_warp_params;
    cm->cur_frame->global_motion[i] = default_warp_params;
  }
  xd->global_motion = cm->global_motion;

  uint8_t clear_data[MAX_AV1_HEADER_SIZE];
  struct aom_read_bit_buffer rb;
  const size_t first_partition_size = read_uncompressed_header(
      pbi, init_read_bit_buffer(pbi, &rb, data, data_end, clear_data));

#if CONFIG_EXT_TILE
  // If cm->single_tile_decoding = 0, the independent decoding of a single tile
  // or a section of a frame is not allowed.
  if (!cm->single_tile_decoding &&
      (pbi->dec_tile_row >= 0 || pbi->dec_tile_col >= 0)) {
    pbi->dec_tile_row = -1;
    pbi->dec_tile_col = -1;
  }
#endif  // CONFIG_EXT_TILE

  pbi->first_partition_size = first_partition_size;
  pbi->uncomp_hdr_size = aom_rb_bytes_read(&rb);
  YV12_BUFFER_CONFIG *new_fb = get_frame_new_buffer(cm);
  xd->cur_buf = new_fb;
#if CONFIG_INTRABC
#if CONFIG_HIGHBITDEPTH
  av1_setup_scale_factors_for_frame(
      &xd->sf_identity, xd->cur_buf->y_crop_width, xd->cur_buf->y_crop_height,
      xd->cur_buf->y_crop_width, xd->cur_buf->y_crop_height,
      cm->use_highbitdepth);
#else
  av1_setup_scale_factors_for_frame(
      &xd->sf_identity, xd->cur_buf->y_crop_width, xd->cur_buf->y_crop_height,
      xd->cur_buf->y_crop_width, xd->cur_buf->y_crop_height);
#endif  // CONFIG_HIGHBITDEPTH
#endif  // CONFIG_INTRABC

  if (cm->show_existing_frame) {
    // showing a frame directly
    *p_data_end = data + aom_rb_bytes_read(&rb);
    return 0;
  }

  data += aom_rb_bytes_read(&rb);
  if (first_partition_size)
    if (!read_is_valid(data, first_partition_size, data_end))
      aom_internal_error(&cm->error, AOM_CODEC_CORRUPT_FRAME,
                         "Truncated packet or corrupt header length");

  cm->setup_mi(cm);

#if !CONFIG_TEMPMV_SIGNALING
  // NOTE(zoeliu): As cm->prev_frame can take neither a frame of
  //               show_exisiting_frame=1, nor can it take a frame not used as
  //               a reference, it is probable that by the time it is being
  //               referred to, the frame buffer it originally points to may
  //               already get expired and have been reassigned to the current
  //               newly coded frame. Hence, we need to check whether this is
  //               the case, and if yes, we have 2 choices:
  //               (1) Simply disable the use of previous frame mvs; or
  //               (2) Have cm->prev_frame point to one reference frame buffer,
  //                   e.g. LAST_FRAME.
  if (!dec_is_ref_frame_buf(pbi, cm->prev_frame)) {
    // Reassign the LAST_FRAME buffer to cm->prev_frame.
    cm->prev_frame = last_fb_ref_buf->idx != INVALID_IDX
                         ? &cm->buffer_pool->frame_bufs[last_fb_ref_buf->idx]
                         : NULL;
  }
#endif

#if CONFIG_TEMPMV_SIGNALING
  if (cm->use_prev_frame_mvs && !frame_can_use_prev_frame_mvs(cm)) {
    aom_internal_error(&cm->error, AOM_CODEC_CORRUPT_FRAME,
                       "Frame wrongly requests previous frame MVs");
  }
#else
  cm->use_prev_frame_mvs = !cm->error_resilient_mode && cm->prev_frame &&
#if CONFIG_FRAME_SUPERRES
                           cm->width == cm->last_width &&
                           cm->height == cm->last_height &&
#else
                           cm->width == cm->prev_frame->buf.y_crop_width &&
                           cm->height == cm->prev_frame->buf.y_crop_height &&
#endif  // CONFIG_FRAME_SUPERRES
                           !cm->last_intra_only && cm->last_show_frame &&
                           (cm->last_frame_type != KEY_FRAME);
#endif  // CONFIG_TEMPMV_SIGNALING

#if CONFIG_SEGMENT_PRED_LAST
  cm->current_frame_seg_map = cm->cur_frame->seg_map;
  if (cm->current_frame_seg_map)
    memset(cm->current_frame_seg_map, 0, (cm->mi_rows * cm->mi_cols));
  if (cm->seg.temporal_update) {
    cm->last_frame_seg_map = cm->prev_frame->seg_map;
  }
#endif

#if CONFIG_MFMV
  av1_setup_motion_field(cm);
#endif  // CONFIG_MFMV

  av1_setup_block_planes(xd, cm->subsampling_x, cm->subsampling_y);
#if CONFIG_NO_FRAME_CONTEXT_SIGNALING
  if (cm->error_resilient_mode || frame_is_intra_only(cm)) {
    // use the default frame context values
    *cm->fc = cm->frame_contexts[FRAME_CONTEXT_DEFAULTS];
    cm->pre_fc = &cm->frame_contexts[FRAME_CONTEXT_DEFAULTS];
  } else {
    *cm->fc = cm->frame_contexts[cm->frame_refs[cm->primary_ref_frame].idx];
    cm->pre_fc = &cm->frame_contexts[cm->frame_refs[cm->primary_ref_frame].idx];
  }
#else
  *cm->fc = cm->frame_contexts[cm->frame_context_idx];
  cm->pre_fc = &cm->frame_contexts[cm->frame_context_idx];
#endif  // CONFIG_NO_FRAME_CONTEXT_SIGNALING
  if (!cm->fc->initialized)
    aom_internal_error(&cm->error, AOM_CODEC_CORRUPT_FRAME,
                       "Uninitialized entropy context.");

  av1_zero(cm->counts);

  xd->corrupted = 0;
  if (first_partition_size) {
    new_fb->corrupted = read_compressed_header(pbi, data, first_partition_size);
    if (new_fb->corrupted)
      aom_internal_error(&cm->error, AOM_CODEC_CORRUPT_FRAME,
                         "Decode failed. Frame data header is corrupted.");
  }
  return first_partition_size;
}

// Once-per-frame initialization
static void setup_frame_info(AV1Decoder *pbi) {
  AV1_COMMON *const cm = &pbi->common;

#if CONFIG_LOOP_RESTORATION
  if (cm->rst_info[0].frame_restoration_type != RESTORE_NONE ||
      cm->rst_info[1].frame_restoration_type != RESTORE_NONE ||
      cm->rst_info[2].frame_restoration_type != RESTORE_NONE) {
    av1_alloc_restoration_buffers(cm);
  }
#endif

#if !CONFIG_LOOPFILTER_LEVEL
  if (cm->lf.filter_level && !cm->skip_loop_filter) {
    av1_loop_filter_frame_init(cm, cm->lf.filter_level, cm->lf.filter_level);
  }
#endif

  // If encoded in frame parallel mode, frame context is ready after decoding
  // the frame header.
  if (cm->frame_parallel_decode &&
      cm->refresh_frame_context != REFRESH_FRAME_CONTEXT_BACKWARD) {
    AVxWorker *const worker = pbi->frame_worker_owner;
    FrameWorkerData *const frame_worker_data = worker->data1;
    if (cm->refresh_frame_context == REFRESH_FRAME_CONTEXT_FORWARD) {
#if CONFIG_NO_FRAME_CONTEXT_SIGNALING
      cm->frame_contexts[cm->new_fb_idx] = *cm->fc;
#else
      cm->frame_contexts[cm->frame_context_idx] = *cm->fc;
#endif  // CONFIG_NO_FRAME_CONTEXT_SIGNALING
    }
    av1_frameworker_lock_stats(worker);
    pbi->cur_buf->row = -1;
    pbi->cur_buf->col = -1;
    frame_worker_data->frame_context_ready = 1;
    // Signal the main thread that context is ready.
    av1_frameworker_signal_stats(worker);
    av1_frameworker_unlock_stats(worker);
  }

  dec_setup_frame_boundary_info(cm);
}

void av1_decode_tg_tiles_and_wrapup(AV1Decoder *pbi, const uint8_t *data,
                                    const uint8_t *data_end,
                                    const uint8_t **p_data_end, int startTile,
                                    int endTile, int initialize_flag) {
  AV1_COMMON *const cm = &pbi->common;
  MACROBLOCKD *const xd = &pbi->mb;

  if (initialize_flag) setup_frame_info(pbi);

#if CONFIG_OBU
  *p_data_end = decode_tiles(pbi, data, data_end, startTile, endTile);
#else
  *p_data_end =
      decode_tiles(pbi, data + pbi->uncomp_hdr_size + pbi->first_partition_size,
                   data_end, startTile, endTile);
#endif

#if CONFIG_MONO_VIDEO
  // If the bit stream is monochrome, or the decoder is set to force a
  // monochrome output, set the U and V buffers to a constant.
  if (pbi->monochrome || cm->seq_params.monochrome) {
#if CONFIG_HIGHBITDEPTH
    const int bytes_per_sample = cm->use_highbitdepth ? 2 : 1;
#else
    const int bytes_per_sample = 1;
#endif

    YV12_BUFFER_CONFIG *cur_buf = (YV12_BUFFER_CONFIG *)xd->cur_buf;

    for (int buf_idx = 1; buf_idx <= 2; buf_idx++) {
      for (int row_idx = 0; row_idx < cur_buf->crop_heights[1]; row_idx++) {
        memset(&cur_buf->buffers[buf_idx][row_idx * cur_buf->uv_stride], 1 << 7,
               cur_buf->crop_widths[1] * bytes_per_sample);
      }
    }
  }
#endif

  if (endTile != cm->tile_rows * cm->tile_cols - 1) {
    return;
  }

#if CONFIG_STRIPED_LOOP_RESTORATION
#if CONFIG_FRAME_SUPERRES && CONFIG_HORZONLY_FRAME_SUPERRES
  if (!av1_superres_unscaled(cm)) aom_extend_frame_borders(&pbi->cur_buf->buf);
#endif
  if (cm->rst_info[0].frame_restoration_type != RESTORE_NONE ||
      cm->rst_info[1].frame_restoration_type != RESTORE_NONE ||
      cm->rst_info[2].frame_restoration_type != RESTORE_NONE) {
    av1_loop_restoration_save_boundary_lines(&pbi->cur_buf->buf, cm, 0);
  }
#endif

  if (!cm->skip_loop_filter &&
#if CONFIG_INTRABC
      !(cm->allow_intrabc && NO_FILTER_FOR_IBC) &&
#endif  // CONFIG_INTRABC
      !cm->all_lossless) {
    av1_cdef_frame(&pbi->cur_buf->buf, cm, &pbi->mb);
  }

#if CONFIG_FRAME_SUPERRES
  superres_post_decode(pbi);
#endif  // CONFIG_FRAME_SUPERRES

#if CONFIG_LOOP_RESTORATION
  if (cm->rst_info[0].frame_restoration_type != RESTORE_NONE ||
      cm->rst_info[1].frame_restoration_type != RESTORE_NONE ||
      cm->rst_info[2].frame_restoration_type != RESTORE_NONE) {
#if CONFIG_STRIPED_LOOP_RESTORATION
    av1_loop_restoration_save_boundary_lines(&pbi->cur_buf->buf, cm, 1);
#endif
    av1_loop_restoration_filter_frame((YV12_BUFFER_CONFIG *)xd->cur_buf, cm,
                                      cm->rst_info, 7, NULL);
  }
#endif  // CONFIG_LOOP_RESTORATION

  if (!xd->corrupted) {
    if (cm->refresh_frame_context == REFRESH_FRAME_CONTEXT_BACKWARD) {
#if CONFIG_SIMPLE_BWD_ADAPT
      const int num_bwd_ctxs = 1;
#else
      const int num_bwd_ctxs = cm->tile_rows * cm->tile_cols;
#endif
      FRAME_CONTEXT **tile_ctxs =
          aom_malloc(num_bwd_ctxs * sizeof(&pbi->tile_data[0].tctx));
      aom_cdf_prob **cdf_ptrs = aom_malloc(
          num_bwd_ctxs * sizeof(&pbi->tile_data[0].tctx.partition_cdf[0][0]));
#if CONFIG_SIMPLE_BWD_ADAPT
      make_update_tile_list_dec(pbi, cm->largest_tile_id, num_bwd_ctxs,
                                tile_ctxs);
#else
      make_update_tile_list_dec(pbi, 0, num_bwd_ctxs, tile_ctxs);
#endif
#if CONFIG_SYMBOLRATE
      av1_dump_symbol_rate(cm);
#endif
      av1_adapt_intra_frame_probs(cm);
      av1_average_tile_coef_cdfs(pbi->common.fc, tile_ctxs, cdf_ptrs,
                                 num_bwd_ctxs);
      av1_average_tile_intra_cdfs(pbi->common.fc, tile_ctxs, cdf_ptrs,
                                  num_bwd_ctxs);
      av1_average_tile_loopfilter_cdfs(pbi->common.fc, tile_ctxs, cdf_ptrs,
                                       num_bwd_ctxs);
#if CONFIG_ADAPT_SCAN
      av1_adapt_scan_order(cm, tile_ctxs, num_bwd_ctxs);
#endif  // CONFIG_ADAPT_SCAN

      if (!frame_is_intra_only(cm)) {
        av1_adapt_inter_frame_probs(cm);
        av1_average_tile_inter_cdfs(&pbi->common, pbi->common.fc, tile_ctxs,
                                    cdf_ptrs, num_bwd_ctxs);
        av1_average_tile_mv_cdfs(pbi->common.fc, tile_ctxs, cdf_ptrs,
                                 num_bwd_ctxs);
      }
      aom_free(tile_ctxs);
      aom_free(cdf_ptrs);
    } else {
      debug_check_frame_counts(cm);
    }
  } else {
    aom_internal_error(&cm->error, AOM_CODEC_CORRUPT_FRAME,
                       "Decode failed. Frame data is corrupted.");
  }

#if CONFIG_INSPECTION
  if (pbi->inspect_cb != NULL) {
    (*pbi->inspect_cb)(pbi, pbi->inspect_ctx);
  }
#endif

// Non frame parallel update frame context here.
#if CONFIG_EXT_TILE
  if (!cm->large_scale_tile) {
#endif  // CONFIG_EXT_TILE
    // TODO(yunqingwang): If cm->frame_parallel_decode = 0, then the following
    // update always happens. Seems it is done more than necessary.
    if (!cm->frame_parallel_decode ||
        cm->refresh_frame_context != REFRESH_FRAME_CONTEXT_FORWARD) {
#if CONFIG_NO_FRAME_CONTEXT_SIGNALING
      cm->frame_contexts[cm->new_fb_idx] = *cm->fc;
#else
    if (!cm->error_resilient_mode)
      cm->frame_contexts[cm->frame_context_idx] = *cm->fc;
#endif
    }
#if CONFIG_EXT_TILE
  }
#endif  // CONFIG_EXT_TILE
}

#if CONFIG_OBU

static OBU_TYPE read_obu_header(struct aom_read_bit_buffer *rb,
                                size_t *header_size) {
  OBU_TYPE obu_type;
  int obu_extension_flag;

  *header_size = 1;

  // first bit is obu_forbidden_bit (0) according to R19
  aom_rb_read_bit(rb);

  obu_type = (OBU_TYPE)aom_rb_read_literal(rb, 4);
  aom_rb_read_literal(rb, 2);  // reserved
  obu_extension_flag = aom_rb_read_bit(rb);
  if (obu_extension_flag) {
    *header_size += 1;
    aom_rb_read_literal(rb, 3);  // temporal_id
    aom_rb_read_literal(rb, 2);
    aom_rb_read_literal(rb, 2);
    aom_rb_read_literal(rb, 1);  // reserved
  }

  return obu_type;
}

static uint32_t read_temporal_delimiter_obu() { return 0; }

static uint32_t read_sequence_header_obu(AV1Decoder *pbi,
                                         struct aom_read_bit_buffer *rb) {
  AV1_COMMON *const cm = &pbi->common;
  uint32_t saved_bit_offset = rb->bit_offset;

  cm->profile = av1_read_profile(rb);
  aom_rb_read_literal(rb, 4);  // level

  read_sequence_header(&cm->seq_params, rb);

  read_bitdepth_colorspace_sampling(cm, rb, pbi->allow_lowbitdepth);

  return ((rb->bit_offset - saved_bit_offset + 7) >> 3);
}

static uint32_t read_frame_header_obu(AV1Decoder *pbi, const uint8_t *data,
                                      const uint8_t *data_end,
                                      const uint8_t **p_data_end) {
  size_t header_size;

  header_size =
      av1_decode_frame_headers_and_setup(pbi, data, data_end, p_data_end);
  return (uint32_t)(pbi->uncomp_hdr_size + header_size);
}

static uint32_t read_tile_group_header(AV1Decoder *pbi,
                                       struct aom_read_bit_buffer *rb,
                                       int *startTile, int *endTile) {
  AV1_COMMON *const cm = &pbi->common;
  uint32_t saved_bit_offset = rb->bit_offset;

  *startTile = aom_rb_read_literal(rb, cm->log2_tile_rows + cm->log2_tile_cols);
  *endTile = aom_rb_read_literal(rb, cm->log2_tile_rows + cm->log2_tile_cols);

  return ((rb->bit_offset - saved_bit_offset + 7) >> 3);
}

static uint32_t read_one_tile_group_obu(AV1Decoder *pbi,
                                        struct aom_read_bit_buffer *rb,
                                        int is_first_tg, const uint8_t *data,
                                        const uint8_t *data_end,
                                        const uint8_t **p_data_end,
                                        int *is_last_tg) {
  AV1_COMMON *const cm = &pbi->common;
  int startTile, endTile;
  uint32_t header_size, tg_payload_size;

  header_size = read_tile_group_header(pbi, rb, &startTile, &endTile);
  data += header_size;
  av1_decode_tg_tiles_and_wrapup(pbi, data, data_end, p_data_end, startTile,
                                 endTile, is_first_tg);
  tg_payload_size = (uint32_t)(*p_data_end - data);

  // TODO(shan):  For now, assume all tile groups received in order
  *is_last_tg = endTile == cm->tile_rows * cm->tile_cols - 1;

  return header_size + tg_payload_size;
}

static void read_metadata_private_data(const uint8_t *data, size_t sz) {
  for (size_t i = 0; i < sz; i++) {
    mem_get_le16(data);
    data += 2;
  }
}

static void read_metadata_hdr_cll(const uint8_t *data) {
  mem_get_le16(data);
  mem_get_le16(data + 2);
}

static void read_metadata_hdr_mdcv(const uint8_t *data) {
  for (int i = 0; i < 3; i++) {
    mem_get_le16(data);
    data += 2;
    mem_get_le16(data);
    data += 2;
  }

  mem_get_le16(data);
  data += 2;
  mem_get_le16(data);
  data += 2;
  mem_get_le16(data);
  data += 2;
  mem_get_le16(data);
}

static size_t read_metadata(const uint8_t *data, size_t sz) {
  OBU_METADATA_TYPE metadata_type;

  assert(sz >= 2);
  metadata_type = (OBU_METADATA_TYPE)mem_get_le16(data);

  if (metadata_type == OBU_METADATA_TYPE_PRIVATE_DATA) {
    read_metadata_private_data(data + 2, sz - 2);
  } else if (metadata_type == OBU_METADATA_TYPE_HDR_CLL) {
    read_metadata_hdr_cll(data + 2);
  } else if (metadata_type == OBU_METADATA_TYPE_HDR_MDCV) {
    read_metadata_hdr_mdcv(data + 2);
  }

  return sz;
}

void av1_decode_frame_from_obus(struct AV1Decoder *pbi, const uint8_t *data,
                                const uint8_t *data_end,
                                const uint8_t **p_data_end) {
  AV1_COMMON *const cm = &pbi->common;
  int frame_decoding_finished = 0;
  int is_first_tg_obu_received = 1;
  int frame_header_received = 0;
  int frame_header_size = 0;

  // decode frame as a series of OBUs
  while (!frame_decoding_finished && !cm->error.error_code) {
    struct aom_read_bit_buffer rb;
    uint8_t clear_data[80];
    size_t obu_size, obu_header_size, obu_payload_size = 0;
    OBU_TYPE obu_type;

    init_read_bit_buffer(pbi, &rb, data + PRE_OBU_SIZE_BYTES, data_end,
                         clear_data);

// every obu is preceded by PRE_OBU_SIZE_BYTES-byte size of obu (obu header +
// payload size)
// The obu size is only needed for tile group OBUs
#if CONFIG_ADD_4BYTES_OBUSIZE
    obu_size = mem_get_le32(data);
#else
    obu_size = data_end - data;
#endif
    obu_type = read_obu_header(&rb, &obu_header_size);
    data += (PRE_OBU_SIZE_BYTES + obu_header_size);

    switch (obu_type) {
      case OBU_TEMPORAL_DELIMITER:
        obu_payload_size = read_temporal_delimiter_obu();
        break;
      case OBU_SEQUENCE_HEADER:
        obu_payload_size = read_sequence_header_obu(pbi, &rb);
        break;
      case OBU_FRAME_HEADER:
        // Only decode first frame header received
        if (!frame_header_received) {
          frame_header_size = obu_payload_size =
              read_frame_header_obu(pbi, data, data_end, p_data_end);
          frame_header_received = 1;
        } else {
          obu_payload_size = frame_header_size;
        }
        if (cm->show_existing_frame) frame_decoding_finished = 1;
        break;
      case OBU_TILE_GROUP:
        obu_payload_size =
            read_one_tile_group_obu(pbi, &rb, is_first_tg_obu_received, data,
                                    data + obu_size - obu_header_size,
                                    p_data_end, &frame_decoding_finished);
        is_first_tg_obu_received = 0;
        break;
      case OBU_METADATA:
        obu_payload_size = read_metadata(data, obu_size);
        break;
      default: break;
    }
    data += obu_payload_size;
  }
}
#endif
