<?php $kfmtFreqNote = "Not sure which frequency to use? <a href=\"https://radio-locator.com/cgi-bin/vacant\" target=\"_blank\" rel=\"noopener noreferrer\">radio-locator.com</a> lists the vacant FM channels for your area."; ?>
<div id="global" class="settings">
<style>
/* Which power control applies depends on the board, which the first status
   poll reports; kfmtShowBoard() reveals the right one. Classes rather than
   jQuery show(), which would replace the Bootstrap row's flex with block. */
#TransmitPowerRow:not(.kfmt-shown), #PowerLevelRow:not(.kfmt-shown) { display: none; }
.kfmt-hidden { display: none !important; }
</style>
<?
global $pluginSettings;
// PowerLevel has no default on purpose: until it is chosen, a K-FMT board keeps
// the level its Transmit Power already gave it. Transmit Power 18 and up maps
// to PAC 30 or more, which is High (see mappedPaTarget()).
if (!isset($pluginSettings['PowerLevel']) || $pluginSettings['PowerLevel'] === '') {
    $kfmtUi = isset($pluginSettings['TransmitPower']) ? floatval($pluginSettings['TransmitPower']) : 50;
    $kfmtUi = max(0, min(100, $kfmtUi));
    $pluginSettings['PowerLevel'] = (round(20 + $kfmtUi / 100 * 55) >= 30) ? '1' : '0';
}
PrintSettingGroup("KFMTRadio", "", $kfmtFreqNote, 1, "fpp-kfmt");
PrintSettingGroup("KFMTRDSSettings", "", "", 1, "fpp-kfmt");
?>
<style>
#kfmtPsPreviewWrap { margin: 0.35rem 0 0 0; max-width: 100%; }
#kfmtPsChunks { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; font-size: 0.95rem; letter-spacing: 0.02em; }
.kfmt-ps-chunk {
    display: inline-block;
    /* FPP's own design-system tokens, redefined under [data-bs-theme='dark'],
       so the boxes follow the theme instead of being a grey that is a
       compromise in both. The literals are fallbacks for FPP 9.x, which this
       plugin still supports and which may not define them. */
    border: 1px solid var(--fpp-border, #888);
    border-radius: 3px;
    padding: 0.15rem 0.35rem;
    margin: 0.15rem 0.35rem 0.15rem 0;
    background: var(--fpp-bg-hover, rgba(127,127,127,0.12));
    white-space: pre;
}
.kfmt-ps-space { opacity: 0.45; }
#kfmtPsMeta { font-size: 0.85rem; opacity: 0.8; margin-top: 0.2rem; }
</style>
<div id="kfmtPsPreviewWrap" style="display:none;">
  <div id="kfmtPsChunks"></div>
  <div id="kfmtPsMeta"></div>
</div>
<h2>Transmitter Status</h2>
<p class="text-body kfmt-tuning">Antenna Tuning shows the QN8027's antenna tuning register. The chip gives no readable measure of how good the match is, so judge it with a receiver. Retune re-runs the chip's calibration - do that after moving the antenna or its enclosure, and not during a show.</p>
<div class="mb-3">
  <button type="button" class="buttons btn-success kfmt-tuning" id="kfmtRetuneBtn" onclick="kfmtRetune();">Retune Antenna</button>
  <button type="button" class="buttons" id="kfmtClearPeakBtn" onclick="kfmtClearPeak();">Clear Audio Peak</button>
  <span id="kfmtRetuneMsg" class="ms-2"></span>
</div>
<div class="table-responsive" style="max-width: 40rem;">
<table class="table table-sm">
  <tbody>
    <tr><th scope="row">Adapter</th><td id="kfmtAdapter">—</td></tr>
    <tr><th scope="row">Channel</th><td id="kfmtChannel">—</td></tr>
    <tr><th scope="row">FSM</th><td id="kfmtFsm">—</td></tr>
    <tr><th scope="row">Carrier</th><td id="kfmtCarrier">—</td></tr>
    <tr class="kfmt-tuning"><th scope="row">Antenna Tuning</th><td id="kfmtAnt">—</td></tr>
    <tr><th scope="row">Audio peak</th><td id="kfmtPeak">—</td></tr>
    <tr class="kfmt-tuning"><th scope="row">Last retune</th><td id="kfmtLastRetune">—</td></tr>
  </tbody>
</table>
</div>
<script>
function kfmtSetText(id, text, warn) {
    var el = document.getElementById(id);
    if (!el) return;
    el.textContent = text;
    el.classList.toggle('text-danger', !!warn);
}
// A K-FMT board gets the High/Low power control instead of the 1-100 range,
// and no antenna tuning: the chip's tuning there faces the RF amplifier, not
// the antenna, so neither the register nor Retune tells you about the antenna.
var kfmtBoard = null;
function kfmtShowBoard(board) {
    if (board === undefined) {
        // A plugin built before boards were reported (fppd not yet restarted
        // after an update) only knows Transmit Power. A timeout reply also
        // lacks it, so this never overrides a board already seen.
        if (kfmtBoard === null) {
            $('#TransmitPowerRow').addClass('kfmt-shown');
        }
        return;
    }
    if (board === kfmtBoard) return;
    kfmtBoard = board;
    var kfmt = board === 'KFMT';
    $('#PowerLevelRow').toggleClass('kfmt-shown', kfmt);
    $('#TransmitPowerRow').toggleClass('kfmt-shown', !kfmt);
    $('.kfmt-tuning').toggleClass('kfmt-hidden', kfmt);
}
// URLs here are relative on purpose. An absolute "/api/..." loses the prefix
// when the page is reached through FPP's proxy or loaded by FPPMon, and the
// request goes to the wrong host.
function kfmtRefreshStatus() {
    $.ajax({
        url: 'api/plugin-apis/kfmt',
        method: 'GET',
        dataType: 'json',
        cache: false
    })
        .done(function (s) {
            kfmtShowBoard(s.board);
            if (s.adapterPresent === false) {
                kfmtSetText('kfmtAdapter', 'Disconnected', true);
                kfmtSetText('kfmtFsm', s.fsmName || 'Disconnected', true);
                kfmtSetText('kfmtChannel', '—', false);
                kfmtSetText('kfmtCarrier', '—', false);
                kfmtSetText('kfmtAnt', '—', false);
                kfmtSetText('kfmtPeak', '—', false);
                return;
            }
            kfmtSetText('kfmtAdapter', 'Connected', false);
            if (!s.detected) {
                kfmtSetText('kfmtFsm', s.fsmName || 'Reconnecting', true);
                kfmtSetText('kfmtChannel', '—', false);
                kfmtSetText('kfmtCarrier', '—', false);
                kfmtSetText('kfmtAnt', '—', false);
                kfmtSetText('kfmtPeak', '—', false);
                return;
            }
            kfmtSetText('kfmtChannel', (s.channel ? s.channel.toFixed(2) : '—') + ' MHz', false);
            kfmtSetText('kfmtFsm', s.fsmName || ('FSM ' + s.fsm), false);
            var carrier = s.transmitting ? (s.muted ? 'on (muted)' : 'on') : 'off';
            kfmtSetText('kfmtCarrier', carrier, !s.transmitting);
            kfmtSetText('kfmtAnt', '0x' + s.antHex + (s.antRail ? ' (at rail)' : ''), s.antRail);
            var peak = String(s.audioPeak) + ' / 15 (max-hold)';
            if (s.audioClip) peak += ' — clipping since last clear';
            kfmtSetText('kfmtPeak', peak, s.audioClip);
            kfmtSetText('kfmtLastRetune', s.lastRetune || 'none', s.lastRetune === 'out of range');
        })
        .fail(function (xhr) {
            // Without a board answer, offer both power controls rather than none.
            if (kfmtBoard === null) {
                $('#PowerLevelRow, #TransmitPowerRow').addClass('kfmt-shown');
            }
            kfmtSetText('kfmtFsm', 'status unavailable' + (xhr && xhr.status ? ' (' + xhr.status + ')' : ''), true);
        });
}
function kfmtRetune() {
    var btn = document.getElementById('kfmtRetuneBtn');
    var msg = document.getElementById('kfmtRetuneMsg');
    btn.disabled = true;
    msg.textContent = 'Retuning…';
    $.ajax({
        url: 'api/plugin-apis/kfmt/retune',
        method: 'POST',
        dataType: 'json'
    }).done(function (s) {
        msg.textContent = 'Retuned — check the signal on a receiver';
        kfmtRefreshStatus();
    }).fail(function () {
        msg.textContent = 'Retune failed';
    }).always(function () {
        btn.disabled = false;
    });
}
function kfmtClearPeak() {
    var btn = document.getElementById('kfmtClearPeakBtn');
    btn.disabled = true;
    $.ajax({
        url: 'api/plugin-apis/kfmt/clearpeak',
        method: 'POST',
        dataType: 'json'
    }).done(function () {
        kfmtRefreshStatus();
    }).always(function () {
        btn.disabled = false;
    });
}
function kfmtFindStationIdInput() {
    return document.getElementById('StationID')
        || document.querySelector('input[name="StationID"]')
        || document.querySelector('#pluginSetting_StationID input')
        || document.querySelector('input[id*="StationID"]');
}
function kfmtPsPreviewFrom(value) {
    var chunksEl = document.getElementById('kfmtPsChunks');
    var metaEl = document.getElementById('kfmtPsMeta');
    var wrap = document.getElementById('kfmtPsPreviewWrap');
    if (!chunksEl || !metaEl || !wrap) return;
    wrap.style.display = '';
    var s = value || '';
    var n = Math.max(1, Math.ceil(s.length / 8) || 1);
    if (s.length === 0) n = 1;
    chunksEl.textContent = '';
    for (var i = 0; i < n; i++) {
        var part = s.substr(i * 8, 8);
        while (part.length < 8) part += ' ';
        var box = document.createElement('span');
        box.className = 'kfmt-ps-chunk';
        box.title = 'Screen ' + (i + 1);
        for (var c = 0; c < part.length; c++) {
            if (part.charAt(c) === ' ') {
                var sp = document.createElement('span');
                sp.className = 'kfmt-ps-space';
                sp.textContent = '·';
                box.appendChild(sp);
            } else {
                box.appendChild(document.createTextNode(part.charAt(c)));
            }
        }
        chunksEl.appendChild(box);
    }
    var screens = n + (n === 1 ? ' screen' : ' screens');
    metaEl.textContent = s.length + ' character' + (s.length === 1 ? '' : 's') + ' → ' + screens
        + ' (rotates with Station ID Display Time)';
}
function kfmtHookStationIdPreview() {
    var input = kfmtFindStationIdInput();
    var wrap = document.getElementById('kfmtPsPreviewWrap');
    if (!input || !wrap) return;
    var fieldCol = document.querySelector('#StationIDRow .printSettingFieldCol')
        || input.closest('.printSettingFieldCol')
        || input.parentNode;
    if (fieldCol) {
        fieldCol.appendChild(wrap);
    }
    var update = function () { kfmtPsPreviewFrom(input.value); };
    input.addEventListener('input', update);
    input.addEventListener('change', update);
    update();
}
kfmtHookStationIdPreview();
kfmtRefreshStatus();
setInterval(kfmtRefreshStatus, 1500);
</script>
</div>
