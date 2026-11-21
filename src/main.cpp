#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <libcamera/libcamera.h>

using namespace libcamera;
namespace fs = std::filesystem;

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_snapshot_requested{false};

static void handle_sigint(int) { g_running = false; }
static void handle_sigusr1(int) { g_snapshot_requested = true; }

struct IspParams {
    bool awb_enable = true;
    bool ae_enable = true;
    float exposure_gain = 1.05f;
    float black_level = 8.0f;
    float brightness = 0.0f; // [-1.0, 1.0]
    float contrast = 1.0f;   // [0.0, 2.0]
    float gamma = 2.2f;
};

// Persistent row-slice worker pool for zero-overhead per-frame multithreading
class RowThreadPool {
public:
    explicit RowThreadPool(size_t num_threads)
        : num_threads_(std::max<size_t>(1, num_threads)) {
        for (size_t i = 0; i < num_threads_; ++i) {
            workers_.emplace_back([this, i]() { worker_loop(i); });
        }
    }

    ~RowThreadPool() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true;
            ++generation_;
        }
        cv_start_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
    }

    size_t size() const { return num_threads_; }

    // Dispatches [0, total_rows) across num_threads_ workers and waits for completion
    void parallel_rows(uint32_t total_rows, const std::function<void(size_t tid, uint32_t y_start, uint32_t y_end)>& fn) {
        if (num_threads_ <= 1 || total_rows == 0) {
            fn(0, 0, total_rows);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            current_fn_ = &fn;
            total_rows_ = total_rows;
            active_workers_ = num_threads_;
            ++generation_;
        }
        cv_start_.notify_all();

        std::unique_lock<std::mutex> lk(mtx_);
        cv_done_.wait(lk, [this]() { return active_workers_ == 0; });
        current_fn_ = nullptr;
    }

private:
    void worker_loop(size_t tid) {
        uint64_t local_gen = 0;
        while (true) {
            const std::function<void(size_t, uint32_t, uint32_t)>* fn_ptr = nullptr;
            uint32_t rows = 0;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_start_.wait(lk, [this, &local_gen]() { return generation_ != local_gen; });
                local_gen = generation_;
                if (stop_) return;
                fn_ptr = current_fn_;
                rows = total_rows_;
            }

            uint32_t chunk = (rows + static_cast<uint32_t>(num_threads_) - 1) / static_cast<uint32_t>(num_threads_);
            uint32_t y_start = std::min(rows, static_cast<uint32_t>(tid) * chunk);
            uint32_t y_end = std::min(rows, y_start + chunk);
            if (fn_ptr && y_start < y_end) {
                (*fn_ptr)(tid, y_start, y_end);
            }

            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (--active_workers_ == 0) {
                    cv_done_.notify_one();
                }
            }
        }
    }

    size_t num_threads_;
    std::vector<std::thread> workers_;
    std::mutex mtx_;
    std::condition_variable cv_start_;
    std::condition_variable cv_done_;
    uint64_t generation_ = 0;
    size_t active_workers_ = 0;
    uint32_t total_rows_ = 0;
    const std::function<void(size_t, uint32_t, uint32_t)>* current_fn_ = nullptr;
    bool stop_ = false;
};

class SoftwareIsp {
public:
    SoftwareIsp(uint32_t w, uint32_t h, size_t threads = 0)
        : width_(w),
          height_(h),
          pool_(threads > 0 ? threads : std::max<size_t>(1, std::thread::hardware_concurrency())) {
        bayer_.resize(static_cast<size_t>(w) * h);
        isp_work_.resize(static_cast<size_t>(w) * h);
        rebuild_gamma_lut(2.2f);
        std::cout << "SoftwareIsp initialized with " << pool_.size() << " worker threads";
    }

    size_t thread_count() const { return pool_.size(); }

    void rebuild_gamma_lut(float gamma) {
        if (std::abs(gamma - current_gamma_) < 1e-3f) return;
        current_gamma_ = gamma;
        float inv_g = 1.0f / gamma;
        for (int i = 0; i < 256; ++i) {
            float norm = static_cast<float>(i) / 255.0f;
            gamma_lut_[i] = static_cast<uint8_t>(
                std::clamp(std::pow(norm, inv_g) * 255.0f, 0.0f, 255.0f));
        }
    }

    void rebuild_blc_gain_lut(float black_level, float gain) {
        if (std::abs(black_level - current_blc_) < 1e-3f &&
            std::abs(gain - current_gain_) < 1e-3f) {
            return;
        }
        current_blc_ = black_level;
        current_gain_ = gain;
        const float blc_scale = 255.0f / std::max(1.0f, 255.0f - black_level);
        for (int i = 0; i < 256; ++i) {
            float v = (static_cast<float>(i) - black_level) * blc_scale * gain;
            blc_gain_lut_[i] = std::clamp(v, 0.0f, 255.0f);
        }
    }

    // Copy #1: Zero-copy DMABUF -> synthetic RGGB 8-bit RAW Bayer buffer (multithreaded)
    void rgb_to_bayer_rggb(const uint8_t* rgb, uint32_t stride) {
        pool_.parallel_rows(height_, [&](size_t, uint32_t y_start, uint32_t y_end) {
            for (uint32_t y = y_start; y < y_end; ++y) {
                const uint8_t* row = rgb + static_cast<size_t>(y) * stride;
                uint8_t* dst = bayer_.data() + static_cast<size_t>(y) * width_;
                bool even_y = ((y & 1u) == 0);
                for (uint32_t x = 0; x < width_; ++x) {
                    bool even_x = ((x & 1u) == 0);
                    const uint8_t* px = row + x * 3;
                    if (even_y && even_x)        dst[x] = px[0]; // R
                    else if (!even_y && !even_x) dst[x] = px[2]; // B
                    else                         dst[x] = px[1]; // G
                }
            }
        });
    }

    const std::vector<uint8_t>& raw_bayer() const { return bayer_; }

    // Runs all 5 ISP stages on RAW Bayer -> Copy #2: processed RGB888 output buffer (multithreaded)
    void process(const IspParams& p, std::vector<uint8_t>& out_rgb) {
        rebuild_gamma_lut(p.gamma);
        const float gain = p.ae_enable ? p.exposure_gain : 1.0f;
        rebuild_blc_gain_lut(p.black_level, gain);
        out_rgb.resize(static_cast<size_t>(width_) * height_ * 3);

        // Stage 1: Black-Level Correction (BLC) + Exposure Gain via O(1) LUT
        pool_.parallel_rows(height_, [&](size_t, uint32_t y_start, uint32_t y_end) {
            size_t idx_start = static_cast<size_t>(y_start) * width_;
            size_t idx_end = static_cast<size_t>(y_end) * width_;
            const uint8_t* src = bayer_.data();
            float* dst = isp_work_.data();
            for (size_t i = idx_start; i < idx_end; ++i) {
                dst[i] = blc_gain_lut_[src[i]];
            }
        });

        // Stage 2: Gray-World Auto White Balance (AWB) with thread-local reduction
        if (p.awb_enable) {
            const size_t nthreads = pool_.size();
            std::vector<double> sum_r(nthreads, 0.0), sum_g(nthreads, 0.0), sum_b(nthreads, 0.0);
            std::vector<size_t> cnt_r(nthreads, 0), cnt_g(nthreads, 0), cnt_b(nthreads, 0);

            pool_.parallel_rows(height_, [&](size_t tid, uint32_t y_start, uint32_t y_end) {
                double lr = 0.0, lg = 0.0, lb = 0.0;
                size_t cr = 0, cg = 0, cb = 0;
                for (uint32_t y = y_start; y < y_end; ++y) {
                    bool even_y = ((y & 1u) == 0);
                    const float* row = isp_work_.data() + static_cast<size_t>(y) * width_;
                    for (uint32_t x = 0; x < width_; ++x) {
                        bool even_x = ((x & 1u) == 0);
                        if (even_y && even_x)        { lr += row[x]; ++cr; }
                        else if (!even_y && !even_x) { lb += row[x]; ++cb; }
                        else                         { lg += row[x]; ++cg; }
                    }
                }
                sum_r[tid] = lr; sum_g[tid] = lg; sum_b[tid] = lb;
                cnt_r[tid] = cr; cnt_g[tid] = cg; cnt_b[tid] = cb;
            });

            double tot_r = 0.0, tot_g = 0.0, tot_b = 0.0;
            size_t tot_cr = 0, tot_cg = 0, tot_cb = 0;
            for (size_t t = 0; t < nthreads; ++t) {
                tot_r += sum_r[t]; tot_g += sum_g[t]; tot_b += sum_b[t];
                tot_cr += cnt_r[t]; tot_cg += cnt_g[t]; tot_cb += cnt_b[t];
            }

            double avg_r = tot_cr ? tot_r / tot_cr : 1.0;
            double avg_g = tot_cg ? tot_g / tot_cg : 1.0;
            double avg_b = tot_cb ? tot_b / tot_cb : 1.0;
            double avg_gray = (avg_r + avg_g + avg_b) / 3.0;
            float r_gain = static_cast<float>(std::clamp(avg_gray / std::max(1.0, avg_r), 0.5, 2.5));
            float g_gain = static_cast<float>(std::clamp(avg_gray / std::max(1.0, avg_g), 0.5, 2.5));
            float b_gain = static_cast<float>(std::clamp(avg_gray / std::max(1.0, avg_b), 0.5, 2.5));

            pool_.parallel_rows(height_, [&](size_t, uint32_t y_start, uint32_t y_end) {
                for (uint32_t y = y_start; y < y_end; ++y) {
                    bool even_y = ((y & 1u) == 0);
                    float* row = isp_work_.data() + static_cast<size_t>(y) * width_;
                    for (uint32_t x = 0; x < width_; ++x) {
                        bool even_x = ((x & 1u) == 0);
                        float cg = (even_y && even_x) ? r_gain : ((!even_y && !even_x) ? b_gain : g_gain);
                        row[x] = std::clamp(row[x] * cg, 0.0f, 255.0f);
                    }
                }
            });
        }

        // 3x3 Color Correction Matrix (sRGB daylight tuning)
        const float ccm[3][3] = {
            { 1.18f, -0.12f, -0.06f},
            {-0.08f,  1.14f, -0.06f},
            {-0.04f, -0.16f,  1.20f}
        };
        const float bright_offset = p.brightness * 128.0f;

        auto at = [&](int y, int x) -> float {
            y = std::clamp(y, 0, static_cast<int>(height_) - 1);
            x = std::clamp(x, 0, static_cast<int>(width_) - 1);
            return isp_work_[static_cast<size_t>(y) * width_ + x];
        };

        // Stage 3 (Bilinear Demosaic) + Stage 4 (CCM + Contrast/Brightness) + Stage 5 (Gamma LUT)
        pool_.parallel_rows(height_, [&](size_t, uint32_t y_start, uint32_t y_end) {
            for (int y = static_cast<int>(y_start); y < static_cast<int>(y_end); ++y) {
                uint8_t* dst = out_rgb.data() + static_cast<size_t>(y) * width_ * 3;
                bool even_y = ((y & 1) == 0);
                for (int x = 0; x < static_cast<int>(width_); ++x) {
                    bool even_x = ((x & 1) == 0);
                    float r = 0.0f, g = 0.0f, b = 0.0f;

                    if (even_y && even_x) {
                        r = at(y, x);
                        g = 0.25f * (at(y - 1, x) + at(y + 1, x) + at(y, x - 1) + at(y, x + 1));
                        b = 0.25f * (at(y - 1, x - 1) + at(y - 1, x + 1) + at(y + 1, x - 1) + at(y + 1, x + 1));
                    } else if (!even_y && !even_x) {
                        r = 0.25f * (at(y - 1, x - 1) + at(y - 1, x + 1) + at(y + 1, x - 1) + at(y + 1, x + 1));
                        g = 0.25f * (at(y - 1, x) + at(y + 1, x) + at(y, x - 1) + at(y, x + 1));
                        b = at(y, x);
                    } else if (even_y && !even_x) {
                        r = 0.5f * (at(y, x - 1) + at(y, x + 1));
                        g = at(y, x);
                        b = 0.5f * (at(y - 1, x) + at(y + 1, x));
                    } else {
                        r = 0.5f * (at(y - 1, x) + at(y + 1, x));
                        g = at(y, x);
                        b = 0.5f * (at(y, x - 1) + at(y, x + 1));
                    }

                    float cr = (ccm[0][0] * r + ccm[0][1] * g + ccm[0][2] * b - 128.0f) * p.contrast + 128.0f + bright_offset;
                    float cg = (ccm[1][0] * r + ccm[1][1] * g + ccm[1][2] * b - 128.0f) * p.contrast + 128.0f + bright_offset;
                    float cb = (ccm[2][0] * r + ccm[2][1] * g + ccm[2][2] * b - 128.0f) * p.contrast + 128.0f + bright_offset;

                    dst[x * 3 + 0] = gamma_lut_[static_cast<int>(std::clamp(cr, 0.0f, 255.0f))];
                    dst[x * 3 + 1] = gamma_lut_[static_cast<int>(std::clamp(cg, 0.0f, 255.0f))];
                    dst[x * 3 + 2] = gamma_lut_[static_cast<int>(std::clamp(cb, 0.0f, 255.0f))];
                }
            }
        });
    }

private:
    uint32_t width_;
    uint32_t height_;
    RowThreadPool pool_;
    float current_gamma_ = 0.0f;
    float current_blc_ = -1.0f;
    float current_gain_ = -1.0f;
    uint8_t gamma_lut_[256]{};
    float blc_gain_lut_[256]{};
    std::vector<uint8_t> bayer_;
    std::vector<float> isp_work_;
};

static void save_snapshots(const std::vector<uint8_t>& bayer,
                           const std::vector<uint8_t>& rgb,
                           uint32_t w, uint32_t h, uint64_t frame_idx) {
    fs::create_directories("/var/lib/isp-app");
    std::string raw_path = "/var/lib/isp-app/snapshot_raw_" + std::to_string(frame_idx) + ".pgm";
    std::ofstream pgm(raw_path, std::ios::binary);
    pgm << "P5\n" << w << " " << h << "\n255\n";
    pgm.write(reinterpret_cast<const char*>(bayer.data()), bayer.size());
    pgm.close();

    std::string png_path = "/var/lib/isp-app/snapshot_" + std::to_string(frame_idx) + ".png";
    std::string cmd = "ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgb24 -s " +
                      std::to_string(w) + "x" + std::to_string(h) +
                      " -i - -frames:v 1 " + png_path;
    FILE* pipe = popen(cmd.c_str(), "w");
    if (pipe) {
        fwrite(rgb.data(), 1, rgb.size(), pipe);
        pclose(pipe);
        std::cout << "[SNAPSHOT] Saved " << png_path << " and " << raw_path << std::endl;
    }
}

struct MappedBuffer {
    void* addr = nullptr;
    size_t length = 0;
};

class CameraIspApp {
public:
    int run() {
        cm_ = std::make_unique<CameraManager>();
        if (cm_->start() != 0) {
            std::cerr << "Failed to start CameraManager\n";
            return 1;
        }

        auto cameras = cm_->cameras();
        if (cameras.empty()) {
            std::cerr << "No libcamera cameras found!\n";
            return 1;
        }

        camera_ = cameras[0];
        std::cout << "Selected camera: " << camera_->id() << std::endl;
        if (camera_->acquire() != 0) {
            std::cerr << "Failed to acquire camera\n";
            return 1;
        }

        std::cout << "--- Supported Camera Controls (ControlInfoMap) ---\n";
        for (const auto& [id, info] : camera_->controls()) {
            std::cout << "  " << id->name() << " : " << info.toString() << "\n";
        }

        config_ = camera_->generateConfiguration({StreamRole::Viewfinder});
        if (!config_ || config_->empty()) {
            std::cerr << "Failed to generate stream configuration\n";
            return 1;
        }

        StreamConfiguration& stream_cfg = config_->at(0);
        stream_cfg.size = {1920, 1080};
        stream_cfg.pixelFormat = formats::RGB888;
        stream_cfg.bufferCount = 4;

        CameraConfiguration::Status status = config_->validate();
        if (status == CameraConfiguration::Invalid) {
            std::cerr << "Camera configuration invalid\n";
            return 1;
        }
        std::cout << "Validated stream: " << stream_cfg.toString()
                  << " (stride=" << stream_cfg.stride << ")\n";

        if (camera_->configure(config_.get()) != 0) {
            std::cerr << "Failed to configure camera\n";
            return 1;
        }

        width_ = stream_cfg.size.width;
        height_ = stream_cfg.size.height;
        stride_ = stream_cfg.stride;
        isp_ = std::make_unique<SoftwareIsp>(width_, height_);

        allocator_ = std::make_unique<FrameBufferAllocator>(camera_);
        Stream* stream = stream_cfg.stream();
        if (allocator_->allocate(stream) < 0) {
            std::cerr << "Failed to allocate buffers\n";
            return 1;
        }

        const auto& buffers = allocator_->buffers(stream);
        std::cout << "Allocated " << buffers.size() << " DMABUF FrameBuffers\n";

        for (const auto& buf : buffers) {
            const FrameBuffer::Plane& plane = buf->planes()[0];
            void* mem = mmap(nullptr, plane.length, PROT_READ, MAP_SHARED, plane.fd.get(), 0);
            if (mem == MAP_FAILED) {
                std::cerr << "mmap failed on DMABUF fd\n";
                return 1;
            }
            mapped_buffers_[buf.get()] = {mem, plane.length};

            std::unique_ptr<Request> req = camera_->createRequest();
            if (!req || req->addBuffer(stream, buf.get()) < 0) {
                std::cerr << "Failed to create/bind request\n";
                return 1;
            }
            requests_.push_back(std::move(req));
        }

        fs::create_directories("/var/lib/isp-app");
        std::string enc_cmd = "ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgb24 -s " +
                              std::to_string(width_) + "x" + std::to_string(height_) +
                              " -r 10 -i - -c:v mpeg4 -q:v 5 -pix_fmt yuv420p /var/lib/isp-app/output.mp4";
        encoder_pipe_ = popen(enc_cmd.c_str(), "w");
        if (!encoder_pipe_) {
            std::cerr << "Warning: failed to open ffmpeg MP4 pipe\n";
        }

        camera_->requestCompleted.connect(this, &CameraIspApp::on_request_completed);

        ControlList init_controls(camera_->controls());
        if (camera_->controls().count(&controls::Brightness)) {
            init_controls.set(controls::Brightness, 0.05f);
            isp_params_.brightness = 0.05f;
        }
        if (camera_->controls().count(&controls::Contrast)) {
            init_controls.set(controls::Contrast, 1.10f);
            isp_params_.contrast = 1.10f;
        }

        if (camera_->start(&init_controls) != 0) {
            std::cerr << "Failed to start camera\n";
            return 1;
        }

        for (auto& req : requests_) {
            camera_->queueRequest(req.get());
        }

        std::cout << "Capture + Multithreaded Software ISP loop started (SIGUSR1 or /tmp/snapshot triggers PNG/RAW capture)\n";

        auto t_start = std::chrono::steady_clock::now();
        auto t_last_log = t_start;
        uint64_t frames_since_log = 0;

        while (g_running) {
            Request* completed = nullptr;
            {
                std::unique_lock<std::mutex> lk(queue_mtx_);
                queue_cv_.wait_for(lk, std::chrono::milliseconds(100), [&] {
                    return !completed_queue_.empty() || !g_running;
                });
                if (!completed_queue_.empty()) {
                    completed = completed_queue_.front();
                    completed_queue_.pop();
                }
            }

            if (!completed) continue;
            if (completed->status() == Request::RequestCancelled) continue;

            auto t_frame_begin = std::chrono::steady_clock::now();

            FrameBuffer* fb = completed->buffers().begin()->second;
            const MappedBuffer& mb = mapped_buffers_[fb];

            // Copy #1: Zero-copy DMABUF -> synthetic RGGB RAW Bayer buffer (multithreaded)
            isp_->rgb_to_bayer_rggb(static_cast<const uint8_t*>(mb.addr), stride_);

            // Copy #2: 5-stage Software ISP -> processed RGB888 buffer (multithreaded)
            isp_->process(isp_params_, processed_rgb_);

            auto t_frame_end = std::chrono::steady_clock::now();
            double isp_ms = std::chrono::duration<double, std::milli>(t_frame_end - t_frame_begin).count();
            total_isp_ms_ += isp_ms;

            // Copy #3: Write RGB888 frame to ffmpeg encoder stdin pipe -> /var/lib/isp-app/output.mp4
            if (encoder_pipe_) {
                fwrite(processed_rgb_.data(), 1, processed_rgb_.size(), encoder_pipe_);
            }

            if (g_snapshot_requested.exchange(false) || fs::exists("/tmp/snapshot")) {
                if (fs::exists("/tmp/snapshot")) fs::remove("/tmp/snapshot");
                save_snapshots(isp_->raw_bayer(), processed_rgb_, width_, height_, total_frames_);
            }

            ++total_frames_;
            ++frames_since_log;

            completed->reuse(Request::ReuseBuffers);
            if (camera_->controls().count(&controls::Brightness)) {
                completed->controls().set(controls::Brightness, isp_params_.brightness);
            }
            if (camera_->controls().count(&controls::Contrast)) {
                completed->controls().set(controls::Contrast, isp_params_.contrast);
            }
            camera_->queueRequest(completed);

            auto now = std::chrono::steady_clock::now();
            double elapsed_sec = std::chrono::duration<double>(now - t_last_log).count();
            if (elapsed_sec >= 5.0) {
                double fps = frames_since_log / elapsed_sec;
                double avg_ms = total_isp_ms_ / total_frames_;
                std::cout << std::fixed << std::setprecision(2)
                          << "[METRICS] threads=" << isp_->thread_count()
                          << " frames=" << total_frames_
                          << " fps=" << fps
                          << " avg_isp_ms=" << avg_ms << "ms" << std::endl;
                frames_since_log = 0;
                t_last_log = now;
            }
        }

        std::cout << "Shutting down cleanly...\n";
        camera_->stop();
        if (encoder_pipe_) {
            fflush(encoder_pipe_);
            pclose(encoder_pipe_);
        }
        for (auto& [fb, mb] : mapped_buffers_) {
            munmap(mb.addr, mb.length);
        }
        allocator_->free(stream);
        camera_->release();
        cm_->stop();
        return 0;
    }

private:
    void on_request_completed(Request* request) {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        completed_queue_.push(request);
        queue_cv_.notify_one();
    }

    std::unique_ptr<CameraManager> cm_;
    std::shared_ptr<Camera> camera_;
    std::unique_ptr<CameraConfiguration> config_;
    std::unique_ptr<FrameBufferAllocator> allocator_;
    std::vector<std::unique_ptr<Request>> requests_;
    std::map<FrameBuffer*, MappedBuffer> mapped_buffers_;

    std::mutex queue_mtx_;
    std::condition_variable queue_cv_;
    std::queue<Request*> completed_queue_;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t stride_ = 0;
    IspParams isp_params_;
    std::unique_ptr<SoftwareIsp> isp_;
    std::vector<uint8_t> processed_rgb_;
    FILE* encoder_pipe_ = nullptr;

    uint64_t total_frames_ = 0;
    double total_isp_ms_ = 0.0;
};

int main() {
    std::signal(SIGINT, handle_sigint);
    std::signal(SIGTERM, handle_sigint);
    std::signal(SIGUSR1, handle_sigusr1);

    CameraIspApp app;
    return app.run();
}
