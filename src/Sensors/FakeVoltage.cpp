// Sensors/FakeVoltage.cpp
// Simulateur de tension d'alimentation (sinusoïde 20-30V)
// À SUPPRIMER en production

#include "Sensors/FakeVoltage.h"
#include "Core/DataBus.h"
#include "Utils/Console.h"
#include <math.h>

static const char* TAG = "FakeVoltage";

uint16_t FakeVoltage::counter = 0;

void FakeVoltage::init()
{
    counter = 0;
    Console::info(TAG, "Simulateur tension initialisé (sinusoïde 20-30V, période 3h)");
}

void FakeVoltage::handle()
{
    float radians = counter * M_PI / 180.0f;
    float voltage = (sin(radians) + 5.0f) * 5.0f;   // 20V — 30V

    BusItem item = {};
    item.type       = getMeta(DataId::SupplyVoltage).type;
    item.id         = DataId::SupplyVoltage;
    item.valueKind  = 0;
    item.valueFloat = voltage;
    DataBus::publish(item);

    Console::debug(TAG, "Tension simulée: " + String(voltage, 2) + "V (angle=" + String(counter) + "°)");

    counter = (counter + 1) % 360;
}