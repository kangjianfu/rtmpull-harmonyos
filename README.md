# rtmpull-harmonyos — 鸿蒙原生拉流播放库

纯 HarmonyOS 实现的拉流播放 SDK，**无任何第三方依赖**。与 [rtmpush-harmonyos](https://github.com/kangjianfu/rtmpush-harmonyos)（推流）组成鸿蒙直播「推 + 播」全家桶。

## 组成

| 模块 | 说明 |
|------|------|
| `FlvPullClient` | 纯 ArkTS HTTP-FLV 拉流 + FLV 解封装：视频（H.264 老 FLV tag / H.265 Enhanced RTMP hvc1）、AAC、onMetaData 宽高解析、实测接收速率统计 |
| `PullPlayer` | 播放器封装：native 硬解（NDK）→ XComponent surface 零拷贝直渲；AAC → PCM → AudioRenderer（音频时钟驱动，欠载补静音） |

## 特性

- 视频硬解直渲（surface 模式），直播低延迟：解码即显示，不攒帧
- H.264 + **H.265（Enhanced RTMP hvc1，与 rtmpush 推流端配套）**
- AAC 硬解，采样率/声道从 ASC 自动解析
- 解码跟不上时丢帧保实时（队列按字节限长）
- 断流回调（`onClosed`），上层可自重连

## 快速开始

```ts
import { PullPlayer } from 'rtmpull';

const player = new PullPlayer();

// XComponent(SURFACE) 的 onLoad 里：
this.player.setSurfaceId(this.xController.getXComponentSurfaceId());

await player.play('http://服务器/app/流名.flv');
player.stop();
```

权限只需 `ohos.permission.INTERNET`。

## 集成方式

```json5
// 你模块的 oh-package.json5
"dependencies": { "rtmpull": "file:../rtmpull-harmonyos/library" }
```

或 git submodule 引用（推荐，版本可锁定）。

## 独立构建 / Demo

```bash
hvigorw assembleHar --mode module -p module=library@default -p product=default --no-daemon
# 产物：library/build/default/outputs/default/library.har

# demo App（输入 FLV 地址直接播，可独立安装验证）：
hvigorw assembleHap --mode module -p product=default -p buildMode=debug --no-daemon
hdc install demo/build/default/outputs/default/demo-default-unsigned.hap
```

## 已知限制

- 仅 HTTP-FLV 拉流（https 暂不支持；RTMP 拉流在路线图）
- 音视频同步为「音频时钟 + 视频解码即显」的直播策略，不追求文件播放级的精确同步
- 模拟器无相机但**有解码器**，播放链路模拟器可验；推流端（rtmpush）必须真机

## License

Apache-2.0（见 [LICENSE](LICENSE)、[NOTICE](NOTICE)）

---

由 [视创新云（北京）科技有限公司](https://www.widk.cn) 开源 · 体云动赛事平台（www.widk.cn）生产环境打磨
