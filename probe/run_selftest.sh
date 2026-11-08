#!/bin/sh
# Device side: the startup self-test fresh, cached, and with a deliberate error (must fall back to "no hardware"),
# then the clip suite (run_lib.sh). Leaves no self-test cache behind. Usage: sh run_selftest.sh <dir>
D=${1:-$(dirname "$0")}
if [ "$(id -u)" != 0 ] && [ -z "$PMHW_NO_SUDO" ] && sudo -n true 2>/dev/null; then exec sudo -n sh "$0" "$@"; fi
C=/tmp/pmhwdec-selftest.$(id -u)
ms() { date +%s%N | cut -c1-13; }
rm -f "$C"
echo "######## fresh"
t=$(ms); PMHWDEC_LOG=1 "$D/pmhw-test" -q 2>&1 | grep -vE "^pmhwdec: cedar: [0-9]|engine"; echo "took $(( $(ms) - t )) ms"
echo "######## cached"
t=$(ms); "$D/pmhw-test" -q 2>&1; echo "took $(( $(ms) - t )) ms"; cat "$C"
echo "######## injected error (+10 on every block mean): expect no hardware"
PMHWDEC_SELFTEST=1 PMHWDEC_SELFTEST_INJECT=10 "$D/pmhw-test" -q 2>&1
rm -f "$C"
echo "######## clip suite"
sh "$D/run_lib.sh" "$D" 2>&1 | grep -vE "^    pmhwdec: (cedar: picture buffers|self-test)"
rm -f "$C"
