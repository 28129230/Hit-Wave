# 双UPS监控系统 - 数据字典

## 📁 硬件引脚定义

| 名称 | 类型 | 值 | 描述 |
|------|------|-----|------|
| `SDA_A` | `#define` | `4` | UPS A I2C数据线引脚 |
| `SCL_A` | `#define` | `5` | UPS A I2C时钟线引脚 |
| `SDA_B` | `#define` | `6` | UPS B I2C数据线引脚 |
| `SCL_B` | `#define` | `7` | UPS B I2C时钟线引脚 |
| `RELAY_A` | `#define` | `8` | UPS A继电器控制引脚 |
| `RELAY_B` | `#define` | `9` | UPS B继电器控制引脚 |
| `STATUS_LED` | `#define` | `2` | 状态指示LED引脚 |

## 📡 I2C地址定义

| 名称 | 类型 | 值 | 描述 |
|------|------|-----|------|
| `STC_ADDR` | `#define` | `0x2D` | STC8G1K08芯片I2C地址 |
| `INA219_ADDR` | `#define` | `0x40` | INA219电流传感器I2C地址 |

## 📋 STC寄存器定义

| 名称 | 类型 | 值 | 描述 |
|------|------|-----|------|
| `REG_CHG_STATUS` | `#define` | `0x02` | 充电状态寄存器 |
| `REG_COMM_STATUS` | `#define` | `0x03` | 通信状态寄存器 |
| `REG_BAT_VOLT_L` | `#define` | `0x20` | 电池电压低字节 |
| `REG_BAT_VOLT_H` | `#define` | `0x21` | 电池电压高字节 |
| `REG_BAT_CURR_L` | `#define` | `0x22` | 电池电流低字节 |
| `REG_BAT_CURR_H` | `#define` | `0x23` | 电池电流高字节 |
| `REG_BAT_PERC_L` | `#define` | `0x24` | 电池百分比低字节 |
| `REG_BAT_PERC_H` | `#define` | `0x25` | 电池百分比高字节 |

## ⚙️ 系统参数定义

| 名称 | 类型 | 值 | 描述 |
|------|------|-----|------|
| `LOW_BAT_THRESHOLD` | `#define` | `20.0` | 低电量阈值(%) |
| `FULL_BAT_THRESHOLD` | `#define` | `95.0` | 满电量阈值(%) |
| `HEARTBEAT_TIMEOUT` | `#define` | `3000` | 心跳超时时间(ms) |
| `RELAY_CHARGE` | `#define` | `HIGH` | 继电器充电状态 |
| `RELAY_STOP` | `#define` | `LOW` | 继电器停止状态 |
| `CHARGE_SWITCH_DELAY_MS` | `#define` | `1000` | 充电切换延迟(ms) |
| `RELAY_DEBOUNCE_MS` | `#define` | `2000` | 继电器防抖时间(ms) |

## 🔢 枚举类型

### UPS_State (UPS状态机)
| 枚举值 | 数值 | 描述 |
|--------|------|------|
| `UPS_IDLE` | 0 | 空闲/待机状态 |
| `UPS_DISCHARGING` | 1 | 放电中状态 |
| `UPS_CHARGING` | 2 | 充电中状态 |
| `UPS_FULL` | 3 | 电池已充满 |
| `UPS_ERROR` | 4 | 故障/错误状态 |

### ChargeTarget (充电仲裁目标)
| 枚举值 | 数值 | 描述 |
|--------|------|------|
| `CHARGE_NONE` | 0 | 无充电目标 |
| `CHARGE_A` | 1 | 给UPS A充电 |
| `CHARGE_B` | 2 | 给UPS B充电 |

## 📊 数据结构体

### STC_Data (STC芯片数据)
| 成员 | 类型 | 描述 | 取值范围 |
|------|------|------|----------|
| `i2c_ok` | `bool` | I2C通信是否正常 | `true/false` |
| `is_charging` | `bool` | 是否正在充电 | `true/false` |
| `vbus_present` | `bool` | VBUS是否存在 | `true/false` |
| `charge_mode` | `uint8_t` | 充电模式 | `0x00-0x0F` |
| `ip2368_ok` | `bool` | IP2368芯片状态 | `true/false` |
| `bq4050_ok` | `bool` | BQ4050芯片状态 | `true/false` |
| `voltage_mv` | `uint16_t` | 电池电压(mV) | `0-65535` |
| `current_ma` | `int16_t` | 电池电流(mA) | `-32768-32767` |
| `percent` | `uint16_t` | 电池百分比(0.01%) | `0-10000` |

### INA_Data (INA219传感器数据)
| 成员 | 类型 | 描述 | 取值范围 |
|------|------|------|----------|
| `voltage_V` | `float` | 总线电压(V) | `0.0-20.0` |
| `current_mA` | `float` | 电流(mA) | `-32768.0-32767.0` |
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
| `stc` | `STC_Data` | STC数据 | 清零 |
| `stc_online` | `bool` | STC在线状态 | `false` |
| `stc_fail_count` | `uint8_t` | STC失败计数 | `0` |
| `last_stc_read_ms` | `unsigned long` | 上次STC读取时间 | `0` |
| `last_stc_fail_ms` | `unsigned long` | 上次STC失败时间 | `0` |
| `full_stc_read_done` | `bool` | 满电STC读取完成 | `false` |
| `relay_state` | `int` | 继电器状态 | `RELAY_STOP` |
| `last_relay_change` | `unsigned long` | 上次继电器变化时间 | `0` |
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
| `initUPS()` | `void` | 初始化UPS硬件 |
| `recoverI2CBus()` | `bool` | I2C总线恢复(L1+L3) |
| `attemptSTCReadWithRecovery()` | `bool` | 尝试读取STC(带恢复) |
| `readSTCWithRecovery()` | `bool` | 读取STC(带重试机制) |
| `readINA219()` | `void` | 读取INA219数据 |
| `updateUPSState()` | `void` | 更新UPS状态机 |
| `handleSTCReading()` | `void` | 处理STC读取调度 |
| `handleHeartbeat()` | `void` | 处理心跳检测 |
| `updateChargingDemand()` | `void` | 更新充电需求 |
| `applyChargingHysteresis()` | `bool` | 应用充电滞回控制 |
| `manageCharging()` | `void` | 管理充电仲裁 |
| `updateSystemStatus()` | `void` | 更新系统状态 |
| `updateStatusLED()` | `void` | 更新状态LED |
| `printCompactInfo()` | `void` | 打印CSV格式数据 |

## 📊 串口输出格式

```
A,电压(V),电流(mA),电量(%),状态,继电器状态 B,电压(V),电流(mA),电量(%),状态,继电器状态,充电目标
```

**示例**：
```
A,7.60,120.0,78.5,CHG,ON B,7.62,-80.0,76.0,DIS,OFF,ACT_A
```

## 🔄 状态转换图

```
┌─────────┐   放电    ┌────────────┐   充电    ┌─────────┐
│ UPS_IDLE │ ───────→ │ UPS_DISCH │ ───────→ │ UPS_CHG │
└─────────┘           └────────────┘           └─────────┘
     ↑                                            │
     │             充满                            ↓
     └───────────────┬───────────────┐      ┌─────────┐
                     ↓               ↓      │ UPS_FULL│
                 ┌─────────┐    ┌─────────┐ └─────────┘
                 │ UPS_ERR │    │ 异常恢复 │      │
                 └─────────┘    └─────────┘      ↓
                     ↑                          │
                     └───────────────┴──────────┘
```

---

**💡 使用提示**：
- 查找特定变量时，可按`Ctrl+F`搜索
- 所有时间单位均为毫秒(ms)
- 电量百分比范围：0.0-100.0%
- 继电器状态：`HIGH`=充电，`LOW`=停止
