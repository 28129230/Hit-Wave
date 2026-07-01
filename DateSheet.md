# 双UPS智能监控与充放电控制器 — Data Sheet

| 项目 | 说明 |
|----|----|
| **型号** | DUCM-2S (Dual UPS Controller, Micro, 2-Stack) |
| **版本** | FW v1.0 |
| **主控** | Seeed Studio XIAO SAMD21 (Cortex-M0+, 48MHz, 32KB SRAM) |
| **适用负载** | 树莓派 / 单板机 / 低功耗网络设备（经微雪 UPS 3S 供电） |
| **电池配置** | 双路 2×18650 串联（每路 ~6–8.4V），经 UPS Module 3S 升压至 5V |
| **文档状态** | 工程发布版 |

---

## 1. 系统框图（逻辑）

```
         ┌──────────────┐
         │  树莓派       │
         │  CHG:1/0     │◄─Serial1 115200─►
         │  heartbeat    │   D6/D7
         └──────────────┘
                │
         ┌──────▼──────┐
         │ XIAO SAMD21  │
         │              │── D0 ──▶ 继电器A ──▶ UPS A 充电
         │ 双路 I2C     │── D1 ──▶ 继电器B ──▶ UPS B 充电
         │ Wire1(0x40)  │── D2/D3 ◄── INA219 A
         │ Wire2(0x40)  │── D4/D5 ◄── INA219 B
         │              │── D8/D10 ◄── 双色LED
         └──────────────┘
           ↑ USB (Serial 调试)
```

---

## 2. 引脚定义（绝对最大）

| 引脚 | 功能 | 类型 | 备注 |
|----|----|----|----|
| D0 | RELAY_A | OUT | 高电平=充电，低电平=停止 |
| D1 | RELAY_B | OUT | 同上 |
| D2 | SDA_A | I2C | SERCOM0，UPS A |
| D3 | SCL_A | I2C | SERCOM0，UPS A |
| D4 | SDA_B | I2C | SERCOM1，UPS B |
| D5 | SCL_B | I2C | SERCOM1，UPS B |
| D6 | Serial1 TX | UART | → 树莓派 RX |
| D7 | Serial1 RX | UART | ← 树莓派 TX |
| D8 | LED_GREEN | PWM | 双色LED 绿通道 |
| D10 | LED_RED | PWM | 双色LED 红通道 |

> ⚠️ 微雪 UPS Module 3S 的 INA219 地址**固定 0x40**，必须用独立 I2C 总线，不可并联。

---

## 3. 电气特性

| 参数 | 典型值 | 备注 |
|----|----|----|
| I2C 速率 | 100 kHz | Wire1 / Wire2 |
| INA219 采样率 | 5 Hz（每路 200ms） | `last_ina_read_ms` |
| 控制环路周期 | 1 Hz | `last_control_ms` |
| 串口波特率 | 115200 8N1 | Serial + Serial1 |
| 继电器防抖 | 2 s | `RELAY_DEBOUNCE_MS` |
| 充电切换延迟 | 1 s | `CHARGE_SWITCH_DELAY_MS` |
| 心跳超时 | 3 s | `HEARTBEAT_TIMEOUT` |
| LED 轮询槽 | 4 s / 路 | `LED_SLOT_DURATION_MS` |

---

## 4. 电池状态机

```
                    curr > +100mA
        ┌──────────────────────────────► CHARGING
        │                               │
        │  volt ≥ 8.2 & │cur│< 50mA    │ curr ≤ +100mA
        │  (稳3s)                      │
   IDLE ├──────────────────────► FULL ─┤
        │                               │
        │  curr < -50mA                 │ volt < 8.1 或 │cur│>150mA
        │                               │
        └──────────────────────────────► DISCHARGING
        ▲
        │ INA219 无效 / 越界
        └ ERROR (需连续5次有效+2s冷却恢复)
```

| 状态 | 判定条件 |
|----|----|
| IDLE | 电流接近 0，电压未满 |
| CHARGING | 电流 > +100 mA |
| DISCHARGING | 电流 < -50 mA |
| FULL | 电压 ≥ 8.2V + 电流 < 50mA 持续 3s |
| ERROR | 电压 NaN / <0.1V / >20V，或 INA219 初始化失败 |

---

## 5. SOC 估算

> ⚠️ **基于开路电压线性近似，仅作参考，不适用于精密场景**

```
单节电压 = V_bus / 2
SOC = (V_per_cell − 3.0) / (4.2 − 3.0) × 100%
```

| 参数 | 值 |
|----|----|
| 空电电压（单节） | 3.0 V |
| 满电电压（单节） | 4.2 V |
| 满电组端电压 | 8.4 V |
| 低电阈值 | SOC < 20%（触发异常+日志） |
| 充电开启 | SOC ≤ 20% − 滞回 |
| 充电停止 | SOC ≥ 95% + 滞回 |
| 默认滞回 | 2%（`lowSocOffset` / `fullSocOffset`） |

---

## 6. 充电仲裁逻辑

- **互斥原则**：同一时刻仅一路电池可被充电
- **触发**：`charging_allowed == true` 且 SOC 低于低电阈值
- **双路同时需求**：SOC 较低者优先
- **释放**：当前目标不再需求 → 延迟 1s → `CHARGE_NONE`
- **心跳断开**：`charging_allowed = false`，两路继电器均断开

```
树莓派 CHG:1 ──► charging_allowed = true ──► 仲裁器工作
     CHG:0 / 超时 3s ──► charging_allowed = false ──► 全路停止
```

---

## 7. LED 编码（核心诊断功能）

每 4s 轮询切换 A / B：

| 时段 | 颜色 | 含义 |
|----|----|----|
| A 槽 | 绿 | 电池 A |
| B 槽 | 黄（绿+红） | 电池 B |

| 状态 | 显示 |
|----|----|
| 正常 (IDLE/FULL) | 长呼吸 2s |
| 充电 | 常亮 |
| 放电 | 短呼吸 1s |
| **异常 (ERROR 或 SOC<20%)** | 🔴 **红快闪 4Hz** |

> 💡 定位方法：看到红灯时记住当前是 A 槽还是 B 槽 → 对应电池故障。

---

## 8. 串口协议（Serial1 ↔ 树莓派）

### 8.1 树莓派 → 控制器

| 指令 | 含义 |
|----|----|
| `CHG:1\n` | 允许充电 |
| `CHG:0\n` | 禁止充电 |

### 8.2 控制器 → 树莓派（返回）

| 报文 | 触发条件 |
|----|----|
| `HB:ALLOW` | 收到 CHG:1 |
| `HB:DENY` | 收到 CHG:0 |
| `HB:TIMEOUT` | 超过 3s 未收到心跳 |
| `ERR:A,INA219_INIT_FAIL` | 上电 INA219 失败 |
| `ERR:B,INVALID_VOLTAGE,V=xx.xx` | 采样越界 |
| `ERR:A,LOW_SOC,V=xx.x` | SOC < 20%（1h 去重） |
| `RECOVER:A` | 从 ERROR 恢复 |

### 8.3 周期状态行（1Hz）

格式：
```
A,<V>,<mA>,<SOC>,<STATE>,<RLY> B,<V>,<mA>,<SOC>,<STATE>,<RLY>,<ACTIVE>
```

示例：
```
A,7.82,-120,78.5,DIS,OFF B,8.10,350,92.0,CHG,ON,ACT_B
```

STATE 编码：`CHG / DIS / FULL / IDLE / ERR`  
ACTIVE 编码：`ACT_A / ACT_B / IDLE`

---

## 9. 错误恢复模型

| 阶段 | 行为 |
|----|----|
| 异常检测 | 单次无效采样即进入 ERROR |
| 冷却期 | 进入 ERROR 时设 `block_until = now + 2s` |
| 恢复计数 | 冷却期过后，连续 **5 次**有效采样 |
| 恢复动作 | → IDLE，清日志标志，发 `RECOVER:X` |

> 防止 I2C 瞬断造成"假恢复"。

---

## 10. 关键宏参数表

| 宏 | 默认值 | 说明 |
|----|----|----|
| `LOW_BAT_THRESHOLD` | 20.0 | 低电 SOC % |
| `FULL_BAT_THRESHOLD` | 95.0 | 满电 SOC % |
| `HEARTBEAT_TIMEOUT` | 3000 ms | 心跳超时 |
| `RELAY_DEBOUNCE_MS` | 2000 ms | 继电器防抖 |
| `CHARGE_SWITCH_DELAY_MS` | 1000 ms | 充电切换延迟 |
| `ERROR_RECOVERY_GOOD_READS` | 5 | 恢复所需连续有效采样 |
| `ERROR_RECOVERY_MIN_INTERVAL_MS` | 2000 ms | 恢复冷却期 |
| `LOW_LOG_REPEAT_INTERVAL_MS` | 3600000 ms (1h) | 低电日志重复窗口 |

---

## 11. 安全声明

> ⚠️ **继电器仅控制充电回路，不得用于切断 UPS 主供电**  
> ⚠️ SOC 为电压估算法，低温 / 大电流下偏差增大  
> ⚠️ 上电前确认 I2C 引脚与 SERCOM 分配，XIAO 引脚复用错误会导致 INA219 无法识别  
> ⚠️ 锂电池相关操作请确认硬件保护（UPS Module 3S 已内置，但仍建议外部保险）

---

## 12. 配套建议（树莓派侧）

- 用 `systemd` 服务维持心跳进程，崩溃自动拉起
- 心跳周期建议 **1 Hz**（`echo "CHG:1" > /dev/ttyS0`）
- 建议监听 `ERR:` / `LOW_SOC` 做告警（Telegram / 邮件）
- 建议监听 `HB:TIMEOUT` 作为本机 watchdog 喂狗失败信号

---

> 📋 **DUCM-2S Data Sheet v1.0** — 适用于固件提交、硬件归档、现场运维参考。  
> 如需配套的 **树莓派心跳守护进程规格 / 接线装配图 / 故障排查 flow chart**，可继续展开。
