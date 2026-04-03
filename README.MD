# ESP32-S3 AI ChatBot

ESP32-S3 chatbot ที่เชื่อมต่อกับ OpenAI API สามารถควบคุม GPIO และ NeoPixel RGB LED ผ่าน Web UI ด้วยภาษาธรรมชาติ (ไทย/อังกฤษ)

## Features

- **AI Chat** — คุยกับ GPT-4o-mini ผ่าน WebSocket แบบ real-time
- **GPIO Control** — สั่ง AI เปิด/ปิด/toggle/อ่านค่า GPIO pins (4, 5, 6, 7, 15, 16, 17, 18)
- **NeoPixel Control** — สั่ง AI เปลี่ยนสี RGB LED (WS2812B) บน GPIO48
- **Web UI** — หน้าเว็บ responsive รองรับมือถือ แสดงสถานะ GPIO และ NeoPixel แบบ real-time
- **Conversation History** — จำบทสนทนาย้อนหลังได้ถึง 20 turns

## Hardware

- **Board**: ESP32-S3-DevKitC-1 (หรือ ESP32-S3 ที่มี GPIO48 เป็น onboard RGB LED)
- **NeoPixel**: WS2812B บน GPIO48 (onboard)
- **GPIO**: สามารถต่ออุปกรณ์ภายนอกที่ pins 4, 5, 6, 7, 15, 16, 17, 18

## Requirements

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/) >= v5.0.0
- OpenAI API Key — [platform.openai.com/api-keys](https://platform.openai.com/api-keys)

## Getting Started

### 1. กรอก credentials

แก้ไขไฟล์ `main/secrets.h`:

```c
#define WIFI_SSID       "ชื่อ WiFi ของคุณ"
#define WIFI_PASSWORD   "รหัส WiFi"
#define OPENAI_API_KEY  "sk-..."
```

> **Warning**: อย่า commit `secrets.h` ขึ้น git ถ้ามี credentials จริง

### 2. Build และ Flash

```bash
idf.py set-target esp32s3
idf.py build flash monitor
```

### 3. เปิดใช้งาน

เมื่อ boot สำเร็จ Serial monitor จะแสดง IP address:

```
I (xxxx) main: WiFi connected! IP: 192.168.x.x
I (xxxx) main: ChatBot ready! Open http://192.168.x.x/
```

เปิดเบราว์เซอร์ไปที่ `http://<ip-address>/`

## การใช้งาน

พิมพ์คำสั่งเป็นภาษาไทยหรืออังกฤษได้เลย เช่น:

| คำสั่ง | ผลลัพธ์ |
|--------|---------|
| `เปิด pin 4` | GPIO4 → HIGH |
| `ปิด pin 7` | GPIO7 → LOW |
| `toggle pin 5` | GPIO5 สลับสถานะ |
| `อ่านค่า pin 6` | แสดงสถานะปัจจุบัน |
| `เปลี่ยน LED เป็นสีแดง` | NeoPixel → RGB(255,0,0) |
| `ปิด LED` | NeoPixel ดับ |

## Project Structure

```
esp32-ai-agent/
├── main/
│   ├── main.c          # WiFi init + app entry
│   ├── openai_api.c/h  # OpenAI API client + tool call parsing
│   ├── web_server.c/h  # HTTP server + WebSocket handler
│   ├── gpio_ctrl.c/h   # GPIO control
│   ├── neopixel.c/h    # WS2812B LED via RMT
│   ├── config.h        # Configuration constants
│   ├── secrets.h       # WiFi + API key (ไม่ commit)
│   ├── idf_component.yml
│   └── spiffs/
│       └── index.html  # Web UI
├── CMakeLists.txt
├── partitions.csv
└── sdkconfig.defaults
```

## Configuration

แก้ไขได้ใน `main/config.h`:

| ค่า | Default | คำอธิบาย |
|-----|---------|---------|
| `OPENAI_MODEL` | `gpt-4o-mini` | OpenAI model |
| `OPENAI_MAX_TOKENS` | `512` | max tokens per response |
| `NEOPIXEL_GPIO` | `48` | GPIO ของ NeoPixel |
| `NEOPIXEL_COUNT` | `1` | จำนวน LED |
| `MAX_CONVERSATION_TURNS` | `20` | จำนวน turn ที่จำได้ |
| `WEB_SERVER_PORT` | `80` | HTTP port |
