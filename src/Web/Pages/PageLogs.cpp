// Web/Pages/PageLogs.cpp
// Portage Waveshare ESP32-S3-Relay-6CH
// Changements :
//  - Suppression paramètre gsmActive et tout le bloc warning GSM
//  - Suppression variable JS gsmActive et garde GSM dans downloadLogs()
//  - Suppression downloadDisabled (plus de blocage download)
//  - Bloc d'info stats remplacé par "État de la flash" (programme + données)
//    via FlashUsageStats.
//  - rawSizeJs utilise stats.datalogFileBytes, cumul de tous les log_*.csv.
#include "Web/Pages/PageLogs.h"

String PageLogs::getHtml(const FlashUsageStats& stats)
{
    String statsInfo = "";

    if (stats.mounted) {
        constexpr float MB = 1024.0f * 1024.0f;

        // Arrondi entier mathématique cohérent avec l'affichage console.
        int appPct = (int)((stats.appUsedBytes * 100ULL + stats.appPartitionBytes / 2)
                           / stats.appPartitionBytes);
        int spPct  = (int)((stats.littlefsUsedBytes * 100ULL + stats.littlefsPartitionBytes / 2)
                           / stats.littlefsPartitionBytes);

        String titleLine =
            "📊 État de la flash (" + String(stats.flashTotalBytes / MB, 2) + " MB)";

        String progLine =
            "Programme : " + String(stats.appUsedBytes / MB, 2) +
            " MB / " + String(stats.appPartitionBytes / MB, 2) +
            " MB partition (" + String(appPct) + "% partition)";

        String dataLine =
            "Données : " + String(stats.littlefsUsedBytes / MB, 2) +
            " MB / " + String(stats.littlefsPartitionBytes / MB, 2) +
            " MB partition (" + String(spPct) + "% partition)";

        statsInfo =
            "<div class=\"card\">"
            "<p style=\"font-size: 1.3em;\">" + titleLine + "</p>"
            "<p class=\"subtext\">" + progLine + "</p>"
            "<p class=\"subtext\">" + dataLine + "</p>"
            "</div>";
    } else {
        constexpr float MB = 1024.0f * 1024.0f;
        String availableSpace =
            "Espace disponible : " + String(stats.littlefsPartitionBytes / MB, 2) + " MB";

        statsInfo =
            "<div class=\"card\">"
            "<p style=\"font-size: 1.3em;\">📊 État de la flash</p>"
            "<p class=\"subtext\">⚠️ LittleFS non disponible</p>"
            "<p style=\"font-size: 0.9em;\">" + availableSpace + "</p>"
            "</div>";
    }

    // Card RAM : pic depuis le boot + utilisation instantanée.
    String ramInfo =
        "<div class=\"card\">"
        "<p style=\"font-size: 1.3em;\">🧠 RAM</p>"
        "<p class=\"subtext\">Pic depuis le boot : " +
        String(stats.ramPeakPercent) + "%</p>"
        "<p class=\"subtext\">Utilisation actuelle : " +
        String(stats.ramCurrentPercent) + "%</p>"
        "</div>";

    // Taille brute cumulée des log_*.csv transmise au JS pour la barre de
    // progression du bundle, distincte de l'occupation totale LittleFS.
    String rawSizeJs = String(stats.datalogFileBytes);

    String html = R"HTML(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Gestion des Logs - Serre de Marie-Pierre</title>
<style>
body { font-family: Arial; background: #d32f2f; color: white; text-align: center; margin: 0; padding: 20px; }
h1 { background: #b71c1c; padding: 20px; border-radius: 10px; }
.card { background: rgba(255,255,255,0.2); margin: 20px auto; max-width: 600px; padding: 20px; border-radius: 15px; }
.subtext { font-size: 1.2em; margin-top: 15px; }
button {
  background: #1976d2;
  color: white;
  border: none;
  padding: 15px 30px;
  font-size: 1.2em;
  border-radius: 10px;
  cursor: pointer;
  margin: 10px;
  min-width: 250px;
}
button:hover:not(:disabled) { background: #0d47a1; }
button:disabled { background: #666; cursor: not-allowed; opacity: 0.5; }
button.danger { background: #c62828; }
button.danger:hover:not(:disabled) { background: #8e0000; }
.back-link {
  display: inline-block;
  margin-top: 30px;
  color: white;
  text-decoration: underline;
  font-size: 1.1em;
}
#download-status {
  margin-top: 10px;
  font-size: 1.0em;
  min-height: 1.4em;
}
#progress-bar-wrap {
  display: none;
  background: rgba(255,255,255,0.2);
  border-radius: 8px;
  height: 18px;
  margin: 10px auto;
  max-width: 400px;
  overflow: hidden;
}
#progress-bar {
  height: 100%;
  width: 0%;
  background: #4caf50;
  border-radius: 8px;
  transition: width 0.3s ease;
}
</style>

<script>
const rawFileSize = )HTML" + rawSizeJs + R"HTML(;

async function downloadLogs() {
  const btn    = document.getElementById('btn-download');
  const status = document.getElementById('download-status');
  const wrap   = document.getElementById('progress-bar-wrap');
  const bar    = document.getElementById('progress-bar');

  btn.disabled = true;
  btn.textContent = '⏳ Téléchargement en cours...';
  status.textContent = 'Connexion à la carte...';
  wrap.style.display = 'block';
  bar.style.width = '0%';

  try {
    const response = await fetch('/logs/download');

    if (!response.ok) {
      const text = await response.text();
      btn.textContent = '📥 Télécharger les données';
      btn.disabled = false;
      wrap.style.display = 'none';
      status.textContent = '❌ Erreur : ' + text;
      return;
    }

    // Lecture streaming avec progression
    const reader = response.body.getReader();
    const chunks = [];
    let received = 0;

    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      chunks.push(value);
      received += value.length;

      // Progression approximative basée sur la taille du fichier brut
      // Le bundle est légèrement plus grand (schéma JSON en tête)
      const pct = rawFileSize > 0
        ? Math.min(99, Math.round((received / (rawFileSize * 1.05)) * 100))
        : 0;
      bar.style.width = pct + '%';
      status.textContent = '📡 ' + (received / 1024).toFixed(1) + ' KB reçus (~' + pct + '%)';
    }

    // Assemblage et déclenchement du téléchargement
    const blob = new Blob(chunks, { type: 'text/plain' });
    const url  = window.URL.createObjectURL(blob);
    const link = document.createElement('a');
    link.href  = url;
    link.download = 'serre_bundle.txt';
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
    window.URL.revokeObjectURL(url);

    bar.style.width = '100%';
    bar.style.background = '#4caf50';
    btn.textContent = '✅ Téléchargement terminé';
    status.textContent = '✅ ' + (received / 1024).toFixed(1) + ' KB téléchargés avec succès.';

  } catch (error) {
    btn.textContent = '📥 Télécharger les données';
    btn.disabled = false;
    wrap.style.display = 'none';
    status.textContent = '❌ Erreur réseau : ' + error;
  }
}

async function clearLogs() {
  if (!confirm('⚠️ ATTENTION ⚠️\n\nSupprimer définitivement toutes les archives clôturées ?\n\nLes données de la période actuellement enregistrée seront conservées.')) return;
  if (!confirm('Dernière confirmation :\n\nLes archives supprimées ne pourront pas être récupérées.\n\nContinuer ?')) return;

  const btn = document.getElementById('btn-clear');
  const status = document.getElementById('clear-status');
  btn.disabled = true;
  btn.textContent = '⏳ Suppression en cours...';
  status.textContent = 'Annulation de la lecture éventuelle puis suppression des archives...';

  try {
    const response = await fetch('/logs/clear', { method: 'POST' });
    if (!response.ok && response.status !== 409) {
      throw new Error(await response.text());
    }

    const deadline = Date.now() + 180000;
    while (Date.now() < deadline) {
      await new Promise(resolve => setTimeout(resolve, 500));
      const stateResponse = await fetch('/logs/clear/status', { cache: 'no-store' });
      if (!stateResponse.ok) throw new Error('état de suppression indisponible');

      const state = (await stateResponse.json()).status;
      if (state === 'success') {
        alert('✅ Archives supprimées avec succès. La période en cours est conservée.');
        location.reload();
        return;
      }
      if (state === 'failed') {
        throw new Error('la carte n’a pas pu supprimer toutes les archives');
      }
    }

    throw new Error('délai de suppression dépassé');
  } catch (error) {
    alert('❌ Erreur : ' + error);
    btn.disabled = false;
    btn.textContent = '🗑️ Effacer les archives';
    status.textContent = '';
  }
}
</script>
</head>
<body>

<h1>🗂️ Gestion des Logs</h1>

)HTML" + statsInfo + ramInfo + R"HTML(

<div class="card">
  <p style="font-size: 1.3em;">Téléchargement des données</p>
  <p class="subtext">Télécharge le bundle (schéma + données brutes) pour analyse PC</p>
  <button id="btn-download" onclick="downloadLogs()">📥 Télécharger les données</button>
  <div id="progress-bar-wrap"><div id="progress-bar"></div></div>
  <div id="download-status"></div>
</div>

<div class="card">
  <p style="font-size: 1.3em;">Suppression des archives</p>
  <p class="subtext">Supprime les fichiers clôturés et conserve la période en cours</p>
  <p style="font-size: 0.9em; color: #ffeb3b;">Les archives supprimées sont irrécupérables</p>
  <button id="btn-clear" class="danger" onclick="clearLogs()">🗑️ Effacer les archives</button>
  <div id="clear-status"></div>
</div>

<a href="/" class="back-link">← Retour à la page principale</a>

</body>
</html>
)HTML";

    return html;
}