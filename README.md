# ESP32 RS-232 Virtual Printer

ESP32-based serial printer emulator that receives RS-232 print data and emails it via WiFi.

## Features

- **Email Delivery**: Sends print jobs to your inbox via SMTP (SSL/TLS)
- **2MB Job Buffer / 4MB Queue**: Handles large jobs using ESP32 PSRAM
- **Dual-Core**: RS-232 reception on Core 0, email transmission on Core 1
- **Auto-Reconnect**: WiFi and NTP time sync with automatic recovery

## Hardware

| Item | Notes |
|------|-------|
| ESP32-S3-DevKitC-1 | With PSRAM (N8R8 recommended) |
| MAX3232 RS-232 Module | Generic 4-pin module (VCC, GND, RX, TX) |
| Jumper wires | 4x female-to-female |

### Wiring

```
MAX3232 Module    ESP32-S3
──────────────    ────────
VCC               3V3
GND               GND
RxOUT             GPIO 5
TxIN              GPIO 4
```

## Quick Start

### 1. Configure

```bash
cp include/config.h.example include/config.h
```

Edit `include/config.h`:
```cpp
#define WIFI_SSID "YourWiFi"
#define WIFI_PASSWORD "YourPassword"
#define SMTP_HOST "smtp.gmail.com"
#define SMTP_PORT 465
#define SMTP_USER "you@gmail.com"
#define SMTP_PASSWORD "app-password"
#define EMAIL_RECIPIENT "recipient@example.com"
#define EMAIL_SUBJECT_PREFIX "[Printer]"
```

### 2. Build and Flash

```bash
pio run -t upload
```

### 3. Monitor

```bash
pio device monitor
```

## Configuration

Serial settings in `src/main.cpp`:

```cpp
#define RS232_BAUD_RATE 9600      // Match your device
#define RS232_CONFIG SERIAL_8N1   // 8N1, 7E1, 8E1, etc.
```

## Architecture

```
┌─────────────────────────────────────────────┐
│                   ESP32-S3                  │
│                                             │
│   UART1 RX ──► 2MB Buffer ──► SMTP Client   │
│   (RS-232)     Job Queue      (WiFi)        │
│   Core 0       (4MB)          Core 1        │
└─────────────────────────────────────────────┘
```