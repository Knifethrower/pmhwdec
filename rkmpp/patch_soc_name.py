"""Patches MPP's osal/mpp_soc.c read_soc_name(): keep only plain SoC compatibles ("rockchip,rk3399") before the chip
table is searched. Some handheld device trees put a board string naming another SoC first; the RG552 (AmberELEC)
has "rockchip,rk3326-rg552-linux rockchip,rk3399", which MPP matched as an RK3326 (HEVC then fails: no hevc_service).
usage: python3 patch_soc_name.py <mpp source dir>   (idempotent)"""
import pathlib
import sys

path = pathlib.Path(sys.argv[1]) / "osal" / "mpp_soc.c"
src = path.read_text(encoding="utf-8")
MARK = "/* pmhwdec: plain SoC compatibles only */"
if MARK in src:
    print("mpp_soc.c already patched")
    sys.exit(0)
anchor = '            sys_dbg_platform("chip name: %s\\n", name);'
assert src.count(anchor) == 1, "anchor not found: MPP changed, check read_soc_name()"
insert = """            %s
            {
                char keep[256] = "", tmp[256], *tok, *save = NULL;

                snprintf(tmp, sizeof(tmp), "%%s", name);
                for (tok = strtok_r(tmp, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
                    if (!strncmp(tok, "rockchip,rk", 11) && !strchr(tok, '-') &&
                        strlen(keep) + strlen(tok) + 2 < sizeof(keep)) {
                        if (keep[0])
                            strcat(keep, " ");
                        strcat(keep, tok);
                    }
                }
                if (keep[0])
                    snprintf(name, size, "%%s", keep);
            }
""" % MARK
src = src.replace(anchor, insert + anchor)
path.write_text(src, encoding="utf-8", newline="\n")
print("mpp_soc.c patched")
