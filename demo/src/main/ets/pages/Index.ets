// demo/pages/Index.ets — rtmpull 演示：输入 HTTP-FLV 地址，拉流播放
import { promptAction } from '@kit.ArkUI';
import { pasteboard } from '@kit.BasicServicesKit';
import { scanBarcode } from '@kit.ScanKit';
import { common } from '@kit.AbilityKit';
import { PullPlayer } from 'rtmpull';

@Entry
@Component
struct Index {
  @State playUrl: string = '';
  @State playing: boolean = false;
  @State statusText: string = '输入拉流地址后开始';
  @State logs: string[] = [];
  @State kbps: number = 0;

  private xController: XComponentController = new XComponentController();
  private player: PullPlayer = new PullPlayer();
  private statTimer: number = -1;

  aboutToAppear(): void {
    this.player.onLog = (msg: string) => {
      this.addLog(msg);
    };
    this.player.onClosed = () => {
      this.addLog('流断开');
      this.stopPlay();
    };
    this.player.onError = (msg: string) => {
      this.addLog(msg);
    };
  }

  aboutToDisappear(): void {
    this.stopPlay();
  }

  private addLog(msg: string): void {
    const line = `${new Date().toTimeString().slice(0, 8)} ${msg}`;
    this.logs = [line].concat(this.logs).slice(0, 40);
  }

  private async startPlay(): Promise<void> {
    if (!this.playUrl.startsWith('http')) {
      promptAction.showToast({ message: '请输入 http(s):// 开头的 FLV 拉流地址' });
      return;
    }
    try {
      this.statusText = '连接中…';
      await this.player.play(this.playUrl);
      this.playing = true;
      this.statusText = '播放中';
      this.statTimer = setInterval(() => {
        const r = this.player.getReceiveRateKbps();
        this.kbps = r >= 0 ? r : 0;
      }, 1000);
    } catch (e) {
      this.statusText = '播放失败';
      this.addLog(e instanceof Error ? e.message : String(e));
      promptAction.showToast({ message: e instanceof Error ? e.message : '播放失败' });
      this.stopPlay();
    }
  }

  private stopPlay(): void {
    this.playing = false;
    if (this.statTimer >= 0) {
      clearInterval(this.statTimer);
      this.statTimer = -1;
    }
    this.player.stop();
    this.statusText = '已停止';
  }

  /** 扫码输入（系统扫码界面，识别二维码里的拉流地址） */
  private async scanUrl(): Promise<void> {
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const result = await scanBarcode.startScanForResult(ctx);
      const text = result.originalValue ?? '';
      if (text.startsWith('http')) {
        this.playUrl = text;
        this.addLog('扫码成功');
      } else if (text.length > 0) {
        promptAction.showToast({ message: '二维码内容不是拉流地址' });
      }
    } catch (e) {
      // 用户取消或设备不支持（模拟器无相机）
    }
  }

  /** 从剪贴板粘贴拉流地址 */
  private pasteUrl(): void {
    try {
      const pb = pasteboard.getSystemPasteboard();
      if (pb.hasDataSync()) {
        const text = pb.getDataSync().getPrimaryText();
        if (text && text.startsWith('http')) {
          this.playUrl = text;
          this.addLog('已粘贴拉流地址');
          return;
        }
      }
      promptAction.showToast({ message: '剪贴板里没有 http 地址' });
    } catch (e) { /* 忽略 */ }
  }

  build() {
    Column({ space: 10 }) {
      // 头部：品牌名 + 副标题
      Column({ space: 2 }) {
        Text('rtmpull')
          .fontSize(24)
          .fontWeight(FontWeight.Bold)
          .fontColor('#111827')
        Text('鸿蒙原生拉流播放库 · 演示')
          .fontSize(12)
          .fontColor('#9ca3af')
      }
      .width('100%')
      .alignItems(HorizontalAlign.Start)
      .margin({ top: 12, bottom: 2 })

      // 播放画面
      XComponent({ id: 'demo_player', type: XComponentType.SURFACE, controller: this.xController })
        .width('100%')
        .aspectRatio(16 / 9)
        .borderRadius(16)
        .clip(true)
        .backgroundColor(Color.Black)
        .onLoad(() => {
          this.player.setSurfaceId(this.xController.getXComponentSurfaceId());
        })

      // 拉流地址（多行大文本框）
      TextArea({ placeholder: 'http://服务器/app/流名.flv', text: this.playUrl })
        .width('100%')
        .height(88)
        .fontSize(14)
        .backgroundColor(Color.White)
        .borderRadius(16)
        .enabled(!this.playing)
        .onChange((v: string) => {
          this.playUrl = v;
        })

      Row({ space: 8 }) {
        Button('扫码输入')
          .height(44)
          .fontSize(14)
          .fontColor(Color.White)
          .backgroundColor('#16a34a')
          .borderRadius(12)
          .layoutWeight(1)
          .enabled(!this.playing)
          .onClick(() => {
            this.scanUrl();
          })
        Button('粘贴地址')
          .height(44)
          .fontSize(14)
          .fontColor('#374151')
          .backgroundColor('#e5e7eb')
          .borderRadius(12)
          .layoutWeight(1)
          .enabled(!this.playing)
          .onClick(() => {
            this.pasteUrl();
          })
      }
      .width('100%')

      // 状态行：状态点 + 文字 + 速率
      Row({ space: 8 }) {
        Circle({ width: 8, height: 8 })
          .fill(this.playing ? '#16a34a' : '#d1d5db')
        Text(this.statusText)
          .fontSize(13)
          .fontColor(this.playing ? '#16a34a' : '#6b7280')
          .layoutWeight(1)
        Text(`${this.kbps}kbps`)
          .fontSize(12)
          .fontColor('#9ca3af')
      }
      .width('100%')
      .padding({ left: 4, right: 4 })

      // 主按钮：全宽渐变
      Button(this.playing ? '停止播放' : '开始播放')
        .width('100%')
        .height(56)
        .fontSize(18)
        .fontWeight(FontWeight.Bold)
        .fontColor(Color.White)
        .borderRadius(16)
        .linearGradient({
          angle: 90,
          colors: this.playing
            ? [['#ef4444', 0], ['#b91c1c', 1]]
            : [['#3b82f6', 0], ['#0a59f7', 1]]
        })
        .onClick(() => {
          if (this.playing) {
            this.stopPlay();
          } else {
            this.startPlay();
          }
        })

      // 日志
      Scroll() {
        Column({ space: 4 }) {
          ForEach(this.logs, (l: string) => {
            Text(l)
              .fontSize(10)
              .fontColor('#6b7280')
              .width('100%')
          }, (l: string) => l)
        }
        .width('100%')
      }
      .layoutWeight(1)
      .backgroundColor(Color.White)
      .borderRadius(16)
      .padding(10)
    }
    .width('100%')
    .height('100%')
    .padding({ left: 12, right: 12, bottom: 12 })
    .backgroundColor('#f5f5f7')
  }
}
