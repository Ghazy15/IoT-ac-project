/* ================= SERVER.JS (The Brain) ================= */
const express = require("express");
const mqtt = require("mqtt");
const fs = require("fs");
const path = require("path");
const dgram = require("dgram"); 
const cors = require("cors");
const os = require("os"); 
const app = express();
const http = require('http').createServer(app);

app.use(express.json());
app.use(cors());
// Menggunakan process.cwd() agar .exe membaca folder public di lokasi fisik yang sama
app.use(express.static(path.join(process.cwd(), "public"))); 

/* ----- CONFIG ----- */
const PORT = 3000;
const MQTT_HOST = "mqtt://broker.hivemq.com"; 
const TOPIC_PREFIX = "/ac_project_unique_123/"; 

/* ----- STATE ----- */
const nodes = {}; // Stores { id, state, lastSeen, model }

// ================= FUNGSI MENCARI IP KOMPUTER (DIPERBAIKI) =================
function getLocalIP() {
  const interfaces = os.networkInterfaces();
  let fallbackIP = 'localhost';

  for (const devName in interfaces) {
    const iface = interfaces[devName];
    
    for (let i = 0; i < iface.length; i++) {
      const alias = iface[i];
      
      // Cari IPv4, bukan localhost, dan bukan jaringan internal
      if (alias.family === 'IPv4' && alias.address !== '127.0.0.1' && !alias.internal) {
        const nameLower = devName.toLowerCase();
        
        // 1. FILTER: Abaikan adapter virtual (VMware, VirtualBox, WSL, Hyper-V)
        if (nameLower.includes('vmware') || nameLower.includes('virtual') || nameLower.includes('veth') || nameLower.includes('wsl')) {
          continue; // Lewati jaringan ini
        }

        // 2. PRIORITAS: Jika namanya Wi-Fi atau Wireless, langsung gunakan ini!
        if (nameLower.includes('wi-fi') || nameLower.includes('wireless') || nameLower.includes('wlan')) {
          return alias.address; 
        }

        // 3. CADANGAN: Jika ada koneksi LAN fisik (Ethernet), simpan dulu
        fallbackIP = alias.address;
      }
    }
  }
  return fallbackIP;
}

// 1. MQTT CONNECTION
const client = mqtt.connect(MQTT_HOST, { port: 1883 });

client.on("connect", () => {
  console.log(`[MQTT] Connected to HiveMQ`);
  
  // 1. Subscribe to everything
  client.subscribe(TOPIC_PREFIX + "+/state");
  client.subscribe(TOPIC_PREFIX + "discovery");
  
  // 2. ⚡ FORCE SCAN: Tell all ESPs to announce themselves immediately
  console.log("[MQTT] Scanning for AC devices...");
  client.publish(TOPIC_PREFIX + "global/scan", "scan");
});

client.on("message", (topic, payload) => {
  try {
    const msg = JSON.parse(payload.toString());
    
    // Determine Node ID
    let nodeId = null;
    let model = "Office_AC_1"; // Default

    if (topic.includes("discovery")) {
      nodeId = msg.node;
      if (msg.model) model = msg.model;
    } else {
      nodeId = topic.split("/")[2]; // Extract AC_...
    }

    if (nodeId) {
      // Create node if not exists
      if (!nodes[nodeId]) {
        nodes[nodeId] = { id: nodeId, state: {}, lastSeen: Date.now(), model: model };
        console.log(`[NEW NODE DETECTED] ${nodeId} (${model})`);
        
        // Notify UI immediately about new node
        broadcast();
      }

      // Update timestamps and state
      nodes[nodeId].lastSeen = Date.now();
      if (topic.includes("state")) {
        nodes[nodeId].state = msg;
      } else if (msg.model) {
        nodes[nodeId].model = msg.model;
      }
      
      broadcast();
    }
  } catch (e) { 
    // console.error("MQTT Parse Error:", e); 
  }
});

/* ----- 2. REALTIME EVENTS (SSE) ----- */
let clients = [];
app.get("/events", (req, res) => {
  res.setHeader("Content-Type", "text/event-stream");
  res.setHeader("Cache-Control", "no-cache");
  res.setHeader("Connection", "keep-alive");
  clients.push(res);
  
  // Send initial data
  res.write(`data: ${JSON.stringify({ type: 'update', data: nodes })}\n\n`);

  req.on("close", () => clients = clients.filter(c => c !== res));
});

function broadcast() {
  const payload = `data: ${JSON.stringify({ type: 'update', data: nodes })}\n\n`;
  clients.forEach(c => c.write(payload));
}

// Menggunakan process.cwd() agar .exe membaca/menulis folder profiles di lokasi fisik
const PROFILE_DIR = path.join(process.cwd(), "profiles");
if (!fs.existsSync(PROFILE_DIR)) fs.mkdirSync(PROFILE_DIR);

/* ----- 3. API ENDPOINTS ----- */

// Get list of learned buttons
app.get("/api/signals/:model", (req, res) => {
  const modelDir = path.join(PROFILE_DIR, req.params.model);
  if (fs.existsSync(modelDir)) {
    const files = fs.readdirSync(modelDir)
      .filter(f => f.endsWith(".json"))
      .map(f => f.replace(".json", ""));
    res.json(files);
  } else {
    res.json([]);
  }
});

// Control Command
app.post("/api/control", (req, res) => {
  const { nodeId, action, name, ssid, pass } = req.body;
  const topic = `${TOPIC_PREFIX}${nodeId}/cmd`;
  
  // Handle Wi-Fi Update Special Case
  if (action === "update_wifi") {
    console.log(`[WIFI] Sending new creds to ${nodeId}`);
    client.publish(topic, JSON.stringify({ action: "wifi", ssid, pass })); 
  } else {
    console.log(`[CMD] ${action} -> ${nodeId} (${name})`);
    client.publish(topic, JSON.stringify({ action, name }));
  }
  
  res.json({ success: true });
});

// Upload (From ESP32)
app.post("/api/profile/upload", (req, res) => {
  const { modelName, signalName, data } = req.body;
  const dir = path.join(PROFILE_DIR, modelName);
  if (!fs.existsSync(dir)) fs.mkdirSync(dir, { recursive: true });
  
  fs.writeFileSync(path.join(dir, `${signalName}.json`), JSON.stringify(data));
  console.log(`[FILE] Learned & Saved: ${signalName}.json`);
  
  // Notify UI that a new button exists
  clients.forEach(c => c.write(`data: ${JSON.stringify({ type: 'new_signal', model: modelName })}\n\n`));
  res.json({ success: true });
});

/* ----- 4. UDP DISCOVERY ----- */
const udpServer = dgram.createSocket('udp4');
udpServer.on('message', (msg, rinfo) => {
  if (msg.toString().trim() === 'DISCOVER_AC_SERVER') {
    udpServer.send('AC_SERVER_HERE', rinfo.port, rinfo.address);
  }
});
udpServer.bind(9999);

// ================= START SERVER =================
http.listen(PORT, () => {
  const networkIP = getLocalIP();
  console.log(`\n===========================================`);
  console.log(` 🌟 SMART AC CONTROLLER SERVER BERJALAN 🌟 `);
  console.log(`===========================================`);
  console.log(`\nBuka Dashboard melalui Browser:`);
  console.log(`➜ Di Komputer ini : http://localhost:${PORT}`);
  console.log(`➜ Di HP / Laptop  : http://${networkIP}:${PORT}`);
  console.log(`\n(Pastikan HP terhubung ke WiFi yang sama)`);
  console.log(`===========================================\n`);
});