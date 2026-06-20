/**
 * 双UPS智能监控与充放电控制 - INA219 Only 版
 * 硬件：XIAO SAMD21 + 2块微雪 UPS Module 3S
 * 
 * 功能：
 * - 双路独立I2C总线（解决地址冲突）
 * - INA219持续读取电压、电流、功率
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
 *   D2 -> 状态LED
 *   USB串口与树莓派通信
 */

#include <Wire.h>
#include <Adafruit_INA219.h>
#include "wiring_private.h"
#include <math.h>  // 用于 isnan()

// ========== 引脚定义 ==========
#define SDA_A       4
#define SCL_A       5
#define SDA_B       6
#define SCL_B       7
#define RELAY_A     8
#define RELAY_B     9
#define STATUS_LED  2

// ========== I2C 地址 ==========
#define INA219_ADDR  0x40

// ========== 充电阈值 ==========
#define LOW_BAT_THRESHOLD   20.0   // 低于20%启动充电
#define FULL_BAT_THRESHOLD  95.0   // 高于95%停止充电

// ========== 心跳超时 (毫秒) ==========
#define HEARTBEAT_TIMEOUT   3000

// ========== 继电器安全定义 ==========
#define RELAY_CHARGE    HIGH   // HIGH = 吸合，允许充电
#define RELAY_STOP      LOW    // LOW = 断开，停止充电

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
    int       relay_state;          // RELAY_CHARGE 或 RELAY_STOP
    unsigned long last_relay_change;
    
    // 满电稳定计时器
    unsigned long full_start_ms;    // 用于FULL状态稳定计时
    
    // 滞回参数
    float lowHysteresis;     // 低电滞回（%）
    float fullHysteresis;    // 满电滞回（%）
};

// ========== 全局对象 ==========
TwoWire Wire1(&sercom0, SDA_A, SCL_A);
TwoWire Wire2(&sercom1, SDA_B, SCL_B);
Adafruit_INA219 ina219_A(INA219_ADDR, &Wire1);
Adafruit_INA219 ina219_B(INA219_ADDR, &Wire2);

UPS_Module upsA = {"A", &Wire1, SDA_A, SCL_A, RELAY_A, UPS_IDLE, {}, RELAY_STOP, 0, 0, 2.0f, 2.0f};
UPS_Module upsB = {"B", &Wire2, SDA_B, SCL_B, RELAY_B, UPS_IDLE, {}, RELAY_STOP, 0, 0, 2.0f, 2.0f};

unsigned long last_ina_read_ms = 0;
unsigned long last_control_ms = 0;
unsigned long last_led_blink_ms = 0;
bool led_state = false;
bool system_normal = true;

// 心跳相关
unsigned long last_heartbeat = 0;
bool charging_allowed = false;   // 默认禁止充电

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

// ========== 函数声明 ==========
void initUPS();
void readINA219(UPS_Module &ups);
void updateUPSState(UPS_Module &ups);
void updateChargingDemand(UPS_Module &ups, bool &need_flag);
bool applyChargingHysteresis(float soc, bool currently_charging, float lowHyst, float fullHyst);
void manageCharging();
void updateSystemStatus();
void updateStatusLED();
void handleHeartbeat();
void printCompactInfo();
float estimateSOC(UPS_Module &ups);  // 根据电压估算SOC

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
}

// ========== 电压估算SOC ==========
float estimateSOC(UPS_Module &ups) {
    const float CELLS_IN_SERIES = 2.0;
    const float VOLTAGE_FULL = 4.2;
    const float VOLTAGE_EMPTY = 3.0;
    const float VOLTAGE_RANGE = VOLTAGE_FULL - VOLTAGE_EMPTY;

    float v = ups.ina.voltage_V;
    // 电压异常检测（包括NaN检测）
    if (v < 0.1 || v > 20.0 || isnan(v)) {
        return 50.0;   // 异常时返回保守值
    }
    float v_per_cell = v / CELLS_IN_SERIES;
    float soc = constrain((v_per_cell - VOLTAGE_EMPTY) / VOLTAGE_RANGE * 100.0, 0.0, 100.0);
    return soc;
}

// ========== 状态机更新 ==========
void updateUPSState(UPS_Module &ups) {
    float curr = ups.ina.current_mA;
    float volt = ups.ina.voltage_V;
    UPS_State old_state = ups.state;

    const float CHARGE_CURRENT_THRESHOLD = 100.0;
    const float DISCHARGE_CURRENT_THRESHOLD = -50.0;
    const float FULL_VOLTAGE = 8.4;
    const float IDLE_CURRENT_THRESHOLD = 50.0;
    const unsigned long FULL_STABLE_TIME_MS = 3000;
    const float FULL_EXIT_VOLTAGE_DROP = 0.3;
    const float FULL_EXIT_CURRENT_THRESH = 150.0;

    // FULL 状态特殊处理
    if (ups.state == UPS_FULL) {
        bool should_exit = false;
        
        // 条件1：电压明显下降
        if (volt < FULL_VOLTAGE - FULL_EXIT_VOLTAGE_DROP) {
            should_exit = true;
        }
        
        // 条件2：出现显著充放电电流
        if (curr > FULL_EXIT_CURRENT_THRESH || curr < -FULL_EXIT_CURRENT_THRESH) {
            should_exit = true;
        }
        
        if (should_exit) {
            ups.state = UPS_IDLE;
            ups.full_start_ms = 0;
        }
        return;
    }

    // 正常状态判断
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
        // 未达到稳定时间时，保持当前状态
    } else {
        ups.state = UPS_IDLE;
        ups.full_start_ms = 0;
    }
}

// ========== 充电需求计算（带滞回） ==========
void updateChargingDemand(UPS_Module &ups, bool &need_flag) {
    float soc = estimateSOC(ups);
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

// ========== 滞回控制 ==========
bool applyChargingHysteresis(float soc, bool currently_charging, float lowHyst, float fullHyst) {
    if (currently_charging) {
        if (soc >= FULL_BAT_THRESHOLD + fullHyst) return false;
        return true;
    } else {
        if (soc <= LOW_BAT_THRESHOLD - lowHyst) return true;
        return false;
    }
}

// ========== 充电仲裁 ==========
void manageCharging() {
    // 第一道防线：心跳丢失立即停充
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

    // 更新需求
    updateChargingDemand(upsA, pending_charge_A);
    updateChargingDemand(upsB, pending_charge_B);

    // 当前目标是否仍需充电
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

    // 选择新目标：谁更空先给谁
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

    // 设置继电器（防抖）
    int new_state_A = (currentChargingTarget == CHARGE_A) ? RELAY_CHARGE : RELAY_STOP;
    int new_state_B = (currentChargingTarget == CHARGE_B) ? RELAY_CHARGE : RELAY_STOP;

    unsigned long now = millis();

    if (new_state_A != upsA.relay_state) {
        if (now - upsA.last_relay_change > RELAY_DEBOUNCE_MS) {
            digitalWrite(upsA.relay_pin, new_state_A);
            upsA.relay_state = new_state_A;
            upsA.last_relay_change = now;
        }
    } else {
        upsA.last_relay_change = now;
    }

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

// ========== 系统状态更新 ==========
void updateSystemStatus() {
    float socA = estimateSOC(upsA);
    float socB = estimateSOC(upsB);
    bool a_ok = (socA >= LOW_BAT_THRESHOLD);
    bool b_ok = (socB >= LOW_BAT_THRESHOLD);
    system_normal = a_ok && b_ok;
}

// ========== LED指示 ==========
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

// ========== 心跳处理 ==========
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

// ========== 串口输出 ==========
void printCompactInfo() {
    auto printOne = [](UPS_Module &ups) {
        float soc = estimateSOC(ups);
        Serial.print(ups.name); Serial.print(",");
        Serial.print(ups.ina.voltage_V, 2); Serial.print(",");
        Serial.print(ups.ina.current_mA, 0); Serial.print(",");
        Serial.print(soc, 1); Serial.print(",");
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
    Serial.print(",");
    switch(currentChargingTarget) {
        case CHARGE_A: Serial.print("ACT_A"); break;
        case CHARGE_B: Serial.print("ACT_B"); break;
        default: Serial.print("IDLE");
    }
    Serial.println();
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
        updateSystemStatus();
        printCompactInfo();
        last_control_ms = now;
    }

    updateStatusLED();
    delay(10);
}
