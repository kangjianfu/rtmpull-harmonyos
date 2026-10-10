// PullPlayer.ets — 拉流播放器封装：FlvPullClient 拉流解封装 → native 硬解 → XComponent 直渲 + native OHAudio 出声
// 用法：setSurfaceId（XComponent onLoad 时）→ play(url) → stop()
import pullDecoder from 'libpull_decoder.so';
import { FlvPullClient, VideoCodecName } from './FlvPullClient';

export type PlayerLog = (msg: string) => void;

interface NativePullDecoder {
  createVideoDecoder(surfaceId: string, mime: string, width: number, height: number, codecConfig: ArrayBuffer): boolean;
  pushVideoFrame(data: ArrayBuffer, ptsUs: number, isKey: boolean): boolean;
  /** 创建 AAC 解码器并启动 native 音频渲染（PCM 不经 ArkTS 中转） */
  createAudioDecoder(sampleRate: number, channels: number, asc: ArrayBuffer): boolean;
  pushAudioFrame(data: ArrayBuffer, ptsUs: number): boolean;
  stopAll(): boolean;
}

const decoder: NativePullDecoder = pullDecoder as NativePullDecoder;

const FREQ_TABLE: number[] = [96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000];

export class PullPlayer {
  onLog: PlayerLog = () => {};
  onClosed: () => void = () => {}; // 流断开（正常 stop 不触发）
  onError: PlayerLog = () => {}; // 解码器等内部错误（播放不中断的可忽略）

  private client: FlvPullClient = new FlvPullClient();
  private surfaceId: string = '';
  private videoStarted: boolean = false;
  private audioStarted: boolean = false;
  private metaW: number = 0;
  private metaH: number = 0;
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
    if (this.playing) {
      this.onLog('正在播放中，忽略重复的 play 调用');
      return;
    }
    // 清理上一次会话的残留（断流后未 stop 直接重播的场景）
    this.cleanupSession();
    const oldClient = this.client;
    oldClient.close().catch(() => { /* 忽略 */ });
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
          decoder.pushVideoFrame(data.slice().buffer as ArrayBuffer, ptsMs * 1000, isKey);
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
          this.playing = false;
          this.cleanupSession(); // 释放解码器/播放器，避免上层不 stop 时泄漏
          this.onClosed();
        }
      }
    };
    try {
      await client.connect(url);
    } catch (e) {
      this.playing = false;
      this.cleanupSession();
      throw e;
    }
  }

  private startVideo(codec: VideoCodecName, config: Uint8Array): void {
    if (this.videoStarted) {
      return;
    }
    const mime = codec === 'hevc' ? 'video/hevc' : 'video/avc';
    const w = this.metaW > 0 ? this.metaW : 1280;
    const h = this.metaH > 0 ? this.metaH : 720;
    try {
      const ok = decoder.createVideoDecoder(this.surfaceId, mime, w, h, config.slice().buffer as ArrayBuffer);
      if (!ok) {
        throw new Error('视频解码器创建失败');
      }
      this.videoStarted = true;
      this.onLog(`视频解码器已启动（${codec === 'hevc' ? 'H.265' : 'H.264'} ${w}x${h}）`);
    } catch (e) {
      this.onError(`视频解码器启动失败: ${e instanceof Error ? e.message : String(e)}`);
    }
  }

  private startAudio(asc: Uint8Array): void {
    if (this.audioStarted || asc.length < 2) {
      return;
    }
    this.audioStarted = true;
    const rateIdx = ((asc[0] & 0x07) << 1) | (asc[1] >> 7);
    const rate = rateIdx < FREQ_TABLE.length ? FREQ_TABLE[rateIdx] : 44100;
    const ch = ((asc[1] >> 3) & 0x0f) || 1;
    try {
      const ok = decoder.createAudioDecoder(rate, ch, asc.slice().buffer as ArrayBuffer);
      if (!ok) {
        throw new Error('音频解码器创建失败');
      }
      this.onLog(`音频解码器已启动（AAC ${rate}Hz ${ch}ch）`);
    } catch (e) {
      this.audioStarted = false;
      this.onError(`音频初始化失败: ${e instanceof Error ? e.message : String(e)}`);
    }
  }

  /** 释放本次会话的解码/渲染资源（不动 TCP 连接） */
  private cleanupSession(): void {
    this.videoStarted = false;
    this.audioStarted = false;
    try {
      decoder.stopAll();
    } catch (e) { /* 忽略 */ }
  }

  stop(): void {
    this.playing = false;
    this.cleanupSession();
    try {
      this.client.close();
    } catch (e) { /* 忽略 */ }
  }
}
