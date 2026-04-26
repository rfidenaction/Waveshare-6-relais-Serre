// Web/Pages/PageLogs.cpp
// Portage Waveshare ESP32-S3-Relay-6CH
// Changements :
//  - Suppression paramètre gsmActive et tout le bloc warning GSM
//  - Suppression variable JS gsmActive et garde GSM dans downloadLogs()
//  - Suppression downloadDisabled (plus de blocage download)
//  - Bloc d'info stats remplacé par "État de la flash" (programme + données)
//    via FlashUsageStats. Les blocs Téléchargement et Suppression, ainsi que
//    tout le JavaScript et le CSS, sont strictement préservés.
//  - rawSizeJs : utilise désormais stats.datalogFileBytes (taille du seul
//    fichier /datalog.csv) pour conserver le comportement exact de la barre
//    de progression du téléchargement.
#include "Web/Pages/PageLogs.h"

String PageLogs::getHtml(const FlashUsageStats& stats)
{
    String statsInfo = "";

    if (stats.mounted) {
        constexpr float MB = 1024.0f * 1024.0f;

        // Arrondi entier mathématique cohérent avec l'affichage console.
        int appPct = (int)((stats.appUsedBytes * 100ULL + stats.appPartitionBytes / 2)
                           / stats.appPartitionBytes);
        int spPct  = (int)((stats.spiffsUsedBytes * 100ULL + stats.spiffsPartitionBytes / 2)
                           / stats.spiffsPartitionBytes);

        String titleLine =
            "📊 État de la flash (" + String(stats.flashTotalBytes / MB, 2) + " MB)";

        String progLine =
            "Programme : " + String(stats.appUsedBytes / MB, 2) +
            " MB / " + String(stats.appPartitionBytes / MB, 2) +
            " MB partition (" + String(appPct) + "% partition)";

        String dataLine =
            "Données : " + String(stats.spiffsUsedBytes / MB, 2) +
            " MB / " + String(stats.spiffsPartitionBytes / MB, 2) +
            " MB partition (" + String(spPct) + "% partition)";

        // "Fichier existant" déduit de la taille du fichier datalog (champ
        // dédié dans FlashUsageStats, indépendant de spiffsUsedBytes).
        bool   datalogExists = (stats.datalogFileBytes > 0);
        String existsLine    = String("Fichier existant : ") + (datalogExists ? "Oui" : "Non");

        statsInfo =
            "<div class=\"card\">"
            "<p style=\"font-size: 1.3em;\">" + titleLine + "</p>"
            "<p class=\"subtext\">" + progLine + "</p>"
            "<p class=\"subtext\">" + dataLine + "</p>"
            "<p style=\"font-size: 0.9em;\">" + existsLine + "</p>"
            "</div>";
    } else {
        constexpr float MB = 1024.0f * 1024.0f;
        String availableSpace =
            "Espace disponible : " + String(stats.spiffsPartitionBytes / MB, 2) + " MB";

        statsInfo =
            "<div class=\"card\">"
            "<p style=\"font-size: 1.3em;\">📊 État de la flash</p>"
            "<p class=\"subtext\">⚠️ SPIFFS non disponible</p>"
            "<p style=\"font-size: 0.9em;\">Fichier existant : Non</p>"
            "<p style=\"font-size: 0.9em;\">" + availableSpace + "</p>"
            "</div>";
    }

    // Taille brute du fichier datalog transmise au JS pour la barre de
    // progression du téléchargement. Champ dédié distinct de spiffsUsedBytes :
    // la barre doit refléter la taille du fichier servi par /logs/download
    // (datalog.csv), pas l'occupation totale SPIFFS.
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

function clearLogs() {
  if (confirm('⚠️ ATTENTION ⚠️\n\nÊtes-vous ABSOLUMENT SÛR de vouloir supprimer TOUTES les données historiques ?\n\nCette action est IRRÉVERSIBLE !')) {
    if (confirm('Dernière confirmation :\n\nToutes les données seront DÉFINITIVEMENT perdues.\n\nContinuer ?')) {
      fetch('/logs/clear', { method: 'POST' })
        .then(response => {
          if (response.ok) {
            alert('✅ Historique supprimé avec succès');
            location.reload();
          } else {
            alert('❌ Erreur lors de la suppression');
          }
        })
        .catch(error => {
          alert('❌ Erreur : ' + error);
        });
    }
  }
}
</script>
</head>
<body>

<h1>🗂️ Gestion des Logs</h1>

)HTML" + statsInfo + R"HTML(

<div class="card">
  <p style="font-size: 1.3em;">Téléchargement des données</p>
  <p class="subtext">Télécharge le bundle (schéma + données brutes) pour analyse PC</p>
  <button id="btn-download" onclick="downloadLogs()">📥 Télécharger les données</button>
  <div id="progress-bar-wrap"><div id="progress-bar"></div></div>
  <div id="download-status"></div>
</div>

<div class="card">
  <p style="font-size: 1.3em;">Suppression des données</p>
  <p class="subtext">⚠️ DANGER : Supprime définitivement tout l'historique</p>
  <p style="font-size: 0.9em; color: #ffeb3b;">Cette action est IRRÉVERSIBLE</p>
  <button class="danger" onclick="clearLogs()">🗑️ Effacer les données</button>
</div>

<a href="/" class="back-link">← Retour à la page principale</a>

</body>
</html>
)HTML";

    return html;
}