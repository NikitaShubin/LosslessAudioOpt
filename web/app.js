"use strict";
const LS_TOKEN = "llao_token";
let token = localStorage.getItem(LS_TOKEN) || "";
let pollTimer = null;
let lastSeq = 0;
let currentRows = [];
let isPaused = false;

const el = (id) => document.getElementById(id);
const loginDiv = el("login"), appDiv = el("app");
const tokenInput = el("token-input"), loginForm = el("login-form"), loginError = el("login-error");
const versionEl = el("version"), cTotal = el("c-total"), cDone = el("c-done"), cFailed = el("c-failed");
const addForm = el("add-form"), addPath = el("add-path"), addRecursive = el("add-recursive"), addMsg = el("add-msg");
const queueBody = el("queue-body"), connStatus = el("conn-status"), lastSeqEl = el("last-seq");
const btnPause = el("btn-pause"), btnClearCompleted = el("btn-clear-completed");

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
function taskDot(st, idx) {
  const c = st==="ok" ? "ok" : st==="running" ? "running" : st==="failed" ? "failed" : "pend";
  const title = `Задача #${idx}: ${st}`;
  return `<span class="task-dot task-${c}" title="${esc(title)}"></span>`;
}
function esc(s){ return String(s).replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;").replace(/"/g,"&quot;"); }

function progressBar(r){
  const tasks = r.tasks || [];
  // Для queued без задач — прочерк
  if (tasks.length===0) {
    if (r.state==="queued") return `<span class="muted">—</span>`;
    if (r.state==="prep") return `<span class="bar"><span class="bar-fill" style="width:15%"></span></span> prep`;
  }
  const done = tasks.filter(t=>t==="ok"||t==="failed").length;
  const total = tasks.length;
  const pct = total ? (done/total*100) : 0;
  // Для завершённых — 100% + экономия
  if (r.state==="ok" && r.pct) {
    return `<span class="bar"><span class="bar-fill" style="width:100%"></span></span> 100% <span class="saving">(${r.pct.toFixed(1)}%)</span>`;
  }
  if (r.state==="skip"||r.state==="error") {
    return `<span class="bar"><span class="bar-fill" style="width:100%"></span></span> ${pct.toFixed(0)}%`;
  }
  return `<span class="bar"><span class="bar-fill" style="width:${pct}%"></span></span> ${pct.toFixed(0)}%`;
}

function renderQueue(rows){
  currentRows = rows || [];
  if (!rows || rows.length===0) {
    queueBody.innerHTML = `<tr><td colspan="7" class="empty">Очередь пуста</td></tr>`;
    return;
  }
  let html = "";
  for (let i=0;i<rows.length;i++){
    const r = rows[i];
    const bar = progressBar(r);
    const tasks = (r.tasks||[]).map((st,idx)=>taskDot(st,idx)).join("");
    let actions = "";
    // Остановить для активных, Удалить для завершённых
    if (r.state==="queued"||r.state==="prep"||r.state==="running") {
      actions += `<button data-stop="${r.id}" title="Остановить обработку файла">Остановить</button> `;
    } else {
      actions += `<button data-remove="${r.id}" class="danger" title="Удалить из списка">Удалить</button> `;
    }
    // Стрелки перемещения
    const canUp = i>0, canDown = i<rows.length-1;
    actions += `<span class="move-btns">`;
    if (canUp) actions += `<button data-move="top" data-id="${r.id}" title="В начало">⇤</button><button data-move="up" data-id="${r.id}" title="Вверх">↑</button>`;
    if (canDown) actions += `<button data-move="down" data-id="${r.id}" title="Вниз">↓</button><button data-move="bottom" data-id="${r.id}" title="В конец">⇥</button>`;
    actions += `</span>`;
    const handle = `<span class="drag-handle" draggable="true" data-drag="${r.id}" title="Перетащите для изменения порядка">≡</span>`;
    html += `<tr draggable="true" data-id="${r.id}"><td>${handle}</td><td>${r.id}</td><td>${esc(r.label)}</td><td>${chipState(r.state)}</td><td>${bar}</td><td><span class="tasks">${tasks}</span></td><td>${actions}</td></tr>`;
  }
  queueBody.innerHTML = html;
  // Остановить
  queueBody.querySelectorAll("[data-stop]").forEach(b=>{
    b.addEventListener("click", async ()=>{
      const id = parseInt(b.getAttribute("data-stop"),10);
      try { await rpc("cancel-file", {id}); addMsg.textContent="Файл "+id+" остановлен"; addMsg.className="msg ok"; } catch(e){ addMsg.textContent=e.message; addMsg.className="msg err"; }
    });
  });
  // Удалить
  queueBody.querySelectorAll("[data-remove]").forEach(b=>{
    b.addEventListener("click", async ()=>{
      const id = parseInt(b.getAttribute("data-remove"),10);
      if (!confirm(`Удалить файл #${id} из очереди?`)) return;
      try { await rpc("remove", {id}); addMsg.textContent="Файл "+id+" удалён"; addMsg.className="msg ok"; } catch(e){ addMsg.textContent=e.message; addMsg.className="msg err"; }
    });
  });
  // Перемещение стрелками
  queueBody.querySelectorAll("[data-move]").forEach(b=>{
    b.addEventListener("click", async ()=>{
      const id = parseInt(b.getAttribute("data-id"),10);
      const dir = b.getAttribute("data-move");
      const ids = currentRows.map(r=>r.id);
      const idx = ids.indexOf(id);
      if (idx===-1) return;
      let newOrder = ids.slice();
      if (dir==="up" && idx>0) { [newOrder[idx-1], newOrder[idx]]=[newOrder[idx], newOrder[idx-1]]; }
      else if (dir==="down" && idx<ids.length-1) { [newOrder[idx], newOrder[idx+1]]=[newOrder[idx+1], newOrder[idx]]; }
      else if (dir==="top" && idx>0) { newOrder.splice(idx,1); newOrder.unshift(id); }
      else if (dir==="bottom" && idx<ids.length-1) { newOrder.splice(idx,1); newOrder.push(id); }
      else return;
      try { await rpc("reorder", {order:newOrder}); } catch(e){ addMsg.textContent=e.message; addMsg.className="msg err"; }
    });
  });
  // Drag & drop за ручку
  let dragId = null;
  queueBody.querySelectorAll("[data-drag]").forEach(h=>{
    h.addEventListener("dragstart", e=>{ dragId = parseInt(h.getAttribute("data-drag"),10); e.dataTransfer.effectAllowed="move"; });
  });
  queueBody.querySelectorAll("tr[data-id]").forEach(tr=>{
    tr.addEventListener("dragover", e=>{ e.preventDefault(); tr.classList.add("drag-over"); });
    tr.addEventListener("dragleave", ()=> tr.classList.remove("drag-over"));
    tr.addEventListener("drop", async e=>{
      e.preventDefault(); tr.classList.remove("drag-over");
      if (dragId===null) return;
      const targetId = parseInt(tr.getAttribute("data-id"),10);
      if (dragId===targetId) return;
      const ids = currentRows.map(r=>r.id);
      const from = ids.indexOf(dragId), to = ids.indexOf(targetId);
      if (from===-1||to===-1) return;
      let newOrder = ids.slice();
      newOrder.splice(from,1);
      newOrder.splice(to,0,dragId);
      try { await rpc("reorder", {order:newOrder}); } catch(err){ addMsg.textContent=err.message; addMsg.className="msg err"; }
      dragId=null;
    });
  });
}

async function pollState(){
  try {
    const r = await api("/api/state");
    const j = await r.json();
    versionEl.textContent = j.version ? "v"+j.version : "";
    const c = j.counters||{}; cTotal.textContent=c.total||0; cDone.textContent=c.done||0; cFailed.textContent=c.failed||0;
    isPaused = !!j.paused;
    btnPause.textContent = isPaused ? "Запустить" : "Остановить";
    btnPause.title = isPaused ? "Возобновить обработку очереди" : "Приостановить очередь";
    lastSeqEl.textContent = "seq "+(j.last_seq||0);
    renderQueue(j.rows);
    setConn(true, isPaused ? "Пауза" : "Подключено");
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
btnPause.addEventListener("click", async ()=>{
  const willPause = !isPaused;
  if (willPause) { if (!confirm("Остановить очередь? Текущие файлы доработают, новые не запустятся.")) return; }
  try {
    await rpc(willPause ? "pause" : "resume", {});
    addMsg.textContent = willPause ? "Очередь остановлена" : "Очередь запущена";
    addMsg.className="msg ok";
  } catch(e){ addMsg.textContent=e.message; addMsg.className="msg err"; }
});
btnClearCompleted.addEventListener("click", async ()=>{
  const done = currentRows.filter(r=>r.state==="ok"||r.state==="skip"||r.state==="error");
  if (done.length===0) { addMsg.textContent="Нет завершённых файлов"; addMsg.className="msg"; return; }
  if (!confirm(`Удалить ${done.length} завершённых файлов из списка?`)) return;
  for (const r of done) { try{ await rpc("remove", {id:r.id}); }catch(e){} }
  addMsg.textContent=`Удалено ${done.length}`; addMsg.className="msg ok";
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
