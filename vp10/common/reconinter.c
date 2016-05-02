/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <assert.h>

#include "./vpx_scale_rtcd.h"
#include "./vpx_config.h"

#include "vpx/vpx_integer.h"

#include "vp10/common/blockd.h"
#include "vp10/common/reconinter.h"
#include "vp10/common/reconintra.h"
#if CONFIG_OBMC
#include "vp10/common/onyxc_int.h"
#endif  // CONFIG_OBMC

#if CONFIG_EXT_INTER
static int get_masked_weight(int m) {
  #define SMOOTHER_LEN  32
  static const uint8_t smoothfn[2 * SMOOTHER_LEN + 1] = {
    0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  1,  1,  1,
    1,  1,  2,  2,  3,  4,  5,  6,
    8,  9, 12, 14, 17, 21, 24, 28,
    32,
    36, 40, 43, 47, 50, 52, 55, 56,
    58, 59, 60, 61, 62, 62, 63, 63,
    63, 63, 63, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64,
  };
  if (m < -SMOOTHER_LEN)
    return 0;
  else if (m > SMOOTHER_LEN)
    return (1 << WEDGE_WEIGHT_BITS);
  else
    return smoothfn[m + SMOOTHER_LEN];
}

#define WEDGE_OBLIQUE  1
#define WEDGE_STRAIGHT 0

#define WEDGE_PARMS    5

// [negative][transpose][reverse]
DECLARE_ALIGNED(16, static uint8_t,
                wedge_mask_obl[2][2][2][MASK_MASTER_SIZE * MASK_MASTER_SIZE]);
// [negative][transpose]
DECLARE_ALIGNED(16, static uint8_t,
                wedge_mask_str[2][2][MASK_MASTER_SIZE * MASK_MASTER_SIZE]);

// Equation of line: f(x, y) = a[0]*(x - a[2]*w/4) + a[1]*(y - a[3]*h/4) = 0
void vp10_init_wedge_masks() {
  int i, j;
  const int w = MASK_MASTER_SIZE;
  const int h = MASK_MASTER_SIZE;
  const int stride = MASK_MASTER_STRIDE;
  const int a[4] = {2, 1, 2, 2};
  for (i = 0; i < h; ++i)
    for (j = 0; j < w; ++j) {
      int x = (2 * j + 1 - (a[2] * w) / 2);
      int y = (2 * i + 1 - (a[3] * h) / 2);
      int m = (a[0] * x + a[1] * y) / 2;
      wedge_mask_obl[1][0][0][i * stride + j] =
      wedge_mask_obl[1][1][0][j * stride + i] =
          get_masked_weight(m);
      wedge_mask_obl[1][0][1][i * stride + w - 1 - j] =
      wedge_mask_obl[1][1][1][(w - 1 - j) * stride + i] =
          (1 << WEDGE_WEIGHT_BITS) - get_masked_weight(m);
      wedge_mask_obl[0][0][0][i * stride + j] =
      wedge_mask_obl[0][1][0][j * stride + i] =
          (1 << WEDGE_WEIGHT_BITS) - get_masked_weight(m);
      wedge_mask_obl[0][0][1][i * stride + w - 1 - j] =
      wedge_mask_obl[0][1][1][(w - 1 - j) * stride + i] =
          get_masked_weight(m);
      wedge_mask_str[1][0][i * stride + j] =
      wedge_mask_str[1][1][j * stride + i] =
          get_masked_weight(x);
      wedge_mask_str[0][0][i * stride + j] =
      wedge_mask_str[0][1][j * stride + i] =
          (1 << WEDGE_WEIGHT_BITS) - get_masked_weight(x);
    }
}

static const int wedge_params_sml[1 << WEDGE_BITS_SML]
                                 [WEDGE_PARMS] = {
    {WEDGE_OBLIQUE,  1, 1, 2, 2},
    {WEDGE_OBLIQUE,  1, 0, 2, 2},
    {WEDGE_OBLIQUE,  0, 1, 2, 2},
    {WEDGE_OBLIQUE,  0, 0, 2, 2},
};

static const int wedge_params_med_hgtw[1 << WEDGE_BITS_MED]
                                      [WEDGE_PARMS] = {
    {WEDGE_OBLIQUE,  1, 1, 2, 2},
    {WEDGE_OBLIQUE,  1, 0, 2, 2},
    {WEDGE_OBLIQUE,  0, 1, 2, 2},
    {WEDGE_OBLIQUE,  0, 0, 2, 2},

    {WEDGE_OBLIQUE,  1, 1, 2, 1},
    {WEDGE_OBLIQUE,  1, 1, 2, 3},
    {WEDGE_OBLIQUE,  1, 0, 2, 1},
    {WEDGE_OBLIQUE,  1, 0, 2, 3},
};

static const int wedge_params_med_hltw[1 << WEDGE_BITS_MED]
                                      [WEDGE_PARMS] = {
    {WEDGE_OBLIQUE,  1, 1, 2, 2},
    {WEDGE_OBLIQUE,  1, 0, 2, 2},
    {WEDGE_OBLIQUE,  0, 1, 2, 2},
    {WEDGE_OBLIQUE,  0, 0, 2, 2},

    {WEDGE_OBLIQUE,  0, 1, 1, 2},
    {WEDGE_OBLIQUE,  0, 1, 3, 2},
    {WEDGE_OBLIQUE,  0, 0, 1, 2},
    {WEDGE_OBLIQUE,  0, 0, 3, 2},
};

static const int wedge_params_med_heqw[1 << WEDGE_BITS_MED]
                                      [WEDGE_PARMS] = {
    {WEDGE_OBLIQUE,  1, 1, 2, 2},
    {WEDGE_OBLIQUE,  1, 0, 2, 2},
    {WEDGE_OBLIQUE,  0, 1, 2, 2},
    {WEDGE_OBLIQUE,  0, 0, 2, 2},

    {WEDGE_STRAIGHT, 1, 0, 2, 1},
    {WEDGE_STRAIGHT, 1, 0, 2, 3},
    {WEDGE_STRAIGHT, 0, 0, 1, 2},
    {WEDGE_STRAIGHT, 0, 0, 3, 2},
};

static const int wedge_params_big_hgtw[1 << WEDGE_BITS_BIG]
                                      [WEDGE_PARMS] = {
    {WEDGE_OBLIQUE,  1, 1, 2, 2},
    {WEDGE_OBLIQUE,  1, 0, 2, 2},
    {WEDGE_OBLIQUE,  0, 1, 2, 2},
    {WEDGE_OBLIQUE,  0, 0, 2, 2},

    {WEDGE_OBLIQUE,  1, 1, 2, 1},
    {WEDGE_OBLIQUE,  1, 1, 2, 3},
    {WEDGE_OBLIQUE,  1, 0, 2, 1},
    {WEDGE_OBLIQUE,  1, 0, 2, 3},

    {WEDGE_OBLIQUE,  0, 1, 1, 2},
    {WEDGE_OBLIQUE,  0, 1, 3, 2},
    {WEDGE_OBLIQUE,  0, 0, 1, 2},
    {WEDGE_OBLIQUE,  0, 0, 3, 2},

    {WEDGE_STRAIGHT, 1, 0, 2, 1},
    {WEDGE_STRAIGHT, 1, 0, 2, 2},
    {WEDGE_STRAIGHT, 1, 0, 2, 3},
    {WEDGE_STRAIGHT, 0, 0, 2, 2},
};

static const int wedge_params_big_hltw[1 << WEDGE_BITS_BIG]
                                      [WEDGE_PARMS] = {
    {WEDGE_OBLIQUE,  1, 1, 2, 2},
    {WEDGE_OBLIQUE,  1, 0, 2, 2},
    {WEDGE_OBLIQUE,  0, 1, 2, 2},
    {WEDGE_OBLIQUE,  0, 0, 2, 2},

    {WEDGE_OBLIQUE,  1, 1, 2, 1},
    {WEDGE_OBLIQUE,  1, 1, 2, 3},
    {WEDGE_OBLIQUE,  1, 0, 2, 1},
    {WEDGE_OBLIQUE,  1, 0, 2, 3},

    {WEDGE_OBLIQUE,  0, 1, 1, 2},
    {WEDGE_OBLIQUE,  0, 1, 3, 2},
    {WEDGE_OBLIQUE,  0, 0, 1, 2},
    {WEDGE_OBLIQUE,  0, 0, 3, 2},

    {WEDGE_STRAIGHT, 0, 0, 1, 2},
    {WEDGE_STRAIGHT, 0, 0, 2, 2},
    {WEDGE_STRAIGHT, 0, 0, 3, 2},
    {WEDGE_STRAIGHT, 1, 0, 2, 2},
};

static const int wedge_params_big_heqw[1 << WEDGE_BITS_BIG]
                                      [WEDGE_PARMS] = {
    {WEDGE_OBLIQUE,  1, 1, 2, 2},
    {WEDGE_OBLIQUE,  1, 0, 2, 2},
    {WEDGE_OBLIQUE,  0, 1, 2, 2},
    {WEDGE_OBLIQUE,  0, 0, 2, 2},

    {WEDGE_OBLIQUE,  1, 1, 2, 1},
    {WEDGE_OBLIQUE,  1, 1, 2, 3},
    {WEDGE_OBLIQUE,  1, 0, 2, 1},
    {WEDGE_OBLIQUE,  1, 0, 2, 3},

    {WEDGE_OBLIQUE,  0, 1, 1, 2},
    {WEDGE_OBLIQUE,  0, 1, 3, 2},
    {WEDGE_OBLIQUE,  0, 0, 1, 2},
    {WEDGE_OBLIQUE,  0, 0, 3, 2},

    {WEDGE_STRAIGHT, 1, 0, 2, 1},
    {WEDGE_STRAIGHT, 1, 0, 2, 3},
    {WEDGE_STRAIGHT, 0, 0, 1, 2},
    {WEDGE_STRAIGHT, 0, 0, 3, 2},
};

static const int *get_wedge_params_lookup[BLOCK_SIZES] = {
  NULL,
  NULL,
  NULL,
  &wedge_params_sml[0][0],
  &wedge_params_med_hgtw[0][0],
  &wedge_params_med_hltw[0][0],
  &wedge_params_med_heqw[0][0],
  &wedge_params_med_hgtw[0][0],
  &wedge_params_med_hltw[0][0],
  &wedge_params_med_heqw[0][0],
  &wedge_params_big_hgtw[0][0],
  &wedge_params_big_hltw[0][0],
  &wedge_params_big_heqw[0][0],
#if CONFIG_EXT_PARTITION
  &wedge_params_big_hgtw[0][0],
  &wedge_params_big_hltw[0][0],
  &wedge_params_big_heqw[0][0],
#endif  // CONFIG_EXT_PARTITION
};

static const int *get_wedge_params(int wedge_index,
                                   BLOCK_SIZE sb_type) {
  const int *a = NULL;
  if (wedge_index != WEDGE_NONE) {
    return get_wedge_params_lookup[sb_type] + WEDGE_PARMS * wedge_index;
  }
  return a;
}

static const uint8_t *get_wedge_mask_inplace(int wedge_index,
                                             int neg,
                                             BLOCK_SIZE sb_type) {
  const uint8_t *master;
  const int bh = 4 << b_height_log2_lookup[sb_type];
  const int bw = 4 << b_width_log2_lookup[sb_type];
  const int *a = get_wedge_params(wedge_index, sb_type);
  int woff, hoff;
  if (!a) return NULL;
  woff = (a[3] * bw) >> 2;
  hoff = (a[4] * bh) >> 2;
  master = (a[0] ?
            wedge_mask_obl[neg][a[1]][a[2]] :
            wedge_mask_str[neg][a[1]]) +
      MASK_MASTER_STRIDE * (MASK_MASTER_SIZE / 2 - hoff) +
      MASK_MASTER_SIZE / 2 - woff;
  return master;
}

const uint8_t *vp10_get_soft_mask(int wedge_index,
                                  int wedge_sign,
                                  BLOCK_SIZE sb_type,
                                  int wedge_offset_x,
                                  int wedge_offset_y) {
  const uint8_t *mask =
      get_wedge_mask_inplace(wedge_index, wedge_sign, sb_type);
  if (mask)
    mask -= (wedge_offset_x + wedge_offset_y * MASK_MASTER_STRIDE);
  return mask;
}

static void build_masked_compound(uint8_t *dst, int dst_stride,
                                  uint8_t *dst1, int dst1_stride,
                                  uint8_t *dst2, int dst2_stride,
                                  const uint8_t *mask,
                                  int h, int w, int subh, int subw) {
  int i, j;
  if (subw == 0 && subh == 0) {
    for (i = 0; i < h; ++i)
      for (j = 0; j < w; ++j) {
        int m = mask[i * MASK_MASTER_STRIDE + j];
        dst[i * dst_stride + j] = (dst1[i * dst1_stride + j] * m +
                                   dst2[i * dst2_stride + j] *
                                   ((1 << WEDGE_WEIGHT_BITS) - m) +
                                   (1 << (WEDGE_WEIGHT_BITS - 1))) >>
            WEDGE_WEIGHT_BITS;
      }
  } else if (subw == 1 && subh == 1) {
    for (i = 0; i < h; ++i)
      for (j = 0; j < w; ++j) {
        int m = (mask[(2 * i) * MASK_MASTER_STRIDE + (2 * j)] +
                 mask[(2 * i + 1) * MASK_MASTER_STRIDE + (2 * j)] +
                 mask[(2 * i) * MASK_MASTER_STRIDE + (2 * j + 1)] +
                 mask[(2 * i + 1) * MASK_MASTER_STRIDE + (2 * j + 1)] + 2) >> 2;
        dst[i * dst_stride + j] = (dst1[i * dst1_stride + j] * m +
                                   dst2[i * dst2_stride + j] *
                                   ((1 << WEDGE_WEIGHT_BITS) - m) +
                                   (1 << (WEDGE_WEIGHT_BITS - 1))) >>
            WEDGE_WEIGHT_BITS;
      }
  } else if (subw == 1 && subh == 0) {
    for (i = 0; i < h; ++i)
      for (j = 0; j < w; ++j) {
        int m = (mask[i * MASK_MASTER_STRIDE + (2 * j)] +
                 mask[i * MASK_MASTER_STRIDE + (2 * j + 1)] + 1) >> 1;
        dst[i * dst_stride + j] = (dst1[i * dst1_stride + j] * m +
                                   dst2[i * dst2_stride + j] *
                                   ((1 << WEDGE_WEIGHT_BITS) - m) +
                                   (1 << (WEDGE_WEIGHT_BITS - 1))) >>
            WEDGE_WEIGHT_BITS;
      }
  } else {
    for (i = 0; i < h; ++i)
      for (j = 0; j < w; ++j) {
        int m = (mask[(2 * i) * MASK_MASTER_STRIDE + j] +
                 mask[(2 * i + 1) * MASK_MASTER_STRIDE + j] + 1) >> 1;
        dst[i * dst_stride + j] = (dst1[i * dst1_stride + j] * m +
                                   dst2[i * dst2_stride + j] *
                                   ((1 << WEDGE_WEIGHT_BITS) - m) +
                                   (1 << (WEDGE_WEIGHT_BITS - 1))) >>
            WEDGE_WEIGHT_BITS;
      }
  }
}

#if CONFIG_VP9_HIGHBITDEPTH
static void build_masked_compound_highbd(uint8_t *dst_8, int dst_stride,
                                         uint8_t *dst1_8, int dst1_stride,
                                         uint8_t *dst2_8, int dst2_stride,
                                         const uint8_t *mask,
                                         int h, int w, int subh, int subw) {
  int i, j;
  uint16_t *dst = CONVERT_TO_SHORTPTR(dst_8);
  uint16_t *dst1 = CONVERT_TO_SHORTPTR(dst1_8);
  uint16_t *dst2 = CONVERT_TO_SHORTPTR(dst2_8);
  if (subw == 0 && subh == 0) {
    for (i = 0; i < h; ++i)
      for (j = 0; j < w; ++j) {
        int m = mask[i * MASK_MASTER_STRIDE + j];
        dst[i * dst_stride + j] = (dst1[i * dst1_stride + j] * m +
                                   dst2[i * dst2_stride + j] *
                                   ((1 << WEDGE_WEIGHT_BITS) - m) +
                                   (1 << (WEDGE_WEIGHT_BITS - 1))) >>
            WEDGE_WEIGHT_BITS;
      }
  } else if (subw == 1 && subh == 1) {
    for (i = 0; i < h; ++i)
      for (j = 0; j < w; ++j) {
        int m = (mask[(2 * i) * MASK_MASTER_STRIDE + (2 * j)] +
                 mask[(2 * i + 1) * MASK_MASTER_STRIDE + (2 * j)] +
                 mask[(2 * i) * MASK_MASTER_STRIDE + (2 * j + 1)] +
                 mask[(2 * i + 1) * MASK_MASTER_STRIDE + (2 * j + 1)] + 2) >> 2;
        dst[i * dst_stride + j] = (dst1[i * dst1_stride + j] * m +
                                   dst2[i * dst2_stride + j] *
                                   ((1 << WEDGE_WEIGHT_BITS) - m) +
                                   (1 << (WEDGE_WEIGHT_BITS - 1))) >>
            WEDGE_WEIGHT_BITS;
      }
  } else if (subw == 1 && subh == 0) {
    for (i = 0; i < h; ++i)
      for (j = 0; j < w; ++j) {
        int m = (mask[i * MASK_MASTER_STRIDE + (2 * j)] +
                 mask[i * MASK_MASTER_STRIDE + (2 * j + 1)] + 1) >> 1;
        dst[i * dst_stride + j] = (dst1[i * dst1_stride + j] * m +
                                   dst2[i * dst2_stride + j] *
                                   ((1 << WEDGE_WEIGHT_BITS) - m) +
                                   (1 << (WEDGE_WEIGHT_BITS - 1))) >>
            WEDGE_WEIGHT_BITS;
      }
  } else {
    for (i = 0; i < h; ++i)
      for (j = 0; j < w; ++j) {
        int m = (mask[(2 * i) * MASK_MASTER_STRIDE + j] +
                 mask[(2 * i + 1) * MASK_MASTER_STRIDE + j] + 1) >> 1;
        dst[i * dst_stride + j] = (dst1[i * dst1_stride + j] * m +
                                   dst2[i * dst2_stride + j] *
                                   ((1 << WEDGE_WEIGHT_BITS) - m) +
                                   (1 << (WEDGE_WEIGHT_BITS - 1))) >>
            WEDGE_WEIGHT_BITS;
      }
  }
}
#endif  // CONFIG_VP9_HIGHBITDEPTH

#if CONFIG_SUPERTX
static void build_masked_compound_wedge_extend(
    uint8_t *dst, int dst_stride,
    uint8_t *dst2, int dst2_stride,
    int wedge_index,
    int wedge_sign,
    BLOCK_SIZE sb_type,
    int wedge_offset_x, int wedge_offset_y,
    int h, int w) {
  const int subh = (2 << b_height_log2_lookup[sb_type]) == h;
  const int subw = (2 << b_width_log2_lookup[sb_type]) == w;
  const uint8_t *mask = vp10_get_soft_mask(
     wedge_index, wedge_sign, sb_type, wedge_offset_x, wedge_offset_y);
  build_masked_compound(dst, dst_stride,
                        dst, dst_stride, dst2, dst2_stride, mask,
                        h, w, subh, subw);
}

#if CONFIG_VP9_HIGHBITDEPTH
static void build_masked_compound_wedge_extend_highbd(
    uint8_t *dst_8, int dst_stride,
    uint8_t *dst2_8, int dst2_stride,
    int wedge_index, int wedge_sign,
    BLOCK_SIZE sb_type,
    int wedge_offset_x, int wedge_offset_y,
    int h, int w) {
  const int subh = (2 << b_height_log2_lookup[sb_type]) == h;
  const int subw = (2 << b_width_log2_lookup[sb_type]) == w;
  const uint8_t *mask = vp10_get_soft_mask(
      wedge_index, wedge_sign, sb_type, wedge_offset_x, wedge_offset_y);
  build_masked_compound_highbd(dst_8, dst_stride,
                               dst_8, dst_stride, dst2_8, dst2_stride, mask,
                               h, w, subh, subw);
}
#endif  // CONFIG_VP9_HIGHBITDEPTH

#else   // CONFIG_SUPERTX

static void build_masked_compound_wedge(uint8_t *dst, int dst_stride,
                                        uint8_t *dst2, int dst2_stride,
                                        int wedge_index, int wedge_sign,
                                        BLOCK_SIZE sb_type,
                                        int h, int w) {
  // Derive subsampling from h and w passed in. May be refactored to
  // pass in subsampling factors directly.
  const int subh = (2 << b_height_log2_lookup[sb_type]) == h;
  const int subw = (2 << b_width_log2_lookup[sb_type]) == w;
  const uint8_t *mask = vp10_get_soft_mask(wedge_index, wedge_sign,
                                           sb_type, 0, 0);
  build_masked_compound(dst, dst_stride,
                        dst, dst_stride, dst2, dst2_stride, mask,
                        h, w, subh, subw);
}

#if CONFIG_VP9_HIGHBITDEPTH
static void build_masked_compound_wedge_highbd(uint8_t *dst_8, int dst_stride,
                                               uint8_t *dst2_8, int dst2_stride,
                                               int wedge_index, int wedge_sign,
                                               BLOCK_SIZE sb_type,
                                               int h, int w) {
  // Derive subsampling from h and w passed in. May be refactored to
  // pass in subsampling factors directly.
  const int subh = (2 << b_height_log2_lookup[sb_type]) == h;
  const int subw = (2 << b_width_log2_lookup[sb_type]) == w;
  const uint8_t *mask = vp10_get_soft_mask(wedge_index, wedge_sign,
                                           sb_type, 0, 0);
  build_masked_compound_highbd(dst_8, dst_stride,
                               dst_8, dst_stride, dst2_8, dst2_stride, mask,
                               h, w, subh, subw);
}
#endif  // CONFIG_VP9_HIGHBITDEPTH
#endif  // CONFIG_SUPERTX

void vp10_make_masked_inter_predictor(
    const uint8_t *pre,
    int pre_stride,
    uint8_t *dst,
    int dst_stride,
    const int subpel_x,
    const int subpel_y,
    const struct scale_factors *sf,
    int w, int h,
    const INTERP_FILTER interp_filter,
    int xs, int ys,
#if CONFIG_SUPERTX
    int wedge_offset_x, int wedge_offset_y,
#endif  // CONFIG_SUPERTX
    const MACROBLOCKD *xd) {
  const MODE_INFO *mi = xd->mi[0];
#if CONFIG_VP9_HIGHBITDEPTH
  DECLARE_ALIGNED(16, uint8_t, tmp_dst_[2 * MAX_SB_SQUARE]);
  uint8_t *tmp_dst =
      (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) ?
      CONVERT_TO_BYTEPTR(tmp_dst_) : tmp_dst_;
  vp10_make_inter_predictor(pre, pre_stride, tmp_dst, MAX_SB_SIZE,
                            subpel_x, subpel_y, sf, w, h, 0,
                            interp_filter, xs, ys, xd);
#if CONFIG_SUPERTX
  if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH)
    build_masked_compound_wedge_extend_highbd(
        dst, dst_stride, tmp_dst, MAX_SB_SIZE,
        mi->mbmi.interinter_wedge_index,
        mi->mbmi.interinter_wedge_sign,
        mi->mbmi.sb_type, plane,
        wedge_offset_x, wedge_offset_y, h, w);
  else
    build_masked_compound_wedge_extend(
        dst, dst_stride, tmp_dst, MAX_SB_SIZE,
        mi->mbmi.interinter_wedge_index,
        mi->mbmi.interinter_wedge_sign,
        mi->mbmi.sb_type, plane,
        wedge_offset_x, wedge_offset_y, h, w);
#else
  if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH)
    build_masked_compound_wedge_highbd(
        dst, dst_stride, tmp_dst, MAX_SB_SIZE,
        mi->mbmi.interinter_wedge_index,
        mi->mbmi.interinter_wedge_sign,
        mi->mbmi.sb_type, h, w);
  else
    build_masked_compound_wedge(
        dst, dst_stride, tmp_dst, MAX_SB_SIZE,
        mi->mbmi.interinter_wedge_index,
        mi->mbmi.interinter_wedge_sign,
        mi->mbmi.sb_type, h, w);
#endif  // CONFIG_SUPERTX
#else   // CONFIG_VP9_HIGHBITDEPTH
  DECLARE_ALIGNED(16, uint8_t, tmp_dst[MAX_SB_SQUARE]);
  vp10_make_inter_predictor(pre, pre_stride, tmp_dst, MAX_SB_SIZE,
                            subpel_x, subpel_y, sf, w, h, 0,
                            interp_filter, xs, ys, xd);
#if CONFIG_SUPERTX
  build_masked_compound_wedge_extend(
      dst, dst_stride, tmp_dst, MAX_SB_SIZE,
      mi->mbmi.interinter_wedge_index,
      mi->mbmi.interinter_wedge_sign,
      mi->mbmi.sb_type,
      wedge_offset_x, wedge_offset_y, h, w);
#else
  build_masked_compound_wedge(
      dst, dst_stride, tmp_dst, MAX_SB_SIZE,
      mi->mbmi.interinter_wedge_index,
      mi->mbmi.interinter_wedge_sign,
      mi->mbmi.sb_type, h, w);
#endif  // CONFIG_SUPERTX
#endif  // CONFIG_VP9_HIGHBITDEPTH
}
#endif  // CONFIG_EXT_INTER

#if CONFIG_VP9_HIGHBITDEPTH
void vp10_highbd_build_inter_predictor(const uint8_t *src, int src_stride,
                                      uint8_t *dst, int dst_stride,
                                      const MV *src_mv,
                                      const struct scale_factors *sf,
                                      int w, int h, int ref,
                                      const INTERP_FILTER interp_filter,
                                      enum mv_precision precision,
                                      int x, int y, int bd) {
  const int is_q4 = precision == MV_PRECISION_Q4;
  const MV mv_q4 = { is_q4 ? src_mv->row : src_mv->row * 2,
                     is_q4 ? src_mv->col : src_mv->col * 2 };
  MV32 mv = vp10_scale_mv(&mv_q4, x, y, sf);
  const int subpel_x = mv.col & SUBPEL_MASK;
  const int subpel_y = mv.row & SUBPEL_MASK;

  src += (mv.row >> SUBPEL_BITS) * src_stride + (mv.col >> SUBPEL_BITS);

  highbd_inter_predictor(src, src_stride, dst, dst_stride, subpel_x, subpel_y,
                       sf, w, h, ref, interp_filter, sf->x_step_q4,
                       sf->y_step_q4, bd);
}
#endif  // CONFIG_VP9_HIGHBITDEPTH

void vp10_build_inter_predictor(const uint8_t *src, int src_stride,
                               uint8_t *dst, int dst_stride,
                               const MV *src_mv,
                               const struct scale_factors *sf,
                               int w, int h, int ref,
                               const INTERP_FILTER interp_filter,
                               enum mv_precision precision,
                               int x, int y) {
  const int is_q4 = precision == MV_PRECISION_Q4;
  const MV mv_q4 = { is_q4 ? src_mv->row : src_mv->row * 2,
                     is_q4 ? src_mv->col : src_mv->col * 2 };
  MV32 mv = vp10_scale_mv(&mv_q4, x, y, sf);
  const int subpel_x = mv.col & SUBPEL_MASK;
  const int subpel_y = mv.row & SUBPEL_MASK;

  src += (mv.row >> SUBPEL_BITS) * src_stride + (mv.col >> SUBPEL_BITS);

  inter_predictor(src, src_stride, dst, dst_stride, subpel_x, subpel_y,
                  sf, w, h, ref, interp_filter, sf->x_step_q4, sf->y_step_q4);
}

void build_inter_predictors(MACROBLOCKD *xd, int plane,
#if CONFIG_OBMC
                            int mi_col_offset, int mi_row_offset,
#endif  // CONFIG_OBMC
                            int block,
                            int bw, int bh,
                            int x, int y, int w, int h,
#if CONFIG_SUPERTX && CONFIG_EXT_INTER
                            int wedge_offset_x, int wedge_offset_y,
#endif  // CONFIG_SUPERTX && CONFIG_EXT_INTER
                            int mi_x, int mi_y) {
  struct macroblockd_plane *const pd = &xd->plane[plane];
#if CONFIG_OBMC
  const MODE_INFO *mi = xd->mi[mi_col_offset + xd->mi_stride * mi_row_offset];
#else
  const MODE_INFO *mi = xd->mi[0];
#endif  // CONFIG_OBMC
  const int is_compound = has_second_ref(&mi->mbmi);
  const INTERP_FILTER interp_filter = mi->mbmi.interp_filter;
  int ref;

  for (ref = 0; ref < 1 + is_compound; ++ref) {
    const struct scale_factors *const sf = &xd->block_refs[ref]->sf;
    struct buf_2d *const pre_buf = &pd->pre[ref];
    struct buf_2d *const dst_buf = &pd->dst;
    uint8_t *const dst = dst_buf->buf + dst_buf->stride * y + x;
    const MV mv = mi->mbmi.sb_type < BLOCK_8X8
               ? average_split_mvs(pd, mi, ref, block)
               : mi->mbmi.mv[ref].as_mv;

    // TODO(jkoleszar): This clamping is done in the incorrect place for the
    // scaling case. It needs to be done on the scaled MV, not the pre-scaling
    // MV. Note however that it performs the subsampling aware scaling so
    // that the result is always q4.
    // mv_precision precision is MV_PRECISION_Q4.
    const MV mv_q4 = clamp_mv_to_umv_border_sb(xd, &mv, bw, bh,
                                               pd->subsampling_x,
                                               pd->subsampling_y);

    uint8_t *pre;
    MV32 scaled_mv;
    int xs, ys, subpel_x, subpel_y;
    const int is_scaled = vp10_is_scaled(sf);

    if (is_scaled) {
      pre = pre_buf->buf + scaled_buffer_offset(x, y, pre_buf->stride, sf);
      scaled_mv = vp10_scale_mv(&mv_q4, mi_x + x, mi_y + y, sf);
      xs = sf->x_step_q4;
      ys = sf->y_step_q4;
    } else {
      pre = pre_buf->buf + (y * pre_buf->stride + x);
      scaled_mv.row = mv_q4.row;
      scaled_mv.col = mv_q4.col;
      xs = ys = 16;
    }

    subpel_x = scaled_mv.col & SUBPEL_MASK;
    subpel_y = scaled_mv.row & SUBPEL_MASK;
    pre += (scaled_mv.row >> SUBPEL_BITS) * pre_buf->stride
           + (scaled_mv.col >> SUBPEL_BITS);

#if CONFIG_EXT_INTER
    if (ref && is_interinter_wedge_used(mi->mbmi.sb_type) &&
        mi->mbmi.use_wedge_interinter)
      vp10_make_masked_inter_predictor(
          pre, pre_buf->stride, dst, dst_buf->stride,
          subpel_x, subpel_y, sf, w, h,
          interp_filter, xs, ys,
#if CONFIG_SUPERTX
          wedge_offset_x, wedge_offset_y,
#endif  // CONFIG_SUPERTX
          xd);
    else
#endif  // CONFIG_EXT_INTER
      vp10_make_inter_predictor(pre, pre_buf->stride, dst, dst_buf->stride,
                                subpel_x, subpel_y, sf, w, h, ref,
                                interp_filter, xs, ys, xd);
  }
}

void vp10_build_inter_predictor_sub8x8(MACROBLOCKD *xd, int plane,
                                       int i, int ir, int ic,
                                       int mi_row, int mi_col) {
  struct macroblockd_plane *const pd = &xd->plane[plane];
  MODE_INFO *const mi = xd->mi[0];
  const BLOCK_SIZE plane_bsize = get_plane_block_size(mi->mbmi.sb_type, pd);
  const int width = 4 * num_4x4_blocks_wide_lookup[plane_bsize];
  const int height = 4 * num_4x4_blocks_high_lookup[plane_bsize];

  uint8_t *const dst = &pd->dst.buf[(ir * pd->dst.stride + ic) << 2];
  int ref;
  const int is_compound = has_second_ref(&mi->mbmi);
  const INTERP_FILTER interp_filter = mi->mbmi.interp_filter;

  for (ref = 0; ref < 1 + is_compound; ++ref) {
    const uint8_t *pre =
        &pd->pre[ref].buf[(ir * pd->pre[ref].stride + ic) << 2];
#if CONFIG_VP9_HIGHBITDEPTH
  if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
    vp10_highbd_build_inter_predictor(pre, pd->pre[ref].stride,
                                      dst, pd->dst.stride,
                                      &mi->bmi[i].as_mv[ref].as_mv,
                                      &xd->block_refs[ref]->sf, width, height,
                                      ref, interp_filter, MV_PRECISION_Q3,
                                      mi_col * MI_SIZE + 4 * ic,
                                      mi_row * MI_SIZE + 4 * ir, xd->bd);
  } else {
    vp10_build_inter_predictor(pre, pd->pre[ref].stride,
                               dst, pd->dst.stride,
                               &mi->bmi[i].as_mv[ref].as_mv,
                               &xd->block_refs[ref]->sf, width, height, ref,
                               interp_filter, MV_PRECISION_Q3,
                               mi_col * MI_SIZE + 4 * ic,
                               mi_row * MI_SIZE + 4 * ir);
  }
#else
    vp10_build_inter_predictor(pre, pd->pre[ref].stride,
                               dst, pd->dst.stride,
                               &mi->bmi[i].as_mv[ref].as_mv,
                               &xd->block_refs[ref]->sf, width, height, ref,
                               interp_filter, MV_PRECISION_Q3,
                               mi_col * MI_SIZE + 4 * ic,
                               mi_row * MI_SIZE + 4 * ir);
#endif  // CONFIG_VP9_HIGHBITDEPTH
  }
}

static void build_inter_predictors_for_planes(MACROBLOCKD *xd, BLOCK_SIZE bsize,
                                              int mi_row, int mi_col,
                                              int plane_from, int plane_to) {
  int plane;
  const int mi_x = mi_col * MI_SIZE;
  const int mi_y = mi_row * MI_SIZE;
  for (plane = plane_from; plane <= plane_to; ++plane) {
    const struct macroblockd_plane *pd = &xd->plane[plane];
    const int bw = 4 * num_4x4_blocks_wide_lookup[bsize] >> pd->subsampling_x;
    const int bh = 4 * num_4x4_blocks_high_lookup[bsize] >> pd->subsampling_y;

    if (xd->mi[0]->mbmi.sb_type < BLOCK_8X8) {
      const PARTITION_TYPE bp = bsize - xd->mi[0]->mbmi.sb_type;
      const int have_vsplit = bp != PARTITION_HORZ;
      const int have_hsplit = bp != PARTITION_VERT;
      const int num_4x4_w = 2 >> ((!have_vsplit) | pd->subsampling_x);
      const int num_4x4_h = 2 >> ((!have_hsplit) | pd->subsampling_y);
      const int pw = 8 >> (have_vsplit | pd->subsampling_x);
      const int ph = 8 >> (have_hsplit | pd->subsampling_y);
      int x, y;
      assert(bp != PARTITION_NONE && bp < PARTITION_TYPES);
      assert(bsize == BLOCK_8X8);
      assert(pw * num_4x4_w == bw && ph * num_4x4_h == bh);
      for (y = 0; y < num_4x4_h; ++y)
        for (x = 0; x < num_4x4_w; ++x)
           build_inter_predictors(xd, plane,
#if CONFIG_OBMC
                                  0, 0,
#endif  // CONFIG_OBMC
                                  y * 2 + x, bw, bh,
                                  4 * x, 4 * y, pw, ph,
#if CONFIG_SUPERTX && CONFIG_EXT_INTER
                                  0, 0,
#endif  // CONFIG_SUPERTX && CONFIG_EXT_INTER
                                  mi_x, mi_y);
    } else {
      build_inter_predictors(xd, plane,
#if CONFIG_OBMC
                             0, 0,
#endif  // CONFIG_OBMC
                             0, bw, bh,
                             0, 0, bw, bh,
#if CONFIG_SUPERTX && CONFIG_EXT_INTER
                             0, 0,
#endif  // CONFIG_SUPERTX && CONFIG_EXT_INTER
                             mi_x, mi_y);
    }
  }
}

void vp10_build_inter_predictors_sby(MACROBLOCKD *xd, int mi_row, int mi_col,
                                    BLOCK_SIZE bsize) {
  build_inter_predictors_for_planes(xd, bsize, mi_row, mi_col, 0, 0);
#if CONFIG_EXT_INTER
  if (is_interintra_pred(&xd->mi[0]->mbmi))
    vp10_build_interintra_predictors_sby(xd,
                                         xd->plane[0].dst.buf,
                                         xd->plane[0].dst.stride,
                                         bsize);
#endif  // CONFIG_EXT_INTER
}

void vp10_build_inter_predictors_sbp(MACROBLOCKD *xd, int mi_row, int mi_col,
                                     BLOCK_SIZE bsize, int plane) {
  build_inter_predictors_for_planes(xd, bsize, mi_row, mi_col, plane, plane);
#if CONFIG_EXT_INTER
  if (is_interintra_pred(&xd->mi[0]->mbmi)) {
    if (plane == 0) {
      vp10_build_interintra_predictors_sby(xd,
                                           xd->plane[0].dst.buf,
                                           xd->plane[0].dst.stride,
                                           bsize);
    } else {
      vp10_build_interintra_predictors_sbc(xd,
                                           xd->plane[plane].dst.buf,
                                           xd->plane[plane].dst.stride,
                                           plane, bsize);
    }
  }
#endif  // CONFIG_EXT_INTER
}

void vp10_build_inter_predictors_sbuv(MACROBLOCKD *xd, int mi_row, int mi_col,
                                      BLOCK_SIZE bsize) {
  build_inter_predictors_for_planes(xd, bsize, mi_row, mi_col, 1,
                                    MAX_MB_PLANE - 1);
#if CONFIG_EXT_INTER
  if (is_interintra_pred(&xd->mi[0]->mbmi))
    vp10_build_interintra_predictors_sbuv(xd,
                                          xd->plane[1].dst.buf,
                                          xd->plane[2].dst.buf,
                                          xd->plane[1].dst.stride,
                                          xd->plane[2].dst.stride,
                                          bsize);
#endif  // CONFIG_EXT_INTER
}

void vp10_build_inter_predictors_sb(MACROBLOCKD *xd, int mi_row, int mi_col,
                                   BLOCK_SIZE bsize) {
  build_inter_predictors_for_planes(xd, bsize, mi_row, mi_col, 0,
                                    MAX_MB_PLANE - 1);
#if CONFIG_EXT_INTER
  if (is_interintra_pred(&xd->mi[0]->mbmi))
    vp10_build_interintra_predictors(xd,
                                     xd->plane[0].dst.buf,
                                     xd->plane[1].dst.buf,
                                     xd->plane[2].dst.buf,
                                     xd->plane[0].dst.stride,
                                     xd->plane[1].dst.stride,
                                     xd->plane[2].dst.stride,
                                     bsize);
#endif  // CONFIG_EXT_INTER
}

void vp10_setup_dst_planes(struct macroblockd_plane planes[MAX_MB_PLANE],
                          const YV12_BUFFER_CONFIG *src,
                          int mi_row, int mi_col) {
  uint8_t *const buffers[MAX_MB_PLANE] = { src->y_buffer, src->u_buffer,
      src->v_buffer};
  const int strides[MAX_MB_PLANE] = { src->y_stride, src->uv_stride,
      src->uv_stride};
  int i;

  for (i = 0; i < MAX_MB_PLANE; ++i) {
    struct macroblockd_plane *const pd = &planes[i];
    setup_pred_plane(&pd->dst, buffers[i], strides[i], mi_row, mi_col, NULL,
                     pd->subsampling_x, pd->subsampling_y);
  }
}

void vp10_setup_pre_planes(MACROBLOCKD *xd, int idx,
                          const YV12_BUFFER_CONFIG *src,
                          int mi_row, int mi_col,
                          const struct scale_factors *sf) {
  if (src != NULL) {
    int i;
    uint8_t *const buffers[MAX_MB_PLANE] = { src->y_buffer, src->u_buffer,
        src->v_buffer};
    const int strides[MAX_MB_PLANE] = { src->y_stride, src->uv_stride,
        src->uv_stride};
    for (i = 0; i < MAX_MB_PLANE; ++i) {
      struct macroblockd_plane *const pd = &xd->plane[i];
      setup_pred_plane(&pd->pre[idx], buffers[i], strides[i], mi_row, mi_col,
                       sf, pd->subsampling_x, pd->subsampling_y);
    }
  }
}

#if CONFIG_SUPERTX
static const uint8_t mask_8[8] = {
  64, 64, 62, 52, 12,  2,  0,  0
};

static const uint8_t mask_16[16] = {
  63, 62, 60, 58, 55, 50, 43, 36, 28, 21, 14, 9, 6, 4, 2, 1
};

static const uint8_t mask_32[32] = {
  64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 63, 61, 57, 52, 45, 36,
  28, 19, 12,  7,  3,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0
};

static const uint8_t mask_8_uv[8] = {
  64, 64, 62, 52,  12,  2,  0,  0
};

static const uint8_t mask_16_uv[16] = {
  64, 64, 64, 64, 61, 53, 45, 36, 28, 19, 11, 3, 0,  0,  0,  0
};

static const uint8_t mask_32_uv[32] = {
  64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 60, 54, 46, 36,
  28, 18, 10,  4,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0
};

static void generate_1dmask(int length, uint8_t *mask, int plane) {
  switch (length) {
    case 8:
      memcpy(mask, plane ? mask_8_uv : mask_8, length);
      break;
    case 16:
      memcpy(mask, plane ? mask_16_uv : mask_16, length);
      break;
    case 32:
      memcpy(mask, plane ? mask_32_uv : mask_32, length);
      break;
    default:
      assert(0);
  }
}

void vp10_build_masked_inter_predictor_complex(
    MACROBLOCKD *xd,
    uint8_t *dst, int dst_stride, uint8_t *dst2, int dst2_stride,
    int mi_row, int mi_col,
    int mi_row_ori, int mi_col_ori, BLOCK_SIZE bsize, BLOCK_SIZE top_bsize,
    PARTITION_TYPE partition, int plane) {
  int i, j;
  const struct macroblockd_plane *pd = &xd->plane[plane];
  uint8_t mask[MAX_TX_SIZE];
  int top_w = 4 << b_width_log2_lookup[top_bsize];
  int top_h = 4 << b_height_log2_lookup[top_bsize];
  int w = 4 << b_width_log2_lookup[bsize];
  int h = 4 << b_height_log2_lookup[bsize];
  int w_offset = (mi_col - mi_col_ori) * MI_SIZE;
  int h_offset = (mi_row - mi_row_ori) * MI_SIZE;

#if CONFIG_VP9_HIGHBITDEPTH
  uint16_t *dst16= CONVERT_TO_SHORTPTR(dst);
  uint16_t *dst216 = CONVERT_TO_SHORTPTR(dst2);
  int b_hdb = (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) ? 1 : 0;
#endif  // CONFIG_VP9_HIGHBITDEPTH

  assert(bsize <= BLOCK_32X32);

  top_w >>= pd->subsampling_x;
  top_h >>= pd->subsampling_y;
  w >>= pd->subsampling_x;
  h >>= pd->subsampling_y;
  w_offset >>= pd->subsampling_x;
  h_offset >>= pd->subsampling_y;

  switch (partition) {
    case PARTITION_HORZ:
    {
#if CONFIG_VP9_HIGHBITDEPTH
      if (b_hdb) {
        uint16_t *dst_tmp = dst16 + h_offset * dst_stride;
        uint16_t *dst2_tmp = dst216 + h_offset * dst2_stride;
        generate_1dmask(h, mask + h_offset,
                        plane && xd->plane[plane].subsampling_y);

        for (i = h_offset; i < h_offset + h; i++) {
          for (j = 0; j < top_w; j++) {
            const int m = mask[i];  assert(m >= 0 && m <= 64);
            if (m == 64)
              continue;

            if (m == 0)
              dst_tmp[j] = dst2_tmp[j];
            else
              dst_tmp[j] = ROUND_POWER_OF_TWO(dst_tmp[j] * m +
                                              dst2_tmp[j] * (64 - m), 6);
          }
          dst_tmp += dst_stride;
          dst2_tmp += dst2_stride;
        }

        for (; i < top_h; i ++) {
          memcpy(dst_tmp, dst2_tmp, top_w * sizeof(uint16_t));
          dst_tmp += dst_stride;
          dst2_tmp += dst2_stride;
        }
      } else {
#endif  // CONFIG_VP9_HIGHBITDEPTH
        uint8_t *dst_tmp = dst + h_offset * dst_stride;
        uint8_t *dst2_tmp = dst2 + h_offset * dst2_stride;
        generate_1dmask(h, mask + h_offset,
                        plane && xd->plane[plane].subsampling_y);

        for (i = h_offset; i < h_offset + h; i++) {
          for (j = 0; j < top_w; j++) {
            const int m = mask[i];  assert(m >= 0 && m <= 64);
            if (m == 64)
              continue;

            if (m == 0)
              dst_tmp[j] = dst2_tmp[j];
            else
              dst_tmp[j] = ROUND_POWER_OF_TWO(dst_tmp[j] * m +
                                              dst2_tmp[j] * (64 - m), 6);
          }
          dst_tmp += dst_stride;
          dst2_tmp += dst2_stride;
        }

        for (; i < top_h; i ++) {
          memcpy(dst_tmp, dst2_tmp, top_w * sizeof(uint8_t));
          dst_tmp += dst_stride;
          dst2_tmp += dst2_stride;
        }
#if CONFIG_VP9_HIGHBITDEPTH
      }
#endif  // CONFIG_VP9_HIGHBITDEPTH
    }

      break;
    case PARTITION_VERT:
    {
#if CONFIG_VP9_HIGHBITDEPTH
      if (b_hdb) {
        uint16_t *dst_tmp = dst16;
        uint16_t *dst2_tmp = dst216;
        generate_1dmask(w, mask + w_offset,
                        plane && xd->plane[plane].subsampling_x);

        for (i = 0; i < top_h; i++) {
          for (j = w_offset; j < w_offset + w; j++) {
            const int m = mask[j];   assert(m >= 0 && m <= 64);
            if (m == 64)
              continue;

            if (m == 0)
              dst_tmp[j] = dst2_tmp[j];
            else
              dst_tmp[j] = ROUND_POWER_OF_TWO(dst_tmp[j] * m +
                                              dst2_tmp[j] * (64 - m), 6);
          }
          memcpy(dst_tmp + j, dst2_tmp + j,
                     (top_w - w_offset - w) * sizeof(uint16_t));
          dst_tmp += dst_stride;
          dst2_tmp += dst2_stride;
        }
      } else {
#endif  // CONFIG_VP9_HIGHBITDEPTH
        uint8_t *dst_tmp = dst;
        uint8_t *dst2_tmp = dst2;
        generate_1dmask(w, mask + w_offset,
                        plane && xd->plane[plane].subsampling_x);

        for (i = 0; i < top_h; i++) {
          for (j = w_offset; j < w_offset + w; j++) {
            const int m = mask[j];   assert(m >= 0 && m <= 64);
            if (m == 64)
              continue;

            if (m == 0)
              dst_tmp[j] = dst2_tmp[j];
            else
              dst_tmp[j] = ROUND_POWER_OF_TWO(dst_tmp[j] * m +
                                              dst2_tmp[j] * (64 - m), 6);
          }
            memcpy(dst_tmp + j, dst2_tmp + j,
                       (top_w - w_offset - w) * sizeof(uint8_t));
          dst_tmp += dst_stride;
          dst2_tmp += dst2_stride;
        }
#if CONFIG_VP9_HIGHBITDEPTH
      }
#endif  // CONFIG_VP9_HIGHBITDEPTH
    }
      break;
    default:
      assert(0);
  }
  (void) xd;
}

void vp10_build_inter_predictors_sb_sub8x8_extend(
    MACROBLOCKD *xd,
#if CONFIG_EXT_INTER
    int mi_row_ori, int mi_col_ori,
#endif  // CONFIG_EXT_INTER
    int mi_row, int mi_col,
    BLOCK_SIZE bsize, int block) {
  // Prediction function used in supertx:
  // Use the mv at current block (which is less than 8x8)
  // to get prediction of a block located at (mi_row, mi_col) at size of bsize
  // bsize can be larger than 8x8.
  // block (0-3): the sub8x8 location of current block
  int plane;
  const int mi_x = mi_col * MI_SIZE;
  const int mi_y = mi_row * MI_SIZE;
#if CONFIG_EXT_INTER
  const int wedge_offset_x = (mi_col_ori - mi_col) * MI_SIZE;
  const int wedge_offset_y = (mi_row_ori - mi_row) * MI_SIZE;
#endif  // CONFIG_EXT_INTER

  // For sub8x8 uv:
  // Skip uv prediction in supertx except the first block (block = 0)
  int max_plane = block ? 1 : MAX_MB_PLANE;

  for (plane = 0; plane < max_plane; plane++) {
    const BLOCK_SIZE plane_bsize = get_plane_block_size(bsize,
                                                        &xd->plane[plane]);
    const int num_4x4_w = num_4x4_blocks_wide_lookup[plane_bsize];
    const int num_4x4_h = num_4x4_blocks_high_lookup[plane_bsize];
    const int bw = 4 * num_4x4_w;
    const int bh = 4 * num_4x4_h;

    build_inter_predictors(xd, plane,
#if CONFIG_OBMC
                           0, 0,
#endif  // CONFIG_OBMC
                           block, bw, bh,
                           0, 0, bw, bh,
#if CONFIG_EXT_INTER
                           wedge_offset_x,
                           wedge_offset_y,
#endif  // CONFIG_SUPERTX
                           mi_x, mi_y);
  }
#if CONFIG_EXT_INTER
  if (is_interintra_pred(&xd->mi[0]->mbmi))
    vp10_build_interintra_predictors(xd,
                                     xd->plane[0].dst.buf,
                                     xd->plane[1].dst.buf,
                                     xd->plane[2].dst.buf,
                                     xd->plane[0].dst.stride,
                                     xd->plane[1].dst.stride,
                                     xd->plane[2].dst.stride,
                                     bsize);
#endif  // CONFIG_EXT_INTER
}

void vp10_build_inter_predictors_sb_extend(MACROBLOCKD *xd,
#if CONFIG_EXT_INTER
                                           int mi_row_ori, int mi_col_ori,
#endif  // CONFIG_EXT_INTER
                                           int mi_row, int mi_col,
                                           BLOCK_SIZE bsize) {
  int plane;
  const int mi_x = mi_col * MI_SIZE;
  const int mi_y = mi_row * MI_SIZE;
#if CONFIG_EXT_INTER
  const int wedge_offset_x = (mi_col_ori - mi_col) * MI_SIZE;
  const int wedge_offset_y = (mi_row_ori - mi_row) * MI_SIZE;
#endif  // CONFIG_EXT_INTER
  for (plane = 0; plane < MAX_MB_PLANE; ++plane) {
    const BLOCK_SIZE plane_bsize = get_plane_block_size(
        bsize, &xd->plane[plane]);
    const int num_4x4_w = num_4x4_blocks_wide_lookup[plane_bsize];
    const int num_4x4_h = num_4x4_blocks_high_lookup[plane_bsize];
    const int bw = 4 * num_4x4_w;
    const int bh = 4 * num_4x4_h;

    if (xd->mi[0]->mbmi.sb_type < BLOCK_8X8) {
      int x, y;
      assert(bsize == BLOCK_8X8);
      for (y = 0; y < num_4x4_h; ++y)
        for (x = 0; x < num_4x4_w; ++x)
           build_inter_predictors(
               xd, plane,
#if CONFIG_OBMC
               0, 0,
#endif  // CONFIG_OBMC
               y * 2 + x, bw, bh, 4 * x, 4 * y, 4, 4,
#if CONFIG_EXT_INTER
               wedge_offset_x,
               wedge_offset_y,
#endif  // CONFIG_EXT_INTER
               mi_x, mi_y);
    } else {
      build_inter_predictors(
          xd, plane,
#if CONFIG_OBMC
          0, 0,
#endif  // CONFIG_OBMC
          0, bw, bh, 0, 0, bw, bh,
#if CONFIG_EXT_INTER
          wedge_offset_x,
          wedge_offset_y,
#endif  // CONFIG_EXT_INTER
          mi_x, mi_y);
    }
  }
}
#endif  // CONFIG_SUPERTX

#if CONFIG_OBMC
// obmc_mask_N[is_neighbor_predictor][overlap_position]
static const uint8_t obmc_mask_1[2][1] = {
    { 55},
    {  9}
};

static const uint8_t obmc_mask_2[2][2] = {
    { 45, 62},
    { 19,  2}
};

static const uint8_t obmc_mask_4[2][4] = {
    { 39, 50, 59, 64},
    { 25, 14,  5,  0}
};

static const uint8_t obmc_mask_8[2][8] = {
    { 36, 42, 48, 53, 57, 61, 63, 64},
    { 28, 22, 16, 11,  7,  3,  1,  0}
};

static const uint8_t obmc_mask_16[2][16] = {
    { 34, 37, 40, 43, 46, 49, 52, 54, 56, 58, 60, 61, 63, 64, 64, 64},
    { 30, 27, 24, 21, 18, 15, 12, 10,  8,  6,  4,  3,  1,  0,  0,  0}
};

static const uint8_t obmc_mask_32[2][32] = {
    { 33, 35, 36, 38, 40, 41, 43, 44,
      45, 47, 48, 50, 51, 52, 53, 55,
      56, 57, 58, 59, 60, 60, 61, 62,
      62, 63, 63, 64, 64, 64, 64, 64 },
    { 31, 29, 28, 26, 24, 23, 21, 20,
      19, 17, 16, 14, 13, 12, 11,  9,
       8,  7,  6,  5,  4,  4,  3,  2,
       2,  1,  1,  0,  0,  0,  0,  0 }
};

#if CONFIG_EXT_PARTITION
static const uint8_t obmc_mask_64[2][64] = {
    {
      33, 34, 35, 35, 36, 37, 38, 39, 40, 40, 41, 42, 43, 44, 44, 44,
      45, 46, 47, 47, 48, 49, 50, 51, 51, 51, 52, 52, 53, 54, 55, 56,
      56, 56, 57, 57, 58, 58, 59, 60, 60, 60, 60, 60, 61, 62, 62, 62,
      62, 62, 63, 63, 63, 63, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    }, {
      31, 30, 29, 29, 28, 27, 26, 25, 24, 24, 23, 22, 21, 20, 20, 20,
      19, 18, 17, 17, 16, 15, 14, 13, 13, 13, 12, 12, 11, 10,  9,  8,
      8,  8,  7,  7,  6,  6,  5, 4,  4,  4,  4,  4,  3,  2,  2,  2,
      2,  2,  1,  1, 1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    }
};
#endif  // CONFIG_EXT_PARTITION


void setup_obmc_mask(int length, const uint8_t *mask[2]) {
  switch (length) {
    case 1:
      mask[0] = obmc_mask_1[0];
      mask[1] = obmc_mask_1[1];
      break;
    case 2:
      mask[0] = obmc_mask_2[0];
      mask[1] = obmc_mask_2[1];
      break;
    case 4:
      mask[0] = obmc_mask_4[0];
      mask[1] = obmc_mask_4[1];
      break;
    case 8:
      mask[0] = obmc_mask_8[0];
      mask[1] = obmc_mask_8[1];
      break;
    case 16:
      mask[0] = obmc_mask_16[0];
      mask[1] = obmc_mask_16[1];
      break;
    case 32:
      mask[0] = obmc_mask_32[0];
      mask[1] = obmc_mask_32[1];
      break;
#if CONFIG_EXT_PARTITION
    case 64:
      mask[0] = obmc_mask_64[0];
      mask[1] = obmc_mask_64[1];
      break;
#endif  // CONFIG_EXT_PARTITION
    default:
      mask[0] = NULL;
      mask[1] = NULL;
      assert(0);
      break;
  }
}

// This function combines motion compensated predictions that is generated by
// top/left neighboring blocks' inter predictors with the regular inter
// prediction. We assume the original prediction (bmc) is stored in
// xd->plane[].dst.buf
void vp10_build_obmc_inter_prediction(VP10_COMMON *cm,
                                      MACROBLOCKD *xd, int mi_row, int mi_col,
                                      int use_tmp_dst_buf,
                                      uint8_t *final_buf[MAX_MB_PLANE],
                                      int final_stride[MAX_MB_PLANE],
                                      uint8_t *tmp_buf1[MAX_MB_PLANE],
                                      int tmp_stride1[MAX_MB_PLANE],
                                      uint8_t *tmp_buf2[MAX_MB_PLANE],
                                      int tmp_stride2[MAX_MB_PLANE]) {
  const TileInfo *const tile = &xd->tile;
  BLOCK_SIZE bsize = xd->mi[0]->mbmi.sb_type;
  int plane, i, mi_step;
#if CONFIG_VP9_HIGHBITDEPTH
  int is_hbd = (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) ? 1 : 0;
#endif  // CONFIG_VP9_HIGHBITDEPTH

  if (use_tmp_dst_buf) {
    for (plane = 0; plane < MAX_MB_PLANE; ++plane) {
      const struct macroblockd_plane *pd = &xd->plane[plane];
      int bw = (xd->n8_w * 8) >> pd->subsampling_x;
      int bh = (xd->n8_h * 8) >> pd->subsampling_y;
      int row;
#if CONFIG_VP9_HIGHBITDEPTH
      if (is_hbd) {
        uint16_t *final_buf16 = CONVERT_TO_SHORTPTR(final_buf[plane]);
        uint16_t *bmc_buf16 = CONVERT_TO_SHORTPTR(pd->dst.buf);
        for (row = 0; row < bh; ++row)
          memcpy(final_buf16 + row * final_stride[plane],
                 bmc_buf16 + row * pd->dst.stride, bw * sizeof(uint16_t));
      } else {
#endif
      for (row = 0; row < bh; ++row)
        memcpy(final_buf[plane] + row * final_stride[plane],
               pd->dst.buf + row * pd->dst.stride, bw);
#if CONFIG_VP9_HIGHBITDEPTH
      }
#endif  // CONFIG_VP9_HIGHBITDEPTH
    }
  }

  // handle above row
  for (i = 0; mi_row > 0 && i < VPXMIN(xd->n8_w, cm->mi_cols - mi_col);
       i += mi_step) {
    int mi_row_offset = -1;
    int mi_col_offset = i;
    int overlap;
    MODE_INFO *above_mi = xd->mi[mi_col_offset +
                                 mi_row_offset * xd->mi_stride];
    MB_MODE_INFO *above_mbmi = &above_mi->mbmi;

    mi_step = VPXMIN(xd->n8_w,
                     num_8x8_blocks_wide_lookup[above_mbmi->sb_type]);

    if (!is_neighbor_overlappable(above_mbmi))
      continue;

    overlap = num_4x4_blocks_high_lookup[bsize] << 1;

    for (plane = 0; plane < MAX_MB_PLANE; ++plane) {
      const struct macroblockd_plane *pd = &xd->plane[plane];
      int bw = (mi_step * MI_SIZE) >> pd->subsampling_x;
      int bh = overlap >> pd->subsampling_y;
      int row, col;
      int dst_stride = use_tmp_dst_buf ? final_stride[plane] : pd->dst.stride;
      uint8_t *dst = use_tmp_dst_buf ?
          &final_buf[plane][(i * MI_SIZE) >> pd->subsampling_x] :
          &pd->dst.buf[(i * MI_SIZE) >> pd->subsampling_x];
      int tmp_stride = tmp_stride1[plane];
      uint8_t *tmp = &tmp_buf1[plane][(i * MI_SIZE) >> pd->subsampling_x];
      const uint8_t *mask[2];

      setup_obmc_mask(bh, mask);

#if CONFIG_VP9_HIGHBITDEPTH
      if (is_hbd) {
        uint16_t *dst16 = CONVERT_TO_SHORTPTR(dst);
        uint16_t *tmp16 = CONVERT_TO_SHORTPTR(tmp);

        for (row = 0; row < bh; ++row) {
          for (col = 0; col < bw; ++col)
            dst16[col] = ROUND_POWER_OF_TWO(mask[0][row] * dst16[col] +
                                            mask[1][row] * tmp16[col], 6);

          dst16 += dst_stride;
          tmp16 += tmp_stride;
        }
      } else {
#endif  // CONFIG_VP9_HIGHBITDEPTH
      for (row = 0; row < bh; ++row) {
        for (col = 0; col < bw; ++col)
          dst[col] = ROUND_POWER_OF_TWO(mask[0][row] * dst[col] +
                                        mask[1][row] * tmp[col], 6);
        dst += dst_stride;
        tmp += tmp_stride;
      }
#if CONFIG_VP9_HIGHBITDEPTH
      }
#endif  // CONFIG_VP9_HIGHBITDEPTH
    }
  }  // each mi in the above row

  if (mi_col == 0 || (mi_col - 1 < tile->mi_col_start) ||
      (mi_col - 1) >= tile->mi_col_end)
    return;
  // handle left column
  for (i = 0; i < VPXMIN(xd->n8_h, cm->mi_rows - mi_row);
       i += mi_step) {
    int mi_row_offset = i;
    int mi_col_offset = -1;
    int overlap;
    MODE_INFO *left_mi = xd->mi[mi_col_offset +
                                mi_row_offset * xd->mi_stride];
    MB_MODE_INFO *left_mbmi = &left_mi->mbmi;

    mi_step = VPXMIN(xd->n8_h,
                     num_8x8_blocks_high_lookup[left_mbmi->sb_type]);

    if (!is_neighbor_overlappable(left_mbmi))
      continue;

    overlap = num_4x4_blocks_wide_lookup[bsize] << 1;

    for (plane = 0; plane < MAX_MB_PLANE; ++plane) {
      const struct macroblockd_plane *pd = &xd->plane[plane];
      int bw = overlap >> pd->subsampling_x;
      int bh = (mi_step * MI_SIZE) >> pd->subsampling_y;
      int row, col;
      int dst_stride = use_tmp_dst_buf ? final_stride[plane] : pd->dst.stride;
      uint8_t *dst = use_tmp_dst_buf ?
          &final_buf[plane][(i * MI_SIZE * dst_stride) >> pd->subsampling_y] :
          &pd->dst.buf[(i * MI_SIZE * dst_stride) >> pd->subsampling_y];
      int tmp_stride = tmp_stride2[plane];
      uint8_t *tmp = &tmp_buf2[plane]
                              [(i * MI_SIZE * tmp_stride) >> pd->subsampling_y];
      const uint8_t *mask[2];

      setup_obmc_mask(bw, mask);

#if CONFIG_VP9_HIGHBITDEPTH
      if (is_hbd) {
        uint16_t *dst16 = CONVERT_TO_SHORTPTR(dst);
        uint16_t *tmp16 = CONVERT_TO_SHORTPTR(tmp);

        for (row = 0; row < bh; ++row) {
          for (col = 0; col < bw; ++col)
            dst16[col] = ROUND_POWER_OF_TWO(mask[0][col] * dst16[col] +
                                            mask[1][col] * tmp16[col], 6);
          dst16 += dst_stride;
          tmp16 += tmp_stride;
        }
      } else {
#endif  // CONFIG_VP9_HIGHBITDEPTH
      for (row = 0; row < bh; ++row) {
        for (col = 0; col < bw; ++col)
          dst[col] = ROUND_POWER_OF_TWO(mask[0][col] * dst[col] +
                                        mask[1][col] * tmp[col], 6);
        dst += dst_stride;
        tmp += tmp_stride;
      }
#if CONFIG_VP9_HIGHBITDEPTH
      }
#endif  // CONFIG_VP9_HIGHBITDEPTH
    }
  }  // each mi in the left column
}

#if CONFIG_EXT_INTER
void modify_neighbor_predictor_for_obmc(MB_MODE_INFO *mbmi) {
  if (is_interintra_pred(mbmi)) {
    mbmi->ref_frame[1] = NONE;
  } else if (has_second_ref(mbmi) && is_interinter_wedge_used(mbmi->sb_type) &&
             mbmi->use_wedge_interinter) {
    mbmi->use_wedge_interinter = 0;
    mbmi->ref_frame[1] = NONE;
  }
  return;
}
#endif  // CONFIG_EXT_INTER

void vp10_build_prediction_by_above_preds(VP10_COMMON *cm,
                                          MACROBLOCKD *xd,
                                          int mi_row, int mi_col,
                                          uint8_t *tmp_buf[MAX_MB_PLANE],
                                          int tmp_stride[MAX_MB_PLANE]) {
  BLOCK_SIZE bsize = xd->mi[0]->mbmi.sb_type;
  int i, j, mi_step, ref;

  if (mi_row == 0)
    return;

  for (i = 0; i < VPXMIN(xd->n8_w, cm->mi_cols - mi_col); i += mi_step) {
    int mi_row_offset = -1;
    int mi_col_offset = i;
    int mi_x, mi_y, bw, bh;
    MODE_INFO *above_mi = xd->mi[mi_col_offset +
                                 mi_row_offset * xd->mi_stride];
    MB_MODE_INFO *above_mbmi = &above_mi->mbmi;
#if CONFIG_EXT_INTER
    MB_MODE_INFO backup_mbmi;
#endif  // CONFIG_EXT_INTER

    mi_step = VPXMIN(xd->n8_w,
                     num_8x8_blocks_wide_lookup[above_mbmi->sb_type]);

    if (!is_neighbor_overlappable(above_mbmi))
      continue;

#if CONFIG_EXT_INTER
    backup_mbmi = *above_mbmi;
    modify_neighbor_predictor_for_obmc(above_mbmi);
#endif  // CONFIG_EXT_INTER

    for (j = 0; j < MAX_MB_PLANE; ++j) {
      struct macroblockd_plane *const pd = &xd->plane[j];
      setup_pred_plane(&pd->dst,
                       tmp_buf[j], tmp_stride[j],
                       0, i, NULL,
                       pd->subsampling_x, pd->subsampling_y);
    }
    for (ref = 0; ref < 1 + has_second_ref(above_mbmi); ++ref) {
      MV_REFERENCE_FRAME frame = above_mbmi->ref_frame[ref];
      RefBuffer *ref_buf = &cm->frame_refs[frame - LAST_FRAME];

      xd->block_refs[ref] = ref_buf;
      if ((!vp10_is_valid_scale(&ref_buf->sf)))
        vpx_internal_error(xd->error_info, VPX_CODEC_UNSUP_BITSTREAM,
                           "Reference frame has invalid dimensions");
      vp10_setup_pre_planes(xd, ref, ref_buf->buf, mi_row, mi_col + i,
                            &ref_buf->sf);
    }

    xd->mb_to_left_edge   = -(((mi_col + i) * MI_SIZE) * 8);
    mi_x = (mi_col + i) << MI_SIZE_LOG2;
    mi_y = mi_row << MI_SIZE_LOG2;

    for (j = 0; j < MAX_MB_PLANE; ++j) {
      const struct macroblockd_plane *pd = &xd->plane[j];
      bw = (mi_step * 8) >> pd->subsampling_x;
      bh = VPXMAX((num_4x4_blocks_high_lookup[bsize] * 2) >> pd->subsampling_y,
                  4);

      if (above_mbmi->sb_type < BLOCK_8X8) {
        const PARTITION_TYPE bp = BLOCK_8X8 - above_mbmi->sb_type;
        const int have_vsplit = bp != PARTITION_HORZ;
        const int have_hsplit = bp != PARTITION_VERT;
        const int num_4x4_w = 2 >> ((!have_vsplit) | pd->subsampling_x);
        const int num_4x4_h = 2 >> ((!have_hsplit) | pd->subsampling_y);
        const int pw = 8 >> (have_vsplit | pd->subsampling_x);
        int x, y;

        for (y = 0; y < num_4x4_h; ++y)
          for (x = 0; x < num_4x4_w; ++x) {
            if ((bp == PARTITION_HORZ || bp == PARTITION_SPLIT)
                && y == 0 && !pd->subsampling_y)
              continue;

            build_inter_predictors(xd, j, mi_col_offset, mi_row_offset,
                                   y * 2 + x, bw, bh,
                                   4 * x, 0, pw, bh,
#if CONFIG_SUPERTX && CONFIG_EXT_INTER
                                   0, 0,
#endif  // CONFIG_SUPERTX && CONFIG_EXT_INTER
                                   mi_x, mi_y);
          }
      } else {
        build_inter_predictors(xd, j, mi_col_offset, mi_row_offset,
                               0, bw, bh, 0, 0, bw, bh,
#if CONFIG_SUPERTX && CONFIG_EXT_INTER
                               0, 0,
#endif  // CONFIG_SUPERTX && CONFIG_EXT_INTER
                               mi_x, mi_y);
      }
    }
#if CONFIG_EXT_INTER
    *above_mbmi = backup_mbmi;
#endif  // CONFIG_EXT_INTER
  }
  xd->mb_to_left_edge   = -((mi_col * MI_SIZE) * 8);
}

void vp10_build_prediction_by_left_preds(VP10_COMMON *cm,
                                         MACROBLOCKD *xd,
                                         int mi_row, int mi_col,
                                         uint8_t *tmp_buf[MAX_MB_PLANE],
                                         int tmp_stride[MAX_MB_PLANE]) {
  const TileInfo *const tile = &xd->tile;
  BLOCK_SIZE bsize = xd->mi[0]->mbmi.sb_type;
  int i, j, mi_step, ref;

  if (mi_col == 0 || (mi_col - 1 < tile->mi_col_start) ||
      (mi_col - 1) >= tile->mi_col_end)
    return;

  for (i = 0; i < VPXMIN(xd->n8_h, cm->mi_rows - mi_row); i += mi_step) {
    int mi_row_offset = i;
    int mi_col_offset = -1;
    int mi_x, mi_y, bw, bh;
    MODE_INFO *left_mi = xd->mi[mi_col_offset +
                                mi_row_offset * xd->mi_stride];
    MB_MODE_INFO *left_mbmi = &left_mi->mbmi;
    const int is_compound = has_second_ref(left_mbmi);
#if CONFIG_EXT_INTER
    MB_MODE_INFO backup_mbmi;
#endif  // CONFIG_EXT_INTER

    mi_step = VPXMIN(xd->n8_h,
                     num_8x8_blocks_high_lookup[left_mbmi->sb_type]);

    if (!is_neighbor_overlappable(left_mbmi))
      continue;

#if CONFIG_EXT_INTER
    backup_mbmi = *left_mbmi;
    modify_neighbor_predictor_for_obmc(left_mbmi);
#endif  // CONFIG_EXT_INTER

    for (j = 0; j < MAX_MB_PLANE; ++j) {
      struct macroblockd_plane *const pd = &xd->plane[j];
      setup_pred_plane(&pd->dst,
                       tmp_buf[j], tmp_stride[j],
                       i, 0, NULL,
                       pd->subsampling_x, pd->subsampling_y);
    }
    for (ref = 0; ref < 1 + is_compound; ++ref) {
      MV_REFERENCE_FRAME frame = left_mbmi->ref_frame[ref];
      RefBuffer *ref_buf = &cm->frame_refs[frame - LAST_FRAME];

      xd->block_refs[ref] = ref_buf;
      if ((!vp10_is_valid_scale(&ref_buf->sf)))
        vpx_internal_error(xd->error_info, VPX_CODEC_UNSUP_BITSTREAM,
                           "Reference frame has invalid dimensions");
      vp10_setup_pre_planes(xd, ref, ref_buf->buf, mi_row + i, mi_col,
                            &ref_buf->sf);
    }

    xd->mb_to_top_edge    = -(((mi_row + i) * MI_SIZE) * 8);
    mi_x = mi_col << MI_SIZE_LOG2;
    mi_y = (mi_row + i) << MI_SIZE_LOG2;

    for (j = 0; j < MAX_MB_PLANE; ++j) {
      const struct macroblockd_plane *pd = &xd->plane[j];
      bw = VPXMAX((num_4x4_blocks_wide_lookup[bsize] * 2) >> pd->subsampling_x,
                  4);
      bh = (mi_step << MI_SIZE_LOG2) >> pd->subsampling_y;

      if (left_mbmi->sb_type < BLOCK_8X8) {
        const PARTITION_TYPE bp = BLOCK_8X8 - left_mbmi->sb_type;
        const int have_vsplit = bp != PARTITION_HORZ;
        const int have_hsplit = bp != PARTITION_VERT;
        const int num_4x4_w = 2 >> ((!have_vsplit) | pd->subsampling_x);
        const int num_4x4_h = 2 >> ((!have_hsplit) | pd->subsampling_y);
        const int ph = 8 >> (have_hsplit | pd->subsampling_y);
        int x, y;

        for (y = 0; y < num_4x4_h; ++y)
          for (x = 0; x < num_4x4_w; ++x) {
            if ((bp == PARTITION_VERT || bp == PARTITION_SPLIT)
                && x == 0 && !pd->subsampling_x)
              continue;

            build_inter_predictors(xd, j, mi_col_offset, mi_row_offset,
                                   y * 2 + x, bw, bh,
                                   0, 4 * y, bw, ph,
#if CONFIG_SUPERTX && CONFIG_EXT_INTER
                                   0, 0,
#endif  // CONFIG_SUPERTX && CONFIG_EXT_INTER
                                   mi_x, mi_y);
          }
      } else {
        build_inter_predictors(xd, j, mi_col_offset, mi_row_offset, 0,
                               bw, bh, 0, 0, bw, bh,
#if CONFIG_SUPERTX && CONFIG_EXT_INTER
                               0, 0,
#endif  // CONFIG_SUPERTX && CONFIG_EXT_INTER
                               mi_x, mi_y);
      }
    }
#if CONFIG_EXT_INTER
    *left_mbmi = backup_mbmi;
#endif  // CONFIG_EXT_INTER
  }
  xd->mb_to_top_edge    = -((mi_row * MI_SIZE) * 8);
}
#endif  // CONFIG_OBMC

#if CONFIG_EXT_INTER
#if CONFIG_EXT_PARTITION
static const int ii_weights1d[MAX_SB_SIZE] = {
  102, 100,  97,  95,  92,  90,  88,  86,
  84,  82,  80,  78,  76,  74,  73,  71,
  69,  68,  67,  65,  64,  62,  61,  60,
  59,  58,  57,  55,  54,  53,  52,  52,
  51,  50,  49,  48,  47,  47,  46,  45,
  45,  44,  43,  43,  42,  41,  41,  40,
  40,  39,  39,  38,  38,  38,  37,  37,
  36,  36,  36,  35,  35,  35,  34,  34,
  34,  33,  33,  33,  33,  32,  32,  32,
  32,  32,  31,  31,  31,  31,  31,  30,
  30,  30,  30,  30,  30,  30,  29,  29,
  29,  29,  29,  29,  29,  29,  28,  28,
  28,  28,  28,  28,  28,  28,  28,  28,
  28,  28,  27,  27,  27,  27,  27,  27,
  27,  27,  27,  27,  27,  27,  27,  27,
  27,  27,  27,  27,  27,  27,  27,  27,
};
static int ii_size_scales[BLOCK_SIZES] = {
  32, 16, 16, 16, 8, 8, 8, 4, 4, 4, 2, 2, 2, 1, 1, 1
};
#else
static const int ii_weights1d[MAX_SB_SIZE] = {
  102, 100,  97,  95,  92,  90,  88,  86,
  84,  82,  80,  78,  76,  74,  73,  71,
  69,  68,  67,  65,  64,  62,  61,  60,
  59,  58,  57,  55,  54,  53,  52,  52,
  51,  50,  49,  48,  47,  47,  46,  45,
  45,  44,  43,  43,  42,  41,  41,  40,
  40,  39,  39,  38,  38,  38,  37,  37,
  36,  36,  36,  35,  35,  35,  34,  34,
};
static int ii_size_scales[BLOCK_SIZES] = {
  16, 8, 8, 8, 4, 4, 4, 2, 2, 2, 1, 1, 1
};
#endif  // CONFIG_EXT_PARTITION

static void combine_interintra(INTERINTRA_MODE mode,
                               int use_wedge_interintra,
                               int wedge_index,
                               int wedge_sign,
                               BLOCK_SIZE bsize,
                               BLOCK_SIZE plane_bsize,
                               uint8_t *comppred,
                               int compstride,
                               uint8_t *interpred,
                               int interstride,
                               uint8_t *intrapred,
                               int intrastride) {
  static const int scale_bits = 8;
  static const int scale_max = 256;
  static const int scale_round = 127;
  const int bw = 4 * num_4x4_blocks_wide_lookup[plane_bsize];
  const int bh = 4 * num_4x4_blocks_high_lookup[plane_bsize];
  const int size_scale = ii_size_scales[plane_bsize];
  int i, j;

  if (use_wedge_interintra) {
    if (is_interintra_wedge_used(bsize)) {
      const uint8_t *mask = vp10_get_soft_mask(wedge_index, wedge_sign,
                                               bsize, 0, 0);
      const int subw = 2 * num_4x4_blocks_wide_lookup[bsize] == bw;
      const int subh = 2 * num_4x4_blocks_high_lookup[bsize] == bh;
      build_masked_compound(comppred, compstride,
                            intrapred, intrastride,
                            interpred, interstride, mask,
                            bh, bw, subh, subw);
    }
    return;
  }

  switch (mode) {
    case II_V_PRED:
      for (i = 0; i < bh; ++i) {
        for (j = 0; j < bw; ++j) {
          int scale = ii_weights1d[i * size_scale];
          comppred[i * compstride + j] =
              ((scale_max - scale) * interpred[i * interstride + j] +
               scale * intrapred[i * intrastride + j] + scale_round)
              >> scale_bits;
        }
      }
      break;

    case II_H_PRED:
      for (i = 0; i < bh; ++i) {
        for (j = 0; j < bw; ++j) {
          int scale = ii_weights1d[j * size_scale];
          comppred[i * compstride + j] =
              ((scale_max - scale) * interpred[i * interstride + j] +
               scale * intrapred[i * intrastride + j] + scale_round)
              >> scale_bits;
        }
      }
      break;

    case II_D63_PRED:
    case II_D117_PRED:
      for (i = 0; i < bh; ++i) {
        for (j = 0; j < bw; ++j) {
          int scale = (ii_weights1d[i * size_scale] * 3 +
                       ii_weights1d[j * size_scale]) >> 2;
          comppred[i * compstride + j] =
              ((scale_max - scale) * interpred[i * interstride + j] +
               scale * intrapred[i * intrastride + j] + scale_round)
              >> scale_bits;
        }
      }
      break;

    case II_D207_PRED:
    case II_D153_PRED:
      for (i = 0; i < bh; ++i) {
        for (j = 0; j < bw; ++j) {
          int scale = (ii_weights1d[j * size_scale] * 3 +
                       ii_weights1d[i * size_scale]) >> 2;
          comppred[i * compstride + j] =
              ((scale_max - scale) * interpred[i * interstride + j] +
               scale * intrapred[i * intrastride + j] + scale_round)
              >> scale_bits;
        }
      }
      break;

    case II_D135_PRED:
      for (i = 0; i < bh; ++i) {
        for (j = 0; j < bw; ++j) {
          int scale = ii_weights1d[(i < j ? i : j) * size_scale];
          comppred[i * compstride + j] =
              ((scale_max - scale) * interpred[i * interstride + j] +
               scale * intrapred[i * intrastride + j] + scale_round)
              >> scale_bits;
        }
      }
      break;

    case II_D45_PRED:
      for (i = 0; i < bh; ++i) {
        for (j = 0; j < bw; ++j) {
          int scale = (ii_weights1d[i * size_scale] +
                       ii_weights1d[j * size_scale]) >> 1;
          comppred[i * compstride + j] =
              ((scale_max - scale) * interpred[i * interstride + j] +
               scale * intrapred[i * intrastride + j] + scale_round)
              >> scale_bits;
        }
      }
      break;

    case II_TM_PRED:
    case II_DC_PRED:
    default:
      for (i = 0; i < bh; ++i) {
        for (j = 0; j < bw; ++j) {
          comppred[i * compstride + j] = (interpred[i * interstride + j] +
                                          intrapred[i * intrastride + j]) >> 1;
        }
      }
      break;
  }
}

#if CONFIG_VP9_HIGHBITDEPTH
static void combine_interintra_highbd(INTERINTRA_MODE mode,
                                      int use_wedge_interintra,
                                      int wedge_index,
                                      int wedge_sign,
                                      BLOCK_SIZE bsize,
                                      BLOCK_SIZE plane_bsize,
                                      uint8_t *comppred8,
                                      int compstride,
                                      uint8_t *interpred8,
                                      int interstride,
                                      uint8_t *intrapred8,
                                      int intrastride, int bd) {
  static const int scale_bits = 8;
  static const int scale_max = 256;
  static const int scale_round = 127;
  const int bw = 4 * num_4x4_blocks_wide_lookup[plane_bsize];
  const int bh = 4 * num_4x4_blocks_high_lookup[plane_bsize];
  const int size_scale = ii_size_scales[plane_bsize];
  int i, j;

  uint16_t *comppred = CONVERT_TO_SHORTPTR(comppred8);
  uint16_t *interpred = CONVERT_TO_SHORTPTR(interpred8);
  uint16_t *intrapred = CONVERT_TO_SHORTPTR(intrapred8);
  (void) bd;

  if (use_wedge_interintra) {
    if (is_interintra_wedge_used(bsize)) {
      const uint8_t *mask = vp10_get_soft_mask(wedge_index, wedge_sign,
                                               bsize, 0, 0);
      const int subh = 2 * num_4x4_blocks_high_lookup[bsize] == bh;
      const int subw = 2 * num_4x4_blocks_wide_lookup[bsize] == bw;
      build_masked_compound(comppred8, compstride,
                            intrapred8, intrastride,
                            interpred8, interstride, mask,
                            bh, bw, subh, subw);
    }
    return;
  }

  switch (mode) {
    case II_V_PRED:
      for (i = 0; i < bh; ++i) {
        for (j = 0; j < bw; ++j) {
          int scale = ii_weights1d[i * size_scale];
          comppred[i * compstride + j] =
              ((scale_max - scale) * interpred[i * interstride + j] +
               scale * intrapred[i * intrastride + j] + scale_round)
              >> scale_bits;
        }
      }
      break;

    case II_H_PRED:
      for (i = 0; i < bh; ++i) {
        for (j = 0; j < bw; ++j) {
          int scale = ii_weights1d[j * size_scale];
          comppred[i * compstride + j] =
              ((scale_max - scale) * interpred[i * interstride + j] +
               scale * intrapred[i * intrastride + j] + scale_round)
              >> scale_bits;
        }
      }
      break;

    case II_D63_PRED:
    case II_D117_PRED:
      for (i = 0; i < bh; ++i) {
        for (j = 0; j < bw; ++j) {
          int scale = (ii_weights1d[i * size_scale] * 3 +
                       ii_weights1d[j * size_scale]) >> 2;
          comppred[i * compstride + j] =
              ((scale_max - scale) * interpred[i * interstride + j] +
               scale * intrapred[i * intrastride + j] + scale_round)
              >> scale_bits;
        }
      }
      break;

    case II_D207_PRED:
    case II_D153_PRED:
      for (i = 0; i < bh; ++i) {
        for (j = 0; j < bw; ++j) {
          int scale = (ii_weights1d[j * size_scale] * 3 +
                       ii_weights1d[i * size_scale]) >> 2;
          comppred[i * compstride + j] =
              ((scale_max - scale) * interpred[i * interstride + j] +
               scale * intrapred[i * intrastride + j] + scale_round)
              >> scale_bits;
        }
      }
      break;

    case II_D135_PRED:
      for (i = 0; i < bh; ++i) {
        for (j = 0; j < bw; ++j) {
          int scale = ii_weights1d[(i < j ? i : j) * size_scale];
          comppred[i * compstride + j] =
              ((scale_max - scale) * interpred[i * interstride + j] +
               scale * intrapred[i * intrastride + j] + scale_round)
              >> scale_bits;
        }
      }
      break;

    case II_D45_PRED:
      for (i = 0; i < bh; ++i) {
        for (j = 0; j < bw; ++j) {
          int scale = (ii_weights1d[i * size_scale] +
                       ii_weights1d[j * size_scale]) >> 1;
          comppred[i * compstride + j] =
              ((scale_max - scale) * interpred[i * interstride + j] +
               scale * intrapred[i * intrastride + j] + scale_round)
              >> scale_bits;
        }
      }
      break;

    case II_TM_PRED:
    case II_DC_PRED:
    default:
      for (i = 0; i < bh; ++i) {
        for (j = 0; j < bw; ++j) {
          comppred[i * compstride + j] = (interpred[i * interstride + j] +
                                          intrapred[i * intrastride + j]) >> 1;
        }
      }
      break;
  }
}
#endif  // CONFIG_VP9_HIGHBITDEPTH

// Break down rectangular intra prediction for joint spatio-temporal prediction
// into two square intra predictions.
static void build_intra_predictors_for_interintra(
    MACROBLOCKD *xd,
    uint8_t *ref, int ref_stride,
    uint8_t *dst, int dst_stride,
    PREDICTION_MODE mode,
    BLOCK_SIZE bsize,
    int plane) {
  BLOCK_SIZE plane_bsize = get_plane_block_size(bsize, &xd->plane[plane]);
  const int bwl = b_width_log2_lookup[plane_bsize];
  const int bhl = b_height_log2_lookup[plane_bsize];
  TX_SIZE max_tx_size = max_txsize_lookup[plane_bsize];

  if (bwl == bhl) {
    vp10_predict_intra_block(xd, bwl, bhl, max_tx_size, mode,
                             ref, ref_stride, dst, dst_stride,
                             0, 0, plane);

  } else if (bwl < bhl) {
    uint8_t *src_2 = ref + (4 << bwl)*ref_stride;
    uint8_t *dst_2 = dst + (4 << bwl)*dst_stride;
    vp10_predict_intra_block(xd, bwl, bhl, max_tx_size, mode,
                             ref, ref_stride, dst, dst_stride,
                             0, 0, plane);
#if CONFIG_VP9_HIGHBITDEPTH
    if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
      uint16_t *src_216 = CONVERT_TO_SHORTPTR(src_2);
      uint16_t *dst_216 = CONVERT_TO_SHORTPTR(dst_2);
      memcpy(src_216 - ref_stride, dst_216 - dst_stride,
             sizeof(*src_216) * (4 << bhl));
    } else
#endif  // CONFIG_VP9_HIGHBITDEPTH
    {
      memcpy(src_2 - ref_stride, dst_2 - dst_stride,
             sizeof(*src_2) * (4 << bhl));
    }
    vp10_predict_intra_block(xd, bwl, bhl, max_tx_size, mode,
                             src_2, ref_stride, dst_2, dst_stride,
                             0, 1 << bwl, plane);
  } else {
    int i;
    uint8_t *src_2 = ref + (4 << bhl);
    uint8_t *dst_2 = dst + (4 << bhl);
    vp10_predict_intra_block(xd, bwl, bhl, max_tx_size, mode,
                             ref, ref_stride, dst, dst_stride,
                             0, 0, plane);
#if CONFIG_VP9_HIGHBITDEPTH
    if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
      uint16_t *src_216 = CONVERT_TO_SHORTPTR(src_2);
      uint16_t *dst_216 = CONVERT_TO_SHORTPTR(dst_2);
      for (i = 0; i < (4 << bwl); ++i)
        src_216[i * ref_stride - 1] = dst_216[i * dst_stride - 1];
    } else
#endif  // CONFIG_VP9_HIGHBITDEPTH
    {
      for (i = 0; i < (4 << bwl); ++i)
        src_2[i * ref_stride - 1] = dst_2[i * dst_stride - 1];
    }
    vp10_predict_intra_block(xd, bwl, bhl, max_tx_size, mode,
                             src_2, ref_stride, dst_2, dst_stride,
                             1 << bhl, 0, plane);
  }
}

// Mapping of interintra to intra mode for use in the intra component
static const int interintra_to_intra_mode[INTERINTRA_MODES] = {
  DC_PRED,
  V_PRED,
  H_PRED,
  D45_PRED,
  D135_PRED,
  D117_PRED,
  D153_PRED,
  D207_PRED,
  D63_PRED,
  TM_PRED
};

void vp10_build_intra_predictors_for_interintra(
    MACROBLOCKD *xd,
    BLOCK_SIZE bsize, int plane,
    uint8_t *dst, int dst_stride) {
  build_intra_predictors_for_interintra(
      xd, xd->plane[plane].dst.buf, xd->plane[plane].dst.stride,
      dst, dst_stride,
      interintra_to_intra_mode[xd->mi[0]->mbmi.interintra_mode],
      bsize, plane);
}

void vp10_combine_interintra(MACROBLOCKD *xd,
                             BLOCK_SIZE bsize, int plane,
                             uint8_t *inter_pred, int inter_stride,
                             uint8_t *intra_pred, int intra_stride) {
  const BLOCK_SIZE plane_bsize = get_plane_block_size(bsize, &xd->plane[plane]);
#if CONFIG_VP9_HIGHBITDEPTH
  if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
    combine_interintra_highbd(xd->mi[0]->mbmi.interintra_mode,
                              xd->mi[0]->mbmi.use_wedge_interintra,
                              xd->mi[0]->mbmi.interintra_wedge_index,
                              xd->mi[0]->mbmi.interintra_wedge_sign,
                              bsize,
                              plane_bsize,
                              xd->plane[plane].dst.buf,
                              xd->plane[plane].dst.stride,
                              inter_pred, inter_stride,
                              intra_pred, intra_stride,
                              xd->bd);
    return;
  }
#endif  // CONFIG_VP9_HIGHBITDEPTH
  combine_interintra(xd->mi[0]->mbmi.interintra_mode,
                     xd->mi[0]->mbmi.use_wedge_interintra,
                     xd->mi[0]->mbmi.interintra_wedge_index,
                     xd->mi[0]->mbmi.interintra_wedge_sign,
                     bsize,
                     plane_bsize,
                     xd->plane[plane].dst.buf, xd->plane[plane].dst.stride,
                     inter_pred, inter_stride,
                     intra_pred, intra_stride);
}

void vp10_build_interintra_predictors_sby(MACROBLOCKD *xd,
                                          uint8_t *ypred,
                                          int ystride,
                                          BLOCK_SIZE bsize) {
#if CONFIG_VP9_HIGHBITDEPTH
  if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
        DECLARE_ALIGNED(16, uint16_t,
                        intrapredictor[MAX_SB_SQUARE]);
    vp10_build_intra_predictors_for_interintra(
        xd, bsize, 0, CONVERT_TO_BYTEPTR(intrapredictor), MAX_SB_SIZE);
    vp10_combine_interintra(xd, bsize, 0, ypred, ystride,
                            CONVERT_TO_BYTEPTR(intrapredictor), MAX_SB_SIZE);
    return;
  }
#endif  // CONFIG_VP9_HIGHBITDEPTH
  {
    DECLARE_ALIGNED(16, uint8_t, intrapredictor[MAX_SB_SQUARE]);
    vp10_build_intra_predictors_for_interintra(
        xd, bsize, 0, intrapredictor, MAX_SB_SIZE);
    vp10_combine_interintra(xd, bsize, 0, ypred, ystride,
                            intrapredictor, MAX_SB_SIZE);
  }
}

void vp10_build_interintra_predictors_sbc(MACROBLOCKD *xd,
                                          uint8_t *upred,
                                          int ustride,
                                          int plane,
                                          BLOCK_SIZE bsize) {
#if CONFIG_VP9_HIGHBITDEPTH
  if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
    DECLARE_ALIGNED(16, uint16_t,
                    uintrapredictor[MAX_SB_SQUARE]);
    vp10_build_intra_predictors_for_interintra(
        xd, bsize, plane, CONVERT_TO_BYTEPTR(uintrapredictor), MAX_SB_SIZE);
    vp10_combine_interintra(xd, bsize, plane, upred, ustride,
                            CONVERT_TO_BYTEPTR(uintrapredictor), MAX_SB_SIZE);
    return;
  }
#endif  // CONFIG_VP9_HIGHBITDEPTH
  {
    DECLARE_ALIGNED(16, uint8_t, uintrapredictor[MAX_SB_SQUARE]);
    vp10_build_intra_predictors_for_interintra(
        xd, bsize, plane, uintrapredictor, MAX_SB_SIZE);
    vp10_combine_interintra(xd, bsize, plane, upred, ustride,
                            uintrapredictor, MAX_SB_SIZE);
  }
}

void vp10_build_interintra_predictors_sbuv(MACROBLOCKD *xd,
                                           uint8_t *upred,
                                           uint8_t *vpred,
                                           int ustride, int vstride,
                                           BLOCK_SIZE bsize) {
  vp10_build_interintra_predictors_sbc(xd, upred, ustride, 1, bsize);
  vp10_build_interintra_predictors_sbc(xd, vpred, vstride, 2, bsize);
}

void vp10_build_interintra_predictors(MACROBLOCKD *xd,
                                      uint8_t *ypred,
                                      uint8_t *upred,
                                      uint8_t *vpred,
                                      int ystride, int ustride, int vstride,
                                      BLOCK_SIZE bsize) {
  vp10_build_interintra_predictors_sby(xd, ypred, ystride, bsize);
  vp10_build_interintra_predictors_sbuv(xd, upred, vpred,
                                        ustride, vstride, bsize);
}

// Builds the inter-predictor for the single ref case
// for use in the encoder to search the wedges efficiently.
static void build_inter_predictors_single_buf(MACROBLOCKD *xd, int plane,
                                              int block,
                                              int bw, int bh,
                                              int x, int y, int w, int h,
                                              int mi_x, int mi_y,
                                              int ref,
                                              uint8_t *const ext_dst,
                                              int ext_dst_stride) {
  struct macroblockd_plane *const pd = &xd->plane[plane];
  const MODE_INFO *mi = xd->mi[0];
  const INTERP_FILTER interp_filter = mi->mbmi.interp_filter;

  const struct scale_factors *const sf = &xd->block_refs[ref]->sf;
  struct buf_2d *const pre_buf = &pd->pre[ref];
#if CONFIG_VP9_HIGHBITDEPTH
  uint8_t *const dst =
      (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH ?
      CONVERT_TO_BYTEPTR(ext_dst) : ext_dst) + ext_dst_stride * y + x;
#else
  uint8_t *const dst = ext_dst + ext_dst_stride * y + x;
#endif
  const MV mv = mi->mbmi.sb_type < BLOCK_8X8
      ? average_split_mvs(pd, mi, ref, block)
      : mi->mbmi.mv[ref].as_mv;

  // TODO(jkoleszar): This clamping is done in the incorrect place for the
  // scaling case. It needs to be done on the scaled MV, not the pre-scaling
  // MV. Note however that it performs the subsampling aware scaling so
  // that the result is always q4.
  // mv_precision precision is MV_PRECISION_Q4.
  const MV mv_q4 = clamp_mv_to_umv_border_sb(xd, &mv, bw, bh,
                                             pd->subsampling_x,
                                             pd->subsampling_y);

  uint8_t *pre;
  MV32 scaled_mv;
  int xs, ys, subpel_x, subpel_y;
  const int is_scaled = vp10_is_scaled(sf);

  if (is_scaled) {
    pre = pre_buf->buf + scaled_buffer_offset(x, y, pre_buf->stride, sf);
    scaled_mv = vp10_scale_mv(&mv_q4, mi_x + x, mi_y + y, sf);
    xs = sf->x_step_q4;
    ys = sf->y_step_q4;
  } else {
    pre = pre_buf->buf + (y * pre_buf->stride + x);
    scaled_mv.row = mv_q4.row;
    scaled_mv.col = mv_q4.col;
    xs = ys = 16;
  }

  subpel_x = scaled_mv.col & SUBPEL_MASK;
  subpel_y = scaled_mv.row & SUBPEL_MASK;
  pre += (scaled_mv.row >> SUBPEL_BITS) * pre_buf->stride
      + (scaled_mv.col >> SUBPEL_BITS);

  vp10_make_inter_predictor(pre, pre_buf->stride, dst, ext_dst_stride,
                            subpel_x, subpel_y, sf, w, h, 0,
                            interp_filter, xs, ys, xd);
}

void vp10_build_inter_predictors_for_planes_single_buf(
    MACROBLOCKD *xd, BLOCK_SIZE bsize,
    int mi_row, int mi_col, int ref,
    uint8_t *ext_dst[3], int ext_dst_stride[3]) {
  const int plane_from = 0;
  const int plane_to = 2;
  int plane;
  const int mi_x = mi_col * MI_SIZE;
  const int mi_y = mi_row * MI_SIZE;
  for (plane = plane_from; plane <= plane_to; ++plane) {
    const BLOCK_SIZE plane_bsize = get_plane_block_size(bsize,
                                                        &xd->plane[plane]);
    const int num_4x4_w = num_4x4_blocks_wide_lookup[plane_bsize];
    const int num_4x4_h = num_4x4_blocks_high_lookup[plane_bsize];
    const int bw = 4 * num_4x4_w;
    const int bh = 4 * num_4x4_h;

    if (xd->mi[0]->mbmi.sb_type < BLOCK_8X8) {
      int x, y;
      assert(bsize == BLOCK_8X8);
      for (y = 0; y < num_4x4_h; ++y)
        for (x = 0; x < num_4x4_w; ++x)
          build_inter_predictors_single_buf(xd, plane,
                                            y * 2 + x, bw, bh,
                                            4 * x, 4 * y, 4, 4,
                                            mi_x, mi_y, ref,
                                            ext_dst[plane],
                                            ext_dst_stride[plane]);
    } else {
      build_inter_predictors_single_buf(xd, plane,
                                        0, bw, bh,
                                        0, 0, bw, bh,
                                        mi_x, mi_y, ref,
                                        ext_dst[plane],
                                        ext_dst_stride[plane]);
    }
  }
}

static void build_wedge_inter_predictor_from_buf(MACROBLOCKD *xd, int plane,
                                                 int block, int bw, int bh,
                                                 int x, int y, int w, int h,
#if CONFIG_SUPERTX
                                                 int wedge_offset_x,
                                                 int wedge_offset_y,
#endif  // CONFIG_SUPERTX
                                                 int mi_x, int mi_y,
                                                 uint8_t *ext_dst0,
                                                 int ext_dst_stride0,
                                                 uint8_t *ext_dst1,
                                                 int ext_dst_stride1) {
  struct macroblockd_plane *const pd = &xd->plane[plane];
  const MODE_INFO *mi = xd->mi[0];
  const int is_compound = has_second_ref(&mi->mbmi);
  int ref;
  (void) block;
  (void) bw;
  (void) bh;
  (void) mi_x;
  (void) mi_y;

  for (ref = 0; ref < 1 + is_compound; ++ref) {
    struct buf_2d *const dst_buf = &pd->dst;
    uint8_t *const dst = dst_buf->buf + dst_buf->stride * y + x;

    if (ref && is_interinter_wedge_used(mi->mbmi.sb_type)
        && mi->mbmi.use_wedge_interinter) {
#if CONFIG_VP9_HIGHBITDEPTH
      DECLARE_ALIGNED(16, uint8_t, tmp_dst_[2 * MAX_SB_SQUARE]);
      uint8_t *tmp_dst =
          (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) ?
          CONVERT_TO_BYTEPTR(tmp_dst_) : tmp_dst_;
#else
      DECLARE_ALIGNED(16, uint8_t, tmp_dst[MAX_SB_SQUARE]);
#endif  // CONFIG_VP9_HIGHBITDEPTH
#if CONFIG_VP9_HIGHBITDEPTH
        if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
          int k;
          for (k = 0; k < h; ++k)
            memcpy(tmp_dst_ + 2 * MAX_SB_SIZE * k, ext_dst1 +
                   ext_dst_stride1 * 2 * k, w * 2);
        } else {
          int k;
          for (k = 0; k < h; ++k)
            memcpy(tmp_dst_ + MAX_SB_SIZE * k, ext_dst1 +
                   ext_dst_stride1 * k, w);
        }
#else
        {
          int k;
          for (k = 0; k < h; ++k)
            memcpy(tmp_dst + MAX_SB_SIZE * k, ext_dst1 +
                   ext_dst_stride1 * k, w);
        }
#endif  // CONFIG_VP9_HIGHBITDEPTH

#if CONFIG_SUPERTX
#if CONFIG_VP9_HIGHBITDEPTH
      if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
        build_masked_compound_wedge_extend_highbd(
            dst, dst_buf->stride, tmp_dst, MAX_SB_SIZE,
            mi->mbmi.interinter_wedge_index,
            mi->mbmi.interinter_wedge_sign,
            mi->mbmi.sb_type,
            wedge_offset_x, wedge_offset_y, h, w);
      } else {
        build_masked_compound_wedge_extend(
            dst, dst_buf->stride, tmp_dst, MAX_SB_SIZE,
            mi->mbmi.interinter_wedge_index,
            mi->mbmi.interinter_wedge_sign,
            mi->mbmi.sb_type,
            wedge_offset_x, wedge_offset_y, h, w);
      }
#else
      build_masked_compound_wedge_extend(dst, dst_buf->stride,
                                         tmp_dst, MAX_SB_SIZE,
                                         mi->mbmi.interinter_wedge_index,
                                         mi->mbmi.interinter_wedge_sign,
                                         mi->mbmi.sb_type,
                                         wedge_offset_x, wedge_offset_y, h, w);
#endif  // CONFIG_VP9_HIGHBITDEPTH
#else   // CONFIG_SUPERTX
#if CONFIG_VP9_HIGHBITDEPTH
      if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH)
        build_masked_compound_wedge_highbd(dst, dst_buf->stride, tmp_dst,
                                           MAX_SB_SIZE,
                                           mi->mbmi.interinter_wedge_index,
                                           mi->mbmi.interinter_wedge_sign,
                                           mi->mbmi.sb_type, h, w);
      else
#endif  // CONFIG_VP9_HIGHBITDEPTH
        build_masked_compound_wedge(dst, dst_buf->stride, tmp_dst, MAX_SB_SIZE,
                                    mi->mbmi.interinter_wedge_index,
                                    mi->mbmi.interinter_wedge_sign,
                                    mi->mbmi.sb_type, h, w);
#endif  // CONFIG_SUPERTX
    } else {
#if CONFIG_VP9_HIGHBITDEPTH
        if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
          int k;
          for (k = 0; k < h; ++k)
            memcpy(CONVERT_TO_SHORTPTR(dst + dst_buf->stride * k),
                   ext_dst0 + ext_dst_stride0 * 2 * k, w * 2);
        } else {
          int k;
          for (k = 0; k < h; ++k)
            memcpy(dst + dst_buf->stride * k,
                   ext_dst0 + ext_dst_stride0 * k, w);
        }
#else
        {
          int k;
          for (k = 0; k < h; ++k)
            memcpy(dst + dst_buf->stride * k,
                   ext_dst0 + ext_dst_stride0 * k, w);
        }
#endif  // CONFIG_VP9_HIGHBITDEPTH
    }
  }
}

void vp10_build_wedge_inter_predictor_from_buf(
    MACROBLOCKD *xd, BLOCK_SIZE bsize,
    int mi_row, int mi_col,
    uint8_t *ext_dst0[3], int ext_dst_stride0[3],
    uint8_t *ext_dst1[3], int ext_dst_stride1[3]) {
  const int plane_from = 0;
  const int plane_to = 2;
  int plane;
  const int mi_x = mi_col * MI_SIZE;
  const int mi_y = mi_row * MI_SIZE;
  for (plane = plane_from; plane <= plane_to; ++plane) {
    const BLOCK_SIZE plane_bsize = get_plane_block_size(bsize,
                                                        &xd->plane[plane]);
    const int num_4x4_w = num_4x4_blocks_wide_lookup[plane_bsize];
    const int num_4x4_h = num_4x4_blocks_high_lookup[plane_bsize];
    const int bw = 4 * num_4x4_w;
    const int bh = 4 * num_4x4_h;

    if (xd->mi[0]->mbmi.sb_type < BLOCK_8X8) {
      int i = 0, x, y;
      assert(bsize == BLOCK_8X8);
      for (y = 0; y < num_4x4_h; ++y)
        for (x = 0; x < num_4x4_w; ++x)
          build_wedge_inter_predictor_from_buf(xd, plane, i++, bw, bh,
                                               4 * x, 4 * y, 4, 4,
#if CONFIG_SUPERTX
                                               0, 0,
#endif
                                               mi_x, mi_y,
                                               ext_dst0[plane],
                                               ext_dst_stride0[plane],
                                               ext_dst1[plane],
                                               ext_dst_stride1[plane]);
    } else {
      build_wedge_inter_predictor_from_buf(xd, plane, 0, bw, bh,
                                           0, 0, bw, bh,
#if CONFIG_SUPERTX
                                           0, 0,
#endif
                                           mi_x, mi_y,
                                           ext_dst0[plane],
                                           ext_dst_stride0[plane],
                                           ext_dst1[plane],
                                           ext_dst_stride1[plane]);
    }
  }
}
#endif  // CONFIG_EXT_INTER
