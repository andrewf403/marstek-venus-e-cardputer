#pragma once

#include <stddef.h>
#include <stdint.h>

namespace venus {
constexpr uint8_t magic = 0x73;
constexpr uint8_t family = 0x23;
constexpr uint8_t runtimeCommand = 0x03;
constexpr uint8_t settingsCommand = 0x0A;
constexpr uint8_t ledCommand = 0x59;
constexpr uint8_t readSchedules = 0x01;
constexpr size_t maxFrameSize = 255;

inline uint8_t checksum(const uint8_t* data, size_t size) {
    uint8_t result = 0;
    for (size_t i = 0; i < size; ++i) {
        result ^= data[i];
    }
    return result;
}

struct Runtime {
    uint8_t inverterState;
    uint8_t soc;
    int16_t outletWatts;
    bool hasWorkMode;
    uint8_t workMode;
    bool hasBackupSetting;
    bool backupEnabled;
    bool hasPowerReadings;
    int16_t batteryWatts;
    int16_t gridWatts;
    bool hasLedSetting;
    bool ledEnabled;
};

struct UpsSettings {
    bool enabled;
    int32_t chargeWatts;
};

inline int16_t signedWatts(const uint8_t* field) {
    const int32_t raw = field[0] | (static_cast<int32_t>(field[1]) << 8);
    return static_cast<int16_t>(raw < 32768 ? raw : raw - 65536);
}

inline bool validReply(const uint8_t* frame, size_t size, uint8_t command, size_t payloadSize) {
    return size >= payloadSize + 5 && size <= maxFrameSize &&
           frame[0] == magic && frame[1] == size && frame[2] == family &&
           frame[3] == command && checksum(frame, size - 1) == frame[size - 1];
}

inline bool decodeRuntime(const uint8_t* frame, size_t size, Runtime& reading) {
    if (!validReply(frame, size, runtimeCommand, 12)) {
        return false;
    }

    const uint8_t* payload = frame + 4;
    if (payload[11] > 100) {
        return false;
    }

    const size_t payloadSize = size - 5;
    const bool hasPower = payloadSize == 133 || payloadSize == 139;
    reading = {payload[4], payload[11], signedWatts(payload),
               payloadSize >= 39, payloadSize >= 39 ? payload[38] : uint8_t{0},
               payloadSize >= 50 && payload[49] <= 1, payloadSize >= 50 && payload[49] == 1,
               hasPower, hasPower ? signedWatts(payload + 112) : int16_t{0},
               hasPower ? signedWatts(payload + 116) : int16_t{0},
               hasPower && payload[124] <= 1, hasPower && payload[124] == 1};
    return true;
}

inline bool decodeUpsSettings(const uint8_t* frame, size_t size, UpsSettings& settings) {
    constexpr size_t upsSlotOffset = 9 * 8;
    if (!validReply(frame, size, settingsCommand, upsSlotOffset + 8)) {
        return false;
    }
    const uint8_t* slot = frame + 4 + upsSlotOffset;
    if (slot[7] > 1) {
        return false;
    }
    const int32_t power = signedWatts(slot + 5);
    settings = {slot[7] == 1, power < 0 ? -power : power};
    return true;
}

class FrameReceiver {
  public:
    void reset() { size_ = 0; }

    bool push(uint8_t byte) {
        if (size_ == 0 && byte != magic) {
            return false;
        }
        if (size_ == 1 && byte < 5) {
            reset();
            return false;
        }

        data_[size_++] = byte;
        return size_ >= 2 && size_ == data_[1];
    }

    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }

  private:
    uint8_t data_[maxFrameSize]{};
    size_t size_ = 0;
};
} // namespace venus
