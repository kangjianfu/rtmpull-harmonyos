// FlvPullClient.ets — 纯 ArkTS HTTP-FLV 拉流 + FLV 解封装（自研，无第三方依赖）
// TCPSocket 发 HTTP GET → FLV 流 → 拆 tag：视频(H.264/H.265)、音频(AAC)、元数据
// 视频帧输出为 AVCC（长度前缀 NALU），音频为 raw AAC；支持 Enhanced RTMP hvc1 封装
import { socket } from '@kit.NetworkKit';
import { util } from '@kit.ArkTS';

export type PullLog = (msg: string) => void;

export type VideoCodecName = 'avc' | 'hevc';

export interface PullEvents {
  onLog: PullLog;
  /** 视频序列头：avc=avcC，hevc=hvcC（原始字节） */
  onVideoConfig: (codec: VideoCodecName, config: Uint8Array) => void;
  /** 视频帧（AVCC 格式），pts 毫秒 */
  onVideoFrame: (data: Uint8Array, ptsMs: number, isKey: boolean) => void;
  /** AAC 序列头（AudioSpecificConfig） */
  onAudioConfig: (asc: Uint8Array) => void;
  /** 音频帧（raw AAC），pts 毫秒 */
  onAudioFrame: (data: Uint8Array, ptsMs: number) => void;
  /** 元数据里的宽高（从 onMetaData 解析，解析不到给 0） */
  onMeta: (width: number, height: number) => void;
  /** 连接断开（非正常 stop） */
  onClosed: () => void;
}

interface FlvUrl {
  host: string;
  port: number;
  path: string;
}

function parseHttpUrl(url: string): FlvUrl {
  let u = url;
  if (u.startsWith('http://')) {
    u = u.slice(7);
  } else if (u.startsWith('https://')) {
    throw new Error('暂不支持 https，请用 http 拉流地址');
  }
  const slash = u.indexOf('/');
  const hostPort = slash >= 0 ? u.slice(0, slash) : u;
  const path = slash >= 0 ? u.slice(slash) : '/';
  let host = hostPort;
  let port = 80;
  const idx = hostPort.lastIndexOf(':');
  if (idx > 0) {
    port = parseInt(hostPort.slice(idx + 1)) || 80;
    host = hostPort.slice(0, idx);
  }
  return { host: host, port: port, path: path };
}

interface RecvStat {
  t: number;
  bytes: number;
}

export class FlvPullClient {
  onLog: PullLog = () => {};
  events: PullEvents | null = null;

  private tcp: socket.TCPSocket = socket.constructTCPSocketInstance();
  private buf: Uint8Array = new Uint8Array(0);
  private closing: boolean = false;
  private headerDone: boolean = false;
  private flvHeaderDone: boolean = false;
  private recvStats: RecvStat[] = [];
  private metaFired: boolean = false;
  private videoCodec: VideoCodecName = 'avc';
  private videoConfigSent: boolean = false;
  private audioConfigSent: boolean = false;

  async connect(url: string): Promise<void> {
    const info = parseHttpUrl(url);
    this.onLog(`连接 ${info.host}:${info.port}${info.path}`);
    await this.tcp.connect({
      address: { address: info.host, port: info.port },
      timeout: 10000
    });
    this.tcp.on('message', (data: socket.SocketMessageInfo) => {
      if (data.message) {
        this.recvStats.push({ t: Date.now(), bytes: data.message.byteLength });
        const cutoff = Date.now() - 4000;
        while (this.recvStats.length > 0 && this.recvStats[0].t < cutoff) {
          this.recvStats.shift();
        }
        this.pushBytes(new Uint8Array(data.message));
      }
    });
    this.tcp.on('close', () => {
      if (!this.closing) {
        this.onLog('连接已断开');
        this.events?.onClosed();
      }
    });
    this.tcp.on('error', () => {
      if (!this.closing) {
        this.onLog('连接错误');
        this.events?.onClosed();
      }
    });
    const req = `GET ${info.path} HTTP/1.1\r\nHost: ${info.host}\r\nAccept: */*\r\nConnection: close\r\n\r\n`;
    await this.tcp.send({ data: new util.TextEncoder().encodeInto(req).buffer });
  }

  /** 最近 3 秒实际接收速率（kbps）；样本不足返回 -1 */
  getReceiveRateKbps(): number {
    const cutoff = Date.now() - 3000;
    const recent = this.recvStats.filter(s => s.t >= cutoff);
    if (recent.length < 2) {
      return -1;
    }
    const span = (recent[recent.length - 1].t - recent[0].t) / 1000;
    if (span <= 0.2) {
      return -1;
    }
    let bytes = 0;
    for (const s of recent) {
      bytes += s.bytes;
    }
    return Math.round(bytes * 8 / 1000 / span);
  }

  async close(): Promise<void> {
    this.closing = true;
    try {
      await this.tcp.close();
    } catch (e) { /* 忽略 */ }
  }

  // ---------------- 流式解析 ----------------

  private pushBytes(data: Uint8Array): void {
    const merged = new Uint8Array(this.buf.length + data.length);
    merged.set(this.buf);
    merged.set(data, this.buf.length);
    this.buf = merged;
    this.parse();
  }

  private parse(): void {
    for (;;) {
      if (!this.headerDone) {
        // HTTP 头到 \r\n\r\n
        const idx = this.findCrlfCrlf();
        if (idx < 0) {
          return;
        }
        const head = new util.TextDecoder('utf-8').decodeToString(this.buf.subarray(0, idx));
        if (!head.includes('200')) {
          this.onLog('HTTP 响应异常: ' + head.split('\r\n')[0]);
        }
        this.onLog('HTTP 已连接');
        this.buf = this.buf.subarray(idx + 4);
        this.headerDone = true;
      }
      if (!this.flvHeaderDone) {
        // FLV 头 9 字节 + PreviousTagSize0 4 字节
        if (this.buf.length < 13) {
          return;
        }
        if (this.buf[0] !== 0x46 || this.buf[1] !== 0x4c || this.buf[2] !== 0x56) { // "FLV"
          this.onLog('不是 FLV 流');
          this.close();
          return;
        }
        this.buf = this.buf.subarray(13);
        this.flvHeaderDone = true;
        this.onLog('FLV 流就绪');
      }
      // tag 头 11 字节 + body + prevTagSize 4
      if (this.buf.length < 15) {
        return;
      }
      const b = this.buf;
      const tagType = b[0];
      const dataSize = (b[1] << 16) | (b[2] << 8) | b[3];
      const ts = (b[4] << 16) | (b[5] << 8) | b[6] | (b[7] << 24);
      if (this.buf.length < 15 + dataSize) {
        return;
      }
      const body = this.buf.subarray(11, 11 + dataSize);
      this.buf = this.buf.subarray(15 + dataSize);
      this.handleTag(tagType, body, ts);
    }
  }

  private findCrlfCrlf(): number {
    for (let i = 0; i + 3 < this.buf.length; i++) {
      if (this.buf[i] === 13 && this.buf[i + 1] === 10 && this.buf[i + 2] === 13 && this.buf[i + 3] === 10) {
        return i;
      }
    }
    return -1;
  }

  private handleTag(tagType: number, body: Uint8Array, ts: number): void {
    const ev = this.events;
    if (!ev || body.length < 2) {
      return;
    }
    if (tagType === 9) {
      this.handleVideo(body, ts, ev);
    } else if (tagType === 8) {
      this.handleAudio(body, ts, ev);
    } else if (tagType === 18 && !this.metaFired) {
      this.metaFired = true;
      this.parseMeta(body, ev);
    }
  }

  private handleVideo(body: Uint8Array, ts: number, ev: PullEvents): void {
    const b0 = body[0];
    if ((b0 & 0x80) !== 0) {
      // Enhanced RTMP：byte0=FrameType|ExHeader|PacketType高2位，byte1=PacketType低位<<7，然后 fourcc
      const frameType = b0 >> 4;
      const packetType = ((b0 & 0x03) << 1) | (body[1] >> 7);
      const fourcc = String.fromCharCode(body[2], body[3], body[4], body[5]);
      if (fourcc !== 'hvc1') {
        return; // 只认 hvc1
      }
      this.videoCodec = 'hevc';
      if (packetType === 0) {
        this.videoConfigSent = true;
        ev.onVideoConfig('hevc', body.subarray(6));
      } else if (packetType === 1) {
        const cts = (body[6] << 16) | (body[7] << 8) | body[8];
        ev.onVideoFrame(body.subarray(9), ts + cts, frameType === 1);
      } else if (packetType === 3) {
        ev.onVideoFrame(body.subarray(6), ts, frameType === 1);
      }
      return;
    }
    // 老 FLV tag：frameType|codecId(7=AVC)
    const frameType = b0 >> 4;
    const codecId = b0 & 0x0f;
    if (codecId !== 7) {
      return;
    }
    const avcType = body[1];
    const cts = (body[2] << 16) | (body[3] << 8) | body[4];
    if (avcType === 0) {
      this.videoConfigSent = true;
      ev.onVideoConfig('avc', body.subarray(5));
    } else if (avcType === 1) {
      ev.onVideoFrame(body.subarray(5), ts + cts, frameType === 1);
    }
  }

  private handleAudio(body: Uint8Array, ts: number, ev: PullEvents): void {
    if (body[0] !== 0xaf) {
      return; // 只认 AAC 44k 16bit
    }
    if (body[1] === 0) {
      this.audioConfigSent = true;
      ev.onAudioConfig(body.subarray(2));
    } else {
      ev.onAudioFrame(body.subarray(2), ts);
    }
  }

  /** 从 onMetaData 字节里找 width/height（AMF0: key + 0x00 + double） */
  private parseMeta(body: Uint8Array, ev: PullEvents): void {
    let w = 0;
    let h = 0;
    const enc = new util.TextEncoder();
    const findNum = (key: string): number => {
      const kb = enc.encodeInto(key);
      for (let i = 0; i + kb.length + 9 < body.length; i++) {
        let match = true;
        for (let j = 0; j < kb.length; j++) {
          if (body[i + j] !== kb[j]) {
            match = false;
            break;
          }
        }
        if (match && body[i + kb.length] === 0x00) {
          const dv = new DataView(body.buffer, body.byteOffset + i + kb.length + 1, 8);
          return dv.getFloat64(0, false);
        }
      }
      return 0;
    };
    w = findNum('width');
    h = findNum('height');
    if (w > 0 && h > 0) {
      ev.onMeta(w, h);
    }
  }
}
