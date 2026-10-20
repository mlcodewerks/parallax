#ifndef RDP_EMIT_RSP_H
#define RDP_EMIT_RSP_H

#include <stdint.h>
void rsp_clip_spill_words(uint32_t on_rgba, uint32_t on_st,
                          uint32_t off_rgba, uint32_t off_st, uint32_t words[4]);
void rsp_set_clip_lerp_gbi1(void);
void rsp_vertex_spill_reset(void);
void rsp_vertex_spill_position(int32_t z, int ratio, int32_t matrix_z);
int rsp_vertex_spill_take(uint32_t words[4]);
void rsp_vertex_spill_cull(int y0,int y1,int y2,int degenerate);
void rsp_vertex_spill_screen(unsigned int vertex,int32_t x,int32_t y,int32_t z);
void rsp_set_vtx_z_signed(int on);
void rsp_vertex_spill_clip(int32_t x,int32_t y,int32_t z);
void rsp_rej_project(unsigned int vertex,unsigned int alpha);
void rsp_rej_prologue(uint32_t words[4], unsigned char alpha[2]);
void rsp_four_pair_begin(void);
int rsp_s2dex_rect_axis(int obj, int scale, int extent, int base_scale,
                        int trans, const int16_t correction[6], int flip,
                        int clip_min, int clip_max, int frac_scale, int truncate, int out[4]);
void rsp_s2dex_sprite_triangle(int32_t packet[24], const int16_t h[4],
    const int16_t m[4], const int16_t l[4], unsigned int tile);
void rsp_s2dex_sprite_corners(int16_t out[4][4],const int obj[6],
    const int32_t matrix[6],unsigned int mode,unsigned int flags);
int rsp_tri_take_base_spill(unsigned int *packet_words, unsigned int *offset,
                             int32_t words[16]);

int32_t rsp_rcp32(int32_t in32);
void rsp_graphics_entry_carry(unsigned int carry);
void rsp_graphics_sdk_boot(int on);
void rsp_vertex_pair_half(unsigned int half);
int rsp_graphics_sdk_one(unsigned int lane);
void rsp_sdk_triangle_xl(const int32_t *x);
int32_t rsp_rcp32_dp(int32_t in32);
int32_t rsp_rcp16(int32_t in16);
int32_t rsp_rs_billboard_size(int32_t size, int32_t invw, int32_t norm, int32_t viewport, int32_t projection);
int32_t rsp_rs_billboard_step(int32_t size, int32_t extent, uint16_t fraction);
int32_t rsp_rsq32(int32_t in32);
void rsp_light_dir_xfrm_one(const int32_t mv[4][4],
                            const int32_t dir[3], int32_t out[3]);
void rsp_light_dir_xfrm_four(const int32_t mv[4][4],const int32_t dir[3],int32_t out[4]);
void rsp_light_dir_xfrm_legacy(const int32_t mv[4][4],const int32_t dir[3],int32_t out[3]);
void rsp_light_dir_xfrm_cached(const int32_t mv[4][4],const int32_t dir[3],int32_t out[4]);
void rsp_light_vtx_legacy(const int32_t normal[3],const int32_t ambient[3],
    const int32_t (*rgb)[3],const int32_t (*dirs)[3],int num,int32_t out[3]);
void rsp_zsort_light_xfrm(const uint16_t mi[3][4], const uint16_t mf[3][4],
                          const int32_t dir[3], unsigned char out[4]);
int32_t rsp_clip_scale_w(int32_t w, int ratio);
int32_t rsp_dkr_clip_scale_w(int32_t w, int ratio);
void rsp_set_clip_lerp_204h(int on);
void rsp_set_vtx_invw_2rd(int on);
void rsp_set_tri_attr_rs(int on);
void rsp_set_tri_attr_dkr(int on);
void rsp_tri_factor5_context(uint32_t values[5]);
void rsp_tri_factor5_seed_fraction(uint32_t value);
/* Arithmetic latches consumed when Factor 5 skips perspective staging.
 * The native RSP snapshot owns their lifetime across audio and save/load. */
void rsp_factor5_seed(const uint16_t values[32][8]);
uint32_t rsp_factor5_get(uint16_t values[32][8]);
void rsp_factor5_row(unsigned reg, const uint16_t values[8]);
void rsp_factor5_lane(unsigned reg,unsigned lane,uint16_t value);
uint32_t rsp_factor5_bound_integer(void);
uint32_t rsp_factor5_bound_fraction(void);
void rsp_factor5_project_pair(const int32_t position[4], unsigned half);
void rsp_factor5_texture_bound(uint32_t integer, uint32_t fraction);
void rsp_factor5_spare_bound(uint32_t integer,uint32_t fraction);
void rsp_factor5_checkpoint(int restore);
void rsp_factor5_clip_residue(const int32_t in4[4],const int32_t out4[4],
    const int16_t plane[4],const uint16_t constants[8],int32_t upper[4]);
void rsp_factor5_reject(const int16_t xy[6]);
void rsp_set_vtx_invw_raw(int on);
void rsp_set_vtx_y_round(int on);
void rsp_set_vtx_x_round(int on);
void rsp_set_vtx_z_quant(int on);
void rsp_set_keep_degenerate(int on);
void rsp_set_affine_tex(int on);
void rsp_set_persp_skip(int on);
int32_t rsp_z_gradient_min(int32_t gradient, unsigned int minimum);
void rsp_set_attr_lowp(int on);
void rsp_set_clip_lerp_wo64(int on);
void rsp_set_clip_lerp_l3dex(int on);
void rsp_set_clip_lerp_early(void);
void rsp_clip_lerp(const int32_t on_pos[4], const int32_t off_pos[4],
                   const int16_t cr[4],
                   const int16_t on_attr[8], const int16_t off_attr[8],
                   int32_t out_pos[4], int16_t out_attr[8]);
void rsp_texgen(const int32_t n[3], const int32_t l0[3], const int32_t l1[3],
                int linear, int32_t *s_out, int32_t *t_out);
int32_t rsp_light_dirdot(const int32_t n[3], const int32_t d[3]);
void rsp_light_fold1(int32_t lt[3], const int32_t rgb[3], int32_t d);
int32_t rsp_light_point_factor(const int32_t mv[4][4], const int32_t n[3],
                               const int32_t vtx[3], const int32_t pos[3],
                               int32_t kc, int32_t kl, int32_t kq);
void rsp_light_vtx(const int32_t n[3], const int32_t amb[3],
                   const int32_t (*rgb)[3], const int32_t (*dirs)[3],
                   int num, int32_t out[3]);
void rsp_cbfd_light(const int32_t clip[4], const int32_t coord[16],
    const int32_t (*positions)[4], const int32_t (*rgb)[3],
    const int32_t *attenuation, int num, const int32_t source[3], int32_t out[3]);
void rsp_cbfd_light_normal(const int32_t clip[4], const int32_t coord[16],
    const int32_t (*positions)[4], const int32_t (*rgb)[3],
    const int32_t *attenuation, int num, const int32_t source[3],
    const int32_t normal[3], const int32_t (*directions)[3], int positional, int32_t out[3]);
int32_t rsp_vtx_invw(int32_t w);
int32_t rsp_vtx_fog(int32_t cz, int32_t cw, int32_t pn,
                    int32_t fog_m, int32_t fog_o);
int32_t rsp_vtx_fog_dkr(int32_t ndcz, int32_t fog_m, int32_t fog_o);
int32_t rsp_dkr_add32(int32_t a, int32_t b);
void rsp_vtx_screen_dkr(int32_t cx, int32_t cy, int32_t cz, int32_t cw,
    int32_t pn, int32_t vsx, int32_t vsy, int32_t vsz,
    int32_t vtx, int32_t vty, int32_t vtz,
    int32_t *sx, int32_t *sy, int32_t *sz, int32_t *iw, int32_t *ndcz);
int32_t rsp_vtx_last_ndc2z(void);
int32_t rsp_vtx_last_pw(void);
int32_t rsp_vtx_scaled_w(int32_t w, int32_t persp_norm);
int32_t rsp_vtx_last_outcode(void);
void rsp_clip_weights_rs(const int32_t in4[4], const int32_t out4[4],
                         const int16_t P[4], int32_t *wc, int32_t *wt);
int32_t rsp_clip_blend32_rs(int32_t a, int32_t b, int32_t wc, int32_t wt);
int32_t rsp_clip_blend16_rs(int32_t a, int32_t b, int32_t wc, int32_t wt);
int32_t rsp_tri_key_rs(int32_t z1, int32_t z2, int32_t z3);
int32_t rsp_interior_blend_rs(int32_t a, int32_t b, int32_t cur,
                              int32_t f2, int32_t g);
int32_t rsp_geomorph_blend_rs(int32_t a, int32_t b, int32_t cur,
                              int32_t wa, int32_t wb,
                              int32_t f, int32_t g);
int32_t rsp_fog_rs(int32_t sz1616,
                   int32_t m_i, int32_t m_f,
                   int32_t o_i, int32_t o_f, int32_t k);
int rsp_vtx_screen_rs(int32_t cx, int32_t cy, int32_t cz, int32_t cw,
                      int32_t pn,
                      const int32_t *vs, const int32_t *vt,
                      int32_t *sx102, int32_t *sy102, int32_t *sz1616,
                      int32_t *invw_out);
int rsp_vtx_screen(int32_t cx, int32_t cy, int32_t cz, int32_t cw,
                   int32_t pn,
                   int32_t vsx, int32_t vsy, int32_t vsz,
                   int32_t vtx_, int32_t vty, int32_t vtz,
                   int32_t *sx102, int32_t *sy102, int32_t *sz1616,
                   int32_t *invw_out);

typedef struct RspTriVtx
{
    int16_t  x, y;      /* screen position, 10.2 */
    int32_t  z;         /* screen z, 16.16 */
    int32_t  r, g, b, a;/* 8-bit colour values */
    int32_t  s, t;      /* texture coordinates as stored in VTX_TC_VEC */
    int32_t  invw;      /* VTX_INV_W 32-bit value (rsp_vtx_invw) */
    int32_t  pw;        /* perspNorm'd w (the divide input; Rogue
                           Squadron's texture normalizer folds these) */
    int      flat2d;    /* raw attribute path: skip the perspective
                           normalizer and colour rounding (Fighting
                           Force 64's 2D overlay microcode) */
} RspTriVtx;

/* dx_scale/idy_scale/frac_mask/vcr_bound are the per-microcode triangle
 * setup constants; see GSPState tri_dx_scale and friends. */
int rsp_line_native(int32_t *cmd,const RspTriVtx *a,const RspTriVtx *b,
    int width,unsigned int flags,unsigned int tile,const int32_t scissor[2]);
int rsp_line_write(int32_t *cmd, const RspTriVtx *e0, const RspTriVtx *e1,
                   int width_q, int32_t dx_scale, int32_t idy_scale,
                   int32_t slope_mask, int32_t *xl_dmem, int zbuf);
void rsp_tri_set_rs_sort(int on);
void rsp_tri_set_zboss_attr(int on);
void rsp_tri_set_d64_sort(int on);
int rsp_tri_early_writer(void);
int rsp_tri_write(int32_t *ew,
                  const RspTriVtx *v1c, const RspTriVtx *v2c,
                  const RspTriVtx *v3c,
                  int textured, int z_buffered, int shaded, int smooth,
                  int tile, int level,
                  int32_t dx_scale, int32_t idy_scale,
                  int32_t frac_mask, int32_t vcr_bound);
/* Verified FIFO DIV normalization also selects its latched gradient MACs. */
int rsp_tri_write_with_texrecip(int32_t *ew,
                  const RspTriVtx *v1c, const RspTriVtx *v2c,
                  const RspTriVtx *v3c,
                  int textured, int z_buffered, int shaded, int smooth,
                  int tile, int level, int32_t dx_scale, int32_t idy_scale,
                  int32_t frac_mask, int32_t vcr_bound, int native_texrecip);

#endif
