#pragma once
#include <Arduino.h>
#ifdef WEB_SERVER
extern void sendRobotOutput(String output);
#endif

class SerialWebFork : public Print {
public:
    void begin(unsigned long baud) { Serial.begin(baud); }
    void setTimeout(unsigned long timeout) { Serial.setTimeout(timeout); }
    int available() { return Serial.available(); }
    int read() { return Serial.read(); }
    void flush() { Serial.flush(); }
    size_t write(uint8_t c) override {
        Serial.write(c);
#ifdef WEB_SERVER
        static String webBuf;
        if (c == '\n' || webBuf.length() > 200) {
            if (webBuf.length() > 0) sendRobotOutput(webBuf);
            webBuf = "";
        } else if (c != '\r') {
            webBuf += (char)c;
        }
#endif
        return 1;
    }
    size_t write(const uint8_t *buffer, size_t size) override {
        size_t n = Serial.write(buffer, size);
#ifdef WEB_SERVER
        for (size_t i = 0; i < size; ++i) write(buffer[i]);
#endif
        return n;
    }
    using Print::write;
};

extern SerialWebFork SerialFork;
