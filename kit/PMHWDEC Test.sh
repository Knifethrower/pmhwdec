#!/bin/bash
# libpmhwdec test kit launcher (PortMaster ports folder): tests the device's hardware video decoder and writes
# pmhwdec-report.txt next to this script. Nothing is installed or changed. Takes about a minute.
K="$(cd "$(dirname "$0")" && pwd)/pmhwdec-test"
printf "\033c" > /dev/tty1 2>/dev/null
echo "Testing the video decoder, about a minute..." > /dev/tty1 2>/dev/null
sh "$K/run.sh" "$K/../pmhwdec-report.txt"
echo "Done: pmhwdec-report.txt is in the ports folder." > /dev/tty1 2>/dev/null
sleep 3
