/*
 * ESP32 Cloud CCTV relay
 *
 *   ESP32 cam  --POST /api/frame/<id> (JPEG)-->  this server  --WebSocket-->  browsers
 *
 * Env vars (set on Render):
 *   CAM_KEY    shared secret the cameras must send in the X-Cam-Key header
 *   DASH_PASS  password the dashboard asks for (leave empty to disable)
 *   PORT       provided by Render automatically
 */
const express = require('express');
const http = require('http');
const path = require('path');
const { WebSocketServer, WebSocket } = require('ws');

const PORT = process.env.PORT || 3000;
const CAM_KEY = process.env.CAM_KEY || '';
const DASH_PASS = process.env.DASH_PASS || '';
const OFFLINE_MS = 15000;          // no frame for this long -> OFFLINE
const MAX_BUFFERED = 1_000_000;    // drop frames for a slow viewer instead of queueing

const app = express();
const server = http.createServer(app);
const wss = new WebSocketServer({ server, path: '/ws' });

/** id -> { frame: Buffer, ts: number, frames: number, recent: number[] } */
const cams = new Map();

// ---------------------------------------------------------------- helpers
const cleanId = (s) => String(s).replace(/[^a-z0-9_-]/gi, '').slice(0, 16);

function viewerCount() {
  let n = 0;
  for (const c of wss.clients) if (c.readyState === WebSocket.OPEN && c.authed) n++;
  return n;
}

function camStatus() {
  const now = Date.now();
  return [...cams.entries()]
    .sort(([a], [b]) => a.localeCompare(b))
    .map(([id, c]) => ({
      id,
      online: now - c.ts < OFFLINE_MS,
      fps: +(c.recent.length / 5).toFixed(1),
      ago: Math.round((now - c.ts) / 1000),
      frames: c.frames,
      bytes: c.frame ? c.frame.length : 0,
    }));
}

function broadcastJSON(obj) {
  const msg = JSON.stringify(obj);
  for (const c of wss.clients)
    if (c.readyState === WebSocket.OPEN && c.authed) c.send(msg);
}

// binary frame = [idLen:1][id bytes][jpeg]
function broadcastFrame(id, jpeg) {
  const idBuf = Buffer.from(id, 'utf8');
  const packet = Buffer.concat([Buffer.from([idBuf.length]), idBuf, jpeg]);
  for (const c of wss.clients) {
    if (c.readyState !== WebSocket.OPEN || !c.authed) continue;
    if (c.bufferedAmount > MAX_BUFFERED) continue;  // viewer is lagging, skip
    c.send(packet);
  }
}

// ---------------------------------------------------------------- HTTP
app.use(express.static(path.join(__dirname, 'public')));

app.post('/api/frame/:id', express.raw({ type: () => true, limit: '3mb' }), (req, res) => {
  if (CAM_KEY && req.get('x-cam-key') !== CAM_KEY) return res.status(401).json({ error: 'bad key' });
  const id = cleanId(req.params.id);
  if (!id || !Buffer.isBuffer(req.body) || req.body.length < 100) return res.status(400).json({ error: 'no frame' });

  let cam = cams.get(id);
  if (!cam) {
    cam = { frame: null, ts: 0, frames: 0, recent: [] };
    cams.set(id, cam);
    console.log(`[cam] new camera: ${id}`);
  }
  cam.frame = req.body;
  cam.ts = Date.now();
  cam.frames++;
  cam.recent.push(cam.ts);
  while (cam.recent.length && cam.recent[0] < cam.ts - 5000) cam.recent.shift();

  broadcastFrame(id, req.body);
  // the camera uses this to decide how fast to send
  res.json({ viewers: viewerCount() });
});

app.get('/api/status', (_req, res) => res.json({ cams: camStatus(), viewers: viewerCount() }));

app.get('/snapshot/:id.jpg', (req, res) => {
  if (DASH_PASS && req.query.pass !== DASH_PASS) return res.status(401).end();
  const cam = cams.get(cleanId(req.params.id));
  if (!cam || !cam.frame) return res.status(404).end();
  res.set('Content-Type', 'image/jpeg').set('Cache-Control', 'no-store').send(cam.frame);
});

app.get('/healthz', (_req, res) => res.send('ok'));

// ---------------------------------------------------------------- WebSocket (viewers)
wss.on('connection', (ws) => {
  ws.authed = !DASH_PASS;
  ws.isAlive = true;
  ws.on('pong', () => (ws.isAlive = true));
  ws.send(JSON.stringify({ type: 'hello', needAuth: !!DASH_PASS }));

  const sendInitial = () => {
    ws.send(JSON.stringify({ type: 'status', cams: camStatus(), viewers: viewerCount() }));
    for (const [id, c] of cams) if (c.frame) {
      const idBuf = Buffer.from(id);
      ws.send(Buffer.concat([Buffer.from([idBuf.length]), idBuf, c.frame]));
    }
  };
  if (ws.authed) sendInitial();

  ws.on('message', (data, isBinary) => {
    if (isBinary) return;
    let msg; try { msg = JSON.parse(data); } catch { return; }
    if (msg.type === 'auth') {
      if (msg.pass === DASH_PASS) { ws.authed = true; ws.send(JSON.stringify({ type: 'auth', ok: true })); sendInitial(); }
      else ws.send(JSON.stringify({ type: 'auth', ok: false }));
    }
  });
});

// status heartbeat + dead-socket cleanup
setInterval(() => {
  for (const c of wss.clients) {
    if (!c.isAlive) { c.terminate(); continue; }
    c.isAlive = false;
    c.ping();
  }
  broadcastJSON({ type: 'status', cams: camStatus(), viewers: viewerCount() });
}, 2000);

server.listen(PORT, () => {
  console.log(`ESP32 Cloud CCTV relay listening on :${PORT}`);
  console.log(`camera key: ${CAM_KEY ? 'set' : 'NOT SET (anyone can push frames)'} | dashboard password: ${DASH_PASS ? 'set' : 'NOT SET (public)'}`);
});
