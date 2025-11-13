const fs = require('fs');
const path = require('path');
const http = require('http');
const https = require('https');
const express = require('express');
const cors = require('cors');
const WebSocket = require('ws');
const { type } = require('os');
const axios = require('axios');
require("dotenv").config();

const HOST = process.env.HOST || '0.0.0.0';
const HTTP_PORT = parseInt(process.env.HTTP_PORT || '80', 10);
const HTTPS_PORT = parseInt(process.env.HTTPS_PORT || '443', 10);
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

// HTTP server that redirects to HTTPS (for normal HTTP requests)
const httpServer = http.createServer((req, res) => {
    const host = req.headers.host || '';
    const target = `https://${host}${req.url}`;
    res.writeHead(301, { Location: target });
    res.end();
});

// HTTPS server (self-signed)
let httpsOptions = {};
try {
    httpsOptions = {
        key: fs.readFileSync('certs/key.pem'),
        cert: fs.readFileSync('certs/cert.pem'),
    };
} catch (e) {
    console.warn('HTTPS cert not found, start HTTPS only if certs exist.');
}
const httpsServer = Object.keys(httpsOptions).length
    ? https.createServer(httpsOptions, app)
    : https.createServer({}, app); // will fail to start if no certs on port 443; ok if you only use HTTP during dev

// WebSocket servers share the HTTPS server for secure ws (wss) and optionally a separate ws dev port
const wss = new WebSocket.Server({ server: httpsServer, path: '/ws' });

// Optional: a plain ws server for LAN dev without SSL (e.g., port 8080)
const devWsPort = parseInt(process.env.WS_DEV_PORT || '8080', 10);
const wsDevServer = http.createServer();
const wssDev = new WebSocket.Server({ server: wsDevServer, path: '/ws' });

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
        //console.log(`Generated AssemblyAI token: ${AAI_TOKEN} for session ${sessionId}`);

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

wssDev.on('connection', (ws, req) => {

    console.log('ESP connected');
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

wss.on('connection', (ws, req) => {

    console.log('UI connected');
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
                } else
                    if (payload.type === "Begin") {
                        console.log(`AAI streaming begun`);
                        sendToUI(ui, { type: 'start' });
                        broadcastToESP(esp, { start: true });
                    } else
                        console.log(`AAI message:`, payload);
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



httpServer.listen(HTTP_PORT, HOST, () => {
    console.log(`HTTP redirect server listening on http://${HOST}:${HTTP_PORT}`);
});

httpsServer.listen(HTTPS_PORT, HOST, () => {
    console.log(`HTTPS/WS server listening on https://${HOST}:${HTTPS_PORT}`);
});

// Optional dev ws (no SSL)
wsDevServer.listen(devWsPort, HOST, () => {
    console.log(`Dev WS (no SSL) listening ws://${HOST}:${devWsPort}/ws`);
});
