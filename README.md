# Embedded Linux Camera & Software ISP Channel (`Track Q — qemuarm64`)

A reproducible **Yocto Linux (Scarthgap 5.0.21)** image and **C++17 `libcamera` + Software ISP** pipeline built for `qemuarm64`. The system boots into a `systemd`-managed environment, captures 1080p frames from the kernel `vimc` virtual camera via `libcamera` (zero direct V4L2 calls), processes RAW Bayer frames through a 5-stage C++ Software ISP, encodes continuous MPEG-4 video (`/var/lib/isp-app/output.mp4`), and supports on-demand PNG and RAW Bayer snapshots.

---

## 1. System Architecture

```text
+-----------------------------------------------------------------------------------+
|                        QEMU ARM64 Target (2048 MB RAM, 4 vCPU)                    |
|                                                                                   |
|  +-----------------------+     +-----------------------------------------------+  |
|  | Linux Kernel (6.6)    |     | Userspace: isp-app (systemd auto-start)       |  |
|  |  vimc Virtual Sensor  |     |                                               |  |
|  |  (Sensor B, 1080p)    |     |  1. libcamera C++ API (CameraManager)         |  |
|  +-----------+-----------+     |     - ControlInfoMap (Brightness/Contrast)    |  |
|              |                 |     - FrameBufferAllocator (4 DMABUFs)        |  |
|              | DMABUF fd       +-----------------------+-----------------------+  |
|              +----------------> [Zero-Copy mmap()]     |                          |
|                                                        v                          |
|                                +-----------------------------------------------+  |
|                                | Copy #1: RGGB RAW Bayer Mosaic Synthesizer    |  |
|                                |          (1920x1080 8-bit single-channel)     |  |
|                                +-----------------------+-----------------------+  |
|                                                        |                          |
|                                                        v                          |
|                                +-----------------------------------------------+  |
|                                | Copy #2: 5-Stage C++ Software ISP             |  |
|                                |   Stage 1: Black-Level Correction + AE Gain   |  |
|                                |   Stage 2: Gray-World Auto White Balance      |  |
|                                |   Stage 3: Bilinear Demosaic (Bayer -> RGB)   |  |
|                                |   Stage 4: 3x3 CCM + Brightness / Contrast    |  |
|                                |   Stage 5: Gamma 2.2 Lookup Table (LUT)       |  |
|                                +-----------+-----------------------+-----------+  |
|                                            |                       |              |
|                       Copy #3 (pipe write) |                       | Trigger      |
|                                            v                       v              |
|                              +--------------------+   +------------------------+  |
|                              | ffmpeg (mpeg4)     |   | Snapshot Handler       |  |
|                              | /var/lib/isp-app/  |   | (SIGUSR1 / /tmp/snap)  |  |
|                              |   output.mp4       |   |  - snapshot_<N>.png    |  |
|                              +--------------------+   |  - snapshot_raw_<N>.pgm|  |
|                                                       +------------------------+  |
+-----------------------------------------------------------------------------------+
```

---

## 2. Repository Structure

```text
yocto-isp-channel/
├── kas-qemu.yml                         # Reproducible kas build config (Scarthgap 5.0.21)
├── run-qemu.sh                          # QEMU launch script (2048 MB RAM, 4 vCPUs, SSH port 2222)
├── src/
│   ├── CMakeLists.txt                   # CMake build configuration for isp-app
│   └── main.cpp                         # libcamera capture + 5-stage Software ISP + encoder/snapshot
├── meta-isp-assignment/
│   ├── conf/layer.conf                  # Custom Yocto layer definition
│   ├── recipes-core/images/
│   │   └── isp-image.bb                 # Custom image extending core-image-minimal
│   ├── recipes-kernel/linux/
│   │   ├── linux-yocto_%.bbappend       # Applies vimc kernel config fragment
│   │   └── files/vimc.cfg               # Enables CONFIG_VIDEO_VIMC=y
│   └── recipes-multimedia/
│       ├── libcamera/
│       │   └── libcamera_%.bbappend     # Enables -Dpipelines=vimc -Dipas=vimc
│       └── isp-app/
│           ├── isp-app_1.0.bb           # Builds & installs isp-app + systemd service
│           └── files/isp-app.service    # Auto-starts isp-app at boot
├── artifacts/                           # Verified logs, video output, and PNG/RAW snapshots
├── README.md
└── REPORT.md
```

---

## 3. Build & Boot Instructions

### Prerequisites (Ubuntu 22.04 / WSL 2)
```bash
sudo apt update && sudo apt install -y \
  gawk wget git diffstat unzip texinfo gcc build-essential chrpath socat \
  cpio python3 python3-pip python3-pexpect xz-utils debianutils iputils-ping \
  python3-git python3-jinja2 libegl1-mesa libsdl1.2-dev pylint xterm \
  python3-subunit mesa-common-dev zstd liblz4-tool file locales qemu-system-arm
pip3 install --user kas
```

### Build the Image
```bash
kas build kas-qemu.yml
```

### Boot in QEMU
```bash
./run-qemu.sh
```
* **Login:** `root` (no password required on serial console)
* **Exit QEMU:** Press `Ctrl + A`, release both keys, then press `X`

---

## 4. Verification & Runtime Usage (Inside QEMU)

`isp-app.service` starts automatically at boot via `systemd`.

### Inspect `systemd` Service & Live FPS Metrics
```bash
systemctl status isp-app.service
journalctl -u isp-app.service -f
```

### Trigger an On-Demand Snapshot (PNG + RAW Bayer PGM)
Either send `SIGUSR1` or create `/tmp/snapshot`:
```bash
# 1. Trigger an immediate PNG + RAW Bayer (.pgm) snapshot
kill -USR1 $(pidof isp-app)
sleep 2

# 2. Verify snapshot files were created
ls -lh /var/lib/isp-app/

# 3. Stop isp-app cleanly so ffmpeg writes the final MP4 header (moov atom)
systemctl stop isp-app.service

# 4. Collect boot log, systemd status, journalctl log, and system metrics
dmesg > /var/lib/isp-app/boot-dmesg.log
uptime > /var/lib/isp-app/stability_report.txt
free -h >> /var/lib/isp-app/stability_report.txt
systemctl status isp-app.service --no-pager >> /var/lib/isp-app/stability_report.txt
journalctl -u isp-app.service --no-pager > /var/lib/isp-app/isp-app-journal.log

```

### Finalize Video & Copy Artifacts to Host
St container header, then pull artifacts via `scp`:
```bash
# Inside QEMU:
systemctl stop isp-app.service
passwd   # Set a temporary password (e.g., root) for SSH transfer

# On Host PC:
 scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -P 2222 root@localhost:/var/lib/isp-app/* ~/yocto-isp-channel/artifacts/
 
```

---

## 5. Memory Copy Audit

| Stage | Source -> Destination | Mechanism | Copy Count |
| :--- | :--- | :--- | :--- |
| **Sensor Capture** | `vimc` kernel buffer -> `isp-app` userspace | `libcamera::FrameBufferAllocator` `DMABUF` fd mapped via `mmap(..., PROT_READ, MAP_SHARED)` | **0 copies (Zero-Copy)** |
| **Bayer Mosaic** | Mapped `DMABUF` (`RGB888`) -> `SoftwareIsp::bayer_` (`RAW8`) | Synthesizes single-channel RGGB Bayer mosaic from sensor output | **Copy #1** |
| **Software ISP** | `SoftwareIsp::bayer_` -> `SoftwareIsp::processed_rgb_` | 5-stage ISP pipeline (BLC, Gray-World AWB, Bilinear Demosaic, 3x3 CCM, Gamma LUT) | **Copy #2** |
| **Video Encoding** | `processed_rgb_` -> `ffmpeg` subprocess `stdin` | `fwrite()` over POSIX pipe to `ffmpeg` (`mpeg4` encoder -> `output.mp4`) | **Copy #3** |
| **Snapshot (On-Demand)** | `bayer_` / `processed_rgb_` -> Flash/Rootfs | Written only when `SIGUSR1` or `/tmp/snapshot` is triggered | **On-Demand Only** |

---

## 6. Performance Benchmarks (`qemuarm64`, 1920x1080)

Measured over a **12+ minute continuous run** (`1,846+` frames) inside QEMU (`-m 2048 -smp 4`, TCG software emulation):

| Metric | Measured Value | Notes |
| :--- | :--- | :--- |
| **Stream Resolution** | `1920 x 1080` (`1080p`) | Validated via `libcamera` `StreamRole::Viewfinder` |
| **Sustained Throughput** | **2.60 FPS** | Steady across 1,846+ continuous frames |
| **Per-Frame ISP Latency** | **365.0 ms / frame** | Pure C++17 scalar execution on 2.07M pixels in QEMU TCG |
| **Application RSS Memory** | **27.1 MB** (peak **27.2 MB**) | Flat memory profile; zero leaks over 12+ minutes |
| **CPU Utilization** | **~18% `isp-app`**, **~20% `vimc-streamer`** | ~55% system idle across 4 virtual cores |
| **V4L2 Calls in App** | **0** | Verified via `grep -rnE "videodev2|VIDIOC_|v4l2-ctl" src/` |
