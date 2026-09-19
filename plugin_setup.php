<div id="global" class="settings">
<?
PrintSettingGroup("KFMTRadio", "", "", 1, "fpp-kfmt");
PrintSettingGroup("KFMTRDSSettings", "", "", 1, "fpp-kfmt");
?>
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
            if (!s.detected) {
                kfmtSetText('kfmtFsm', 'QN8027 not detected', true);
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
kfmtRefreshStatus();
setInterval(kfmtRefreshStatus, 1500);
</script>
</div>
