#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>

// Константы AP режима
const char* AP_SSID = "spoofer";
const char* AP_PASSWORD = "spoofer123456";
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_GATEWAY(192, 168, 4, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);

const char* HOSTNAME = "spoofer";

const int DNS_PORT = 53;
const int MAX_HOSTS = 10;
const int EEPROM_SIZE = 4096;
const int LED_PIN = LED_BUILTIN;  // D4 на D1 mini

// Структура для хранения WiFi настроек
struct WiFiSettings {
  char ssid[32];
  char password[64];
  bool configured;
  char upstreamDNS[16];  // Upstream DNS сервер
};

// Структура для хоста
struct HostEntry {
  char domain[64];
  char targetIP[16];
  bool enabled;
};

// Структура для DNS запроса
struct DNSQuery {
  uint8_t id[2];
  String domain;
  uint16_t qtype;
  IPAddress clientIP;
  uint16_t clientPort;
};

WiFiSettings wifiSettings;
HostEntry hosts[MAX_HOSTS];
int hostCount = 0;

WiFiUDP dnsUDP;
WiFiUDP dnsForwardUDP;
ESP8266WebServer webServer(80);
ESP8266WebServer captiveServer(80);

DNSQuery pendingQueries[10];
int pendingCount = 0;

bool isCaptiveMode = false;
IPAddress DNS_UPSTREAM(8, 8, 8, 8);  // Динамический upstream DNS

String getHostsJSON();
String getCaptivePortalPage();

// Функции для работы с WiFi настройками в EEPROM
void loadWifiSettings() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, wifiSettings);
  
  // Проверяем валидность данных
  if (wifiSettings.configured && strlen(wifiSettings.ssid) > 0) {
    Serial.println("[EEPROM] WiFi settings found: " + String(wifiSettings.ssid));
  } else {
    Serial.println("[EEPROM] No WiFi settings found, will start captive portal");
    wifiSettings.configured = false;
    wifiSettings.ssid[0] = '\0';
    wifiSettings.password[0] = '\0';
  }
  
  // Проверяем upstream DNS
  if (strlen(wifiSettings.upstreamDNS) == 0) {
    strcpy(wifiSettings.upstreamDNS, "8.8.8.8");  // Значение по умолчанию
    Serial.println("[EEPROM] Using default upstream DNS: 8.8.8.8");
  } else {
    Serial.println("[EEPROM] Upstream DNS: " + String(wifiSettings.upstreamDNS));
  }
  
  EEPROM.end();
}

void saveWifiSettings() {
  EEPROM.begin(EEPROM_SIZE);
  wifiSettings.configured = true;
  EEPROM.put(0, wifiSettings);
  EEPROM.commit();
  EEPROM.end();
  Serial.println("[EEPROM] WiFi settings saved: " + String(wifiSettings.ssid));
}

void loadSettings() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(sizeof(WiFiSettings), hostCount);
  if (hostCount > MAX_HOSTS) hostCount = MAX_HOSTS;
  
  Serial.println("[EEPROM] Loading " + String(hostCount) + " hosts");
  
  for (int i = 0; i < hostCount; i++) {
    EEPROM.get(sizeof(WiFiSettings) + sizeof(int) + i * sizeof(HostEntry), hosts[i]);
    Serial.println("[EEPROM] Host " + String(i) + ": " + String(hosts[i].domain) + " -> " + String(hosts[i].targetIP) + " (enabled: " + String(hosts[i].enabled ? "yes" : "no") + ")");
  }
  EEPROM.end();
}

void saveSettings() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(sizeof(WiFiSettings), hostCount);
  
  Serial.println("[EEPROM] Saving " + String(hostCount) + " hosts");
  
  for (int i = 0; i < hostCount; i++) {
    EEPROM.put(sizeof(WiFiSettings) + sizeof(int) + i * sizeof(HostEntry), hosts[i]);
    Serial.println("[EEPROM] Saved host " + String(i) + ": " + String(hosts[i].domain) + " -> " + String(hosts[i].targetIP));
  }
  EEPROM.commit();
  EEPROM.end();
  
  Serial.println("[EEPROM] Save complete");
}

String getHostsJSON() {
  Serial.println("[JSON] Generating hosts JSON, count: " + String(hostCount));
  
  String json = "[";
  for (int i = 0; i < hostCount; i++) {
    if (i > 0) json += ",";
    json += "{\"domain\":\"" + String(hosts[i].domain) + "\",";
    json += "\"targetIP\":\"" + String(hosts[i].targetIP) + "\",";
    json += "\"enabled\":" + String(hosts[i].enabled ? "true" : "false") + "}";
    Serial.println("[JSON] Added host " + String(i) + ": " + String(hosts[i].domain));
  }
  json += "]";
  
  Serial.println("[JSON] Final JSON: " + json);
  return json;
}

String getPage() {
  String page = "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>DNS Spoofer</title>";
  page += "<style>";
  page += "*{box-sizing:border-box;margin:0;padding:0}body{font-family:Arial,sans-serif;background:#667eea;min-height:100vh;padding:20px}";
  page += ".container{max-width:900px;margin:0 auto}.card{background:#fff;border-radius:10px;padding:20px;margin-bottom:20px;box-shadow:0 4px 6px rgba(0,0,0,0.1)}";
  page += "h1{color:#fff;text-align:center;margin-bottom:20px}h2{color:#333;margin-bottom:15px;padding-bottom:10px;border-bottom:2px solid #667eea}";
  page += ".form-group{margin-bottom:15px}label{display:block;margin-bottom:5px;color:#555;font-weight:600}";
  page += "input{width:100%;padding:10px;border:2px solid #ddd;border-radius:5px;font-size:14px}input:focus{outline:none;border-color:#667eea}";
  page += ".btn{padding:10px 20px;border:none;border-radius:5px;cursor:pointer;font-size:14px;font-weight:600}";
  page += ".btn-primary{background:#667eea;color:#fff}.btn-primary:hover{background:#5568d3}";
  page += ".btn-danger{background:#e74c3c;color:#fff}.btn-danger:hover{background:#c0392b}";
  page += ".btn-success{background:#27ae60;color:#fff}.btn-success:hover{background:#219a52}";
  page += "table{width:100%;border-collapse:collapse;margin-top:15px}th,td{padding:10px;text-align:left;border-bottom:1px solid #ddd}";
  page += "th{background:#667eea;color:#fff}tr:hover{background:#f5f5f5}.actions{display:flex;gap:10px}";
  page += ".alert{padding:10px;border-radius:5px;margin-bottom:10px}.alert-success{background:#d4edda;color:#155724}";
  page += ".alert-error{background:#f8d7da;color:#721c24}.info{background:#e7f3ff;padding:15px;border-radius:5px;margin-bottom:15px;color:#004085}";
  page += ".info strong{color:#667eea}";
  page += "</style></head><body>";
  page += "<div class=\"container\">";
  page += "<h1>DNS Spoofer Control Panel</h1>";
  page += "<div class=\"info\"><strong>Device:</strong> spoofer.local<br><strong>Mode:</strong> DNS Server Active<br><strong>Upstream DNS:</strong> " + String(wifiSettings.upstreamDNS) + "<br><strong>Records:</strong> " + String(hostCount) + " / " + String(MAX_HOSTS) + "</div>";
  
  page += "<div class=\"card\"><h2>Add New Host</h2>";
  page += "<form id=\"addForm\">";
  page += "<div class=\"form-group\"><label>Domain</label><input type=\"text\" name=\"domain\" placeholder=\"example.com\" required></div>";
  page += "<div class=\"form-group\"><label>Target IP</label><input type=\"text\" name=\"targetIP\" placeholder=\"192.168.1.100\" required></div>";
  page += "<div class=\"form-group\"><label><input type=\"checkbox\" name=\"enabled\" checked> Enabled</label></div>";
  page += "<button type=\"submit\" class=\"btn btn-primary\">Add Host</button>";
  page += "</form></div>";
  
  page += "<div class=\"card\"><h2>Host List</h2><div id=\"message\"></div>";
  page += "<table><thead><tr><th>On</th><th>Domain</th><th>Target IP</th><th>Actions</th></tr></thead>";
  page += "<tbody id=\"hostTable\"></tbody></table></div>";
  
  page += "<div class=\"card\"><h2>Controls</h2>";
  page += "<button class=\"btn btn-danger\" onclick=\"clearAll()\">Clear All</button> ";
  page += "<button class=\"btn btn-success\" onclick=\"location.reload()\">Refresh</button>";
  page += "</div>";
  
  page += "<div class=\"card\"><h2>Upstream DNS</h2>";
  page += "<form id=\"dnsForm\">";
  page += "<div class=\"form-group\"><label>DNS Server</label><input type=\"text\" id=\"upstreamDNS\" name=\"upstreamDNS\" value=\"" + String(wifiSettings.upstreamDNS) + "\" placeholder=\"8.8.8.8\" required></div>";
  page += "<button type=\"submit\" class=\"btn btn-primary\">Save DNS</button>";
  page += "</form>";
  page += "<div id=\"dnsMessage\"></div>";
  page += "</div></div>";
  
  page += "<script>";
  page += "console.log('Loading hosts...');";
  page += "var hosts=" + getHostsJSON() + ";";
  page += "console.log('Hosts loaded:', JSON.stringify(hosts));";
  page += "function renderTable(){var t=document.getElementById('hostTable');t.innerHTML='';";
  page += "console.log('Rendering table, hosts length:', hosts.length);";
  page += "if(hosts.length===0){t.innerHTML='<tr><td colspan=\"4\" style=\"text-align:center;color:#999\">No records</td></tr>';return;} ";
  page += "for(var i=0;i<hosts.length;i++){var c=hosts[i].enabled?'checked':'';";
  page += "console.log('Rendering host', i, hosts[i]);";
  page += "var r=document.createElement('tr');";
  page += "r.innerHTML='<td><input type=\"checkbox\" '+c+' onclick=\"toggleHost('+i+')\"></td><td>'+hosts[i].domain+'</td><td>'+hosts[i].targetIP+'</td><td><button class=\"btn btn-danger\" onclick=\"deleteHost('+i+')>Delete</button></td>';";
  page += "t.appendChild(r);}}";
  page += "function showMsg(t,m){var el=document.getElementById('message');el.innerHTML='<div class=\"alert alert-'+t+'\">'+m+'</div>';setTimeout(function(){el.innerHTML=''},3000);}";
  page += "document.getElementById('addForm').onsubmit=function(e){e.preventDefault();";
  page += "var f=e.target,d={domain:f.domain.value,targetIP:f.targetIP.value,enabled:f.enabled.checked};";
  page += "console.log('Adding host:', JSON.stringify(d));";
  page += "var x=new XMLHttpRequest();x.open('POST','/add',true);x.setRequestHeader('Content-Type','application/json');";
  page += "x.onreadystatechange=function(){if(x.readyState===4){console.log('Response:', x.responseText);var r=JSON.parse(x.responseText);showMsg(r.success?'success':'error',r.message);";
  page += "if(r.success){hosts.push(d);renderTable();f.reset();}}};x.send(JSON.stringify(d));};";
  page += "function deleteHost(i){if(!confirm('Delete?'))return;console.log('Deleting host', i);var x=new XMLHttpRequest();x.open('POST','/delete/'+i,true);";
  page += "x.onreadystatechange=function(){if(x.readyState===4){console.log('Delete response:', x.responseText);var r=JSON.parse(x.responseText);showMsg(r.success?'success':'error',r.message);";
  page += "if(r.success){hosts.splice(i,1);renderTable();}}};x.send();}";
  page += "function toggleHost(i){console.log('Toggling host', i);hosts[i].enabled=!hosts[i].enabled;var x=new XMLHttpRequest();x.open('POST','/toggle/'+i,true);x.send();}";
  page += "function clearAll(){if(!confirm('Delete all?'))return;console.log('Clearing all');var x=new XMLHttpRequest();x.open('POST','/clear',true);";
  page += "x.onreadystatechange=function(){if(x.readyState===4){console.log('Clear response:', x.responseText);var r=JSON.parse(x.responseText);showMsg(r.success?'success':'error',r.message);";
  page += "if(r.success){hosts=[];renderTable();}}};x.send();}";
  page += "document.getElementById('dnsForm').onsubmit=function(e){e.preventDefault();";
  page += "var dnsVal=document.getElementById('upstreamDNS').value;";
  page += "var x=new XMLHttpRequest();x.open('POST','/setdns',true);x.setRequestHeader('Content-Type','application/json');";
  page += "x.onreadystatechange=function(){if(x.readyState===4){var r=JSON.parse(x.responseText);";
  page += "var el=document.getElementById('dnsMessage');el.innerHTML='<div class=\"alert alert-'+(r.success?'success':'error')+'\">'+r.message+'</div>';";
  page += "setTimeout(function(){el.innerHTML=''},3000);}};x.send(JSON.stringify({upstreamDNS:dnsVal}));};";
  page += "renderTable();</script></body></html>";
  return page;
}

// Страница Captive Portal
String getCaptivePortalPage() {
  String page = "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">";
  page += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  page += "<title>DNS Spoofer - WiFi Setup</title>";
  page += "<style>";
  page += "*{box-sizing:border-box;margin:0;padding:0}";
  page += "body{font-family:Arial,sans-serif;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);min-height:100vh;padding:20px;display:flex;justify-content:center;align-items:center}";
  page += ".container{max-width:500px;width:100%}.card{background:#fff;border-radius:15px;padding:30px;box-shadow:0 10px 40px rgba(0,0,0,0.2)}";
  page += "h1{color:#333;text-align:center;margin-bottom:10px;font-size:24px}";
  page += ".subtitle{color:#666;text-align:center;margin-bottom:25px;font-size:14px}";
  page += ".form-group{margin-bottom:20px}";
  page += "label{display:block;margin-bottom:8px;color:#555;font-weight:600;font-size:14px}";
  page += "input,select{width:100%;padding:12px;border:2px solid #ddd;border-radius:8px;font-size:16px;transition:border-color 0.3s}";
  page += "input:focus,select:focus{outline:none;border-color:#667eea}";
  page += ".btn{width:100%;padding:15px;background:#667eea;color:#fff;border:none;border-radius:8px;font-size:16px;font-weight:600;cursor:pointer;transition:background 0.3s}";
  page += ".btn:hover{background:#5568d3}";
  page += ".btn:disabled{background:#ccc;cursor:not-allowed}";
  page += ".status{padding:15px;border-radius:8px;margin-bottom:20px;text-align:center;font-size:14px;display:none}";
  page += ".status.loading{background:#fff3cd;color:#856404;display:block}";
  page += ".status.success{background:#d4edda;color:#155724;display:block}";
  page += ".status.error{background:#f8d7da;color:#721c24;display:block}";
  page += ".info{background:#e7f3ff;padding:15px;border-radius:8px;margin-bottom:20px;font-size:13px;color:#004085}";
  page += ".info strong{color:#667eea}";
  page += ".spinner{display:inline-block;width:20px;height:20px;border:3px solid #fff;border-top-color:transparent;border-radius:50%;animation:spin 1s linear infinite;vertical-align:middle;margin-right:10px}";
  page += "@keyframes spin{to{transform:rotate(360deg)}}";
  page += ".network-list{margin-top:15px;max-height:200px;overflow-y:auto;border:1px solid #ddd;border-radius:8px}";
  page += ".network-item{padding:12px 15px;border-bottom:1px solid #eee;cursor:pointer;transition:background 0.2s;display:flex;justify-content:space-between;align-items:center}";
  page += ".network-item:hover{background:#f5f5f5}";
  page += ".network-item:last-child{border-bottom:none}";
  page += ".network-item.selected{background:#e7f3ff;border-left:3px solid #667eea}";
  page += ".network-name{font-weight:600;color:#333}";
  page += ".network-signal{font-size:12px;color:#666}";
  page += ".network-signal.strong{color:#27ae60}";
  page += ".network-signal.medium{color:#f39c12}";
  page += ".network-signal.weak{color:#e74c3c}";
  page += ".lock-icon{color:#666;margin-left:5px}";
  page += "</style></head><body>";
  page += "<div class=\"container\">";
  page += "<div class=\"card\">";
  page += "<h1>DNS Spoofer Setup</h1>";
  page += "<p class=\"subtitle\">Connect to your WiFi network</p>";
  
  page += "<div class=\"info\">";
  page += "<strong>Device:</strong> spoofer<br>";
  page += "<strong>AP Password:</strong> spoofer123456<br>";
  page += "<strong>Status:</strong> <span id=\"connectionStatus\">Not connected</span><br>";
  page += "<strong>Current Upstream DNS:</strong> <span id=\"currentDNS\">" + String(wifiSettings.upstreamDNS) + "</span>";
  page += "</div>";
  
  page += "<div id=\"status\" class=\"status\"></div>";
  
  page += "<div class=\"form-group\">";
  page += "<label>Available Networks</label>";
  page += "<button type=\"button\" class=\"btn\" id=\"scanBtn\" onclick=\"scanNetworks()\">";
  page += "<span class=\"spinner\" id=\"scanSpinner\" style=\"display:none\"></span>Scan Networks</button>";
  page += "<div class=\"network-list\" id=\"networkList\">";
  page += "<div class=\"network-item\" style=\"color:#999;text-align:center\">Click \"Scan Networks\" to find WiFi</div>";
  page += "</div>";
  page += "</div>";
  
  page += "<form id=\"wifiForm\">";
  page += "<div class=\"form-group\">";
  page += "<label>Selected Network</label>";
  page += "<input type=\"text\" id=\"ssid\" name=\"ssid\" placeholder=\"Select a network above\" readonly required>";
  page += "</div>";
  page += "<div class=\"form-group\">";
  page += "<label>Password</label>";
  page += "<input type=\"password\" id=\"password\" name=\"password\" placeholder=\"Enter WiFi password\" required>";
  page += "</div>";
  page += "<div class=\"form-group\">";
  page += "<label>Upstream DNS Server</label>";
  page += "<input type=\"text\" id=\"upstreamDNS\" name=\"upstreamDNS\" placeholder=\"8.8.8.8\" value=\"8.8.8.8\">";
  page += "</div>";
  page += "<button type=\"submit\" class=\"btn\" id=\"submitBtn\" disabled>Connect</button>";
  page += "</form>";
  
  page += "</div>";
  page += "</div>";
  
  page += "<script>";
  page += "var selectedSSID = '';";
  page += "var isProtected = false;";
  
  page += "function scanNetworks() {";
  page += "  var btn = document.getElementById('scanBtn');";
  page += "  var spinner = document.getElementById('scanSpinner');";
  page += "  var list = document.getElementById('networkList');";
  page += "  btn.disabled = true;";
  page += "  spinner.style.display = 'inline-block';";
  page += "  btn.innerHTML = '<span class=\"spinner\"></span>Scanning...';";
  page += "  list.innerHTML = '<div class=\"network-item\" style=\"color:#999;text-align:center\">Scanning...</div>';";
  page += "  ";
  page += "  var x = new XMLHttpRequest();";
  page += "  x.open('GET', '/scan', true);";
  page += "  x.onreadystatechange = function() {";
  page += "    if (x.readyState === 4) {";
  page += "      btn.disabled = false;";
  page += "      spinner.style.display = 'none';";
  page += "      btn.innerHTML = 'Scan Networks';";
  page += "      ";
  page += "      if (x.status === 200) {";
  page += "        try {";
  page += "          var networks = JSON.parse(x.responseText);";
  page += "          displayNetworks(networks);";
  page += "        } catch (e) {";
  page += "          list.innerHTML = '<div class=\"network-item\" style=\"color:#e74c3c;text-align:center\">Error parsing networks</div>';";
  page += "        }";
  page += "      } else {";
  page += "        list.innerHTML = '<div class=\"network-item\" style=\"color:#e74c3c;text-align:center\">Scan failed</div>';";
  page += "      }";
  page += "    }";
  page += "  };";
  page += "  x.send();";
  page += "}";
  
  page += "function displayNetworks(networks) {";
  page += "  var list = document.getElementById('networkList');";
  page += "  if (networks.length === 0) {";
  page += "    list.innerHTML = '<div class=\"network-item\" style=\"color:#999;text-align:center\">No networks found</div>';";
  page += "    return;";
  page += "  }";
  page += "  ";
  page += "  list.innerHTML = '';";
  page += "  networks.forEach(function(net) {";
  page += "    var item = document.createElement('div');";
  page += "    item.className = 'network-item';";
  page += "    item.onclick = function() { selectNetwork(net.ssid, net.encrypted); };";
  page += "    ";
  page += "    var signalClass = 'weak';";
  page += "    if (net.rssi > -50) signalClass = 'strong';";
  page += "    else if (net.rssi > -70) signalClass = 'medium';";
  page += "    ";
  page += "    var lockIcon = net.encrypted ? '<span class=\"lock-icon\">&#128274;</span>' : '';";
  page += "    ";
  page += "    item.innerHTML = '<span class=\"network-name\">' + net.ssid + lockIcon + '</span>' + '<span class=\"network-signal ' + signalClass + '\">' + net.rssi + ' dBm</span>';";
  page += "    list.appendChild(item);";
  page += "  });";
  page += "}";
  
  page += "function selectNetwork(ssid, encrypted) {";
  page += "  selectedSSID = ssid;";
  page += "  isProtected = encrypted;";
  page += "  ";
  page += "  document.getElementById('ssid').value = ssid;";
  page += "  document.getElementById('password').value = '';";
  page += "  document.getElementById('password').disabled = !encrypted;";
  page += "  document.getElementById('password').placeholder = encrypted ? 'Enter WiFi password' : 'No password required';";
  page += "  document.getElementById('submitBtn').disabled = false;";
  page += "  ";
  page += "  document.querySelectorAll('.network-item').forEach(function(item) {";
  page += "    item.classList.remove('selected');";
  page += "  });";
  page += "  event.currentTarget.classList.add('selected');";
  page += "}";
  
  page += "document.getElementById('wifiForm').onsubmit = function(e) {";
  page += "  e.preventDefault();";
  page += "  ";
  page += "  var status = document.getElementById('status');";
  page += "  var btn = document.getElementById('submitBtn');";
  page += "  var ssidVal = document.getElementById('ssid').value;";
  page += "  var passVal = document.getElementById('password').value;";
  page += "  ";
  page += "  if (!ssidVal) {";
  page += "    status.className = 'status error';";
  page += "    status.textContent = 'Please select a network';";
  page += "    return;";
  page += "  }";
  page += "  ";
  page += "  if (isProtected && !passVal) {";
  page += "    status.className = 'status error';";
  page += "    status.textContent = 'Please enter password';";
  page += "    return;";
  page += "  }";
  page += "  ";
  page += "  status.className = 'status loading';";
  page += "  status.innerHTML = '<span class=\"spinner\"></span>Connecting to ' + ssidVal + '...';";
  page += "  btn.disabled = true;";
  page += "  ";
  page += "  var x = new XMLHttpRequest();";
  page += "  x.open('POST', '/connect', true);";
  page += "  x.setRequestHeader('Content-Type', 'application/json');";
  page += "  ";
  page += "  var upstreamDNSVal = document.getElementById('upstreamDNS').value || '8.8.8.8';";
  page += "  var data = {ssid: ssidVal, password: passVal, upstreamDNS: upstreamDNSVal};";
  page += "  ";
  page += "  x.onreadystatechange = function() {";
  page += "    if (x.readyState === 4) {";
  page += "      if (x.status === 200) {";
  page += "        try {";
  page += "          var res = JSON.parse(x.responseText);";
  page += "          if (res.success) {";
  page += "            status.className = 'status success';";
  page += "            status.textContent = 'Connected! Rebooting...';";
  page += "            document.getElementById('connectionStatus').textContent = 'Connected to ' + ssidVal;";
  page += "          } else {";
  page += "            status.className = 'status error';";
  page += "            status.textContent = res.message || 'Connection failed';";
  page += "            btn.disabled = false;";
  page += "          }";
  page += "        } catch (e) {";
  page += "          status.className = 'status error';";
  page += "          status.textContent = 'Error parsing response';";
  page += "          btn.disabled = false;";
  page += "        }";
  page += "      } else {";
  page += "        status.className = 'status error';";
  page += "        status.textContent = 'Request failed: ' + x.status;";
  page += "        btn.disabled = false;";
  page += "      }";
  page += "    }";
  page += "  };";
  page += "  ";
  page += "  x.send(JSON.stringify(data));";
  page += "};";
  page += "</script>";
  page += "</body></html>";
  return page;
}

void handleRoot() {
  Serial.println("[WEB] Serving main page, hostCount: " + String(hostCount));
  webServer.send(200, "text/html", getPage());
}

void handleAdd() {
  Serial.println("[WEB] Handle add called");
  
  if (!webServer.hasArg("plain")) {
    Serial.println("[WEB] Empty request");
    webServer.send(400, "application/json", "{\"success\":false,\"message\":\"Empty request\"}");
    return;
  }
  
  String body = webServer.arg("plain");
  Serial.println("[WEB] Request body: " + body);
  
  if (hostCount >= MAX_HOSTS) {
    Serial.println("[WEB] Limit reached");
    webServer.send(400, "application/json", "{\"success\":false,\"message\":\"Limit reached\"}");
    return;
  }
  
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, body);
  
  if (error) {
    Serial.println("[WEB] JSON parse error: " + String(error.c_str()));
    webServer.send(400, "application/json", "{\"success\":false,\"message\":\"JSON error\"}");
    return;
  }
  
  String domain = doc["domain"].as<String>();
  String targetIP = doc["targetIP"].as<String>();
  bool enabled = doc["enabled"] | true;
  
  Serial.println("[WEB] Parsed - domain: " + domain + ", targetIP: " + targetIP + ", enabled: " + String(enabled));
  
  if (domain.length() == 0 || targetIP.length() == 0) {
    Serial.println("[WEB] Empty fields");
    webServer.send(400, "application/json", "{\"success\":false,\"message\":\"Fill fields\"}");
    return;
  }
  
  for (int i = 0; i < hostCount; i++) {
    if (domain.equalsIgnoreCase(String(hosts[i].domain))) {
      Serial.println("[WEB] Domain exists");
      webServer.send(400, "application/json", "{\"success\":false,\"message\":\"Domain exists\"}");
      return;
    }
  }
  
  strncpy(hosts[hostCount].domain, domain.c_str(), sizeof(hosts[hostCount].domain) - 1);
  hosts[hostCount].domain[sizeof(hosts[hostCount].domain) - 1] = 0;
  
  strncpy(hosts[hostCount].targetIP, targetIP.c_str(), sizeof(hosts[hostCount].targetIP) - 1);
  hosts[hostCount].targetIP[sizeof(hosts[hostCount].targetIP) - 1] = 0;
  
  hosts[hostCount].enabled = enabled;
  hostCount++;
  
  Serial.println("[WEB] Added host, new count: " + String(hostCount));
  
  saveSettings();
  webServer.send(200, "application/json", "{\"success\":true,\"message\":\"Host added\"}");
}

void handleDelete() {
  String path = webServer.uri();
  Serial.println("[WEB] Delete path: " + path);
  
  int index = -1;
  for (unsigned int i = 8; i < path.length(); i++) {
    if (path[i] >= '0' && path[i] <= '9') {
      index = (index == -1) ? 0 : (index * 10 + (path[i] - '0'));
    } else break;
  }
  
  Serial.println("[WEB] Delete index: " + String(index) + ", hostCount: " + String(hostCount));
  
  if (index < 0 || index >= hostCount) {
    webServer.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid index\"}");
    return;
  }
  
  Serial.println("[WEB] Deleting host: " + String(hosts[index].domain));
  
  for (int i = index; i < hostCount - 1; i++) hosts[i] = hosts[i + 1];
  hostCount--;
  
  saveSettings();
  webServer.send(200, "application/json", "{\"success\":true,\"message\":\"Host deleted\"}");
}

void handleToggle() {
  String path = webServer.uri();
  Serial.println("[WEB] Toggle path: " + path);
  
  int index = -1;
  for (unsigned int i = 8; i < path.length(); i++) {
    if (path[i] >= '0' && path[i] <= '9') {
      index = (index == -1) ? 0 : (index * 10 + (path[i] - '0'));
    } else break;
  }
  
  if (index < 0 || index >= hostCount) {
    webServer.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid index\"}");
    return;
  }
  
  hosts[index].enabled = !hosts[index].enabled;
  Serial.println("[WEB] Toggled host " + String(index) + " to " + String(hosts[index].enabled));
  
  saveSettings();
  webServer.send(200, "application/json", "{\"success\":true,\"message\":\"Toggled\"}");
}

void handleClear() {
  Serial.println("[WEB] Clearing all hosts");
  hostCount = 0;
  saveSettings();
  webServer.send(200, "application/json", "{\"success\":true,\"message\":\"All cleared\"}");
}

void handleSetDNS() {
  Serial.println("[WEB] Set DNS request");
  
  if (!webServer.hasArg("plain")) {
    webServer.send(400, "application/json", "{\"success\":false,\"message\":\"Empty request\"}");
    return;
  }
  
  String body = webServer.arg("plain");
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, body);
  
  if (error) {
    webServer.send(400, "application/json", "{\"success\":false,\"message\":\"JSON error\"}");
    return;
  }
  
  String upstreamDNS = doc["upstreamDNS"].as<String>();
  
  if (upstreamDNS.length() == 0) {
    webServer.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid DNS\"}");
    return;
  }
  
  strncpy(wifiSettings.upstreamDNS, upstreamDNS.c_str(), sizeof(wifiSettings.upstreamDNS) - 1);
  wifiSettings.upstreamDNS[sizeof(wifiSettings.upstreamDNS) - 1] = 0;
  saveWifiSettings();
  
  DNS_UPSTREAM.fromString(upstreamDNS);
  
  Serial.println("[WEB] Upstream DNS changed to: " + upstreamDNS);
  webServer.send(200, "application/json", "{\"success\":true,\"message\":\"DNS updated\"}");
}

void handleNotFound() {
  Serial.println("[WEB] 404: " + webServer.uri());
  webServer.send(404, "text/plain", "Not Found");
}

// Captive Portal обработчики
void handleCaptiveRoot() {
  Serial.println("[CAPTIVE] Serving captive portal page");
  captiveServer.send(200, "text/html", getCaptivePortalPage());
}

void handleCaptiveScan() {
  Serial.println("[CAPTIVE] Scanning networks...");
  
  int n = WiFi.scanNetworks();
  String json = "[";
  
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    json += "{";
    json += "\"ssid\":\"" + WiFi.SSID(i) + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
    json += "\"encrypted\":" + String(WiFi.encryptionType(i) != ENC_TYPE_NONE ? "true" : "false");
    json += "}";
    Serial.println("[CAPTIVE] Found: " + WiFi.SSID(i) + " (" + WiFi.RSSI(i) + " dBm)");
  }
  
  json += "]";
  WiFi.scanDelete();
  
  Serial.println("[CAPTIVE] Scan complete, found " + String(n) + " networks");
  captiveServer.send(200, "application/json", json);
}

void handleCaptiveConnect() {
  Serial.println("[CAPTIVE] Connect request received");
  
  if (!captiveServer.hasArg("plain")) {
    Serial.println("[CAPTIVE] Empty request");
    captiveServer.send(400, "application/json", "{\"success\":false,\"message\":\"Empty request\"}");
    return;
  }
  
  String body = captiveServer.arg("plain");
  Serial.println("[CAPTIVE] Request body: " + body);
  
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, body);
  
  if (error) {
    Serial.println("[CAPTIVE] JSON parse error: " + String(error.c_str()));
    captiveServer.send(400, "application/json", "{\"success\":false,\"message\":\"JSON error\"}");
    return;
  }
  
  String ssid = doc["ssid"].as<String>();
  String password = doc["password"].as<String>();
  String upstreamDNS = doc["upstreamDNS"].as<String>();
  
  Serial.println("[CAPTIVE] Connecting to: " + ssid);
  Serial.println("[CAPTIVE] Upstream DNS: " + upstreamDNS);
  
  // Сохраняем настройки
  strncpy(wifiSettings.ssid, ssid.c_str(), sizeof(wifiSettings.ssid) - 1);
  wifiSettings.ssid[sizeof(wifiSettings.ssid) - 1] = 0;
  
  strncpy(wifiSettings.password, password.c_str(), sizeof(wifiSettings.password) - 1);
  wifiSettings.password[sizeof(wifiSettings.password) - 1] = 0;
  
  if (upstreamDNS.length() > 0) {
    strncpy(wifiSettings.upstreamDNS, upstreamDNS.c_str(), sizeof(wifiSettings.upstreamDNS) - 1);
    wifiSettings.upstreamDNS[sizeof(wifiSettings.upstreamDNS) - 1] = 0;
  }
  
  wifiSettings.configured = true;
  saveWifiSettings();
  
  Serial.println("[CAPTIVE] Settings saved, responding to client");
  captiveServer.send(200, "application/json", "{\"success\":true,\"message\":\"Configuration saved, rebooting...\"}");
  
  delay(1000);
  ESP.restart();
}

void handleCaptiveNotFound() {
  Serial.println("[CAPTIVE] 404: " + captiveServer.uri() + ", redirecting to /");
  captiveServer.sendHeader("Location", "http://" + String(AP_SSID) + ".local/", 302);
  captiveServer.send(302, "text/plain", "");
}

IPAddress findSpoofedIP(const String& domain) {
  for (int i = 0; i < hostCount; i++) {
    if (hosts[i].enabled) {
      if (domain.equalsIgnoreCase(String(hosts[i].domain))) {
        IPAddress ip;
        if (ip.fromString(hosts[i].targetIP)) {
          Serial.println("[DNS] SPOOF: " + domain + " -> " + hosts[i].targetIP);
          return ip;
        }
      }
    }
  }
  return IPAddress(0, 0, 0, 0);
}

String extractDomainFromDNS(const uint8_t* buffer, size_t len) {
  String domain = "";
  if (len < 12) return domain;
  
  int pos = 12;
  while (pos < len) {
    uint8_t labelLen = buffer[pos];
    if (labelLen == 0) break;
    
    if (pos + labelLen + 1 > len) break;
    
    if (domain.length() > 0) domain += ".";
    
    for (int i = 1; i <= labelLen; i++) {
      char c = buffer[pos + i];
      if (c >= 32 && c < 127) {
        domain += c;
      }
    }
    
    pos += labelLen + 1;
  }
  
  return domain;
}

void sendSpoofedDNSResponse(const uint8_t* buffer, size_t len, const IPAddress& clientIP, uint16_t clientPort, const IPAddress& spoofedIP) {
    // Минимальный размер DNS заголовка — 12 байт
    if (len < 12 || len > 512) return; 

    uint8_t response[512];
    memcpy(response, buffer, len);

    // Модификация заголовка DNS: Ответ, Стандартный запрос, Без ошибок
    response[2] = 0x81; 
    response[3] = 0x80;
    
    // Устанавливаем количество ответов (Answer Count) в 1
    response[6] = 0x00; 
    response[7] = 0x01;
    
    // Сбрасываем Authority и Additional записи, так как мы их не передаем
    response[8] = 0x00;  response[9] = 0x00;
    response[10] = 0x00; response[11] = 0x00;

    // Безопасный поиск конца секции Question (имени хоста)
    size_t pos = 12;
    while (pos < len) {
        uint8_t labelLen = buffer[pos];
        if (labelLen == 0) {
            pos++; // Учитываем замыкающий нулевой байт имени
            break;
        }
        // Проверка: не выходит ли длина за рамки пакета
        if (pos + labelLen + 1 > len) return; 
        pos += labelLen + 1;
    }

    // После имени идут QTYPE (2 байта) и QCLASS (2 байта) — всего 4 байта
    if (pos + 4 > len) return;
    pos += 4; // Теперь pos указывает на начало секции Answer (answerPos)

    // Проверяем, поместится ли 16 байт DNS-ответа в наш буфер 512 байт
    if (pos + 16 > 512) return;

    // Формируем Answer Record (A-запись)
    response[pos]     = 0xC0; // Ссылка на имя из секции Question
    response[pos + 1] = 0x0C; // Смещение 12 (0x0C) указывает на начало имени хоста
    response[pos + 2] = 0x00; response[pos + 3] = 0x01; // Type: A (IPv4)
    response[pos + 4] = 0x00; response[pos + 5] = 0x01; // Class: IN (Internet)
    
    // TTL: Время жизни записи (например, 60 секунд -> 0x0000003C)
    response[pos + 6] = 0x00; 
    response[pos + 7] = 0x00; 
    response[pos + 8] = 0x00; 
    response[pos + 9] = 0x3C;
    
    // Data Length: Длина IP-адреса (4 байта)
    response[pos + 10] = 0x00; 
    response[pos + 11] = 0x04;
    
    // Запись IP-адреса
    response[pos + 12] = spoofedIP[0];
    response[pos + 13] = spoofedIP[1];
    response[pos + 14] = spoofedIP[2];
    response[pos + 15] = spoofedIP[3];

    size_t responseLen = pos + 16;

    // Отправка пакета
    dnsUDP.beginPacket(clientIP, clientPort);
    dnsUDP.write(response, responseLen);
    dnsUDP.endPacket();
}

void forwardDNSQuery(const uint8_t* buffer, size_t len, const IPAddress& clientIP, uint16_t clientPort) {
  if (pendingCount >= 10) {
    Serial.println("[DNS] Pending query buffer full, dropping");
    return;
  }
  
  DNSQuery query;
  query.id[0] = buffer[0];
  query.id[1] = buffer[1];
  query.clientIP = clientIP;
  query.clientPort = clientPort;
  
  int pos = 12;
  while (pos < len && buffer[pos] != 0) {
    pos += buffer[pos] + 1;
  }
  query.qtype = (buffer[pos + 1] << 8) | buffer[pos + 2];
  
  pendingQueries[pendingCount++] = query;
  
  dnsForwardUDP.beginPacket(DNS_UPSTREAM, 53);
  dnsForwardUDP.write(buffer, len);
  dnsForwardUDP.endPacket();
  
  Serial.println("[DNS] Forwarding query to " + DNS_UPSTREAM.toString());
}

void processDNS() {
  int packetSize = dnsUDP.parsePacket();
  if (packetSize > 0) {
    uint8_t buffer[512];
    int len = dnsUDP.read(buffer, sizeof(buffer) - 1);
    if (len > 0) {
      buffer[len] = 0;
      
      IPAddress clientIP = dnsUDP.remoteIP();
      uint16_t clientPort = dnsUDP.remotePort();
      
      String domain = extractDomainFromDNS(buffer, len);
      Serial.println("[DNS] Query from " + clientIP.toString() + ":" + String(clientPort) + " for: " + domain);
      
      IPAddress spoofedIP = findSpoofedIP(domain);
      
      if (spoofedIP != IPAddress(0, 0, 0, 0)) {
        delay(10);
        sendSpoofedDNSResponse(buffer, len, clientIP, clientPort, spoofedIP);
        Serial.println("[DNS] Spoofed response sent to " + clientIP.toString());
      } else {
        Serial.println("[DNS] Domain not in spoof list, forwarding to upstream");
        forwardDNSQuery(buffer, len, clientIP, clientPort);
      }
    }
  }
  
  int forwardSize = dnsForwardUDP.parsePacket();
  if (forwardSize > 0) {
    uint8_t forwardBuffer[512];
    int forwardLen = dnsForwardUDP.read(forwardBuffer, sizeof(forwardBuffer) - 1);
    if (forwardLen > 0) {
      forwardBuffer[forwardLen] = 0;
      
      uint16_t responseID = (forwardBuffer[0] << 8) | forwardBuffer[1];
      
      for (int i = 0; i < pendingCount; i++) {
        uint16_t pendingID = (pendingQueries[i].id[0] << 8) | pendingQueries[i].id[1];
        if (pendingID == responseID) {
          Serial.println("[DNS] Received response from upstream, forwarding to client");
          
          dnsUDP.beginPacket(pendingQueries[i].clientIP, pendingQueries[i].clientPort);
          dnsUDP.write(forwardBuffer, forwardLen);
          dnsUDP.endPacket();
          
          for (int j = i; j < pendingCount - 1; j++) {
            pendingQueries[j] = pendingQueries[j + 1];
          }
          pendingCount--;
          break;
        }
      }
    }
  }
}

void startCaptivePortal() {
  Serial.println();
  Serial.println("========================================");
  Serial.println("Starting Captive Portal Mode");
  Serial.println("========================================");
  
  isCaptiveMode = true;
  
  // Запускаем AP
  WiFi.mode(WIFI_AP);
  delay(100);
  
  bool apStarted = WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
  if (!apStarted) {
    Serial.println("[CAPTIVE] softAPConfig failed!");
  }
  
  apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD);
  if (apStarted) {
    Serial.println("[CAPTIVE] AP started successfully");
    Serial.println("[CAPTIVE] SSID: " + String(AP_SSID));
    Serial.println("[CAPTIVE] Password: " + String(AP_PASSWORD));
    Serial.println("[CAPTIVE] IP: " + WiFi.softAPIP().toString());
  } else {
    Serial.println("[CAPTIVE] Failed to start AP!");
  }
  
  // Настраиваем веб-сервер для captive portal
  captiveServer.on("/", handleCaptiveRoot);
  captiveServer.on("/scan", HTTP_GET, handleCaptiveScan);
  captiveServer.on("/connect", HTTP_POST, handleCaptiveConnect);
  captiveServer.onNotFound(handleCaptiveNotFound);
  captiveServer.begin();
  
  Serial.println("[CAPTIVE] Web server started");
  Serial.println("[CAPTIVE] Open http://" + String(AP_SSID) + ".local or http://" + WiFi.softAPIP().toString());
  Serial.println("========================================");
}

void setup() {
  Serial.begin(115200);
  delay(100);
  
  // Инициализация светодиода
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);  // Выключен по умолчанию
  
  Serial.println();
  Serial.println("========================================");
  Serial.println("DNS Spoofer v4.0 Starting...");
  Serial.println("========================================");
  
  // Загружаем настройки
  loadWifiSettings();
  loadSettings();
  
  // Устанавливаем upstream DNS из сохраненных настроек
  if (strlen(wifiSettings.upstreamDNS) > 0) {
    DNS_UPSTREAM.fromString(wifiSettings.upstreamDNS);
    Serial.println("[DNS] Upstream DNS set to: " + String(wifiSettings.upstreamDNS));
  }
  
  // Проверяем, есть ли сохраненные WiFi настройки
  if (wifiSettings.configured && strlen(wifiSettings.ssid) > 0) {
    Serial.println("[WIFI] Found saved WiFi settings, trying to connect...");
    Serial.println("[WIFI] SSID: " + String(wifiSettings.ssid));
    
    WiFi.mode(WIFI_STA);
    WiFi.hostname(HOSTNAME);
    WiFi.begin(wifiSettings.ssid, wifiSettings.password);
    
    Serial.println("[WIFI] Connecting...");
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println();
      Serial.println("[WIFI] Connected successfully!");
      Serial.println("[WIFI] IP: " + WiFi.localIP().toString());
      Serial.println("[WIFI] Gateway: " + WiFi.gatewayIP().toString());
      Serial.println("[WIFI] Hostname: " + String(HOSTNAME) + ".local");
      
      isCaptiveMode = false;
      digitalWrite(LED_PIN, HIGH);  // Выключаем светодиод
      
      dnsUDP.begin(DNS_PORT);
      dnsForwardUDP.begin(0);
      Serial.println("[DNS] UDP Server started on port " + String(DNS_PORT));
      Serial.println("[DNS] Upstream DNS: " + DNS_UPSTREAM.toString());
      
      if (MDNS.begin(HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("[mDNS] Started: spoofer.local");
      } else {
        Serial.println("[mDNS] Failed to start!");
      }
      
      webServer.on("/", handleRoot);
      webServer.on("/add", HTTP_POST, handleAdd);
      webServer.on("/delete/", HTTP_POST, handleDelete);
      webServer.on("/toggle/", HTTP_POST, handleToggle);
      webServer.on("/clear", HTTP_POST, handleClear);
      webServer.on("/setdns", HTTP_POST, handleSetDNS);
      webServer.onNotFound(handleNotFound);
      webServer.begin();
      
      Serial.println("[WEB] Server started");
      Serial.println("[INFO] Open http://spoofer.local or http://" + WiFi.localIP().toString());
      Serial.println("========================================");
      Serial.println("Configured hosts for spoofing:");
      for (int i = 0; i < hostCount; i++) {
        Serial.println("  " + String(hosts[i].domain) + " -> " + String(hosts[i].targetIP) + " (enabled: " + String(hosts[i].enabled ? "yes" : "no") + ")");
      }
      Serial.println("========================================");
    } else {
      Serial.println();
      Serial.println("[WIFI] Connection failed! Starting captive portal...");
      startCaptivePortal();
    }
  } else {
    Serial.println("[WIFI] No saved WiFi settings, starting captive portal...");
    startCaptivePortal();
  }
}

unsigned long lastLedToggle = 0;
bool ledState = false;

void loop() {
  if (isCaptiveMode) {
    // Captive portal mode - только веб-сервер
    captiveServer.handleClient();
    
    // Быстрое мигание светодиодом (каждые 200мс)
    if (millis() - lastLedToggle >= 200) {
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState ? LOW : HIGH);
      lastLedToggle = millis();
    }
  } else {
    // Normal mode
    processDNS();
    webServer.handleClient();
    MDNS.update();
  }
  
  delay(1);
}