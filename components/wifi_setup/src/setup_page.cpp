// WiFi setup page (/wifi): scan, choose a network, enter the password.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

namespace wifi_setup {

extern const char kSetupPage[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32Klipper WiFi</title>
<style>
body{font-family:system-ui,sans-serif;background:#1d2125;color:#e8eaed;
margin:0;padding:16px;max-width:480px}
h1{font-size:20px;margin:0 0 12px}
#st{padding:10px;border-radius:6px;background:#2b3035;margin-bottom:12px}
button{font-size:16px;padding:10px 14px;border:0;border-radius:6px;
background:#2196f3;color:#fff}
ul{list-style:none;padding:0;margin:12px 0}
li{padding:12px;border-bottom:1px solid #33383d;cursor:pointer;display:flex;
justify-content:space-between}
li:hover{background:#2b3035}
input{font-size:16px;padding:10px;width:100%;box-sizing:border-box;
margin:6px 0 12px;border-radius:6px;border:1px solid #555;background:#111;
color:#eee}
.muted{color:#9aa0a6;font-size:14px}
</style></head><body>
<h1>ESP32Klipper WiFi</h1>
<div id="st">...</div>
<button onclick="scan()">Scan networks</button>
<ul id="list"></ul>
<form id="f" onsubmit="save(event)">
<label>Network (SSID)<input id="ssid" maxlength="32" required></label>
<label>Password<input id="pw" type="password" maxlength="64"></label>
<button type="submit">Connect</button>
</form>
<p class="muted">After connecting, open the address shown above for
Mainsail. This setup network turns off a minute after the board joins
your WiFi.</p>
<script>
const $=id=>document.getElementById(id);
function bars(r){return r>-55?'▂▄▆█':r>-67?'▂▄▆':
r>-78?'▂▄':'▂';}
async function status(){try{const s=await(await fetch('/wifi/status')).json();
let t=s.connected?'Connected to <b>'+esc(s.ssid)+'</b>: <a href="http://'+s.ip+
'/">http://'+s.ip+'/</a>':s.ssid?'Connecting to <b>'+esc(s.ssid)+'</b>'+
(s.message?' ('+esc(s.message)+')':''):'No network set up';
$('st').innerHTML=t;}catch(e){}}
function esc(s){const d=document.createElement('div');d.textContent=s;
return d.innerHTML;}
async function scan(){$('list').innerHTML='<li>Scanning...</li>';
const n=await(await fetch('/wifi/scan')).json();$('list').innerHTML='';
for(const w of n){const li=document.createElement('li');
li.innerHTML='<span>'+esc(w.ssid)+(w.secure?' 🔒':'')+
'</span><span>'+bars(w.rssi)+'</span>';
li.onclick=()=>{$('ssid').value=w.ssid;$('pw').value='';$('pw').focus();};
$('list').appendChild(li);}
if(!n.length)$('list').innerHTML='<li>No networks found</li>';}
async function save(e){e.preventDefault();
const b=new URLSearchParams({ssid:$('ssid').value,password:$('pw').value});
await fetch('/wifi/save',{method:'POST',body:b});
$('st').textContent='Connecting...';}
status();setInterval(status,2000);
</script></body></html>
)HTML";

} // namespace wifi_setup
