"""Reads FFmpeg trace_headers output on stdin, prints one line per slice: slice header size in bits incl. NAL header,
up to (not including) H.264's cabac_alignment_one_bit / HEVC's byte_alignment() bits, as the VE is given it."""
import re
import sys

FIELD = re.compile(r"\]\s+(\d+)\s+(\S+)\s+([01]+) = ")
SKIP = {"cabac_alignment_one_bit", "alignment_bit_equal_to_one", "alignment_bit_equal_to_zero"}
in_slice, end = False, 0
for line in sys.stdin:
    m = FIELD.search(line)
    if m:
        if in_slice and m.group(2) not in SKIP:
            end = max(end, int(m.group(1)) + len(m.group(3)))
        continue
    if "]" not in line:
        continue
    title = line.split("]", 1)[1].strip()
    if not title or title.startswith(("Packet", "nal_unit_type", "size=")):
        continue
    if in_slice:
        print(end)
    in_slice, end = title in ("Slice Header", "Slice Segment Header"), 0
if in_slice:
    print(end)
