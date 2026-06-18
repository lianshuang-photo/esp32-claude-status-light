# 给同事用 — 共享 daemon 模式

你的同事**不需要**装 daemon、也**不需要**自己有一块 ESP32。
他们的 Claude Code / Codex hook 直接打到**你电脑**的 daemon 上,
你电脑再驱动**那块共享的灯**。

## 拓扑

```
 同事的 Claude Code/Codex
       │
       │ HTTP (< 100 ms)
       ▼
┌──────────────────────────────┐
│ 你的电脑 (192.168.1.198)      │   ← daemon 跑在这里, 7878 端口
│   signal_daemon.py            │
└──────────┬───────────────────┘
           │ TCP
           ▼
┌──────────────────────────────┐
│ ESP32-C3 灯 (192.168.1.197)   │
└──────────────────────────────┘
```

## 前提

- 同事跟你**在同一个内网**(能 ping 通你的电脑 IP)
- 你的 daemon 一直在跑
- 你的电脑 IP **不要变**(建议在路由器后台给你的 Mac 绑定固定 IP)

---

## 同事的 3 步操作

### 1. Clone 仓库

```bash
git clone https://github.com/lianshuang-photo/esp32-claude-status-light.git
cd esp32-claude-status-light
```

### 2. 设环境变量,指向你的 daemon

把 `<YOUR_DAEMON_IP>` 替换成你电脑的 IP(本例:**192.168.1.198**)。

加进 `~/.zshrc`(或 `~/.bashrc`):

```bash
echo 'export HOOK_DAEMON_URL=http://192.168.1.198:7878/hook' >> ~/.zshrc
source ~/.zshrc
```

### 3. 装 hooks(只针对自己用的 agent)

只装 Claude:
```bash
python3 daemon/install_hooks.py --apply --agent claude
```

或只装 Codex:
```bash
python3 daemon/install_hooks.py --apply --agent codex
```

或都装:
```bash
python3 daemon/install_hooks.py --apply
```

**安装器会自动备份现有 hooks**(`.bak.<时间戳>`),不会覆盖同事原有的其他 hook。

### 4. 验证(可选)

同事终端跑一下,看你的灯有没有反应:

```bash
echo '{"hook_event_name":"PreToolUse"}' | python3 daemon/hook_client.py --agent claude
```

如果灯**绿黄红循环呼吸**了几秒,说明链路通了。

### 5. 重启他们的 Claude / Codex

- Claude Code: 退出整个 app 重开,或 `/restart`
- Codex: 退出重开, **会弹窗问是否信任新 hook,选"信任"**

之后他们正常用,你的灯会反映他们的状态。

---

## 多人同时用会怎么样?

`signal_daemon.py` 已经做了**优先级聚合**:

| 谁的状态 | 灯效 |
|---|---|
| 任何一人在等权限 | 红闪 → 红常亮 |
| 任何一人卡 / 报错 | 红黄交替闪 → 红常亮 |
| 任何一人需要回答(AskUserQuestion) | 黄闪 → 黄常亮 |
| 多人在跑 | 绿黄红三色循环呼吸 |
| 都空闲 | 绿常亮 |

打开 Web UI(<http://192.168.1.198:7878>)能看到:
- 当前哪个 session 在驱动灯(WINNER 标签)
- 所有活跃 session 列表
- 谁是 Claude / 谁是 Codex
- 每个 session 的 cwd

## 想撤销

```bash
python3 daemon/install_hooks.py --uninstall --apply
```

只移除 signal-light 自己的 hook,**完全不动同事别的 hook**。

## 常见问题

**Q: 同事 Mac 防火墙拦了怎么办?**
→ 同事不需要做啥(他们是出站连接,不受入站防火墙影响)。需要看的是**你**这边的入站防火墙没开(默认 macOS 没开)。

**Q: 灯没反应?**
→ 同事在终端跑这条,看输出:
```bash
curl -sv -X POST http://192.168.1.198:7878/hook \
  -H 'Content-Type: application/json' \
  -d '{"hook_event_name":"PreToolUse","session_id":"test"}'
```
看到 `HTTP/1.1 200 OK` = 通了。看到 `Connection refused` = 你的 daemon 没跑。

**Q: 你电脑睡眠 / 重启了灯就死了?**
→ 是。要么 daemon 装成开机自启(LaunchAgent),要么把 daemon 搬到一台不睡眠的机器(NAS / mac mini)上。
