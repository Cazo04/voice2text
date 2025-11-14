const http = require('http');
const express = require('express');
const cors = require('cors');
const WebSocket = require('ws');
const axios = require('axios');
require("dotenv").config();

const HOST = process.env.HOST || '0.0.0.0';
const HTTP_PORT = 80;
const WS_UI_PORT = 8080;
const WS_ESP_PORT = 8081;
const AAI_SAMPLE_RATE = parseInt(process.env.SAMPLE_RATE || '16000', 10);
const ASSEMBLYAI_API_KEY = process.env.ASSEMBLYAI_API_KEY || '';

console.log('Using ASSEMBLYAI_API_KEY:', process.env.ASSEMBLYAI_API_KEY ? '***' : '(not set)');

const app = express();

// Disable CORS (allow all)
app.use(cors({ origin: '*', credentials: true }));
app.use(express.json());
app.set('trust proxy', true);

// Serve static UI (place index.html in ./public)
app.use(express.static('public'));

// Main HTTP server on port 80
const httpServer = http.createServer(app);

// WebSocket servers on separate ports
const wsUIServer = http.createServer();
const wssUI = new WebSocket.Server({ server: wsUIServer, path: '/wsui' });

const wsESPServer = http.createServer();
const wssESP = new WebSocket.Server({ server: wsESPServer, path: '/wsesp' });

// Proxy middleware for WebSocket upgrade requests
httpServer.on('upgrade', (request, socket, head) => {
  const { url } = request;
  
  if (url.startsWith('/wsui')) {
    // Proxy to UI WebSocket on port 8080
    wssUI.handleUpgrade(request, socket, head, (ws) => {
      wssUI.emit('connection', ws, request);
    });
  } else if (url.startsWith('/wsesp')) {
    // Proxy to ESP WebSocket on port 8081
    wssESP.handleUpgrade(request, socket, head, (ws) => {
      wssESP.emit('connection', ws, request);
    });
  } else {
    socket.destroy();
  }
});

async function generateTempToken(expiresInSeconds) {
  const url = `https://streaming.assemblyai.com/v3/token?expires_in_seconds=${expiresInSeconds}`;
  try {
    const response = await axios.get(url, {
      headers: {
        Authorization: ASSEMBLYAI_API_KEY,
      },
    });
    return response.data.token;
  } catch (error) {
    console.error("Error generating temp token:", error.response?.data || error.message);
    throw error;
  }
}

// Helper: create AssemblyAI WS per session
function connectAssemblyAI(sampleRate = AAI_SAMPLE_RATE) {
  return new Promise(async (resolve, reject) => {
    const AAI_TOKEN = await generateTempToken(60);
    const url = `wss://streaming.assemblyai.com/v3/ws?sample_rate=${sampleRate}&formatted_finals=true&token=${AAI_TOKEN}`;
    const aaiWS = new WebSocket(url);
    aaiWS.on('open', () => resolve(aaiWS));
    aaiWS.on('error', (err) => reject(err));
  });
}

function sendSafe(ws, obj) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify(obj));
  }
}

function broadcastToESP(esp, obj) {
  sendSafe(esp, obj);
}

function sendToUI(ui, obj) {
  sendSafe(ui, obj);
}

let esp = null;
let ui = null;
let aai = null;
let turns = {};

// ESP WebSocket connection
wssESP.on('connection', (ws, req) => {
  console.log('ESP connected on /wsesp');
  esp = ws;
  ws.on('message', (data) => {
    if (aai && aai.readyState === WebSocket.OPEN) {
      aai.send(data);
    }
  });
  ws.on('close', () => {
    console.log('ESP disconnected');
    esp = null;
  });
});

// UI WebSocket connection
wssUI.on('connection', (ws, req) => {
  console.log('UI connected on /wsui');
  ui = ws;
  ws.on('message', async (data) => {
    const msg = JSON.parse(data);
    if (msg.type === 'start') {
      turns = {}; // Reset turns for new session
      const aaiWs = await connectAssemblyAI(AAI_SAMPLE_RATE);
      aai = aaiWs;
      aai.on('message', (data) => {
        let payload = {};
        try { payload = JSON.parse(data); } catch { return; }
        if (payload.type === "Turn") {
          const { turn_order, transcript } = payload;
          turns[turn_order] = transcript;
          // Full transcript
          const orderedTurns = Object.keys(turns)
            .sort((a, b) => Number(a) - Number(b))
            .map((k) => turns[k])
            .join(" ");
          console.log(`Turn ${turn_order}: ${transcript}`);
          sendToUI(ui, { type: 'transcript', text: orderedTurns });
          broadcastToESP(esp, { text: orderedTurns });
        } else if (payload.type === "Begin") {
          console.log(`AAI streaming begun`);
          sendToUI(ui, { type: 'start' });
          broadcastToESP(esp, { start: true });
        } else {
          console.log(`AAI message:`, payload);
        }
      });
      aai.on('close', () => {
        console.log(`AAI WS closed`);
        aai = null;
        sendToUI(ui, { type: 'stop' });
        broadcastToESP(esp, { stop: true });
      });
    }
    if (msg.type === 'stop') {
      console.log('Stopping session');
      broadcastToESP(esp, { stop: true });
      if (aai && aai.readyState === WebSocket.OPEN) {
        aai.send(JSON.stringify({ type: 'Terminate' }));
      }
      aai = null;
      turns = {};
    }
  });
  ws.on('close', () => {
    console.log('UI disconnected');
    ui = null;
  });
});

// Start servers
httpServer.listen(HTTP_PORT, HOST, () => {
  console.log(`HTTP server with proxy listening on http://${HOST}:${HTTP_PORT}`);
  console.log(`  - "/" → serves static files (port 80)`);
  console.log(`  - "/wsui" → WebSocket UI (proxied internally to port 8080)`);
  console.log(`  - "/wsesp" → WebSocket ESP (proxied internally to port 8081)`);
});

wsUIServer.listen(WS_UI_PORT, '127.0.0.1', () => {
  console.log(`Internal WS UI server on 127.0.0.1:${WS_UI_PORT}`);
});

wsESPServer.listen(WS_ESP_PORT, '127.0.0.1', () => {
  console.log(`Internal WS ESP server on 127.0.0.1:${WS_ESP_PORT}`);
});
