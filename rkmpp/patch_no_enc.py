"""Patches MPP so librockchip_mpp is built without its encoder framework: build_mpp.sh already turns every encoder
codec off (ENABLE_H264E=OFF ...), but the encoder core still links in (mpp_enc_cfg's config trie, mpp_enc_impl's
threads, the rate-control models, the vepu HAL commons, kmpp's kernel-venc path: ~150 KB). Those sources leave the
CMake lists and mpp/codec/pmhwdec_noenc.c stands in for the 12 symbols the decoder side still names (all reached only
for an MPP_CTX_ENC context): mpp_init(MPP_CTX_ENC) fails with MPP_NOK, decoding is untouched.
build_mpp.sh links with -z defs, so a symbol this misses breaks the build instead of the dlopen().
usage: python3 patch_no_enc.py <mpp source dir>   (idempotent)"""
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
MARK = "# pmhwdec: no encoder (rkmpp/patch_no_enc.py)"

STUB = r"""/* pmhwdec (rkmpp/patch_no_enc.py): librockchip_mpp without the encoder framework. These stand in for the symbols
 * the decoder side still references; each is reached only for an MPP_CTX_ENC context. */
#include "rk_mpi.h"
#include "mpp_enc.h"
#include "rk_venc_cfg.h"
#include "kmpp.h"

MPP_RET mpp_enc_init_v2(MppEnc *ctx, MppEncInitCfg *cfg)
{
    (void)cfg;
    if (ctx)
        *ctx = NULL;
    return MPP_NOK;
}
MPP_RET mpp_enc_deinit_v2(MppEnc ctx) { (void)ctx; return MPP_OK; }
MPP_RET mpp_enc_start_v2(MppEnc ctx) { (void)ctx; return MPP_NOK; }
MPP_RET mpp_enc_start_async(MppEnc ctx) { (void)ctx; return MPP_NOK; }
MPP_RET mpp_enc_stop_v2(MppEnc ctx) { (void)ctx; return MPP_OK; }
MPP_RET mpp_enc_control_v2(MppEnc ctx, MpiCmd cmd, void *param) { (void)ctx; (void)cmd; (void)param; return MPP_NOK; }
MPP_RET mpp_enc_notify_v2(MppEnc ctx, RK_U32 flag) { (void)ctx; (void)flag; return MPP_NOK; }
MPP_RET mpp_enc_reset_v2(MppEnc ctx) { (void)ctx; return MPP_NOK; }
MppEncCfg mpp_enc_to_cfg(MppEnc ctx) { (void)ctx; return NULL; }
MPP_RET mpp_enc_cfg_get_s32(MppEncCfg cfg, const char *name, RK_S32 *val)
{
    (void)cfg; (void)name; (void)val;
    return MPP_NOK;
}
MPP_RET mpp_enc_cfg_extract(MppEncCfg cfg, MppCfgStrFmt fmt, char **buf)
{
    (void)cfg; (void)fmt;
    if (buf)
        *buf = NULL;
    return MPP_NOK;
}

/* kmpp.c picks this for a kernel-venc context (mpp.c only builds one for an encoder with an init kcfg) */
static MPP_RET pmhwdec_kmpp_venc_init(Kmpp *ctx, MppCtxType type, MppCodingType coding)
{
    (void)ctx; (void)type; (void)coding;
    return MPP_NOK;
}
KmppOps kmpp_venc_obj_ops = { .init = pmhwdec_kmpp_venc_init };
"""

# (file, old, new): each old must occur exactly once before patching
EDITS = [
    ("mpp/base/CMakeLists.txt",
     "add_library(mpp_base OBJECT\n    mpp_enc_refs.c\n    mpp_enc_ref.c\n    mpp_enc_cfg.c\n",
     "%s\nadd_library(mpp_base OBJECT\n" % MARK),
    ("mpp/codec/CMakeLists.txt",
     "add_library(mpp_codec STATIC\n    mpp_enc_impl.c\n    mpp_enc_v2.c\n    enc_impl.c\n",
     "%s\nadd_library(mpp_codec STATIC\n    pmhwdec_noenc.c\n" % MARK),
    ("mpp/codec/CMakeLists.txt", "add_library(mpp_rc OBJECT mpp_rc.c)\n", ""),
    ("mpp/codec/CMakeLists.txt", "add_subdirectory(enc)\n", ""),
    ("mpp/codec/CMakeLists.txt", "add_subdirectory(rc)\n", ""),
    ("mpp/codec/CMakeLists.txt", "target_link_libraries(mpp_codec\n    mpp_rc\n    enc_rc\n",
     "target_link_libraries(mpp_codec\n"),
    ("mpp/hal/CMakeLists.txt", "add_subdirectory(rkenc)\n", "%s\n" % MARK),
    ("mpp/hal/CMakeLists.txt", "add_library(mpp_hal STATIC\n    mpp_enc_hal.c\n", "add_library(mpp_hal STATIC\n"),
    ("mpp/hal/CMakeLists.txt", "    hal_vepu5xx_common\n    hal_vepu_common\n", ""),
    ("kmpp/CMakeLists.txt", "    kmpp_venc.c\n    kmpp_venc_obj.c\n", "    %s\n" % MARK),
    ("kmpp/base/CMakeLists.txt", "    kmpp_venc_cfg.c\n", "    %s\n" % MARK),
]

stub = root / "mpp" / "codec" / "pmhwdec_noenc.c"
if all(MARK in (root / f).read_text(encoding="utf-8") for f, _, _ in EDITS) and stub.exists():
    stub.write_text(STUB, encoding="utf-8", newline="\n")   # keep the stub current
    print("encoder framework already patched out")
    sys.exit(0)
texts = {f: (root / f).read_text(encoding="utf-8") for f, _, _ in EDITS}
assert not any(MARK in t for t in texts.values()), "half-patched tree: git -C <mpp> checkout -- the CMakeLists.txt files"
for f, old, new in EDITS:
    assert texts[f].count(old) == 1, "%s: %r not found: MPP changed, check the encoder CMake lists" % (f, old)
    texts[f] = texts[f].replace(old, new)
for f, t in texts.items():
    (root / f).write_text(t, encoding="utf-8", newline="\n")
stub.write_text(STUB, encoding="utf-8", newline="\n")
print("encoder framework patched out")
