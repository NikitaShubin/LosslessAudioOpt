"use strict";
const LS_TOKEN = "llao_token";
let token = localStorage.getItem(LS_TOKEN) || "";
let pollTimer = null;
let lastSeq = 0;

const el = (id) => document.getElementById(id);
const loginDiv = el("login"), appDiv = el("app");
const tokenInput = el("token-input"), loginForm = el("login-form"), loginError = el("login-error");
const versionEl = el("version"), cTotal = el("c-total"), cDone = el("c-done"), cFailed = el("c-failed");
const addForm = el("add-form"), addPath = el("add-path"), addRecursive = el("add-recursive"), addMsg = el("add-msg");
const queueBody = el("queue-body"), connStatus = el("conn-status"), lastSeqEl = el("last-seq");

function showLogin(msg) {
  loginDiv.classList.remove("hidden");
  appDiv.classList.add("hidden");
  if (msg) { loginError.textContent = msg; loginError.classList.remove("hidden"); }
  else loginError.classList.add("hidden");
  stopPoll();
}
function showApp() {
  loginDiv.classList.add("hidden");
  appDiv.classList.remove("hidden");
  loginError.classList.add("hidden");
  startPoll();
}

async function api(path, opts) {
  opts = opts || {};
  opts.headers = opts.headers || {};
  if (token) opts.headers["Authorization"] = "Bearer " + token;
  if (opts.body && typeof opts.body !== "string") {
    opts.headers["Content-Type"] = "application/json";
    opts.body = JSON.stringify(opts.body);
  }
  const r = await fetch(path, opts);
  if (r.status === 401) { showLogin("Неверный токен (401). Введите корректный токен."); throw new Error("401"); }
  return r;
}
async function rpc(cmd, args) {
  const r = await api("/rpc", {method:"POST", body:{cmd, args: args||{}}});
  const j = await r.json();
  if (!j.ok) throw new Error(j.error || j.code || "rpc failed");
  return j.result;
}

function setConn(ok, text) {
  connStatus.textContent = text;
  connStatus.className = "conn " + (ok ? "ok" : "err");
}
function chipState(s) {
  const m = {queued:"queued", prep:"prep", running:"running", ok:"ok", skip:"skip", error:"error"};
  const cls = m[s] || "queued";
  return `<span class="chip chip-${cls}">${s}</span>`;
}
function taskDot(st) {
  const c = st==="ok" ? "ok" : st==="running" ? "running" : st==="failed" ? "failed" : "pend";
  return `<span class="task-dot task-${c}" title="${st}"></span>`;
}
function renderQueue(rows) {
  if (!rows || rows.length===0) {
    queueBody.innerHTML = `<tr><td colspan="6" class="empty">Очередь пуста</td></tr>`;
    return;
  }
  let html = "";
  for (const r of rows) {
    const pct = (r.pct||0).toFixed(1);
    const bar = `<span class="bar"><span class="bar-fill" style="width:${Math.min(100, r.pct||0)}%"></span></span> ${pct}%`;
    const tasks = (r.tasks||[]).map(taskDot).join("");
    const btn = (r.state==="ok"||r.state==="skip"||r.state==="error") ? "" : `<button data-cancel="${r.id}">Отменить</button>`;
    html += `<tr><td>${r.id}</td><td>${esc(r.label)}</td><td>${chipState(r.state)}</td><td>${bar}</td><td><span class="tasks">${tasks}</span></td><td>${btn}</td></tr>`;
  }
  queueBody.innerHTML = html;
  queueBody.querySelectorAll("[data-cancel]").forEach(b=>{
    b.addEventListener("click", async ()=>{
      const id = parseInt(b.getAttribute("data-cancel"),10);
      try { await rpc("cancel-file", {id}); addMsg.textContent="Файл "+id+" отменён"; addMsg.className="msg ok"; } catch(e){ addMsg.textContent=e.message; addMsg.className="msg err"; }
    });
  });
}
function esc(s){ return String(s).replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;").replace(/"/g,"&quot;"); }

async function pollState(){
  try {
    const r = await api("/api/state");
    const j = await r.json();
    versionEl.textContent = j.version ? "v"+j.version : "";
    const c = j.counters||{}; cTotal.textContent=c.total||0; cDone.textContent=c.done||0; cFailed.textContent=c.failed||0;
    lastSeqEl.textContent = "seq "+(j.last_seq||0);
    renderQueue(j.rows);
    setConn(true, "Подключено");
  } catch(e){
    if (String(e.message)==="401") return;
    setConn(false, "Ошибка: "+e.message);
  }
}
function startPoll(){
  stopPoll();
  pollState();
  pollTimer = setInterval(()=>{
    if (document.hidden) return;
    pollState();
  }, 1000);
}
function stopPoll(){ if(pollTimer){ clearInterval(pollTimer); pollTimer=null; } }

loginForm.addEventListener("submit", (e)=>{
  e.preventDefault();
  const t = tokenInput.value.trim();
  if (!t) { loginError.textContent="Введите токен"; loginError.classList.remove("hidden"); return; }
  token = t; localStorage.setItem(LS_TOKEN, token);
  showApp();
  pollState().catch(()=>{});
});
el("btn-logout").addEventListener("click", ()=>{
  token=""; localStorage.removeItem(LS_TOKEN); tokenInput.value=""; showLogin();
});
addForm.addEventListener("submit", async (e)=>{
  e.preventDefault();
  const p = addPath.value.trim();
  if (!p) { addMsg.textContent="Введите путь"; addMsg.className="msg err"; return; }
  addMsg.textContent="Отправка..."; addMsg.className="msg";
  try {
    const res = await rpc("add", {paths:[p], recursive: !!addRecursive.checked});
    const added = (res.added||[]).length, rej = (res.rejected||[]).length;
    let msg = `Добавлено: ${added}`; if(rej) msg+=`, отклонено: ${rej} (${(res.rejected[0]||{}).reason||""})`;
    addMsg.textContent=msg; addMsg.className="msg ok"; addPath.value="";
  } catch(err){ addMsg.textContent=err.message; addMsg.className="msg err"; }
});
el("btn-cancel-all").addEventListener("click", async ()=>{
  if(!confirm("Отменить все ожидающие файлы (пауза очереди)?")) return;
  try{ await rpc("cancel-all", {}); addMsg.textContent="Очередь поставлена на паузу"; addMsg.className="msg ok"; }catch(e){ addMsg.textContent=e.message; addMsg.className="msg err"; }
});
el("btn-shutdown").addEventListener("click", async ()=>{
  if(!confirm("Выключить демон? Обработка активных файлов завершится, затем демон остановится.")) return;
  try{ await rpc("shutdown", {}); setConn(false,"Демон выключается..."); }catch(e){ addMsg.textContent=e.message; addMsg.className="msg err"; }
});

document.addEventListener("visibilitychange", ()=>{ if(!document.hidden) pollState(); });

(function init(){
  if (token) { showApp(); }
  else showLogin();
})();
