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
.btn-stop { background: #e53935; color: white; }
.btn-stop:hover { background: #c62828; }
.warning { background: rgba(244,67,54,0.3); padding: 12px; border-radius: 8px; margin: 15px 0; }
.success { background: rgba(76,175,80,0.3); padding: 12px; border-radius: 8px; margin: 15px 0; }
</style>
</head>
<body>

<h1>Programmation RS485</h1>

<div class="card">
  <h2>Capteurs sol ZTS-3000</h2>
  <p>Brancher <strong>un seul capteur</strong> sur le bus RS485.</p>

  <button class="btn btn-primary" id="btnReadSoil" onclick="doReadSoil()">Lire la configuration</button>
  <button class="btn btn-stop" id="btnStopSoil" onclick="stopScan('soil')" style="display:none;">Stopper</button>
  <div id="soilReadResult"></div>

  <div id="soilProgramSection" style="display:none; margin-top: 20px; border-top: 1px solid rgba(255,255,255,0.3); padding-top: 15px;">
    <p>Nouvelle adresse (1–15) :</p>
    <select id="soilNewAddr">
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
    <p>Vitesse :</p>
    <select id="soilNewBaud">
      <option value="2400">2400</option>
      <option value="4800" selected>4800</option>
      <option value="9600">9600</option>
    </select>
    <p>Parité :</p>
    <select id="soilNewParity">
      <option value="0" selected>aucune</option>
      <option value="1">impaire</option>
      <option value="2">paire</option>
    </select>
    <br>
    <button class="btn btn-primary" id="btnProgramSoil" onclick="doProgramSoil()">Programmer</button>
    <div id="soilProgramResult"></div>
  </div>
</div>

<div class="card">
  <h2>Capteurs air Ebyte KTH2-R</h2>
  <p>Brancher <strong>un seul capteur Ebyte</strong> sur le bus RS485.</p>

  <button class="btn btn-primary" id="btnReadEbyte" onclick="doReadEbyte()">Lire la configuration</button>
  <button class="btn btn-stop" id="btnStopEbyte" onclick="stopScan('ebyte')" style="display:none;">Stopper</button>
  <div id="ebyteReadResult"></div>

  <div id="ebyteProgramSection" style="display:none; margin-top: 20px; border-top: 1px solid rgba(255,255,255,0.3); padding-top: 15px;">
    <p>Nouvelle adresse (1–16) :</p>
    <select id="ebyteNewAddr">
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
      <option value="16">16</option>
    </select>
    <p>Vitesse :</p>
    <select id="ebyteNewBaud">
      <option value="1200">1200</option>
      <option value="2400">2400</option>
      <option value="4800" selected>4800</option>
      <option value="9600">9600</option>
      <option value="19200">19200</option>
    </select>
    <p>Parité :</p>
    <select id="ebyteNewParity">
      <option value="0" selected>aucune</option>
      <option value="1">impaire</option>
      <option value="2">paire</option>
    </select>
    <br>
    <button class="btn btn-primary" id="btnProgramEbyte" onclick="doProgramEbyte()">Programmer</button>
    <div id="ebyteProgramResult"></div>
  </div>
</div>

<div class="card">
  <h2>Module Analog Input 8CH</h2>
  <p>Brancher <strong>uniquement le module Analog Input</strong> sur le bus RS485.</p>

  <button class="btn btn-primary" id="btnReadAnalog" onclick="doReadAnalog()">Lire la configuration</button>
  <button class="btn btn-stop" id="btnStopAnalog" onclick="stopScan('analog')" style="display:none;">Stopper</button>
  <div id="analogReadResult"></div>

  <div id="analogProgramSection" style="display:none; margin-top: 20px; border-top: 1px solid rgba(255,255,255,0.3); padding-top: 15px;">
    <p>Nouvelle adresse (16–30) :</p>
    <select id="analogNewAddr">
      <option value="16" selected>16</option>
      <option value="17">17</option>
      <option value="18">18</option>
      <option value="19">19</option>
      <option value="20">20</option>
      <option value="21">21</option>
      <option value="22">22</option>
      <option value="23">23</option>
      <option value="24">24</option>
      <option value="25">25</option>
      <option value="26">26</option>
      <option value="27">27</option>
      <option value="28">28</option>
      <option value="29">29</option>
      <option value="30">30</option>
    </select>
    <p>Vitesse :</p>
    <select id="analogNewBaud">
      <option value="4800" selected>4800</option>
      <option value="9600">9600</option>
      <option value="19200">19200</option>
      <option value="38400">38400</option>
      <option value="57600">57600</option>
      <option value="115200">115200</option>
    </select>
    <p>Parité :</p>
    <select id="analogNewParity">
      <option value="0" selected>aucune</option>
      <option value="1">impaire</option>
      <option value="2">paire</option>
    </select>
    <br>
    <button class="btn btn-primary" id="btnProgramAnalog" onclick="doProgramAnalog()">Programmer</button>
    <div id="analogProgramResult"></div>
  </div>

  <div id="analogChannelSection" style="display:none; margin-top: 20px; border-top: 1px solid rgba(255,255,255,0.3); padding-top: 15px;">
    <p>Lire le mode d'un canal :</p>
    <select id="analogChannel">
      <option value="1">Canal 1</option>
      <option value="2">Canal 2</option>
      <option value="3">Canal 3</option>
      <option value="4">Canal 4</option>
      <option value="5">Canal 5</option>
      <option value="6">Canal 6</option>
      <option value="7">Canal 7</option>
      <option value="8">Canal 8</option>
    </select>
    <button class="btn btn-primary" id="btnReadChannel" onclick="doReadAnalogChannel()">Lire le mode</button>
    <div id="analogChannelResult"></div>
    <div id="analogFixSection" style="display:none; margin-top: 10px;">
      <button class="btn btn-primary" id="btnFixChannel" onclick="doFixAnalogChannel()" style="background:#ff9800;">Corriger &rarr; 0&ndash;10V</button>
      <div id="analogFixResult"></div>
    </div>
  </div>
</div>

<div style="margin-top: 30px;">
  <a href="/" class="btn btn-back" onclick="exitMaintenance()">&#8592; Retour</a>
</div>

<script>
// ── Capteurs air Ebyte KTH2-R — lecture config + programmation ───────
var EB_SCAN_BAUDS = [9600, 4800, 19200, 2400, 1200];
var EB_SCAN_PARITY_NAMES = ['aucune', 'impaire', 'paire'];
var ebyteCurrent = { address: 0, baudrate: 0, parity: 0 };
var scanAbort = { ebyte: false, soil: false, analog: false };
var scanCtrl = { ebyte: null, soil: null, analog: null };
var scanBtn = {
  ebyte:  { read: 'btnReadEbyte',  stop: 'btnStopEbyte' },
  soil:   { read: 'btnReadSoil',   stop: 'btnStopSoil' },
  analog: { read: 'btnReadAnalog', stop: 'btnStopAnalog' }
};

function beginScan(kind) {
  scanAbort[kind] = false;
  if (scanCtrl[kind]) scanCtrl[kind].abort();
  scanCtrl[kind] = new AbortController();
  var readBtn = document.getElementById(scanBtn[kind].read);
  var stopBtn = document.getElementById(scanBtn[kind].stop);
  readBtn.disabled = true;
  readBtn.textContent = 'Scan en cours...';
  stopBtn.style.display = 'inline-block';
  return scanCtrl[kind];
}

function endScan(kind) {
  var readBtn = document.getElementById(scanBtn[kind].read);
  var stopBtn = document.getElementById(scanBtn[kind].stop);
  readBtn.textContent = 'Lire la configuration';
  readBtn.disabled = false;
  stopBtn.style.display = 'none';
}

function stopScan(kind) {
  scanAbort[kind] = true;
  if (scanCtrl[kind]) scanCtrl[kind].abort();
}

function scanWasStopped(kind, err) {
  return scanAbort[kind] || (err && err.name === 'AbortError');
}

function ebyteChunkSize(baud) {
  return (baud <= 1200) ? 8 : 16;
}

function ebyteScanSlices() {
  var slices = [];
  var b, p, from, to, chunk;
  for (p = 0; p <= 2; p++) {
    for (b = 0; b < EB_SCAN_BAUDS.length; b++) {
      slices.push({ baud: EB_SCAN_BAUDS[b], parity: p, from: 1, to: 16 });
    }
  }
  for (b = 0; b < EB_SCAN_BAUDS.length; b++) {
    chunk = ebyteChunkSize(EB_SCAN_BAUDS[b]);
    for (from = 17; from <= 254; from += chunk) {
      to = from + chunk - 1;
      if (to > 254) to = 254;
      slices.push({ baud: EB_SCAN_BAUDS[b], parity: 0, from: from, to: to });
    }
  }
  return slices;
}

function doReadEbyte() {
  var btn = document.getElementById('btnReadEbyte');
  var resultEl = document.getElementById('ebyteReadResult');
  var progSection = document.getElementById('ebyteProgramSection');
  var slices = ebyteScanSlices();

  beginScan('ebyte');
  progSection.style.display = 'none';

  function finishOk(data) {
    endScan('ebyte');
    ebyteCurrent.address = data.address;
    ebyteCurrent.baudrate = data.baudrate;
    ebyteCurrent.parity = (typeof data.parity === 'number') ? data.parity : 0;
    document.getElementById('ebyteNewAddr').value = String(data.address);
    document.getElementById('ebyteNewBaud').value = '4800';
    document.getElementById('ebyteNewParity').value = '0';
    var parityLine = data.parityName
      ? '<br>Parité : <strong>' + data.parityName + '</strong>'
      : '';
    resultEl.innerHTML =
      '<div class="success">'
      + 'Adresse actuelle : <strong>' + data.address + '</strong><br>'
      + 'Baud rate : <strong>' + data.baudrate + '</strong>'
      + parityLine
      + '</div>';
    progSection.style.display = 'block';
  }

  function finishErr(msg) {
    endScan('ebyte');
    resultEl.innerHTML = '<div class="warning">' + msg + '</div>';
  }

  function runSlice(index) {
    if (scanAbort.ebyte) {
      finishErr('Recherche interrompue');
      return;
    }
    if (index >= slices.length) {
      finishErr('Aucun capteur détecté (adresses 1–254, 1200–19200 bauds, 3 parités)');
      return;
    }

    var s = slices[index];
    var parityName = EB_SCAN_PARITY_NAMES[s.parity] || 'aucune';
    resultEl.innerHTML =
      '<div class="status"><span class="spinner"></span>Scan '
      + s.baud + ' bauds, parité ' + parityName
      + ', adresses ' + s.from + '–' + s.to
      + ' (' + (index + 1) + '/' + slices.length + ')…</div>';

    var url = '/rs485/read-ebyte?baud=' + s.baud
            + '&parity=' + s.parity
            + '&from=' + s.from
            + '&to=' + s.to;

    fetch(url, { method: 'POST', signal: scanCtrl.ebyte.signal })
      .then(function(r) {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
      })
      .then(function(data) {
        if (scanAbort.ebyte) {
          finishErr('Recherche interrompue');
        } else if (data.ok) {
          finishOk(data);
        } else {
          setTimeout(function() { runSlice(index + 1); }, 120);
        }
      })
      .catch(function(err) {
        if (scanWasStopped('ebyte', err)) {
          finishErr('Recherche interrompue');
        } else {
          finishErr('Erreur de communication : ' + err.message
                    + ' (arrêt à ' + s.baud + ' bauds, adresses ' + s.from + '–' + s.to + ')');
        }
      });
  }

  runSlice(0);
}

function doProgramEbyte() {
  var btn = document.getElementById('btnProgramEbyte');
  var resultEl = document.getElementById('ebyteProgramResult');

  if (!ebyteCurrent.address) {
    resultEl.innerHTML = '<div class="warning">Lancez d\'abord une lecture de la configuration</div>';
    return;
  }

  var toAddr = parseInt(document.getElementById('ebyteNewAddr').value);
  var toBaud = parseInt(document.getElementById('ebyteNewBaud').value);
  var toParity = parseInt(document.getElementById('ebyteNewParity').value);

  btn.disabled = true;
  btn.textContent = 'Programmation...';
  resultEl.innerHTML = '<div class="status"><span class="spinner"></span>Envoi des commandes…</div>';

  var url = '/rs485/program-ebyte'
          + '?from=' + ebyteCurrent.address
          + '&fromBaud=' + ebyteCurrent.baudrate
          + '&fromParity=' + ebyteCurrent.parity
          + '&to=' + toAddr
          + '&baud=' + toBaud
          + '&parity=' + toParity;

  fetch(url, { method: 'POST' })
    .then(function(r) { return r.json(); })
    .then(function(data) {
      btn.textContent = 'Programmer';
      btn.disabled = false;
      if (data.ok) {
        ebyteCurrent.address = toAddr;
        ebyteCurrent.baudrate = toBaud;
        ebyteCurrent.parity = toParity;
        resultEl.innerHTML =
          '<div class="success">' + (data.msg || 'Capteur programmé avec succès') + '</div>';
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

// ── Analog Input 8CH (B) — lecture config + programmation ────────────
var analogCurrent = { address: 0, baudrate: 0, parity: 0 };
var analogDetectedAddr = 0;
var ANALOG_SCAN_BAUDS = [9600, 4800, 19200, 38400, 57600, 115200];

function analogScanSlices() {
  var slices = [];
  var b, p, from, to, chunk;
  for (p = 0; p <= 2; p++) {
    for (b = 0; b < ANALOG_SCAN_BAUDS.length; b++) {
      slices.push({ baud: ANALOG_SCAN_BAUDS[b], parity: p, from: 1, to: 30 });
    }
  }
  for (b = 0; b < 2; b++) {
    chunk = 16;
    for (from = 31; from <= 254; from += chunk) {
      to = from + chunk - 1;
      if (to > 254) to = 254;
      slices.push({ baud: ANALOG_SCAN_BAUDS[b], parity: 0, from: from, to: to });
    }
  }
  return slices;
}

function doReadAnalog() {
  var btn = document.getElementById('btnReadAnalog');
  var resultEl = document.getElementById('analogReadResult');
  var progSection = document.getElementById('analogProgramSection');
  var chSection = document.getElementById('analogChannelSection');
  var slices = analogScanSlices();

  beginScan('analog');
  progSection.style.display = 'none';
  chSection.style.display = 'none';
  analogDetectedAddr = 0;
  analogCurrent = { address: 0, baudrate: 0, parity: 0 };

  function finishOk(data) {
    endScan('analog');
    analogDetectedAddr = data.address;
    analogCurrent.address = data.address;
    analogCurrent.baudrate = data.baudrate;
    analogCurrent.parity = (typeof data.parity === 'number') ? data.parity : 0;
    if (data.address >= 16 && data.address <= 30) {
      document.getElementById('analogNewAddr').value = String(data.address);
    } else {
      document.getElementById('analogNewAddr').value = '16';
    }
    document.getElementById('analogNewBaud').value = '4800';
    document.getElementById('analogNewParity').value = '0';
    var parityLine = data.parityName
      ? '<br>Parité : <strong>' + data.parityName + '</strong>'
      : '';
    var versionLine = data.version
      ? '<br>Version firmware : <strong>' + data.version + '</strong>'
      : '';
    resultEl.innerHTML =
      '<div class="success">'
      + 'Adresse actuelle : <strong>' + data.address + '</strong><br>'
      + 'Baud rate : <strong>' + data.baudrate + '</strong>'
      + parityLine
      + versionLine
      + '</div>';
    progSection.style.display = 'block';
    chSection.style.display = 'block';
  }

  function finishErr(msg) {
    endScan('analog');
    resultEl.innerHTML = '<div class="warning">' + msg + '</div>';
  }

  function runSlice(index) {
    if (scanAbort.analog) {
      finishErr('Recherche interrompue');
      return;
    }
    if (index >= slices.length) {
      finishErr('Aucun module détecté (adresses 1–254, 4800–115200 bauds, 3 parités)');
      return;
    }
    var s = slices[index];
    var parityName = EB_SCAN_PARITY_NAMES[s.parity] || 'aucune';
    resultEl.innerHTML =
      '<div class="status"><span class="spinner"></span>Scan '
      + s.baud + ' bauds, parité ' + parityName
      + ', adresses ' + s.from + '–' + s.to
      + ' (' + (index + 1) + '/' + slices.length + ')…</div>';
    var url = '/rs485/read-analog?baud=' + s.baud
            + '&parity=' + s.parity
            + '&from=' + s.from
            + '&to=' + s.to;
    fetch(url, { method: 'POST', signal: scanCtrl.analog.signal })
      .then(function(r) {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
      })
      .then(function(data) {
        if (scanAbort.analog) finishErr('Recherche interrompue');
        else if (data.ok) finishOk(data);
        else setTimeout(function() { runSlice(index + 1); }, 120);
      })
      .catch(function(err) {
        if (scanWasStopped('analog', err)) {
          finishErr('Recherche interrompue');
        } else {
          finishErr('Erreur de communication : ' + err.message
                    + ' (arrêt à ' + s.baud + ' bauds, adresses ' + s.from + '–' + s.to + ')');
        }
      });
  }

  runSlice(0);
}

var AI_MODE_LABELS = [
  '0–10V (tension)',
  '2–10V (tension)',
  '0–20mA (courant)',
  '4–20mA (courant)',
  'Code brut (0–4096)'
];

function doReadAnalogChannel() {
  var ch = parseInt(document.getElementById('analogChannel').value);
  var btn = document.getElementById('btnReadChannel');
  var resultEl = document.getElementById('analogChannelResult');

  if (analogDetectedAddr === 0) {
    resultEl.innerHTML = '<div class="warning">Lancez d\'abord une lecture de la configuration</div>';
    return;
  }

  btn.disabled = true;
  btn.textContent = 'Lecture...';
  resultEl.innerHTML = '<div class="status"><span class="spinner"></span>Lecture du canal ' + ch + '…</div>';

  var fixSection = document.getElementById('analogFixSection');
  var fixResult = document.getElementById('analogFixResult');
  fixSection.style.display = 'none';
  fixResult.innerHTML = '';

  fetch('/rs485/read-analog-channel?ch=' + ch + '&addr=' + analogDetectedAddr
        + '&baud=' + analogCurrent.baudrate + '&parity=' + analogCurrent.parity, { method: 'POST' })
    .then(function(r) { return r.json(); })
    .then(function(data) {
      btn.textContent = 'Lire le mode';
      btn.disabled = false;
      if (data.ok) {
        var label = (data.mode >= 0 && data.mode < AI_MODE_LABELS.length)
                    ? AI_MODE_LABELS[data.mode]
                    : 'Inconnu (code ' + data.mode + ')';
        resultEl.innerHTML =
          '<div class="success">Canal ' + data.channel + ' : <strong>' + label + '</strong></div>';
        if (data.mode !== 0) {
          fixSection.style.display = 'block';
        }
      } else {
        resultEl.innerHTML =
          '<div class="warning">' + (data.error || 'Erreur de lecture') + '</div>';
      }
    })
    .catch(function(err) {
      btn.textContent = 'Lire le mode';
      btn.disabled = false;
      resultEl.innerHTML =
        '<div class="warning">Erreur de communication : ' + err.message + '</div>';
    });
}

function doFixAnalogChannel() {
  var ch = parseInt(document.getElementById('analogChannel').value);
  var btn = document.getElementById('btnFixChannel');
  var resultEl = document.getElementById('analogFixResult');

  if (analogDetectedAddr === 0) {
    resultEl.innerHTML = '<div class="warning">Module non détecté</div>';
    return;
  }

  btn.disabled = true;
  btn.textContent = 'Correction...';
  resultEl.innerHTML = '<div class="status"><span class="spinner"></span>Écriture mode 0 (0–10V) sur canal ' + ch + '…</div>';

  fetch('/rs485/write-analog-channel?ch=' + ch + '&addr=' + analogDetectedAddr
        + '&mode=0&baud=' + analogCurrent.baudrate + '&parity=' + analogCurrent.parity, { method: 'POST' })
    .then(function(r) { return r.json(); })
    .then(function(data) {
      btn.textContent = 'Corriger \u2192 0\u201310V';
      btn.disabled = false;
      if (data.ok) {
        var label = AI_MODE_LABELS[data.mode] || ('code ' + data.mode);
        resultEl.innerHTML =
          '<div class="success">Canal ' + data.channel + ' corrigé : <strong>' + label + '</strong></div>';
        document.getElementById('analogFixSection').style.display = 'none';
        document.getElementById('analogChannelResult').innerHTML =
          '<div class="success">Canal ' + data.channel + ' : <strong>' + label + '</strong></div>';
      } else {
        resultEl.innerHTML =
          '<div class="warning">' + (data.error || 'Erreur d\'écriture') + '</div>';
      }
    })
    .catch(function(err) {
      btn.textContent = 'Corriger \u2192 0\u201310V';
      btn.disabled = false;
      resultEl.innerHTML =
        '<div class="warning">Erreur de communication : ' + err.message + '</div>';
    });
}

function doProgramAnalog() {
  var btn = document.getElementById('btnProgramAnalog');
  var resultEl = document.getElementById('analogProgramResult');

  if (!analogCurrent.address) {
    resultEl.innerHTML = '<div class="warning">Lancez d\'abord une lecture de la configuration</div>';
    return;
  }

  var toAddr = parseInt(document.getElementById('analogNewAddr').value);
  var toBaud = parseInt(document.getElementById('analogNewBaud').value);
  var toParity = parseInt(document.getElementById('analogNewParity').value);

  btn.disabled = true;
  btn.textContent = 'Programmation...';
  resultEl.innerHTML = '<div class="status"><span class="spinner"></span>Envoi des commandes…</div>';

  var url = '/rs485/program-analog'
          + '?from=' + analogCurrent.address
          + '&fromBaud=' + analogCurrent.baudrate
          + '&fromParity=' + analogCurrent.parity
          + '&to=' + toAddr
          + '&baud=' + toBaud
          + '&parity=' + toParity;

  fetch(url, { method: 'POST' })
    .then(function(r) { return r.json(); })
    .then(function(data) {
      btn.textContent = 'Programmer';
      btn.disabled = false;
      if (data.ok) {
        analogCurrent.address = toAddr;
        analogCurrent.baudrate = toBaud;
        analogCurrent.parity = toParity;
        analogDetectedAddr = toAddr;
        resultEl.innerHTML =
          '<div class="success">' + (data.msg || 'Module programmé avec succès') + '</div>';
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

// ── Capteurs sol ZTS-3000 — lecture + programmation ───────────
var soilCurrent = { address: 0, baudrate: 0, parity: 0 };
var SOIL_SCAN_BAUDS = [4800, 9600, 2400];

function soilScanSlices() {
  var slices = [];
  var b, p, from, to, chunk;
  for (p = 0; p <= 2; p++) {
    for (b = 0; b < SOIL_SCAN_BAUDS.length; b++) {
      slices.push({ baud: SOIL_SCAN_BAUDS[b], parity: p, from: 1, to: 15 });
    }
  }
  for (b = 0; b < SOIL_SCAN_BAUDS.length; b++) {
    chunk = ebyteChunkSize(SOIL_SCAN_BAUDS[b]);
    for (from = 16; from <= 254; from += chunk) {
      to = from + chunk - 1;
      if (to > 254) to = 254;
      slices.push({ baud: SOIL_SCAN_BAUDS[b], parity: 0, from: from, to: to });
    }
  }
  return slices;
}

function doReadSoil() {
  var btn = document.getElementById('btnReadSoil');
  var resultEl = document.getElementById('soilReadResult');
  var progSection = document.getElementById('soilProgramSection');
  var slices = soilScanSlices();

  beginScan('soil');
  progSection.style.display = 'none';

  function finishOk(data) {
    endScan('soil');
    soilCurrent.address = data.address;
    soilCurrent.baudrate = data.baudrate;
    soilCurrent.parity = (typeof data.parity === 'number') ? data.parity : 0;
    document.getElementById('soilNewAddr').value = String(data.address <= 15 ? data.address : 1);
    document.getElementById('soilNewBaud').value = '4800';
    document.getElementById('soilNewParity').value = '0';
    var parityLine = data.parityName
      ? '<br>Parité : <strong>' + data.parityName + '</strong>'
      : '';
    resultEl.innerHTML =
      '<div class="success">'
      + 'Adresse actuelle : <strong>' + data.address + '</strong><br>'
      + 'Baud rate : <strong>' + data.baudrate + '</strong>'
      + parityLine
      + '</div>';
    progSection.style.display = 'block';
  }

  function finishErr(msg) {
    endScan('soil');
    resultEl.innerHTML = '<div class="warning">' + msg + '</div>';
  }

  function runSlice(index) {
    if (scanAbort.soil) {
      finishErr('Recherche interrompue');
      return;
    }
    if (index >= slices.length) {
      finishErr('Aucun capteur détecté (adresses 1–254, 2400–9600 bauds, 3 parités)');
      return;
    }
    var s = slices[index];
    var parityName = EB_SCAN_PARITY_NAMES[s.parity] || 'aucune';
    resultEl.innerHTML =
      '<div class="status"><span class="spinner"></span>Scan '
      + s.baud + ' bauds, parité ' + parityName
      + ', adresses ' + s.from + '–' + s.to
      + ' (' + (index + 1) + '/' + slices.length + ')…</div>';
    var url = '/rs485/read-soil?baud=' + s.baud
            + '&parity=' + s.parity
            + '&from=' + s.from
            + '&to=' + s.to;
    fetch(url, { method: 'POST', signal: scanCtrl.soil.signal })
      .then(function(r) {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
      })
      .then(function(data) {
        if (scanAbort.soil) finishErr('Recherche interrompue');
        else if (data.ok) finishOk(data);
        else setTimeout(function() { runSlice(index + 1); }, 120);
      })
      .catch(function(err) {
        if (scanWasStopped('soil', err)) {
          finishErr('Recherche interrompue');
        } else {
          finishErr('Erreur de communication : ' + err.message
                    + ' (arrêt à ' + s.baud + ' bauds, adresses ' + s.from + '–' + s.to + ')');
        }
      });
  }

  runSlice(0);
}

function doProgramSoil() {
  var btn = document.getElementById('btnProgramSoil');
  var resultEl = document.getElementById('soilProgramResult');

  if (!soilCurrent.address) {
    resultEl.innerHTML = '<div class="warning">Lancez d\'abord une lecture de la configuration</div>';
    return;
  }

  var toAddr = parseInt(document.getElementById('soilNewAddr').value);
  var toBaud = parseInt(document.getElementById('soilNewBaud').value);
  var toParity = parseInt(document.getElementById('soilNewParity').value);

  btn.disabled = true;
  btn.textContent = 'Programmation...';
  resultEl.innerHTML = '<div class="status"><span class="spinner"></span>Envoi des commandes…</div>';

  var url = '/rs485/program-soil'
          + '?from=' + soilCurrent.address
          + '&fromBaud=' + soilCurrent.baudrate
          + '&fromParity=' + soilCurrent.parity
          + '&to=' + toAddr
          + '&baud=' + toBaud
          + '&parity=' + toParity;

  fetch(url, { method: 'POST' })
    .then(function(r) { return r.json(); })
    .then(function(data) {
      btn.textContent = 'Programmer';
      btn.disabled = false;
      if (data.ok) {
        soilCurrent.address = toAddr;
        soilCurrent.baudrate = toBaud;
        soilCurrent.parity = toParity;
        resultEl.innerHTML =
          '<div class="success">' + (data.msg || 'Capteur programmé avec succès') + '</div>';
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
