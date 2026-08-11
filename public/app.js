const editor = document.querySelector("#editor");
const connection = document.querySelector("#connection");
const revisionEl = document.querySelector("#revision");
const clientIdEl = document.querySelector("#clientId");
const pendingEl = document.querySelector("#pending");

let socket;
let clientId = "";
let revision = 0;
let confirmedText = "";
let localText = "";
let pending = [];
let appliedOpIds = new Set();
let applyingRemote = false;
let intentionalClose = false;
let hasSession = false;
let typingAtEnd = true;

function setStatus(online) {
  connection.textContent = online ? "Online" : "Offline";
  connection.className = `pill ${online ? "online" : "offline"}`;
}

function updateMeta() {
  revisionEl.textContent = `rev ${revision}`;
  clientIdEl.textContent = clientId || "connecting";
  pendingEl.textContent = String(pending.length);
}

function applyOp(text, op) {
  const position = Math.min(op.position, text.length);
  if (op.type === "insert") {
    return text.slice(0, position) + op.text + text.slice(position);
  }
  return text.slice(0, position) + text.slice(position + (op.count || 0));
}

function transform(a, b) {
  const op = { ...a };
  if (b.type === "insert") {
    const inserted = b.text.length;
    if (op.type === "insert") {
      const sameSpot = op.position === b.position;
      const bWinsTie = b.opId < op.opId || (b.opId === op.opId && b.clientId < op.clientId);
      if (op.position > b.position || (sameSpot && bWinsTie)) op.position += inserted;
    } else if (op.position >= b.position) {
      op.position += inserted;
    } else if (op.position + op.count > b.position) {
      op.count += inserted;
    }
    return op;
  }

  const deletedStart = b.position;
  const deletedEnd = b.position + b.count;
  if (op.type === "insert") {
    if (op.position > deletedEnd) op.position -= b.count;
    else if (op.position >= deletedStart) op.position = deletedStart;
    return op;
  }

  const start = op.position;
  const end = op.position + op.count;
  if (end <= deletedStart) return op;
  if (start >= deletedEnd) {
    op.position -= b.count;
    return op;
  }

  const overlapStart = Math.max(start, deletedStart);
  const overlapEnd = Math.min(end, deletedEnd);
  op.count -= Math.max(0, overlapEnd - overlapStart);
  if (start >= deletedStart) op.position = deletedStart;
  return op;
}

// Pending ops are authored against confirmed+earlier-pending. When composing,
// transform each against earlier pending so display matches server OT.
function composeDisplay() {
  let text = confirmedText;
  const normalized = [];
  for (const raw of pending) {
    let op = { ...raw };
    for (const prev of normalized) {
      op = transform(op, prev);
    }
    normalized.push(op);
    text = applyOp(text, op);
  }
  return text;
}

function replaceEditorValue(nextText) {
  const oldValue = editor.value;
  const oldStart = editor.selectionStart;
  const oldEnd = editor.selectionEnd;
  const wasAtEnd = oldStart >= oldValue.length && oldEnd >= oldValue.length;

  applyingRemote = true;
  editor.value = nextText;

  if (wasAtEnd || typingAtEnd) {
    editor.selectionStart = nextText.length;
    editor.selectionEnd = nextText.length;
  } else {
    const prefix = commonPrefixLength(oldValue.slice(0, oldStart), nextText);
    const pos = Math.min(nextText.length, prefix);
    editor.selectionStart = pos;
    editor.selectionEnd = pos;
  }

  localText = nextText;
  applyingRemote = false;
}

function commonPrefixLength(a, b) {
  const n = Math.min(a.length, b.length);
  let i = 0;
  while (i < n && a[i] === b[i]) i += 1;
  return i;
}

function diffToOp(before, after) {
  let start = 0;
  while (start < before.length && start < after.length && before[start] === after[start]) start++;

  let beforeEnd = before.length - 1;
  let afterEnd = after.length - 1;
  while (beforeEnd >= start && afterEnd >= start && before[beforeEnd] === after[afterEnd]) {
    beforeEnd--;
    afterEnd--;
  }

  const removed = beforeEnd - start + 1;
  const inserted = after.slice(start, afterEnd + 1);
  if (removed > 0 && inserted.length === 0) {
    return { type: "delete", position: start, count: removed, text: "" };
  }
  if (inserted.length > 0 && removed === 0) {
    return { type: "insert", position: start, count: 0, text: inserted };
  }

  return [
    removed > 0 ? { type: "delete", position: start, count: removed, text: "" } : null,
    inserted.length > 0 ? { type: "insert", position: start, count: 0, text: inserted } : null
  ].filter(Boolean);
}

function sendOp(partial) {
  const op = {
    kind: "op",
    ...partial,
    baseRevision: revision,
    opId: `${clientId}-${Date.now()}-${Math.random().toString(16).slice(2)}`,
    clientId
  };
  pending.push(op);
  socket.send(JSON.stringify(op));
}

function flushPending() {
  pending.forEach((op) => {
    socket.send(JSON.stringify({
      kind: "op",
      type: op.type,
      position: op.position,
      text: op.text || "",
      count: op.count || 0,
      baseRevision: revision,
      opId: op.opId
    }));
  });
}

editor.addEventListener("select", () => {
  typingAtEnd = editor.selectionStart >= editor.value.length;
});

editor.addEventListener("click", () => {
  typingAtEnd = editor.selectionStart >= editor.value.length;
});

editor.addEventListener("keyup", () => {
  typingAtEnd = editor.selectionStart >= editor.value.length;
});

editor.addEventListener("input", () => {
  if (applyingRemote || !socket || socket.readyState !== WebSocket.OPEN) return;
  const next = editor.value;
  typingAtEnd = editor.selectionStart >= next.length;
  const diff = diffToOp(localText, next);
  const ops = Array.isArray(diff) ? diff : [diff];
  ops.filter(Boolean).forEach(sendOp);
  localText = next;
  updateMeta();
});

function receiveOp(op) {
  if (!op.opId) return;
  if (appliedOpIds.has(op.opId)) {
    // Duplicate ACK / Pub/Sub echo — do not apply twice.
    pending = pending.filter((candidate) => candidate.opId !== op.opId);
    updateMeta();
    return;
  }
  appliedOpIds.add(op.opId);
  if (appliedOpIds.size > 5000) {
    appliedOpIds = new Set([...appliedOpIds].slice(-2500));
  }

  revision = op.revision;
  confirmedText = applyOp(confirmedText, op);

  const pendingIdx = pending.findIndex((candidate) => candidate.opId === op.opId);
  if (pendingIdx >= 0) {
    pending = pending.filter((candidate) => candidate.opId !== op.opId);
  } else {
    pending = pending.map((candidate) => transform(candidate, op));
  }

  const display = composeDisplay();
  if (display !== editor.value) replaceEditorValue(display);
  else localText = editor.value;
  updateMeta();
}

function applySnapshot(message) {
  revision = message.revision;
  confirmedText = message.document;
  pending = [];
  appliedOpIds = new Set();
  replaceEditorValue(confirmedText);
  updateMeta();
}

function applyCatchup(message) {
  (message.ops || []).forEach((op) => receiveOp(op));
  flushPending();
  updateMeta();
}

function connect() {
  const protocol = location.protocol === "https:" ? "wss" : "ws";
  socket = new WebSocket(`${protocol}://${location.host}/ws`);

  socket.addEventListener("open", () => {
    setStatus(true);
    if (hasSession) {
      socket.send(JSON.stringify({ kind: "sync", revision }));
    }
  });

  socket.addEventListener("close", () => {
    setStatus(false);
    editor.disabled = true;
    if (!intentionalClose) {
      setTimeout(connect, 1000);
    }
  });

  socket.addEventListener("message", (event) => {
    const message = JSON.parse(event.data);
    if (message.kind === "init") {
      if (!hasSession) {
        clientId = message.clientId;
        revision = message.revision;
        confirmedText = message.document;
        pending = [];
        appliedOpIds = new Set();
        hasSession = true;
        replaceEditorValue(confirmedText);
      } else {
        clientId = message.clientId;
        socket.send(JSON.stringify({ kind: "sync", revision }));
      }
      editor.disabled = false;
      updateMeta();
    } else if (message.kind === "op") {
      receiveOp(message);
    } else if (message.kind === "catchup") {
      applyCatchup(message);
    } else if (message.kind === "snapshot") {
      applySnapshot(message);
      flushPending();
    } else if (message.kind === "error") {
      console.error(message.message);
    }
  });
}

setStatus(false);
updateMeta();
connect();
