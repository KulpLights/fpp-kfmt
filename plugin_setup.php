<div id="global" class="settings">
<?
PrintSettingGroup("KFMTRadio", "", "", 1, "fpp-kfmt");
PrintSettingGroup("KFMTRDSSettings", "", "", 1, "fpp-kfmt");
?>
<style>
#kfmtPsPreviewWrap { margin: 0.35rem 0 0 0; max-width: 100%; }
#kfmtPsChunks { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; font-size: 0.95rem; letter-spacing: 0.02em; }
.kfmt-ps-chunk {
    display: inline-block;
    border: 1px solid #888;
    border-radius: 3px;
    padding: 0.15rem 0.35rem;
    margin: 0.15rem 0.35rem 0.15rem 0;
    background: rgba(127,127,127,0.12);
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
<p class="text-body">Match is judged from the QN8027 PACAP register (0x00–0x1F is in range). Retune after moving the antenna or enclosure. During a show, leave the policy on Manual only and do not press Retune unless you mean to.</p>
<div class="mb-3">
  <button type="button" class="buttons btn-success" id="kfmtRetuneBtn" onclick="kfmtRetune();">Retune Antenna</button>
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
    <tr><th scope="row">Antenna match</th><td id="kfmtMatch">—</td></tr>
    <tr><th scope="row">PACAP / ANT</th><td id="kfmtPacap">—</td></tr>
    <tr><th scope="row">Audio peak</th><td id="kfmtPeak">—</td></tr>
    <tr><th scope="row">Last retune</th><td id="kfmtLastRetune">—</td></tr>
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
function kfmtRefreshStatus() {
    $.ajax({
        url: '/api/plugin-apis/kfmt',
        method: 'GET',
        dataType: 'json',
        cache: false
    })
        .done(function (s) {
            if (s.adapterPresent === false) {
                kfmtSetText('kfmtAdapter', 'Disconnected', true);
                kfmtSetText('kfmtFsm', s.fsmName || 'Disconnected', true);
                kfmtSetText('kfmtChannel', '—', false);
                kfmtSetText('kfmtCarrier', '—', false);
                kfmtSetText('kfmtMatch', '—', false);
                kfmtSetText('kfmtPacap', '—', false);
                kfmtSetText('kfmtPeak', '—', false);
                return;
            }
            kfmtSetText('kfmtAdapter', 'Connected', false);
            if (!s.detected) {
                kfmtSetText('kfmtFsm', s.fsmName || 'Needs FPPD restart', true);
                kfmtSetText('kfmtChannel', '—', false);
                kfmtSetText('kfmtCarrier', '—', false);
                kfmtSetText('kfmtMatch', '—', false);
                kfmtSetText('kfmtPacap', '—', false);
                kfmtSetText('kfmtPeak', '—', false);
                return;
            }
            kfmtSetText('kfmtChannel', (s.channel ? s.channel.toFixed(2) : '—') + ' MHz', false);
            kfmtSetText('kfmtFsm', s.fsmName || ('FSM ' + s.fsm), false);
            var carrier = s.transmitting ? (s.muted ? 'on (muted)' : 'on') : 'off';
            kfmtSetText('kfmtCarrier', carrier, !s.transmitting);
            var match = s.matchOk ? 'OK' : 'out of range';
            if (s.antRail) match += ' (ANT at rail)';
            kfmtSetText('kfmtMatch', match, !s.matchOk || s.antRail);
            kfmtSetText('kfmtPacap', 'PACAP 0x' + s.pacapHex + ' / ANT 0x' + s.antHex, !s.matchOk);
            var peak = String(s.audioPeak) + ' / 15 (max-hold)';
            if (s.audioClip) peak += ' — clipping since last clear';
            kfmtSetText('kfmtPeak', peak, s.audioClip);
            kfmtSetText('kfmtLastRetune', s.lastRetune || 'none', s.lastRetune === 'out of range');
        })
        .fail(function (xhr) {
            kfmtSetText('kfmtFsm', 'status unavailable' + (xhr && xhr.status ? ' (' + xhr.status + ')' : ''), true);
        });
}
function kfmtRetune() {
    var btn = document.getElementById('kfmtRetuneBtn');
    var msg = document.getElementById('kfmtRetuneMsg');
    btn.disabled = true;
    msg.textContent = 'Retuning…';
    $.ajax({
        url: '/api/plugin-apis/kfmt/retune',
        method: 'POST',
        dataType: 'json'
    }).done(function (s) {
        msg.textContent = s.ok ? 'Match OK' : 'Match out of range — try antenna position and retune again';
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
        url: '/api/plugin-apis/kfmt/clearpeak',
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
