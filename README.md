# 双UPS智能监控与充放电控制系统

[https://img.shields.io/badge/platform-XIAO%20SAMD21-blue](https://www.seeedstudio.com/Seeed-XIAO-SAMD21-Cortex-M0-p-4173.html)
[https://img.shields.io/badge/hardware-UPS%20Module%203S-green](https://www.waveshare.com/ups-module-3s.htm)

一个基于 **XIAO SAMD21** 的双路 UPS 智能监控系统，具备 I2C 死锁恢复、继电器安全控制和树莓派心跳保活功能。

## 📖 项目简介

本系统用于监控和管理两套 3S 锂电池组（约 12.6V），通过独立的 I2C 总线读取电压、电流和电量，并根据预设策略自动控制充电继电器。**绝不同时给两个电池充电**，支持轮流充电和智能调度。

## ✨ 核心特性

### 🔒 安全设计（第一优先级）
- **继电器断电安全**：`LOW` = 停止充电（断电自动断开）
- **心跳保活**：树莓派必须每 3 秒发送 `CHG:1`，超时立即禁充
- **I2C 总线死锁恢复**：L1（9脉冲）+ L3（重启外设）
- **继电器防抖**：2 秒切换延迟，防止频繁动作

### 🔄 智能充电仲裁
- **互斥充电**：同一时间仅允许一个 UPS 充电
- **谁更空先充**：双 UPS 同时低电量时，优先给 SOC 更低的充电
- **轮流充电**：一个充满后自动切换到另一个
- **滞回控制**：±2% 滞回，防止阈值点抖动

### 📊 数据监控
- **INA219**：持续监测电压、电流、功率（200ms 周期）
- **STC8G1K08**：精确电量读取（充电时每 5 秒一次）
- **STC 离线降级**：STC 故障时自动切换为电压估算

## 🔌 硬件连接

| XIAO Pin | 连接目标 | 备注 |
|---------|---------|------|
| **D4** | UPS A SDA | I2C 总线 A |
| **D5** | UPS A SCL | I2C 总线 A |
| **D6** | UPS B SDA | I2C 总线 B |
| **D7** | UPS B SCL | I2C 总线 B |
| **D8** | Relay A IN | 高电平吸合（充电） |
| **D9** | Relay B IN | 高电平吸合（充电） |
| **D2** | Status LED+ | 串联 220Ω 电阻到 GND |
| **USB** | Raspberry Pi | 串口通信 (115200) |

## 📦 硬件清单

- XIAO SAMD21 × 1
- 微雪 UPS Module 3S × 2
- 12.6V 继电器模块 × 2
- 220Ω 电阻 × 1
- LED × 1

## 🚀 快速开始

### 1️⃣ 编译环境
- Arduino IDE
- 安装库：
  - `Adafruit INA219`
  - `Adafruit SleepyDog`（可选，看门狗）

### 2️⃣ 烧录代码
1. 打开 `dual_ups_monitor.ino`
2. 选择开发板：`Seeed XIAO SAMD21`
3. 选择端口并上传

### 3️⃣ 树莓派配置
启用串口：
```bash
sudo raspi-config
# Interface Options → Serial Port → Disable login shell, Enable serial port hardware
```

## 📡 通信协议

### 树莓派 → XIAO（命令）
```
CHG:1    # 允许充电，重置心跳计时
CHG:0    # 禁止充电
```

### XIAO → 树莓派（CSV 数据，1Hz）
```csv
A,7.60,120.0,78.5,CHG,ON B,7.62,-80.0,76.0,DIS,OFF,ACT_A
```

字段说明：
| 字段 | 含义 |
|------|------|
| Name | UPS 名称 (A/B) |
| Voltage | 电压 (V) |
| Current | 电流 (mA) |
| SOC | 电量 (%) |
| State | IDLE/DIS/CHG/FULL/ERR |
| Relay | ON/OFF |
| Target | ACT_A/ACT_B/IDLE |

## ⚙️ 参数配置

```cpp
// 充电阈值
#define LOW_BAT_THRESHOLD   20.0   // 低于20%开启充电
#define FULL_BAT_THRESHOLD  95.0   // 高于95%停止充电

// 滞回参数（可在结构体中调整）
float lowHysteresis = 2.0;     // 低电滞回 ±2%
float fullHysteresis = 2.0;    // 满电滞回 ±2%

// 时间参数
#define HEARTBEAT_TIMEOUT   3000   // 心跳超时 3秒
#define CHARGE_SWITCH_DELAY_MS  1000   // 切换延迟 1秒
#define RELAY_DEBOUNCE_MS       2000   // 继电器防抖 2秒
```

## 🔧 故障排查

### UPS A/B INA219 not found
- 检查 I2C 接线（SDA/SCL 是否反接）
- 确认上拉电阻（4.7kΩ）已连接
- 测量 UPS 模块供电是否正常

### STC 通信失败
- 检查 STC8G1K08 是否已正确焊接
- 观察串口输出是否有 `WARN` 提示
- 系统会自动降级为电压估算，不影响基本功能

### 继电器不动作
- 确认继电器为 **高电平触发**
- 检查 `charging_allowed` 是否为 `true`
- 观察串口输出的 `Target` 字段是否正确

## 📈 系统状态指示

| LED 状态 | 含义 |
|---------|------|
| 常亮 | 系统正常（两个 UPS 均健康） |
| 快闪（4Hz） | 异常（至少一个 UPS 电量 <20% 或 STC 离线） |

## 🛡️ 安全注意事项

1. **首次上电测试**：先断开继电器接线，观察串口日志确认逻辑正确
2. **电池参数**：确保电池为 2S 锂电（满电 8.4V），如需 3S 请修改电压估算公式
3. **散热**：继电器和大电流路径需预留散热空间
4. **断电保护**：系统断电时继电器自动断开，充电立即停止

## 📝 更新日志

### v2.0 (当前版本)
- ✅ 实现双 UPS 互斥充电
- ✅ 添加滞回控制，防止抖动
- ✅ STC 离线降级为电压估算
- ✅ 完善心跳超时保护

### v1.0
- 基础 INA219 监控
- 简单继电器控制

## 📜 许可证

MIT License

## 🙏 致谢

- https://github.com/adafruit/Adafruit_INA219
- https://wiki.seeedstudio.com/Seeeduino-XIAO/
- https://www.waveshare.com/wiki/UPS_Module_3S

---

**⭐ 如果这个项目对你有帮助，请点个 Star！**
