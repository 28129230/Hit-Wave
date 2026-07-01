/**
 * 双UPS智能监控与充放电控制 - 双色LED轮询版（异常定位增强）
 * 硬件：XIAO SAMD21 + 2块微雪 UPS Module 3S
 * 
 * 功能：
 * - 双路独立I2C总线
 * - INA219持续读取电压、电流、功率
 * - 继电器自动充电（电量<20%开启，>95%停止，防抖2秒）
 * - 通过串口接收树莓派心跳，控制充电许可（超时3秒自动禁止）
 * - 双色LED轮询显示：
 *   - 每4秒切换显示电池A（绿色）和电池B（黄色）
 *   - 正常：长呼吸（2秒）/ 充电：常亮 / 放电：短呼吸（1秒）
 *   - 异常（ERROR或SOC<20%）：红色快闪（4Hz）
 *   - 通过观察红色快闪出现的时段定位故障电池
 * 
 * 引脚分配：
 *   D0 -> 继电器 A
 *   D1 -> 继电器 B
 *   D2(SDA), D3(SCL) -> UPS A I2C
 *   D4(SDA), D5(SCL) -> UPS B I2C
 *   D6(TX), D7(RX) -> UART与树莓派通信 (Serial1)
 *   D8 -> 双色LED 绿色 (PWM)
 *   D10 -> 双色LED 红色 (PWM)
 *   USB串口用于调试 (Serial)
 */

#include <Wire.h>
#include <Adafruit_INA219.h>
#include "wiring_private.h"
#include <math.h>

// ========== 引脚定义 ==========
#define SDA_A       2
#define SCL_A       3
#define SDA_B       4
#define SCL_B       5
#define RELAY_A     0
#define RELAY_B     1
#define LED_GREEN   8
#define LED_RED     10

// ========== I2C 地址 ==========
#define INA219_ADDR  0x40

// ========== 充电阈值 ==========
#define LOW_BAT_THRESHOLD   20.0
#define FULL_BAT_THRESHOLD  95.0

// ========== 心跳超时 (毫秒) ==========
#define HEARTBEAT_TIMEOUT   3000

// ========== 继电器安全定义 ==========
#define RELAY_CHARGE    HIGH
#define RELAY_STOP      LOW

// ========== LED 参数 ==========
#define LED_SLOT_DURATION_MS  4000   // 每个电池显示时长 4秒
#define BREATH_PERIOD_LONG    2000   // 正常呼吸周期 2秒
#define BREATH_PERIOD_SHORT   1000   // 放电呼吸周期 1秒
#define FLASH_PERIOD_MS       125    // 急闪周期 125ms (4Hz)

// ========== 错误恢复参数 ==========
#define ERROR_RECOVERY_GOOD_READS  5    // 连续5次有效采样才恢复
#define ERROR_RECOVERY_MIN_INTERVAL_MS  2000  // 最小恢复间隔2秒

// ========== 状态机枚举 ==========
enum UPS_State {
    UPS_IDLE,
    UPS_DISCHARGING,
    UPS_CHARGING,
    UPS_FULL,
    UPS_ERROR
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
    int       relay_state;
    unsigned long last_relay_change;
    unsigned long full_start_ms;
    float lowSocOffset;      // 低电SOC滞回偏移量（%）
    float fullSocOffset;     // 满电SOC滞回偏移量（%）
    
    // 错误恢复相关
    uint8_t   error_good_read_count;
    unsigned long error_recovery_block_until;  // 阻止恢复的时间点
};

// ========== 全局对象 ==========
TwoWire Wire1(&sercom0, SDA_A, SCL_A);
TwoWire Wire2(&sercom1, SDA_B, SCL_B);

Adafruit_INA219 ina219_A(INA219_ADDR, &Wire1);
Adafruit_INA219 ina219_B(INA219_ADDR, &Wire2);

UPS_Module upsA = {"A", &Wire1, SDA_A, SCL_A, RELAY_A, UPS_IDLE, {}, RELAY_STOP, 0, 0, 2.0f, 2.0f, 0, 0};
UPS_Module upsB = {"B", &Wire2, SDA_B, SCL_B, RELAY_B, UPS_IDLE, {}, RELAY_STOP, 0, 0, 2.0f, 2.0f, 0, 0};

unsigned long last_ina_read_ms = 0;
unsigned long last_control_ms = 0;

// 心跳相关
unsigned long last_heartbeat = 0;
bool charging_allowed = false;

// ========== 充电仲裁器 ==========
enum ChargeTarget {
    CHARGE_NONE,
    CHARGE_A,
    CHARGE_B
};

ChargeTarget currentChargingTarget = CHARGE_NONE;
bool chargeSwitchPending = false;
unsigned long chargeSwitchDelayStart = 0;
bool chargeSwitchDelayActive = false;
bool pending_charge_A = false;
bool pending_charge_B = false;

// ========== 时间常量 ==========
#define CHARGE_SWITCH_DELAY_MS  1000
#define RELAY_DEBOUNCE_MS       2000

// ========== 错误日志去重标志 ==========
static bool a_error_logged = false;
static bool b_error_logged = false;
static bool a_low_logged = false;
static bool b_low_logged = false;
static unsigned long a_low_log_time = 0;
static unsigned long b_low_log_time = 0;
#define LOW_LOG_REPEAT_INTERVAL_MS  3600000  // 1小时允许重复上报一次

// ========== 函数声明 ==========
void initUPS();
void readINA219(UPS_Module &ups);
void updateUPSState(UPS_Module &ups);
void updateChargingDemand(UPS_Module &ups, bool &need_flag);
bool applyChargingHysteresis(float soc, bool currently_charging, float lowOffset, float fullOffset);
void manageCharging();
void updateLED();
void handleHeartbeat();
void printCompactInfo();
float estimateSOC(UPS_Module &ups);
void setLEDColor(int green, int red);
void setLEDBreath(int green_peak, int red_peak, unsigned long period_ms);
void setLEDFlash(int green, int red);
void logError(UPS_Module &ups, const char* reason, float value);
void resetErrorLogFlags(UPS_Module &ups);

// ========== 初始化 ==========
void setup() {
    Serial.begin(115200);
    Serial1.begin(115200);

    pinMode(RELAY_A, OUTPUT);
    pinMode(RELAY_B, OUTPUT);
    pinMode(LED_GREEN, OUTPUT);
    pinMode(LED_RED, OUTPUT);

    digitalWrite(RELAY_A, RELAY_STOP);
    digitalWrite(RELAY_B, RELAY_STOP);
    digitalWrite(LED_GREEN, LOW);
    digitalWrite(LED_RED, LOW);

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

    if (!ina219_A.begin()) {
        Serial.println("WARN: UPS A INA219 not found");
        upsA.state = UPS_ERROR;
        upsA.error_recovery_block_until = millis() + ERROR_RECOVERY_MIN_INTERVAL_MS;
        logError(upsA, "INA219_INIT_FAIL", 0);
        resetErrorLogFlags(upsA);
    }
    if (!ina219_B.begin()) {
        Serial.println("WARN: UPS B INA219 not found");
        upsB.state = UPS_ERROR;
        upsB.error_recovery_block_until = millis() + ERROR_RECOVERY_MIN_INTERVAL_MS;
        logError(upsB, "INA219_INIT_FAIL", 0);
        resetErrorLogFlags(upsB);
    }
}

// ========== LED 控制函数 ==========
void setLEDColor(int green, int red) {
    analogWrite(LED_GREEN, constrain(green, 0, 255));
    analogWrite(LED_RED, constrain(red, 0, 255));
}

void setLEDBreath(int green_peak, int red_peak, unsigned long period_ms) {
    if (period_ms < 50) period_ms = 50;
    
    unsigned long t = millis() % period_ms;
    float phase;
    if (t < period_ms / 2) {
        phase = (float)t / (period_ms / 2);
    } else {
        phase = 2.0 - (float)t / (period_ms / 2);
    }
    if (phase < 0) phase = 0;
    if (phase > 2) phase = 2;
    
    float brightness = sin(phase * PI);
    setLEDColor(green_peak * brightness, red_peak * brightness);
}

void setLEDFlash(int green, int red) {
    unsigned long t = millis() % (FLASH_PERIOD_MS * 2);
    bool on = (t < FLASH_PERIOD_MS);
    setLEDColor(on ? green : 0, on ? red : 0);
}

// ========== 错误日志函数 ==========
void logError(UPS_Module &ups, const char* reason, float value) {
    Serial1.print("ERR:");
    Serial1.print(ups.name);
    Serial1.print(",");
    Serial1.print(reason);
    if (value != 0) {
        Serial1.print(",V=");
        Serial1.print(value, 2);
    }
    Serial1.println();
    Serial.print("ERR:");
    Serial.print(ups.name);
    Serial.print(",");
    Serial.print(reason);
    if (value != 0) {
        Serial.print(",V=");
        Serial.print(value, 2);
    }
    Serial.println();
}

void resetErrorLogFlags(UPS_Module &ups) {
    if (&ups == &upsA) {
        a_error_logged = false;
        a_low_logged = false;
        a_low_log_time = 0;
    } else {
        b_error_logged = false;
        b_low_logged = false;
        b_low_log_time = 0;
    }
}

// ========== LED 状态更新 ==========
void updateLED() {
    unsigned long t = millis() % (LED_SLOT_DURATION_MS * 2);
    bool isA_slot = (t < LED_SLOT_DURATION_MS);

    UPS_Module &ups = isA_slot ? upsA : upsB;

    bool is_error = (ups.state == UPS_ERROR);
    float soc = 0.0;
    
    if (!is_error) {
        soc = estimateSOC(ups);
        if (soc < LOW_BAT_THRESHOLD) {
            is_error = true;
            // 低电量日志 - 带时间窗口去重
            unsigned long now = millis();
            bool should_log = false;
            if (&ups == &upsA) {
                if (!a_low_logged || (now - a_low_log_time > LOW_LOG_REPEAT_INTERVAL_MS)) {
                    should_log = true;
                    a_low_logged = true;
                    a_low_log_time = now;
                }
            } else {
                if (!b_low_logged || (now - b_low_log_time > LOW_LOG_REPEAT_INTERVAL_MS)) {
                    should_log = true;
                    b_low_logged = true;
                    b_low_log_time = now;
                }
            }
            if (should_log) {
                logError(ups, "LOW_SOC", soc);
            }
        } else {
            // 恢复正常时重置日志标志
            resetErrorLogFlags(ups);
        }
    }

    int green, red;
    if (isA_slot) {
        green = 128; red = 0;
    } else {
        green = 128; red = 128;
    }

    if (is_error) {
        setLEDFlash(0, 255);
    } else {
        bool charging = (ups.state == UPS_CHARGING);
        bool discharging = (ups.state == UPS_DISCHARGING);
        if (charging) {
            setLEDColor(green, red);
        } else if (discharging) {
            setLEDBreath(green, red, BREATH_PERIOD_SHORT);
        } else {
            setLEDBreath(green, red, BREATH_PERIOD_LONG);
        }
    }
}

// ========== INA219 读取 ==========
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

    bool is_valid = !isnan(ups.ina.voltage_V) && 
                    ups.ina.voltage_V >= 0.1 && 
                    ups.ina.voltage_V <= 20.0;

    if (!is_valid) {
        // 采样无效：重置好采样计数
        ups.error_good_read_count = 0;
        if (ups.state != UPS_ERROR) {
            ups.state = UPS_ERROR;
            // 进入ERROR时设置恢复阻止时间
            ups.error_recovery_block_until = millis() + ERROR_RECOVERY_MIN_INTERVAL_MS;
            logError(ups, "INVALID_VOLTAGE", ups.ina.voltage_V);
            // 进入ERROR时重置日志标志（防止残留标志影响后续上报）
            resetErrorLogFlags(ups);
        }
    } else {
        // 采样有效：检查是否需要从ERROR恢复
        if (ups.state == UPS_ERROR) {
            unsigned long now = millis();
            // 检查是否在恢复阻止期内
            if (now < ups.error_recovery_block_until) {
                // 仍处于阻止期，继续等待
                ups.error_good_read_count = 0;
                return;
            }
            
            // 增加好采样计数
            ups.error_good_read_count++;
            if (ups.error_good_read_count >= ERROR_RECOVERY_GOOD_READS) {
                // 连续N次有效，恢复
                ups.state = UPS_IDLE;
                ups.error_good_read_count = 0;
                ups.error_recovery_block_until = 0;
                resetErrorLogFlags(ups);
                Serial1.print("RECOVER:");
                Serial1.println(ups.name);
                Serial.print("RECOVER:");
                Serial.println(ups.name);
            }
        } else {
            // 正常状态，重置好采样计数
            ups.error_good_read_count = 0;
        }
    }
}

// ========== 电压估算SOC ==========
float estimateSOC(UPS_Module &ups) {
    // ERROR状态下不计算SOC，直接返回0
    if (ups.state == UPS_ERROR) return 0.0f;
    
    const float CELLS_IN_SERIES = 2.0;
    const float VOLTAGE_FULL = 4.2;
    const float VOLTAGE_EMPTY = 3.0;
    const float VOLTAGE_RANGE = VOLTAGE_FULL - VOLTAGE_EMPTY;

    float v = ups.ina.voltage_V;
    if (v < 0.1 || v > 20.0 || isnan(v)) return 0.0f;
    float v_per_cell = v / CELLS_IN_SERIES;
    return constrain((v_per_cell - VOLTAGE_EMPTY) / VOLTAGE_RANGE * 100.0, 0.0, 100.0);
}

// ========== 状态机更新 ==========
void updateUPSState(UPS_Module &ups) {
    if (ups.state == UPS_ERROR) return;

    float curr = ups.ina.current_mA;
    float volt = ups.ina.voltage_V;
    const float CHARGE_CURRENT_THRESHOLD = 100.0;
    const float DISCHARGE_CURRENT_THRESHOLD = -50.0;
    const float FULL_VOLTAGE = 8.4;
    const float IDLE_CURRENT_THRESHOLD = 50.0;
    const unsigned long FULL_STABLE_TIME_MS = 3000;
    const float FULL_EXIT_VOLTAGE_DROP = 0.3;
    const float FULL_EXIT_CURRENT_THRESH = 150.0;

    if (ups.state == UPS_FULL) {
        bool should_exit = false;
        if (volt < FULL_VOLTAGE - FULL_EXIT_VOLTAGE_DROP) should_exit = true;
        if (curr > FULL_EXIT_CURRENT_THRESH || curr < -FULL_EXIT_CURRENT_THRESH) should_exit = true;
        if (should_exit) {
            ups.state = UPS_IDLE;
            ups.full_start_ms = 0;
        }
        return;
    }

    if (curr > CHARGE_CURRENT_THRESHOLD) {
        ups.state = UPS_CHARGING;
        ups.full_start_ms = 0;
    } else if (curr < DISCHARGE_CURRENT_THRESHOLD) {
        ups.state = UPS_DISCHARGING;
        ups.full_start_ms = 0;
    } else if (volt >= FULL_VOLTAGE - 0.2 && fabs(curr) < IDLE_CURRENT_THRESHOLD) {
        if (ups.full_start_ms == 0) {
            ups.full_start_ms = millis();
        } else if (millis() - ups.full_start_ms >= FULL_STABLE_TIME_MS) {
            ups.state = UPS_FULL;
        }
    } else {
        ups.state = UPS_IDLE;
        ups.full_start_ms = 0;
    }
}

// ========== 充电需求计算 ==========
void updateChargingDemand(UPS_Module &ups, bool &need_flag) {
    // ERROR状态下强制禁止充电需求
    if (ups.state == UPS_ERROR) {
        need_flag = false;
        return;
    }

    float soc = estimateSOC(ups);
    if (!charging_allowed) {
        need_flag = false;
        return;
    }
    need_flag = applyChargingHysteresis(
        soc,
        (ups.relay_state == RELAY_CHARGE),
        ups.lowSocOffset,
        ups.fullSocOffset
    );
}

bool applyChargingHysteresis(float soc, bool currently_charging, float lowOffset, float fullOffset) {
    if (currently_charging) {
        if (soc >= FULL_BAT_THRESHOLD + fullOffset) return false;
        return true;
    } else {
        if (soc <= LOW_BAT_THRESHOLD - lowOffset) return true;
        return false;
    }
}

// ========== 充电仲裁 ==========
void manageCharging() {
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

    updateChargingDemand(upsA, pending_charge_A);
    updateChargingDemand(upsB, pending_charge_B);

    bool currentNeedsCharge = false;
    if (currentChargingTarget == CHARGE_A)
        currentNeedsCharge = pending_charge_A;
    else if (currentChargingTarget == CHARGE_B)
        currentNeedsCharge = pending_charge_B;

    if (!currentNeedsCharge && currentChargingTarget != CHARGE_NONE) {
        if (!chargeSwitchPending) {
            chargeSwitchPending = true;
            chargeSwitchDelayActive = false;
        }
    } else if (currentNeedsCharge) {
        chargeSwitchPending = false;
        chargeSwitchDelayActive = false;
    }

    if (chargeSwitchPending && !chargeSwitchDelayActive) {
        chargeSwitchDelayStart = millis();
        chargeSwitchDelayActive = true;
    }

    if (chargeSwitchPending && chargeSwitchDelayActive) {
        if (millis() - chargeSwitchDelayStart >= CHARGE_SWITCH_DELAY_MS) {
            currentChargingTarget = CHARGE_NONE;
            chargeSwitchPending = false;
            chargeSwitchDelayActive = false;
        }
    }

    if (currentChargingTarget == CHARGE_NONE && (pending_charge_A || pending_charge_B)) {
        if (pending_charge_A && pending_charge_B) {
            float socA = estimateSOC(upsA);
            float socB = estimateSOC(upsB);
            currentChargingTarget = (socA < socB) ? CHARGE_A : CHARGE_B;
        } else if (pending_charge_A) {
            currentChargingTarget = CHARGE_A;
        } else if (pending_charge_B) {
            currentChargingTarget = CHARGE_B;
        }
        chargeSwitchPending = false;
        chargeSwitchDelayActive = false;
    }

    int new_state_A = (currentChargingTarget == CHARGE_A) ? RELAY_CHARGE : RELAY_STOP;
    int new_state_B = (currentChargingTarget == CHARGE_B) ? RELAY_CHARGE : RELAY_STOP;

    unsigned long now = millis();

    if (new_state_A != upsA.relay_state) {
        if (now - upsA.last_relay_change > RELAY_DEBOUNCE_MS) {
            digitalWrite(RELAY_A, new_state_A);
            upsA.relay_state = new_state_A;
            upsA.last_relay_change = now;
        }
    } else {
        upsA.last_relay_change = now;
    }

    if (new_state_B != upsB.relay_state) {
        if (now - upsB.last_relay_change > RELAY_DEBOUNCE_MS) {
            digitalWrite(RELAY_B, new_state_B);
            upsB.relay_state = new_state_B;
            upsB.last_relay_change = now;
        }
    } else {
        upsB.last_relay_change = now;
    }
}

// ========== 心跳处理 ==========
void handleHeartbeat() {
    while (Serial1.available()) {
        String line = Serial1.readStringUntil('\n');
        line.trim();
        if (line.startsWith("CHG:")) {
            int val = line.substring(4).toInt();
            charging_allowed = (val == 1);
            last_heartbeat = millis();
            if (charging_allowed) {
                Serial1.println("HB:ALLOW");
            } else {
                Serial1.println("HB:DENY");
            }
        }
    }
    if (millis() - last_heartbeat > HEARTBEAT_TIMEOUT) {
        if (charging_allowed) {
            charging_allowed = false;
            Serial1.println("HB:TIMEOUT");
        }
    }
}

// ========== 串口输出 ==========
void printCompactInfo() {
    auto printOne = [](UPS_Module &ups) {
        float soc = estimateSOC(ups);
        Serial1.print(ups.name); Serial1.print(",");
        Serial1.print(ups.ina.voltage_V, 2); Serial1.print(",");
        Serial1.print(ups.ina.current_mA, 0); Serial1.print(",");
        Serial1.print(soc, 1); Serial1.print(",");
        switch (ups.state) {
            case UPS_CHARGING: Serial1.print("CHG"); break;
            case UPS_DISCHARGING: Serial1.print("DIS"); break;
            case UPS_FULL: Serial1.print("FULL"); break;
            case UPS_IDLE: Serial1.print("IDLE"); break;
            default: Serial1.print("ERR");
        }
        Serial1.print(",");
        Serial1.print(ups.relay_state == RELAY_CHARGE ? "ON" : "OFF");
    };
    printOne(upsA);
    Serial1.print(" ");
    printOne(upsB);
    Serial1.print(",");
    switch(currentChargingTarget) {
        case CHARGE_A: Serial1.print("ACT_A"); break;
        case CHARGE_B: Serial1.print("ACT_B"); break;
        default: Serial1.print("IDLE");
    }
    Serial1.println();
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

    if (now - last_control_ms >= 1000) {
        manageCharging();
        printCompactInfo();
        last_control_ms = now;
    }

    updateLED();
    delay(10);
}
