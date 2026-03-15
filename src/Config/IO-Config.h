// Config/IO-Config.h
#pragma once

/*
 * IO-Config
 * 
 * Configuration des GPIO et pins physiques.
 * Carte : Waveshare ESP32-S3-Relay-6CH
 * MCU   : ESP32-S3 (dual-core Xtensa LX7 @ 240MHz)
 * 
 * Source : documentation Waveshare officielle (pinout PDF)
 */

// =============================================================================
// Relais (6 canaux — actif HIGH)
// =============================================================================

#define RELAY_CH1_PIN      1
#define RELAY_CH2_PIN      2
#define RELAY_CH3_PIN      41
#define RELAY_CH4_PIN      42
#define RELAY_CH5_PIN      45
#define RELAY_CH6_PIN      46

// =============================================================================
// RS485 (UART isolé)
// =============================================================================

#define RS485_TX_PIN       17
#define RS485_RX_PIN       18

// =============================================================================
// RTC DS3231 (module Pico-RTC-DS3231 via header Pico HAT, bus I2C)
// =============================================================================

#define RTC_SDA_PIN        4
#define RTC_SCL_PIN        5

// =============================================================================
// Buzzer passif (fréquence contrôlable par PWM)
// =============================================================================

#define BUZZER_PIN         21

// =============================================================================
// LED RGB (WS2812)
// =============================================================================

#define RGB_LED_PIN        38

// =============================================================================
// Bouton BOOT
// =============================================================================

#define BOOT_BUTTON_PIN    0