"""Patches MPP's mpp/soc.cmake: without MPP_SOC it turns on every decoder/encoder IP (HAVE_VDPU1 ... HAVE_VEPU720),
and each one adds its HAL files (mpp/hal/rkdec/h264d/CMakeLists.txt etc.). MPP_SOC takes only one chip, so this adds
MPP_HW_LIST (comma list of MPP_HW_TYPES names): when set, only those IPs are on. The HALs left out register nothing,
so MPP behaves the same on the chips whose HALs stay (each HAL lists its SoCs in .soc_type; build_mpp.sh says which).
usage: python3 patch_hw_list.py <mpp source dir>   (idempotent)"""
import pathlib
import sys

path = pathlib.Path(sys.argv[1]) / "mpp" / "soc.cmake"
src = path.read_text(encoding="utf-8")
MARK = "# pmhwdec: MPP_HW_LIST"
if MARK in src:
    print("soc.cmake already patched")
    sys.exit(0)
anchor = """else()
    # Fallback: enable all if no SOC specified
    soc_enable_hw(${MPP_HW_TYPES})
"""
assert src.count(anchor) == 1, "anchor not found: MPP changed, check mpp/soc.cmake"
patched = """else()
    # Fallback: enable all if no SOC specified
    %s (pmhwdec rkmpp/patch_hw_list.py): only the listed hardware
    if (MPP_HW_LIST)
        string(REPLACE "," ";" pmhw_hw_list "${MPP_HW_LIST}")
        foreach(hw ${pmhw_hw_list})
            list(FIND MPP_HW_TYPES ${hw} pmhw_hw_found)
            if (pmhw_hw_found LESS 0)
                message(FATAL_ERROR "MPP_HW_LIST: unknown hardware ${hw}")
            endif()
        endforeach()
        message(STATUS "MPP_HW_LIST: ${pmhw_hw_list}")
        soc_enable_hw(${pmhw_hw_list})
    else()
        soc_enable_hw(${MPP_HW_TYPES})
    endif()
""" % MARK
src = src.replace(anchor, patched)
path.write_text(src, encoding="utf-8", newline="\n")
print("soc.cmake patched")
