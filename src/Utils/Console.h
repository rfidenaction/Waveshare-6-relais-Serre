// Utils/Console.h
// Renommage Logger → Console (portage Waveshare)
// Évite la confusion avec DataLogger (données applicatives)
// Sortie via printf (UART0/CH343 sur Waveshare ESP32-S3-Relay-6CH)
#pragma once

#include <Arduino.h>

class Console {
public:
    enum class Level : uint8_t {
        ERROR = 0,
        WARN,
        INFO,
        DEBUG,
        TRACE
    };

    // Initialisation globale (plus besoin de Stream sur Waveshare)
    static void begin(Level level = Level::INFO);

    // Configuration
    static void setLevel(Level level);
    static Level getLevel();

    // Méthodes principales
    static void error(const String& message);
    static void warn(const String& message);
    static void info(const String& message);
    static void debug(const String& message);
    static void trace(const String& message);

    // Version avec tag (module)
    static void error(const String& tag, const String& message);
    static void warn (const String& tag, const String& message);
    static void info (const String& tag, const String& message);
    static void debug(const String& tag, const String& message);
    static void trace(const String& tag, const String& message);

private:
    static void log(Level level, const String& tag, const String& message);
    static const char* levelToString(Level level);

    static Level _currentLevel;
};