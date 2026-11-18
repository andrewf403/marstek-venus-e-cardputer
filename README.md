# Venus E 3.0 BLE dashboard for the original Cardputer

Use the printed left and right arrows on the comma and slash keys to cycle
through three pages. The dashboard requests Runtime Info (`0x03`) about every
two seconds.

1. Inverter state, state of charge, outlet watts, and battery activity. The
   activity indicator uses the inverter state.
2. Grid watts and battery watts, both signed, from the extended Venus E 3.0
   runtime reply.
3. Configured charge mode, UPS charge speed, and backup power enabled status.
   The firmware reads the UPS schedule slot with `0x0A` once per connection
   and refreshes it while this page is open. UPS charge speed is the configured
   wattage, not a live power reading.

## Build and flash

From this directory, run:

```sh
pio run
pio run -t upload
pio device monitor -b 115200
```

Use a USB-C data cable. If PlatformIO cannot enter download mode, switch the
Cardputer off and hold **G0** while plugging in the USB cable, then release it
and run the upload again.

## Test on the Venus

1. Keep Bluetooth enabled on the Venus and bring the Cardputer within range.
2. Compare the Cardputer's state, SOC and outlet watts with the Marstek app.
   If the connection fails while the app is open, close the app and retry.
3. Observe the display with the outlet in mains bypass, on battery backup,
   and after mains returns. The known inverter codes are `6` for bypass,
   `4` for backup, and `2` for charging.
4. Save the USB serial log. It prints the first complete frame after each
   connection and whenever the inverter state changes. Press **Q** to pause
   and disconnect. Press **Q** again to reconnect and capture another frame.

The firmware uses the original Cardputer's keyboard and screen through
M5Cardputer and the BLE library bundled with Arduino-ESP32.

Protocol references:
[BLE connection](https://github.com/sphings79/marstek-ble-control/blob/main/src/lib/transport/WebBluetoothTransport.ts),
[packet framing](https://github.com/sphings79/marstek-ble-control/blob/main/src/lib/VenusPacket.ts),
and [runtime layout](https://github.com/sphings79/marstek-ble-control/blob/main/src/lib/payloads/StatePayload.ts).
