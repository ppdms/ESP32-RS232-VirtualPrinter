#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include <time.h>
#include <atomic>
#include <deque>
#include <new>
#include <vector>

#define ENABLE_SMTP
// #define ENABLE_DEBUG // Enable ReadyMail debug logging
#include <ReadyMail.h>

#include "config.h"
#include "codepages.h"

// RS-232 configuration
#define RS232_BAUD_RATE 9600
#define RS232_CONFIG SERIAL_8N1

// Pin definitions
#define PIN_RS232_RX 5
#define PIN_RS232_TX 4

// NOTE: The currently used RS232 module (generic MAX3232) only exposes
// VCC, GND, RX, and TX. The RTS/CTS pins are NOT broken out.
// Therefore, Hardware Flow Control is physically impossible.
// The sending device must be configured to ignore CTS.

#ifndef PIN_STATUS_LED
#define PIN_STATUS_LED 38 // On-board WS2812 RGB LED (GPIO 48 on some DevKitC-1 v1.0 boards)
#endif
#define STATUS_LED_LEVEL 16 // Brightness per colour channel, 0-255

#ifndef TIMEZONE
#define TIMEZONE "UTC0" // POSIX TZ string used for the Date header and subjects
#endif

// ReadyMail rescans the whole body for every byte it reads, so body cost grows
// with the square of its size. Larger jobs are cut short here and attached in full.
#ifndef EMAIL_BODY_MAX_BYTES
#define EMAIL_BODY_MAX_BYTES (16 * 1024)
#endif

#ifndef EMAIL_ALWAYS_ATTACH_RAW
#define EMAIL_ALWAYS_ATTACH_RAW 0 // 0: attach the raw job only when the body can't show all of it
#endif

// Timing
#define PRINT_JOB_TIMEOUT_MS 2000
#define PRINT_JOB_BUFFER_SIZE (2 * 1024 * 1024) // 2MB, hard limit per job
#define MAX_QUEUE_BYTES (4 * 1024 * 1024) // 4MB for queued jobs, including the one being sent
#define NO_PSRAM_JOB_BUFFER_SIZE (32 * 1024)
#define NO_PSRAM_MAX_QUEUE_BYTES (64 * 1024)
#define STATUS_PRINT_INTERVAL_MS 10000
#define WIFI_CHECK_INTERVAL_MS 1000
#define WIFI_RECONNECT_INTERVAL_MS 30000 // fallback for disconnects the driver doesn't retry itself
#define EMAIL_SEND_INTERVAL_MS 1000 // pause between emails to avoid flooding the SMTP server
#define EMAIL_RETRY_DELAY_MS 5000 // first retry delay, doubled after each failure
#define EMAIL_RETRY_MAX_DELAY_MS (5 * 60 * 1000)
#define EMAIL_MAX_SEND_FAILURES 8 // drop a job that keeps failing after a successful login
#define SMTP_STEP_TIMEOUT_MS 60000
#define SMTP_HANDSHAKE_TIMEOUT_S 20
#define MIN_VALID_EPOCH_TIME 1735689600 // 2025-01-01; anything earlier means NTP hasn't synced

// Tasks
#define RS232_TASK_PRIORITY 6
#define EMAIL_TASK_PRIORITY 5
#define RS232_TASK_STACK_SIZE 8192
#define EMAIL_TASK_STACK_SIZE (16 * 1024)
#define WDT_TIMEOUT_SECONDS 120 // must exceed DNS + TCP connect + TLS handshake, which block in one call

#define RS232_RX_BUFFER_SIZE 4096 // UART driver ring buffer
#define RS232_READ_CHUNK_SIZE 1024

struct PrintJob {
    std::vector<uint8_t> data;
    uint32_t seq = 0;
    time_t receivedAt = 0;      // 0 if the clock wasn't set yet
    uint32_t droppedBytes = 0;  // bytes lost because the job outgrew the receive buffer
    uint8_t sendFailures = 0;   // failures after a successful login, i.e. likely caused by this job
};

// Globals wrapped in a struct for better organization
struct SystemState {
    std::vector<uint8_t> printJobBuffer; // job being received; only the RS232 task touches it
    std::deque<PrintJob> jobQueue;       // completed jobs, guarded by printJobMutex
    SemaphoreHandle_t printJobMutex = NULL;
    size_t maxJobBytes = PRINT_JOB_BUFFER_SIZE;
    size_t maxQueueBytes = MAX_QUEUE_BYTES;
    std::atomic<uint32_t> totalBytesReceived{0};
    std::atomic<uint32_t> droppedBytes{0};
    std::atomic<uint32_t> emailsSent{0};
    std::atomic<uint32_t> emailsFailed{0};
    std::atomic<uint32_t> droppedJobs{0};
    std::atomic<uint32_t> queueSize{0};
    std::atomic<uint32_t> queueTotalBytes{0}; // queued jobs plus the one being sent
    std::atomic<bool> wifiConnected{false};
    TaskHandle_t rs232TaskHandle = NULL;
    TaskHandle_t emailTaskHandle = NULL;
} sys;

HardwareSerial RS232Serial(1);

static bool clockIsSet() {
    return time(nullptr) >= MIN_VALID_EPOCH_TIME;
}

// Callback must remain global or static for ReadyMail
void smtpCallback(SMTPStatus status) {
    // Only print errors or critical info in production
    if (status.errorCode != 0) {
        Serial.printf("[SMTP] Error %d: %s\r\n", status.errorCode, status.text.c_str());
    }
}

// Keeps sys.wifiConnected in step with the driver. WiFi.begin() runs once in
// setup() and the driver reconnects by itself for most disconnect reasons; this
// only restarts the connection if it stays down.
static void maintainWiFi() {
    static uint32_t lastCheck = 0;
    static uint32_t lastAttempt = 0;

    if (millis() - lastCheck < WIFI_CHECK_INTERVAL_MS) return;
    lastCheck = millis();

    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected != sys.wifiConnected) {
        sys.wifiConnected = connected;
        if (connected) {
            Serial.printf("[WiFi] Connected (%s)\r\n", WiFi.localIP().toString().c_str());
        } else {
            Serial.println("[WiFi] Disconnected");
        }
        lastAttempt = millis();
    }

    if (!connected && millis() - lastAttempt >= WIFI_RECONNECT_INTERVAL_MS) {
        Serial.println("[WiFi] Reconnecting...");
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        lastAttempt = millis();
    }
}

// Drives ReadyMail's async state machine until the current step finishes.
// Each loop() call moves at most one ~76-byte line of the message, so it is
// called in batches; one call per 10 ms tick capped uploads at ~6 KB/s.
static bool awaitSMTP(SMTPClient& smtp, uint32_t timeoutMs) {
    uint32_t start = millis();
    while (smtp.isProcessing()) {
        for (int i = 0; i < 8 && smtp.isProcessing(); i++) {
            smtp.loop();
        }
        esp_task_wdt_reset();
        if (millis() - start > timeoutMs) {
            Serial.println("[SMTP] Operation timed out");
            return false;
        }
        vTaskDelay(1);
    }
    return true;
}

#ifdef PRINTER_CODEPAGE
static void appendUtf8(String& out, uint16_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}
#endif

// Renders a job as readable text for the email body: line endings become CRLF,
// printer control codes are dropped and, if PRINTER_CODEPAGE is set, bytes
// >= 0x80 are translated to UTF-8. Returns true if anything was dropped.
static bool renderJobText(const std::vector<uint8_t>& data, String& out, bool& truncated) {
    bool dropped = false;
    truncated = false;
    out.reserve(min(data.size() + data.size() / 4, (size_t)EMAIL_BODY_MAX_BYTES) + 4);

    for (size_t i = 0; i < data.size(); i++) {
        if (out.length() >= EMAIL_BODY_MAX_BYTES) {
            truncated = true;
            break;
        }
        uint8_t c = data[i];
        if (c == '\n') {
            out += "\r\n";
        } else if (c == '\r') {
            if (i + 1 >= data.size() || data[i + 1] != '\n') out += "\r\n"; // bare CR also ends a line
        } else if (c == '\f') {
            out += "\r\n\r\n";
        } else if (c == '\t') {
            out += '\t';
        } else if (c == 0x1B || c == 0x1C || c == 0x1D) {
            i++; // ESC, FS and GS start ESC/P and ESC/POS commands; skip the command byte too
            dropped = true;
        } else if (c < 0x20 || c == 0x7F) {
            dropped = true;
        } else if (c < 0x80) {
            out += (char)c;
        } else {
#ifdef PRINTER_CODEPAGE
            appendUtf8(out, PRINTER_CODEPAGE[c - 0x80]);
#else
            out += (char)c; // passed through unchanged
#endif
        }
    }
    return dropped;
}

// Retry: the job was never transferred (connection, login or setup failed).
// JobFailed: the connection or server failed while this job was being sent.
enum class SendResult { Sent, Retry, JobFailed };

static SendResult sendPrintJobEmail(const PrintJob& job) {
    // Use local client and SMTP instance for fresh state every time
    WiFiClientSecure sslClient;
#ifdef SMTP_TLS_INSECURE
    sslClient.setInsecure();
#else
    // Verify the server against the Mozilla root CA bundle built into the core.
    sslClient.useBuiltinCACertBundle();
#endif
    sslClient.setHandshakeTimeout(SMTP_HANDSHAKE_TIMEOUT_S);

    SMTPClient smtp(sslClient);

    // Async connect (await = false)
    if (!smtp.connect(SMTP_HOST, SMTP_PORT, smtpCallback, true, false) ||
        !awaitSMTP(smtp, SMTP_STEP_TIMEOUT_MS) || !smtp.isConnected()) {
        Serial.println("[EMAIL] Connect failed");
        smtp.stop();
        return SendResult::Retry;
    }

    // Async authenticate (await = false)
    if (!smtp.authenticate(SMTP_USER, SMTP_PASSWORD, readymail_auth_password, false) ||
        !awaitSMTP(smtp, SMTP_STEP_TIMEOUT_MS) || !smtp.isAuthenticated()) {
        Serial.println("[EMAIL] Auth failed");
        smtp.stop();
        return SendResult::Retry;
    }

    char when[24] = "";
    char fileTime[20] = "";
    if (job.receivedAt) {
        struct tm tm;
        localtime_r(&job.receivedAt, &tm);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
        strftime(fileTime, sizeof(fileTime), "%Y%m%d-%H%M%S", &tm);
    }
    // A unique subject per job also stops Gmail from threading every job together.
    String subject = String(EMAIL_SUBJECT_PREFIX) + " Print Job #" + job.seq;
    if (when[0]) subject += String(" - ") + when;
    String filename = String("print-") + (fileTime[0] ? String(fileTime) : String(job.seq)) + ".prn";

    String body;
    bool truncated;
    bool dropped = renderJobText(job.data, body, truncated);
    bool attachRaw = EMAIL_ALWAYS_ATTACH_RAW || dropped || truncated;
    if (body.length() == 0) body = "(This job contains no printable text.)";
    if (truncated) body += String("\r\n\r\n[Preview truncated. The full job is attached as ") + filename + ".]";
    if (job.droppedBytes) body += String("\r\n\r\n[") + job.droppedBytes + " bytes were lost: the job exceeded the receive buffer.]";

    // Use internal message object for Async operations
    SMTPMessage& msg = smtp.getMessage();
    msg.clear();

    msg.headers.add(rfc822_from, String("ESP32 Printer <") + SMTP_USER + ">");
    msg.headers.add(rfc822_to, EMAIL_RECIPIENT);
    msg.headers.add(rfc822_subject, subject);
    msg.timestamp = time(nullptr);

    msg.text.body(body);
    // ReadyMail sends 7bit and quoted-printable bodies without SMTP dot-stuffing,
    // so a printed line holding a single "." would end the message early. Base64
    // makes any content safe.
    msg.text.transferEncoding("base64");

    if (attachRaw) {
        Attachment att;
        att.filename = filename;
        att.name = filename;
        att.mime = "application/octet-stream";
        // Read straight from the job buffer, which outlives the send.
        att.attach_file.blob = job.data.data();
        att.attach_file.blob_size = job.data.size();
        msg.attachments.add(att, attach_type_attachment);
    }

    // Upload time grows with the job; allow for an uplink as slow as ~8 KB/s.
    uint32_t sendTimeoutMs = SMTP_STEP_TIMEOUT_MS + job.data.size() / 8;

    // Async send (await = false). This fails up front on invalid addresses,
    // which is a configuration problem rather than this job's.
    if (!smtp.send(msg, "", false)) {
        Serial.println("[EMAIL] Send initiate failed");
        smtp.stop();
        return SendResult::Retry;
    }

    // isComplete is only set once the server has accepted the message;
    // processing also stops on errors.
    bool sent = awaitSMTP(smtp, sendTimeoutMs) && smtp.status().isComplete;
    smtp.stop();

    if (!sent) {
        Serial.println("[EMAIL] Send failed");
        return SendResult::JobFailed;
    }
    Serial.println("[EMAIL] Sent");
    return SendResult::Sent;
}

// Hands the job that just finished receiving to the email task, dropping the
// oldest queued jobs if it would push the queue over its memory budget.
static void enqueueJob(uint32_t seq, time_t receivedAt, uint32_t droppedBytes) {
    size_t jobSize = sys.printJobBuffer.size();
    if (jobSize == 0) return;

    xSemaphoreTake(sys.printJobMutex, portMAX_DELAY);

    while (!sys.jobQueue.empty() && sys.queueTotalBytes + jobSize > sys.maxQueueBytes) {
        sys.queueTotalBytes -= sys.jobQueue.front().data.size();
        sys.jobQueue.pop_front();
        sys.queueSize = sys.jobQueue.size();
        sys.droppedJobs++;
        Serial.println("[QUEUE] Full (Memory). Dropped oldest job.");
    }

    try {
        PrintJob job;
        // Copy into an exactly-sized vector. Swapping buffers would leave every
        // queued job holding the receive buffer's full 2MB capacity.
        job.data.assign(sys.printJobBuffer.begin(), sys.printJobBuffer.end());
        job.seq = seq;
        job.receivedAt = receivedAt;
        job.droppedBytes = droppedBytes;
        sys.jobQueue.push_back(std::move(job));
        sys.queueTotalBytes += jobSize;
        sys.queueSize = sys.jobQueue.size();
        Serial.printf("[RS232] Job #%u queued (%u B). Queue size: %u | Bytes: %u\r\n",
            (unsigned)seq, (unsigned)jobSize, (unsigned)sys.queueSize, (unsigned)sys.queueTotalBytes);
    } catch (const std::bad_alloc&) {
        sys.droppedJobs++;
        Serial.printf("[QUEUE] Out of memory. Dropped job #%u.\r\n", (unsigned)seq);
    }

    xSemaphoreGive(sys.printJobMutex);

    sys.printJobBuffer.clear(); // keeps the reserved capacity for the next job
}

static bool takeNextJob(PrintJob& job) {
    xSemaphoreTake(sys.printJobMutex, portMAX_DELAY);
    bool hasJob = !sys.jobQueue.empty();
    if (hasJob) {
        job = std::move(sys.jobQueue.front());
        sys.jobQueue.pop_front();
        sys.queueSize = sys.jobQueue.size();
    }
    xSemaphoreGive(sys.printJobMutex);
    return hasJob;
}

// Puts a job that failed to send back at the front of the queue, keeping order.
static void requeueJob(PrintJob&& job) {
    xSemaphoreTake(sys.printJobMutex, portMAX_DELAY);
    try {
        sys.jobQueue.push_front(std::move(job));
        sys.queueSize = sys.jobQueue.size();
    } catch (const std::bad_alloc&) {
        sys.queueTotalBytes -= job.data.size();
        sys.droppedJobs++;
        Serial.printf("[QUEUE] Out of memory. Dropped job #%u.\r\n", (unsigned)job.seq);
    }
    xSemaphoreGive(sys.printJobMutex);
}

void rs232ReceiveTask(void* parameter) {
    uint8_t rxBuffer[RS232_READ_CHUNK_SIZE];
    uint32_t lastDataTime = 0;
    bool printJobActive = false;
    uint32_t jobSeq = 0;
    time_t jobReceivedAt = 0;
    uint32_t jobDroppedBytes = 0;

    esp_task_wdt_add(NULL);

    while (true) {
        esp_task_wdt_reset();

        int bytesRead = 0;
        int available = RS232Serial.available();
        if (available > 0) {
            bytesRead = RS232Serial.read(rxBuffer, min(available, (int)sizeof(rxBuffer)));
        }

        if (bytesRead > 0) {
            if (!printJobActive) {
                printJobActive = true;
                jobSeq++;
                jobReceivedAt = clockIsSet() ? time(nullptr) : 0;
                jobDroppedBytes = 0;
                Serial.printf("[RS232] Job #%u started\r\n", (unsigned)jobSeq);
            }
            lastDataTime = millis();
            sys.totalBytesReceived += bytesRead;

            size_t space = sys.maxJobBytes - sys.printJobBuffer.size();
            size_t toCopy = min(space, (size_t)bytesRead);
            sys.printJobBuffer.insert(sys.printJobBuffer.end(), rxBuffer, rxBuffer + toCopy);

            if (toCopy < (size_t)bytesRead) {
                if (jobDroppedBytes == 0) Serial.println("[RS232] Buffer Full! Dropping the rest of this job.");
                jobDroppedBytes += bytesRead - toCopy;
                sys.droppedBytes += bytesRead - toCopy;
            }
        } else if (printJobActive && millis() - lastDataTime > PRINT_JOB_TIMEOUT_MS) {
            enqueueJob(jobSeq, jobReceivedAt, jobDroppedBytes);
            printJobActive = false;
        }

        // Always yield: the UART driver buffers incoming data, and a task that
        // never blocks would starve core 0's idle task and trip the watchdog.
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void emailSendTask(void* parameter) {
    uint32_t retryDelayMs = EMAIL_RETRY_DELAY_MS;
    uint32_t nextAttemptAt = millis();

    esp_task_wdt_add(NULL);

    while (true) {
        esp_task_wdt_reset();

        // 1. Track the WiFi connection
        maintainWiFi();

        bool due = (int32_t)(millis() - nextAttemptAt) >= 0;
        if (!due || !sys.wifiConnected || sys.queueSize == 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

#ifndef SMTP_TLS_INSECURE
        // Certificate validation needs the real date, so wait for NTP.
        if (!clockIsSet()) {
            static bool loggedClockWait = false;
            if (!loggedClockWait) {
                Serial.println("[EMAIL] Waiting for NTP time sync (needed to verify the server certificate)");
                loggedClockWait = true;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
#endif

        // 2. Process the Queue. The job stays counted in queueTotalBytes while it
        // is sent, so new jobs can't push memory use past the budget meanwhile.
        PrintJob job;
        if (!takeNextJob(job)) continue;

        Serial.printf("[EMAIL] Processing job #%u (%u bytes)...\r\n", (unsigned)job.seq, (unsigned)job.data.size());
        SendResult result = sendPrintJobEmail(job);

        if (result == SendResult::Sent) {
            sys.emailsSent++;
            sys.queueTotalBytes -= job.data.size();
            retryDelayMs = EMAIL_RETRY_DELAY_MS;
            // Small delay to prevent flooding SMTP server
            nextAttemptAt = millis() + EMAIL_SEND_INTERVAL_MS;
            continue;
        }

        sys.emailsFailed++;
        if (result == SendResult::JobFailed && ++job.sendFailures >= EMAIL_MAX_SEND_FAILURES) {
            Serial.printf("[EMAIL] Job #%u failed %u times after login. Dropping it.\r\n",
                (unsigned)job.seq, (unsigned)job.sendFailures);
            sys.queueTotalBytes -= job.data.size();
            sys.droppedJobs++;
        } else {
            Serial.printf("[EMAIL] Failed to send. Will retry in %u s.\r\n", (unsigned)(retryDelayMs / 1000));
            requeueJob(std::move(job));
        }

        // Back off so a broken connection or rejected login doesn't hammer the server
        nextAttemptAt = millis() + retryDelayMs;
        retryDelayMs = min(retryDelayMs * 2, (uint32_t)EMAIL_RETRY_MAX_DELAY_MS);
    }
}

static void printStatus() {
    Serial.printf("[STATUS] RX: %u | Queue: %u (%u B) | Dropped: %u jobs, %u B | WiFi: %s | Clock: %s | Sent: %u | Failed: %u\r\n",
        (unsigned)sys.totalBytesReceived,
        (unsigned)sys.queueSize,
        (unsigned)sys.queueTotalBytes,
        (unsigned)sys.droppedJobs,
        (unsigned)sys.droppedBytes,
        sys.wifiConnected ? "OK" : "DOWN",
        clockIsSet() ? "OK" : "NOT SET",
        (unsigned)sys.emailsSent,
        (unsigned)sys.emailsFailed);
}

void setup() {
    Serial.begin(115200);
    delay(100);

    Serial.println("\nESP32 Virtual Printer");

    sys.printJobMutex = xSemaphoreCreateMutex();
    if (sys.printJobMutex == NULL) {
        Serial.println("ERROR: Mutex failed");
        delay(3000);
        ESP.restart();
    }

    if (psramFound()) {
        Serial.printf("PSRAM: %u bytes found\r\n", (unsigned)ESP.getPsramSize());
    } else {
        Serial.println("WARNING: No PSRAM found! Buffers limited to internal RAM.");
        sys.maxJobBytes = NO_PSRAM_JOB_BUFFER_SIZE;
        sys.maxQueueBytes = NO_PSRAM_MAX_QUEUE_BYTES;
    }
    sys.printJobBuffer.reserve(sys.maxJobBytes);

    // Increase RX buffer size before begin() to handle high-speed bursts
    RS232Serial.setRxBufferSize(RS232_RX_BUFFER_SIZE);
    RS232Serial.begin(RS232_BAUD_RATE, RS232_CONFIG, PIN_RS232_RX, PIN_RS232_TX);

    Serial.printf("RS232: %d baud, RX=%d, TX=%d\r\n", RS232_BAUD_RATE, PIN_RS232_RX, PIN_RS232_TX);

    // Connect in the background; the email task tracks the connection state.
    Serial.printf("[WiFi] Connecting to %s...\r\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    configTzTime(TIMEZONE, "pool.ntp.org", "time.nist.gov");

    Serial.printf("SMTP: %s:%d -> %s\r\n", SMTP_HOST, SMTP_PORT, EMAIL_RECIPIENT);
#ifdef SMTP_TLS_INSECURE
    Serial.println("WARNING: SMTP_TLS_INSECURE is set. The SMTP server certificate is NOT verified.");
#endif

    esp_task_wdt_config_t wdtConfig = {
        .timeout_ms = WDT_TIMEOUT_SECONDS * 1000,
        .idle_core_mask = 1 << 0, // keep watching core 0's idle task, as the default config does
        .trigger_panic = true,
    };
    esp_task_wdt_reconfigure(&wdtConfig);

    xTaskCreatePinnedToCore(rs232ReceiveTask, "RS232", RS232_TASK_STACK_SIZE, NULL, RS232_TASK_PRIORITY, &sys.rs232TaskHandle, 0);
    xTaskCreatePinnedToCore(emailSendTask, "EMAIL", EMAIL_TASK_STACK_SIZE, NULL, EMAIL_TASK_PRIORITY, &sys.emailTaskHandle, 1);

#ifdef SEND_TEST_EMAIL_ON_BOOT
    {
        const char* testMsg = "This is a test email from ESP32 Virtual Printer startup.";
        PrintJob testJob;
        testJob.data.assign(testMsg, testMsg + strlen(testMsg));

        xSemaphoreTake(sys.printJobMutex, portMAX_DELAY);
        sys.queueTotalBytes += testJob.data.size();
        sys.jobQueue.push_back(std::move(testJob));
        sys.queueSize = sys.jobQueue.size();
        xSemaphoreGive(sys.printJobMutex);
        Serial.println("[TEST] Boot test email queued.");
    }
#endif

    Serial.println("Ready\n");
}

void loop() {
    static uint32_t lastBlink = 0;
    static uint32_t lastStatus = 0;
    static bool ledOn = false;
    uint32_t blinkInterval = sys.wifiConnected ? 1000 : 250;

    if (millis() - lastBlink >= blinkInterval) {
        ledOn = !ledOn;
        uint8_t level = ledOn ? STATUS_LED_LEVEL : 0;
        // Red: WiFi down. Blue: jobs waiting to be emailed. Green: idle.
        if (!sys.wifiConnected) {
            rgbLedWrite(PIN_STATUS_LED, level, 0, 0);
        } else if (sys.queueTotalBytes > 0) {
            rgbLedWrite(PIN_STATUS_LED, 0, 0, level);
        } else {
            rgbLedWrite(PIN_STATUS_LED, 0, level, 0);
        }
        lastBlink = millis();
    }

    if (millis() - lastStatus >= STATUS_PRINT_INTERVAL_MS) {
        printStatus();
        lastStatus = millis();
    }

    delay(50);
}
