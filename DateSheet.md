# 双UPS监控系统 - 数据字典（INA219 Only 版）

## 📁 硬件引脚定义

| 名称 | 类型 | 值 | 描述 |
|------|------|-----|------|
| `SDA_A` | `#define` | `4` | UPS A I2C数据线引脚 (XIAO D4) |
| `SCL_A` | `#define` | `5` | UPS A I2C时钟线引脚 (XIAO D5) |
| `SDA_B` | `#define` | `6` | UPS B I2C数据线引脚 (XIAO D6) |
| `SCL_B` | `#define` | `7` | UPS B I2C时钟线引脚 (XIAO D7) |
| `RELAY_A` | `#define` | `8` | UPS A继电器控制引脚 (XIAO D8) |
| `RELAY_B` | `#define` | `9` | UPS B继电器控制引脚 (XIAO D9) |
| `STATUS_LED` | `#define` | `2` | 状态指示LED引脚 (XIAO D2) |

## 📡 I2C地址定义

| 名称 | 类型 | 值 | 描述 |
|------|------|-----|------|
| `INA219_ADDR` | `#define` | `0x40` | INA219电流传感器I2C地址 |

## ⚙️ 系统参数定义

| 名称 | 类型 | 值 | 描述 |
|------|------|-----|------|
| `LOW_BAT_THRESHOLD` | `#define` | `20.0` | 低电量阈值(%) |
| `FULL_BAT_THRESHOLD` | `#define` | `95.0` | 满电量阈值(%) |
| `HEARTBEAT_TIMEOUT` | `#define` | `3000` | 心跳超时时间(ms) |
| `RELAY_CHARGE` | `#define` | `HIGH` | 继电器充电状态（吸合） |
| `RELAY_STOP` | `#define` | `LOW` | 继电器停止状态（断开） |
| `CHARGE_SWITCH_DELAY_MS` | `#define` | `1000` | 充电切换延迟(ms) |
| `RELAY_DEBOUNCE_MS` | `#define` | `2000` | 继电器防抖时间(ms) |
| `FULL_STABLE_TIME_MS` | `const unsigned long` | `3000` | 满电状态稳定时间(ms) |
| `FULL_EXIT_VOLTAGE_DROP` | `const float` | `0.3` | 退出满电状态的电压下降阈值(V) |
| `FULL_EXIT_CURRENT_THRESH` | `const float` | `150.0` | 退出满电状态的电流阈值(mA) |

## 🔢 枚举类型

### UPS_State (UPS状态机)
| 枚举值 | 数值 | 描述 |
|--------|------|------|
| `UPS_IDLE` | 0 | 空闲/待机状态（小电流或无电流） |
| `UPS_DISCHARGING` | 1 | 放电中状态（电流 < -50mA） |
| `UPS_CHARGING` | 2 | 充电中状态（电流 > 100mA） |
| `UPS_FULL` | 3 | 电池已充满（电压高且电流小） |
| `UPS_ERROR` | 4 | 故障/错误状态（当前代码未使用，保留） |

### ChargeTarget (充电仲裁目标)
| 枚举值 | 数值 | 描述 |
|--------|------|------|
| `CHARGE_NONE` | 0 | 无充电目标 |
| `CHARGE_A` | 1 | 给UPS A充电 |
| `CHARGE_B` | 2 | 给UPS B充电 |

## 📊 数据结构体

### INA_Data (INA219传感器数据)
| 成员 | 类型 | 描述 | 取值范围 |
|------|------|------|----------|
| `voltage_V` | `float` | 总线电压(V) | `0.0-20.0` |
| `current_mA` | `float` | 电流(mA) | 负值表示放电 |
| `power_mW` | `float` | 功率(mW) | `0.0-65535.0` |
| `timestamp_ms` | `unsigned long` | 时间戳(ms) | `0-4294967295` |

### UPS_Module (UPS模块结构)
| 成员 | 类型 | 描述 | 默认值 |
|------|------|------|--------|
| `name` | `char[3]` | UPS名称("A"/"B") | `"A"`/`"B"` |
| `wire` | `TwoWire*` | I2C总线指针 | `&Wire1`/`&Wire2` |
| `sda_pin` | `int` | SDA引脚号 | `4`/`6` |
| `scl_pin` | `int` | SCL引脚号 | `5`/`7` |
| `relay_pin` | `int` | 继电器引脚号 | `8`/`9` |
| `state` | `UPS_State` | 当前状态 | `UPS_IDLE` |
| `ina` | `INA_Data` | INA219数据 | 清零 |
| `relay_state` | `int` | 继电器状态 | `RELAY_STOP` |
| `last_relay_change` | `unsigned long` | 上次继电器变化时间 | `0` |
| `full_start_ms` | `unsigned long` | 满电稳定计时器 | `0` |
| `lowHysteresis` | `float` | 低电滞回(%) | `2.0f` |
| `fullHysteresis` | `float` | 满电滞回(%) | `2.0f` |

## 🌐 全局变量

| 名称 | 类型 | 描述 | 初始值 |
|------|------|------|--------|
| `Wire1` | `TwoWire` | UPS A I2C总线 | - |
| `Wire2` | `TwoWire` | UPS B I2C总线 | - |
| `ina219_A` | `Adafruit_INA219` | UPS A电流传感器 | - |
| `ina219_B` | `Adafruit_INA219` | UPS B电流传感器 | - |
| `upsA` | `UPS_Module` | UPS A模块 | 见结构体 |
| `upsB` | `UPS_Module` | UPS B模块 | 见结构体 |
| `last_ina_read_ms` | `unsigned long` | 上次INA读取时间 | `0` |
| `last_control_ms` | `unsigned long` | 上次控制更新时间 | `0` |
| `last_led_blink_ms` | `unsigned long` | 上次LED闪烁时间 | `0` |
| `led_state` | `bool` | LED当前状态 | `false` |
| `system_normal` | `bool` | 系统是否正常 | `true` |
| `last_heartbeat` | `unsigned long` | 上次心跳时间 | `0` |
| `charging_allowed` | `bool` | 是否允许充电 | `false` |
| `currentChargingTarget` | `ChargeTarget` | 当前充电目标 | `CHARGE_NONE` |
| `chargeSwitchPending` | `bool` | 充电切换挂起标志 | `false` |
| `chargeSwitchDelayStart` | `unsigned long` | 切换延迟开始时间 | `0` |
| `chargeSwitchDelayActive` | `bool` | 延迟激活标志 | `false` |
| `pending_charge_A` | `bool` | UPS A充电需求 | `false` |
| `pending_charge_B` | `bool` | UPS B充电需求 | `false` |

## 🔧 核心函数

| 函数名 | 返回值 | 描述 |
|--------|--------|------|
| `initUPS()` | `void` | 初始化UPS硬件和I2C总线 |
| `readINA219()` | `void` | 读取INA219传感器数据 |
| `estimateSOC()` | `float` | 根据电压估算SOC百分比 |
| `updateUPSState()` | `void` | 更新UPS状态机 |
| `updateChargingDemand()` | `void` | 更新充电需求（带滞回） |
| `applyChargingHysteresis()` | `bool` | 应用充电滞回控制逻辑 |
| `manageCharging()` | `void` | 管理充电仲裁和继电器控制 |
| `updateSystemStatus()` | `void` | 更新系统健康状态 |
| `updateStatusLED()` | `void` | 更新状态LED指示 |
| `handleHeartbeat()` | `void` | 处理树莓派心跳命令 |
| `printCompactInfo()` | `void` | 打印CSV格式监控数据 |

## 📊 串口输出格式

```
A,电压(V),电流(mA),电量(%),状态,继电器状态 B,电压(V),电流(mA),电量(%),状态,继电器状态,充电目标
```

**示例**：
```
A,7.60,120.0,78.5,CHG,ON B,7.62,-80.0,76.0,DIS,OFF,ACT_A
```

## 🔄 状态转换逻辑

```
┌─────────┐   电流 < -50mA   ┌────────────┐   电流 > 100mA   ┌─────────┐
│ UPS_IDLE │ ──────────────→ │ UPS_DISCH │ ───────────────→ │ UPS_CHG │
└─────────┘                  └────────────┘                  └─────────┘
     ↑                                                            │
     │             电压≥8.2V且电流<50mA（持续3秒）                ↓
     └───────────────┬───────────────┐      ┌─────────┐
                     ↓               ↓      │ UPS_FULL│
                 ┌─────────┐    ┌─────────┐ └─────────┘
                 │ UPS_ERR │    │ 异常恢复 │      │
                 └─────────┘    └─────────┘      ↓
                     ↑                          │
                     └───────────────┴──────────┘
```

## ⚠️ 关键设计说明

1. **SOC估算**：基于2串锂电电压（3.0V-4.2V）线性映射，无库仑计
2. **满电判定**：需要同时满足电压≥8.2V和电流<50mA持续3秒
3. **安全机制**：心跳丢失立即停充，继电器断电默认断开
4. **互斥充电**：同一时间仅允许一个UPS充电，防止过载
5. **滞回控制**：±2%滞回防止阈值点抖动

---

**💡 使用提示**：
- 所有时间单位均为毫秒(ms)
- 电量百分比范围：0.0-100.0%
- 电流负值表示放电，正值表示充电
- 继电器状态：`HIGH`=充电，`LOW`=停止
- 系统异常时LED以4Hz频率闪烁
