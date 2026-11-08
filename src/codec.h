/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Parsed H.264 / HEVC syntax handed from the codec layers to the stateless hardware backends.
 * Roughly the V4L2 stateless controls (v4l2-controls.h), so a v4l2req backend can be fed from the same jobs. */
#ifndef PMHW_CODEC_H
#define PMHW_CODEC_H
#include <stdint.h>
#include "internal.h"

/* ---------------- H.264 ---------------- */
typedef struct H264Sps {
    int ok, profile, chroma_format_idc, log2_max_frame_num, poc_type, log2_max_poc_lsb, delta_pic_order_always_zero;
    int offset_non_ref, offset_top_bottom, num_ref_cycle, offset_ref[256];
    int max_num_ref_frames, width_mbs, height_map_units, frame_mbs_only, mbaff, direct_8x8;
    int crop_l, crop_r, crop_t, crop_b;
    int scaling_present; uint8_t sl4[6][16], sl8[2][64];
    int id, constraint_flags, level, gaps_allowed, qpprime_bypass;     /* for V4L2 */
} H264Sps;

typedef struct H264Pps {
    int ok, sps_id, entropy, bottom_field_poc_present, num_ref_l0, num_ref_l1, weighted_pred, weighted_bipred;
    int init_qp, chroma_qp_offset, second_chroma_qp_offset, deblock_ctrl, constrained_intra, redundant_pic_cnt, t8x8;
    int scaling_present; uint8_t sl4[6][16], sl8[2][64];
    int id, init_qs;                                                    /* for V4L2 */
} H264Pps;

typedef struct H264Slice {
    int nal_type, nal_ref_idc, first_mb, type, pps_id, frame_num, idr, poc_lsb, delta_poc_bottom, delta_poc[2];
    int direct_spatial, num_ref[2];                 /* active - 1 */
    int nmod[2], mod_idc[2][33], mod_val[2][33];
    int luma_denom, chroma_denom;
    int16_t lw[2][32], lo[2][32], cw[2][32][2], co[2][32][2];
    int adaptive, nmmco, mmco[33][2];
    int cabac_init, qp_delta, disable_deblock, alpha, beta;
    int header_bits;                                /* incl. the NAL header byte, before cabac alignment */
    int idr_pic_id, poc_bits, ref_marking_bits;    /* for V4L2 decode params */
} H264Slice;

enum { H264_P = 0, H264_B = 1, H264_I = 2 };

typedef struct H264Job {
    const H264Sps *sps;
    const H264Pps *pps;
    const H264Slice *sl;
    const Slot *slots;
    int nslots, cur, cur_poc, first_slice;
    int list[2][32], len[2];                        /* slot indices */
    const uint8_t *nal;                             /* the slice NAL, from its header byte */
    int nal_len;
} H264Job;

/* ---------------- HEVC ---------------- */
typedef struct HevcRps { int num_neg, num_pos, d0[16], d1[16], u0[16], u1[16]; } HevcRps;

typedef struct HevcSps {
    int ok, chroma_format_idc, width, height, conf_l, conf_r, conf_t, conf_b, bit_depth, bit_depth_c;
    int log2_max_poc_lsb, max_dec_pic_buffering, max_num_reorder;
    int log2_min_cb, log2_diff_cb, log2_min_tb, log2_diff_tb, depth_inter, depth_intra;
    int amp, sao, pcm, pcm_bd, pcm_bd_c, log2_min_pcm, log2_diff_pcm, pcm_lf_disabled;
    int num_rps; HevcRps rps[65];
    int temporal_mvp, strong_intra;
} HevcSps;

typedef struct HevcPps {
    int ok, sps_id, dependent_slices, output_flag, extra_bits, sign_hiding, cabac_init_present, num_ref_l0, num_ref_l1;
    int init_qp, constrained_intra, transform_skip, cu_qp_delta, diff_cu_qp_delta_depth, cb_qp, cr_qp;
    int slice_chroma_qp_offsets, weighted_pred, weighted_bipred, transquant_bypass, tiles, wpp, lf_across_slices;
    int deblock_override_enabled, deblock_disabled, beta, tc, lists_mod, log2_pml, slice_ext;
} HevcPps;

typedef struct HevcSlice {
    int nal_type, tid, first, address, type, pps_id, poc_lsb, sao_luma, sao_chroma, temporal_mvp, num_ref[2];
    int mvd_l1_zero, cabac_init, col_from_l0, col_ref_idx, max_merge, qp_delta, cb_qp, cr_qp;
    int deblock_disabled, beta, tc, lf_across;
    int mod[2], list_entry[2][16];
    HevcRps rps;
    int luma_denom, chroma_denom;
    int8_t dlw[2][16], lo[2][16], dcw[2][16][2], co[2][16][2];
    int num_ep, ep[1024];                           /* entry_point_offset_minus1 as coded */
    int header_bits, data_byte;                     /* RBSP bits before byte_alignment(); RBSP byte of slice data */
} HevcSlice;

enum { HEVC_B = 0, HEVC_P = 1, HEVC_I = 2 };

typedef struct HevcJob {
    const HevcSps *sps;
    const HevcPps *pps;
    const HevcSlice *sl;
    const Slot *slots;
    int nslots, cur, cur_poc, first_slice;
    int l0[16], l1[16];                             /* slot indices */
    const uint32_t *ep_unescaped;                   /* entry points as unescaped-byte offsets - 1 (num_ep) */
    const uint8_t *nal;
    int nal_len;
} HevcJob;

/* ---------------- VP8 ---------------- */
typedef struct Vp8Entropy {
    uint8_t coeff[4][8][3][11], y_mode[4], uv_mode[3], mv[2][19];
} Vp8Entropy;

typedef struct Vp8Hdr {
    int key, version, show, first_part_size, header_size;    /* header_size: the uncompressed bytes, 3 or 10 */
    int width, height, hscale, vscale;
    int seg_enabled, seg_update_map, seg_update_data, seg_abs;
    int8_t seg_q[4], seg_lf[4];
    uint8_t seg_probs[3];
    int filter_simple, lf_level, sharpness, lf_delta_enabled, lf_delta_update;
    int8_t ref_delta[4], mode_delta[4];
    int log2_parts;
    uint32_t part_size[8];
    int y_ac_qi, y_dc, y2_dc, y2_ac, uv_dc, uv_ac;
    int refresh_golden, refresh_alt, refresh_last, copy_to_golden, copy_to_alt, sign_golden, sign_alt;
    int no_coeff_skip, prob_skip, prob_intra, prob_last, prob_gf;
    Vp8Entropy e;                                   /* after this frame's updates */
    int coder_range, coder_value, coder_bits, header_bits;   /* boolean decoder at the end of the header (V4L2) */
} Vp8Hdr;

typedef struct Vp8Job {
    const Vp8Hdr *h;
    const Slot *slots;
    int nslots, cur, last, golden, alt;             /* slot indices; references are -1 before the first key frame */
    const uint8_t *data;                            /* the whole frame, from its 3-byte tag */
    int len;
} Vp8Job;

#endif
