// PullPlayer.ets — 拉流播放器封装：FlvPullClient 拉流解封装 → native 硬解 → XComponent 直渲 + AudioRenderer 出声
// 用法：setSurfaceId（XComponent onLoad 时）→ play(url) → stop()
import { audio } from '@kit.AudioKit';
import pullDecoder from 'libpull_decoder.so';
import { FlvPullClient, VideoCodecName } from './FlvPullClient';

export type PlayerLog = (msg: string) => void;

interface NativePullDecoder {
  createVideoDecoder(surfaceId: string, mime: string, width: number, height: number, codecConfig: ArrayBuffer): boolean;
  pushVideoFrame(data: ArrayBuffer, ptsUs: number): boolean;
  createAudioDecoder(sampleRate: number, channels: number, asc: ArrayBuffer): boolean;
  pushAudioFrame(data: ArrayBuffer, ptsUs: number): boolean;
  setPcmCallback(cb: (pcm: ArrayBuffer, ptsUs: number) => void): boolean;
  stopAll(): boolean;
}

const decoder: NativePullDecoder = pullDecoder as NativePullDecoder;

const FREQ_TABLE: number[] = [96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000];

function sampleRateEnum(rate: number): audio.AudioSamplingRate {
  switch (rate) {
    case 8000: return audio.AudioSamplingRate.SAMPLE_RATE_8000;
    case 11025: return audio.AudioSamplingRate.SAMPLE_RATE_11025;
    case 12000: return audio.AudioSamplingRate.SAMPLE_RATE_12000;
    case 16000: return audio.AudioSamplingRate.SAMPLE_RATE_16000;
    case 22050: return audio.AudioSamplingRate.SAMPLE_RATE_22050;
    case 24000: return audio.AudioSamplingRate.SAMPLE_RATE_24000;
    case 32000: return audio.AudioSamplingRate.SAMPLE_RATE_32000;
    case 48000: return audio.AudioSamplingRate.SAMPLE_RATE_48000;
    case 64000: return audio.AudioSamplingRate.SAMPLE_RATE_64000;
    case 88200: return audio.AudioSamplingRate.SAMPLE_RATE_88200;
    case 96000: return audio.AudioSamplingRate.SAMPLE_RATE_96000;
    default: return audio.AudioSamplingRate.SAMPLE_RATE_44100;
  }
}

export class PullPlayer {
  onLog: PlayerLog = () => {};
  onClosed: () => void = () => {}; // 流断开（正常 stop 不触发）

  private client: FlvPullClient = new FlvPullClient();
  private surfaceId: string = '';
  private videoStarted: boolean = false;
  private audioStarted: boolean = false;
  private metaW: number = 0;
  private metaH: number = 0;
  private renderer: audio.AudioRenderer | null = null;
  private pcmQueue: ArrayBuffer[] = [];
  private playing: boolean = false;

  /** XComponent onLoad 时传入 surfaceId */
  setSurfaceId(id: string): void {
    this.surfaceId = id;
  }

  getReceiveRateKbps(): number {
    return this.client.getReceiveRateKbps();
  }

  isPlaying(): boolean {
    return this.playing;
  }

  async play(url: string): Promise<void> {
    if (!this.surfaceId) {
      throw new Error('请先 setSurfaceId（XComponent onLoad 回调）');
    }
    this.stopDecoderOnly();
    this.playing = true;
    const client = new FlvPullClient();
    this.client = client;
    client.onLog = (msg: string) => {
      this.onLog(msg);
    };
    client.events = {
      onLog: (msg: string) => {
        this.onLog(msg);
      },
      onVideoConfig: (codec: VideoCodecName, config: Uint8Array) => {
        this.startVideo(codec, config);
      },
      onVideoFrame: (data: Uint8Array, ptsMs: number, isKey: boolean) => {
        if (this.videoStarted) {
          // subarray → 独立 ArrayBuffer（native 按整个 buffer 读）
          decoder.pushVideoFrame(data.slice().buffer as ArrayBuffer, ptsMs * 1000);
        }
      },
      onAudioConfig: (asc: Uint8Array) => {
        this.startAudio(asc);
      },
      onAudioFrame: (data: Uint8Array, ptsMs: number) => {
        if (this.audioStarted) {
          decoder.pushAudioFrame(data.slice().buffer as ArrayBuffer, ptsMs * 1000);
        }
      },
      onMeta: (w: number, h: number) => {
        this.metaW = w;
        this.metaH = h;
        this.onLog(`流信息 ${w}x${h}`);
      },
      onClosed: () => {
        if (this.playing) {
          this.onClosed();
        }
      }
    };
    await client.connect(url);
  }

  private startVideo(codec: VideoCodecName, config: Uint8Array): void {
    if (this.videoStarted) {
      return;
    }
    const mime = codec === 'hevc' ? 'video/hevc' : 'video/avc';
    const w = this.metaW > 0 ? this.metaW : 1280;
    const h = this.metaH > 0 ? this.metaH : 720;
    decoder.createVideoDecoder(this.surfaceId, mime, w, h, config.slice().buffer as ArrayBuffer);
    this.videoStarted = true;
    this.onLog(`视频解码器已启动（${codec === 'hevc' ? 'H.265' : 'H.264'} ${w}x${h}）`);
  }

  private async startAudio(asc: Uint8Array): Promise<void> {
    if (this.audioStarted || asc.length < 2) {
      return;
    }
    this.audioStarted = true;
    const rateIdx = ((asc[0] & 0x07) << 1) | (asc[1] >> 7);
    const rate = rateIdx < FREQ_TABLE.length ? FREQ_TABLE[rateIdx] : 44100;
    const ch = ((asc[1] >> 3) & 0x0f) || 1;
    decoder.createAudioDecoder(rate, ch, asc.slice().buffer as ArrayBuffer);
    this.onLog(`音频解码器已启动（AAC ${rate}Hz ${ch}ch）`);

    const streamInfo: audio.AudioStreamInfo = {
      samplingRate: sampleRateEnum(rate),
      channels: ch === 1 ? audio.AudioChannel.CHANNEL_1 : audio.AudioChannel.CHANNEL_2,
      sampleFormat: audio.AudioSampleFormat.SAMPLE_FORMAT_S16LE,
      encodingType: audio.AudioEncodingType.ENCODING_TYPE_RAW
    };
    const rendererInfo: audio.AudioRendererInfo = {
      usage: audio.StreamUsage.STREAM_USAGE_MEDIA,
      rendererFlags: 0
    };
    const renderer = await audio.createAudioRenderer({
      streamInfo: streamInfo,
      rendererInfo: rendererInfo
    });
    this.renderer = renderer;
    // PCM 供给：tsfn 回调入队，renderer 拉取（音频时钟驱动播放节奏）
    decoder.setPcmCallback((pcm: ArrayBuffer, ptsUs: number) => {
      if (this.pcmQueue.length >= 50) {
        this.pcmQueue.shift();
      }
      this.pcmQueue.push(pcm);
    });
    renderer.on('writeData', (data: ArrayBuffer) => {
      const next = this.pcmQueue.shift();
      const view = new Uint8Array(data);
      if (next) {
        view.set(new Uint8Array(next).subarray(0, view.length));
      } else {
        view.fill(0); // 欠载补静音
      }
    });
    await renderer.start();
  }

  private stopDecoderOnly(): void {
    this.videoStarted = false;
    this.audioStarted = false;
    this.pcmQueue = [];
  }

  stop(): void {
    this.playing = false;
    this.stopDecoderOnly();
    try {
      this.renderer?.stop();
      this.renderer?.release();
    } catch (e) { /* 忽略 */ }
    this.renderer = null;
    try {
      decoder.stopAll();
    } catch (e) { /* 忽略 */ }
    try {
      this.client.close();
    } catch (e) { /* 忽略 */ }
  }
}
