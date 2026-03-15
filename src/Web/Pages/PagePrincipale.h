// Web/Pages/PagePrincipale.h
#pragma once

#include <Arduino.h>

class PagePrincipale {
public:
    /**
     * Retourne le code HTML complet de la page principale (dashboard)
     */
    static String getHtml();

private:
    static String getUptimeString();
};