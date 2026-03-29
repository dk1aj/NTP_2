/*
  Rui Santos
  Complete project details at https://RandomNerdTutorials.com/esp32-date-time-ntp-client-server-arduino/

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files.

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.
*/

#include <WiFi.h>
#include "time.h"
#include "credential.h"

namespace
{
const long kGmtOffsetSeconds = 0;
const int kDaylightOffsetSeconds = 3600;
constexpr unsigned long kTimeReadTimeoutMs = 10000;
constexpr unsigned long kNtpRetryIntervalMs = 30000;
constexpr unsigned long kPrintIntervalMs = 1000;
constexpr uint32_t kTeensyUartBaud = 115200;
constexpr int kTeensyRxPin = 16;
constexpr int kTeensyTxPin = 17;
constexpr int kMinValidYear = 2024;

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
}

void beginTeensyUart()
{
    Serial2.begin(kTeensyUartBaud, SERIAL_8N1, kTeensyRxPin, kTeensyTxPin);
    Serial.print("UART2 ready: TX=");
    Serial.print(kTeensyTxPin);
    Serial.print(" RX=");
    Serial.print(kTeensyRxPin);
    Serial.print(" baud=");
    Serial.println(kTeensyUartBaud);
}

void sendTimeToTeensy(const char *timestamp)
{
    Serial2.print(timestamp);
    Serial2.print("\r\n");
}

bool connectToWifi()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        return true;
    }

    Serial.print("WiFi connect: ");
    Serial.println(ssid);

    WiFi.begin(ssid, password);

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
    }

    Serial.print("WiFi OK: ");
    Serial.println(WiFi.localIP());
    return true;
}

void disconnectWifi()
{
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

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

bool syncTimeFromServer(const char *server)
{
    tm timeinfo;

    Serial.print("NTP try: ");
    Serial.println(server);

    configTime(kGmtOffsetSeconds, kDaylightOffsetSeconds, server);

    if (!readValidatedLocalTime(timeinfo))
    {
        return false;
    }

    activeNtpServer = server;
    Serial.print("NTP OK: ");
    Serial.println(activeNtpServer);
    return true;
}

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

void formatTransferTimestamp(const tm &timeinfo, char *buffer, size_t bufferSize)
{
    strftime(buffer, bufferSize, "%Y-%m-%d %H:%M:%S", &timeinfo);
}

void printTransferTime(const tm &timeinfo)
{
    char transferBuffer[20];
    formatTransferTimestamp(timeinfo, transferBuffer, sizeof(transferBuffer));
    sendTimeToTeensy(transferBuffer);

    Serial.print("TIME_TX: ");
    Serial.print(transferBuffer);

    if (activeNtpServer != nullptr)
    {
        Serial.print(" | SRC: ");
        Serial.println(activeNtpServer);
        return;
    }

    Serial.println();
}

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

void printLocalTime()
{
    tm timeinfo;
    if (!readValidatedLocalTime(timeinfo))
    {
        return;
    }

    printTransferTime(timeinfo);
    printCompactTimeInfo(timeinfo);
}

void handleTimeSync(unsigned long now)
{
    if (timeSynced)
    {
        return;
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        return;
    }

    if (now - lastNtpSyncAttemptMs < kNtpRetryIntervalMs)
    {
        return;
    }

    lastNtpSyncAttemptMs = now;
    Serial.println("NTP retry...");
    timeSynced = syncTimeFromNtpServers();

    if (timeSynced)
    {
        printLocalTime();
        disconnectWifi();
    }
}

void setup()
{
    Serial.begin(115200);
    beginTeensyUart();

    connectToWifi();

    lastNtpSyncAttemptMs = millis() - kNtpRetryIntervalMs;
    timeSynced = syncTimeFromNtpServers();

    if (timeSynced)
    {
        printLocalTime();
        disconnectWifi();
    }
}

void loop()
{
    const unsigned long now = millis();

    handleTimeSync(now);

    if (!timeSynced)
    {
        delay(100);
        return;
    }

    if (now - lastPrintMs >= kPrintIntervalMs)
    {
        lastPrintMs = now;
        printLocalTime();
    }
}
