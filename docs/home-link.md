# Home Link（局域网控制接口）

这是 `alfred-bot-001/StackChan` fork（分支 `home-link`）对官方固件新增的局域网接口，供家庭控制台直接控制 StackChan。出厂固件没有任何局域网接口；本接口只增不改现有应用。中心侧契约与适配器在 AI_Home 仓库：`contracts/devices/stackchan/v1`、`adapters/stackchan`。

## 接口

地址 `http://<ip>:8700`，所有请求带 `Authorization: Bearer <token>`。响应 JSON；错误体 `{"error":"<code>","message":"..."}`。

| 方法 路径 | 任何应用下 | 说明 |
| --- | --- | --- |
| `GET /v1/status` | 是 | 固件版本、`home_link_version`、运行时间、电量、Wi-Fi、当前应用、头部角度/是否移动/是否上力、音量、亮度、是否挂了 avatar |
| `POST /v1/system/reboot` | 是 | 重启回启动器（先回 `accepted`） |
| `POST /v1/system/open-home` | 是 | 重启进 HOME 应用；已在 HOME 时直接 `completed` |
| `POST /v1/audio/volume {volume}` | 是 | 0–100，持久化 |
| `POST /v1/display/brightness {brightness}` | 是 | 0–100，持久化 |
| `POST /v1/led {r,g,b}` / `{leds:[{index,r,g,b}]}` | 是 | 12 颗 WS2812，通道 0–168 |
| `POST /v1/sound {name}` | 是 | `new_notification` / `camera_shutter` |
| `POST /v1/avatar {leftEye,rightEye,mouth}` | 仅 HOME | 同 `updateAvatarFromJson` |
| `POST /v1/avatar/emotion {emotion,duration_ms}` | 仅 HOME | `neutral/happy/angry/sad/doubt/sleepy`；默认 3000 ms，0 常驻 |
| `POST /v1/avatar/speech {text,duration_ms}` | 仅 HOME | ≤120 字节，默认 6000 ms，附说话动画 |
| `POST /v1/motion {yaw,pitch,speed}` | 仅 HOME | 度；yaw −128..128，pitch 0..90，speed 100–1000（默认 150） |
| `POST /v1/motion/home {speed}` | 仅 HOME | 回零 |
| `POST /v1/motion/look {x,y,speed}` | 仅 HOME | 归一化坐标 −1..1 |
| `POST /v1/motion/torque {enabled}` | 仅 HOME | 舵机上/卸力 |
| `POST /v1/neon {leftRgbColor,rightRgbColor,leftRgbDuration,rightRgbDuration}` | 仅 HOME | 同 `updateNeonLightFromJson` |
| `POST /v1/dance [keyframes]` | 仅 HOME | 同跳舞序列 JSON（数组） |
| `DELETE /v1/modifiers` | 仅 HOME | 清掉跳舞/定时表情，恢复默认空闲行为 |
| `GET /v1/camera/snapshot` | 仅 HOME | 一帧 JPEG |
| `WS /v1/events` | 是 | 每 5 秒 `{"type":"status","status":{...}}`；即时 `head_pet`、`imu`、`app`、`battery`；发 `{"type":"camera","enabled":true}` 后每 200 ms 收一帧二进制 JPEG（仅 HOME） |

写命令可带 `command_id`（8–64 位 `[A-Za-z0-9_.:-]`，首字符字母或数字），响应 `{"command_id":"...","state":"accepted"|"completed"}`。音量/亮度/灯/提示音/表情/文字/上卸力/灯带/清 modifier 同步生效（`completed`）；头部移动/回零/看向/跳舞/重启/进 HOME 异步（`accepted`），是否到位看 `status.motion.moving`。

状态码：401 令牌错（`unauthorized`）、503 已关闭（`disabled`，此时服务不监听，表现为连接拒绝）、409 非 HOME 应用调 HOME 专属命令（`home_app_not_active`，附 `current_app`）、400 请求体超 4096 字节或字段非法（`bad_request`）、404 未知路径、405 方法不对。WS 客户端上限 4。

## 令牌与开关

令牌 32 位 hex，首次启动随机生成，存 NVS 命名空间 `home_link`。只在设备 **Setup › Wi-Fi › Home Link** 页显示；该页可「New Token」（点两次确认）与「Disable/Enable」。令牌不写日志。

## 运行时机

- 固件启动进入启动器后，Home Link 任务会主动连接已保存的 Wi-Fi（出厂固件在启动器里不连网），连上后开 8700。AI Agent 开机自启时由小智连网，Home Link 复用。
- 进入 ESP-NOW 应用时停止 Home Link 并断开站点模式（两者争用信道）；退出回启动器后自动恢复。
- `GET /v1/status` 与任何应用下的命令随时可用；HOME 专属命令要先进入 HOME 应用（主菜单最后一个图标，或 `POST /v1/system/open-home`）。
- mDNS：主机名 `stackchan-<MAC 后 4 位>.local`，服务 `_stackchan-home._tcp`。

## 构建、烧录、恢复

```sh
./firmware/docker-build.sh build          # espressif/idf:v5.5.4 镜像内 fetch_repos.py + idf.py build
./firmware/docker-build.sh merge          # 合并成 build/stack-chan-merged.bin
./firmware/docker-build.sh flash /dev/cu.usbmodemXXXX   # 按偏移分段写入，不动 NVS；USB-Serial/JTAG 可自动进下载模式
cd firmware && cmake -S tests -B build-host-tests && cmake --build build-host-tests && ctest --test-dir build-host-tests
```

`flash` 按 `flash_args` 分段写入，NVS（0x9000，Wi-Fi、小智配置、App 绑定标记、令牌）保留。**不要把合并镜像写到 0x0**（`flash-full`）：merge-bin 用 0xFF 填充间隙，会把 NVS 清空，设备会回到首次配网流程；万一清了，用 `esptool read-flash 0x9000 0x4000` 事先留的备份写回 0x9000。恢复出厂固件：M5Burner v3 搜索「StackChan」，勾选 Only Official，Download → Burn。

## 自建固件上已知失效的功能

`secret_logic` 的设备签名在开源版里只是弱符号占位，连不上 M5Stack 云：App 绑定、Avatar 远程、App Center 下载、EZData、官方 OTA 失效。AI Agent（小智）是否仍可用以实机验收记录为准。

## 版本

`PROJECT_VER` 为 `1.6.0-home.N`，每次合入 `home-link` 分支发布时递增 N。`home_link_version` 只在接口不兼容时递增。

## 整机验收清单

- [ ] 冷启动进入启动器后 ≤ 20 秒 `GET /v1/status` 200
- [ ] 断开路由器 Wi-Fi 30 秒再恢复：状态 200 恢复，令牌不变
- [ ] AI Agent 对话中 `POST /v1/led` 生效且对话继续；AI Agent 能否正常唤醒对话（记录结果）
- [ ] HOME 下表情/文字/头部/灯带/跳舞/清空/抓拍各一次
- [ ] 非 HOME 下 HOME 命令 409
- [ ] 错令牌 401、超长 body 400、未知路径 404
- [ ] 两客户端并发 `motion` 各 50 次无重启
- [ ] Setup 页重新生成与关闭
