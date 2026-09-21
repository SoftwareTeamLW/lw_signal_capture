#!/usr/bin/env bash
set -u

echo "=== LW39X0 PCIe permission diagnostic ==="
echo "user: $(id)"
echo

echo "Candidate device nodes:"
for pattern in /dev/lw* /dev/uio* /dev/vfio/* /dev/xdma* /dev/dma*; do
    for node in $pattern; do
        [ -e "$node" ] || continue
        ls -l "$node"
    done
done

echo
echo "PCI devices / bound drivers:"
if command -v lspci >/dev/null 2>&1; then
    lspci -nnk
else
    echo "lspci not installed (package: pciutils)"
fi

echo
echo "Interpretation:"
echo "- If sudo can open pcie_0 but the normal user cannot, do not run Qt Creator as root."
echo "- Identify the exact /dev node used by the LW kernel driver and grant that node to a dedicated group with a udev rule."
echo "- If the SDK maps /dev/mem directly, the vendor driver must expose a safer device interface or explicitly document the required capability."
