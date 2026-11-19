SUMMARY = "Track Q Camera ISP Assignment Image"
LICENSE = "MIT"

inherit core-image

IMAGE_FEATURES += "ssh-server-openssh allow-root-login empty-root-password"

IMAGE_INSTALL += " \
    libcamera \
    ffmpeg \
    isp-app \
    iproute2 \
"