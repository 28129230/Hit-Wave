# 双UPS智能监控与充放电控制系统（INA219 Only 版）

[https://img.shields.io/badge/platform-XIAO%20SAMD21-blue](https://www.seeedstudio.com/Seeed-XIAO-SAMD21-Cortex-M0-p-4173.html)
[https://img.shields.io/badge/hardware-UPS%20Module%203S-green](https://www.waveshare.com/ups-module-3s.htm)
[https://img.shields.io/badge/status-production%20ready-brightgreen](https://github.com)

一个基于 **XIAO SAMD21** 的双路 UPS 智能监控系统，通过 **INA219** 传感器实现电压、电流、功率监测，并提供安全的继电器充电控制。

## 📖 项目简介

本系统用于监控和管理两套 3S 锂电池组（约 12.6V），通过独立的 I2C 总线读取电池状态，并根据预设策略自动控制充电继电器。**绝不同时给两个电池充电**，支持互斥充电和智能调度。

### ✨ 核心特性

- 🔒 **安全优先**：心跳丢失立即停充，继电器断电自动断开
- 🔄 **智能仲裁**：同一时间仅允许一个 UPS 充电，谁更空先充
- 📊 **实时监控**：200ms 采样周期，1Hz 数据输出
- 🛡️ **滞回控制**：±2% 滞回防止阈值抖动
- 🔧 **无额外MCU**：移除 STC8G1K08，仅依赖 INA219，简化硬件

## 🔌 硬件连接

### 引脚分配

| XIAO Pin | 连接目标 | 说明 |
|---------|---------|------|
| **D4** | UPS A SDA | I2C 总线 A 数据 |
| **D5** | UPS A SCL | I2C 总线 A 时钟 |
| **D6** | UPS B SDA | I2C 总线 B 数据 |
| **D7** | UPS B SCL | I2C 总线 B 时钟 |
| **D8** | Relay A IN | UPS A 继电器控制（高电平吸合） |
| **D9** | Relay B IN | UPS B 继电器控制（高电平吸合） |
| **D2** | Status LED+ | 状态指示（串联 220Ω 电阻到 GND） |
| **USB** | Raspberry Pi | 串口通信 (115200 波特率) |

### 硬件清单

- ✅ Seeed XIAO SAMD21 × 1
- ✅ 微雪 UPS Module 3S × 2
- ✅ 12V 继电器模块（支持高电平触发）× 2
- ✅ 220Ω 电阻 × 1
- ✅ LED × 1
- ✅ 杜邦线若干

## 🚀 快速开始

### 1️⃣ 软件环境准备

1. 安装 **Arduino IDE**
2. 添加 XIAO SAMD21 开发板支持：
   - 文件 → 首选项 → 附加开发板管理器网址：
     ```
     https://files.seeedstudio.com/arduino/package_seeeduino_boards_index.json
     ```
3. 安装库：
   - `Adafruit INA219`（库管理器搜索安装）

### 2️⃣ 编译与烧录

```bash
# 1. 打开 dual_ups_monitor.ino
# 2. 选择开发板：Tools → Board → Seeed SAMD → Seeed XIAO SAMD21
# 3. 选择端口：Tools → Port → 选择 XIAO 对应串口
# 4. 点击 Upload 烧录
```

### 3️⃣ 树莓派配置

启用串口通信：
```bash
sudo raspi-config
# Interface Options → Serial Port
# 选择：Disable shell access over serial, Enable serial port hardware
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

| 字段 | 说明 |
|------|------|
| `A`/`B` | UPS 标识 |
| `7.60` | 电压 (V) |
| `120.0` | 电流 (mA，正值充电，负值放电) |
| `78.5` | 估算电量 (%) |
| `CHG` | 状态：IDLE/DIS/CHG/FULL |
| `ON` | 继电器状态 |
| `ACT_A` | 当前充电目标 |

## ⚙️ 参数配置

在代码中可调整的关键参数：

```cpp
// 充电阈值
#define LOW_BAT_THRESHOLD   20.0   // 低于20%启动充电
#define FULL_BAT_THRESHOLD  95.0   // 高于95%停止充电

// 滞回参数（UPS_Module 结构体）
float lowHysteresis = 2.0;     // 低电滞回 ±2%
float fullHysteresis = 2.0;    // 满电滞回 ±2%

// 时间参数
#define HEARTBEAT_TIMEOUT   3000   // 心跳超时 3秒
#define CHARGE_SWITCH_DELAY_MS  1000   // 切换延迟 1秒
#define RELAY_DEBOUNCE_MS       2000   // 继电器防抖 2秒
```

## 🔧 故障排查

### INA219 未检测到
```bash
# 运行 I2C 扫描器，应看到：
# - UPS A: 0x40
# - UPS B: 0x40
```
**解决**：检查 SDA/SCL 接线，确认上拉电阻已连接。

### 继电器不动作
1. 确认继电器为 **高电平触发**
2. 检查 `charging_allowed` 是否为 `true`
3. 观察串口输出的 `Target` 字段是否正确

### 电量估算不准
- 这是正常现象，因为没有库仑计
- 可通过调整 `estimateSOC()` 中的电压范围进行校准
- 建议在实际使用中记录满电和空电的电压值

## 📊 系统状态指示

| LED 状态 | 含义 |
|---------|------|
| 常亮 | 系统正常（两个 UPS 电量均 ≥20%） |
| 快闪（4Hz） | 异常（至少一个 UPS 电量 <20% 或通信异常） |

## ⚠️ 安全注意事项

1. **首次上电测试**：先断开继电器接线，观察串口日志确认逻辑正确
2. **电池参数**：代码默认 2 串锂电（满电 8.4V），如需 3 串请修改 `FULL_VOLTAGE`
3. **散热**：继电器和大电流路径需预留散热空间
4. **断电保护**：系统断电时继电器自动断开，充电立即停止
5. **共地处理**：确保所有设备共地，避免电位差损坏设备

## 🔍 技术细节

### SOC 估算算法
```cpp
SOC = (单节电压 - 3.0) / (4.2 - 3.0) * 100%
```
- 3.0V = 0%，4.2V = 100%
- 线性映射，无库仑计

### 状态机逻辑
```
IDLE  ←→  DISCHARGING  ←→  CHARGING  ←→  FULL
```

### 充电仲裁规则
1. 心跳丢失 → 立即停止所有充电
2. 仅当一个 UPS 需要充电时才开启继电器
3. 两个 UPS 都需要充电 → 选择 SOC 更低的
4. 充满一个后延迟 1 秒切换到另一个

## 📁 文件结构

```
dual-ups-monitor/
├── dual_ups_monitor.ino    # 主程序
├── README.md               # 本文档
└── docs/
    └── data_dictionary.md  # 数据字典
```

## 🔄 版本历史

### v2.0 (INA219 Only)
- ✅ 移除 STC8G1K08 相关代码
- ✅ 简化状态机逻辑
- ✅ 优化满电判定算法
- ✅ 完善安全机制

### v1.0
- 初始版本，支持 STC8G1K08

## 📜 许可证

MIT License

## 🙏 致谢

- https://github.com/adafruit/Adafruit_INA219
- https://wiki.seeedstudio.com/Seeeduino-XIAO/
- https://www.waveshare.com/wiki/UPS_Module_3S

---

**⭐ 如果这个项目对你有帮助，请点个 Star！**
