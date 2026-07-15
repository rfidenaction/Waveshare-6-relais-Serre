// Web/Pages/PageRS485.cpp
// Page de programmation des adresses RS485 (maintenance capteurs sol)
//
// Workflow :
//   1. L'utilisateur ouvre /rs485, choisit la nouvelle adresse (1-15)
//   2. POST /rs485/setaddr?to=Y → détecte le capteur puis programme
//   3. Bouton "Retour" → désactive le mode maintenance

#include "Web/Pages/PageRS485.h"

String PageRS485::getHtml()
{
    String html = R"HTML(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Programmation RS485</title>
<style>
body { font-family: Arial; background: #1976d2; color: white; text-align: center; margin: 0; padding: 20px; }
h1 { background: #0d47a1; padding: 20px; border-radius: 10px; }
.card { background: rgba(255,255,255,0.2); margin: 20px auto; max-width: 600px; padding: 20px; border-radius: 15px; }
.status { font-size: 1.2em; margin: 15px 0; }
.spinner { display: inline-block; width: 20px; height: 20px; border: 3px solid rgba(255,255,255,0.3); border-top-color: white; border-radius: 50%; animation: spin 0.8s linear infinite; vertical-align: middle; margin-right: 8px; }
@keyframes spin { to { transform: rotate(360deg); } }
select { font-size: 1.2em; padding: 8px 16px; border-radius: 8px; border: none; margin: 10px; }
.btn { display: inline-block; padding: 12px 24px; font-size: 1.1em; font-weight: bold; border: none; border-radius: 8px; cursor: pointer; margin: 8px; text-decoration: none; }
.btn-primary { background: #4caf50; color: white; }
.btn-primary:hover { background: #43a047; }
.btn-primary:disabled { background: #9e9e9e; cursor: not-allowed; }
.btn-back { background: rgba(255,255,255,0.2); color: white; border: 2px solid rgba(255,255,255,0.4); }
.btn-back:hover { background: rgba(255,255,255,0.3); }
.warning { background: rgba(244,67,54,0.3); padding: 12px; border-radius: 8px; margin: 15px 0; }
.success { background: rgba(76,175,80,0.3); padding: 12px; border-radius: 8px; margin: 15px 0; }
</style>
</head>
<body>

<h1>Programmation RS485</h1>

<div class="card">
  <p>Brancher <strong>un seul capteur</strong> sur le bus RS485, puis choisir l'adresse à lui attribuer.</p>

  <p>Nouvelle adresse :</p>
  <select id="newAddr">
    <option value="1">1</option>
    <option value="2">2</option>
    <option value="3">3</option>
    <option value="4">4</option>
    <option value="5">5</option>
    <option value="6">6</option>
    <option value="7">7</option>
    <option value="8">8</option>
    <option value="9">9</option>
    <option value="10">10</option>
    <option value="11">11</option>
    <option value="12">12</option>
    <option value="13">13</option>
    <option value="14">14</option>
    <option value="15">15</option>
  </select>
  <br>
  <button class="btn btn-primary" id="btnProgram" onclick="doProgram()">Programmer</button>
  <div id="programResult"></div>
</div>

<div style="margin-top: 30px;">
  <a href="/" class="btn btn-back" onclick="exitMaintenance()">&#8592; Retour</a>
</div>

<script>
function doProgram() {
  var newAddr = parseInt(document.getElementById('newAddr').value);
  var btn = document.getElementById('btnProgram');
  var resultEl = document.getElementById('programResult');

  btn.disabled = true;
  btn.textContent = 'Recherche du capteur...';
  resultEl.innerHTML = '';

  fetch('/rs485/setaddr?to=' + newAddr, { method: 'POST' })
    .then(function(r) { return r.json(); })
    .then(function(data) {
      btn.textContent = 'Programmer';
      btn.disabled = false;
      if (data.ok) {
        var msg = data.msg || ('Capteur programmé à l\'adresse ' + newAddr);
        resultEl.innerHTML = '<div class="success">' + msg + '</div>';
      } else {
        resultEl.innerHTML =
          '<div class="warning">' + (data.error || 'Erreur inconnue') + '</div>';
      }
    })
    .catch(function(err) {
      btn.textContent = 'Programmer';
      btn.disabled = false;
      resultEl.innerHTML =
        '<div class="warning">Erreur de communication : ' + err.message + '</div>';
    });
}

function exitMaintenance() {
  fetch('/rs485/exit', { method: 'POST' });
}
</script>
</body>
</html>
)HTML";

    return html;
}
