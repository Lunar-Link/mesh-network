/* Connections: USB through Web Serial, or the simulator bridge in serve.py. */
"use strict";
/* Transports */
function setConnected(kind) {
  const on = !!kind;
  $("conn").classList.toggle("on", on);
  $("conn-text").textContent = kind === "usb" ? "USB serial" : kind === "sim" ? "Simulator" : "Not connected";
  $("btn-usb").hidden = on;
  $("btn-sim").hidden = on || !simAvailable;
  $("btn-close").hidden = !on;
  if (kind !== "sim") $("simclock").hidden = true;
  renderHealth();
}

async function connected(t) {
  transport = t;
  st = fresh();
  simIds = new Set();
  if (t.kind === "sim") st.sim = { alive: null, links: [] };
  $("log").innerHTML = "";
  $("banner").style.display = "none";
  setConnected(t.kind);
  log(`connected (${t.kind === "usb" ? "USB serial, 115200 baud" : "simulator"})`, "ev");
  start();
}

function disconnected(reason) {
  if (!transport) return;
  transport = null;
  dropQueue();
  setConnected(null);
  log(reason || "disconnected", "err");
  renderAll();
}

async function connectUsb() {
  if (!("serial" in navigator)) {
    showBanner("This browser has no Web Serial. Use Chrome or Edge, and open the page from http://localhost (run serve.py).");
    return;
  }
  let port;
  try { port = await navigator.serial.requestPort(); } catch { return; }
  try { await port.open({ baudRate: 115200 }); } catch (e) {
    showBanner(`Could not open the port: ${e.message}. Close any other serial monitor (PlatformIO, PuTTY) using it.`);
    return;
  }
  const encoder = new TextEncoder();
  const writer = port.writable.getWriter();
  let reader = null, closing = false;
  const t = {
    kind: "usb",
    send: line => writer.write(encoder.encode(line + "\n")),
    close: async () => {
      closing = true;
      try { await reader?.cancel(); } catch {}
      await loop;
      try { writer.releaseLock(); } catch {}
      try { await port.close(); } catch {}
      disconnected("disconnected");
    },
  };
  const loop = (async () => {
    const dec = new TextDecoder();
    let buf = "";
    while (port.readable && !closing) {
      reader = port.readable.getReader();
      try {
        for (;;) {
          const { value, done } = await reader.read();
          if (done) break;
          buf += dec.decode(value, { stream: true });
          let i;
          while ((i = buf.indexOf("\n")) >= 0) {
            const line = buf.slice(0, i).replace(/\r$/, "");
            buf = buf.slice(i + 1);
            if (line) onLine(line);
          }
          if (buf.length > 4096) buf = "";
        }
      } catch (e) {
        if (!closing) log(`serial: ${e.message}`, "err");
      } finally {
        reader.releaseLock();
      }
    }
    if (!closing) disconnected("USB disconnected");
  })();
  connected(t);
}

let simAvailable = false;
function connectSim() {
  const src = new EventSource("/sim/events");
  const t = {
    kind: "sim",
    send: line => fetch("/sim/send", { method: "POST", body: line }).then(r => { if (!r.ok) throw new Error("bridge refused"); }),
    close: async () => { src.close(); disconnected("disconnected"); },
  };
  src.onmessage = e => onLine(e.data);
  src.addEventListener("sim", e => onSimStatus(JSON.parse(e.data)));
  src.onerror = () => { if (src.readyState === EventSource.CLOSED) disconnected("simulator bridge closed"); };
  src.onopen = () => { if (transport !== t) connected(t); };
}

$("btn-usb").onclick = connectUsb;
$("btn-sim").onclick = connectSim;
$("btn-close").onclick = () => transport && transport.close();
