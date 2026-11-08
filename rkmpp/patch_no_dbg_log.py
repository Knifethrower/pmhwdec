"""Patches MPP's osal/inc/mpp_debug.h: with -DPMHW_NO_DBG_LOG, mpp_dbg() / mpp_dbg_f() compile to nothing. They are the
env-gated trace logs (mpp_debug=, h264d_debug=, hal_h265d_debug= ... set to a flag mask); every module's xxx_dbg()
macro ends in them. Errors and warnings (mpp_err, mpp_log, mpp_logw) stay. None of the H.264 / HEVC / HAL / osal
call sites has a side effect in its arguments (checked on 1.1.0: only vp9d_parser.c, not built). Saves ~55 KB.
build_mpp.sh passes the define unless MPP_DBG_LOG=1 (a build for chasing an MPP problem on a device).
usage: python3 patch_no_dbg_log.py <mpp source dir>   (idempotent)"""
import pathlib
import sys

path = pathlib.Path(sys.argv[1]) / "osal" / "inc" / "mpp_debug.h"
src = path.read_text(encoding="utf-8")
MARK = "/* pmhwdec: PMHW_NO_DBG_LOG (rkmpp/patch_no_dbg_log.py) */"
if MARK in src:
    print("mpp_debug.h already patched")
    sys.exit(0)
anchor = """#define mpp_dbg(debug, flag, fmt, ...)     mpp_log_c((debug) & (flag), fmt, ## __VA_ARGS__)
#define mpp_dbg_f(debug, flag, fmt, ...)   mpp_log_cf((debug) & (flag), fmt, ## __VA_ARGS__)
"""
assert src.count(anchor) == 1, "anchor not found: MPP changed, check osal/inc/mpp_debug.h"
patched = """%s
#ifdef PMHW_NO_DBG_LOG
#define mpp_dbg(debug, flag, fmt, ...)     do {} while (0)
#define mpp_dbg_f(debug, flag, fmt, ...)   do {} while (0)
#else
%s#endif
""" % (MARK, anchor)
src = src.replace(anchor, patched)
path.write_text(src, encoding="utf-8", newline="\n")
print("mpp_debug.h patched")
