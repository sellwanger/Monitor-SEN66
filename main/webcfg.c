#include "webcfg.h"
#include "display.h"
#include "ha_mqtt.h"
#include "mic.h"
#include "net.h"
#include "pmu_axp2101.h"
#include "sound.h"
#include "ui.h"
#include "sen66.h"
#include "settings.h"
#include "version.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "mbedtls/base64.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "webcfg";

static httpd_handle_t s_server;
static webcfg_sample_fn s_get_sample;
static webcfg_recal_fn s_recal_request;
static webcfg_recal_status_fn s_recal_status;
static webcfg_fan_fn s_fan;

void webcfg_set_fan(webcfg_fan_fn fn) { s_fan = fn; }

void webcfg_set_co2_recal(webcfg_recal_fn request, webcfg_recal_status_fn status)
{
    s_recal_request = request;
    s_recal_status = status;
}

// ------------------------------------------------------------- utilidades
// snprintf acumulativo a prueba de desbordes: si el offset ya alcanzo el final
// del buffer, deja de escribir en vez de dejar que (size_t)(cap - n) con n > cap
// se convierta en un tamano gigante y el siguiente snprintf pise el stack.
static int sappend(char *buf, size_t cap, int n, const char *fmt, ...)
{
    if (n < 0 || (size_t)n >= cap) return (int)cap - 1;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf + n, cap - n, fmt, ap);
    va_end(ap);
    if (r < 0) return (int)cap - 1;
    if ((size_t)(n + r) >= cap) return (int)cap - 1;
    return n + r;
}

// Escapa una cadena para incrustarla entre comillas en JSON. Sin esto, unas
// comillas en un campo de texto (nombre del aparato, SSID, TZ...) rompen el
// JSON del panel y podrian colar claves ajenas.
static void json_escape(char *dst, size_t dstlen, const char *src)
{
    size_t j = 0;
    if (dstlen == 0) return;
    for (size_t i = 0; src && src[i] && j + 2 < dstlen; i++) {
        unsigned char c = (unsigned char)src[i];
        char esc = 0;
        if (c == '"' || c == '\\') esc = (char)c;
        else if (c == '\n') esc = 'n';
        else if (c == '\r') esc = 'r';
        else if (c == '\t') esc = 't';
        if (esc) { dst[j++] = '\\'; dst[j++] = esc; }
        else if (c >= 0x20) dst[j++] = (char)c;
    }
    dst[j] = '\0';
}

// ------------------------------------------------------------- seguridad
// El panel puede protegerse con Basic Auth (usuario/clave en los ajustes).
// Clave vacia = sin autenticacion (comportamiento historico, panel abierto en
// la red local). Durante el portal de aprovisionamiento NO se exige: es la
// primera configuracion y se asume presencia fisica.
static bool auth_ok(httpd_req_t *req)
{
    const settings_t *c = settings_get();
    if (c->web_pass[0] == '\0') return true;
    if (net_state() == NET_PORTAL) return true;

    char hdr[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK)
        return false;
    if (strncmp(hdr, "Basic ", 6) != 0) return false;

    const char *user = c->web_user[0] ? c->web_user : "admin";
    char cred[100];
    int m = snprintf(cred, sizeof(cred), "%s:%s", user, c->web_pass);
    if (m < 0 || m >= (int)sizeof(cred)) return false;

    unsigned char enc[160];
    size_t enclen = 0;
    if (mbedtls_base64_encode(enc, sizeof(enc), &enclen,
                              (const unsigned char *)cred, (size_t)m) != 0)
        return false;
    // Comparacion de longitud fija tras validar el tamano: ni corta ni larga.
    return strlen(hdr + 6) == enclen &&
           strncmp(hdr + 6, (const char *)enc, enclen) == 0;
}

static esp_err_t need_auth(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Monitor SEN66\"");
    httpd_resp_sendstr(req, "Anmeldung erforderlich");
    return ESP_FAIL;
}

// El Host tiene que ser la IP del aparato (o la del portal). Un dominio del
// atacante que resuelva a esa IP (DNS rebinding) llegaria con SU nombre en el
// Host y aqui se rechaza. El panel se usa siempre por IP, asi que no estorba.
static bool host_ok(httpd_req_t *req)
{
    char host[64];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK)
        return true; // sin Host (HTTP/1.0): no aplicamos la defensa
    char *colon = strchr(host, ':');
    if (colon) *colon = '\0';
    if (strcmp(host, "192.168.4.1") == 0) return true;
    const char *ip = net_ip();
    return ip[0] && strcmp(host, ip) == 0;
}

// Cabecera propia que el navegador solo deja poner a JS del MISMO origen: una
// peticion cross-site (otra web, un enlace, un formulario ajeno) no puede
// anadirla sin un preflight CORS que no respondemos. Cierra CSRF en las rutas
// que leen o cambian estado por fetch().
static bool csrf_ok(httpd_req_t *req)
{
    char v[8];
    return httpd_req_get_hdr_value_str(req, "X-CSRF", v, sizeof(v)) == ESP_OK;
}

// Puerta comun de la API (rutas servidas por fetch() desde el propio panel):
// Host correcto + mismo origen + clave si la hay. Devuelve ESP_OK o ya ha
// enviado el error correspondiente.
static esp_err_t guard_api(httpd_req_t *req)
{
    if (!host_ok(req)) { httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Host nicht erlaubt"); return ESP_FAIL; }
    if (!csrf_ok(req)) { httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Cross-Site-Anfrage blockiert"); return ESP_FAIL; }
    if (!auth_ok(req)) return need_auth(req);
    return ESP_OK;
}

// ------------------------------------------------------------------- pagina
// Una sola pagina, sin dependencias externas (no hay internet en el portal).
static const char k_page[] =
"<!doctype html><html lang=de><meta charset=utf-8>\n"
"<meta name=viewport content='width=device-width,initial-scale=1'>\n"
"<title>Monitor SEN66</title><style>\n"
"*{box-sizing:border-box}\n"
"body{margin:0;padding:16px;font:15px/1.55 system-ui,sans-serif;background:#0f172a;color:#e2e8f0;max-width:720px;margin-inline:auto}\n"
"h1{font-size:21px;margin:0 0 2px}\n"
"h2{font-size:13px;margin:24px 0 8px;color:#94a3b8;text-transform:uppercase;letter-spacing:.07em}\n"
".card{background:#1e293b;border-radius:14px;padding:16px;margin-bottom:12px}\n"
"label{display:block;margin:12px 0 4px;color:#cbd5e1;font-size:14px}\n"
"label:first-child{margin-top:0}\n"
".hint{color:#64748b;font-size:12.5px;margin-top:4px}\n"
"input,select{width:100%;padding:9px 10px;border-radius:9px;border:1px solid #334155;background:#0f172a;color:#e2e8f0;font-size:15px}\n"
"input[type=range]{padding:0;border:0;background:0;height:26px}\n"
"input[type=checkbox]{width:auto;margin:0 8px 0 0;transform:scale(1.25)}\n"
"button{margin-top:14px;padding:11px 18px;border:0;border-radius:9px;background:#2563eb;color:#fff;font-weight:600;font-size:15px;cursor:pointer}\n"
"button.alt{background:#334155}\n"
"button:hover{filter:brightness(1.12)}\n"
".grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}\n"
"@media(max-width:540px){.grid{grid-template-columns:1fr}}\n"
"table{width:100%;border-collapse:collapse}\n"
"td{padding:6px 0;border-bottom:1px solid #334155}\n"
"td:last-child{text-align:right;font-variant-numeric:tabular-nums;font-weight:600}\n"
"tr:last-child td{border-bottom:0}\n"
".ok{color:#22c55e}.warn{color:#facc15}\n"
"#msg{margin-top:10px;color:#facc15}\n"
".pg{display:flex;align-items:center;margin:6px 0;color:#e2e8f0;font-size:14.5px}\n"
".val{float:right;color:#94a3b8;font-variant-numeric:tabular-nums}\n"
".row{display:flex;gap:10px;flex-wrap:wrap}\n"
".row button{margin-top:0}\n"
"</style>\n"
"<h1>Monitor SEN66</h1><div id=sub class=hint></div>\n"
"\n"
"<h2>Aktuell</h2><div class=card><table id=st></table></div>\n"
"\n"
"<h2>Netzwerk</h2><div class=card><form id=f>\n"
"<div class=grid>\n"
"<div><label>WLAN-Netz</label><input name=wifi_ssid autocomplete=off></div>\n"
"<div><label>WLAN-Passwort</label><input name=wifi_pass type=password placeholder='(unverändert)' autocomplete=new-password></div>\n"
"</div>\n"
"<div class=grid>\n"
"<div><label>Zeitzone</label><select name=tz>\n"
"<option value='CET-1CEST,M3.5.0,M10.5.0/3'>Mitteleuropa (Deutschland)</option>\n"
"<option value='WET0WEST,M3.5.0/1,M10.5.0'>Westeuropa</option>\n"
"<option value='EET-2EEST,M3.5.0/3,M10.5.0/4'>Osteuropa</option>\n"
"<option value='GMT0BST,M3.5.0/1,M10.5.0'>Großbritannien</option>\n"
"<option value='EST5EDT,M3.2.0,M11.1.0'>New York</option>\n"
"<option value='UTC0'>UTC</option>\n"
"</select></div>\n"
"<div><label>Zeitserver</label><input name=ntp></div>\n"
"</div>\n"
"<div class=hint>Fehlt deine Zone, akzeptiert das Feld beim Wiederherstellen einer Sicherung jeden POSIX-TZ-String.</div>\n"
"\n"
"<h2 style='margin-top:22px'>Panel-Zugang</h2>\n"
"<div class=grid>\n"
"<div><label>Benutzer</label><input name=web_user autocomplete=off placeholder=admin></div>\n"
"<div><label>Panel-Passwort</label><input name=web_pass type=password placeholder='(unverändert)' autocomplete=new-password></div>\n"
"</div>\n"
"<div class=hint>Leer lassen = kein Passwort (Panel im lokalen Netz offen). Mit Passwort verlangen alle Panel-Funktionen eine Anmeldung; nur die Ersteinrichtung über den Setup-Hotspot bleibt frei.</div>\n"
"\n"
"<h2 style='margin-top:22px'>Home Assistant und HomeKit</h2>\n"
"<label>MQTT-Broker</label><input name=mqtt_uri placeholder='mqtt://192.168.1.10:1883'>\n"
"<div class=hint>Leer lassen, wenn du keine Hausautomation willst: das Gerät läuft trotzdem eigenständig.</div>\n"
"<div class=grid>\n"
"<div><label>Benutzer</label><input name=mqtt_user autocomplete=off></div>\n"
"<div><label>Passwort</label><input name=mqtt_pass type=password placeholder='(unverändert)' autocomplete=new-password></div>\n"
"</div>\n"
"<div class=grid>\n"
"<div><label>Gerätename</label><input name=device_name></div>\n"
"<div><label>Discovery-Präfix</label><input name=mqtt_prefix placeholder=homeassistant></div>\n"
"</div>\n"
"<div class=hint>Das Präfix lässt Home Assistant das Gerät selbst anlegen. Mit Homebridge <b>leer</b> lassen.</div>\n"
"\n"
"<h2 style='margin-top:22px'>Anzeige</h2>\n"
"<label>Sprache</label><select name=lang>\n"
"<option value=de>Deutsch</option><option value=en>English</option><option value=es>Espanol</option>\n"
"</select>\n"
"<label>Helligkeit <span class=val id=bv></span></label>\n"
"<input name=brightness type=range min=1 max=255 oninput=vivo(this)>\n"
"<label>Helligkeit gedimmt <span class=val id=nv></span></label>\n"
"<input name=night_brightness type=range min=0 max=255 oninput=vivo(this)>\n"
"<div class=hint>Werden sofort sichtbar; gespeichert wird erst mit Speichern.</div>\n"
"<div class=grid>\n"
"<div><label>Dimmen nach</label><select name=screen_timeout_s>\n"
"<option value=0>Nie</option><option value=30>30 Sekunden</option>\n"
"<option value=45>45 Sekunden</option><option value=60>1 Minute</option>\n"
"<option value=300>5 Minuten</option><option value=900>15 Minuten</option>\n"
"</select></div>\n"
"<div><label>Diagramm-Zeitfenster</label><select name=chart_span_min>\n"
"<option value=30>30 Minuten</option><option value=60>1 Stunde</option>\n"
"<option value=180>3 Stunden</option><option value=360>6 Stunden</option>\n"
"<option value=720>12 Stunden</option><option value=1440>24 Stunden</option>\n"
"</select></div>\n"
"</div>\n"
"<label>Sichtbare Seiten</label>\n"
"<div class=pg><input type=checkbox id=pg0><span>Gesamt</span></div>\n"
"<div class=pg><input type=checkbox id=pg1><span>CO2</span></div>\n"
"<div class=pg><input type=checkbox id=pg2><span>Feinstaub</span></div>\n"
"<div class=pg><input type=checkbox id=pg3><span>Gase (VOC und NOx)</span></div>\n"
"<div class=pg><input type=checkbox id=pg4><span>Klima</span></div>\n"
"<div class=pg><input type=checkbox id=pg5><span>Lärm</span></div>\n"
"\n"
"<h2 style='margin-top:22px'>Sensor</h2>\n"
"<div class=grid>\n"
"<div><label>Temperatur-Korrektur</label><input name=temp_offset type=number step=0.1></div>\n"
"<div><label>Höhe (m)</label><input name=altitude_m type=number min=0 max=3000></div>\n"
"</div>\n"
"<div class=hint>Die Korrektur wird zum Messwert ADDIERT: zeigt er zu viel, negativ eintragen. Vorher mit einem echten Thermometer vergleichen, nicht mit einem anderen Luftmessgerät.</div>\n"
"<label>CO2-Autokalibrierung</label><select name=co2_asc>\n"
"<option value=1>An (empfohlen)</option><option value=0>Aus</option>\n"
"</select>\n"
"<div class=hint>Setzt voraus, dass das Gerät ab und zu Frischluft sieht. In einem nie gelüfteten Raum ausschalten.</div>\n"
"<label>Lärm-Kalibrierung (dB)</label><input name=noise_offset_db type=number min=0 max=200>\n"
"<div class=hint>Wird zum Mikrofonpegel addiert. Mit einem Schallpegelmesser daneben und etwas Lärm im Raum einstellen, nicht in Stille.</div>\n"
"\n"
"<h2 style='margin-top:22px'>Akkubetrieb</h2>\n"
"<label>Sensor im Akkubetrieb zyklisch pausieren</label><select name=batt_saver>\n"
"<option value=0>Nein (Dauermessung)</option><option value=1>Ja (Sparmodus)</option>\n"
"</select>\n"
"<div class=grid>\n"
"<div><label>Messfenster (s)</label><input name=batt_on_s type=number min=60 max=3600></div>\n"
"<div><label>Zyklus (s)</label><input name=batt_period_s type=number min=120 max=7200></div>\n"
"</div>\n"
"<div class=hint>Greift nur ohne USB. Der SEN66 zieht beim Messen ~90 mA, in der Pause ~3 mA; mit 180/600 s halbiert sich der Sensorverbrauch grob. Preis: NOx ist in kurzen Fenstern praktisch blind, die CO2-Autokalibrierung setzt Dauerbetrieb voraus, und die ersten ~30 s jedes Fensters braucht der Feinstaub zum Einschwingen. PM, Temperatur, Feuchte und VOC bleiben brauchbar. Zyklus muss mindestens 60 s länger sein als das Fenster.</div>\n"
"\n"
"<h2 style='margin-top:22px'>Akustischer Alarm</h2>\n"
"<label>Warnen, wenn CO2 steigt</label><select name=alarm_enabled>\n"
"<option value=1>Ja</option><option value=0>Nein</option>\n"
"</select>\n"
"<div class=grid>\n"
"<div><label>Warnen über (ppm)</label><input name=alarm_co2_ppm type=number min=400 max=5000></div>\n"
"<div><label>Ruhe unter (ppm)</label><input name=alarm_clear_ppm type=number min=400 max=5000></div>\n"
"</div>\n"
"<label>Lautstärke <span class=val id=av></span></label>\n"
"<input name=alarm_volume type=range min=0 max=100 oninput=vivo(this)>\n"
"<div class=hint>Die Ruhe-Schwelle muss NIEDRIGER sein als die Warn-Schwelle: dieser Abstand verhindert Dauerpiepen, wenn CO2 um die Grenze pendelt. Falsch herum wird es automatisch korrigiert.</div>\n"
"\n"
"<button type=submit>Speichern und neu starten</button></form><div id=msg></div></div>\n"
"\n"
"<h2>Wartung</h2><div class=card>\n"
"<div class=row>\n"
"<button class=alt onclick=\"post('/api/fanclean')\">Lüfter reinigen</button>\n"
"<button class=alt onclick=\"post('/api/beep')\">Ton testen</button>\n"
"<button class=alt onclick=\"post('/api/reboot')\">Neu starten</button>\n"
"</div>\n"
"<label style='margin-top:16px'>CO2 gegen eine Referenz neu kalibrieren</label>\n"
"<div class=grid><div><input id=frc type=number value=420 min=400 max=2000></div>\n"
"<div><button class=alt style='margin-top:0;width:100%' onclick=recal()>Kalibrieren</button></div></div>\n"
"<div class=hint style='color:#facc15'>Nur mit einer echten Referenz: Gerät ins Freie bringen, weg von Personen, 5 Minuten messen lassen und 420 ppm verwenden. Ein erfundener Wert verschlechtert die Messung und wird im Sensor gespeichert.</div>\n"
"<div id=frcmsg class=hint></div>\n"
"<label style='margin-top:16px'>Firmware aktualisieren (.bin)</label>\n"
"<input type=file id=fw accept='.bin'>\n"
"<button class=alt onclick=ota()>Hochladen und installieren</button>\n"
"<div id=otamsg class=hint></div></div>\n"
"\n"
"<h2>Sicherung</h2><div class=card>\n"
"<div class=hint>Speichert die Einstellungen in einer Datei, um sie nach einem Speicherverlust wiederherzustellen.</div>\n"
"<div class=row style='margin-top:12px'>\n"
"<button type=button class=alt onclick=\"location='/api/backup'\">Herunterladen</button>\n"
"<button type=button class=alt onclick=\"location='/api/backup?secrets=1'\">Mit Passwörtern herunterladen</button>\n"
"</div>\n"
"<div class=hint style='color:#facc15'>Die Datei mit Passwörtern enthält dein WLAN-Passwort im Klartext und erfordert ein gesetztes Panel-Passwort. Bewahre sie so sicher auf wie ein Passwort.</div>\n"
"<label>Aus Datei wiederherstellen</label><input type=file id=bk accept=.json>\n"
"<button type=button onclick=restore()>Wiederherstellen</button>\n"
"<div id=bkmsg class=hint></div></div>\n"
"\n"
"<script>\n"
"const H={'X-CSRF':'1'};\n" // cabecera anti-CSRF en TODAS las peticiones fetch
"const M={co2:['CO2','ppm'],pm25:['PM2.5','ug/m3'],pm10:['PM10','ug/m3'],\n"
"pm1:['PM1.0','ug/m3'],pm4:['PM4.0','ug/m3'],temperature:['Temperatur','C'],\n"
"humidity:['Feuchte','%'],voc:['VOC',''],nox:['NOx',''],noise:['Lärm','dB']};\n"
"const NIV={good:'Gut',fair:'Mäßig',moderate:'Mittel',poor:'Schlecht',bad:'Sehr schlecht'};\n"
"function vivo(e){\n"
" const s={brightness:'bv',night_brightness:'nv',alarm_volume:'av'}[e.name];\n"
" if(s)document.getElementById(s).textContent=e.value;\n"
" if(e.name=='brightness')fetch('/api/brightness?v='+e.value,{method:'POST',headers:H});\n"
"}\n"
"async function refresh(){const s=await(await fetch('/api/state',{headers:H})).json();\n"
" document.getElementById('sub').textContent=s.name+' - v'+s.version+' - '+s.id+' - '+s.ip;\n"
" let h='';for(const k in M){const v=s[k];\n"
"  h+='<tr><td>'+M[k][0]+'</td><td>'+(v==null?'--':v+' '+M[k][1])+'</td></tr>'}\n"
" h+='<tr><td>Luftqualität</td><td>'+(NIV[s.level]||s.level||'--')+'</td></tr>';\n"
" h+='<tr><td>Sensor</td><td class='+(s.sensor=='OK'?'ok':'warn')+'>'+s.sensor+'</td></tr>';\n"
" h+='<tr><td>Home Assistant</td><td class='+(s.mqtt?'ok':'warn')+'>'+(s.mqtt?'verbunden':'keine Verbindung')+'</td></tr>';\n"
" if(s.bat_pct!=null)h+='<tr><td>Akku</td><td>'+s.bat_pct+' % '+(s.charging?'(lädt)':(s.usb?'(mit USB)':'(im Akkubetrieb)'))+'</td></tr>';\n"
" document.getElementById('st').innerHTML=h;\n"
" if(s.co2_recal)document.getElementById('frcmsg').textContent=s.co2_recal}\n"
"async function load(){const c=await(await fetch('/api/settings',{headers:H})).json();\n"
" for(const [k,v] of Object.entries(c)){\n"
"  if(k=='pages_mask'){for(let i=0;i<6;i++)document.getElementById('pg'+i).checked=!!(v>>i&1);continue}\n"
"  const e=document.forms.f[k];if(!e)continue;\n"
"  e.value=v;\n"
"  if(e.type=='select-one'&&e.selectedIndex<0){\n"
"   const o=document.createElement('option');o.value=v;o.textContent=v+' (aktuell)';\n"
"   e.appendChild(o);e.value=v}\n"
"  if(e.type=='range'){const s={brightness:'bv',night_brightness:'nv',alarm_volume:'av'}[k];\n"
"   if(s)document.getElementById(s).textContent=e.value}}}\n"
"document.getElementById('f').onsubmit=async ev=>{ev.preventDefault();\n"
" const d=Object.fromEntries(new FormData(ev.target));\n"
" let m=0;for(let i=0;i<6;i++)if(document.getElementById('pg'+i).checked)m|=1<<i;\n"
" d.pages_mask=m||1;\n"
" const r=await fetch('/api/settings',{method:'POST',headers:H,body:JSON.stringify(d)});\n"
" document.getElementById('msg').textContent=r.ok?'Gespeichert. Neustart...':'Fehler beim Speichern'};\n"
"async function post(u){const r=await fetch(u,{method:'POST',headers:H});\n"
" document.getElementById('msg').textContent=r.ok?'Erledigt':'Fehler'}\n"
"async function recal(){const p=document.getElementById('frc').value;\n"
" const m=document.getElementById('frcmsg');\n"
" if(!confirm('CO2 auf '+p+' ppm neu kalibrieren? Wird im Sensor gespeichert.'))return;\n"
" m.textContent='Anfrage läuft...';\n"
" const r=await fetch('/api/co2recal?ppm='+p,{method:'POST',headers:H});\n"
" m.textContent=await r.text()}\n"
"async function ota(){const f=document.getElementById('fw').files[0];if(!f)return;\n"
" const m=document.getElementById('otamsg');m.textContent='Lade '+f.size+' Bytes hoch...';\n"
" const r=await fetch('/api/ota',{method:'POST',headers:H,body:f});\n"
" m.textContent=r.ok?'Installiert, Neustart...':'Fehler: '+await r.text()}\n"
"async function restore(){const f=document.getElementById('bk').files[0];\n"
" const m=document.getElementById('bkmsg');if(!f){m.textContent='Datei wählen';return}\n"
" let d;try{d=JSON.parse(await f.text())}catch(e){m.textContent='Kein gültiges JSON';return}\n"
" const r=await fetch('/api/settings',{method:'POST',headers:H,body:JSON.stringify(d)});\n"
" m.textContent=r.ok?'Wiederhergestellt. Neustart...':'Fehler beim Wiederherstellen'}\n"
"load();refresh();setInterval(refresh,5000);\n"
"</script></html>\n";

static esp_err_t h_root(httpd_req_t *req)
{
    // La pagina en si no cambia estado; solo pedimos clave (si la hay) para que
    // el navegador muestre el dialogo y cachee las credenciales para la API.
    // Sin Host/CSRF aqui, para no romper el acceso por marcador o IP escrita.
    if (!auth_ok(req)) return need_auth(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, k_page, HTTPD_RESP_USE_STRLEN);
}

// -------------------------------------------------------------------- estado
static esp_err_t h_state(httpd_req_t *req)
{
    if (guard_api(req) != ESP_OK) return ESP_FAIL;

    air_sample_t s;
    air_sample_clear(&s);
    if (s_get_sample) s_get_sample(&s);

    uint32_t status = 0;
    char sensor[96];
    if (!sen66_present()) {
        snprintf(sensor, sizeof(sensor), "nicht erkannt");
    } else if (sen66_read_status(&status) == ESP_OK) {
        sen66_status_text(status, sensor, sizeof(sensor));
    } else {
        snprintf(sensor, sizeof(sensor), "keine Antwort");
    }

    char name[80], sensor_e[192];
    json_escape(name, sizeof(name), settings_get()->device_name);
    json_escape(sensor_e, sizeof(sensor_e), sensor);

    char json[1024];
    int n = 0;
    n = sappend(json, sizeof(json), n,
        "{\"name\":\"%s\",\"version\":\"%s\",\"id\":\"%s\",\"ip\":\"%s\","
        "\"rssi\":%d,\"mqtt\":%s,\"sensor\":\"%s\",\"age_s\":%u,",
        name, APP_VERSION, net_device_id(),
        net_ip()[0] ? net_ip() : "sin IP", net_rssi(),
        ha_mqtt_connected() ? "true" : "false", sensor_e, (unsigned)s.age_s);

    uint32_t idle_s = 0; bool dimmed = false, idle_shown = false;
    ui_idle_debug(&idle_s, &dimmed, &idle_shown);
    n = sappend(json, sizeof(json), n,
                "\"idle_s\":%u,\"dimmed\":%s,\"idle_view\":%s,",
                (unsigned)idle_s, dimmed ? "true" : "false",
                idle_shown ? "true" : "false");

    // Bateria: es lo unico que se puede consultar con el USB fuera, asi que
    // es la herramienta para medir el consumo real en descarga.
    pmu_status_t b;
    if (pmu_available() && pmu_read(&b) == ESP_OK && b.present) {
        n = sappend(json, sizeof(json), n,
                    "\"bat_pct\":%d,\"bat_mv\":%u,\"charging\":%s,\"usb\":%s,",
                    b.percent, b.millivolts,
                    b.charging ? "true" : "false", b.vbus ? "true" : "false");
    }

    for (int m = 0; m < AIR_METRIC_COUNT; m++) {
        const char *key = air_metric_key((air_metric_t)m);
        if (isnan(s.v[m])) {
            n = sappend(json, sizeof(json), n, "\"%s\":null,", key);
        } else {
            n = sappend(json, sizeof(json), n, "\"%s\":%.*f,", key,
                        air_metric_decimals((air_metric_t)m), s.v[m]);
        }
    }
    n = sappend(json, sizeof(json), n, "\"level\":\"%s\"",
                s.valid ? air_level_text(air_overall(&s)) : "");

    {
        const float lvl = mic_level_dbfs(), pk = mic_peak_dbfs();
        n = sappend(json, sizeof(json), n, ",\"mic\":\"%s\"", mic_status());
        if (!isnan(lvl)) {
            n = sappend(json, sizeof(json), n,
                        ",\"noise_dbfs\":%.1f,\"noise_peak_dbfs\":%.1f", lvl, pk);
        }
    }

    if (s_recal_status) {
        char recal[96], recal_e[192];
        s_recal_status(recal, sizeof(recal));
        if (recal[0]) {
            json_escape(recal_e, sizeof(recal_e), recal);
            n = sappend(json, sizeof(json), n, ",\"co2_recal\":\"%s\"", recal_e);
        }
    }
    n = sappend(json, sizeof(json), n, "}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n);
}

// ------------------------------------------------------------- configuracion
static esp_err_t h_settings_get(httpd_req_t *req)
{
    if (guard_api(req) != ESP_OK) return ESP_FAIL;

    const settings_t *c = settings_get();
    char json[1200];
    int n = 0;
    char e[196]; // buffer de escape reutilizado: sappend lo copia al momento
    // Las contrasenas nunca se devuelven; el formulario las deja en blanco y
    // solo se cambian si se escribe algo. Todos los campos de texto se escapan.
    n = sappend(json, sizeof(json), n, "{");
    json_escape(e, sizeof(e), c->wifi_ssid);   n = sappend(json, sizeof(json), n, "\"wifi_ssid\":\"%s\",", e);
    json_escape(e, sizeof(e), c->mqtt_uri);    n = sappend(json, sizeof(json), n, "\"mqtt_uri\":\"%s\",", e);
    json_escape(e, sizeof(e), c->mqtt_user);   n = sappend(json, sizeof(json), n, "\"mqtt_user\":\"%s\",", e);
    json_escape(e, sizeof(e), c->mqtt_prefix); n = sappend(json, sizeof(json), n, "\"mqtt_prefix\":\"%s\",", e);
    json_escape(e, sizeof(e), c->device_name); n = sappend(json, sizeof(json), n, "\"device_name\":\"%s\",", e);
    json_escape(e, sizeof(e), c->web_user);    n = sappend(json, sizeof(json), n, "\"web_user\":\"%s\",", e);
    json_escape(e, sizeof(e), c->lang);        n = sappend(json, sizeof(json), n, "\"lang\":\"%s\",", e);
    json_escape(e, sizeof(e), c->tz);          n = sappend(json, sizeof(json), n, "\"tz\":\"%s\",", e);
    json_escape(e, sizeof(e), c->ntp);         n = sappend(json, sizeof(json), n, "\"ntp\":\"%s\",", e);
    n = sappend(json, sizeof(json), n,
        "\"brightness\":%u,\"night_brightness\":%u,\"screen_timeout_s\":%u,"
        "\"page_dwell_s\":%u,\"chart_span_min\":%u,\"pages_mask\":%u,"
        "\"temp_offset\":%.1f,\"altitude_m\":%u,\"co2_asc\":%d,"
        "\"alarm_enabled\":%d,\"alarm_co2_ppm\":%u,\"alarm_clear_ppm\":%u,"
        "\"alarm_volume\":%u,\"noise_offset_db\":%d,"
        "\"batt_saver\":%d,\"batt_on_s\":%u,\"batt_period_s\":%u}",
        c->brightness, c->night_brightness, c->screen_timeout_s,
        c->page_dwell_s, c->chart_span_min, c->pages_mask,
        c->temp_offset_dc / 10.0f, c->altitude_m, c->co2_asc ? 1 : 0,
        c->alarm_enabled ? 1 : 0, c->alarm_co2_ppm, c->alarm_clear_ppm,
        c->alarm_volume, c->noise_offset_db,
        c->batt_saver ? 1 : 0, c->batt_on_s, c->batt_period_s);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n);
}

static void copy_str(const cJSON *root, const char *key, char *dst, size_t len,
                     bool skip_if_empty)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(it) || !it->valuestring) return;
    if (skip_if_empty && it->valuestring[0] == '\0') return;
    strlcpy(dst, it->valuestring, len);
}

// Los <input> llegan como cadenas aunque sean numericos, asi que se aceptan
// las dos formas.
static bool get_num(const cJSON *root, const char *key, double *out)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(it)) { *out = it->valuedouble; return true; }
    if (cJSON_IsString(it) && it->valuestring && it->valuestring[0]) {
        *out = atof(it->valuestring);
        return true;
    }
    return false;
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(700)); // deja salir la respuesta HTTP
    esp_restart();
}

static esp_err_t h_settings_post(httpd_req_t *req)
{
    if (guard_api(req) != ESP_OK) return ESP_FAIL;
    if (req->content_len == 0 || req->content_len > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Anfrage ohne Inhalt");
        return ESP_FAIL;
    }
    char *body = malloc(req->content_len + 1);
    if (!body) return ESP_FAIL;

    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) { free(body); return ESP_FAIL; }
        got += r;
    }
    body[got] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON fehlerhaft");
        return ESP_FAIL;
    }

    settings_t *c = settings_get();
    copy_str(root, "wifi_ssid", c->wifi_ssid, sizeof(c->wifi_ssid), false);
    copy_str(root, "wifi_pass", c->wifi_pass, sizeof(c->wifi_pass), true);
    copy_str(root, "mqtt_uri", c->mqtt_uri, sizeof(c->mqtt_uri), false);
    copy_str(root, "mqtt_user", c->mqtt_user, sizeof(c->mqtt_user), false);
    copy_str(root, "mqtt_pass", c->mqtt_pass, sizeof(c->mqtt_pass), true);
    copy_str(root, "mqtt_prefix", c->mqtt_prefix, sizeof(c->mqtt_prefix), true);
    copy_str(root, "device_name", c->device_name, sizeof(c->device_name), true);
    copy_str(root, "web_user", c->web_user, sizeof(c->web_user), false);
    // web_pass vacio = no tocar (igual que las demas contrasenas): asi el panel
    // no borra la clave solo porque el campo se envie en blanco.
    copy_str(root, "web_pass", c->web_pass, sizeof(c->web_pass), true);
    copy_str(root, "lang", c->lang, sizeof(c->lang), true);
    copy_str(root, "tz", c->tz, sizeof(c->tz), true);
    copy_str(root, "ntp", c->ntp, sizeof(c->ntp), true);

    double d;
    if (get_num(root, "brightness", &d))       c->brightness = (uint8_t)(d < 1 ? 1 : (d > 255 ? 255 : d));
    if (get_num(root, "night_brightness", &d)) c->night_brightness = (uint8_t)(d < 0 ? 0 : (d > 255 ? 255 : d));
    if (get_num(root, "screen_timeout_s", &d)) c->screen_timeout_s = (uint16_t)(d < 0 ? 0 : d);
    if (get_num(root, "page_dwell_s", &d))     c->page_dwell_s = (uint16_t)(d < 0 ? 0 : d);
    if (get_num(root, "chart_span_min", &d))   c->chart_span_min = (uint16_t)(d < 5 ? 5 : (d > 1440 ? 1440 : d));
    if (get_num(root, "pages_mask", &d))       c->pages_mask = (uint8_t)(d < 1 ? 1 : (d > 63 ? 63 : d));
    if (get_num(root, "temp_offset", &d))      c->temp_offset_dc = (int16_t)lrint(d * 10.0);
    if (get_num(root, "altitude_m", &d))       c->altitude_m = (uint16_t)(d < 0 ? 0 : (d > 3000 ? 3000 : d));
    if (get_num(root, "co2_asc", &d))          c->co2_asc = (d != 0);
    if (get_num(root, "alarm_enabled", &d))    c->alarm_enabled = (d != 0);
    if (get_num(root, "alarm_co2_ppm", &d))    c->alarm_co2_ppm = (uint16_t)(d < 400 ? 400 : (d > 5000 ? 5000 : d));
    if (get_num(root, "alarm_clear_ppm", &d))  c->alarm_clear_ppm = (uint16_t)(d < 400 ? 400 : (d > 5000 ? 5000 : d));
    if (get_num(root, "alarm_volume", &d))     c->alarm_volume = (uint8_t)(d < 0 ? 0 : (d > 100 ? 100 : d));
    if (get_num(root, "noise_offset_db", &d)) c->noise_offset_db = (int16_t)(d < 0 ? 0 : (d > 200 ? 200 : d));
    if (get_num(root, "batt_saver", &d))       c->batt_saver = (d != 0);
    if (get_num(root, "batt_on_s", &d))        c->batt_on_s = (uint16_t)(d < 60 ? 60 : (d > 3600 ? 3600 : d));
    if (get_num(root, "batt_period_s", &d))    c->batt_period_s = (uint16_t)(d < 120 ? 120 : (d > 7200 ? 7200 : d));
    cJSON_Delete(root);

    // El ciclo tiene que dejar una pausa real: si no, el ahorro no ahorra y el
    // sensor se pasaria el dia parando y arrancando (1,4 s cada parada).
    if (c->batt_period_s < c->batt_on_s + 60) {
        c->batt_period_s = c->batt_on_s + 60;
        ESP_LOGW(TAG, "ciclo de ahorro demasiado corto: subido a %u s", c->batt_period_s);
    }

    // La histeresis solo existe si el umbral de callar queda POR DEBAJO del de
    // avisar. Igualados o al reves, el aviso se dispararia y se cancelaria en
    // la misma muestra: mejor corregirlo aqui que dejar el aparato pitando.
    if (c->alarm_clear_ppm >= c->alarm_co2_ppm) {
        c->alarm_clear_ppm = (c->alarm_co2_ppm > 500) ? c->alarm_co2_ppm - 200 : 400;
        ESP_LOGW(TAG, "umbral de rearme por encima del de aviso: bajado a %u ppm",
                 c->alarm_clear_ppm);
    }

    display_set_brightness(c->brightness); // esto si se nota al instante
    sound_set_volume(c->alarm_volume);     // para que el boton de prueba use el nuevo

    esp_err_t err = settings_save();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Speichern fehlgeschlagen");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "ok");
    // Reiniciar es la forma honesta de aplicar red, MQTT, paginas y ajustes
    // del sensor de una vez, sin medio estado a medias.
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// ----------------------------------------------------------- mantenimiento
// Copia de seguridad de los ajustes. Se descarga por navegacion (location=...),
// no por fetch, asi que NO lleva la cabecera X-CSRF; por eso aqui no pasa por
// guard_api, sino por Host + Basic Auth. La copia CON contrasenas (?secrets=1)
// exige ademas que haya clave de panel puesta: sin ella nos negamos a soltar la
// del WiFi en claro, que era la fuga original.
static esp_err_t h_backup(httpd_req_t *req)
{
    if (!host_ok(req)) { httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Host nicht erlaubt"); return ESP_FAIL; }
    if (!auth_ok(req)) return need_auth(req);

    bool con_secretos = false;
    char q[32];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char v[8];
        if (httpd_query_key_value(q, "secrets", v, sizeof(v)) == ESP_OK && v[0] == '1') {
            con_secretos = true;
        }
    }

    const settings_t *c = settings_get();
    if (con_secretos && c->web_pass[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
            "Zum Export der Zugangsdaten zuerst ein Panel-Passwort setzen");
        return ESP_FAIL;
    }

    char json[1200];
    int n = 0;
    char e[196];
    n = sappend(json, sizeof(json), n, "{");
    json_escape(e, sizeof(e), c->wifi_ssid);   n = sappend(json, sizeof(json), n, "\"wifi_ssid\":\"%s\",", e);
    json_escape(e, sizeof(e), c->mqtt_uri);    n = sappend(json, sizeof(json), n, "\"mqtt_uri\":\"%s\",", e);
    json_escape(e, sizeof(e), c->mqtt_user);   n = sappend(json, sizeof(json), n, "\"mqtt_user\":\"%s\",", e);
    json_escape(e, sizeof(e), c->mqtt_prefix); n = sappend(json, sizeof(json), n, "\"mqtt_prefix\":\"%s\",", e);
    json_escape(e, sizeof(e), c->device_name); n = sappend(json, sizeof(json), n, "\"device_name\":\"%s\",", e);
    json_escape(e, sizeof(e), c->web_user);    n = sappend(json, sizeof(json), n, "\"web_user\":\"%s\",", e);
    json_escape(e, sizeof(e), c->lang);        n = sappend(json, sizeof(json), n, "\"lang\":\"%s\",", e);
    json_escape(e, sizeof(e), c->tz);          n = sappend(json, sizeof(json), n, "\"tz\":\"%s\",", e);
    json_escape(e, sizeof(e), c->ntp);         n = sappend(json, sizeof(json), n, "\"ntp\":\"%s\",", e);
    n = sappend(json, sizeof(json), n,
        "\"brightness\":%u,\"night_brightness\":%u,\"screen_timeout_s\":%u,"
        "\"chart_span_min\":%u,\"pages_mask\":%u,\"temp_offset\":%.1f,"
        "\"altitude_m\":%u,\"co2_asc\":%d,\"alarm_enabled\":%d,"
        "\"alarm_co2_ppm\":%u,\"alarm_clear_ppm\":%u,\"alarm_volume\":%u,"
        "\"noise_offset_db\":%d,"
        "\"batt_saver\":%d,\"batt_on_s\":%u,\"batt_period_s\":%u",
        c->brightness, c->night_brightness, c->screen_timeout_s,
        c->chart_span_min, c->pages_mask, c->temp_offset_dc / 10.0,
        c->altitude_m, c->co2_asc, c->alarm_enabled,
        c->alarm_co2_ppm, c->alarm_clear_ppm, c->alarm_volume,
        c->noise_offset_db,
        c->batt_saver ? 1 : 0, c->batt_on_s, c->batt_period_s);

    if (con_secretos) {
        char wp[132], mp[132];
        json_escape(wp, sizeof(wp), c->wifi_pass);
        json_escape(mp, sizeof(mp), c->mqtt_pass);
        n = sappend(json, sizeof(json), n,
                    ",\"wifi_pass\":\"%s\",\"mqtt_pass\":\"%s\"", wp, mp);
    }
    n = sappend(json, sizeof(json), n, "}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"monitor-sen66.json\"");
    httpd_resp_send(req, json, n);
    return ESP_OK;
}

// Prueba del altavoz: sin esto no hay forma de saber si el audio funciona
// sin esperar a que el CO2 pase del umbral.
static esp_err_t h_beep(httpd_req_t *req)
{
    if (guard_api(req) != ESP_OK) return ESP_FAIL;
    if (!sound_available()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "kein Audio");
        return ESP_FAIL;
    }
    sound_play(SOUND_TEST);
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static esp_err_t h_fanclean(httpd_req_t *req)
{
    if (guard_api(req) != ESP_OK) return ESP_FAIL;
    if (sen66_fan_clean() != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Sensor antwortet nicht");
        return ESP_FAIL;
    }
    return httpd_resp_sendstr(req, "Reinigung laeuft (10 s)");
}

// Recalibracion forzada de CO2. Aqui solo se valida y se encola: el comando
// exige la medicion parada, y pararla desde este hilo chocaria con la lectura
// que hace la tarea del sensor cada segundo.
static esp_err_t h_co2recal(httpd_req_t *req)
{
    if (guard_api(req) != ESP_OK) return ESP_FAIL;
    char q[48], v[8];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "ppm", v, sizeof(v)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ppm fehlt");
        return ESP_FAIL;
    }
    const int ppm = atoi(v);
    if (ppm < 400 || ppm > 2000) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Referenz muss zwischen 400 und 2000 ppm liegen");
        return ESP_FAIL;
    }
    if (!s_recal_request || !s_recal_request((uint16_t)ppm)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Nicht jetzt: Sensor misst nicht oder Kalibrierung ist bereits aktiv");
        return ESP_FAIL;
    }
    return httpd_resp_sendstr(req, "Kalibrierung gestartet, dauert einige Sekunden...");
}

// Diagnostico: parar o arrancar la medicion del sensor. Parado el ventilador
// se detiene, que es lo que permite medir cuanto ruido mete. No se mide nada
// mientras tanto.
static esp_err_t h_fan(httpd_req_t *req)
{
    if (guard_api(req) != ESP_OK) return ESP_FAIL;
    char q[32], v[8];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "on", v, sizeof(v)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "on=0|1 fehlt");
        return ESP_FAIL;
    }
    if (!s_fan || !s_fan(v[0] == '1')) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Sensor nicht erreichbar");
        return ESP_FAIL;
    }
    return httpd_resp_sendstr(req, v[0] == '1' ? "misst" : "gestoppt (es wird nichts gemessen)");
}

// Brillo en vivo mientras se arrastra el deslizador. NO guarda: lo guardado
// sigue siendo lo que haya en NVS hasta que se pulse Guardar. Sin esto el
// deslizador es a ciegas y hay que reiniciar para ver el resultado.
static esp_err_t h_brightness(httpd_req_t *req)
{
    if (guard_api(req) != ESP_OK) return ESP_FAIL;
    char q[24], v[8];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "v", v, sizeof(v)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "v fehlt");
        return ESP_FAIL;
    }
    int b = atoi(v);
    if (b < 1) b = 1;
    if (b > 255) b = 255;
    display_set_brightness((uint8_t)b);
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    if (guard_api(req) != ESP_OK) return ESP_FAIL;
    httpd_resp_sendstr(req, "ok");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// --------------------------------------------------------------------- OTA
static esp_err_t h_ota(httpd_req_t *req)
{
    if (guard_api(req) != ESP_OK) return ESP_FAIL;
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "keine OTA-Partition");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA a '%s', %d bytes", part->label, req->content_len);

    esp_ota_handle_t ota;
    if (esp_ota_begin(part, req->content_len, &ota) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_begin");
        return ESP_FAIL;
    }

    char buf[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            esp_ota_abort(ota);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Upload abgebrochen");
            return ESP_FAIL;
        }
        if (esp_ota_write(ota, buf, r) != ESP_OK) {
            esp_ota_abort(ota);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_write");
            return ESP_FAIL;
        }
        remaining -= r;
    }

    if (esp_ota_end(ota) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Image nicht akzeptiert");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "ok");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// -------------------------------------------------------------------- inicio
esp_err_t webcfg_start(webcfg_sample_fn get_sample)
{
    s_get_sample = get_sample;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;          // el OTA y cJSON necesitan holgura
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "httpd");

    static const httpd_uri_t routes[] = {
        {.uri = "/",              .method = HTTP_GET,  .handler = h_root},
        {.uri = "/api/state",     .method = HTTP_GET,  .handler = h_state},
        {.uri = "/api/settings",  .method = HTTP_GET,  .handler = h_settings_get},
        {.uri = "/api/settings",  .method = HTTP_POST, .handler = h_settings_post},
        {.uri = "/api/fanclean",  .method = HTTP_POST, .handler = h_fanclean},
        {.uri = "/api/beep",      .method = HTTP_POST, .handler = h_beep},
        {.uri = "/api/co2recal",  .method = HTTP_POST, .handler = h_co2recal},
        {.uri = "/api/fan",       .method = HTTP_POST, .handler = h_fan},
        {.uri = "/api/brightness",.method = HTTP_POST, .handler = h_brightness},
        {.uri = "/api/backup",    .method = HTTP_GET,  .handler = h_backup},
        {.uri = "/api/reboot",    .method = HTTP_POST, .handler = h_reboot},
        {.uri = "/api/ota",       .method = HTTP_POST, .handler = h_ota},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &routes[i]),
                            TAG, "ruta %s", routes[i].uri);
    }
    // Sin IP todavia: arrancamos antes de que el DHCP conteste. Decir una
    // direccion concreta aqui seria mentir (lo hacia: cantaba 192.168.4.1
    // incluso conectado a la red de casa). La IP real la canta net.c en
    // cuanto llega el GOT_IP; en el portal es siempre 192.168.4.1.
    ESP_LOGI(TAG, "servidor web escuchando en el puerto %d", cfg.server_port);
    return ESP_OK;
}
