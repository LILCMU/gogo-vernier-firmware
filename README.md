# GoGo Vernier Integration Firmware

An integration firmware that enables Vernier wireless sensors to work with GoGo Board 7 using the ESP32C3 chip as a co-processor. Communication between the ESP32C3 and GoGo Board 7 is handled through a serial JSON protocol.

## Overview

This project aims to bridge Vernier Go Direct wireless sensors with the GoGo Board 7 ecosystem. The ESP32C3 acts as a dedicated sensor interface, collecting data from Vernier sensors and transmitting it to the GoGo Board 7 via serial communication using a structured JSON protocol.

## Project Goals

- Enable seamless integration of Vernier wireless sensors with GoGo Board 7
- Provide real-time sensor data communication through JSON protocol
- Support multiple Vernier Go Direct sensor types
- Maintain compatibility with existing GoGo Board 7 programming environment

## Hardware Requirements

- **GoGo Board 7** - Main controller board
- **ESP32C3 module** - Co-processor for Vernier sensor communication
- **Vernier Go Direct sensors** - Compatible wireless sensors

## Project Status

🚧 **Work in Progress** 🚧

This project is currently in development. The current codebase contains example Arduino programs for testing Vernier sensor connectivity and Arduino library functionality.

## Development Phases

1. **Phase 1** (Current): Testing Vernier sensor Arduino library integration
2. **Phase 2**: Implement JSON serial communication protocol
3. **Phase 3**: Develop GoGo Board 7 integration layer
4. **Phase 4**: Testing and optimization

## Supported Sensors

The firmware will support Vernier Go Direct wireless sensors, including:
- Temperature sensors (GDX-TMP)
- Motion sensors
- Light sensors
- And other Go Direct compatible devices

## Communication Protocol

The ESP32C3 will communicate with GoGo Board 7 using a serial JSON protocol to ensure structured and reliable data exchange.

## Installation

### Prerequisites

- Arduino IDE or PlatformIO
- ESP32 board package
- GDXLib library for Vernier sensor communication

### Current Testing Setup

1. Install required libraries and ESP32 support
2. Load the example firmware onto ESP32C3
3. Test connectivity with available Vernier Go Direct sensors

## References

- [GDXLib Documentation](https://github.com/VernierST/GDXLib)
- [Vernier Go Direct Sensors](https://www.vernier.com/products/sensors/)
- [GoGo Board 7 Documentation](https://gogoboard.org/)

## Contributing

This project is under active development. Contributions and feedback are welcome as we work toward full integration.
