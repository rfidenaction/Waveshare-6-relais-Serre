// Utils/Console.cpp
// Renommage Logger → Console (portage Waveshare)
// Sortie via printf (UART0/CH343 sur Waveshare ESP32-S3-Relay-6CH)
#include "Console.h"

Console::Level Console::_currentLevel = Console::Level::INFO;

void Console::begin(Level level) {
    _currentLevel = level;
}

void Console::setLevel(Level level) {
    _currentLevel = level;
}

Console::Level Console::getLevel() {
    return _currentLevel;
}

// ---------- API publique sans tag ----------

void Console::error(const String& message) { log(Level::ERROR, "", message); }
void Console::warn (const String& message) { log(Level::WARN,  "", message); }
void Console::info (const String& message) { log(Level::INFO,  "", message); }
void Console::debug(const String& message) { log(Level::DEBUG, "", message); }
void Console::trace(const String& message) { log(Level::TRACE, "", message); }

// ---------- API publique avec tag ----------

void Console::error(const String& tag, const String& message) { log(Level::ERROR, tag, message); }
void Console::warn (const String& tag, const String& message) { log(Level::WARN,  tag, message); }
void Console::info (const String& tag, const String& message) { log(Level::INFO,  tag, message); }
void Console::debug(const String& tag, const String& message) { log(Level::DEBUG, tag, message); }
void Console::trace(const String& tag, const String& message) { log(Level::TRACE, tag, message); }

// ---------- Implémentation centrale ----------

void Console::log(Level level, const String& tag, const String& message) {
    if (level > _currentLevel) return;

    unsigned long timestamp = millis();

    if (tag.isEmpty()) {
        printf("[%lu ms] %s %s\n", timestamp, levelToString(level), message.c_str());
    } else {
        printf("[%lu ms] %s [%s] %s\n", timestamp, levelToString(level), tag.c_str(), message.c_str());
    }
}

const char* Console::levelToString(Level level) {
    switch (level) {
        case Level::ERROR: return "ERROR";
        case Level::WARN:  return "WARN ";
        case Level::INFO:  return "INFO ";
        case Level::DEBUG: return "DEBUG";
        case Level::TRACE: return "TRACE";
        default:           return "UNKWN";
    }
}