<?php $kfmtFreqNote = "Not sure which frequency to use? <a href=\"https://radio-locator.com/cgi-bin/vacant\" target=\"_blank\" rel=\"noopener noreferrer\">radio-locator.com</a> lists the vacant FM channels for your area."; ?>
<div id="global" class="settings">
<?
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
.kfmt-ps-live { outline: 2px solid var(--fpp-primary, #0d6efd); outline-offset: 1px; }
#kfmtPsOnAir, #kfmtRtOnAir {
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
    font-size: 0.9rem;
}
.kfmt-override {
    font-size: 0.85rem;
    margin-top: 0.3rem;
    padding: 0.2rem 0.45rem;
    /* FPP's warning trio, all redefined under [data-bs-theme='dark'], so the
       note follows the theme rather than being a compromise in one of them.
       The literals are the light-mode values, as fallbacks for FPP 9.x. */
    color: var(--fpp-warning-text, #856404);
    background: var(--fpp-warning-light, #fef8e8);
    border-left: 3px solid var(--fpp-warning-dark, #d9a84e);
}
.kfmt-override-clear {
    margin-left: 0.5rem;
    padding: 0 0.45rem;
    font-size: 0.8rem;
    vertical-align: baseline;
}
#kfmtPsMeta { font-size: 0.85rem; opacity: 0.8; margin-top: 0.2rem; }
</style>
<div id="kfmtPsPreviewWrap" style="display:none;">
  <div id="kfmtPsChunks"></div>
  <div id="kfmtPsMeta"></div>
</div>
<h2>Transmitter Status</h2>
<p class="text-body">The RDS rows show what the plugin last sent - the chip has no register that reports
what it is transmitting, so this is not proof of what a receiver decoded. The outlined box is the
station-name screen on air right now.</p>
<p class="text-body">Antenna Tuning shows the QN8027's antenna tuning register. The chip gives no readable measure of how good the match is, so judge it with a receiver. Retune re-runs the chip's calibration - do that after moving the antenna or its enclosure, and not during a show.</p>
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
    <tr><th scope="row">Antenna Tuning</th><td id="kfmtAnt">—</td></tr>
    <tr><th scope="row">Audio peak</th><td id="kfmtPeak">—</td></tr>
    <tr><th scope="row">Station name (PS)</th><td id="kfmtPsOnAir">—</td></tr>
    <tr><th scope="row">RadioText (RT)</th><td id="kfmtRtOnAir">—</td></tr>
    <tr><th scope="row">RDS source</th><td id="kfmtRdsSource">—</td></tr>
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
            // Before the early returns below: an override still applies while
            // the adapter is missing or the chip is not answering.
            kfmtSyncFields(s);
            kfmtUpdateOverrides(s);
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
            // "off" on its own is the thing people file bugs about. The reason
            // comes from the plugin, and is the same string it logged when it
            // put the radio in this state.
            if (s.carrierWhy) carrier += ' \u2014 ' + s.carrierWhy;
            kfmtSetText('kfmtCarrier', carrier, !s.transmitting && !s.transmitterForced);
            if (s.antRail) match += ' (ANT at rail)';
            kfmtSetText('kfmtAnt', '0x' + s.antHex, s.antRail);
            var peak = String(s.audioPeak) + ' / 15 (max-hold)';
            if (s.audioClip) peak += ' — clipping since last clear';
            kfmtSetText('kfmtPeak', peak, s.audioClip);
            kfmtSetText('kfmtLastRetune', s.lastRetune || 'none', s.lastRetune === 'out of range');
            kfmtRenderRds(s);
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
// A command can override the station ID, the RadioText and the idle
// behaviour without touching the configuration, which means the field on this
// page can be showing something that is not what is on the air. These notes sit
// with the field they contradict rather than down in the status table, because
// that is where someone reads the wrong value.
//
// Every lookup is best-effort: PrintSetting emits an id but no name, and the
// row and column markup around it is FPP's rather than ours, so a version that
// renders it differently has to end up with no note rather than a script error.
// Same reason kfmtFindStationIdInput() carries a chain of fallbacks.
function kfmtFindSettingField(id) {
    return document.getElementById(id)
        || document.querySelector('[name="' + id + '"]')
        || document.querySelector('#pluginSetting_' + id + ' input, #pluginSetting_' + id + ' select')
        || document.querySelector('[id*="' + id + '"]');
}
function kfmtOverrideNote(id, clearable) {
    var el = document.getElementById('kfmtOverride_' + id);
    if (el) return el;
    var field = kfmtFindSettingField(id);
    if (!field) return null;
    var col = document.querySelector('#' + id + 'Row .printSettingFieldCol')
        || (field.closest ? field.closest('.printSettingFieldCol') : null)
        || field.parentNode;
    if (!col) return null;
    el = document.createElement('div');
    el.id = 'kfmtOverride_' + id;
    el.className = 'kfmt-override';
    el.style.display = 'none';
    // The wording goes in its own span so refreshing the text does not take
    // the button with it, and the button lives inside the note so it appears
    // and disappears with the override instead of sitting there as a no-op.
    var span = document.createElement('span');
    span.id = 'kfmtOverrideText_' + id;
    el.appendChild(span);
    if (clearable) {
        var btn = document.createElement('button');
        btn.type = 'button';
        btn.className = 'buttons kfmt-override-clear';
        btn.id = 'kfmtOverrideClear_' + id;
        btn.textContent = 'Clear';
        btn.onclick = function () { kfmtClearOverride(id, btn); };
        el.appendChild(btn);
    }
    col.appendChild(el);
    return el;
}
function kfmtShowOverride(id, text, clearable) {
    var el = kfmtOverrideNote(id, clearable !== false);
    if (!el) return;
    var span = document.getElementById('kfmtOverrideText_' + id);
    if (span) span.textContent = text || '';
    el.style.display = text ? '' : 'none';
}
// Each override clears on its own. The field id is what the page knows; the
// endpoints are named separately because the route has to carry which one.
// Transmitter state is not here: it is a saved setting with its own field on
// this page, so its note explains rather than offering a button.
var kfmtOverrideEndpoints = {
    StationID: 'stationid',
    StationName: 'rdstext'
};
function kfmtClearOverride(id, btn) {
    var which = kfmtOverrideEndpoints[id];
    if (!which) return;
    btn.disabled = true;
    $.ajax({
        url: 'api/plugin-apis/kfmt/clearoverride/' + which,
        method: 'POST',
        dataType: 'json'
    }).done(function (s) {
        // The answer carries the fresh status when the chip is there; when it
        // is not it is only an acknowledgement, so poll as well rather than
        // leaving the note up until the next tick.
        kfmtUpdateOverrides(s);
        kfmtRefreshStatus();
    }).always(function () {
        btn.disabled = false;
    });
}
// A command can change Transmitter without the page knowing, which left the
// field showing something the plugin had stopped believing until someone
// reloaded. Put it back in step on the poll instead.
//
// Assigning .value does not fire a change event, so FPP's own onChange handler
// does not run and this can never write the setting back - it only ever
// reflects what the plugin reports.
function kfmtSyncSelect(id, value) {
    if (typeof value !== 'string' || value === '') return;
    var el = document.getElementById(id);
    if (!el || el.value === value) return;
    // Not while someone is actually using the control: a field that changes
    // under an open dropdown is worse than one that is briefly stale.
    if (document.activeElement === el) return;
    el.value = value;
}
function kfmtSyncFields(s) {
    kfmtSyncSelect('TransmitterState', s.transmitterStateValue);
}
// Only acts on a field the status actually carried. The handler's radio-thread
// timeout answers with a cut-down payload, and a missing key there means "not
// reported", not "no override" - clearing the notes on it would blink them off
// every time a status read timed out.
function kfmtUpdateOverrides(s) {
    var tail = ' Clears on restart, or run the command with an empty value.';
    // Not an override: while the transmitter is forced, Playlist Idle Behavior
    // decides nothing, so say so on the field itself rather than leaving it
    // looking like it is in charge.
    if (typeof s.transmitterForced === 'boolean') {
        kfmtShowOverride('IdleAction', s.transmitterForced
            ? 'Transmitter is set to ' + (s.transmitterState || 'a forced state')
              + ', so this setting has no effect. Set Transmitter back to '
              + 'Follow Idle Setting to use it.'
            : '', false);
    }
    if (typeof s.stationIdOverride === 'string') {
        kfmtShowOverride('StationID', s.stationIdOverride
            ? 'On the air now: "' + s.stationIdOverride + '", set by an FPP command.' + tail
            : '');
    }
    if (typeof s.rdsTextOverride === 'string') {
        kfmtShowOverride('StationName', s.rdsTextOverride
            ? 'RadioText is "' + s.rdsTextOverride + '", set by an FPP command. '
              + 'Station name, URL and song information are not being sent.' + tail
            : '');
    }
}
function kfmtFindStationIdInput() {
    return document.getElementById('StationID')
        || document.querySelector('input[name="StationID"]')
        || document.querySelector('#pluginSetting_StationID input')
        || document.querySelector('input[id*="StationID"]');
}
// One 8-character station-name screen, with spaces shown as dots so trailing
// padding is visible. Shared by the Station ID preview and the on-air rows, so
// the two always read the same way.
function kfmtPsChunk(part, title, live) {
    var box = document.createElement('span');
    box.className = 'kfmt-ps-chunk' + (live ? ' kfmt-ps-live' : '');
    box.title = title;
    for (var c = 0; c < part.length; c++) {
        if (part.charAt(c) === ' ') {
            var sp = document.createElement('span');
            sp.className = 'kfmt-ps-space';
            sp.textContent = '\u00b7';
            box.appendChild(sp);
        } else {
            box.appendChild(document.createTextNode(part.charAt(c)));
        }
    }
    return box;
}
// What is actually going out. Everything here is what the plugin recorded at
// the moment it sent it - no reading back from the chip, which has no register
// for it - so these rows cost nothing beyond the poll that was happening anyway.
function kfmtRenderRds(s) {
    var psEl = document.getElementById('kfmtPsOnAir');
    var rtEl = document.getElementById('kfmtRtOnAir');
    var srcEl = document.getElementById('kfmtRdsSource');
    if (!psEl || !rtEl || !srcEl) return;

    var off = null;
    if (s.rdsEnabled === false) off = 'RDS is turned off';
    else if (s.transmitting === false) off = 'not being sent \u2014 carrier is off';

    psEl.textContent = '';
    if (off) {
        psEl.textContent = off;
        rtEl.textContent = off;
        srcEl.textContent = '\u2014';
        return;
    }
    var full = s.psFullText || '';
    var screens = s.psScreens || 0;
    if (!screens) {
        psEl.textContent = '\u2014';
    } else {
        for (var i = 0; i < screens; i++) {
            var part = full.substr(i * 8, 8);
            while (part.length < 8) part += ' ';
            var live = (i + 1) === s.psScreen;
            psEl.appendChild(kfmtPsChunk(part, live ? 'On air now' : 'Screen ' + (i + 1), live));
        }
    }
    rtEl.textContent = s.rtOnAir || '\u2014';
    srcEl.textContent = s.rdsSource || '\u2014';
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
        chunksEl.appendChild(kfmtPsChunk(part, 'Screen ' + (i + 1), false));
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
