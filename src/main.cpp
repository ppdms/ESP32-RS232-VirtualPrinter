#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include <time.h>
#include <atomic>
#include <vector>
#include <deque>

#define ENABLE_SMTP
// #define ENABLE_DEBUG // Enable ReadyMail debug logging
#include <ReadyMail.h>

#include "config.h"

#ifndef DEBUG_MODE
#define DEBUG_MODE 0
#endif

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

#define PIN_STATUS_LED 2

// Timing
#define PRINT_JOB_TIMEOUT_MS 2000
#define PRINT_JOB_BUFFER_SIZE (2 * 1024 * 1024) // 2MB
#define MAX_BUFFER_SIZE (PRINT_JOB_BUFFER_SIZE) // Hard limit for safety
#define MAX_QUEUE_BYTES (4 * 1024 * 1024) // 4MB limit for queued jobs
#define STATUS_PRINT_INTERVAL_MS 10000
#define WIFI_CONNECT_TIMEOUT_MS 10000
#define WIFI_RECONNECT_INTERVAL_MS 10000
#define EMAIL_RETRY_COUNT 3
#define EMAIL_RETRY_DELAY_MS 5000
#define MIN_VALID_EPOCH_TIME 100000 // approx 1 day after 1970, ensures NTP sync

// Tasks
#define RS232_TASK_PRIORITY 6
#define EMAIL_TASK_PRIORITY 5
#define RS232_TASK_STACK_SIZE 8192
#define EMAIL_TASK_STACK_SIZE (16 * 1024)
#define WDT_TIMEOUT_SECONDS 60

#define RS232_RX_BUFFER_SIZE 1024 // Increased from default 256 for reliability

// Globals wrapped in a struct for better organization
struct SystemState {
    std::vector<uint8_t> printJobBuffer;
    std::deque<std::vector<uint8_t>> jobQueue; // Shared queue for completed jobs
    SemaphoreHandle_t printJobMutex = NULL;
    std::atomic<uint32_t> printJobSize{0};
    std::atomic<uint32_t> totalBytesReceived{0};
    std::atomic<uint32_t> emailsSent{0};
    std::atomic<uint32_t> emailsFailed{0};
    std::atomic<uint32_t> droppedJobs{0};
    std::atomic<uint32_t> queueSize{0};
    std::atomic<uint32_t> queueTotalBytes{0};
    std::atomic<bool> currentPrintJobActive{false};
    std::atomic<bool> wifiConnected{false};
    TaskHandle_t rs232TaskHandle = NULL;
    TaskHandle_t emailTaskHandle = NULL;
} sys;

HardwareSerial RS232Serial(1);

// Callback must remain global or static for ReadyMail
void smtpCallback(SMTPStatus status) {
    // Only print errors or critical info in production
    if (status.errorCode != 0) {
        Serial.printf("[SMTP] Error %d: %s\r\n", status.errorCode, status.text.c_str());
    }
}

void connectWiFi() {
    static bool timeConfigured = false;

    if (WiFi.status() == WL_CONNECTED) {
        sys.wifiConnected = true;
        return;
    }

    Serial.printf("[WiFi] Connecting to %s...", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t startTime = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_CONNECT_TIMEOUT_MS) {
        delay(500);
        esp_task_wdt_reset(); // Prevent watchdog timeout
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        sys.wifiConnected = true;
        Serial.printf(" OK (%s)\r\n", WiFi.localIP().toString().c_str());

        if (!timeConfigured) {
            configTime(0, 0, "pool.ntp.org", "time.nist.gov");
            timeConfigured = true;
        }

        // Wait for valid time
        uint32_t ntpStart = millis();
        while (time(nullptr) < MIN_VALID_EPOCH_TIME) {
            delay(100);
            esp_task_wdt_reset(); // Prevent watchdog timeout
            if (millis() - ntpStart > 10000) break; // 10s timeout for NTP
        }
    } else {
        sys.wifiConnected = false;
        Serial.println(" FAILED");
    }
}

// Helper to wait for SMTP async operations while feeding the watchdog
bool awaitSMTP(SMTPClient& smtp) {
    uint32_t start = millis();
    while (smtp.isProcessing()) {
        esp_task_wdt_reset();
        smtp.loop();
        if (millis() - start > 60000) { // 60s fallback timeout
             Serial.println("[SMTP] Operation timed out");
             return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

bool sendPrintJobEmail(const std::vector<uint8_t>& content) {
    if (!sys.wifiConnected || WiFi.status() != WL_CONNECTED) {
        Serial.println("[EMAIL] No WiFi");
        return false;
    }

    // Use local client and SMTP instance for fresh state every time
    WiFiClientSecure local_ssl_client;
    local_ssl_client.setInsecure();
    local_ssl_client.setTimeout(15000); // 15 seconds

    SMTPClient smtp(local_ssl_client);

    // Async connect (await = false)
    if (!smtp.connect(SMTP_HOST, SMTP_PORT, smtpCallback, true, false)) {
        Serial.println("[EMAIL] Connect initiate failed");
        return false;
    }

    if (!awaitSMTP(smtp)) {
        Serial.println("[EMAIL] Connect wait failed");
        smtp.stop();
        return false;
    }

    if (!smtp.isConnected()) {
        Serial.println("[EMAIL] Not connected after wait");
        return false;
    }

    // Async authenticate (await = false)
    if (!smtp.authenticate(SMTP_USER, SMTP_PASSWORD, readymail_auth_password, false)) {
        Serial.println("[EMAIL] Auth initiate failed");
        smtp.stop();
        return false;
    }

    if (!awaitSMTP(smtp)) {
         Serial.println("[EMAIL] Auth wait failed");
         smtp.stop();
         return false;
    }

    if (!smtp.isAuthenticated()) {
        Serial.println("[EMAIL] Auth failed");
        smtp.stop();
        return false;
    }

    // Use internal message object for Async operations
    SMTPMessage& msg = smtp.getMessage();
    msg.clear(); 
    
    msg.headers.add(rfc822_from, String("ESP32 Printer <") + SMTP_USER + ">");
    msg.headers.add(rfc822_to, EMAIL_RECIPIENT);
    msg.headers.add(rfc822_subject, String(EMAIL_SUBJECT_PREFIX) + " Print Job");
    msg.timestamp = time(nullptr);
    
    // Efficiently set body using pointer and size (Zero-Copy)
    // Note: content buffer must remain valid until smtp.send() returns.
    if (!content.empty()) {
        // Create a null-terminated string copy to ensure safety and compatibility
        String bodyStr((const char*)content.data(), content.size());
        msg.text.body(bodyStr);
    } else {
        msg.text.body("Empty Job");
    }

    // Async send (await = false)
    bool success = smtp.send(msg, "", false);
    
    if (success) {
        if (awaitSMTP(smtp)) {
             Serial.println("[EMAIL] Sent");
             sys.emailsSent++;
        } else {
             Serial.println("[EMAIL] Send wait failed");
             success = false;
        }
    } else {
        Serial.println("[EMAIL] Send initiate failed");
    }
    
    smtp.stop();
    return success;
}

void rs232ReceiveTask(void* parameter) {
    uint32_t lastDataTime = 0;
    bool printJobActive = false;
    uint32_t lastStatusTime = 0;
    uint8_t rxBuffer[RS232_RX_BUFFER_SIZE]; 
    
    // Local accumulator to reduce mutex locking frequency
    std::vector<uint8_t> localAccumulator;
    localAccumulator.reserve(4096); 

    esp_task_wdt_add(NULL);

    while (true) {
        esp_task_wdt_reset();

        int available = RS232Serial.available();
        bool dataReceived = false;

        if (available > 0) {
            int toRead = min(available, (int)sizeof(rxBuffer));
            int bytesRead = RS232Serial.readBytes(rxBuffer, toRead);

            if (bytesRead > 0) {
                // Accumulate locally
                localAccumulator.insert(localAccumulator.end(), rxBuffer, rxBuffer + bytesRead);
                lastDataTime = millis();
                dataReceived = true;

                if (!printJobActive) {
                    printJobActive = true;
                    sys.currentPrintJobActive = true;
                    Serial.println("[RS232] Job started");
                }
            }
        }

        // Determine if we need to flush to global buffer
        bool jobTimedOut = printJobActive && (millis() - lastDataTime > PRINT_JOB_TIMEOUT_MS);
        bool thresholdReached = localAccumulator.size() >= 2048; // Flush at 2KB
        
        if (jobTimedOut || (thresholdReached && dataReceived)) {
             // Try to take mutex to flush
             // Increase timeout if job is ending to ensure we queue it
             TickType_t waitTime = jobTimedOut ? pdMS_TO_TICKS(500) : pdMS_TO_TICKS(10);
             
             if (xSemaphoreTake(sys.printJobMutex, waitTime) == pdTRUE) {
                 
                 // 1. Flush Local -> Global
                 if (!localAccumulator.empty()) {
                     if (sys.printJobBuffer.size() < MAX_BUFFER_SIZE) {
                         // Append what fits
                         size_t availableSpace = MAX_BUFFER_SIZE - sys.printJobBuffer.size();
                         size_t toCopy = min(availableSpace, localAccumulator.size());
                         
                         sys.printJobBuffer.insert(sys.printJobBuffer.end(), 
                                                   localAccumulator.begin(), 
                                                   localAccumulator.begin() + toCopy);
                         
                         sys.totalBytesReceived += toCopy;
                         
                         if (toCopy < localAccumulator.size()) {
                              Serial.println("[RS232] Buffer Full! Dropping data.");
                         }
                     }
                     localAccumulator.clear();
                 }

                 // 2. Handle Job Completion (Queueing)
                 if (jobTimedOut) {
                      size_t jobSize = sys.printJobBuffer.size();
                      
                      if (jobSize > 0) { // Only queue if we have data
                          // Enforce queue limit
                          while (!sys.jobQueue.empty() && (sys.queueTotalBytes + jobSize > MAX_QUEUE_BYTES)) {
                               size_t freedBytes = sys.jobQueue.front().size();
                               sys.jobQueue.pop_front();
                               sys.queueTotalBytes -= freedBytes;
                               sys.droppedJobs++;
                               Serial.println("[QUEUE] Full (Memory). Dropped oldest job.");
                          }

                          std::vector<uint8_t> job;
                          job.reserve(jobSize);
                          sys.printJobBuffer.swap(job);
                          // Restore capacity for next job
                          sys.printJobBuffer.reserve(PRINT_JOB_BUFFER_SIZE);
                          
                          sys.jobQueue.push_back(std::move(job));
                          sys.queueSize = sys.jobQueue.size();
                          sys.queueTotalBytes += jobSize;
                          sys.printJobSize = 0;
                          
                          Serial.printf("[RS232] Job queued. Queue size: %u | Bytes: %u\r\n", (uint32_t)sys.queueSize, (uint32_t)sys.queueTotalBytes);
                      }
                 }
                 
                 xSemaphoreGive(sys.printJobMutex);
                 
                 if (jobTimedOut) {
                     printJobActive = false;
                     sys.currentPrintJobActive = false;
                 }
             }
        }

        if (millis() - lastStatusTime >= STATUS_PRINT_INTERVAL_MS) {
            Serial.printf("[STATUS] RX: %u | Queue: %u (%u B) | Dropped: %u | WiFi: %s | Sent: %u | Failed: %u\r\n",
                (uint32_t)sys.totalBytesReceived,
                (uint32_t)sys.queueSize,
                (uint32_t)sys.queueTotalBytes,
                (uint32_t)sys.droppedJobs,
                sys.wifiConnected ? "OK" : "DOWN",
                (uint32_t)sys.emailsSent,
                (uint32_t)sys.emailsFailed);
            Serial.flush();
            lastStatusTime = millis();
        }

        // Adaptive delay: only delay if no data was available
        if (available == 0 && !dataReceived) vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void emailSendTask(void* parameter) {
    uint32_t lastWifiCheck = 0;
    std::deque<std::vector<uint8_t>> jobQueue;

    esp_task_wdt_add(NULL);

    while (true) {
        esp_task_wdt_reset();

        // 1. Maintain WiFi Connection
        if (millis() - lastWifiCheck >= WIFI_RECONNECT_INTERVAL_MS) {
            if (WiFi.status() != WL_CONNECTED) {
                sys.wifiConnected = false;
                Serial.println("[WiFi] Reconnecting...");
                connectWiFi();
            }
            lastWifiCheck = millis();
        }

        // 2. Process the Queue
        std::vector<uint8_t> currentJob;
        bool hasJob = false;

        if (xSemaphoreTake(sys.printJobMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (!sys.jobQueue.empty()) {
                currentJob = std::move(sys.jobQueue.front());
                sys.jobQueue.pop_front();
                sys.queueSize = sys.jobQueue.size();
                
                if (sys.queueTotalBytes >= currentJob.size()) {
                    sys.queueTotalBytes -= currentJob.size();
                } else {
                    sys.queueTotalBytes = 0;
                    Serial.println("[QUEUE] Underflow detected, resetting bytes to 0");
                }
                
                hasJob = true;
            }
            xSemaphoreGive(sys.printJobMutex);
        }

        if (hasJob) {
            // Only try to send if we believe we are connected
            if (sys.wifiConnected && WiFi.status() == WL_CONNECTED) {
                Serial.printf("[EMAIL] Processing job (%u bytes)...\r\n", currentJob.size());

                if (sendPrintJobEmail(currentJob)) {
                    // Success: Remove from queue
                    // Small delay to prevent flooding SMTP server
                    vTaskDelay(pdMS_TO_TICKS(1000));
                } else {
                    // Failure: Keep in queue, retry later
                    Serial.println("[EMAIL] Failed to send. Will retry.");
                    sys.emailsFailed++;
                    
                    // CRITICAL: Push job back to front of queue to avoid data loss
                    if (xSemaphoreTake(sys.printJobMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                        size_t jobSize = currentJob.size(); // Capture size BEFORE move
                        sys.jobQueue.push_front(std::move(currentJob));
                        sys.queueSize = sys.jobQueue.size();
                        sys.queueTotalBytes += jobSize;
                        xSemaphoreGive(sys.printJobMutex);
                    }

                    // Longer delay on failure
                    vTaskDelay(pdMS_TO_TICKS(5000));
                }
            } else {
                // WiFi down. Put the job back at the front of the queue to try again later.
                if (xSemaphoreTake(sys.printJobMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    sys.jobQueue.push_front(std::move(currentJob));
                    sys.queueSize = sys.jobQueue.size();
                    sys.queueTotalBytes += currentJob.size();
                    xSemaphoreGive(sys.printJobMutex);
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        } else {
            // Queue empty, idle
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
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
        Serial.printf("PSRAM: %u bytes found\n", ESP.getPsramSize());
        sys.printJobBuffer.reserve(PRINT_JOB_BUFFER_SIZE);
    } else {
        Serial.println("WARNING: No PSRAM found! Buffer limited to internal RAM.");
        // Fallback or just don't reserve, let vector manage small growth
    }

    pinMode(PIN_STATUS_LED, OUTPUT);
    
    // Increase RX buffer size before begin() to handle high-speed bursts
    RS232Serial.setRxBufferSize(RS232_RX_BUFFER_SIZE);
    RS232Serial.begin(RS232_BAUD_RATE, RS232_CONFIG, PIN_RS232_RX, PIN_RS232_TX);

    Serial.printf("RS232: %d baud, RX=%d, TX=%d\r\n", RS232_BAUD_RATE, PIN_RS232_RX, PIN_RS232_TX);

    connectWiFi();

    Serial.printf("SMTP: %s:%d -> %s\r\n", SMTP_HOST, SMTP_PORT, EMAIL_RECIPIENT);

    esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true);

    xTaskCreatePinnedToCore(rs232ReceiveTask, "RS232", RS232_TASK_STACK_SIZE, NULL, RS232_TASK_PRIORITY, &sys.rs232TaskHandle, 0);
    xTaskCreatePinnedToCore(emailSendTask, "EMAIL", EMAIL_TASK_STACK_SIZE, NULL, EMAIL_TASK_PRIORITY, &sys.emailTaskHandle, 1);

#ifdef SEND_TEST_EMAIL_ON_BOOT
    {
        const char* testMsg = "This is a test email from ESP32 Virtual Printer startup.";
        std::vector<uint8_t> testJob(testMsg, testMsg + strlen(testMsg));
        
        if (xSemaphoreTake(sys.printJobMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            sys.jobQueue.push_back(std::move(testJob));
            sys.queueSize = sys.jobQueue.size();
            sys.queueTotalBytes += strlen(testMsg);
            xSemaphoreGive(sys.printJobMutex);
            Serial.println("[TEST] Boot test email queued.");
        }
    }
#endif

    Serial.println("Ready\n");
}

void loop() {
    static uint32_t lastBlink = 0;
    uint32_t blinkInterval = sys.wifiConnected ? 1000 : 250;

    if (millis() - lastBlink >= blinkInterval) {
        digitalWrite(PIN_STATUS_LED, !digitalRead(PIN_STATUS_LED));
        lastBlink = millis();
    }

    delay(100);
}
