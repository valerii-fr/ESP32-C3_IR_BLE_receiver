# ESP IR BLE Receiver (ESP32-C3)

A minimal firmware for ESP32-C3 designed to capture IR signals and forward them to an Android application over BLE.

## TL;DR how-to
- Flash the ESP device with the firmware from the ESP repository
- Download and install the Android APK (available in the GitHub releases)
- Enable BLE on Android and grant all required permissions (no internet required)
- Press the “+” button in the app
- Wait until the BLE connection is established
- Capture IR commands and save the new remote
- Swipe on a command or remote card to edit or remove it
- 
## Purpose

The firmware is intentionally limited to a very narrow scope:

1. Establish a BLE connection with an Android device.
2. Capture IR signals from an IR receiver (TSOP).
3. Transfer the captured IR frame to Android via a BLE characteristic.
4. Log all received IR data to UART (for debugging).

Storage, retransmission, higher-level protocols, and any business logic are **intentionally excluded**.

## Features

- ESP32-C3
- BLE GATT server
- Single BLE service
- Single notify characteristic for IR frame transfer
- IR capture using RMT
- MTU > 20 bytes support (frame chunking)
- UART logging

## Architecture

TSOP IR Receiver -> RMT -> IR durations (Int[]) -> BLE Notify -> Android App

## BLE

- Custom 128-bit UUIDs
- Data transfer via `notify` only
- ESP acts as a passive sender; Android initiates and controls the interaction

## Intentional Limitations

- No SPIFFS / NVS
- No IR code storage
- No IR retransmission
- No IR protocol decoding (NEC / RC5 / etc.)
- No connection security (bonding / encryption)

## Usage

1. Connect an IR receiver (TSOP) to a GPIO pin (set to GPIO 4 by default).
2. Build and flash the firmware to ESP32-C3.
3. Launch the Android app and connect via BLE.
4. Send an IR command using a remote control.
5. Receive the raw duration array on the Android side.

## Project Goal

To provide a simple and reliable bridge between an IR receiver and an Android application, keeping all processing, visualization, and storage logic **entirely on the Android side**.

## Status

Experimental / Prototype  
Interfaces and data formats may change.

## Links
[Android project](https://github.com/valerii-fr/IR-Bridge)
