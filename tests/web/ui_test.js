// Drives the bundled web client in jsdom against a live server.
// Run via tests/web_ui.sh, which starts the server and an echo agent.
const { JSDOM } = require("jsdom");
const port = process.argv[2];
const OPTS = { runScripts: "dangerously", resources: "usable", pretendToBeVisual: true,
  beforeParse(w) { w.TextEncoder = TextEncoder; w.TextDecoder = TextDecoder; } };
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function open(name) {
  const dom = await JSDOM.fromURL(`http://127.0.0.1:${port}/`, OPTS);
  const w = dom.window, d = w.document;
  // jsdom lacks requestSubmit on forms in some versions
  if (!w.HTMLFormElement.prototype.requestSubmit) w.HTMLFormElement.prototype.requestSubmit = function () { this.dispatchEvent(new w.Event("submit", { cancelable: true })); };
  d.getElementById("name").value = name;
  d.getElementById("login").dispatchEvent(new w.Event("submit", { cancelable: true }));
  for (let i = 0; i < 50 && d.getElementById("app").classList.contains("hidden"); i++) await sleep(50);
  return { w, d };
}
function say(c, text) {
  c.d.getElementById("text").value = text;
  c.d.getElementById("compose").dispatchEvent(new c.w.Event("submit", { cancelable: true }));
}
const logText = c => c.d.getElementById("log").textContent;
let failed = false;
const report = (name, ok) => { if (!ok) failed = true; console.log((ok ? "ok   - " : "FAIL - ") + "web: " + name); };

(async () => {
  const a = await open("webalice");
  const b = await open("webbob");
  report("logged in", !a.d.getElementById("app").classList.contains("hidden") && !b.d.getElementById("app").classList.contains("hidden"));
  await sleep(200);
  say(a, "hi from the browser");
  await sleep(400);
  report("lobby delivered", logText(b).includes("hi from the browser"));
  report("own message shown", logText(a).includes("hi from the browser"));
  // join a room
  a.d.getElementById("joinName").value = "web";
  a.d.getElementById("joinBtn").click();
  await sleep(300);
  report("joined room", a.d.getElementById("rooms").textContent.includes("#web"));
  // DM
  b.d.getElementById("dmName").value = "webalice";
  b.d.getElementById("dmBtn").click();
  say(b, "secret note");
  await sleep(400);
  a.d.getElementById("dms").querySelector(".item")?.click();
  report("dm delivered", logText(a).includes("secret note"));
  // agents list and request (an echo agent is running in the test)
  a.d.getElementById("refreshAgents").click();
  await sleep(300);
  const agentItem = [...a.d.getElementById("agents").querySelectorAll(".item")].find(e => e.textContent.startsWith("webecho"));
  report("agent listed", !!agentItem);
  if (agentItem) {
    agentItem.click();
    say(a, "ping the agent");
    await sleep(500);
    report("agent answered", logText(a).includes("echo: ping the agent"));
  }
  // the server restarts: the old session is gone, the client must start a new one
  console.log("restart-now");
  const fs = require("fs");
  for (let i = 0; i < 100 && !fs.existsSync(process.argv[3]); i++) await sleep(100);
  for (let i = 0; i < 100 && !a.d.getElementById("status").textContent.includes("connected as webalice"); i++) await sleep(100);
  await sleep(500);
  report("reconnects with a new session after a restart", a.d.getElementById("status").textContent.includes("connected as webalice"));
  [...a.d.getElementById("rooms").querySelectorAll(".item")].find(e => e.textContent.startsWith("#lobby"))?.click();
  say(a, "after the restart");
  await sleep(400);
  const ok = logText(a).includes("after the restart") && !logText(a).includes("Not connected");
  if (!ok) console.log(logText(a).slice(-300));
  report("can send after a restart", ok);
  report("rejoined rooms after a restart", a.d.getElementById("rooms").textContent.includes("#web"));

  // bad name is rejected client-side
  const c = await JSDOM.fromURL(`http://127.0.0.1:${port}/`, OPTS);
  c.window.document.getElementById("name").value = "has space";
  c.window.document.getElementById("login").dispatchEvent(new c.window.Event("submit", { cancelable: true }));
  await sleep(100);
  report("bad name message", c.window.document.getElementById("loginError").textContent.length > 0);
  process.exit(failed ? 1 : 0);
})().catch(e => { console.error(e); process.exit(1); });
