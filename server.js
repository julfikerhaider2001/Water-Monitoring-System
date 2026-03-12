/*
 * ============================================================
 *  SOUTHERN IOT — Water Monitor Dashboard Server
 *  Hosts the UI and proxies requests to ESP32 devices
 * ============================================================
 *
 *  HOW TO RUN:
 *    1. npm install
 *    2. node server.js
 *    3. Open browser: http://localhost:3000
 *
 *  HOW IT WORKS:
 *    - Serves the dashboard UI from /public/index.html
 *    - Acts as a proxy between browser and ESP32 devices
 *    - Handles CORS so browser can talk to ESP32 on local network
 *    - Stores device list in memory (can extend to database)
 *    - Exposes /api/device/:ip/* routes that forward to ESP32
 * ============================================================
 */

const express = require('express');
const cors    = require('cors');
const axios   = require('axios');
const path    = require('path');

const app  = express();
const PORT = 3000;

app.use(cors());
app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

// ============================================================
//  IN-MEMORY DEVICE REGISTRY
//  In production: replace with SQLite or InfluxDB
// ============================================================
let deviceRegistry = [];

// ============================================================
//  PROXY — Forward requests to ESP32 device by IP
//  URL pattern: /api/device/192.168.1.105/api/status
//  Becomes:     http://192.168.1.105/api/status
// ============================================================
app.all('/api/device/:ip/*', async (req, res) => {
  const deviceIp  = req.params.ip;
  const endpoint  = req.params[0];
  const targetUrl = `http://${deviceIp}/${endpoint}`;

  try {
    const options = {
      method:  req.method,
      url:     targetUrl,
      timeout: 10000,
      headers: { 'Content-Type': 'application/json' }
    };

    if (req.method === 'POST' && req.body) {
      options.data = req.body;
    }

    const response = await axios(options);

    // Update device registry with fresh data
    const existing = deviceRegistry.find(d => d.ip === deviceIp);
    if (existing && response.data) {
      existing.lastSeen  = new Date().toISOString();
      existing.online    = true;
      if (response.data.level  !== undefined) existing.level   = response.data.level;
      if (response.data.pump   !== undefined) existing.pump    = response.data.pump;
      if (response.data.mode   !== undefined) existing.mode    = response.data.mode;
      if (response.data.mac    !== undefined) existing.mac     = response.data.mac;
      if (response.data.fw     !== undefined) existing.fw      = response.data.fw;
    }

    res.status(response.status).json(response.data);
  } catch (err) {
    // Mark device offline if unreachable
    const existing = deviceRegistry.find(d => d.ip === deviceIp);
    if (existing) existing.online = false;

    res.status(503).json({
      ok:    false,
      error: 'Device unreachable',
      ip:    deviceIp,
      msg:   err.message
    });
  }
});

// ============================================================
//  DEVICE REGISTRY ENDPOINTS
// ============================================================

// Add a device to the registry
app.post('/api/registry/add', (req, res) => {
  const { ip, name, location } = req.body;
  if (!ip) return res.status(400).json({ ok: false, msg: 'IP required' });

  const existing = deviceRegistry.find(d => d.ip === ip);
  if (existing) {
    existing.name     = name || existing.name;
    existing.location = location || existing.location;
    return res.json({ ok: true, msg: 'Updated', device: existing });
  }

  const device = {
    ip,
    name:     name || `Device ${deviceRegistry.length + 1}`,
    location: location || 'Unknown',
    online:   false,
    lastSeen: null,
    level:    null,
    pump:     null,
    mode:     null,
    mac:      null,
    fw:       null,
    addedAt:  new Date().toISOString()
  };
  deviceRegistry.push(device);
  res.json({ ok: true, msg: 'Added', device });
});

// Remove a device
app.delete('/api/registry/:ip', (req, res) => {
  const ip = req.params.ip;
  deviceRegistry = deviceRegistry.filter(d => d.ip !== ip);
  res.json({ ok: true });
});

// Get all registered devices
app.get('/api/registry', (req, res) => {
  res.json({ devices: deviceRegistry });
});

// Health check for a specific device
app.get('/api/registry/:ip/ping', async (req, res) => {
  const ip = req.params.ip;
  try {
    const r = await axios.get(`http://${ip}/api/status`, { timeout: 8000 });
    const d = deviceRegistry.find(dev => dev.ip === ip);
    if (d) {
      d.online   = true;
      d.lastSeen = new Date().toISOString();
      if (r.data.level !== undefined) d.level = r.data.level;
      if (r.data.pump  !== undefined) d.pump  = r.data.pump;
      if (r.data.mode  !== undefined) d.mode  = r.data.mode;
      if (r.data.mac   !== undefined) d.mac   = r.data.mac;
    }
    res.json({ ok: true, online: true, data: r.data });
  } catch {
    const d = deviceRegistry.find(dev => dev.ip === ip);
    if (d) d.online = false;
    res.json({ ok: false, online: false });
  }
});

// ============================================================
//  SERVER START
// ============================================================
app.listen(PORT, '0.0.0.0', () => {
  console.log('\n========================================');
  console.log('  Southern IoT — Dashboard Server');
  console.log('========================================');
  console.log(`  URL:  http://localhost:${PORT}`);
  console.log(`  Also: http://<your-pc-ip>:${PORT}`);
  console.log('  Access from any device on same network');
  console.log('========================================\n');
});
