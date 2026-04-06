/*
  Rui Santos
  Complete project details at https://RandomNerdTutorials.com/esp32-date-time-ntp-client-server-arduino/

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files.

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.
*/

#include <WiFi.h>
#include <string.h>
#include "time.h"
#include "credential.h"

namespace
{
constexpr char kBerlinTimeZone[] = "CET-1CEST,M3.5.0/2,M10.5.0/3";
constexpr unsigned long kTimeReadTimeoutMs = 10000;
constexpr unsigned long kWifiConnectTimeoutMs = 15000;
constexpr unsigned long kNtpRetryIntervalMs = 30000;
constexpr unsigned long kPrintIntervalMs = 1000;
constexpr uint8_t kSpiCsPin = 5;
constexpr uint8_t kSpiMosiPin = 23;
constexpr uint8_t kSpiClkPin = 18;
constexpr uint8_t kSpiMisoPin = 19;
constexpr unsigned int kSpiHalfClockDelayUs = 500;
constexpr unsigned long kSpiCsSetupDelayUs = 20000;
constexpr size_t kSpiTimeFrameSize = 32;
constexpr size_t kSpiTimeTextLength = 19;
constexpr char kSpiStatusPollMarker[] = "STATUS?";
constexpr size_t kSpiStatusPollMarkerLength = sizeof(kSpiStatusPollMarker) - 1;
constexpr unsigned long kSpiReplyPollDelayMs = 30;
constexpr int kMinValidYear = 2024;
constexpr size_t kSerialCommandBufferSize = 64;
constexpr unsigned long kSpiTestStepDelayMs = 250;
constexpr uint32_t kInvalidMinuteStamp = 0xFFFFFFFFUL;
constexpr char kTestPrefix[] = "test ";
constexpr size_t kTestPrefixLength = sizeof(kTestPrefix) - 1;
constexpr char kSendPrefix[] = "send ";
constexpr size_t kSendPrefixLength = sizeof(kSendPrefix) - 1;

const char *kNtpServers[] = {
    "fritz.box",
    "0.europe.pool.ntp.org",
    "1.europe.pool.ntp.org",
    "0.pool.ntp.org",
    "1.pool.ntp.org"
};

const size_t kNtpServerCount = sizeof(kNtpServers) / sizeof(kNtpServers[0]);

const char *activeNtpServer = nullptr;
bool timeSynced = false;
unsigned long lastNtpSyncAttemptMs = 0;
unsigned long lastPrintMs = 0;
uint32_t lastTransferredMinuteStamp = kInvalidMinuteStamp;

struct SpiTimestampTestCase
{
    const char *name;
    const char *timestamp;
};

const SpiTimestampTestCase kSpiTimestampTests[] =  {
    {"winter", "2026-01-15 12:00:00"},
    {"summer", "2026-07-15 12:00:00"},
    {"dst-start-before", "2026-03-29 01:59:59"},
    {"dst-start-at", "2026-03-29 02:00:00"},
    {"dst-end-before", "2026-10-25 02:59:59"},
    {"dst-end-at", "2026-10-25 03:00:00"},
    {"invalid-date", "2026-02-30 12:00:00"},
    {"invalid-format", "2026/01/15 12:00:00"},
};

const size_t kSpiTimestampTestCount = sizeof(kSpiTimestampTests) / sizeof(kSpiTimestampTests[0]);
}

/**
 * Converts a raw SPI acknowledgement byte into a stable human-readable label.
 *
 * This function is purely diagnostic. The wire protocol between ESP32 and
 * Teensy remains byte-oriented; the returned strings are only used for serial
 * monitor output and GUI parsing. Unknown values intentionally map to
 * `custom` so unexpected protocol states remain visible during debugging.
 */
const char *spiReplyLabel(uint8_t replyCode)
{
    switch (replyCode)
    {
        case 0x01:
            return "accepted";
        case 0x02:
            return "parse-error";
        case 0x03:
            return "rtc-write-failed";
        case 0x00:
            return "idle";
        default:
            return "custom";
    }
}

uint8_t transferFrameToTeensy(const uint8_t *frame, size_t frameSize);
uint8_t pollTeensyReply();

/**
 * Writes a byte as two uppercase hexadecimal digits to the serial monitor.
 *
 * The implementation avoids temporary formatting buffers and `snprintf()` so
 * the diagnostic path stays lightweight and deterministic.
 */
void printHexByte(uint8_t value)
{
    static constexpr char kHexDigits[] = "0123456789ABCDEF";
    Serial.print(kHexDigits[(value >> 4) & 0x0FU]);
    Serial.print(kHexDigits[value & 0x0FU]);
}

/**
 * Copies ASCII text into a frame buffer up to the smaller of:
 * - the physical frame size
 * - the protocol payload limit
 * - the first input null terminator
 *
 * The caller supplies a buffer that is already zero-initialized, so this
 * helper only needs to write the leading payload bytes.
 */
size_t copyBoundedTextToFrame(uint8_t *frame, size_t frameSize, const char *text, size_t textLimit)
{
    if (frame == nullptr || text == nullptr || frameSize == 0)
    {
        return 0;
    }

    const size_t copyLimit = (textLimit < frameSize) ? textLimit : frameSize;
    size_t copied = 0;

    while (copied < copyLimit && text[copied] != '\0')
    {
        frame[copied] = static_cast<uint8_t>(text[copied]);
        ++copied;
    }

    return copied;
}

/**
 * Transfers one byte over the manually clocked SPI connection and reconstructs
 * the simultaneous reply byte sampled on MISO.
 *
 * Assumptions:
 * - CS is already active
 * - the pins were configured by `beginTeensySpi()`
 * - the configured clock delay is slow enough for the Teensy's software slave
 */
inline uint8_t transferByteToTeensy(uint8_t transmitByte)
{
    uint8_t receivedByte = 0;

    for (uint8_t bitIndex = 0; bitIndex < 8; ++bitIndex)
    {
        digitalWrite(kSpiClkPin, LOW);
        digitalWrite(kSpiMosiPin, (transmitByte & 0x80U) != 0 ? HIGH : LOW);
        delayMicroseconds(kSpiHalfClockDelayUs);

        transmitByte <<= 1;
        digitalWrite(kSpiClkPin, HIGH);
        receivedByte = static_cast<uint8_t>(
            (receivedByte << 1) | (digitalRead(kSpiMisoPin) == HIGH ? 1U : 0U));

        delayMicroseconds(kSpiHalfClockDelayUs);
    }

    return receivedByte;
}

/**
 * Configures the GPIO pins for the ESP32 side of the custom SPI link and
 * prints the resulting wiring map for manual verification.
 */
void beginTeensySpi()
{
    pinMode(kSpiCsPin, OUTPUT);
    pinMode(kSpiClkPin, OUTPUT);
    pinMode(kSpiMosiPin, OUTPUT);
    pinMode(kSpiMisoPin, INPUT);
    digitalWrite(kSpiCsPin, HIGH);
    digitalWrite(kSpiClkPin, LOW);
    digitalWrite(kSpiMosiPin, LOW);

    Serial.print("SPI ready: CS=");
    Serial.print(kSpiCsPin);
    Serial.print(" MOSI=");
    Serial.print(kSpiMosiPin);
    Serial.print(" MISO=");
    Serial.print(kSpiMisoPin);
    Serial.print(" CLK=");
    Serial.println(kSpiClkPin);
}

/**
 * Sends one timestamp payload to the Teensy using the fixed 32-byte frame
 * layout, then polls the Teensy's latched reply code.
 *
 * The returned byte is the status-poll result, not merely the last byte seen
 * during the payload transfer.
 */
uint8_t sendTimeToTeensy(const char *timestamp)
{
    uint8_t frame[kSpiTimeFrameSize] = {};
    copyBoundedTextToFrame(frame, sizeof(frame), timestamp, kSpiTimeTextLength);
    frame[kSpiTimeTextLength] = '\0';

    transferFrameToTeensy(frame, kSpiTimeFrameSize);
    delay(kSpiReplyPollDelayMs);
    return pollTeensyReply();
}

/**
 * Transfers a complete frame with manual chip-select handling.
 *
 * The function returns the last byte sampled from MISO during that transfer.
 * Callers interpret that byte according to context.
 */
uint8_t transferFrameToTeensy(const uint8_t *frame, size_t frameSize)
{
    if (frame == nullptr || frameSize == 0)
    {
        return 0x00;
    }

    uint8_t lastReceivedByte = 0;
    digitalWrite(kSpiCsPin, LOW);
    delayMicroseconds(kSpiCsSetupDelayUs);

    for (size_t byteIndex = 0; byteIndex < frameSize; ++byteIndex)
    {
        lastReceivedByte = transferByteToTeensy(frame[byteIndex]);
    }

    digitalWrite(kSpiClkPin, LOW);
    digitalWrite(kSpiCsPin, HIGH);

    return lastReceivedByte;
}

/**
 * Sends the dedicated `STATUS?` poll frame and returns the Teensy's currently
 * latched one-byte status code.
 */
uint8_t pollTeensyReply()
{
    uint8_t pollFrame[kSpiTimeFrameSize] = {};
    memcpy(pollFrame, kSpiStatusPollMarker, kSpiStatusPollMarkerLength);
    pollFrame[kSpiStatusPollMarkerLength] = '\0';
    return transferFrameToTeensy(pollFrame, kSpiTimeFrameSize);
}

/**
 * Prints one SPI reply byte in the canonical debug format used throughout the
 * project and by the GUI tooling.
 */
void printSpiReply(uint8_t spiReply)
{
    Serial.print("SPI reply: 0x");
    printHexByte(spiReply);
    Serial.print(' ');
    Serial.println(spiReplyLabel(spiReply));
}

/**
 * Prints a labeled one-line transfer summary that includes the payload and the
 * final acknowledgement code.
 */
void printSpiTransferSummary(const char *label, const char *payload, uint8_t spiReply)
{
    Serial.print("SPI TX");
    if (label != nullptr && label[0] != '\0')
    {
        Serial.print(" [");
        Serial.print(label);
        Serial.print(']');
    }

    if (payload != nullptr)
    {
        Serial.print(": ");
        Serial.print(payload);
    }

    Serial.print(" | ACK=0x");
    printHexByte(spiReply);
    Serial.print(' ');
    Serial.println(spiReplyLabel(spiReply));
}

/**
 * Executes one predefined timestamp transfer test and prints both the generic
 * reply line and the labeled transfer summary.
 */
void sendTimestampTestCase(const char *label, const char *timestamp)
{
    const uint8_t spiReply = sendTimeToTeensy(timestamp);
    printSpiReply(spiReply);
    printSpiTransferSummary(label, timestamp, spiReply);
}

/**
 * Sends a deliberately malformed frame with an invalid terminator byte so the
 * Teensy's negative-path parser behavior can be checked.
 */
void sendInvalidTerminatorFrame()
{
    uint8_t frame[kSpiTimeFrameSize] = {};
    const char *timestamp = "2026-01-15 12:00:00";

    copyBoundedTextToFrame(frame, sizeof(frame), timestamp, kSpiTimeTextLength);
    frame[kSpiTimeTextLength] = 'X';

    transferFrameToTeensy(frame, sizeof(frame));
    delay(kSpiReplyPollDelayMs);
    const uint8_t spiReply = pollTeensyReply();
    printSpiReply(spiReply);
    printSpiTransferSummary("invalid-terminator", "2026-01-15 12:00:00 + bad byte[19]", spiReply);
}

/**
 * Prints the complete command reference understood by the ESP32 monitor
 * interface.
 */
void printSpiTestHelp()
{
    Serial.println("Commands:");
    Serial.println("  help         - show commands");
    Serial.println("  now          - send current NTP/local time once");
    Serial.println("  test         - run all canned SPI tests");
    Serial.println("  test <name>  - run one canned test");
    Serial.println("  invalid      - send bad frame terminator test");
    Serial.println("  send <stamp> - send explicit YYYY-MM-DD HH:MM:SS");
    Serial.println("Tests: winter, summer, dst-start-before, dst-start-at, dst-end-before, dst-end-at, invalid-date, invalid-format");
}

/**
 * Looks up and executes one canned timestamp test by symbolic name.
 *
 * Returns `true` when a matching test exists, otherwise `false`.
 */
bool runNamedTimestampTest(const char *testName)
{
    for (size_t i = 0; i < kSpiTimestampTestCount; ++i)
    {
        if (strcmp(testName, kSpiTimestampTests[i].name) == 0)
        {
            sendTimestampTestCase(kSpiTimestampTests[i].name, kSpiTimestampTests[i].timestamp);
            return true;
        }
    }

    return false;
}

/**
 * Executes the complete SPI regression suite, including positive and negative
 * protocol cases.
 */
void runSpiTestSuite()
{
    Serial.println("SPI test suite start");

    for (size_t i = 0; i < kSpiTimestampTestCount; ++i)
    {
        sendTimestampTestCase(kSpiTimestampTests[i].name, kSpiTimestampTests[i].timestamp);
        delay(kSpiTestStepDelayMs);
    }

    sendInvalidTerminatorFrame();
    Serial.println("SPI test suite end");
}

/**
 * Connects the ESP32 to Wi-Fi with a bounded timeout.
 *
 * The timeout is intentional so network failure does not block the entire
 * firmware indefinitely. Higher-level retry policy is handled elsewhere.
 */
bool connectToWifi()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        return true;
    }

    Serial.print("WiFi connect: ");
    Serial.println(ssid);

    WiFi.begin(ssid, password);
    const unsigned long connectStartMs = millis();

    while (WiFi.status() != WL_CONNECTED)
    {
        if (millis() - connectStartMs >= kWifiConnectTimeoutMs)
        {
            Serial.println("WiFi connect timeout");
            WiFi.disconnect(true);
            return false;
        }

        delay(500);
    }

    Serial.print("WiFi OK: ");
    Serial.println(WiFi.localIP());
    return true;
}

/**
 * Disconnects from Wi-Fi and powers the radio down to minimize idle activity
 * after time sync has completed.
 */
void disconnectWifi()
{
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

/**
 * Performs coarse plausibility checks on a `tm` structure before the rest of
 * the program formats or transfers it.
 */
bool isTimePlausible(const tm &timeinfo)
{
    const int year = timeinfo.tm_year + 1900;

    return year >= kMinValidYear &&
           timeinfo.tm_mon >= 0 && timeinfo.tm_mon <= 11 &&
           timeinfo.tm_mday >= 1 && timeinfo.tm_mday <= 31 &&
           timeinfo.tm_hour >= 0 && timeinfo.tm_hour <= 23 &&
           timeinfo.tm_min >= 0 && timeinfo.tm_min <= 59 &&
           timeinfo.tm_sec >= 0 && timeinfo.tm_sec <= 59;
}

/**
 * Reads local civil time from the ESP32 time subsystem and validates the
 * returned structure before handing it to callers.
 */
bool readValidatedLocalTime(tm &timeinfo)
{
    if (!getLocalTime(&timeinfo, kTimeReadTimeoutMs))
    {
        Serial.println("NTP read failed");
        return false;
    }

    if (!isTimePlausible(timeinfo))
    {
        Serial.println("NTP time invalid");
        return false;
    }

    return true;
}

/**
 * Configures one NTP server together with the Berlin time zone rule set and
 * accepts the server only if a valid local time sample is returned.
 */
bool syncTimeFromServer(const char *server)
{
    tm timeinfo;

    Serial.print("NTP try: ");
    Serial.println(server);

    configTzTime(kBerlinTimeZone, server);

    if (!readValidatedLocalTime(timeinfo))
    {
        return false;
    }

    activeNtpServer = server;
    Serial.print("NTP OK: ");
    Serial.println(activeNtpServer);
    return true;
}

/**
 * Tries the configured list of NTP servers until one succeeds.
 */
bool syncTimeFromNtpServers()
{
    for (size_t i = 0; i < kNtpServerCount; ++i)
    {
        if (syncTimeFromServer(kNtpServers[i]))
        {
            return true;
        }
    }

    Serial.print("NTP failed, retry in ");
    Serial.print(kNtpRetryIntervalMs / 1000);
    Serial.println("s");
    return false;
}

/**
 * Formats a `tm` structure into the exact ASCII timestamp syntax expected by
 * the Teensy parser.
 */
void formatTransferTimestamp(const tm &timeinfo, char *buffer, size_t bufferSize)
{
    strftime(buffer, bufferSize, "%Y-%m-%d %H:%M:%S", &timeinfo);
}

/**
 * Sends one local timestamp to the Teensy and prints the resulting transfer
 * diagnostics.
 *
 * Returns `true` only when the Teensy acknowledges the update with `0x01`.
 */
bool printTransferTime(const tm &timeinfo)
{
    char transferBuffer[20];
    formatTransferTimestamp(timeinfo, transferBuffer, sizeof(transferBuffer));
    const uint8_t spiReply = sendTimeToTeensy(transferBuffer);
    printSpiReply(spiReply);

    Serial.print("TIME_TX: ");
    Serial.print(transferBuffer);
    Serial.print(" | SPI CS=");
    Serial.print(kSpiCsPin);
    Serial.print(" | ACK=0x");
    printHexByte(spiReply);
    Serial.print(' ');
    Serial.print(spiReplyLabel(spiReply));

    if (activeNtpServer != nullptr)
    {
        Serial.print(" | SRC: ");
        Serial.println(activeNtpServer);
        return spiReply == 0x01;
    }

    Serial.println();
    return spiReply == 0x01;
}

/**
 * Prints the current local time fields in a compact diagnostic line for manual
 * runtime monitoring.
 */
void printCompactTimeInfo(const tm &timeinfo)
{
    Serial.printf(
        "TIME_INFO: Y=%04d MO=%02d D=%02d WD=%d H=%02d MIN=%02d S=%02d DST=%d\n",
        timeinfo.tm_year + 1900,
        timeinfo.tm_mon + 1,
        timeinfo.tm_mday,
        timeinfo.tm_wday,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec,
        timeinfo.tm_isdst);
}

/**
 * Packs year, month, day, hour, and minute into a compact minute-level change
 * marker used by the transfer scheduler.
 */
uint32_t buildMinuteStamp(const tm &timeinfo)
{
    const uint32_t year = static_cast<uint32_t>(timeinfo.tm_year + 1900 - 2000);
    const uint32_t month = static_cast<uint32_t>(timeinfo.tm_mon + 1);
    const uint32_t day = static_cast<uint32_t>(timeinfo.tm_mday);
    const uint32_t hour = static_cast<uint32_t>(timeinfo.tm_hour);
    const uint32_t minute = static_cast<uint32_t>(timeinfo.tm_min);

    return (year << 20) | (month << 16) | (day << 11) | (hour << 6) | minute;
}

/**
 * Decides whether the current minute should be transferred to the Teensy.
 *
 * Forced calls bypass duplicate suppression so manual actions still trigger an
 * immediate send.
 */
bool shouldTransferCurrentMinute(uint32_t currentMinuteStamp, bool forceTransfer)
{
    if (!forceTransfer && currentMinuteStamp == lastTransferredMinuteStamp)
    {
        return false;
    }

    return true;
}

/**
 * Centralized time-service path shared by startup, retry handling, and manual
 * commands.
 *
 * This function owns:
 * - reading validated local time
 * - minute-change detection
 * - optional transfer to the Teensy
 * - updating the last-successfully-transferred minute marker
 * - printing compact diagnostics
 */
void serviceCurrentTime(bool forceTransfer = false)
{
    tm timeinfo;
    if (!readValidatedLocalTime(timeinfo))
    {
        return;
    }

    const uint32_t currentMinuteStamp = buildMinuteStamp(timeinfo);

    if (shouldTransferCurrentMinute(currentMinuteStamp, forceTransfer))
    {
        if (printTransferTime(timeinfo))
        {
            lastTransferredMinuteStamp = currentMinuteStamp;
        }
    }

    printCompactTimeInfo(timeinfo);
}

/**
 * Discards the remainder of the current serial command line after a command
 * overflow so trailing bytes cannot be misinterpreted as a new command.
 */
void discardSerialCommandLine()
{
    while (Serial.available() > 0)
    {
        const char c = static_cast<char>(Serial.read());
        if (c == '\r' || c == '\n')
        {
            break;
        }
    }
}

/**
 * Reads one complete non-empty command line from the serial monitor into the
 * provided buffer.
 */
bool readSerialCommand(char *buffer, size_t bufferSize)
{
    static size_t index = 0;

    while (Serial.available() > 0)
    {
        const char c = static_cast<char>(Serial.read());

        if (c == '\r')
        {
            continue;
        }

        if (c == '\n')
        {
            buffer[index] = '\0';
            index = 0;
            return buffer[0] != '\0';
        }

        if (index < bufferSize - 1)
        {
            buffer[index++] = c;
        }
        else
        {
            index = 0;
            discardSerialCommandLine();
            Serial.println("Command too long");
            return false;
        }
    }

    return false;
}

/**
 * Dispatches interactive commands entered on the ESP32 monitor interface.
 */
void handleSerialCommands()
{
    char command[kSerialCommandBufferSize] = {};

    if (!readSerialCommand(command, sizeof(command)))
    {
        return;
    }

    if (strcmp(command, "help") == 0)
    {
        printSpiTestHelp();
        return;
    }

    if (strcmp(command, "test") == 0)
    {
        runSpiTestSuite();
        return;
    }

    if (strcmp(command, "invalid") == 0)
    {
        sendInvalidTerminatorFrame();
        return;
    }

    if (strcmp(command, "now") == 0)
    {
        serviceCurrentTime(true);
        return;
    }

    if (strncmp(command, kTestPrefix, kTestPrefixLength) == 0)
    {
        if (!runNamedTimestampTest(command + kTestPrefixLength))
        {
            Serial.print("Unknown test: ");
            Serial.println(command + kTestPrefixLength);
        }
        return;
    }

    if (strncmp(command, kSendPrefix, kSendPrefixLength) == 0)
    {
        sendTimestampTestCase("manual", command + kSendPrefixLength);
        return;
    }

    Serial.print("Unknown command: ");
    Serial.println(command);
    printSpiTestHelp();
}

/**
 * Drives the Wi-Fi/NTP retry state machine until synchronization succeeds.
 */
void handleTimeSync(unsigned long now)
{
    if (timeSynced)
    {
        return;
    }

    if (now - lastNtpSyncAttemptMs < kNtpRetryIntervalMs)
    {
        return;
    }

    lastNtpSyncAttemptMs = now;

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("WiFi reconnect...");
        if (!connectToWifi())
        {
            return;
        }
    }

    Serial.println("NTP retry...");
    timeSynced = syncTimeFromNtpServers();

    if (timeSynced)
    {
        serviceCurrentTime(true);
        disconnectWifi();
    }
}

/**
 * Initializes serial output, SPI wiring, and the first Wi-Fi/NTP sync attempt.
 */
void setup()
{
    Serial.begin(115200);
    beginTeensySpi();
    printSpiTestHelp();

    lastNtpSyncAttemptMs = millis() - kNtpRetryIntervalMs;
    if (connectToWifi())
    {
        timeSynced = syncTimeFromNtpServers();
    }

    if (timeSynced)
    {
        serviceCurrentTime(true);
        disconnectWifi();
    }
}

/**
 * Main runtime loop for interactive commands, sync retry handling, and regular
 * minute-based time transfer policy.
 */
void loop()
{
    const unsigned long now = millis();

    handleSerialCommands();

    handleTimeSync(now);

    if (!timeSynced)
    {
        delay(100);
        return;
    }

    if (now - lastPrintMs >= kPrintIntervalMs)
    {
        lastPrintMs = now;
        serviceCurrentTime(false);
    }
}
