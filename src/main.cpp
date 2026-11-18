#include <M5Cardputer.h>
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>

#include "venus_protocol.h"

namespace {
constexpr uint32_t pollMs = 2000;
constexpr uint32_t settingsPollMs = 10000;
constexpr uint32_t timeoutMs = 3500;
constexpr uint32_t reconnectMs = 2500;
constexpr int screenWidth = 240;
constexpr int screenHeight = 135;
constexpr uint16_t background = 0x0842;
constexpr uint16_t divider = 0x31A8;
constexpr uint16_t muted = 0x9D36;
constexpr uint16_t accent = 0x67DD;
constexpr uint16_t backupColor = 0xFD67;
M5Canvas dashboard(&M5Cardputer.Display);

struct Notification {
    uint16_t size;
    uint8_t bytes[venus::maxFrameSize];
};

QueueHandle_t notifications = nullptr;
BLEClient* client = nullptr;
BLERemoteCharacteristic* command = nullptr;
venus::FrameReceiver frame;
uint32_t nextAction = 0;
uint32_t deadline = 0;
uint32_t nextSettings = 0;
uint8_t pendingCommand = 0;
unsigned page = 0;
constexpr unsigned pageCount = 3;
constexpr unsigned settingsPage = 2;
venus::Runtime currentReading{};
venus::UpsSettings upsSettings{};
bool hasReading = false;
bool hasUpsSettings = false;
bool settingsAttempted = false;
bool paused = false;
bool capture = true;
enum class LedToggle { Idle, ReadBefore, Confirm };
LedToggle ledToggle = LedToggle::Idle;
bool ledTarget = false;
uint32_t ledConfirmUntil = 0;
const char* ledMessage = nullptr;
uint32_t ledMessageUntil = 0;

bool due(uint32_t time) {
    return static_cast<int32_t>(millis() - time) >= 0;
}

void showLedMessage(const char* message) {
    ledMessage = message;
    ledMessageUntil = millis() + 2500;
    Serial.println(message);
}

lgfx::LGFXBase& beginDashboard() {
    auto& display = M5Cardputer.Display;
    lgfx::LGFXBase& screen = dashboard.width() == screenWidth
        ? static_cast<lgfx::LGFXBase&>(dashboard)
        : static_cast<lgfx::LGFXBase&>(display);
    screen.fillScreen(background);
    screen.setFont(&fonts::Font0);
    screen.setTextWrap(false, false);
    screen.setTextSize(1);
    screen.setTextDatum(top_left);
    screen.setTextColor(accent);
    screen.drawString("VENUS E 3.0", 10, 9);
    if (ledMessage && !due(ledMessageUntil)) {
        screen.setTextDatum(top_center);
        screen.drawString(ledMessage, 150, 9);
    }
    char pageLabel[8];
    snprintf(pageLabel, sizeof(pageLabel), "%u/%u", page + 1, pageCount);
    screen.setTextDatum(top_right);
    screen.drawString(pageLabel, 230, 9);
    screen.setTextDatum(top_left);
    screen.drawFastHLine(10, 23, 220, divider);
    screen.drawFastHLine(10, 110, 220, divider);
    screen.setTextColor(muted);
    screen.drawString(paused ? "Q  Resume" : "Q  Pause", 10, 119);
    screen.drawString("< > Pages", 90, 119);
    return screen;
}

void endDashboard() {
    if (dashboard.width() == screenWidth) {
        dashboard.pushSprite(0, 0);
    }
}

void draw(const char* title, const char* detail = nullptr) {
    auto& screen = beginDashboard();
    screen.setTextSize(2);
    if (screen.textWidth(title) > screenWidth - 20) {
        screen.setTextSize(1);
    }
    screen.setTextColor(TFT_WHITE);
    screen.drawString(title, 10, 35);
    if (detail) {
        screen.setTextSize(1);
        screen.setTextColor(muted);
        screen.drawString(detail, 10, 63);
    }
    endDashboard();
}

const char* stateName(uint8_t code) {
    switch (code) {
        case 0: return "Sleep";
        case 1: return "Standby";
        case 2: return "Charging";
        case 3: return "Discharging";
        case 4: return "Battery backup";
        case 5: return "Updating";
        case 6: return "Mains bypass";
        default: return "Unknown state";
    }
}

void showReading(const venus::Runtime& reading) {
    auto& screen = beginDashboard();
    screen.setTextSize(2);
    screen.setTextColor(reading.inverterState == 4 ? backupColor : TFT_WHITE);
    screen.drawString(stateName(reading.inverterState), 10, 35);
    screen.drawFastHLine(10, 59, 220, divider);
    screen.drawFastVLine(80, 68, 34, divider);
    screen.drawFastVLine(160, 68, 34, divider);

    char soc[8];
    char outlet[12];
    snprintf(soc, sizeof(soc), "%u%%", static_cast<unsigned>(reading.soc));
    snprintf(outlet, sizeof(outlet), "%d", reading.outletWatts);

    screen.setTextDatum(top_center);
    screen.setTextColor(muted);
    screen.setTextSize(1);
    screen.drawString("BATTERY", 40, 68);
    screen.drawString("OUTLET (W)", 120, 68);
    screen.drawString("ACTIVITY", 200, 68);
    screen.setTextColor(TFT_WHITE);
    screen.setTextSize(2);
    screen.drawString(soc, 40, 83);
    screen.drawString(outlet, 120, 83);

    // Use the inverter state so a small negative pack reading in bypass
    // does not appear as a discharge.
    const uint8_t state = reading.inverterState;
    const char* activity = "Unknown";
    if (state == 2 || state == 3 || state == 4) {
        const bool charging = state == 2;
        const uint16_t color = charging ? accent : backupColor;
        const int tip = charging ? 80 : 92;
        const int wing = charging ? 85 : 87;
        screen.drawFastVLine(200, 80, 13, color);
        screen.drawLine(200, tip, 195, wing, color);
        screen.drawLine(200, tip, 205, wing, color);
        activity = charging ? "Charging" : "Discharging";
    } else if (state == 0 || state == 1 || state == 6) {
        screen.drawFastHLine(195, 84, 11, muted);
        screen.drawFastHLine(195, 88, 11, muted);
        activity = "Idle";
    }
    screen.setTextSize(1);
    screen.setTextColor(TFT_WHITE);
    screen.drawString(activity, 200, 97);

    char stateCode[16];
    snprintf(stateCode, sizeof(stateCode), "State %u",
             static_cast<unsigned>(reading.inverterState));
    screen.setTextColor(muted);
    screen.setTextDatum(top_right);
    screen.drawString(stateCode, 230, 119);
    endDashboard();
}

void showPower(const venus::Runtime& reading) {
    auto& screen = beginDashboard();
    screen.setTextColor(TFT_WHITE);
    screen.setTextSize(2);
    screen.drawString(stateName(reading.inverterState), 10, 35);
    screen.drawFastHLine(10, 59, 220, divider);
    screen.drawFastVLine(120, 68, 34, divider);

    char grid[12] = "--";
    char battery[12] = "--";
    if (reading.hasPowerReadings) {
        snprintf(grid, sizeof(grid), "%+d", reading.gridWatts);
        snprintf(battery, sizeof(battery), "%+d", reading.batteryWatts);
    }
    screen.setTextDatum(top_center);
    screen.setTextColor(muted);
    screen.setTextSize(1);
    screen.drawString("GRID (W)", 60, 68);
    screen.drawString("BATTERY (W)", 180, 68);
    screen.setTextColor(TFT_WHITE);
    screen.setTextSize(2);
    screen.drawString(grid, 60, 83);
    screen.drawString(battery, 180, 83);
    endDashboard();
}

const char* chargeMode() {
    if (!currentReading.hasWorkMode) {
        return "--";
    }
    switch (currentReading.workMode) {
        case 0: return "Self-consumption";
        case 1:
            if (!hasUpsSettings) {
                return "Manual / UPS ?";
            }
            return upsSettings.enabled ? "UPS" : "Manual";
        case 5: return "AI";
        default: return "Unknown";
    }
}

void settingRow(lgfx::LGFXBase& screen, int y, const char* label, const char* value) {
    screen.setTextDatum(top_left);
    screen.setTextColor(muted);
    screen.drawString(label, 10, y);
    screen.setTextDatum(top_right);
    screen.setTextColor(TFT_WHITE);
    screen.drawString(value, 230, y);
}

void showSettings() {
    auto& screen = beginDashboard();
    screen.setTextSize(2);
    screen.setTextColor(TFT_WHITE);
    screen.drawString("Settings", 10, 32);
    screen.setTextSize(1);

    char speed[16] = "--";
    if (hasUpsSettings) {
        snprintf(speed, sizeof(speed), "%ld W", static_cast<long>(upsSettings.chargeWatts));
    }
    const char* backup = currentReading.hasBackupSetting
        ? (currentReading.backupEnabled ? "Enabled" : "Disabled") : "--";
    settingRow(screen, 59, "Charge mode", chargeMode());
    settingRow(screen, 76, "UPS charge speed", speed);
    settingRow(screen, 93, "Backup power", backup);
    endDashboard();
}

void showPage() {
    if (!hasReading) {
        draw(paused ? "Paused" : "Waiting for Venus");
    } else if (page == settingsPage) {
        showSettings();
    } else if (page == 1) {
        showPower(currentReading);
    } else {
        showReading(currentReading);
    }
}

void onNotification(BLERemoteCharacteristic*, uint8_t* bytes, size_t count, bool) {
    if (count == 0 || count > venus::maxFrameSize) {
        return;
    }

    Notification message{};
    message.size = static_cast<uint16_t>(count);
    memcpy(message.bytes, bytes, count);
    xQueueSend(notifications, &message, 0);
}

void disconnect() {
    if (ledToggle != LedToggle::Idle) {
        showLedMessage("LED unconfirmed");
        ledToggle = LedToggle::Idle;
    }
    command = nullptr;
    pendingCommand = 0;
    hasReading = false;
    hasUpsSettings = false;
    settingsAttempted = false;
    frame.reset();
    if (client->isConnected()) {
        client->disconnect();
    }
    xQueueReset(notifications);
}

bool connectVenus() {
    draw("Scanning for Venus");
    BLEScan* scan = BLEDevice::getScan();
    scan->setActiveScan(true);
    BLEScanResults results = scan->start(2, false);
    int candidate = -1;
    for (int i = 0; i < results.getCount(); ++i) {
        auto device = results.getDevice(i);
        const std::string name = device.getName();
        if (name.rfind("MST_VNSE3_", 0) != 0) {
            continue;
        }
        if (candidate >= 0) {
            scan->clearResults();
            draw("Multiple Venus units", "Leave one in range");
            return false;
        }
        candidate = i;
    }

    if (candidate < 0) {
        scan->clearResults();
        draw("Venus not found", "Move closer");
        return false;
    }

    auto device = results.getDevice(candidate);
    scan->clearResults();
    Serial.printf("Connecting to %s\n", device.getName().c_str());
    draw("Connecting");
    if (!client->connect(&device)) {
        draw("Connection failed");
        return false;
    }

    client->setMTU(185);
    auto* service = client->getService(BLEUUID(static_cast<uint16_t>(0xff00)));
    if (service) {
        command = service->getCharacteristic(BLEUUID(static_cast<uint16_t>(0xff01)));
        auto* reply = service->getCharacteristic(BLEUUID(static_cast<uint16_t>(0xff02)));
        if (command && reply && reply->canNotify() &&
            (command->canWrite() || command->canWriteNoResponse())) {
            reply->registerForNotify(onNotification);
            capture = true;
            return true;
        }
    }

    disconnect();
    draw("Venus service missing");
    return false;
}

void handleLedReadback(const venus::Runtime& reading) {
    if (ledToggle == LedToggle::ReadBefore) {
        if (!reading.hasLedSetting) {
            showLedMessage("LED unavailable");
            ledToggle = LedToggle::Idle;
            return;
        }
        ledTarget = !reading.ledEnabled;
        uint8_t request[] = {venus::magic, 6, venus::family, venus::ledCommand,
                             static_cast<uint8_t>(ledTarget), 0};
        request[5] = venus::checksum(request, 5);
        ledToggle = LedToggle::Confirm;
        ledConfirmUntil = millis() + timeoutMs;
        showLedMessage("Checking LED...");
        command->writeValue(request, sizeof(request), command->canWrite());
        nextAction = millis() + 400;
    } else if (ledToggle == LedToggle::Confirm) {
        if (reading.hasLedSetting && reading.ledEnabled == ledTarget) {
            showLedMessage(ledTarget ? "LED on" : "LED off");
            ledToggle = LedToggle::Idle;
        } else if (due(ledConfirmUntil)) {
            showLedMessage("LED unconfirmed");
            ledToggle = LedToggle::Idle;
        } else {
            nextAction = millis() + 400;
        }
    }
}

void handleFrame() {
    if (pendingCommand == venus::settingsCommand) {
        venus::UpsSettings settings{};
        if (venus::decodeUpsSettings(frame.data(), frame.size(), settings)) {
            upsSettings = settings;
            hasUpsSettings = true;
            pendingCommand = 0;
            Serial.printf("UPS slot: %s, %ld W; mode %u, backup %s\n",
                          settings.enabled ? "enabled" : "disabled",
                          static_cast<long>(settings.chargeWatts),
                          static_cast<unsigned>(currentReading.workMode),
                          currentReading.hasBackupSetting
                              ? (currentReading.backupEnabled ? "enabled" : "disabled")
                              : "unavailable");
            if (page == settingsPage) {
                showPage();
            }
        }
        frame.reset();
        return;
    }

    venus::Runtime reading{};
    const bool valid = venus::decodeRuntime(frame.data(), frame.size(), reading);
    static uint8_t lastState = 255;
    if (capture || !valid || lastState != reading.inverterState) {
        Serial.printf("BLE frame (%u bytes):", static_cast<unsigned>(frame.size()));
        for (size_t i = 0; i < frame.size(); ++i) {
            Serial.printf(" %02X", static_cast<unsigned>(frame.data()[i]));
        }
        Serial.println();
    }

    if (valid) {
        if (capture || lastState != reading.inverterState) {
            const auto& screen = M5Cardputer.Display;
            Serial.printf("Dashboard: %d x %d\n", screen.width(), screen.height());
            Serial.printf("State %u, SOC %u%%, outlet %d W\n",
                          static_cast<unsigned>(reading.inverterState),
                          static_cast<unsigned>(reading.soc), reading.outletWatts);
            if (reading.hasPowerReadings) {
                Serial.printf("Grid %d W, battery %d W\n", reading.gridWatts, reading.batteryWatts);
            }
            if (reading.hasLedSetting) {
                Serial.printf("Panel LED: %s\n", reading.ledEnabled ? "on" : "off");
            }
        }
        lastState = reading.inverterState;
        capture = false;
        pendingCommand = 0;
        nextAction = millis() + pollMs;
        currentReading = reading;
        hasReading = true;
        handleLedReadback(reading);
        showPage();
    }
    frame.reset();
}

void requestRuntimeOrSettings(uint8_t id) {
    uint8_t request[] = {venus::magic, 5, venus::family, id, 0, 0};
    size_t size = 5;
    if (id == venus::settingsCommand) {
        size = 6;
        request[1] = size;
        request[4] = venus::readSchedules;
        settingsAttempted = true;
        nextSettings = millis() + settingsPollMs;
    }
    request[size - 1] = venus::checksum(request, size - 1);
    pendingCommand = id;
    deadline = millis() + timeoutMs;
    command->writeValue(request, size, command->canWrite());
}
} // namespace

void setup() {
    Serial.begin(115200);
    M5Cardputer.begin(M5.config(), true);
    // M5GFX already applies the Cardputer panel's physical rotation offset.
    auto& screen = M5Cardputer.Display;
    screen.setRotation(0);
    Serial.printf("Display: %d x %d, rotation %u\n", screen.width(), screen.height(),
                  static_cast<unsigned>(screen.getRotation()));
    dashboard.setColorDepth(8);
    dashboard.createSprite(screenWidth, screenHeight);
    notifications = xQueueCreate(8, sizeof(Notification));
    if (!notifications) {
        draw("BLE queue failed");
        return;
    }
    BLEDevice::init("");
    BLEDevice::setMTU(185);
    client = BLEDevice::createClient();
    nextAction = millis();
}

void loop() {
    if (!client) {
        delay(20);
        return;
    }

    M5Cardputer.update();
    auto& keyboard = M5Cardputer.Keyboard;
    const bool pauseKey = keyboard.isKeyPressed('q');
    const bool leftKey = keyboard.isKeyPressed(',');
    const bool rightKey = keyboard.isKeyPressed('/');
    const bool ledKey = keyboard.isKeyPressed('l');
    static bool pauseHeld = false;
    static bool leftHeld = false;
    static bool rightHeld = false;
    static bool ledHeld = false;
    if (pauseKey && !pauseHeld) {
        paused = !paused;
        disconnect();
        draw(paused ? "Paused" : "Resuming");
        nextAction = millis();
    } else if ((leftKey && !leftHeld) || (rightKey && !rightHeld)) {
        page = (page + (leftKey && !leftHeld ? pageCount - 1 : 1)) % pageCount;
        if (page == settingsPage) {
            nextSettings = millis();
        }
        showPage();
    } else if (ledKey && !ledHeld && ledToggle == LedToggle::Idle) {
        if (!paused && command && client->isConnected()) {
            ledToggle = LedToggle::ReadBefore;
            nextAction = millis();
        } else {
            showLedMessage("LED unavailable");
            showPage();
        }
    }
    pauseHeld = pauseKey;
    leftHeld = leftKey;
    rightHeld = rightKey;
    ledHeld = ledKey;
    if (ledMessage && due(ledMessageUntil)) {
        ledMessage = nullptr;
        if (hasReading || paused) {
            showPage();
        }
    }

    if (!paused && !client->isConnected() && due(nextAction)) {
        disconnect();
        nextAction = millis() + (connectVenus() ? 0 : reconnectMs);
    }

    if (!paused && command && client->isConnected()) {
        Notification message;
        while (xQueueReceive(notifications, &message, 0) == pdTRUE) {
            for (size_t i = 0; i < message.size && pendingCommand; ++i) {
                if (frame.push(message.bytes[i])) {
                    handleFrame();
                }
            }
        }

        if (pendingCommand && due(deadline)) {
            Serial.printf("BLE reply 0x%02X timed out (%u partial bytes)\n", pendingCommand,
                          static_cast<unsigned>(frame.size()));
            if (pendingCommand == venus::settingsCommand) {
                hasUpsSettings = false;
                pendingCommand = 0;
                frame.reset();
                if (page == settingsPage) {
                    showPage();
                }
            } else {
                disconnect();
                draw("No valid reply", "Reconnecting");
                nextAction = millis() + reconnectMs;
            }
        } else if (!pendingCommand && due(nextAction)) {
            requestRuntimeOrSettings(venus::runtimeCommand);
        } else if (!pendingCommand && hasReading &&
                   (!settingsAttempted || (page == settingsPage && due(nextSettings)))) {
            requestRuntimeOrSettings(venus::settingsCommand);
        }
    } else if (command) {
        disconnect();
        draw("Connection lost", "Reconnecting");
        nextAction = millis() + reconnectMs;
    }

    delay(15);
}
