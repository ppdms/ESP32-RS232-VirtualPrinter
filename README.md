# ESP32 RS-232 Virtual Printer

ESP32-based serial printer emulator that receives RS-232 print data and emails it via WiFi.

## Features

- **Email Delivery**: Sends print jobs to your inbox via SMTP over TLS, with the server certificate verified
- **2MB Job Buffer / 4MB Queue**: Handles large jobs using ESP32 PSRAM
- **Dual-Core**: RS-232 reception on Core 0, email transmission on Core 1
- **Auto-Reconnect**: WiFi and NTP time sync with automatic recovery, and backoff when sending fails
- **Faithful Delivery**: The email body shows the printed text; the exact bytes are attached as a `.prn` file whenever the body can't show everything (printer control codes, very long jobs)

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

### Status LED

The on-board RGB LED (GPIO 38; set `PIN_STATUS_LED` if yours is on 48) shows:

| Colour | Meaning |
|--------|---------|
| Red, fast blink | WiFi down |
| Blue, slow blink | Jobs waiting to be emailed |
| Green, slow blink | Idle |

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

`config.h.example` also lists optional settings: time zone, the printer's
character set (e.g. CP437 or CP737 for DOS Greek), and whether to always attach
the raw job.

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

### Network requirements

The device only makes outbound connections: SMTP over TLS (port 465) and NTP
(UDP 123). It waits for NTP before sending, because verifying the SMTP
server's certificate needs the correct date. If your network blocks NTP,
`SMTP_TLS_INSECURE` turns verification off, but then anyone on the network
path can read your SMTP password.

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

A print job ends after 2 seconds without data. Each job becomes one email
with a unique subject (`[Printer] Print Job #12 - 2026-10-25 14:03:11`). If
sending fails, the job stays at the front of the queue and is retried with
exponential backoff (5 s up to 5 min). If the queue's 4MB budget is exceeded,
the oldest jobs are dropped.
