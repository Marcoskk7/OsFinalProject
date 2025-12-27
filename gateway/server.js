// Simple HTTP -> TCP bridge for the C++ server
// Exposes JSON APIs and serves the web UI from ../web

const net = require('net');
const path = require('path');
const express = require('express');

const TCP_HOST = process.env.TCP_HOST || '127.0.0.1';
const TCP_PORT = parseInt(process.env.TCP_PORT || '5555', 10);
const HTTP_PORT = parseInt(process.env.PORT || '3000', 10);

function sendTcpEnvelope(envelope) {
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
        resolve(body); // JSON string: {"type":"...","payload":{...}}
      }
    });

    socket.connect(TCP_PORT, TCP_HOST, () => {
      const body = JSON.stringify(envelope);
      const len = Buffer.alloc(4);
      len.writeUInt32BE(Buffer.byteLength(body), 0);
      socket.write(len);
      socket.write(body);
    });
  });
}

function parseCommandLine(line) {
  const s = (line || '').trim();
  if (!s) {
    return { name: '', rawArgs: '', args: [] };
  }
  const firstSpace = s.search(/\s/);
  if (firstSpace === -1) {
    return { name: s, rawArgs: '', args: [] };
  }
  const name = s.slice(0, firstSpace);
  const rawArgs = s.slice(firstSpace).trim();
  const args = rawArgs ? rawArgs.split(/\s+/).filter(Boolean) : [];
  return { name, rawArgs, args };
}

function toLegacyPayload(commandName, respPayload) {
  const data = respPayload && respPayload.data ? respPayload.data : {};

  if (commandName === 'PING') {
    return 'PONG';
  }

  if (commandName === 'LIST_PAPERS' && data && Array.isArray(data.papers)) {
    // Keep old UI parser working: "[ID: 1] title (Status: Submitted)"
    return data.papers
      .map((p) => `[ID: ${p.id}] ${p.title} (Status: ${p.status})`)
      .join('\n');
  }

  // Default: stringify returned data or whole payload
  if (data && Object.keys(data).length > 0) {
    return JSON.stringify(data, null, 2);
  }
  return JSON.stringify(respPayload, null, 2);
}

async function forwardCommand(command, sessionId) {
  const parsed = parseCommandLine(command);
  if (!parsed.name) {
    return { ok: false, error: 'Missing command' };
  }

  const cmdJson = {
    sessionId: sessionId ? String(sessionId) : null,
    cmd: parsed.name,
    args: parsed.args,
    rawArgs: parsed.rawArgs,
  };

  const reqEnvelope = { type: 'CommandRequest', payload: cmdJson };
  const raw = await sendTcpEnvelope(reqEnvelope);

  let respEnvelope;
  try {
    respEnvelope = JSON.parse(raw);
  } catch (e) {
    return { ok: false, error: 'Malformed response from TCP server (not JSON)' };
  }

  const payload = respEnvelope && respEnvelope.payload ? respEnvelope.payload : {};
  const isOk = !!(payload && payload.ok);

  if (!isOk) {
    const msg = payload && payload.error && payload.error.message ? payload.error.message : 'Command failed';
    return { ok: false, type: respEnvelope.type, error: msg, raw: payload };
  }

  return {
    ok: true,
    type: respEnvelope.type,
    payload: toLegacyPayload(parsed.name, payload),
    data: payload.data,
  };
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
    if (!resp.ok) {
      return res.status(502).json(resp);
    }
    res.json(resp);
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
    // Talk to C++ server via JSON protocol, but respond in legacy string for existing web UI.
    const resp = await forwardCommand(`LOGIN ${username} ${password}`);
    if (!resp.ok) {
      return res.status(401).json(resp);
    }

    // Build legacy payload: "SESSION <id> ROLE <role> USER <username> USERID <userId>"
    const data = resp.data || {};
    if (!data.sessionId || !data.role) {
      return res.status(401).json({ ok: false, error: 'Login failed: missing sessionId/role in response', raw: resp });
    }
    const userId = (data.userId !== undefined && data.userId !== null) ? data.userId : '';
    const legacy = `SESSION ${data.sessionId} ROLE ${data.role} USER ${data.username || username} USERID ${userId}`.trim();
    return res.json({ ok: true, type: resp.type, payload: legacy, data });
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
    if (!resp.ok) {
      return res.status(200).json(resp);
    }
    res.json(resp);
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
