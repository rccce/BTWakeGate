# BTWakeGate r1：唤醒后让蓝牙更快恢复

对应报告第 8.12 节（问题 10）。r1 只做了语法检查和状态机模拟（在 Linux 上），**还没有用 macOS SDK 编译过，也没有在 macOS 上运行过**。

## 为什么要它

`btwake.sh` r2 的数据（11-19、11-20 共 8 次唤醒）显示三种情况：

| 情况 | 次数 | 发生了什么 | 蓝牙恢复 |
|---|---|---|---|
| 方式 A（大多数） | 5 | 睡眠中控制器断电。醒来后 +0.3 秒设备断开，旧 bluetoothd 读失败后退出；新实例 +0.4 秒启动，`IOKitWaitQuiet()` 立刻返回，+0.6 秒只找一次外接蓝牙，那时设备还没回来；之后它只认 Apple 内置控制器，15 秒后放弃 | 约 16.5 秒 |
| 好的一次 | 1（17:36） | 设备断开时旧 bluetoothd 没有先收到读错误，没有退出；+1.8 秒固件载完它就看到了新设备，+3 秒重启一次后直接找到 | 3.4 秒 |
| 方式 B | 1（17:07，只睡 2.7 秒） | 设备没有断开，但对所有端点（包括端点 0）都不应答（`0xe00002ed`）。没有任何组件复位它，每个新 bluetoothd 都找到一个"死"设备 | 一直不恢复，要重启 |

所以只要 bluetoothd 等固件载完再找，蓝牙约 3 秒就能回来（推断）；方式 B 需要有人复位这个设备。

## 它做什么

1. **闸门**：唤醒的第一条消息（`kIOMessageSystemWillPowerOn`，USB 设备上电之前）时，如果睡前有蓝牙控制器，它让自己处于"忙"状态。`IOKitWaitQuiet()` 要等所有 IOService 都不忙才返回，所以唤醒后新起的 bluetoothd 会在那里等。新的控制器（同一 USB 位置、不同的注册表对象）完成匹配、不忙时（IntelBluetoothFirmware 在 `start()` 里传完固件才返回）立刻放行。最多等 8 秒（`GateMaxMs`）；又要睡眠、驱动停止时也立刻放行。
2. **复位**：唤醒 2.5 秒（`SurvivorDelayMs`）后，睡前那个设备还在，而且确实睡过（日历时间减开机时间，至少 1 秒，`MinSleepMs`），就在单独的线程里调用 `IOUSBHostDevice::reset()`：端口重新枚举，IntelBluetoothFirmware 重新传固件，闸门等它就绪再放行。每次唤醒最多一次，10 分钟内最多 3 次（`MaxResets`）。睡眠被中途取消（没有真正睡）时不复位。

不改 IntelBluetoothFirmware、BlueToolFixup，不改系统设置。

## 编译（GitHub Actions，和 PkgCProbe 同一个仓库即可）

在仓库里新建 3 个文件：

| 仓库里的路径 | 内容 |
|---|---|
| `BTWakeGate/BTWakeGate.cpp` | 本目录的 `BTWakeGate.cpp` |
| `BTWakeGate/Info.plist` | 本目录的 `Info.plist` |
| `.github/workflows/btwakegate.yml` | 本目录的 `btwakegate.yml` |

提交后 Actions 里的 "BTWakeGate build" 会自动运行（推送 `Info.plist` 时 PkgCProbe 的编译也会跑一次，不用管）。完成后下载 `BTWakeGate`，解压得到 `BTWakeGate.kext`。编译失败时下载 `BTWakeGate-build-log` 发我。

## 安装

1. 把 `BTWakeGate.kext` 放进 `EFI/OC/Kexts/`。
2. 在 `config.plist` 的 `Kernel → Add` 里加一项（放在 IntelBluetoothFirmware 后面即可）：

```xml
<dict>
	<key>Arch</key>
	<string>x86_64</string>
	<key>BundlePath</key>
	<string>BTWakeGate.kext</string>
	<key>Comment</key>
	<string>BT wake gate</string>
	<key>Enabled</key>
	<true/>
	<key>ExecutablePath</key>
	<string>Contents/MacOS/BTWakeGate</string>
	<key>MaxKernel</key>
	<string></string>
	<key>MinKernel</key>
	<string></string>
	<key>PlistPath</key>
	<string>Contents/Info.plist</string>
</dict>
```

3. 重启后确认已加载：

```bash
ioreg -rw0 -c BTWakeGate -l -d 1 | grep -E '"(Version|Config|State)"'
```

应看到 `"Version" = "r1"` 和 `"State" = "idle"`。没有输出就是没加载，蓝牙和以前一样，把 `kmutil showloaded | grep -i btwake` 的结果发我。

## 之后

正常使用，不用专门测试。有过至少一次较长的睡眠（比如过夜）之后，运行新的 `btwake.sh`（r3，只读，不睡眠，约 3 分钟），把桌面上的 zip 发我：

```bash
sudo bash ~/Downloads/btwake.sh past
```

r3 会多列出 BTWakeGate 的状态和最近 12 次唤醒的记录（时间是 UTC，北京时间加 8 小时），每行像这样：

```
#3 2026-11-21 00:12:03 UTC | slept 9.5 h | old gone +0.30 s | reset no | new ready +2.90 s firmware loaded | gate 2.90 s: controller ready
```

| 字段 | 含义 |
|---|---|
| slept | 这次睡了多久（日历时间减开机时间） |
| old gone / old kept | 睡前的设备醒来后多久消失；kept 是一直没消失（方式 B 或睡眠被取消） |
| reset | 有没有复位、在什么时候、返回值（`0x00000000` 是成功） |
| new ready | 新设备完成匹配、不忙的时间，以及 IntelBluetoothFirmware 的 `FirmwareLoaded` |
| gate | 闸门关了多久、为什么放行 |

你自己也能直接感觉：唤醒后蓝牙鼠标、键盘等多久能用（以前约 16 秒）。

## 出问题时

- 只想停用闸门和复位、保留记录：把 `BTWakeGate.kext/Contents/Info.plist` 里的 `Enabled` 改成 `<false/>`。
- 只停用复位：把 `ResetSurvivor` 改成 `<false/>`。
- 完全停用：把 `config.plist` 里这一项的 `Enabled` 改成 `<false/>`。

## 可能的副作用（推断）

- 唤醒后最多约 8 秒内，其他调用 `IOKitWaitQuiet()` 的程序也会等。很少有程序在唤醒时这样做；而且唤醒时 USB 设备重新枚举本来就会让注册表忙 1 到 3 秒。
- 极短的睡眠后，如果控制器其实没坏，复位会让蓝牙重新初始化一次（约 3 到 6 秒）。在你这台上，真正睡过之后设备没消失只见过一次，那次控制器是坏的。
- `IOUSBHostDevice::reset()` 在内核里能不能直接调用（不先打开设备），我没有在 macOS 上验证。不行的话记录里 reset 会显示非 0 的返回值，闸门立刻放行，和现在一样。
