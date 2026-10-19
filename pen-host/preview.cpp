#include "preview.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrandr.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include "protocol.h"

namespace vp {

// ---------------------------------------------------------------------------
// Monitor lookup

bool queryX11Monitor(const std::string &name, MonitorGeom &out, std::string &err) {
    Display *d = XOpenDisplay(nullptr);
    if (!d) {
        err = "cannot open the X display (is DISPLAY set? the preview needs an X11 session)";
        return false;
    }
    Window root = DefaultRootWindow(d);
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(d, root);
    RROutput primary = XRRGetOutputPrimary(d, root);
    std::string want = name, names;
    std::transform(want.begin(), want.end(), want.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    bool found = false;
    for (int pass = 0; pass < 2 && !found && res; pass++) {
        for (int i = 0; i < res->noutput && !found; i++) {
            XRROutputInfo *info = XRRGetOutputInfo(d, res, res->outputs[i]);
            if (!info) continue;
            if (info->connection == RR_Connected && info->crtc) {
                std::string oname(info->name, size_t(info->nameLen)), lname = oname;
                std::transform(lname.begin(), lname.end(), lname.begin(),
                               [](unsigned char c) { return char(std::tolower(c)); });
                if (pass == 0) names += (names.empty() ? "" : ", ") + oname;
                // Pass 0: exact name or primary; pass 1 (no name given): any active output.
                bool match = want.empty() ? (pass == 1 || res->outputs[i] == primary) : (pass == 0 && lname == want);
                if (match) {
                    XRRCrtcInfo *crtc = XRRGetCrtcInfo(d, res, info->crtc);
                    if (crtc) {
                        out.name = oname;
                        out.x = crtc->x;
                        out.y = crtc->y;
                        out.w = int(crtc->width);
                        out.h = int(crtc->height);
                        bool rotated = crtc->rotation & (RR_Rotate_90 | RR_Rotate_270);
                        out.mm_w = double(rotated ? info->mm_height : info->mm_width);
                        out.mm_h = double(rotated ? info->mm_width : info->mm_height);
                        XRRFreeCrtcInfo(crtc);
                        found = true;
                    }
                }
            }
            XRRFreeOutputInfo(info);
        }
    }
    if (res) XRRFreeScreenResources(res);
    XCloseDisplay(d);
    if (!found) err = "no active X output named \"" + name + "\" (active: " + names + ")";
    return found;
}

// ---------------------------------------------------------------------------
// Annex-B helpers

std::vector<std::pair<size_t, size_t>> splitNals(const uint8_t *d, size_t n) {
    std::vector<size_t> starts; // offset of each start code
    for (size_t i = 0; i + 3 <= n; i++) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            starts.push_back(i > 0 && d[i - 1] == 0 ? i - 1 : i);
            i += 2;
        }
    }
    std::vector<std::pair<size_t, size_t>> nals;
    for (size_t k = 0; k < starts.size(); k++) {
        size_t end = k + 1 < starts.size() ? starts[k + 1] : n;
        nals.emplace_back(starts[k], end - starts[k]);
    }
    return nals;
}

bool extractParameterSets(const uint8_t *au, size_t len, std::vector<uint8_t> &sps, std::vector<uint8_t> &pps) {
    sps.clear();
    pps.clear();
    for (auto [off, n] : splitNals(au, len)) {
        size_t hdr = au[off + 2] == 1 ? 3 : 4; // start code length
        if (n <= hdr) continue;
        int type = au[off + hdr] & 0x1f;
        if (type == 7) sps.assign(au + off, au + off + n);
        if (type == 8) pps.assign(au + off, au + off + n);
    }
    return !sps.empty() && !pps.empty();
}

namespace {

// ---------------------------------------------------------------------------
// Capture

class X11Capture {
public:
    ~X11Capture() {
        release();
        if (dpy_) XCloseDisplay(dpy_);
    }

    bool open(std::string &err) {
        dpy_ = XOpenDisplay(nullptr);
        if (!dpy_) {
            err = "cannot open the X display";
            return false;
        }
        if (!XShmQueryExtension(dpy_)) {
            err = "the X server has no MIT-SHM extension";
            return false;
        }
        return true;
    }

    bool setRegion(int x, int y, int w, int h) {
        release();
        int scr = DefaultScreen(dpy_);
        img_ = XShmCreateImage(dpy_, DefaultVisual(dpy_, scr), unsigned(DefaultDepth(dpy_, scr)), ZPixmap,
                               nullptr, &shm_, unsigned(w), unsigned(h));
        if (!img_) return false;
        if (img_->bits_per_pixel != 32 || img_->byte_order != LSBFirst) {
            fprintf(stderr, "preview: unsupported X pixel format (%d bpp)\n", img_->bits_per_pixel);
            release();
            return false;
        }
        shm_.shmid = shmget(IPC_PRIVATE, size_t(img_->bytes_per_line) * size_t(h), IPC_CREAT | 0600);
        void *addr = shm_.shmid < 0 ? reinterpret_cast<void *>(-1) : shmat(shm_.shmid, nullptr, 0);
        if (addr == reinterpret_cast<void *>(-1)) {
            if (shm_.shmid >= 0) shmctl(shm_.shmid, IPC_RMID, nullptr);
            XDestroyImage(img_);
            img_ = nullptr;
            return false;
        }
        shm_.shmaddr = img_->data = static_cast<char *>(addr);
        shm_.readOnly = False;
        XShmAttach(dpy_, &shm_);
        XSync(dpy_, False);
        shmctl(shm_.shmid, IPC_RMID, nullptr); // freed once both sides detach
        x_ = x;
        y_ = y;
        return true;
    }

    bool grab() { return img_ && XShmGetImage(dpy_, DefaultRootWindow(dpy_), img_, x_, y_, AllPlanes); }
    const uint8_t *data() const { return reinterpret_cast<const uint8_t *>(img_->data); }
    int stride() const { return img_->bytes_per_line; }

private:
    Display *dpy_ = nullptr;
    XImage *img_ = nullptr;
    XShmSegmentInfo shm_{};
    int x_ = 0, y_ = 0;

    void release() {
        if (!img_) return;
        XShmDetach(dpy_, &shm_);
        XDestroyImage(img_); // for XShm images this frees only the struct
        shmdt(shm_.shmaddr);
        img_ = nullptr;
    }
};

// ---------------------------------------------------------------------------
// Encoder

class H264Encoder {
public:
    ~H264Encoder() { close(); }

    bool open(int w, int h, const PreviewOptions &opt, std::string &err) {
        close();
        std::vector<const char *> names;
        if (opt.encoder == "nvenc") names = {"h264_nvenc"};
        else if (opt.encoder == "x264") names = {"libx264"};
        else names = {"h264_nvenc", "libx264"};
        for (const char *n : names) {
            if (tryOpen(n, w, h, opt)) {
                name_ = n;
                return true;
            }
        }
        err = "no usable H.264 encoder (tried";
        for (const char *n : names) err += std::string(" ") + n;
        err += ")";
        return false;
    }

    void close() {
        if (sws_) sws_freeContext(sws_);
        av_frame_free(&frame_);
        av_packet_free(&pkt_);
        avcodec_free_context(&ctx_);
        sws_ = nullptr;
    }

    const char *name() const { return name_; }

    // Encodes one BGRx frame; onPacket gets each finished access unit.
    void encode(const uint8_t *bgrx, int stride, bool keyframe,
                const std::function<void(const uint8_t *, size_t, bool)> &onPacket) {
        if (rgbInput_) {
            frame_->data[0] = const_cast<uint8_t *>(bgrx);
            frame_->linesize[0] = stride;
        } else {
            if (av_frame_make_writable(frame_) < 0) return;
            const uint8_t *src[1] = {bgrx};
            int srcStride[1] = {stride};
            sws_scale(sws_, src, srcStride, 0, ctx_->height, frame_->data, frame_->linesize);
        }
        frame_->pts = pts_++;
        frame_->pict_type = keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        if (avcodec_send_frame(ctx_, frame_) < 0) return;
        while (avcodec_receive_packet(ctx_, pkt_) == 0) {
            onPacket(pkt_->data, size_t(pkt_->size), pkt_->flags & AV_PKT_FLAG_KEY);
            av_packet_unref(pkt_);
        }
    }

private:
    AVCodecContext *ctx_ = nullptr;
    AVFrame *frame_ = nullptr;
    AVPacket *pkt_ = nullptr;
    SwsContext *sws_ = nullptr;
    bool rgbInput_ = false;
    int64_t pts_ = 0;
    const char *name_ = "";

    bool tryOpen(const char *name, int w, int h, const PreviewOptions &opt) {
        const AVCodec *codec = avcodec_find_encoder_by_name(name);
        if (!codec) return false;
        ctx_ = avcodec_alloc_context3(codec);
        ctx_->width = w;
        ctx_->height = h;
        ctx_->time_base = AVRational{1, opt.fps};
        ctx_->framerate = AVRational{opt.fps, 1};
        ctx_->gop_size = opt.fps * 10; // keyframes come on demand; this is a safety net
        ctx_->max_b_frames = 0;
        ctx_->bit_rate = int64_t(opt.bitrate_kbps) * 1000;
        ctx_->rc_max_rate = ctx_->bit_rate;
        ctx_->rc_buffer_size = int(ctx_->bit_rate / 4);

        // Feed BGRx straight in when the encoder takes it (NVENC converts on
        // the GPU); otherwise convert to 4:2:0 here.
        rgbInput_ = false;
        for (const AVPixelFormat *f = codec->pix_fmts; f && *f != AV_PIX_FMT_NONE; f++)
            if (*f == AV_PIX_FMT_0RGB32) rgbInput_ = true;
        ctx_->pix_fmt = rgbInput_ ? AV_PIX_FMT_0RGB32 : AV_PIX_FMT_YUV420P;

        if (strcmp(name, "h264_nvenc") == 0) {
            av_opt_set(ctx_->priv_data, "preset", "p1", 0);
            av_opt_set(ctx_->priv_data, "tune", "ull", 0);
            av_opt_set(ctx_->priv_data, "rc", "cbr", 0);
            av_opt_set(ctx_->priv_data, "zerolatency", "1", 0);
            av_opt_set(ctx_->priv_data, "delay", "0", 0);
            av_opt_set(ctx_->priv_data, "rc-lookahead", "0", 0);
            av_opt_set(ctx_->priv_data, "forced-idr", "1", 0);
        } else {
            av_opt_set(ctx_->priv_data, "preset", "ultrafast", 0);
            av_opt_set(ctx_->priv_data, "tune", "zerolatency", 0);
            av_opt_set(ctx_->priv_data, "forced-idr", "1", 0);
        }
        if (avcodec_open2(ctx_, codec, nullptr) < 0) {
            avcodec_free_context(&ctx_);
            return false;
        }
        frame_ = av_frame_alloc();
        frame_->format = ctx_->pix_fmt;
        frame_->width = w;
        frame_->height = h;
        if (!rgbInput_) {
            av_frame_get_buffer(frame_, 0);
            sws_ = sws_getContext(w, h, AV_PIX_FMT_0RGB32, w, h, AV_PIX_FMT_YUV420P, SWS_POINT, nullptr, nullptr,
                                  nullptr);
        }
        pkt_ = av_packet_alloc();
        pts_ = 0;
        return true;
    }
};

} // namespace

// ---------------------------------------------------------------------------

bool Preview::start(const MonitorGeom &monitor, std::string &err) {
    mon_ = monitor;
    // Probe the display here so a missing X session is reported up front.
    Display *d = XOpenDisplay(nullptr);
    if (!d) {
        err = "cannot open the X display";
        return false;
    }
    XCloseDisplay(d);
    stop_ = false;
    thread_ = std::thread(&Preview::run, this);
    return true;
}

void Preview::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    active_ = false;
}

void Preview::setRegion(int x, int y, int w, int h) {
    std::lock_guard<std::mutex> lock(m_);
    if (x == rx_ && y == ry_ && w == rw_ && h == rh_) return;
    rx_ = x;
    ry_ = y;
    rw_ = w;
    rh_ = h;
    regionGen_++;
}

void Preview::run() {
    X11Capture cap;
    std::string err;
    if (!cap.open(err)) {
        fprintf(stderr, "preview disabled: %s\n", err.c_str());
        return;
    }
    active_ = true;
    H264Encoder enc;
    unsigned gen = 0;
    int w = 0, h = 0, encW = 0, encH = 0;
    bool encoderOk = false;
    std::vector<uint8_t> prev, sps, pps, lastSps, lastPps, msg;
    const auto period = std::chrono::nanoseconds(1000000000LL / std::max(1, opt_.fps));
    auto next = std::chrono::steady_clock::now();
    int frames = 0, dropped = 0;
    auto statsAt = next + std::chrono::seconds(1);

    while (!stop_) {
        next += period;
        auto now = std::chrono::steady_clock::now();
        if (next < now) next = now; // fell behind: don't try to catch up
        std::this_thread::sleep_until(next);

        if (!slot_.connected()) {
            resendConfig_ = true;
            keyframe_ = true;
            continue;
        }

        int x, y;
        unsigned g;
        {
            std::lock_guard<std::mutex> lock(m_);
            g = regionGen_;
            x = rx_;
            y = ry_;
            if (g != gen) {
                w = rw_;
                h = rh_;
            }
        }
        if (w <= 0 || h <= 0) continue;
        if (g != gen) {
            bool sizeChanged = !encoderOk || w != encW || h != encH;
            gen = g;
            if (!cap.setRegion(mon_.x + x, mon_.y + y, w, h)) {
                fprintf(stderr, "preview: cannot capture %dx%d\n", w, h);
                encoderOk = false;
                continue;
            }
            if (sizeChanged) {
                encoderOk = enc.open(w, h, opt_, err);
                encW = w;
                encH = h;
                active_ = encoderOk;
                if (!encoderOk) {
                    fprintf(stderr, "preview disabled: %s\n", err.c_str());
                    continue;
                }
                fprintf(stderr, "preview: %dx%d at %d fps with %s\n", w, h, opt_.fps, enc.name());
                resendConfig_ = true;
            }
            keyframe_ = true;
            prev.clear();
        }
        if (!encoderOk || !cap.grab()) continue;

        // Skip unchanged frames: the tablet keeps showing the last one.
        const uint8_t *px = cap.data();
        const int stride = cap.stride();
        const size_t row = size_t(w) * 4;
        bool changed = prev.empty();
        for (int r = 0; r < h && !changed; r++)
            changed = memcmp(px + size_t(r) * size_t(stride), &prev[size_t(r) * row], row) != 0;
        bool key = keyframe_.exchange(false);
        if (!changed && !key) continue;
        prev.resize(row * size_t(h));
        for (int r = 0; r < h; r++) memcpy(&prev[size_t(r) * row], px + size_t(r) * size_t(stride), row);

        enc.encode(px, stride, key, [&](const uint8_t *au, size_t len, bool isKey) {
            if (isKey && extractParameterSets(au, len, sps, pps) &&
                (resendConfig_ || sps != lastSps || pps != lastPps)) {
                std::vector<uint8_t> p(8 + sps.size() + pps.size());
                wr16(&p[0], unsigned(w));
                wr16(&p[2], unsigned(h));
                wr16(&p[4], unsigned(sps.size()));
                wr16(&p[6], unsigned(pps.size()));
                memcpy(&p[8], sps.data(), sps.size());
                memcpy(&p[8 + sps.size()], pps.data(), pps.size());
                msg = downlinkMessage(DL_CODEC, 0, p.data(), p.size());
                if (slot_.send(msg.data(), msg.size(), false)) {
                    lastSps = sps;
                    lastPps = pps;
                    resendConfig_ = false;
                }
            }
            if (resendConfig_) {
                keyframe_ = true; // nothing decodable without the configuration
                return;
            }
            msg = downlinkMessage(DL_FRAME, isKey ? DL_FLAG_KEYFRAME : 0, au, len);
            if (slot_.send(msg.data(), msg.size(), true)) {
                frames++;
            } else {
                dropped++;
                keyframe_ = true; // the next frames would reference the lost one
            }
        });

        if (opt_.verbose && std::chrono::steady_clock::now() >= statsAt) {
            fprintf(stderr, "preview: %d frames/s sent, %d dropped\n", frames, dropped);
            frames = dropped = 0;
            statsAt = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
    }
}

} // namespace vp
