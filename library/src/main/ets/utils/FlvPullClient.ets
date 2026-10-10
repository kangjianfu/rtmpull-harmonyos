// FlvPullClient.ets — 纯 ArkTS HTTP-FLV 拉流 + FLV 解封装（自研，无第三方依赖）
// TCPSocket 发 HTTP GET → FLV 流 → 拆 tag：视频(H.264/H.265)、音频(AAC)、元数据
// 视频帧输出为 AVCC（长度前缀 NALU），音频为 raw AAC
// H.265 同时兼容标准 Enhanced RTMP（fourcc 在 byte1）和旧版配套封装（fourcc 在 byte2），自动识别
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
  /** 连接断开或收流超时（非正常 stop） */
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

/** 24bit 有符号整数（FLV CompositionTime 可为负） */
function s24(a: number, b: number, c: number): number {
  let v = (a << 16) | (b << 8) | c;
  if (v >= 0x800000) {
    v -= 0x1000000;
  }
  return v;
}

interface RecvStat {
  t: number;
  bytes: number;
}

const NO_DATA_TIMEOUT_MS = 10000; // 收流看门狗：连续无数据判定断流
const WATCHDOG_INTERVAL_MS = 2000;

export class FlvPullClient {
  onLog: PullLog = () => {};
  events: PullEvents | null = null;

  private tcp: socket.TCPSocket = socket.constructTCPSocketInstance();
  // 分块缓冲：parse 按需合并，避免每次收包整体拷贝（O(n²) → 每 tag 至多一次拷贝）
  private chunks: Uint8Array[] = [];
  private bufLen: number = 0;
  private closing: boolean = false;
  private headerDone: boolean = false;
  private flvHeaderDone: boolean = false;
  private recvStats: RecvStat[] = [];
  private metaFired: boolean = false;
  private videoCodec: VideoCodecName = 'avc';
  private lastRecvAt: number = 0;
  private watchdogTimer: number = -1;

  async connect(url: string): Promise<void> {
    const info = parseHttpUrl(url);
    this.onLog(`连接 ${info.host}:${info.port}${info.path}`);
    await this.tcp.connect({
      address: { address: info.host, port: info.port },
      timeout: 10000
    });
    this.tcp.on('message', (data: socket.SocketMessageInfo) => {
      if (data.message) {
        this.lastRecvAt = Date.now();
        this.recvStats.push({ t: this.lastRecvAt, bytes: data.message.byteLength });
        const cutoff = this.lastRecvAt - 4000;
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
    // 收流看门狗：服务器假死（TCP 半开、不再发数据）时主动判定断流
    this.lastRecvAt = Date.now();
    this.watchdogTimer = setInterval(() => {
      if (!this.closing && Date.now() - this.lastRecvAt > NO_DATA_TIMEOUT_MS) {
        this.onLog('收流超时（10 秒无数据）');
        this.close();
        this.events?.onClosed();
      }
    }, WATCHDOG_INTERVAL_MS);
  }

  /** 异常断流：关闭连接并通知上层（正常 stop 走 close()，不触发 onClosed） */
  private failClosed(): void {
    this.close();
    this.events?.onClosed();
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
    if (this.watchdogTimer >= 0) {
      clearInterval(this.watchdogTimer);
      this.watchdogTimer = -1;
    }
    try {
      await this.tcp.close();
    } catch (e) { /* 忽略 */ }
  }

  // ---------------- 流式解析 ----------------

  private pushBytes(data: Uint8Array): void {
    this.chunks.push(data);
    this.bufLen += data.length;
    this.parse();
  }

  /** 返回头部连续 n 字节视图（不足返回 null；必要时把 chunks 合并成一整块） */
  private peek(n: number): Uint8Array | null {
    if (this.bufLen < n || this.chunks.length === 0) {
      return null;
    }
    if (this.chunks[0].length >= n) {
      return this.chunks[0];
    }
    const merged = new Uint8Array(this.bufLen);
    let off = 0;
    for (const c of this.chunks) {
      merged.set(c, off);
      off += c.length;
    }
    this.chunks = [merged];
    return merged;
  }

  private consume(n: number): void {
    this.bufLen -= n;
    while (n > 0 && this.chunks.length > 0) {
      const c = this.chunks[0];
      if (c.length <= n) {
        n -= c.length;
        this.chunks.shift();
      } else {
        this.chunks[0] = c.subarray(n);
        n = 0;
      }
    }
  }

  private parse(): void {
    for (;;) {
      if (!this.headerDone) {
        // HTTP 头到 \r\n\r\n（头很小，直接合并全量找）
        if (this.bufLen === 0) {
          return;
        }
        const all = this.peek(this.bufLen);
        if (!all) {
          return;
        }
        const idx = this.findCrlfCrlf(all);
        if (idx < 0) {
          if (this.bufLen > 64 * 1024) {
            this.onLog('HTTP 响应头异常');
            this.failClosed();
          }
          return;
        }
        const head = new util.TextDecoder('utf-8').decodeToString(all.subarray(0, idx));
        const statusLine = head.split('\r\n')[0];
        if (!statusLine.includes(' 200')) {
          this.onLog('HTTP 响应异常: ' + statusLine);
        } else {
          this.onLog('HTTP 已连接');
        }
        this.consume(idx + 4);
        this.headerDone = true;
      }
      if (!this.flvHeaderDone) {
        // FLV 头 9 字节 + PreviousTagSize0 4 字节
        const h = this.peek(13);
        if (!h) {
          return;
        }
        if (h[0] !== 0x46 || h[1] !== 0x4c || h[2] !== 0x56) { // "FLV"
          this.onLog('不是 FLV 流');
          this.failClosed();
          return;
        }
        this.consume(13);
        this.flvHeaderDone = true;
        this.onLog('FLV 流就绪');
      }
      // tag 头 11 字节 + body + prevTagSize 4
      const head = this.peek(15);
      if (!head) {
        return;
      }
      const tagType = head[0];
      const dataSize = (head[1] << 16) | (head[2] << 8) | head[3];
      const ts = ((head[4] << 16) | (head[5] << 8) | head[6] | (head[7] << 24)) >>> 0;
      if (this.bufLen < 15 + dataSize) {
        return;
      }
      const full = this.peek(15 + dataSize);
      if (!full) {
        return;
      }
      const body = full.subarray(11, 11 + dataSize);
      this.consume(15 + dataSize);
      this.handleTag(tagType, body, ts);
    }
  }

  private findCrlfCrlf(buf: Uint8Array): number {
    for (let i = 0; i + 3 < buf.length; i++) {
      if (buf[i] === 13 && buf[i + 1] === 10 && buf[i + 2] === 13 && buf[i + 3] === 10) {
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
      // Enhanced RTMP：byte0 = IsExHeader|FrameType(3bit)|PacketType(4bit)
      const frameType = (b0 >> 4) & 0x07;
      const isKey = frameType === 1;
      if (body.length >= 5 && String.fromCharCode(body[1], body[2], body[3], body[4]) === 'hvc1') {
        // 标准封装（enhanced-RTMP v2）：fourcc 紧跟 byte0
        const packetType = b0 & 0x0f;
        this.videoCodec = 'hevc';
        if (packetType === 0) { // SequenceStart
          ev.onVideoConfig('hevc', body.subarray(5));
        } else if (packetType === 1 && body.length >= 8) { // CodedFrames（带 CTS）
          const cts = s24(body[5], body[6], body[7]);
          ev.onVideoFrame(body.subarray(8), ts + cts, isKey);
        } else if (packetType === 3) { // CodedFramesX（无 CTS）
          ev.onVideoFrame(body.subarray(5), ts, isKey);
        }
        return;
      }
      if (body.length >= 6 && String.fromCharCode(body[2], body[3], body[4], body[5]) === 'hvc1') {
        // 旧版配套封装（rtmpush 早期版本）：PacketType 3bit 跨 byte0 低 2 位 + byte1 最高位，fourcc 在 byte2
        const packetType = ((b0 & 0x03) << 1) | (body[1] >> 7);
        this.videoCodec = 'hevc';
        if (packetType === 0) {
          ev.onVideoConfig('hevc', body.subarray(6));
        } else if (packetType === 1 && body.length >= 9) {
          const cts = s24(body[6], body[7], body[8]);
          ev.onVideoFrame(body.subarray(9), ts + cts, isKey);
        } else if (packetType === 3) {
          ev.onVideoFrame(body.subarray(6), ts, isKey);
        }
        return;
      }
      return; // 只认 hvc1
    }
    // 老 FLV tag：frameType|codecId(7=AVC)
    const frameType = b0 >> 4;
    const codecId = b0 & 0x0f;
    if (codecId !== 7 || body.length < 5) {
      return;
    }
    const avcType = body[1];
    if (avcType === 0) {
      ev.onVideoConfig('avc', body.subarray(5));
    } else if (avcType === 1) {
      const cts = s24(body[2], body[3], body[4]);
      ev.onVideoFrame(body.subarray(5), ts + cts, frameType === 1);
    }
  }

  private handleAudio(body: Uint8Array, ts: number, ev: PullEvents): void {
    if (body[0] !== 0xaf) {
      return; // 只认 AAC 44k 16bit
    }
    if (body[1] === 0) {
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
      for (let i = 0; i + kb.length + 9 <= body.length; i++) {
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
