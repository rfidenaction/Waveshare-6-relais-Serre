// Web/Pages/PagePrincipale.cpp
// Portage Waveshare ESP32-S3-Relay-6CH
// Changements :
//  - Suppression bloc Batterie (BatteryVoltage, BatteryPercent, Charging)
//  - Suppression bloc Alimentation externe (ExternalPower)
//  - Suppression bloc GSM/Cellular complet
//  - Suppression fonction signalTodBm() (GSM uniquement)
//  - Suppression JS toggleSta() et toggleGsm()
//  - Suppression helper getString() (plus aucun DataId texte affiché)
//  - Carte WiFi STA : sans toggle (STA toujours actif), SSID + IP + état + RSSI
//  - Carte AP : SSID + IP + état + toggle désactivation irréversible
//  - Nouvelle carte Alimentation avec DataId::SupplyVoltage
//  - Graphique adapté : tension alimentation, échelle Y 5–40 V
//  - Logger → Console
#include "Web/Pages/PagePrincipale.h"

#include "Storage/DataLogger.h"
#include "Config/NetworkConfig.h"
#include "Utils/Console.h"

#include <time.h>

// Tag pour logs
static const char* TAG = "PagePrincipale";

// startTime est déclaré dans main.cpp
extern unsigned long startTime;

// ─────────────────────────────────────────────
// Helper sécurisé pour extraire un float du variant
// ─────────────────────────────────────────────

static float getFloat(const LastDataForWeb& d, float defaultValue = 0.0f)
{
    if (std::holds_alternative<float>(d.value)) {
        return std::get<float>(d.value);
    }
    Console::warn(TAG, "Tentative d'extraire float depuis un String!");
    return defaultValue;
}

// ─────────────────────────────────────────────
// Uptime
// ─────────────────────────────────────────────

String PagePrincipale::getUptimeString()
{
    unsigned long secs = (millis() - startTime) / 1000;
    int days = secs / 86400; secs %= 86400;
    int hours = secs / 3600; secs %= 3600;
    int mins = secs / 60; secs %= 60;

    return String(days) + "j " +
           String(hours) + "h " +
           String(mins) + "m " +
           String(secs) + "s";
}

// ─────────────────────────────────────────────
// Helpers temps (UI uniquement)
// ─────────────────────────────────────────────

static String formatUtc(time_t t)
{
    struct tm tmLocal;
    localtime_r(&t, &tmLocal);
    char buf[20];
    strftime(buf, sizeof(buf), "%d/%m/%y %H:%M", &tmLocal);
    return String(buf);
}

static String formatSince(uint32_t ageMs)
{
    uint32_t s = ageMs / 1000;
    uint32_t m = s / 60; s %= 60;
    uint32_t h = m / 60; m %= 60;

    String out = "Depuis ";
    if (h) out += String(h) + "h ";
    if (m) out += String(m) + "m ";
    out += String(s) + "s";
    return out;
}

static String timeHtml(const LastDataForWeb& d)
{
    if (d.UTC_available && d.UTC_reliable) {
        // RTC — heure précise
        return formatUtc(d.timestamp);
    }

    if (d.UTC_available && !d.UTC_reliable) {
        // VClock — heure approximative
        return formatUtc(d.timestamp) + " <em>(Imprécis)</em>";
    }

    // Pas d'UTC — timestamp contient millis(), affichage relatif
    uint32_t ageMs = millis() - static_cast<uint32_t>(d.timestamp);
    return "<span class=\"age\" data-age-ms=\"" +
           String(ageMs) + "\">" +
           formatSince(ageMs) +
           "</span>";
}

// ─────────────────────────────────────────────
// Génération HTML
// ─────────────────────────────────────────────

String PagePrincipale::getHtml()
{
    LastDataForWeb d;

    // ───────── Wi-Fi STA (toujours actif) ─────────
    bool staConnected = false;
    String staTime;

    if (DataLogger::hasLastDataForWeb(DataId::WifiStaConnected, d)) {
        staConnected = getFloat(d) > 0.5f;
        staTime = timeHtml(d);
    }

    int wifiRssi = 0;
    bool hasRssi = DataLogger::hasLastDataForWeb(DataId::WifiRssi, d);
    if (hasRssi) wifiRssi = (int)getFloat(d);

    String staStatus =
        (staConnected && hasRssi) ? "Connecté (" + String(wifiRssi) + " dBm)" :
        staConnected              ? "Connecté" :
                                    "Recherche réseau...";

    String staSsid = WIFI_STA_SSID;
    String staIp   = WIFI_STA_IP.toString();

    // ───────── Wi-Fi AP ─────────
    bool apEnabled = false;
    String apTime;

    if (DataLogger::hasLastDataForWeb(DataId::WifiApEnabled, d)) {
        apEnabled = getFloat(d) > 0.5f;
        apTime = timeHtml(d);
    }

    String apStatus = apEnabled ? "Actif" : "Désactivé";
    String apSsid   = WIFI_AP_SSID;
    String apIp     = WIFI_AP_IP.toString();

    // ───────── Alimentation ─────────
    String supplyLine;
    String supplyTime;
    float supplyVoltage = 0.0f;

    if (DataLogger::hasLastDataForWeb(DataId::SupplyVoltage, d)) {
        supplyVoltage = getFloat(d);
        supplyTime = timeHtml(d);
    }

    supplyLine = String(supplyVoltage, 2) + " V";

    // ───────── HTML ─────────
    String html = R"HTML(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Serre de Marie-Pierre</title>
<style>
body { font-family: Arial; background: #1976d2; color: white; text-align: center; margin: 0; padding: 20px; }
h1 { background: #0d47a1; padding: 20px; border-radius: 10px; }
.card { background: rgba(255,255,255,0.2); margin: 20px auto; max-width: 600px; padding: 20px; border-radius: 15px; }
.card.clickable { cursor: pointer; transition: background 0.3s; }
.card.clickable:hover { background: rgba(255,255,255,0.3); }
.value { font-size: 1.8em; font-weight: bold; }
.subtext { font-size: 1.2em; margin-top: 15px; }
small { font-size: 0.8em; }
.switch { position: relative; display: inline-block; width: 90px; height: 44px; }
.switch input { opacity: 0; width: 0; height: 0; }
.slider { position: absolute; cursor: pointer; inset: 0; background-color: #ccc; transition: .4s; border-radius: 44px; }
.slider:before { position: absolute; content: ""; height: 36px; width: 36px; left: 4px; bottom: 4px; background-color: white; transition: .4s; border-radius: 50%; }
input:checked + .slider { background-color: #0d47a1; }
input:checked + .slider:before { transform: translateX(46px); }
input:disabled + .slider { opacity: 0.5; cursor: default; }
#graphContainer { display: none; margin: 20px auto; max-width: 600px; background: rgba(255,255,255,0.9); padding: 20px; border-radius: 15px; }
#graphContainer canvas { max-width: 100%; }
#graphClose { background: #c62828; color: white; border: none; padding: 10px 20px; border-radius: 5px; cursor: pointer; margin-top: 10px; }
#graphClose:hover { background: #8e0000; }
#graphLoading { color: #333; font-size: 1.2em; }
</style>

<script>
function toggleAp(cb) {
  if (!cb.checked) {
    fetch('/ap-toggle', { method: 'POST', body: new URLSearchParams() });
  }
}

// Graphique tension alimentation
let supplyChart = null;
let chartJsLoaded = false;

// Charge Chart.js depuis le firmware embarqué (pas de CDN nécessaire)
function loadChartJs() {
  return new Promise((resolve, reject) => {
    if (chartJsLoaded) { resolve(); return; }
    const s = document.createElement('script');
    s.src = '/js/chart.min.js';
    s.onload = () => { chartJsLoaded = true; resolve(); };
    s.onerror = () => reject(new Error('Erreur chargement Chart.js'));
    document.head.appendChild(s);
    // Timeout 15s — le fichier est servi en local mais peut être lent sur ESP32
    setTimeout(() => {
      if (!chartJsLoaded) {
        s.remove();
        reject(new Error('Timeout chargement Chart.js'));
      }
    }, 15000);
  });
}

function showSupplyGraph() {
  const container = document.getElementById('graphContainer');
  const loading = document.getElementById('graphLoading');
  const canvas = document.getElementById('supplyChart');

  container.style.display = 'block';
  loading.style.display = 'block';
  loading.textContent = 'Chargement...';
  canvas.style.display = 'none';

  // Charger Chart.js + données en parallèle
  Promise.all([
    loadChartJs(),
    fetch('/graphdata').then(r => r.text())
  ])
    .then(([_, csv]) => {
      // Parser le CSV
      const lines = csv.trim().split('\n');
      const labels = [];
      const values = [];

      for (let i = 1; i < lines.length; i++) {
        const parts = lines[i].split(',');
        if (parts.length >= 2) {
          const timestamp = parseInt(parts[0]);
          const value = parseFloat(parts[1]);

          const date = new Date(timestamp * 1000);
          const label = date.toLocaleString('fr-FR', {
            day: '2-digit',
            month: '2-digit',
            hour: '2-digit',
            minute: '2-digit'
          });

          labels.push(label);
          values.push(value);
        }
      }

      // ── Sous-échantillonnage (~100 points max pour mobile) ──
      const MAX_POINTS = 100;
      let plotLabels = labels;
      let plotValues = values;
      if (values.length > MAX_POINTS) {
        const step = Math.ceil(values.length / MAX_POINTS);
        plotLabels = [];
        plotValues = [];
        for (let i = 0; i < values.length; i += step) {
          plotLabels.push(labels[i]);
          plotValues.push(values[i]);
        }
      }

      loading.style.display = 'none';
      canvas.style.display = 'block';

      // Détruire l'ancien graphique si existe
      if (supplyChart) {
        supplyChart.destroy();
      }

      // Créer le graphique
      const ctx = canvas.getContext('2d');
      supplyChart = new Chart(ctx, {
        type: 'line',
        data: {
          labels: plotLabels,
          datasets: [{
            label: 'Tension alimentation (V)',
            data: plotValues,
            borderColor: '#1976d2',
            backgroundColor: 'rgba(25, 118, 210, 0.1)',
            fill: true,
            tension: 0.3
          }]
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          plugins: {
            title: {
              display: true,
              text: 'Historique tension alimentation (30 derniers jours)',
              color: '#333'
            },
            legend: {
              labels: { color: '#333' }
            }
          },
          scales: {
            x: {
              ticks: {
                color: '#333',
                maxTicksLimit: 10
              }
            },
            y: {
              ticks: { color: '#333' },
              suggestedMin: 5,
              suggestedMax: 40
            }
          }
        }
      });
    })
    .catch(error => {
      loading.style.display = 'block';
      canvas.style.display = 'none';
      loading.textContent = 'Graphique indisponible : ' + error.message;
    });
}

function hideGraph() {
  document.getElementById('graphContainer').style.display = 'none';
}

setInterval(() => {
  document.querySelectorAll('.age').forEach(e => {
    let ms = parseInt(e.dataset.ageMs);
    ms += 1000;
    e.dataset.ageMs = ms;

    let s = Math.floor(ms / 1000);
    let m = Math.floor(s / 60); s %= 60;
    let h = Math.floor(m / 60); m %= 60;

    e.textContent = 'Depuis ' +
      (h ? h + 'h ' : '') +
      (m ? m + 'm ' : '') +
      s + 's';
  });
}, 1000);

// rafraîchissement périodique
setInterval(() => {
  location.reload();
}, 30000);
</script>
</head>
<body>

<h1>Serre de Marie-Pierre</h1>

<div class="card">
  <p>WIFI</p>
  <p class="value">)HTML" + staStatus + R"HTML(</p>
  <p class="subtext">SSID : )HTML" + staSsid + R"HTML(<br>IP : )HTML" + staIp + R"HTML(</p>
  <p><small>)HTML" + staTime + R"HTML(</small></p>
</div>

<div class="card">
  <p>ACCES LOCAL</p>
  <p class="value">)HTML" + apStatus + R"HTML(</p>
  <p class="subtext">SSID : )HTML" + apSsid + R"HTML(<br>IP : )HTML" + apIp + R"HTML(</p>
  <p><small>)HTML" + apTime + R"HTML(</small></p>
  <label class="switch">
    <input type="checkbox"
           )HTML" + String(apEnabled ? "checked" : "") + R"HTML(
           )HTML" + String(apEnabled ? "" : "disabled") + R"HTML(
           onchange="toggleAp(this)">
    <span class="slider"></span>
  </label>
</div>

<div class="card clickable" onclick="showSupplyGraph()">
  <p>Alimentation <small>(cliquez pour le graphique)</small></p>
  <p class="value">)HTML" + supplyLine + R"HTML(</p>
  <p><small>)HTML" + supplyTime + R"HTML(</small></p>
</div>

<div id="graphContainer">
  <p id="graphLoading">Chargement des données...</p>
  <div style="position:relative; height:250px; width:100%;">
    <canvas id="supplyChart"></canvas>
  </div>
  <button id="graphClose" onclick="hideGraph()">Fermer</button>
</div>

<div class="card">
  <p>Durée de fonctionnement</p>
  <p class="value">)HTML" + getUptimeString() + R"HTML(</p>
</div>

<div class="card" style="margin-top: 40px;">
  <a href="/logs" style="color: white; text-decoration: none; display: block;">
    <p style="font-size: 1.2em;">🗂️ Gestion des Logs</p>
    <p style="font-size: 0.9em;">Télécharger ou supprimer les données</p>
  </a>
</div>

</body>
</html>
)HTML";

    return html;
}