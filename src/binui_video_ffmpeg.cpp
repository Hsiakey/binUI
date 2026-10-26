// binui_video_ffmpeg.cpp - FFmpeg + WASAPI 视频引擎（BINUI_ENABLE_VIDEO=ON 时编译）
//
// 架构：
//   - 每个 <video> 节点一条解码线程：FFmpeg demux → 视频 sws 转 RGBA 推入帧队列，
//     音频 swr 转混音格式推入 PCM 环形缓冲。
//   - 每流一条 WASAPI 共享模式输出线程：消费 PCM（应用音量/静音）并产生音频时钟。
//   - 渲染器播放时钟为主时钟：解码线程按 renderer.findVideo()->position 决定
//     "此刻应显示哪一帧"，经 updateImageRaw 推送到伪图片槽（后端按版本号重传纹理）。
//     音频与渲染器时钟漂移超过阈值时做 quiet 校正（不触发事件、不重绘）。
//   - seek / loop 回绕由目标位置跳变检测驱动：清队列 + 关键帧附近重定位。
//
// 依赖：avformat avcodec swscale swresample avutil + ole32(mmdeviceapi/audioclient)
#include "binui_video.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
}

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace binui {
namespace {

constexpr double kPresentLeadSec  = 0.03; // 提前量：帧 pts ≤ 目标+lead 即可显示
constexpr double kDecodeAheadSec  = 0.35; // 解码预读深度（秒）
constexpr size_t kMaxVideoQueue   = 8;    // 帧队列上限
constexpr double kReseekJumpSec   = 1.5;  // 前向跳变超此值视为 seek
constexpr double kWrapJumpSec     = 0.5;  // 后向跳变超此值视为回绕/seek
constexpr double kDriftCorrectSec = 0.15; // A/V 漂移校正阈值

struct VideoFrame {
    double pts = 0;          // 秒（流时间基）
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

// ---- PCM 环形缓冲（float 交错，混音参数） ----
class PcmRing {
public:
    void configure(int channels, int rate) {
        std::lock_guard<std::mutex> lk(m_);
        ch_ = channels; rate_ = rate; q_.clear();
    }
    int channels() const { return ch_; }
    int rate() const { return rate_; }
    void push(const float* s, size_t n) {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.size() > 6 * 1024 * 1024) return; // 防爆（约 30s@48k stereo）
        q_.insert(q_.end(), s, s + n);
    }
    bool pull(float* dst, size_t n) { // 不足补静音，返回是否 underrun
        std::lock_guard<std::mutex> lk(m_);
        bool ok = q_.size() >= n;
        size_t got = std::min(n, q_.size());
        if (got) {
            std::copy(q_.begin(), q_.begin() + got, dst);
            q_.erase(q_.begin(), q_.begin() + got);
        }
        for (size_t i = got; i < n; i++) dst[i] = 0.0f;
        return ok;
    }
    void clear() { std::lock_guard<std::mutex> lk(m_); q_.clear(); }

private:
    std::mutex m_;
    std::deque<float> q_;
    int ch_ = 2, rate_ = 48000;
};

// ---- WASAPI 共享模式输出（每流一条线程；填充回调由 MediaStream 提供） ----
class WasapiOut {
public:
    bool init(PcmRing* ring) {
        ring_ = ring;
        HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), (void**)&enumr_);
        if (FAILED(hr)) return false;
        IMMDevice* dev = nullptr;
        if (FAILED(enumr_->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return false;
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client_);
        dev->Release();
        if (FAILED(hr)) return false;
        if (FAILED(client_->GetMixFormat(&mix_))) return false;
        if (FAILED(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                       10000000 /*1s*/, 0, mix_, nullptr))) return false;
        if (FAILED(client_->GetBufferSize(&bufFrames_))) return false;
        if (FAILED(client_->GetService(__uuidof(IAudioRenderClient), (void**)&render_)))
            return false;

        isFloat_ = true;
        if (mix_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) isFloat_ = true;
        else if (mix_->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            auto* ex = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix_);
            isFloat_ = IsEqualGUID(ex->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        } else isFloat_ = false;
        ring_->configure(mix_->nChannels, (int)mix_->nSamplesPerSec);
        return SUCCEEDED(client_->Start());
    }

    // 输出循环：fill(dst, frames) 负责取样本与音量；onLoop() 每轮回调（时钟/校正）
    void run(std::atomic<bool>& running,
             const std::function<void(float*, UINT32)>& fill,
             const std::function<void(double playedSec)>& onLoop) {
        std::vector<float> tmp;
        while (running) {
            UINT32 pad = 0;
            if (FAILED(client_->GetCurrentPadding(&pad))) break;
            UINT32 avail = bufFrames_ - pad;
            if (avail == 0) { Sleep(6); continue; }
            BYTE* p = nullptr;
            if (FAILED(render_->GetBuffer(avail, &p))) break;
            size_t samples = (size_t)avail * mix_->nChannels;
            tmp.resize(samples);
            fill(tmp.data(), avail);
            writtenFrames_ += avail;
            if (isFloat_) memcpy(p, tmp.data(), samples * sizeof(float));
            else { // float -> s16
                int16_t* d = reinterpret_cast<int16_t*>(p);
                for (size_t i = 0; i < samples; i++) {
                    float s = std::min(std::max(tmp[i], -1.0f), 1.0f);
                    d[i] = (int16_t)std::lrint(s * 32767.0f);
                }
            }
            render_->ReleaseBuffer(avail, 0);
            if (onLoop) onLoop(playedSec());
            Sleep(6);
        }
    }

    double playedSec() const { // 近似音频时钟：（累计写入-当前填充）/采样率
        UINT32 pad = 0;
        if (!client_ || FAILED(client_->GetCurrentPadding(&pad))) return clockSec_;
        clockSec_ = ((double)writtenFrames_ - (double)pad) / mix_->nSamplesPerSec;
        return clockSec_;
    }

    void shutdown() {
        if (client_) client_->Stop();
        if (render_) { render_->Release(); render_ = nullptr; }
        if (mix_) { CoTaskMemFree(mix_); mix_ = nullptr; }
        if (client_) { client_->Release(); client_ = nullptr; }
        if (enumr_) { enumr_->Release(); enumr_ = nullptr; }
    }

private:
    IMMDeviceEnumerator* enumr_ = nullptr;
    IAudioClient* client_ = nullptr;
    IAudioRenderClient* render_ = nullptr;
    WAVEFORMATEX* mix_ = nullptr;
    PcmRing* ring_ = nullptr;
    UINT32 bufFrames_ = 0;
    UINT64 writtenFrames_ = 0;
    mutable double clockSec_ = 0;
    bool isFloat_ = true;
};

// ---- 每视频流上下文 ----
struct MediaStream {
    uint16_t nodeId = 0;
    uint16_t imageIndex = 0;
    BinUIRenderer* r = nullptr;

    AVFormatContext* fmt = nullptr;
    int vidIdx = -1, audIdx = -1;
    AVCodecContext* vdec = nullptr;
    AVCodecContext* adec = nullptr;
    SwsContext* sws = nullptr;
    SwrContext* swr = nullptr;
    AVRational vTb{0, 1};

    std::vector<VideoFrame> vq;   // 待显示帧（pts 升序）
    std::mutex vm;
    PcmRing pcm;

    std::atomic<bool> running{false};
    std::thread worker;
    std::thread audioThr;
    WasapiOut wasapi;

    bool eofAny = false;
    double lastTarget = -1;       // 上次看到的渲染器位置（跳变检测）
    double audioClock0 = 0;       // 播放段起点音频时钟（漂移计算用）

    ~MediaStream() { close(); }

    void closeCodecs() {
        if (vdec) avcodec_free_context(&vdec), vdec = nullptr;
        if (adec) avcodec_free_context(&adec), adec = nullptr;
        if (sws)  sws_freeContext(sws), sws = nullptr;
        if (swr)  swr_free(&swr), swr = nullptr;
        if (fmt)  avformat_close_input(&fmt), fmt = nullptr;
        vidIdx = audIdx = -1;
    }
    void close() {
        running = false;
        if (worker.joinable()) worker.join();
        if (audioThr.joinable()) audioThr.join();
        wasapi.shutdown();
        closeCodecs();
        std::lock_guard<std::mutex> lk(vm);
        vq.clear();
    }

    bool open(const std::string& path) {
        if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return false;
        if (avformat_find_stream_info(fmt, nullptr) < 0) return false;
        const AVCodec* vc = nullptr, *ac = nullptr;
        vidIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &vc, 0);
        audIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &ac, 0);
        if (vidIdx < 0) return false;

        vdec = avcodec_alloc_context3(vc);
        avcodec_parameters_to_context(vdec, fmt->streams[vidIdx]->codecpar);
        vTb = fmt->streams[vidIdx]->time_base;
        if (avcodec_open2(vdec, vc, nullptr) < 0) return false;

        sws = sws_getContext(vdec->width, vdec->height, vdec->pix_fmt,
                             vdec->width, vdec->height, AV_PIX_FMT_RGBA,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
        return sws != nullptr;
    }

    // WASAPI 初始化后配置音频解码与重采样
    bool configureAudioOut() {
        if (audIdx < 0) return true; // 无音轨也合法
        const AVCodec* ac = nullptr;
        if (av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &ac, 0) != audIdx)
            return false;
        adec = avcodec_alloc_context3(ac);
        avcodec_parameters_to_context(adec, fmt->streams[audIdx]->codecpar);
        if (avcodec_open2(adec, ac, nullptr) < 0) {
            avcodec_free_context(&adec);
            audIdx = -1;
            return true; // 音频开不动就放无声视频
        }
        AVChannelLayout outLayout;
        av_channel_layout_default(&outLayout, pcm.channels());
        int err = swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, pcm.rate(),
                                      &adec->ch_layout, adec->sample_fmt, adec->sample_rate,
                                      0, nullptr);
        av_channel_layout_uninit(&outLayout);
        if (err < 0 || !swr || swr_init(swr) < 0) {
            audIdx = -1;
            if (adec) avcodec_free_context(&adec), adec = nullptr;
            if (swr) swr_free(&swr), swr = nullptr;
        }
        return true;
    }

    void flushQueues() {
        std::lock_guard<std::mutex> lk(vm);
        vq.clear();
        pcm.clear();
        eofAny = false;
    }

    void seekNear(double t) {
        flushQueues();
        avcodec_flush_buffers(vdec);
        if (adec) avcodec_flush_buffers(adec);
        int64_t us = (int64_t)(std::max(t, 0.0) * AV_TIME_BASE);
        avformat_seek_file(fmt, -1, INT64_MIN, us, us, 0);
        lastTarget = -1;
    }

    // 读一个包并喂给对应解码器
    void readOnePacket() {
        AVPacket pkt;
        if (av_read_frame(fmt, &pkt) < 0) { eofAny = true; return; }
        if (pkt.stream_index == vidIdx && vdec && sws) {
            avcodec_send_packet(vdec, &pkt);
            AVFrame* f = av_frame_alloc();
            while (avcodec_receive_frame(vdec, f) >= 0) {
                VideoFrame vf;
                vf.w = vdec->width; vf.h = vdec->height;
                vf.rgba.resize((size_t)vf.w * vf.h * 4);
                uint8_t* dst[4] = { vf.rgba.data(), nullptr, nullptr, nullptr };
                int stride[4] = { vf.w * 4, 0, 0, 0 };
                sws_scale(sws, f->data, f->linesize, 0, f->height, dst, stride);
                int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE
                            ? f->best_effort_timestamp : f->pts;
                vf.pts = pts * av_q2d(vTb);
                std::lock_guard<std::mutex> lk(vm);
                if (vq.size() >= kMaxVideoQueue) vq.erase(vq.begin());
                vq.push_back(std::move(vf));
            }
            av_frame_free(&f);
        } else if (pkt.stream_index == audIdx && adec && swr) {
            avcodec_send_packet(adec, &pkt);
            AVFrame* f = av_frame_alloc();
            while (avcodec_receive_frame(adec, f) >= 0) {
                int outN = (int)av_rescale_rnd(f->nb_samples, pcm.rate(),
                                               adec->sample_rate, AV_ROUND_UP);
                if (outN <= 0) continue;
                std::vector<float> out((size_t)outN * pcm.channels());
                uint8_t* outB[1] = { reinterpret_cast<uint8_t*>(out.data()) };
                int conv = swr_convert(swr, outB, outN,
                                       const_cast<const uint8_t**>(f->extended_data),
                                       f->nb_samples);
                if (conv > 0) pcm.push(out.data(), (size_t)conv * pcm.channels());
            }
            av_frame_free(&f);
        }
        av_packet_unref(&pkt);
    }

    // 呈现：把 pts ≤ target 的最新一帧推给渲染器，淘汰更旧帧
    void present(double target) {
        std::lock_guard<std::mutex> lk(vm);
        if (vq.empty()) return;
        long keep = -1;
        for (size_t i = 0; i < vq.size(); i++)
            if (vq[i].pts <= target + kPresentLeadSec) keep = (long)i;
            else break;
        if (keep < 0) return;
        VideoFrame& f = vq[(size_t)keep];
        r->updateImageRaw(imageIndex, f.w, f.h, f.rgba.data());
        if (keep > 0) vq.erase(vq.begin(), vq.begin() + keep);
    }

    // 解码主循环（渲染器时钟主控）
    void work() {
        while (running) {
            const VideoState* v = r->findVideo(nodeId);
            if (!v || !v->playing) { lastTarget = -1; Sleep(20); continue; }
            double target = v->position;

            // 跳变检测：loop 回绕 / 用户 seek / quiet 校正
            if (lastTarget >= 0 &&
                (target < lastTarget - kWrapJumpSec ||
                 target > lastTarget + kReseekJumpSec))
                seekNear(std::max(target - 0.25, 0.0));
            lastTarget = target;

            // 补充解码直到预读足够或 EOF
            for (int guard = 0; guard < 64; guard++) {
                bool vAhead = false;
                { std::lock_guard<std::mutex> lk(vm);
                  vAhead = !vq.empty() && vq.back().pts > target + kDecodeAheadSec; }
                bool enough = eofAny || (vAhead && (audIdx < 0 || !pcmQuiet()));
                if (enough) break;
                readOnePacket();
            }
            present(target);
            Sleep(4);
        }
    }
    bool pcmQuiet() { return false; } // 音频随包自然填充（简化：不做水位判断）

    // WASAPI 填充回调：取 PCM + 应用音量/静音
    void fillAudio(float* dst, UINT32 frames) {
        const VideoState* v = r->findVideo(nodeId);
        float vol = v ? v->volume : 1.0f;
        if (v && (v->flags & VID_MUTED)) vol = 0.0f;
        size_t n = (size_t)frames * pcm.channels();
        pcm.pull(dst, n);
        if (vol < 0.999f)
            for (size_t i = 0; i < n; i++) dst[i] *= vol;
    }

    // 音频输出循环：时钟推进 + 漂移 quiet 校正
    void audioWork() {
        wasapi.run(running,
                   [this](float* d, UINT32 fr) { fillAudio(d, fr); },
                   [this](double played) {
                       const VideoState* v = r->findVideo(nodeId);
                       if (!v || !v->playing) { audioClock0 = played; return; }
                       double drift = played - audioClock0 - v->position;
                       if (std::fabs(drift) > kDriftCorrectSec)
                           r->seekVideo(nodeId, played - audioClock0, /*quiet=*/true);
                   });
    }

    bool start(BinUIRenderer* renderer_, uint16_t node, uint16_t imgIdx,
               const std::string& path) {
        r = renderer_; nodeId = node; imageIndex = imgIdx;
        if (!open(path)) return false;
        running = true;
        if (!wasapi.init(&pcm)) { /* 无声降级 */ }
        configureAudioOut();
        worker = std::thread([this] { work(); });
        if (audIdx >= 0) audioThr = std::thread([this] { audioWork(); });
        return true;
    }
};

// ============================================================
// 引擎全局状态
// ============================================================
struct VideoEngine {
    std::mutex m;
    std::vector<std::unique_ptr<MediaStream>> streams;
};
VideoEngine* gEngine = nullptr;

bool IsAbsolutePath(const std::string& p) {
    return p.size() > 1 && (p[1] == ':' || p[0] == '/' || p[0] == '\\');
}

} // namespace

bool AttachVideoEngine(BinUIRenderer& renderer, const std::string& baseDir) {
    DetachVideoEngine(); // 幂等：先清理旧引擎

    auto eng = std::make_unique<VideoEngine>();
    bool anyOpened = false;
    bool comOk = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

    for (const auto& v : renderer.getVideos()) {
        if (v.src.empty()) continue;
        std::string path = IsAbsolutePath(v.src) ? v.src
                         : (baseDir.empty() ? v.src : baseDir + "/" + v.src);
        auto s = std::make_unique<MediaStream>();
        // 伪图片槽编号空间兜底（setImageOrder 未覆盖时）
        renderer.reserveImageSlots((uint16_t)(v.imageIndex + 1));
        if (!s->start(&renderer, v.nodeId, v.imageIndex, path)) continue;
        double dur = s->fmt ? (s->fmt->duration == AV_NOPTS_VALUE
                               ? 0 : s->fmt->duration / (double)AV_TIME_BASE) : 0;
        if (dur > 0) renderer.setVideoDuration(v.nodeId, dur);
        eng->streams.push_back(std::move(s));
        anyOpened = true;
        std::printf("[binui_video] attached #%u -> %s (%.2fs)%s\n", v.nodeId,
                    path.c_str(), dur, comOk ? "" : " [COM init failed]");
    }

    gEngine = eng.release();
    return anyOpened;
}

void DetachVideoEngine() {
    VideoEngine* e = gEngine;
    gEngine = nullptr;
    if (e) delete e; // ~MediaStream join 全部线程并释放
}

} // namespace binui
