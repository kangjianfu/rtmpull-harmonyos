// pull_decoder.cpp — 原生解码器（NDK），NAPI 桥接到 ArkTS
// 视频：AVCC 帧 → H.264/H.265 硬解 → surface 直渲（XComponent，零拷贝），按音频时钟丢弃严重迟到的帧
// 音频：raw AAC → AAC 硬解 → PCM 直喂 native OHAudio 渲染器（不经 ArkTS 中转，低延迟）
#include <napi/native_api.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avcodec_audiodecoder.h>
#include <multimedia/player_framework/native_avcodec_audiocodec.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avmemory.h>
#include <native_window/external_window.h>
#include <ohaudio/native_audiostreambuilder.h>
#include <hilog/log.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#define LOG_TAG "PullDecoder"
#define LOGI(...) OH_LOG_INFO(LOG_APP, LOG_TAG ": " __VA_ARGS__)
#define LOGE(...) OH_LOG_ERROR(LOG_APP, LOG_TAG ": " __VA_ARGS__)

namespace {

int64_t SteadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------- 视频解码器（surface 直渲） ----------------

struct VideoFrame {
    std::vector<uint8_t> data;
    int64_t pts = 0;
    bool isKey = false;
};

// 空闲输入 buffer 槽：回调到来但暂无数据时暂存，数据到达后立即消费，避免解码器饿死
struct InputSlot {
    uint32_t index;
    OH_AVMemory *memory;
};

struct VideoDecoderCtx {
    OH_AVCodec *codec = nullptr;
    OHNativeWindow *window = nullptr;
    std::deque<VideoFrame> pending;
    std::vector<InputSlot> freeInputs;
    std::mutex mtx;
    bool running = false;
    size_t queuedBytes = 0;
};

constexpr size_t MAX_VIDEO_QUEUED = 32 * 1024 * 1024; // 待解帧队列上限 32MB
constexpr int64_t MAX_VIDEO_LATE_US = 120 * 1000;     // 视频帧晚于音频时钟 120ms 则不渲染
constexpr int64_t CLOCK_STALE_MS = 1500;              // 音频时钟超过 1.5s 未推进视为失效

VideoDecoderCtx g_video;

// ---------------- 音频播放器（PCM → native OHAudio） ----------------

struct PcmEvent {
    std::vector<uint8_t> data;
    int64_t pts = 0; // us，FLV 时间线（与视频帧 pts 同源）
    size_t off = 0;  // 已播放到事件内的字节偏移
};

struct AudioPlayerCtx {
    OH_AudioRenderer *renderer = nullptr;
    std::deque<PcmEvent> queue;
    std::mutex mtx;
    std::atomic<int64_t> clockUs{0};        // 音频播放时钟（us，FLV 时间线）
    std::atomic<int64_t> clockSteadyMs{0};  // 时钟最后一次推进对应的 steady 时间
    std::atomic<bool> clockValid{false};
    int64_t bytesPerSec = 0;
    bool running = false;
};

constexpr size_t MAX_PCM_EVENTS = 10;          // 待播 PCM 上限（约 200ms），超出丢最旧
constexpr int64_t CLOCK_RESYNC_US = 500 * 1000; // 时钟与流 pts 偏差超 500ms 时重同步

AudioPlayerCtx g_player;

// OHAudio 数据回调（渲染线程拉取）：队列出 PCM，欠载补静音，并推进音频时钟
int32_t OnPcmWriteData(OH_AudioRenderer *renderer, void *userData, void *buffer, int32_t length) {
    uint8_t *dst = static_cast<uint8_t *>(buffer);
    int64_t ptsBase = -1; // 本次写入的第一个字节在流时间线上的 pts（-1 = 全是静音）
    {
        std::lock_guard<std::mutex> lock(g_player.mtx);
        int32_t remaining = length;
        while (remaining > 0 && !g_player.queue.empty()) {
            PcmEvent &ev = g_player.queue.front();
            size_t avail = ev.data.size() - ev.off;
            size_t n = avail < static_cast<size_t>(remaining) ? avail : static_cast<size_t>(remaining);
            std::memcpy(dst, ev.data.data() + ev.off, n);
            if (ptsBase < 0 && g_player.bytesPerSec > 0) {
                ptsBase = ev.pts + static_cast<int64_t>(ev.off) * 1000000 / g_player.bytesPerSec;
            }
            ev.off += n;
            dst += n;
            remaining -= static_cast<int32_t>(n);
            if (ev.off >= ev.data.size()) {
                g_player.queue.pop_front();
            }
        }
        if (remaining > 0) {
            std::memset(dst, 0, remaining); // 欠载补静音
        }
    }
    if (g_player.bytesPerSec > 0) {
        int64_t durUs = static_cast<int64_t>(length) * 1000000 / g_player.bytesPerSec;
        int64_t base = g_player.clockUs.load();
        int64_t diff = ptsBase - base;
        if (ptsBase >= 0 && (!g_player.clockValid.load() || diff > CLOCK_RESYNC_US || diff < -CLOCK_RESYNC_US)) {
            g_player.clockUs.store(ptsBase + durUs); // 重同步到流时间线
            g_player.clockValid.store(true);
            g_player.clockSteadyMs.store(SteadyNowMs());
        } else if (g_player.clockValid.load()) {
            g_player.clockUs.store(base + durUs); // 含静音段：静音同样占用播放时长
            g_player.clockSteadyMs.store(SteadyNowMs());
        }
    }
    return 0;
}

// 调用方不持 g_player.mtx
void StopAudioPlayer() {
    OH_AudioRenderer *r = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_player.mtx);
        r = g_player.renderer;
        g_player.renderer = nullptr;
        g_player.running = false;
        g_player.queue.clear();
    }
    if (r) {
        OH_AudioRenderer_Stop(r);
        OH_AudioRenderer_Release(r);
    }
    g_player.clockValid.store(false);
    g_player.clockUs.store(0);
}

// 返回的 renderer 由调用方发布到 g_player；失败返回 nullptr
OH_AudioRenderer *CreateAudioRendererInst(int32_t sampleRate, int32_t channels) {
    OH_AudioStreamBuilder *builder = nullptr;
    if (OH_AudioStreamBuilder_Create(&builder, AUDIOSTREAM_TYPE_RENDERER) != AUDIOSTREAM_SUCCESS || !builder) {
        LOGE("create audio stream builder failed");
        return nullptr;
    }
    OH_AudioStreamBuilder_SetSamplingRate(builder, sampleRate);
    OH_AudioStreamBuilder_SetChannelCount(builder, channels);
    OH_AudioStreamBuilder_SetSampleFormat(builder, AUDIOSTREAM_SAMPLE_S16LE);
    OH_AudioStreamBuilder_SetEncodingType(builder, AUDIOSTREAM_ENCODING_TYPE_RAW);
    OH_AudioStreamBuilder_SetLatencyMode(builder, AUDIOSTREAM_LATENCY_MODE_FAST); // 直播低延迟
    OH_AudioStreamBuilder_SetRendererInfo(builder, AUDIOSTREAM_USAGE_MEDIA);
    OH_AudioRenderer_Callbacks cbs = { OnPcmWriteData, nullptr, nullptr, nullptr };
    OH_AudioStreamBuilder_SetRendererCallback(builder, cbs, nullptr);
    OH_AudioRenderer *renderer = nullptr;
    OH_AudioStream_Result res = OH_AudioStreamBuilder_GenerateRenderer(builder, &renderer);
    OH_AudioStreamBuilder_Destroy(builder);
    if (res != AUDIOSTREAM_SUCCESS || !renderer) {
        LOGE("generate audio renderer failed %{public}d", res);
        return nullptr;
    }
    if (OH_AudioRenderer_Start(renderer) != AUDIOSTREAM_SUCCESS) {
        LOGE("start audio renderer failed");
        OH_AudioRenderer_Release(renderer);
        return nullptr;
    }
    return renderer;
}

// ---------------- 视频解码器回调 ----------------

// 调用方须持有 g_video.mtx
void DrainVideoLocked() {
    while (g_video.running && g_video.codec && !g_video.pending.empty() && !g_video.freeInputs.empty()) {
        InputSlot slot = g_video.freeInputs.front();
        g_video.freeInputs.erase(g_video.freeInputs.begin());
        VideoFrame frame = std::move(g_video.pending.front());
        g_video.pending.pop_front();
        g_video.queuedBytes -= frame.data.size();
        OH_AVCodecBufferAttr attr;
        attr.pts = frame.pts;
        attr.offset = 0;
        uint8_t *addr = slot.memory ? OH_AVMemory_GetAddr(slot.memory) : nullptr;
        int32_t cap = slot.memory ? OH_AVMemory_GetSize(slot.memory) : 0;
        if (!addr || static_cast<int32_t>(frame.data.size()) > cap) {
            LOGE("video frame too big %{public}zu > %{public}d, dropped", frame.data.size(), cap);
            attr.size = 0;
            attr.flags = AVCODEC_BUFFER_FLAGS_DISCARD;
        } else {
            std::memcpy(addr, frame.data.data(), frame.data.size());
            attr.size = frame.data.size();
            attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
        }
        OH_VideoDecoder_PushInputData(g_video.codec, slot.index, attr);
    }
}

void OnVideoError(OH_AVCodec *codec, int32_t err, void *userData) {
    LOGE("video decoder error %{public}d", err);
}

void OnVideoStreamChanged(OH_AVCodec *codec, OH_AVFormat *format, void *userData) {}

void OnVideoNeedInput(OH_AVCodec *codec, uint32_t index, OH_AVMemory *data, void *userData) {
    std::lock_guard<std::mutex> lock(g_video.mtx);
    if (!g_video.running || !data) {
        return;
    }
    g_video.freeInputs.push_back({ index, data });
    DrainVideoLocked();
}

void OnVideoOutput(OH_AVCodec *codec, uint32_t index, OH_AVMemory *data, OH_AVCodecBufferAttr *attr, void *userData) {
    if (!attr) {
        return;
    }
    if ((attr->flags & AVCODEC_BUFFER_FLAGS_DISCARD) != 0) {
        OH_VideoDecoder_FreeOutputBuffer(codec, index);
        return;
    }
    // 音画同步：晚于音频时钟太多直接不渲染（音频时钟失效/无音频流时全部渲染）
    if (g_player.clockValid.load() &&
        SteadyNowMs() - g_player.clockSteadyMs.load() < CLOCK_STALE_MS &&
        attr->pts + MAX_VIDEO_LATE_US < g_player.clockUs.load()) {
        OH_VideoDecoder_FreeOutputBuffer(codec, index);
        return;
    }
    // surface 模式：渲染上屏
    OH_VideoDecoder_RenderOutputBuffer(codec, index);
}

// 取出旧实例并复位状态（持锁），返回的旧资源由调用方在锁外销毁
void ResetVideoCtx(OH_AVCodec **oldCodec, OHNativeWindow **oldWindow) {
    std::lock_guard<std::mutex> lock(g_video.mtx);
    g_video.running = false;
    *oldCodec = g_video.codec;
    g_video.codec = nullptr;
    *oldWindow = g_video.window;
    g_video.window = nullptr;
    g_video.pending.clear();
    g_video.freeInputs.clear();
    g_video.queuedBytes = 0;
}

void DestroyVideoInst(OH_AVCodec *codec, OHNativeWindow *window) {
    if (codec) {
        OH_VideoDecoder_Stop(codec);
        OH_VideoDecoder_Destroy(codec);
    }
    if (window) {
        OH_NativeWindow_DestroyNativeWindow(window);
    }
}

napi_value CreateVideoDecoder(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value args[5];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    // 参数：surfaceId(string), mime(string), width, height, codecConfig(arraybuffer)
    char surfaceIdStr[32];
    size_t sidLen = 0;
    napi_get_value_string_utf8(env, args[0], surfaceIdStr, sizeof(surfaceIdStr), &sidLen);
    char mime[32];
    size_t mimeLen = 0;
    napi_get_value_string_utf8(env, args[1], mime, sizeof(mime), &mimeLen);
    int32_t width, height;
    napi_get_value_int32(env, args[2], &width);
    napi_get_value_int32(env, args[3], &height);
    void *cfgData = nullptr;
    size_t cfgLen = 0;
    napi_get_arraybuffer_info(env, args[4], &cfgData, &cfgLen);

    // 重播场景：先停掉旧解码器（锁内摘指针，锁外销毁，避免与回调线程死锁）
    OH_AVCodec *oldCodec = nullptr;
    OHNativeWindow *oldWindow = nullptr;
    ResetVideoCtx(&oldCodec, &oldWindow);
    DestroyVideoInst(oldCodec, oldWindow);

    uint64_t surfaceId = 0;
    try {
        surfaceId = std::stoull(surfaceIdStr);
    } catch (...) {
        napi_throw_error(env, nullptr, "surfaceId 非法");
        return nullptr;
    }

    OH_AVCodec *codec = OH_VideoDecoder_CreateByMime(mime);
    if (!codec) {
        napi_throw_error(env, nullptr, "本设备不支持该视频解码格式");
        return nullptr;
    }
    OHNativeWindow *window = nullptr;
    int32_t wret = OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId, &window);
    if (wret != 0 || !window) {
        OH_VideoDecoder_Destroy(codec);
        napi_throw_error(env, nullptr, "创建渲染窗口失败");
        return nullptr;
    }
    OH_AVCodecAsyncCallback cb = { OnVideoError, OnVideoStreamChanged, OnVideoNeedInput, OnVideoOutput };
    OH_VideoDecoder_SetCallback(codec, cb, nullptr);

    OH_AVFormat *format = OH_AVFormat_CreateVideoFormat(mime, width, height);
    if (cfgData && cfgLen > 0) {
        OH_AVFormat_SetBuffer(format, OH_MD_KEY_CODEC_CONFIG, static_cast<uint8_t *>(cfgData), cfgLen);
    }
    OH_AVErrCode ret = OH_VideoDecoder_Configure(codec, format);
    OH_AVFormat_Destroy(format);
    if (ret != AV_ERR_OK) {
        LOGE("video configure failed %{public}d", ret);
        DestroyVideoInst(codec, window);
        napi_throw_error(env, nullptr, "视频解码器配置失败");
        return nullptr;
    }
    ret = OH_VideoDecoder_SetSurface(codec, window);
    if (ret != AV_ERR_OK) {
        LOGE("set surface failed %{public}d", ret);
        DestroyVideoInst(codec, window);
        napi_throw_error(env, nullptr, "设置渲染 surface 失败");
        return nullptr;
    }
    ret = OH_VideoDecoder_Prepare(codec);
    if (ret != AV_ERR_OK) {
        DestroyVideoInst(codec, window);
        napi_throw_error(env, nullptr, "视频解码器准备失败");
        return nullptr;
    }
    ret = OH_VideoDecoder_Start(codec);
    if (ret != AV_ERR_OK) {
        DestroyVideoInst(codec, window);
        napi_throw_error(env, nullptr, "视频解码器启动失败");
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(g_video.mtx);
        g_video.codec = codec;
        g_video.window = window;
        g_video.running = true;
    }
    LOGI("video decoder started %{public}s %{public}dx%{public}d", mime, width, height);
    napi_value out;
    napi_get_boolean(env, true, &out);
    return out;
}

napi_value PushVideoFrame(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    void *data = nullptr;
    size_t len = 0;
    napi_get_arraybuffer_info(env, args[0], &data, &len);
    int64_t pts;
    napi_get_value_int64(env, args[1], &pts);
    bool isKey = false;
    napi_get_value_bool(env, args[2], &isKey);

    std::lock_guard<std::mutex> lock(g_video.mtx);
    napi_value out;
    if (!g_video.running || !g_video.codec) {
        napi_get_boolean(env, false, &out);
        return out;
    }
    // 解码跟不上就丢（直播低延迟优先：宁丢不等）。
    // 优先丢队首非关键帧；若队首是关键帧，则整 GOP 丢到下一个关键帧（丢关键帧会花屏整个 GOP）
    while (!g_video.pending.empty() && g_video.queuedBytes + len > MAX_VIDEO_QUEUED) {
        if (!g_video.pending.front().isKey) {
            g_video.queuedBytes -= g_video.pending.front().data.size();
            g_video.pending.pop_front();
            continue;
        }
        size_t nextKey = g_video.pending.size();
        for (size_t i = 1; i < g_video.pending.size(); i++) {
            if (g_video.pending[i].isKey) {
                nextKey = i;
                break;
            }
        }
        if (nextKey >= g_video.pending.size()) {
            break; // 队列里只有这一个 GOP，丢无可丢
        }
        for (size_t i = 0; i < nextKey; i++) {
            g_video.queuedBytes -= g_video.pending.front().data.size();
            g_video.pending.pop_front();
        }
    }
    VideoFrame f;
    f.data.resize(len);
    std::memcpy(f.data.data(), data, len);
    f.pts = pts;
    f.isKey = isKey;
    g_video.pending.push_back(std::move(f));
    g_video.queuedBytes += len;
    DrainVideoLocked();
    napi_get_boolean(env, true, &out);
    return out;
}

// ---------------- 音频解码器（AAC → PCM → native OHAudio） ----------------

struct AudioDecoderCtx {
    OH_AVCodec *codec = nullptr;
    std::deque<VideoFrame> pending; // 复用结构（data=raw AAC）
    std::vector<InputSlot> freeInputs;
    std::mutex mtx;
    bool running = false;
};

constexpr size_t MAX_AUDIO_QUEUED = 100; // 待解音频帧上限（约 2 秒），超出丢最旧

AudioDecoderCtx g_audio;

// 调用方须持有 g_audio.mtx
void DrainAudioLocked() {
    while (g_audio.running && g_audio.codec && !g_audio.pending.empty() && !g_audio.freeInputs.empty()) {
        InputSlot slot = g_audio.freeInputs.front();
        g_audio.freeInputs.erase(g_audio.freeInputs.begin());
        VideoFrame frame = std::move(g_audio.pending.front());
        g_audio.pending.pop_front();
        OH_AVCodecBufferAttr attr;
        attr.pts = frame.pts;
        attr.offset = 0;
        uint8_t *addr = slot.memory ? OH_AVMemory_GetAddr(slot.memory) : nullptr;
        int32_t cap = slot.memory ? OH_AVMemory_GetSize(slot.memory) : 0;
        if (!addr || static_cast<int32_t>(frame.data.size()) > cap) {
            // 截断的 AAC 帧是废帧，整帧丢弃
            LOGE("audio frame too big %{public}zu > %{public}d, dropped", frame.data.size(), cap);
            attr.size = 0;
            attr.flags = AVCODEC_BUFFER_FLAGS_DISCARD;
        } else {
            std::memcpy(addr, frame.data.data(), frame.data.size());
            attr.size = frame.data.size();
            attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
        }
        OH_AudioDecoder_PushInputData(g_audio.codec, slot.index, attr);
    }
}

void OnAudioError(OH_AVCodec *codec, int32_t err, void *userData) {
    LOGE("audio decoder error %{public}d", err);
}

void OnAudioStreamChanged(OH_AVCodec *codec, OH_AVFormat *format, void *userData) {}

void OnAudioNeedInput(OH_AVCodec *codec, uint32_t index, OH_AVMemory *data, void *userData) {
    std::lock_guard<std::mutex> lock(g_audio.mtx);
    if (!g_audio.running || !data) {
        return;
    }
    g_audio.freeInputs.push_back({ index, data });
    DrainAudioLocked();
}

void OnAudioOutput(OH_AVCodec *codec, uint32_t index, OH_AVMemory *data, OH_AVCodecBufferAttr *attr, void *userData) {
    if (!attr || !data) {
        return;
    }
    // PCM 直接入 native 播放队列（不经 ArkTS 中转）
    {
        std::lock_guard<std::mutex> lock(g_player.mtx);
        if (g_player.running && attr->size > 0 &&
            (attr->flags & AVCODEC_BUFFER_FLAGS_DISCARD) == 0) {
            if (g_player.queue.size() >= MAX_PCM_EVENTS) {
                g_player.queue.pop_front(); // 播放跟不上丢最旧，直播保实时
            }
            uint8_t *addr = OH_AVMemory_GetAddr(data);
            PcmEvent ev;
            ev.pts = attr->pts;
            ev.data.assign(addr + attr->offset, addr + attr->offset + attr->size);
            g_player.queue.push_back(std::move(ev));
        }
    }
    OH_AudioCodec_FreeOutputBuffer(codec, index);
}

void ResetAudioCtx(OH_AVCodec **oldCodec) {
    std::lock_guard<std::mutex> lock(g_audio.mtx);
    g_audio.running = false;
    *oldCodec = g_audio.codec;
    g_audio.codec = nullptr;
    g_audio.pending.clear();
    g_audio.freeInputs.clear();
}

napi_value CreateAudioDecoder(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t sampleRate, channels;
    napi_get_value_int32(env, args[0], &sampleRate);
    napi_get_value_int32(env, args[1], &channels);
    void *ascData = nullptr;
    size_t ascLen = 0;
    napi_get_arraybuffer_info(env, args[2], &ascData, &ascLen);

    // 重播场景：先停掉旧解码器与旧播放器
    OH_AVCodec *oldCodec = nullptr;
    ResetAudioCtx(&oldCodec);
    if (oldCodec) {
        OH_AudioDecoder_Stop(oldCodec);
        OH_AudioDecoder_Destroy(oldCodec);
    }
    StopAudioPlayer();

    OH_AVCodec *codec = OH_AudioDecoder_CreateByMime(OH_AVCODEC_MIMETYPE_AUDIO_AAC);
    if (!codec) {
        napi_throw_error(env, nullptr, "本设备不支持 AAC 解码");
        return nullptr;
    }
    OH_AVFormat *format = OH_AVFormat_CreateAudioFormat(OH_AVCODEC_MIMETYPE_AUDIO_AAC, sampleRate, channels);
    if (ascData && ascLen > 0) {
        OH_AVFormat_SetBuffer(format, OH_MD_KEY_CODEC_CONFIG, static_cast<uint8_t *>(ascData), ascLen);
    }
    OH_AVCodecAsyncCallback cb = { OnAudioError, OnAudioStreamChanged, OnAudioNeedInput, OnAudioOutput };
    OH_AudioDecoder_SetCallback(codec, cb, nullptr);
    OH_AVErrCode ret = OH_AudioDecoder_Configure(codec, format);
    OH_AVFormat_Destroy(format);
    if (ret != AV_ERR_OK) {
        OH_AudioDecoder_Destroy(codec);
        napi_throw_error(env, nullptr, "音频解码器配置失败");
        return nullptr;
    }
    ret = OH_AudioDecoder_Prepare(codec);
    if (ret != AV_ERR_OK) {
        OH_AudioDecoder_Destroy(codec);
        napi_throw_error(env, nullptr, "音频解码器准备失败");
        return nullptr;
    }
    ret = OH_AudioDecoder_Start(codec);
    if (ret != AV_ERR_OK) {
        OH_AudioDecoder_Destroy(codec);
        napi_throw_error(env, nullptr, "音频解码器启动失败");
        return nullptr;
    }
    OH_AudioRenderer *renderer = CreateAudioRendererInst(sampleRate, channels);
    if (!renderer) {
        OH_AudioDecoder_Stop(codec);
        OH_AudioDecoder_Destroy(codec);
        napi_throw_error(env, nullptr, "音频播放器启动失败");
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(g_audio.mtx);
        g_audio.codec = codec;
        g_audio.running = true;
    }
    {
        std::lock_guard<std::mutex> lock(g_player.mtx);
        g_player.renderer = renderer;
        g_player.bytesPerSec = static_cast<int64_t>(sampleRate) * channels * 2; // S16LE
        g_player.running = true;
    }
    LOGI("audio pipeline started rate=%{public}d ch=%{public}d", sampleRate, channels);
    napi_value out;
    napi_get_boolean(env, true, &out);
    return out;
}

napi_value PushAudioFrame(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    void *data = nullptr;
    size_t len = 0;
    napi_get_arraybuffer_info(env, args[0], &data, &len);
    int64_t pts;
    napi_get_value_int64(env, args[1], &pts);

    std::lock_guard<std::mutex> lock(g_audio.mtx);
    napi_value out;
    if (!g_audio.running || !g_audio.codec) {
        napi_get_boolean(env, false, &out);
        return out;
    }
    if (g_audio.pending.size() >= MAX_AUDIO_QUEUED) {
        g_audio.pending.pop_front(); // 解码跟不上丢最旧
    }
    VideoFrame f;
    f.data.resize(len);
    std::memcpy(f.data.data(), data, len);
    f.pts = pts;
    g_audio.pending.push_back(std::move(f));
    DrainAudioLocked();
    napi_get_boolean(env, true, &out);
    return out;
}

napi_value StopAll(napi_env env, napi_callback_info info) {
    // 锁内只摘指针、置状态，锁外停/销毁，避免与回调线程死锁
    OH_AVCodec *videoCodec = nullptr;
    OHNativeWindow *window = nullptr;
    ResetVideoCtx(&videoCodec, &window);
    OH_AVCodec *audioCodec = nullptr;
    ResetAudioCtx(&audioCodec);

    DestroyVideoInst(videoCodec, window);
    if (audioCodec) {
        OH_AudioDecoder_Stop(audioCodec);
        OH_AudioDecoder_Destroy(audioCodec);
    }
    StopAudioPlayer();
    napi_value out;
    napi_get_boolean(env, true, &out);
    return out;
}

napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        { "createVideoDecoder", nullptr, CreateVideoDecoder, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "pushVideoFrame", nullptr, PushVideoFrame, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "createAudioDecoder", nullptr, CreateAudioDecoder, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "pushAudioFrame", nullptr, PushAudioFrame, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "stopAll", nullptr, StopAll, nullptr, nullptr, nullptr, napi_default, nullptr },
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}

} // namespace

extern "C" __attribute__((constructor)) void RegisterPullDecoderModule() {
    static napi_module pullDecoderModule = {
        .nm_version = 1,
        .nm_flags = 0,
        .nm_filename = nullptr,
        .nm_register_func = Init,
        .nm_modname = "pull_decoder",
        .nm_priv = nullptr,
        .reserved = { 0 },
    };
    napi_module_register(&pullDecoderModule);
}
