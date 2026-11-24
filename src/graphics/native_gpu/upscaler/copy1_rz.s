// Native upscaler image->image copy of a LINEAR view depth into reversed-Z device depth (copy1_rz): source T#
// s[8:15] (table +0), destination T# s[24:31] (table +32), as copy1. d = near / max(z, near), with near 0.1 (the
// profile's camera near plane, taa_near; HLX and frame generation get the same near plane and infinite far), and
// z <= 0 (nothing drawn) at the far plane, d = 0. Marvel's Wolverine's TAA reads view depth in world units (16_FLOAT).
// user SGPRs: s[0:1] table, s2 width, s3 height, s4 unused; s5 TGID_X, s6 TGID_Y.
  s_load_dwordx8 s[8:15], s[0:1], 0x0
  s_load_dwordx8 s[24:31], s[0:1], 0x20
  v_and_b32 v1, 7, v0
  v_lshrrev_b32 v2, 3, v0
  v_lshl_add_u32 v1, s5, 3, v1
  v_lshl_add_u32 v2, s6, 3, v2
  v_cmp_gt_u32 vcc, s2, v1
  v_cmp_gt_u32 s[20:21], s3, v2
  s_and_b64 vcc, vcc, s[20:21]
  s_and_saveexec_b64 s[22:23], vcc
  s_cbranch_execz .Lend
  s_waitcnt lgkmcnt(0)
  image_load v4, v[1:2], s[8:15] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
  s_waitcnt vmcnt(0)
  v_max_f32 v5, 0x3dcccccd, v4
  v_rcp_f32 v5, v5
  v_mul_f32 v5, 0x3dcccccd, v5
  v_cmp_lt_f32 vcc, 0, v4
  v_cndmask_b32 v4, 0, v5, vcc
  image_store v4, v[1:2], s[24:31] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
.Lend:
  s_endpgm
