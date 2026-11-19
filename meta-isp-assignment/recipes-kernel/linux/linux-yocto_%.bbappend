FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += "file://vimc.cfg"
KERNEL_CONFIG_FRAGMENTS += "${WORKDIR}/vimc.cfg"KERNEL_MODULE_AUTOLOAD += "vimc"
