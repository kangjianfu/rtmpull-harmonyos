// pull_decoder.cpp — 原生解码器（NDK），NAPI 桥接到 ArkTS
// 视频：AVCC 帧 → H.264/H.265 硬解 → surface 直渲（XComponent，零拷贝）
// 音频：raw AAC → AAC 硬解 → PCM 经 tsfn 回调 ArkTS（ArkTS 侧 AudioRenderer 播放）
#include <napi/native_api.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avcodec_audiodecoder.h>
#include <multimedia/player_framework/native_avcodec_audiocodec.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avmemory.h>
#include <native_window/external_window.h>
#include <hilog/log.h>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#define LOG_TAG "PullDecoder"
#define LOGI(...) OH_LOG_INFO(LOG_APP, LOG_TAG ": " __VA_ARGS__)
#define LOGE(...) OH_LOG_ERROR(LOG_APP, LOG_TAG ": " __VA_ARGS__)

namespace {

// ---------------- 视频解码器（surface 直渲） ----------------

struct VideoFrame {
    std::vector<uint8_t> data;
    int64_t pts;
};

struct VideoDecoderCtx {
    OH_AVCodec *codec = nullptr;
    OHNativeWindow *window = nullptr;
    std::queue<VideoFrame> pending;
    std::mutex mtx;
    bool running = false;
    size_t queuedBytes = 0;
};

constexpr size_t MAX_VIDEO_QUEUED = 32 * 1024 * 1024; // 待解帧队列上限 32MB

VideoDecoderCtx g_video;

void OnVideoError(OH_AVCodec *codec, int32_t err, void *userData) {
    LOGE("video decoder error %{public}d", err);
}

void OnVideoStreamChanged(OH_AVCodec *codec, OH_AVFormat *format, void *userData) {}

void OnVideoNeedInput(OH_AVCodec *codec, uint32_t index, OH_AVMemory *data, void *userData) {
    std::lock_guard<std::mutex> lock(g_video.mtx);
    if (!g_video.running || g_video.pending.empty() || !data) {
        return;
    }
    VideoFrame frame = std::move(g_video.pending.front());
    g_video.pending.pop();
    g_video.queuedBytes -= frame.data.size();
    uint8_t *addr = OH_AVMemory_GetAddr(data);
    int32_t cap = OH_AVMemory_GetSize(data);
    if (static_cast<int32_t>(frame.data.size()) > cap) {
        LOGE("frame too big %{public}zu > %{public}d, dropped", frame.data.size(), cap);
        OH_AVCodecBufferAttr discard;
        discard.pts = 0;
        discard.size = 0;
        discard.offset = 0;
        discard.flags = AVCODEC_BUFFER_FLAGS_DISCARD;
        OH_VideoDecoder_PushInputData(codec, index, discard);
        return;
    }
    std::memcpy(addr, frame.data.data(), frame.data.size());
    OH_AVCodecBufferAttr attr;
    attr.pts = frame.pts;
    attr.size = frame.data.size();
    attr.offset = 0;
    attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
    OH_VideoDecoder_PushInputData(codec, index, attr);
}

void OnVideoOutput(OH_AVCodec *codec, uint32_t index, OH_AVMemory *data, OH_AVCodecBufferAttr *attr, void *userData) {
    if (!attr) {
        return;
    }
    if ((attr->flags & AVCODEC_BUFFER_FLAGS_DISCARD) != 0) {
        OH_VideoDecoder_FreeOutputBuffer(codec, index);
        return;
    }
    // surface 模式：渲染上屏（解码即显示，直播低延迟优先）
    OH_VideoDecoder_RenderOutputBuffer(codec, index);
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

    std::lock_guard<std::mutex> lock(g_video.mtx);
    g_video.pending = std::queue<VideoFrame>();
    g_video.queuedBytes = 0;

    OH_AVCodec *codec = OH_VideoDecoder_CreateByMime(mime);
    if (!codec) {
        napi_throw_error(env, nullptr, "本设备不支持该视频解码格式");
        return nullptr;
    }
    uint64_t surfaceId = std::stoull(surfaceIdStr);
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
        OH_VideoDecoder_Destroy(codec);
        napi_throw_error(env, nullptr, "视频解码器配置失败");
        return nullptr;
    }
    ret = OH_VideoDecoder_SetSurface(codec, window);
    if (ret != AV_ERR_OK) {
        LOGE("set surface failed %{public}d", ret);
        OH_VideoDecoder_Destroy(codec);
        napi_throw_error(env, nullptr, "设置渲染 surface 失败");
        return nullptr;
    }
    ret = OH_VideoDecoder_Prepare(codec);
    if (ret != AV_ERR_OK) {
        OH_VideoDecoder_Destroy(codec);
        napi_throw_error(env, nullptr, "视频解码器准备失败");
        return nullptr;
    }
    ret = OH_VideoDecoder_Start(codec);
    if (ret != AV_ERR_OK) {
        OH_VideoDecoder_Destroy(codec);
        napi_throw_error(env, nullptr, "视频解码器启动失败");
        return nullptr;
    }
    g_video.codec = codec;
    g_video.window = window;
    g_video.running = true;
    LOGI("video decoder started %{public}s %{public}dx%{public}d", mime, width, height);
    napi_value out;
    napi_get_boolean(env, true, &out);
    return out;
}

napi_value PushVideoFrame(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    void *data = nullptr;
    size_t len = 0;
    napi_get_arraybuffer_info(env, args[0], &data, &len);
    int64_t pts;
    napi_get_value_int64(env, args[1], &pts);

    std::lock_guard<std::mutex> lock(g_video.mtx);
    napi_value out;
    if (!g_video.running || !g_video.codec) {
        napi_get_boolean(env, false, &out);
        return out;
    }
    // 解码跟不上就丢（直播低延迟优先：宁丢不等）
    while (!g_video.pending.empty() && g_video.queuedBytes + len > MAX_VIDEO_QUEUED) {
        g_video.queuedBytes -= g_video.pending.front().data.size();
        g_video.pending.pop();
    }
    VideoFrame f;
    f.data.resize(len);
    std::memcpy(f.data.data(), data, len);
    f.pts = pts;
    g_video.pending.push(std::move(f));
    g_video.queuedBytes += len;
    napi_get_boolean(env, true, &out);
    return out;
}

// ---------------- 音频解码器（AAC → PCM → tsfn → ArkTS） ----------------

struct PcmEvent {
    std::vector<uint8_t> data;
    int64_t pts;
};

struct AudioDecoderCtx {
    OH_AVCodec *codec = nullptr;
    napi_threadsafe_function tsfn = nullptr;
    std::queue<VideoFrame> pending; // 复用结构（data=raw AAC）
    std::mutex mtx;
    bool running = false;
};

AudioDecoderCtx g_audio;

void OnAudioError(OH_AVCodec *codec, int32_t err, void *userData) {
    LOGE("audio decoder error %{public}d", err);
}

void OnAudioStreamChanged(OH_AVCodec *codec, OH_AVFormat *format, void *userData) {}

void OnAudioNeedInput(OH_AVCodec *codec, uint32_t index, OH_AVMemory *data, void *userData) {
    std::lock_guard<std::mutex> lock(g_audio.mtx);
    if (!g_audio.running || g_audio.pending.empty() || !data) {
        return;
    }
    VideoFrame frame = std::move(g_audio.pending.front());
    g_audio.pending.pop();
    uint8_t *addr = OH_AVMemory_GetAddr(data);
    int32_t cap = OH_AVMemory_GetSize(data);
    int32_t len = std::min(static_cast<int32_t>(frame.data.size()), cap);
    std::memcpy(addr, frame.data.data(), len);
    OH_AVCodecBufferAttr attr;
    attr.pts = frame.pts;
    attr.size = len;
    attr.offset = 0;
    attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
    OH_AudioDecoder_PushInputData(codec, index, attr);
}

void CallArkTsPcm(napi_env env, napi_value jsCb, void *context, void *data) {
    PcmEvent *ev = static_cast<PcmEvent *>(data);
    if (!ev) {
        return;
    }
    napi_value buffer;
    void *bufData = nullptr;
    napi_create_arraybuffer(env, ev->data.size(), &bufData, &buffer);
    std::memcpy(bufData, ev->data.data(), ev->data.size());
    napi_value pts;
    napi_create_int64(env, ev->pts, &pts);
    napi_value argv[2] = { buffer, pts };
    napi_call_function(env, nullptr, jsCb, 2, argv, nullptr);
    delete ev;
}

void OnAudioOutput(OH_AVCodec *codec, uint32_t index, OH_AVMemory *data, OH_AVCodecBufferAttr *attr, void *userData) {
    if (!attr || !data || !g_audio.tsfn) {
        return;
    }
    uint8_t *addr = OH_AVMemory_GetAddr(data);
    PcmEvent *ev = new PcmEvent();
    ev->pts = attr->pts;
    ev->data.assign(addr + attr->offset, addr + attr->offset + attr->size);
    if (napi_call_threadsafe_function(g_audio.tsfn, ev, napi_tsfn_nonblocking) != napi_ok) {
        delete ev;
    }
    OH_AudioCodec_FreeOutputBuffer(codec, index);
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

    std::lock_guard<std::mutex> lock(g_audio.mtx);
    g_audio.pending = std::queue<VideoFrame>();

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
    g_audio.codec = codec;
    g_audio.running = true;
    LOGI("audio decoder started rate=%{public}d ch=%{public}d", sampleRate, channels);
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
    if (g_audio.pending.size() >= 300) {
        g_audio.pending.pop();
    }
    VideoFrame f;
    f.data.resize(len);
    std::memcpy(f.data.data(), data, len);
    f.pts = pts;
    g_audio.pending.push(std::move(f));
    napi_get_boolean(env, true, &out);
    return out;
}

napi_value SetPcmCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (g_audio.tsfn) {
        napi_release_threadsafe_function(g_audio.tsfn, napi_tsfn_release);
        g_audio.tsfn = nullptr;
    }
    napi_value name;
    napi_create_string_utf8(env, "pull_decoder_pcm", NAPI_AUTO_LENGTH, &name);
    napi_create_threadsafe_function(env, args[0], nullptr, name, 300, 1, nullptr, nullptr, nullptr, CallArkTsPcm,
                                    &g_audio.tsfn);
    napi_value out;
    napi_get_boolean(env, true, &out);
    return out;
}

napi_value StopAll(napi_env env, napi_callback_info info) {
    {
        std::lock_guard<std::mutex> lock(g_video.mtx);
        g_video.running = false;
        if (g_video.codec) {
            OH_VideoDecoder_Stop(g_video.codec);
            OH_VideoDecoder_Destroy(g_video.codec);
            g_video.codec = nullptr;
        }
        g_video.window = nullptr;
        g_video.pending = std::queue<VideoFrame>();
        g_video.queuedBytes = 0;
    }
    {
        std::lock_guard<std::mutex> lock(g_audio.mtx);
        g_audio.running = false;
        if (g_audio.codec) {
            OH_AudioDecoder_Stop(g_audio.codec);
            OH_AudioDecoder_Destroy(g_audio.codec);
            g_audio.codec = nullptr;
        }
        g_audio.pending = std::queue<VideoFrame>();
    }
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
        { "setPcmCallback", nullptr, SetPcmCallback, nullptr, nullptr, nullptr, napi_default, nullptr },
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
