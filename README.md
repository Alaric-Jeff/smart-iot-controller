# Smart IoT Laundry Controller

This project is an **ESP32-based prototype** for a smart automated laundry drying system. It combines environmental sensing, actuator control, and cloud integration for remote monitoring.

## Features
- **Temperature & Humidity Monitoring** using DHT22 sensor
- **Rain & Light Detection** to prevent clothes from being exposed to rain or low sunlight
- **Linear Actuator Control** to extend/retract the drying rod
- **Fan Control** to assist in drying when conditions allow
- **Cloud Integration** with **Google Firestore** for real-time monitoring of sensor data
- **Safe Actuator Operation** with built-in relay dead-time and state guards

## Hardware
- ESP32 Development Board
- DHT22 Temperature & Humidity Sensor
- Rain sensor (analog + digital)
- Light sensor (analog + digital)
- 12V Linear Actuator
- Relay module for actuator & fans
- 12V 30A Power Supply (or appropriate PSU)
- Jumper wires, connectors, and protective housing

## Wiring Overview
- Actuator:
  - Extend Pin → Relay control pin on ESP32
  - Retract Pin → Relay control pin on ESP32
- Fans Pin → Relay control pin
- Sensors → Analog/Digital pins on ESP32 as defined in the code

> **Note:** Always ensure PSU is safely connected, and actuator relays are never triggered simultaneously to prevent damage.

## Software
- Written in **Arduino C++**
- Uses the following libraries:
  - `WiFi.h` / `WiFiClientSecure.h`
  - `HTTPClient.h`
  - `ArduinoJson.h`
  - `DHT.h`
- Periodically reads sensors every 10 seconds and sends updates to Firestore
- Provides **manual control functions** for actuator and fans (`extendActuator()`, `retractActuator()`, `openFans()`, `closeFans()`)

## Setup
1. Connect hardware according to pins in the sketch
2. Update WiFi credentials
3. Update Firestore document URL
4. Upload the sketch to ESP32
5. Ensure PSU and relays are wired safely
6. Monitor serial output for sensor readings and actuator/fan state

## License
MIT License – open for educational and hobbyist use.

