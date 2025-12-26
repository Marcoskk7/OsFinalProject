// Simple HTTP -> TCP bridge for the C++ server
// Exposes JSON APIs and serves the web UI from ../web

const net = require('net');
const path = require('path');
const express = require('express');

const TCP_HOST = process.env.TCP_HOST || '127.0.0.1';
const TCP_PORT = parseInt(process.env.TCP_PORT || '5555', 10);
const HTTP_PORT = parseInt(process.env.PORT || '3000', 10);

const MessageType = {
  AuthRequest: 0,
  AuthResponse: 1,
  CommandRequest: 2,
  CommandResponse: 3,
  Error: 4,
};

function serializeMessage(type, payload) {
  return `${type}\n${payload}`;
}

function sendTcpCommand(payload) {
  return new Promise((resolve, reject) => {
    const socket = new net.Socket();
    let buffer = Buffer.alloc(0);
    let expectedLength = null;

    socket.on('error', (err) => {
      reject(err);
    });

    socket.on('data', (chunk) => {
      buffer = Buffer.concat([buffer, chunk]);
      if (expectedLength === null && buffer.length >= 4) {
        expectedLength = buffer.readUInt32BE(0);
      }
      if (expectedLength !== null && buffer.length >= 4 + expectedLength) {
        const body = buffer.slice(4, 4 + expectedLength).toString();
        socket.end();
        resolve(body);
      }
    });

    socket.connect(TCP_PORT, TCP_HOST, () => {
      const body = serializeMessage(MessageType.CommandRequest, payload);
      const len = Buffer.alloc(4);
      len.writeUInt32BE(Buffer.byteLength(body), 0);
      socket.write(len);
      socket.write(body);
    });
  });
}

async function forwardCommand(command, sessionId) {
  const payload = sessionId ? `SESSION ${sessionId} CMD ${command}` : command;
  const raw = await sendTcpCommand(payload);
  // raw format: "<type>\n<payload>"
  const nl = raw.indexOf('\n');
  if (nl === -1) {
    return { type: MessageType.Error, payload: 'Malformed response' };
  }
  const typeInt = parseInt(raw.slice(0, nl), 10);
  const respPayload = raw.slice(nl + 1);
  return { type: typeInt, payload: respPayload };
}

const app = express();
app.use(express.json());

// CORS (simple allow-all for dev)
app.use((req, res, next) => {
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET,POST,OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');
  if (req.method === 'OPTIONS') return res.sendStatus(204);
  next();
});

app.post('/api/ping', async (req, res) => {
  try {
    const resp = await forwardCommand('PING');
    res.json({ ok: true, type: resp.type, payload: resp.payload });
  } catch (err) {
    res.status(502).json({ ok: false, error: err.message });
  }
});

app.post('/api/login', async (req, res) => {
  const { username, password } = req.body || {};
  if (!username || !password) {
    return res.status(400).json({ ok: false, error: 'Missing username or password' });
  }
  try {
    const resp = await forwardCommand(`LOGIN ${username} ${password}`);
    // If the underlying server returned an Error message type, treat as failure
    if (resp.type === MessageType.Error) {
      return res.status(401).json({ ok: false, error: resp.payload || 'Authentication failed' });
    }

    // Expect the payload to include a SESSION token on success
    if (typeof resp.payload !== 'string' || !/SESSION\s+\S+/i.test(resp.payload)) {
      return res.status(401).json({ ok: false, error: resp.payload || 'Invalid credentials' });
    }

    // Successful login
    return res.json({ ok: true, type: resp.type, payload: resp.payload });
  } catch (err) {
    res.status(502).json({ ok: false, error: err.message });
  }
});

app.post('/api/command', async (req, res) => {
  const { command, sessionId } = req.body || {};
  if (!command || typeof command !== 'string') {
    return res.status(400).json({ ok: false, error: 'Missing command' });
  }
  try {
    const resp = await forwardCommand(command, sessionId);
    res.json({ ok: true, type: resp.type, payload: resp.payload });
  } catch (err) {
    res.status(502).json({ ok: false, error: err.message });
  }
});

// Simple health endpoint for the web UI
app.post('/api/health', (_req, res) => {
  res.json({ ok: true });
});

// Serve static web UI
const webDir = path.join(__dirname, '..', 'web');

// Serve login page as the root route so users land on the login screen first
app.get('/', (_req, res) => {
  res.sendFile(path.join(webDir, 'login.html'));
});

app.use(express.static(webDir));

app.get('/healthz', (req, res) => res.json({ ok: true }));

app.listen(HTTP_PORT, () => {
  console.log(`Gateway listening on http://localhost:${HTTP_PORT} (TCP -> ${TCP_HOST}:${TCP_PORT})`);
});
