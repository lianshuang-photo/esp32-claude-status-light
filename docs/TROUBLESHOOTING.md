# 调试与排错笔记

把开发过程中真实踩到的坑、根因、修复方法记下来,主要给三种人看:

- **遇到同样症状的用户** — 直接查表对症修。
- **想 fork 改的人** — 别再踩同样的坑。
- **下次重构的我** — 别忘了为什么那么写。

---

## 1. 灯效问题

### 1.1 OFF 关不掉,某些灯还亮着 💡

**症状**
- Web UI 推 `off` effect,daemon 串口都确认推到了,但灯没全灭(一般是黄或绿残留)。
- 探针固件单独测同一 GPIO 的纯 `digitalWrite` 完全正常。

**根因**
Arduino-ESP32 的 `analogWrite()` 在内部把 GPIO **attach 到 LEDC 硬件 PWM 通道**。一旦发生,该 GPIO 上后续的 `digitalWrite()` 在 ESP32-C3 上**不可靠**。所以"用 `digitalWrite(pin, HIGH)` 强制关掉"会沉默失败,LEDC 继续以上一帧的 duty 输出。

**为什么 starlight36 / Mittif 没遇到**
他们的硬件不是 ESP32-C3:
- starlight36 用 MCP2221A(USB-GPIO 转接板,根本没 PWM 外设)
- Mittif 用 CH552(8051 单片机,自己写 USB HID)

两个项目都没碰 Arduino-ESP32 的 LEDC 抽象。**ESP32 这一坑只有 ESP32 用户能踩到**。

**修复**
分两步迭代:
1. **去掉 analogWrite,改纯 `digitalWrite` + 软件 PWM**(commit `30005c8`)。"OFF 真灭"解决了,但引入新问题 → 见 1.2。
2. **改用低层 LEDC API + 关闭时 detach**(commit `a934d0a`)。`ledcAttach()` 上 PWM,duty=0 时 `ledcDetach()` 然后 `digitalWrite(pin, HIGH)`(共阳 = 灭)。这才是稳定版。

```cpp
static inline void setChannel(uint8_t pin, uint8_t duty, bool &attached) {
  if (duty == 0) {
    if (attached) { ledcDetach(pin); pinMode(pin, OUTPUT); attached = false; }
    digitalWrite(pin, ACTIVE_LOW ? HIGH : LOW);  // 真正关死
  } else {
    if (!attached) { ledcAttach(pin, 1000, 8); attached = true; }
    ledcWrite(pin, ACTIVE_LOW ? (255 - duty) : duty);
  }
}
```

---

### 1.2 灯每秒"暗一下",像蜡烛抖

**症状**
所有静态灯效(idle/常亮)会**每秒一次**短暂变暗约 100 ms。

**根因(第一次)**
软件 PWM 跑在 `loop()` 里。每个 PWM 周期约 1 ms。但主循环里还有 `WiFi.status()`、TCP 数据处理、ping/pong —— 任何一次阻塞 50-200 ms 都会让 GPIO 卡在那一帧最后的状态。

**修复 v1(后来又被推翻)**
把 PWM tick 挪到**硬件定时器中断**(`timerBegin/timerAlarm`,125 µs 周期),guarantee 恒定 cadence。commit `2f01549`。
表面上修好了,但**带来更隐蔽的问题** → 见 1.3。

---

### 1.3 闪烁灯效(attention / permission / blocked)闪 11-12 下后"卡住",某通道粘住不释放

**症状**
- Permission 刚开始红灯紧闪,大约 11-12 闪之后突然变成"红灯常亮不闪"。
- daemon 串口持续 `[FRM]` 报告帧在切换,duty 也写了 0。
- 但实际 LED 没被关掉。
- 只在 Wi-Fi 活跃时发生;板子断网后能恢复。

**根因**
ISR 里调用了 `digitalWrite()`。Arduino 的 `digitalWrite` **不在 IRAM 里**,而 ESP32 在 Wi-Fi 任务繁忙时会**暂时禁用 flash cache**(call to flash 会跑飞或 fault)。在那个窗口内,ISR 调 `digitalWrite` 要么 no-op 要么 crash 静默。

**第一次尝试(无效)**
把 `digitalWrite` 换成直接读写 GPIO 寄存器 `GPIO.out_w1ts.val = mask`,理论上完全 IRAM-safe。`hardOn/hardOff` 都标了 `IRAM_ATTR`,`pwmIsr` 也标了。
**还是闪 11-12 下后失败**,只是失败模式略变。怀疑还有别的非 IRAM 路径在 ISR 调用栈里,定位成本太高。

**最终修复**
**整个软件 PWM 方案放弃,换 ESP32 LEDC 硬件 PWM**(commit `a934d0a`)。LEDC 是真正的硬件外设,**完全不依赖 CPU、不依赖 flash cache、Wi-Fi 怎么忙都不影响**。这就是为啥社区里 LEDC + Wi-Fi 同时跑没人报 flicker 的 issue。

**教训**
软件 PWM 在 ESP32 上跑 Wi-Fi 的工程不要做。能用 LEDC 就用 LEDC。

---

### 1.4 闪烁灯效在桌上"持续不停闪",用户烦

**症状**
最早的 attention/permission/blocked 都是无限循环闪,放在桌上长时间用,余光一直被吸引。

**修复**
所有报警类灯效改成"**闪 6 次 + 然后停在常亮帧**"(`ms: null` 表示 hold 永久)。

视觉冲击足够,持续骚扰为零。落实在 `daemon/config.default.json` 的 effect frames。

---

## 2. 板子 / Wi-Fi 问题

### 2.1 板子重启循环,每 30 秒一次,灯不停闪黄

**症状**
- daemon 看 `connects` 数字每 7-30 秒涨 1。
- 串口能看到反复的 boot 输出。
- 灯短暂亮一下 → 切到黄灯慢闪(Wi-Fi 等待) → 然后变回 idle → 再黄闪 → ...

**根因**
**daemon 跟板子在不同 Wi-Fi 网络**!
- 板子拿到 `192.168.1.197`(在 CherryStudio)
- 你电脑切到了别的 Wi-Fi(ChinaNet,IP 是别的网段)
- daemon 配置里 `board.host = 192.168.1.197`
- daemon 发的心跳因为不在同一网段**根本到不了板子**
- 板子 30 秒没收到任何消息 → 触发 watchdog reboot
- 板子重启 → Wi-Fi 重连 → 又拿 `192.168.1.197` → 又等不到心跳 → 又 reboot
- 形成**无限重启循环**

**为什么 ping 看起来通**
我们最早 ping `.197` 显示通,**这是 Mac ARP 表里的缓存命中**(同一个 LAN 上、不同子网仍可能 ARP 缓存)。真正的 TCP 三次握手是不通的。daemon `connects` 也涨,但 `errors` 也涨同样快。

**修复**
确认电脑跟板子在**同一个 Wi-Fi 网络**。今天最终都连 CherryStudio。

**长期改进**
- watchdog 30 秒 → 90 秒(commit `5b478bd`),让短期网络抖动不立刻引发 reboot。
- Wi-Fi 凭据用 NVS 持久化,下次重启用上次成功的(commit `5b478bd`)。
- 把 CherryStudio 放 WIFI_NETWORKS[0]、ChinaNet 放 [1] —— 实测 ChinaNet 经常连不上,不要让它每次都先试。
- (待办)路由器后台给板子绑固定 IP。

---

### 2.2 板子 IP 变了,daemon 找不到 (.197 → .198)

**症状**
一直能用的板子突然 daemon 显示 offline 或 `connection refused`。

**真相**
我以为板子换 IP 了,结果是我**误以为 `.198` 是板子 IP**。

`192.168.1.198` 实际是**这台 Mac 自己的 IP**。串口里看到的 `[CONN] daemon from 192.168.1.198` 那行的"192.168.1.198"是**daemon 客户端的源 IP**(也就是我的 Mac),不是板子的 IP。板子那头一直是 `.197`。

`ping .198 → 0.1ms` 是回环延迟,因为 ping 自己。`ping .197 → 144ms` 才是真正的 LAN 设备延迟。

**怎么辨认**
- ping 延迟 **< 1ms** → 99% 是自己电脑(回环或本机别名)
- ping 延迟 **30-150ms** → LAN 设备
- 板子真正的 IP **必须从板子串口的 `[WIFI] connected, IP=...` 那行读**,不能从 daemon 端推

---

### 2.3 升级到 90 秒 watchdog 后还是会反复 reboot

**症状**
固件改 90 秒了,但板子还是几分钟就 reboot 一次。

**根因**
跟 2.1 同一个根因(daemon 跟板子不在同网),只是症状被 90 秒 watchdog 拉长了周期。**watchdog 时长只能缓解症状,真正问题是网络可达性**。

**修复**
先解决网络可达性。watchdog 是兜底,不是主防线。

---

## 3. USB / 烧录问题

### 3.1 ESP32-C3 SuperMini 烧完就掉线

**症状**
- `arduino-cli upload` 成功
- 但烧完之后 `ls /dev/cu.*` 看不到端口
- 偶发,不是每次都发生

**根因**
ESP32-C3 用原生 USB-Serial-JTAG。固件刚烧完 `Hard resetting via RTS pin` 那一刻,USB 设备会重新枚举。macOS 偶尔枚举失败,设备就消失。

**解决**
**拔 USB 再插**。或者按板子上的 **RST 按钮**。
重新插上之后端口号也可能变,从 `usbmodem21201` 跳到 `usbmodem21101`(USB endpoint ID 跟枚举顺序有关)。

---

### 3.2 烧完用 `arduino-cli monitor` 串口什么都看不到

**症状**
固件运行正常,板子也连上了 Wi-Fi(从 daemon 端确认),但串口监听 30+ 秒一行都没有。

**根因**
ESP32-C3 SuperMini 的 USB-Serial-JTAG 在 Arduino-ESP32 默认 FQBN 下,`Serial.println()` 输出**不走 USB CDC**,而是走硬件 UART(物理引脚)。USB 那条管子是哑的。

**修复**
编译和烧录时 FQBN 加 `:CDCOnBoot=cdc`:

```bash
arduino-cli compile --fqbn 'esp32:esp32:esp32c3:CDCOnBoot=cdc' ./esp32c3_signal_light_debug
arduino-cli upload  --fqbn 'esp32:esp32:esp32c3:CDCOnBoot=cdc' -p /dev/cu.usbmodemXXXX ./esp32c3_signal_light_debug
```

加了之后 `Serial.println()` 会走 USB CDC 输出到主机,串口监视器正常。

---

### 3.3 `arduino-cli monitor` 卡住后退出,端口被占

**症状**
`arduino-cli monitor` 退出后,下次 upload 报 "Failed uploading: exit status 2"。

**根因**
串口资源在 macOS 上被某个挂掉的进程持有。

**修复**
```bash
lsof -t /dev/cu.usbmodemXXXX | xargs -r kill -9
```

或者更暴力一点 — 直接拔 USB 重插。

---

### 3.4 reset 板子时表笔 / DTR pulse 让板子进了下载模式

**症状**
用 pyserial `s.dtr = False; s.dtr = True` 想软 reset,但板子串口立刻输出:

```
rst:0x15 (USB_UART_CHIP_RESET),boot:0x5 (DOWNLOAD(USB/UART0/1))
waiting for download
```

然后板子卡死等下载,固件不跑了。

**根因**
**同时拉低 DTR 和 RTS** = 触发下载模式(esptool 干的事就是这个组合)。

**修复**
只 pulse DTR,**不动 RTS**:

```python
s.setDTR(False)
time.sleep(0.05)
s.setDTR(True)
# 不要动 s.setRTS()
```

---

## 4. Hook / 集成问题

### 4.1 改了 `~/.codex/hooks.json` 但 Codex 不响应新 hook

**症状**
- `install_hooks.py --apply` 成功
- `~/.codex/hooks.json` 里能看到 `_signal_light: true` 的条目
- 但用 Codex 跑工具,daemon 完全没收到任何 hook

**根因**
Codex(还有 Claude Code)**只在启动时读一次 hooks.json**。运行中改文件不会被重读。如果你的 Codex 已经在跑了 2 天,它用的是 2 天前那份配置。

**怎么确认**
```bash
# 1. hooks.json 改动时间 vs Codex 进程启动时间对比
stat -f "%Sm" ~/.codex/hooks.json   # 看是不是新的
ps -eo etime,comm | awk '$2 ~ /Codex/'   # 看 Codex 跑了多久
# 2. Codex trust 表里有没有我们的 hash
grep -c "signal" ~/.codex/config.toml
# 0 → Codex 从来没看见过我们的 hook
```

**修复**
**完全 Quit 然后重开 Codex** —— 不只是关窗口。重开时 Codex 会**弹窗问"是否信任来自 hook_client.py 的命令"**,选 Allow。

---

### 4.2 网页按钮点击没反应,但 curl 推 API 一切正常

**症状**
- `curl -X POST /api/manual ...` 灯会响应
- daemon 串口看到推送
- 但浏览器网页上点 Manual Test 按钮,灯不动

**根因**
浏览器跟 daemon 之间有个**SSE 长连接** (`/stream`)。daemon 重启过的话,浏览器那条 SSE 流断了**但前端 JS 没自动重连**,所以前端以为 daemon 还活着、UI 不更新。再点按钮不一定能发出去,或者发出去了 UI 也不刷新。

**修复**
- **临时**:浏览器 Cmd+Shift+R 强刷,或者开新 tab。
- **长期**(待办):前端 `EventSource` 的 `onerror` 加 setTimeout 重连逻辑。

---

### 4.3 hook 触发后灯一会儿就变回 idle,本该一直 working 的

**症状**
让 Codex 跑个长命令,灯转了几下三色循环就**变回绿色**(idle),但 Codex 还在跑。

**真根因(今天没排到底,但能解释)**
我们 session sweeper TTL 是 120 秒,远端 Codex 的 hook 进入 daemon 时 `pid` 字段是远端机器上 hook_client.py 的 ppid,跟我们 Mac 上根本对不上。本机 Mac 上 `_pid_alive(那个 ppid)` 大概率返回 `False`(不存在的 pid),所以**远端来的 session 一进就被 sweeper 当成"已经死了"扫掉**。

**待修**
- daemon 端识别 LAN 进来的 hook(不是 127.0.0.1),不做 pid 死亡检测,只看 TTL。

或者更简单:**hook_client.py 远端调用时不要塞 pid 字段**。

---

## 5. 反直觉的小坑

### 5.1 daemon 配置改了不立刻生效

`~/.signal_light/config.json` 不是 hot reload 的。改完要么:

1. POST 到 `/api/config` 强制 reload:
   ```bash
   curl -X POST http://127.0.0.1:7878/api/config \
     -H 'Content-Type: application/json' \
     --data @~/.signal_light/config.json
   ```
2. 或者重启 daemon。

`board.host` 字段尤其要注意:**BoardClient 的当前 TCP 连接不会因为 config 变了就重新连**,必须重启 daemon。

---

### 5.2 推 effect 之后,UI status 显示新 effect,但板子串口没收到

**根因**
daemon 端 `evaluate()` 算出来 chosen 跟当前 `_current_effect_id` 相同就**不推送**(节流)。
所以"重复推同一个 effect" daemon 不会真发给板子。

如果你在调试**想强制重发**,用 `--force` 或者 `evaluate(force=True)`,或者推个不同 effect 隔开。

---

### 5.3 daemon 重启时 board.online 仍显示 True

**根因**
`board.online` 是基于 BoardClient 的 TCP socket 状态。daemon 刚启动 + TCP 还没第一次成功之前,`online` 默认从上次缓存值取。短暂(秒级)`online=True` 但实际不通是正常的。

判断真实状态看 `last_send_at` —— 超过 10 秒没动的就可能假阳性。

---

### 5.4 LaunchAgent 装上后,daemon 双开了

**症状**
`install_launchagent.sh` 装完之后,`pgrep -f signal_daemon.py` 看到 2 个进程。

**根因**
你之前 `python3 signal_daemon.py &` 手动起的进程没杀,LaunchAgent 又起了一个。两个都监听 7878,后起的会失败但不退出。

**修复**
`install_launchagent.sh` 的最新版会在装之前 `pkill -f signal_daemon.py` 把所有手动跑的杀干净再装。如果你装完看到双开,手动 `pkill -f signal_daemon.py`,5 秒后 LaunchAgent 自己会唯一地把它起回来。

---

## 6. 兜底排查命令清单

灯不响应的时候按顺序跑:

```bash
# 1. daemon 在跑吗
pgrep -lf signal_daemon.py

# 2. 监听端口对吗
lsof -iTCP:7878 -sTCP:LISTEN

# 3. HTTP API 响应吗
curl -s --max-time 2 http://127.0.0.1:7878/api/status | head -c 200

# 4. daemon 看板子在线吗
curl -s http://127.0.0.1:7878/api/status | python3 -c "
import sys, json, time
s = json.load(sys.stdin)
b = s['board']
print(f'online={b[\"online\"]}, last_send {round(time.time()-b[\"last_send_at\"],1)}s 前, errors={b[\"errors\"]}')
"

# 5. 真的能 ping 通板子吗(认得 < 1ms 是回环)
ping -c 3 -W 1 <板子IP>

# 6. 板子在不在同一个网段
ifconfig | grep "inet 192\|inet 10"  # 你电脑的 LAN IP

# 7. 终极测试: 直接 curl 推灯效, 看物理灯有没有变
curl -X POST http://127.0.0.1:7878/api/manual \
  -H 'Content-Type: application/json' \
  -d '{"effect":"permission","hold_seconds":15}'
# 灯应该闪红 6 次然后红常亮. 如果没变, 问题在 daemon→板子链路.

# 8. 串口看板子收到了吗 (FQBN 必须带 :CDCOnBoot=cdc)
python3 -c "
import serial, time
s = serial.Serial('/dev/cu.usbmodemXXXX', 115200, timeout=0.3)
end = time.time() + 5
while time.time() < end:
    line = s.readline()
    if line: print(line.decode('utf-8','replace').rstrip())
"

# 9. LaunchAgent 健康吗
~/path/to/daemon/install_launchagent.sh --status
```
