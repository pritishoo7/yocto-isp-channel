SUMMARY = "Track Q libcamera Software ISP Application"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

DEPENDS = "libcamera pkgconfig-native"
RDEPENDS:${PN} = "libcamera ffmpeg"

FILESEXTRAPATHS:prepend := "${THISDIR}/files:${TOPDIR}/../src:"

SRC_URI = " \
    file://CMakeLists.txt \
    file://main.cpp \
    file://isp-app.service \
"

S = "${WORKDIR}"

inherit cmake systemd

SYSTEMD_SERVICE:${PN} = "isp-app.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

do_install:append() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/isp-app.service ${D}${systemd_system_unitdir}/isp-app.service
}