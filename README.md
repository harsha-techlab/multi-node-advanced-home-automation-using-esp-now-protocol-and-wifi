# 🏠 Multi-Node Advanced Home Automation System

### ESP32-Based Master-Slave Home Automation System with Firebase Cloud Integration

A multi-room home automation project using ESP32 microcontrollers, ESP-NOW communication, and Firebase Realtime Database for monitoring and controlling devices.

## 🚀 Project Overview

This system provides smart automation and remote control for different areas of a home using a master controller and multiple slave nodes.

## ✨ Features

* Multi-node communication using ESP-NOW
* Firebase Realtime Database integration
* Multi-room control: Hall, Bedroom, Kitchen, and Door
* Motion-based lighting automation
* Temperature-based bedroom fan control
* Gas detection and automatic kitchen exhaust control
* Door lock control
* Touch-based and website-based operation
* Device state persistence

## 🛠️ Hardware and Technologies

* ESP32 and ESP32-S3 microcontrollers
* ESP-NOW
* Firebase Realtime Database
* MCP9808 temperature sensor
* PIR and IR motion sensors
* LDR light sensor
* MQ-2 gas sensor
* Relays, fans, buzzer, and solenoid door lock

## 🏗️ System Architecture

* **Master:** Coordinates communication and Firebase connectivity.
* **Hall & Door Node:** Controls hall lighting, fan, and door lock.
* **Bedroom Node:** Controls bedroom lighting and temperature-based fan automation.
* **Kitchen Node:** Controls lighting and gas detection-based exhaust automation.

## 📂 Project Documentation

The complete project documentation is available in the repository as a PDF.

## 🔐 Security Note

Wi-Fi passwords and Firebase credentials should be kept private. Do not upload real credentials to a public repository.

## 👨‍💻 Project Status

Hardware and software implementation documented. Refer to the project PDF for detailed configuration, implementation, and testing information.
