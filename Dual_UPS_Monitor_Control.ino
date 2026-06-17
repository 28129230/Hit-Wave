/**
 * 双UPS智能监控与充放电控制 - 完整版
 * 硬件：XIAO SAMD21 + 2块微雪 UPS Module 3S
 * 
 * 功能：
 * - 双路独立I2C总线（解决地址冲突）
 * - INA219持续读取电压、电流、功率
 * - STC8G1K08按需读取（充电时每5秒一次，状态跳变时立即读）
 * - I2C总线死锁软件恢复（L1:9脉冲+空闲检测，L3:重启外设）
 * - 继电器自动充电（电量<20%开启，>95%停止，防抖2秒）
 * - 通过串口接收树莓派心跳，同时控制充电许可（超时3秒自动禁止）
 * - 单LED指示：系统正常常亮，异常快闪（约4Hz）
 * - 串口每1秒输出CSV格式数据，供树莓派解析
 * 
 * 引脚分配：
 *   D4(SDA), D5(SCL) -> UPS A
 *   D6(SDA), D7(SCL) -> UPS B
 *   D8 -> 继电器 A (高电平充电)
 *   D9 -> 继电器 B (高电平充电)
 *   D2 -> 状态LED (阳极接D2，阴极串220Ω电阻到GND)
 *   USB串口与树莓派通信（发送心跳命令，接收CSV数据）
 * 
 * 树莓派端命令（每行以换行符结尾）：
 *   CHG:1  允许充电，同时重置心跳计时
 *   CHG:0  禁止充电
 * 超过3秒未收到任何命令，自动禁止充电
 */

#include <Wire.h>
#include <Adafruit_INA219.h>
#include "wiring_private.h"

// ========== 引脚定义 ==========
#define SDA_A       4
#define SCL_A       5
#define SDA_B       6
#define SCL_B       7
#define RELAY_A     8
#define RELAY_B     9
#define STATUS_LED  2

// ========== I2C 地址 ==========
#define STC_ADDR     0x2D   // STC8G1K08
#define INA219_ADDR  0x40

// ========== STC8G1K08 寄存器 ==========
#define REG_CHG_STATUS   0x02
#define REG_COMM_STATUS  0x03
#define REG_BAT_VOLT_L   0x20
#define REG_BAT_VOLT_H   0x21
#define REG_BAT_CURR_L   0x22
#define REG_BAT_CURR_H   0x23
#define REG_BAT_PERC_L   0x24
#define REG_BAT_PERC_H   0x25

// ========== 充电阈值 ==========
#define LOW_BAT_THRESHOLD   20.0   // 低于20%启动充电
#define FULL_BAT_THRESHOLD  95.0   // 高于95%停止充电


// ========== 心跳超时 (毫秒) ==========
#define HEARTBEAT_TIMEOUT   3000

// ========== 继电器安全定义 ==========
#define RELAY_CHARGE    HIGH   // HIGH = 吸合，允许充电（断电时自动断开，安全）
#define RELAY_STOP      LOW    // LOW = 断开，停止充电

// ========== 状态机枚举 ==========
enum UPS_State {
    UPS_IDLE,
    UPS_DISCHARGING,
    UPS_CHARGING,
    UPS_FULL,
    UPS_ERROR
};

// ========== STC 数据结构 ==========
struct STC_Data {
    bool  i2c_ok;
    bool  is_charging;
    bool  vbus_present;
    uint8_t charge_mode;
    bool  ip2368_ok;
    bool  bq4050_ok;
    uint16_t voltage_mv;
    int16_t  current_ma;
    uint16_t percent;      // 0-10000
};

// ========== INA219 数据结构 ==========
struct INA_Data {
    float voltage_V;
    float current_mA;
    float power_mW;
    unsigned long timestamp_ms;
};

// ========== UPS 模块结构体 ==========
struct UPS_Module {
    char      name[3];
    TwoWire*  wire;
    int       sda_pin;
    int       scl_pin;
    int       relay_pin;
    UPS_State state;
    INA_Data  ina;
    STC_Data  stc;
    bool      stc_online;
    uint8_t   stc_fail_count;
    unsigned long last_stc_read_ms;
    unsigned long last_stc_fail_ms;
    bool      full_stc_read_done;
    int       relay_state;
    unsigned long last_relay_change;
    
    // ===== 新增滞回参数 =====
    float lowHysteresis;     // 低电滞回（%）
    float fullHysteresis;    // 满电滞回（%）
};

// ========== 全局对象 ==========
TwoWire Wire1(&sercom0, SDA_A, SCL_A);
TwoWire Wire2(&sercom1, SDA_B, SCL_B);
Adafruit_INA219 ina219_A(INA219_ADDR, &Wire1);
Adafruit_INA219 ina219_B(INA219_ADDR, &Wire2);

UPS_Module upsA = {"A", &Wire1, SDA_A, SCL_A, RELAY_A, UPS_IDLE, {}, {}, false, 0, 0, 0, false, RELAY_STOP, 0, 2.0f, 2.0f};
UPS_Module upsB = {"B", &Wire2, SDA_B, SCL_B, RELAY_B, UPS_IDLE, {}, {}, false, 0, 0, 0, false, RELAY_STOP, 0, 2.0f, 2.0f};

unsigned long last_ina_read_ms = 0;
unsigned long last_control_ms = 0;
unsigned long last_led_blink_ms = 0;
bool led_state = false;
bool system_normal = true;

// 心跳相关
unsigned long last_heartbeat = 0;
bool charging_allowed = false;   // 默认禁止充电，等待树莓派发送 CHG:1

// ========== 充电仲裁器 ==========
enum ChargeTarget {
    CHARGE_NONE,
    CHARGE_A,
    CHARGE_B
};

// 充电仲裁状态
ChargeTarget currentChargingTarget = CHARGE_NONE;  // 当前正在充电的目标
bool chargeSwitchPending = false;                  // 切换延迟标志
unsigned long chargeSwitchDelayStart = 0;           // 切换延迟开始时间
bool chargeSwitchDelayActive = false;               // 延迟是否激活

// 充电需求标志
bool pending_charge_A = false;
bool pending_charge_B = false;

// ========== 时间常量 ==========
#define CHARGE_SWITCH_DELAY_MS  1000   // 充满后切换延迟（毫秒）
#define RELAY_DEBOUNCE_MS       2000   // 继电器防抖时间（毫秒）

// ========== 函数声明 ==========
void initUPS();
bool recoverI2CBus(TwoWire &wire, int sda_pin, int scl_pin, int timeout_ms);
bool attemptSTCReadWithRecovery(UPS_Module &ups, STC_Data &out);
bool readSTCWithRecovery(UPS_Module &ups);
void readINA219(UPS_Module &ups);
void updateUPSState(UPS_Module &ups);
void handleSTCReading(UPS_Module &ups);
void updateChargingDemand(UPS_Module &ups, bool &need_flag);
bool applyChargingHysteresis(float soc, bool currently_charging, float lowHyst, float fullHyst);
void manageCharging();
void updateSystemStatus();
void updateStatusLED();
void handleHeartbeat();
void printCompactInfo();

// ========== 初始化 ==========
void setup() {
    Serial.begin(115200);
    pinMode(RELAY_A, OUTPUT);
    pinMode(RELAY_B, OUTPUT);
    pinMode(STATUS_LED, OUTPUT);
    digitalWrite(RELAY_A, RELAY_STOP);
    digitalWrite(RELAY_B, RELAY_STOP);
    digitalWrite(STATUS_LED, LOW);
    upsA.relay_state = RELAY_STOP;
    upsB.relay_state = RELAY_STOP;

    initUPS();
    delay(100);
}

void initUPS() {
    pinPeripheral(SDA_A, PIO_SERCOM_ALT);
    pinPeripheral(SCL_A, PIO_SERCOM_ALT);
    pinPeripheral(SDA_B, PIO_SERCOM_ALT);
    pinPeripheral(SCL_B, PIO_SERCOM_ALT);

    Wire1.begin();
    Wire1.setClock(100000);
    Wire2.begin();
    Wire2.setClock(100000);

    if (!ina219_A.begin()) Serial.println("WARN: UPS A INA219 not found");
    if (!ina219_B.begin()) Serial.println("WARN: UPS B INA219 not found");
}

// ========== I2C 总线恢复 (L1 + L3) ==========
bool recoverI2CBus(TwoWire &wire, int sda_pin, int scl_pin, int timeout_ms = 50) {
    wire.end();
    pinMode(scl_pin, OUTPUT);
    pinMode(sda_pin, OUTPUT);
    digitalWrite(sda_pin, LOW);
    for (int i = 0; i < 9; i++) {
        digitalWrite(scl_pin, HIGH);
        delayMicroseconds(5);
        digitalWrite(scl_pin, LOW);
        delayMicroseconds(5);
    }
    // STOP condition
    digitalWrite(sda_pin, LOW);
    delayMicroseconds(5);
    digitalWrite(scl_pin, HIGH);
    delayMicroseconds(5);
    digitalWrite(sda_pin, HIGH);
    delayMicroseconds(5);
    pinMode(sda_pin, INPUT_PULLUP);
    pinMode(scl_pin, INPUT_PULLUP);
    unsigned long start = millis();
    while ((digitalRead(sda_pin) == LOW || digitalRead(scl_pin) == LOW) &&
           (millis() - start) < timeout_ms) {
        delayMicroseconds(10);
    }
    bool idle = (digitalRead(sda_pin) == HIGH && digitalRead(scl_pin) == HIGH);
    if (idle) {
        wire.begin();
        wire.setClock(100000);
    }
    return idle;
}

bool attemptSTCReadWithRecovery(UPS_Module &ups, STC_Data &out) {
    TwoWire &wire = *(ups.wire);
    int sda = ups.sda_pin;
    int scl = ups.scl_pin;

    auto readOnce = [&]() -> bool {
        wire.beginTransmission(STC_ADDR);
        if (wire.endTransmission() != 0) return false;
        wire.beginTransmission(STC_ADDR);
        wire.write(REG_CHG_STATUS);
        if (wire.endTransmission(false) != 0) return false;
        if (wire.requestFrom(STC_ADDR, (uint8_t)1) != 1) return false;
        uint8_t chg = wire.read();
        out.is_charging = (chg >> 7) & 1;
        out.vbus_present = (chg >> 5) & 1;
        out.charge_mode = chg & 0x0F;
        wire.beginTransmission(STC_ADDR);
        wire.write(REG_COMM_STATUS);
        if (wire.endTransmission(false) != 0) return false;
        if (wire.requestFrom(STC_ADDR, (uint8_t)1) != 1) return false;
        uint8_t comm = wire.read();
        out.ip2368_ok = (comm >> 1) & 1;
        out.bq4050_ok = comm & 1;
        wire.beginTransmission(STC_ADDR);
        wire.write(REG_BAT_VOLT_L);
        if (wire.endTransmission(false) != 0) return false;
        if (wire.requestFrom(STC_ADDR, (uint8_t)2) != 2) return false;
        uint16_t v = wire.read() | (wire.read() << 8);
        out.voltage_mv = v;
        wire.beginTransmission(STC_ADDR);
        wire.write(REG_BAT_CURR_L);
        if (wire.endTransmission(false) != 0) return false;
        if (wire.requestFrom(STC_ADDR, (uint8_t)2) != 2) return false;
        int16_t i = wire.read() | (wire.read() << 8);
        out.current_ma = i;
        wire.beginTransmission(STC_ADDR);
        wire.write(REG_BAT_PERC_L);
        if (wire.endTransmission(false) != 0) return false;
        if (wire.requestFrom(STC_ADDR, (uint8_t)2) != 2) return false;
        uint16_t p = wire.read() | (wire.read() << 8);
        out.percent = p;
        out.i2c_ok = true;
        return true;
    };

    if (readOnce()) return true;
    if (recoverI2CBus(wire, sda, scl, 50)) {
        delay(20);
        if (readOnce()) return true;
    } else {
        out.i2c_ok = false;
        return false;
    }
    wire.end();
    delay(10);
    wire.begin();
    wire.setClock(100000);
    delay(20);
    if (readOnce()) return true;
    out.i2c_ok = false;
    return false;
}

bool readSTCWithRecovery(UPS_Module &ups) {
    if (ups.stc_fail_count > 0 && (millis() - ups.last_stc_fail_ms) < 10000) return false;
    if (ups.stc_fail_count >= 3) {
        ups.stc_online = false;
        ups.state = UPS_ERROR;
        return false;
    }
    STC_Data new_data;
    bool ok = attemptSTCReadWithRecovery(ups, new_data);
    if (ok) {
        ups.stc = new_data;
        ups.stc_online = true;
        ups.stc_fail_count = 0;
        ups.last_stc_fail_ms = 0;
        if (ups.state == UPS_ERROR) ups.state = UPS_IDLE;
        return true;
    } else {
        ups.stc_fail_count++;
        ups.last_stc_fail_ms = millis();
        ups.stc_online = false;
        return false;
    }
}

void readINA219(UPS_Module &ups) {
    if (&ups == &upsA) {
        ups.ina.voltage_V = ina219_A.getBusVoltage_V();
        ups.ina.current_mA = ina219_A.getCurrent_mA();
        ups.ina.power_mW = ina219_A.getPower_mW();
    } else {
        ups.ina.voltage_V = ina219_B.getBusVoltage_V();
        ups.ina.current_mA = ina219_B.getCurrent_mA();
        ups.ina.power_mW = ina219_B.getPower_mW();
    }
    ups.ina.timestamp_ms = millis();
}

void updateUPSState(UPS_Module &ups) {
    float curr = ups.ina.current_mA;
    float volt = ups.ina.voltage_V;
    UPS_State old_state = ups.state;
    const float CHARGE_CURRENT_THRESHOLD = 100.0;
    const float DISCHARGE_CURRENT_THRESHOLD = -50.0;
    const float FULL_VOLTAGE = 8.4;  // 2串锂电满电8.4V

    if (ups.stc_online) {
        if (ups.stc.vbus_present) {
            if (ups.stc.charge_mode == 0x05) ups.state = UPS_FULL;
            else ups.state = UPS_CHARGING;
        } else {
            if (curr < DISCHARGE_CURRENT_THRESHOLD) ups.state = UPS_DISCHARGING;
            else if (fabs(curr) < 20.0 && volt > FULL_VOLTAGE - 0.1) ups.state = UPS_FULL;
            else ups.state = UPS_IDLE;
        }
    } else {
        if (curr > CHARGE_CURRENT_THRESHOLD) ups.state = UPS_CHARGING;
        else if (curr < DISCHARGE_CURRENT_THRESHOLD) ups.state = UPS_DISCHARGING;
        else if (volt >= FULL_VOLTAGE - 0.2) ups.state = UPS_FULL;
        else ups.state = UPS_IDLE;
    }

    if (old_state != ups.state) {
        if ((old_state == UPS_DISCHARGING || old_state == UPS_IDLE) && ups.state == UPS_CHARGING) {
            readSTCWithRecovery(ups);
            ups.last_stc_read_ms = millis();
            ups.full_stc_read_done = false;
        }
        if (ups.state == UPS_FULL && !ups.full_stc_read_done) {
            readSTCWithRecovery(ups);
            ups.full_stc_read_done = true;
            ups.last_stc_read_ms = millis();
        }
        if (old_state == UPS_FULL && ups.state != UPS_FULL) {
            ups.full_stc_read_done = false;
        }
    }
}

void handleSTCReading(UPS_Module &ups) {
    unsigned long now = millis();
    if (ups.state == UPS_CHARGING && (now - ups.last_stc_read_ms >= 5000)) {
        readSTCWithRecovery(ups);
        ups.last_stc_read_ms = now;
    }
}

void handleHeartbeat() {
    while (Serial.available()) {
        String line = Serial.readStringUntil('\n');
        line.trim();
        if (line.startsWith("CHG:")) {
            int val = line.substring(4).toInt();
            charging_allowed = (val == 1);
            last_heartbeat = millis();
        }
    }
    if (millis() - last_heartbeat > HEARTBEAT_TIMEOUT) {
        charging_allowed = false;
    }
}


void updateChargingDemand(UPS_Module &ups, bool &need_flag) {
    // ========== 边界保护：STC 离线时降级为电压估算 ==========
    if (!ups.stc_online) {
        const float CELLS_IN_SERIES = 2.0;
        const float VOLTAGE_FULL = 4.2;
        const float VOLTAGE_EMPTY = 3.0;
        const float VOLTAGE_RANGE = VOLTAGE_FULL - VOLTAGE_EMPTY;

        float v_per_cell = ups.ina.voltage_V / CELLS_IN_SERIES;

        if (ups.ina.voltage_V < 0.1 || ups.ina.voltage_V > 20.0 || ups.ina.voltage_V != ups.ina.voltage_V) {
            need_flag = false;
            return;
        }

        float soc = constrain(
            (v_per_cell - VOLTAGE_EMPTY) / VOLTAGE_RANGE * 100.0,
            0.0, 100.0
        );

        // 使用结构体中的滞回值
        need_flag = applyChargingHysteresis(
            soc, 
            (ups.relay_state == RELAY_CHARGE),
            ups.lowHysteresis,
            ups.fullHysteresis
        );
        return;
    }

    // ========== STC 在线：使用精确数据 ==========
    if (ups.stc.percent < 0 || ups.stc.percent > 10000) {
    // 数据异常：降级为电压估算或保守处理
    need_flag = false;
    return;
    }
    float soc = ups.stc.percent / 100.0;   // 0~100

    if (!charging_allowed) {
        need_flag = false;
        return;
    }

    need_flag = applyChargingHysteresis(
        soc,
        (ups.relay_state == RELAY_CHARGE),
        ups.lowHysteresis,
        ups.fullHysteresis
    );
}

// ========== 充电滞回控制 ==========
bool applyChargingHysteresis(float soc, bool currently_charging, float lowHyst, float fullHyst) {
    if (currently_charging) {
        // 正在充电：需要达到满电阈值 + 满电滞回才停止
        if (soc >= FULL_BAT_THRESHOLD + fullHyst) {
            return false;
        }
        return true;
    } else {
        // 未充电：需要低于低电阈值 - 低电滞回才启动
        if (soc <= LOW_BAT_THRESHOLD - lowHyst) {
            return true;
        }
        return false;
    }
}

void manageCharging() {
    // ========== 第一道防线：心跳丢失，立刻无条件停充 ==========
    if (!charging_allowed) {
        if (upsA.relay_state != RELAY_STOP) {
            digitalWrite(RELAY_A, RELAY_STOP);
            upsA.relay_state = RELAY_STOP;
            upsA.last_relay_change = millis();
        }
        if (upsB.relay_state != RELAY_STOP) {
            digitalWrite(RELAY_B, RELAY_STOP);
            upsB.relay_state = RELAY_STOP;
            upsB.last_relay_change = millis();
        }
        currentChargingTarget = CHARGE_NONE;
        chargeSwitchPending = false;
        chargeSwitchDelayActive = false;
        return;
    }

    // ========== 第二道：更新充电需求 ==========
    updateChargingDemand(upsA, pending_charge_A);
    updateChargingDemand(upsB, pending_charge_B);

    // ========== 第三道：当前充电目标是否需要维持或切换 ==========
    bool currentNeedsCharge = false;
    if (currentChargingTarget == CHARGE_A)
        currentNeedsCharge = pending_charge_A;
    else if (currentChargingTarget == CHARGE_B)
        currentNeedsCharge = pending_charge_B;

    // 如果当前目标不再需要充电，且尚未挂起切换，则启动切换挂起
    if (!currentNeedsCharge && currentChargingTarget != CHARGE_NONE) {
        if (!chargeSwitchPending) {
            chargeSwitchPending = true;
            chargeSwitchDelayActive = false;   // 重置延迟激活标志
        }
    } 
    // 如果当前目标仍然需要充电，则清除所有切换标志
    else if (currentNeedsCharge) {
        chargeSwitchPending = false;
        chargeSwitchDelayActive = false;
    }

    // 如果切换已挂起，但延迟计时器尚未激活，则启动延迟
    if (chargeSwitchPending && !chargeSwitchDelayActive) {
        chargeSwitchDelayStart = millis();
        chargeSwitchDelayActive = true;
    }

    // 检查延迟是否已到期（仅当延迟计时器激活时）
    if (chargeSwitchPending && chargeSwitchDelayActive) {
        if (millis() - chargeSwitchDelayStart >= CHARGE_SWITCH_DELAY_MS) {
            currentChargingTarget = CHARGE_NONE;
            chargeSwitchPending = false;
            chargeSwitchDelayActive = false;
        }
    }

    // ========== 第四道：选择新的充电目标（谁更空先给谁） ==========
if (currentChargingTarget == CHARGE_NONE && (pending_charge_A || pending_charge_B)) {
    if (pending_charge_A && pending_charge_B) {
        // 两者都需要 → 选择 SOC 更低的（谁更空先给谁）
        float socA;
        if (upsA.stc_online && upsA.stc.percent >= 0 && upsA.stc.percent <= 10000) {
            socA = upsA.stc.percent / 100.0;
        } else {
            socA = 50.0;   // STC离线或数据异常，保守默认值
        }

        float socB;
        if (upsB.stc_online && upsB.stc.percent >= 0 && upsB.stc.percent <= 10000) {
            socB = upsB.stc.percent / 100.0;
        } else {
            socB = 50.0;
        }

        currentChargingTarget = (socA < socB) ? CHARGE_A : CHARGE_B;
    } else if (pending_charge_A) {
        currentChargingTarget = CHARGE_A;
    } else if (pending_charge_B) {
        currentChargingTarget = CHARGE_B;
    }
    chargeSwitchPending = false;
    chargeSwitchDelayActive = false;
}

    // ========== 第五道：根据最终决策设置继电器输出（带防抖） ==========
    int new_state_A = (currentChargingTarget == CHARGE_A) ? RELAY_CHARGE : RELAY_STOP;
    int new_state_B = (currentChargingTarget == CHARGE_B) ? RELAY_CHARGE : RELAY_STOP;

    unsigned long now = millis();

    // UPS A 控制
    if (new_state_A != upsA.relay_state) {
        if (now - upsA.last_relay_change > RELAY_DEBOUNCE_MS) {
            digitalWrite(upsA.relay_pin, new_state_A);
            upsA.relay_state = new_state_A;
            upsA.last_relay_change = now;
        }
    } else {
        upsA.last_relay_change = now;   // 状态一致时刷新计时
    }

    // UPS B 控制
    if (new_state_B != upsB.relay_state) {
        if (now - upsB.last_relay_change > RELAY_DEBOUNCE_MS) {
            digitalWrite(upsB.relay_pin, new_state_B);
            upsB.relay_state = new_state_B;
            upsB.last_relay_change = now;
        }
    } else {
        upsB.last_relay_change = now;
    }
}

void updateSystemStatus() {
    // 检查 UPS A 是否正常
    bool a_ok = false;
    if (upsA.stc_online && upsA.state != UPS_ERROR) {
        if (upsA.stc.percent >= 0 && upsA.stc.percent <= 10000) {
            float socA = upsA.stc.percent / 100.0;
            if (socA >= 0.0 && socA <= 100.0) {
                a_ok = (socA >= LOW_BAT_THRESHOLD);
            } else {
                a_ok = false;   // 数据异常，视为不健康
            }
        }
    }

    // 检查 UPS B 是否正常
    bool b_ok = false;
    if (upsB.stc_online && upsB.state != UPS_ERROR) {
        if (upsB.stc.percent >= 0 && upsB.stc.percent <= 10000) {
            float socB = upsB.stc.percent / 100.0;
            if (socB >= 0.0 && socB <= 100.0) {
                b_ok = (socB >= LOW_BAT_THRESHOLD);
            } else {
                b_ok = false;   // 数据异常，视为不健康
            }
        }
    }

    system_normal = a_ok && b_ok;
}

void updateStatusLED() {
    if (system_normal) {
        digitalWrite(STATUS_LED, HIGH);
    } else {
        if (millis() - last_led_blink_ms >= 125) {
            last_led_blink_ms = millis();
            led_state = !led_state;
            digitalWrite(STATUS_LED, led_state);
        }
    }
}

void printCompactInfo() {
    auto printOne = [](UPS_Module &ups) {
        Serial.print(ups.name); Serial.print(",");
        Serial.print(ups.ina.voltage_V, 2); Serial.print(",");
        Serial.print(ups.ina.current_mA, 0); Serial.print(",");
        if (ups.stc_online) {
            Serial.print(ups.stc.percent / 100.0, 1);
        } else {
            float est = constrain((ups.ina.voltage_V / 2.0 - 3.0) / 1.2 * 100.0, 0, 100);
            Serial.print(est, 1);
        }
        Serial.print(",");
        switch (ups.state) {
            case UPS_CHARGING: Serial.print("CHG"); break;
            case UPS_DISCHARGING: Serial.print("DIS"); break;
            case UPS_FULL: Serial.print("FULL"); break;
            case UPS_IDLE: Serial.print("IDLE"); break;
            default: Serial.print("ERR");
        }
        Serial.print(",");
        Serial.print(ups.relay_state == RELAY_CHARGE ? "ON" : "OFF");
    };
    printOne(upsA);
    Serial.print(" ");
    printOne(upsB);
    Serial.println();
    // 在继电器状态后追加
    Serial.print(",");
    switch(currentChargingTarget) {
        case CHARGE_A: Serial.print("ACT_A"); break;
        case CHARGE_B: Serial.print("ACT_B"); break;
        default: Serial.print("IDLE");
    }
}

// ========== 主循环 ==========
void loop() {
    unsigned long now = millis();

    handleHeartbeat();

    if (now - last_ina_read_ms >= 200) {
        readINA219(upsA);
        readINA219(upsB);
        updateUPSState(upsA);
        updateUPSState(upsB);
        last_ina_read_ms = now;
    }

    handleSTCReading(upsA);
    handleSTCReading(upsB);

    if (now - last_control_ms >= 1000) {
        manageCharging(); 
        updateSystemStatus();
        printCompactInfo();
        last_control_ms = now;
    }

    updateStatusLED();
    delay(10);
}
