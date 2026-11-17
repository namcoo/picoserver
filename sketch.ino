#include <NCMEthernetlwIP.h>
#include <WebServer.h>
#include <WiFi.h>

NCMEthernetlwIP eth;
WebServer server(80);

bool serverStarted = false;

const IPAddress HP_SERVER(IP);
const IPAddress ZERO_SERVER(IP);

const unsigned long POLL_INTERVAL = 5000;
unsigned long lastPoll = 0;
unsigned long lastStatus = 0;

String hpMetrics = "";
String zeroMetrics = "";

bool hpOnline = false;
bool zeroOnline = false;

const int LED_PIN = LED_BUILTIN;
unsigned long lastLedUpdate = 0;
bool ledState = false;

const char dashboardHTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Home Server Monitor</title>
<style>
body {
  background: #101820;
  color: #e8edf2;
  font-family: Arial, sans-serif;
  margin: 0;
  padding: 20px;
}
h1 { color: #55d6be; }
.subtitle { color: #aab7c4; }
.grid {
  display: grid;
  grid-template-columns: repeat(auto-fit,minmax(280px,1fr));
  gap: 18px;
}
.card {
  background: #1c2935;
  padding: 20px;
  border-radius: 12px;
  margin-top: 15px;
}
.online { color: #55d6be; }
.offline { color: #ff6464; }
.metric { margin: 15px 0; }
.bar {
  height: 9px;
  background: #344454;
  border-radius: 8px;
  overflow: hidden;
}
.fill {
  height: 100%;
  background: #55d6be;
  width: 0%;
}
.small { color: #aab7c4; font-size: 13px; }
.container {
  border-top: 1px solid #344454;
  padding: 9px 0;
  font-size: 13px;
  overflow-wrap: anywhere;
}
</style>
</head>
<body>

<h1>Home Server Monitor</h1>
<p class="subtitle">Raspberry Pi Pico 2 - USB NCM - Live monitoring</p>

<div class="grid">

<div class="card">
<h2>HP t620</h2>
<p>IP</p>
<h3 id="hpStatus">Checking...</h3>

<div class="metric">
CPU <span id="hpCPU">--</span>
<div class="bar"><div class="fill" id="hpCPUBar"></div></div>
</div>

<div class="metric">
RAM <span id="hpRAM">--</span>
<div class="bar"><div class="fill" id="hpRAMBar"></div></div>
</div>

<div class="metric">
Disk <span id="hpDisk">--</span>
<div class="bar"><div class="fill" id="hpDiskBar"></div></div>
</div>

<p>Uptime <b id="hpUptime">--</b></p>
<p>Docker <b id="hpDocker">--</b></p>
<h3>Containers</h3>
<div id="hpContainers">--</div>
</div>

<div class="card">
<h2>Pi Zero 2 W</h2>
<p>IP</p>
<h3 id="zeroStatus">Checking...</h3>

<div class="metric">
CPU <span id="zeroCPU">--</span>
<div class="bar"><div class="fill" id="zeroCPUBar"></div></div>
</div>

<div class="metric">
RAM <span id="zeroRAM">--</span>
<div class="bar"><div class="fill" id="zeroRAMBar"></div></div>
</div>

<div class="metric">
Disk <span id="zeroDisk">--</span>
<div class="bar"><div class="fill" id="zeroDiskBar"></div></div>
</div>

<p>Uptime <b id="zeroUptime">--</b></p>
<p>Docker <b id="zeroDocker">--</b></p>
<h3>Containers</h3>
<div id="zeroContainers">--</div>
</div>

</div>

<p class="small">Refreshes every 5 seconds - Data provided by your DietPi servers</p>

<script>
function setText(id, value) {
  document.getElementById(id).textContent = value;
}

function setBar(id, value) {
  const n = Math.max(0, Math.min(100, Number(value) || 0));
  document.getElementById(id).style.width = n + "%";
}

function renderServer(prefix, server) {
  const status = document.getElementById(prefix + "Status");

  if (!server || !server.online || !server.metrics) {
    status.textContent = "OFFLINE";
    status.className = "offline";

    ["CPU","RAM","Disk","Uptime","Docker"].forEach(k => {
      setText(prefix + k, "--");
    });

    ["CPU","RAM","Disk"].forEach(k => {
      setBar(prefix + k + "Bar", 0);
    });

    setText(prefix + "Containers", "--");
    return;
  }

  const m = server.metrics;
  status.textContent = "ONLINE";
  status.className = "online";

  setText(prefix + "CPU", m.cpu.usage_percent.toFixed(1) + "%");
  setText(prefix + "RAM", m.memory.usage_percent.toFixed(1) + "%");
  setText(prefix + "Disk", m.disk.usage_percent.toFixed(1) + "%");

  setBar(prefix + "CPUBar", m.cpu.usage_percent);
  setBar(prefix + "RAMBar", m.memory.usage_percent);
  setBar(prefix + "DiskBar", m.disk.usage_percent);

  setText(prefix + "Uptime", m.uptime.hours.toFixed(1) + " h");
  setText(prefix + "Docker",
    m.docker.running + "/" + m.docker.total);

  const containerBox = document.getElementById(prefix + "Containers");
  containerBox.replaceChildren();

  m.docker.containers.forEach(c => {
    const item = document.createElement("div");
    item.className = "container";
    item.textContent = c.name + " - " + c.status;
    containerBox.appendChild(item);
  });
}

async function refresh() {
  try {
    const response = await fetch("/api/metrics", {cache:"no-store"});
    if (!response.ok) throw new Error("HTTP " + response.status);

    const data = await response.json();

    renderServer("hp", data.hp);
    renderServer("zero", data.zero);
  } catch (e) {
    console.log("Dashboard update failed:", e);
  }
}

refresh();
setInterval(refresh, 5000);
</script>

</body>
</html>
)rawliteral";

void handleRoot() {
  server.send(200, "text/html", dashboardHTML);
}

void handleHealth() {
  server.send(200, "application/json", "{\"status\":\"online\"}");
}

bool fetchMetrics(IPAddress host, String &body) {
  WiFiClient client;

  if (!client.connect(host, 8765)) {
    client.stop();
    return false;
  }

  client.setTimeout(500);

  client.print("GET /metrics HTTP/1.0\r\n");
  client.print("Host: ");
  client.print(host);
  client.print(":8765\r\n");
  client.print("Connection: close\r\n\r\n");

  unsigned long started = millis();
  String response = "";

  while (millis() - started < 3000) {
    while (client.available()) {
      char c = client.read();
      response += c;
      started = millis();
    }

    if (!client.connected() && !client.available()) {
      break;
    }

    delay(2);
  }

  client.stop();

  int separator = response.indexOf("\r\n\r\n");

  if (separator < 0) return false;

  String headers = response.substring(0, separator);
  String payload = response.substring(separator + 4);

  if (!headers.startsWith("HTTP/1.0 200") &&
      !headers.startsWith("HTTP/1.1 200")) {
    return false;
  }

  payload.trim();

  if (!payload.startsWith("{") || !payload.endsWith("}")) {
    return false;
  }

  body = payload;
  return true;
}

void pollServers() {
  Serial.println();
  Serial.println("===== SERVER MONITORING =====");
  Serial.flush();

  hpOnline = fetchMetrics(HP_SERVER, hpMetrics);
  zeroOnline = fetchMetrics(ZERO_SERVER, zeroMetrics);

  Serial.print("HP t620: ");
  Serial.println(hpOnline ? "ONLINE" : "OFFLINE");

  Serial.print("Pi Zero 2 W: ");
  Serial.println(zeroOnline ? "ONLINE" : "OFFLINE");

  Serial.println("=============================");
  Serial.flush();
}

void handleMetrics() {
  String json = "{\"hp\":{\"online\":";
  json += hpOnline ? "true" : "false";
  json += ",\"metrics\":";

  json += hpOnline ? hpMetrics : "null";

  json += "},\"zero\":{\"online\":";
  json += zeroOnline ? "true" : "false";
  json += ",\"metrics\":";

  json += zeroOnline ? zeroMetrics : "null";

  json += "}}";

  server.send(200, "application/json", json);
}

void updateLED() {
  unsigned long interval;

  if (hpOnline && zeroOnline) {
    ledState = true;
    digitalWrite(LED_PIN, HIGH);
    return;
  }

  if (!hpOnline && !zeroOnline) {
    interval = 250;
  } else {
    interval = 1000;
  }

  if (millis() - lastLedUpdate >= interval) {
    lastLedUpdate = millis();
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? HIGH : LOW);
  }
}

void printNetworkDiagnostics() {
  Serial.println();
  Serial.println("===== PICO NETWORK CONFIGURATION =====");

  Serial.print("IP Address: ");
  Serial.println(eth.localIP());

  Serial.print("Subnet Mask: ");
  Serial.println(eth.subnetMask());

  Serial.print("Gateway: ");
  Serial.println(eth.gatewayIP());

  Serial.print("Routable: ");
  Serial.println(eth.routable() ? "YES" : "NO");

  Serial.println("======================================");
  Serial.flush();
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(150);
    digitalWrite(LED_PIN, LOW);
    delay(150);
  }

  Serial.println();
  Serial.println("==============================");
  Serial.println("PICO 2 HOME SERVER DASHBOARD");
  Serial.println("==============================");
  Serial.flush();

  Serial.println("DIAGNOSTIC A: Before eth.begin()");
  Serial.flush();

  Serial.println("Starting USB NCM...");
  Serial.flush();

  eth.config(IPAddress(192, 168, 137, 2), IPAddress(192, 168, 137, 1), IPAddress(255, 255, 255, 0), IPAddress(192, 168, 137, 1));
  bool ncmResult = eth.begin();

  Serial.println("DIAGNOSTIC B: eth.begin() returned");
  Serial.flush();

  if (!ncmResult) {
    Serial.println("ERROR: Ethernet initialization failed");
    Serial.flush();
    return;
  }

  Serial.println("USB NCM initialization successful.");
  Serial.flush();

  printNetworkDiagnostics();
}

void loop() {
  if (!serverStarted && millis() - lastStatus >= 5000) {
    lastStatus = millis();

    Serial.print("NCM STATUS: ");
    Serial.println(eth.connected() ? "CONNECTED" : "NOT CONNECTED");
    Serial.flush();
  }

  if (eth.connected() && !serverStarted) {
    Serial.println();
    Serial.println("USB ETHERNET CONNECTED");

    Serial.print("Pico IP: ");
    Serial.println(eth.localIP());

    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/metrics", HTTP_GET, handleMetrics);
    server.on("/health", HTTP_GET, handleHealth);

    server.begin();
    serverStarted = true;

    Serial.println("HTTP SERVER STARTED");
    Serial.println("Port: 80");

    Serial.print("Dashboard: http://");
    Serial.println(eth.localIP());
    Serial.flush();

    pollServers();
    lastPoll = millis();
  }

  if (serverStarted) {
    server.handleClient();

    if (millis() - lastPoll >= POLL_INTERVAL) {
      lastPoll = millis();
      pollServers();
    }

    server.handleClient();
    updateLED();
  }

  delay(2);
}