"use strict";
const LS_TOKEN = "llao_token";
let token = localStorage.getItem(LS_TOKEN) || "";
let pollTimer = null;
let lastSeq = 0;
let currentRows = [];
let isPaused = false;
let selectedIds = new Set();
let lastCheckedId = null;
let dragIds = null;
let ghost = null;
let isDragging = false;

const el = (id) => document.getElementById(id);
const loginDiv = el("login"), appDiv = el("app");
const tokenInput = el("token-input"), loginForm = el("login-form"), loginError = el("login-error");
const versionEl = el("version"), cTotal = el("c-total"), cDone = el("c-done"), cFailed = el("c-failed");
const addForm = el("add-form"), addPath = el("add-path"), addRecursive = el("add-recursive"), addMsg = el("add-msg");
const queueBody = el("queue-body"), connStatus = el("conn-status"), lastSeqEl = el("last-seq");
const btnPause = el("btn-pause"), btnClearCompleted = el("btn-clear-completed");
const pausedBadge = el("paused-badge"), doneBadge = el("c-done-badge");
const chkAll = el("chk-all"), selInfo = el("sel-info"), selCount = el("sel-count");
const btnBatchStop = el("btn-batch-stop"), btnBatchDelete = el("btn-batch-delete");

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

function sanitizeToken(t) {
  return String(t == null ? "" : t).replace(/\s+/g, "");
}
async function api(path, opts) {
  opts = opts || {};
  opts.headers = opts.headers || {};
  if (token) {
    const clean = sanitizeToken(token);
    if (clean !== token) token = clean;
    if (/[^\x20-\x7E]/.test(clean) || clean.length === 0) {
      showLogin("В токене недопустимые символы. Скопируйте 64 hex-символа из консоли демона без пробелов.");
      throw new Error("bad token");
    }
    opts.headers["Authorization"] = "Bearer " + clean;
  }
  if (opts.body && typeof opts.body !== "string") {
    opts.headers["Content-Type"] = "application/json";
    opts.body = JSON.stringify(opts.body);
  }
  let r;
  try {
    r = await fetch(path, opts);
  } catch (e) {
    throw new Error("Сеть: " + (e && e.message ? e.message : e));
  }
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
function taskDot(st, idx, info) {
  const c = st==="ok" ? "ok" : st==="running" ? "running" : st==="failed" ? "failed" : "pend";
  let title = "";
  if (info && info.fmt) {
    title = `${info.fmt}/${info.variant} ${info.params ? info.params.join(" ") : ""} — ${st}`;
    if (info.note) title += ` (${info.note})`;
  } else {
    title = `Задача #${idx}: ${st}`;
  }
  return `<span class="task-dot task-${c}" title="${esc(title)}"></span>`;
}
function esc(s){ return String(s).replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;").replace(/"/g,"&quot;"); }

function progressBar(r){
  const tasks = r.tasks || [];
  if (tasks.length===0) {
    if (r.state==="queued") return `<span class="muted">—</span>`;
    if (r.state==="prep") return `<span class="bar"><span class="bar-fill" style="width:15%"></span></span> prep`;
  }
  const done = tasks.filter(t=>t==="ok"||t==="failed").length;
  const total = tasks.length;
  const pct = total ? (done/total*100) : 0;
  if (r.state==="ok" && r.pct) {
    return `<span class="bar"><span class="bar-fill" style="width:100%"></span></span> 100% <span class="saving">(${r.pct.toFixed(1)}%)</span>`;
  }
  if (r.state==="skip"||r.state==="error") {
    return `<span class="bar"><span class="bar-fill" style="width:100%"></span></span> ${pct.toFixed(0)}%`;
  }
  return `<span class="bar"><span class="bar-fill" style="width:${pct}%"></span></span> ${pct.toFixed(0)}%`;
}

function updateSelectionUI(){
  const n = selectedIds.size;
  if (n>0) { selInfo.classList.remove("hidden"); selCount.textContent=n; }
  else selInfo.classList.add("hidden");
  if (!currentRows.length) { chkAll.checked=false; chkAll.indeterminate=false; return; }
  const all = currentRows.every(r=>selectedIds.has(r.id));
  const some = currentRows.some(r=>selectedIds.has(r.id));
  chkAll.checked = all;
  chkAll.indeterminate = !all && some;
}

function renderQueue(rows){
  if (isDragging) return;
  currentRows = rows || [];
  for (let id of [...selectedIds]) if (!currentRows.find(r=>r.id===id)) selectedIds.delete(id);
  if (!rows || rows.length===0) {
    queueBody.innerHTML = `<tr><td colspan="8" class="empty">Очередь пуста</td></tr>`;
    updateSelectionUI();
    return;
  }
  let html = "";
  for (let i=0;i<rows.length;i++){
    const r = rows[i];
    const pos = i+1;
    const bar = progressBar(r);
    const infos = r.task_infos || [];
    const tasks = (r.tasks||[]).map((st,idx)=>taskDot(st,idx, infos[idx])).join("");
    let actions = "";
    if (r.state==="queued"||r.state==="prep"||r.state==="running") {
      actions += `<button data-stop="${r.id}" title="Остановить">⏸</button> `;
    } else {
      actions += `<button data-restart="${r.id}" title="Запустить снова">↻</button> `;
      actions += `<button data-remove="${r.id}" class="danger" title="Удалить">🗑</button> `;
    }
    const canUp = i>0, canDown = i<rows.length-1;
    actions += `<span class="move-btns">`;
    if (canUp) actions += `<button data-move="top" data-id="${r.id}" title="В начало">⇤</button><button data-move="up" data-id="${r.id}" title="Вверх">↑</button>`;
    if (canDown) actions += `<button data-move="down" data-id="${r.id}" title="Вниз">↓</button><button data-move="bottom" data-id="${r.id}" title="В конец">⇥</button>`;
    actions += `</span>`;
    const handle = `<span class="drag-handle" draggable="true" data-drag="${r.id}" title="Перетащите">≡</span>`;
    const chk = `<input type="checkbox" data-chk="${r.id}" ${selectedIds.has(r.id)?"checked":""}>`;
    const selClass = selectedIds.has(r.id) ? "selected" : "";
    html += `<tr data-id="${r.id}" class="${selClass}"><td>${chk}</td><td>${handle}</td><td title="#${r.id}">${pos}</td><td>${esc(r.label)}</td><td>${chipState(r.state)}</td><td>${bar}</td><td><span class="tasks">${tasks}</span></td><td>${actions}</td></tr>`;
  }
  queueBody.innerHTML = html;
  updateSelectionUI();
  queueBody.querySelectorAll("[data-stop]").forEach(b=>{
    b.addEventListener("click", async ()=>{
      const id = parseInt(b.getAttribute("data-stop"),10);
      try { await rpc("cancel-file", {id}); } catch(e){ addMsg.textContent=e.message; addMsg.className="msg err"; }
    });
  });
  queueBody.querySelectorAll("[data-remove]").forEach(b=>{
    b.addEventListener("click", async ()=>{
      const id = parseInt(b.getAttribute("data-remove"),10);
      if (!confirm(`Удалить файл #${id} из очереди?`)) return;
      try { await rpc("remove", {id}); selectedIds.delete(id); } catch(e){ addMsg.textContent=e.message; addMsg.className="msg err"; }
    });
  });
  queueBody.querySelectorAll("[data-restart]").forEach(b=>{
    b.addEventListener("click", async ()=>{
      const id = parseInt(b.getAttribute("data-restart"),10);
      try {
        const res = await rpc("restart", {ids:[id]});
        addMsg.textContent = (res.restarted||[]).length ? "Файл #"+id+" запущен снова" : "Файл #"+id+" не перезапущен";
        addMsg.className = "msg ok";
      } catch(e){ addMsg.textContent=e.message; addMsg.className="msg err"; }
    });
  });
  queueBody.querySelectorAll("[data-move]").forEach(b=>{
    b.addEventListener("click", async ()=>{
      const id = parseInt(b.getAttribute("data-id"),10);
      const dir = b.getAttribute("data-move");
      const ids = currentRows.map(r=>r.id);
      let block = selectedIds.has(id) ? currentRows.filter(r=>selectedIds.has(r.id)).map(r=>r.id) : [id];
      let remaining = ids.filter(x=>!block.includes(x));
      let newOrder;
      if (dir==="up"||dir==="top") {
        let firstPos = Math.min(...block.map(x=>ids.indexOf(x)));
        let insertAt = 0;
        if (dir==="up") {
          let beforeId = null;
          for (let i=firstPos-1;i>=0;i--) if (!block.includes(ids[i])) { beforeId = ids[i]; break; }
          insertAt = beforeId===null ? 0 : remaining.indexOf(beforeId)+1;
        } else if (dir==="top") insertAt=0;
        newOrder = remaining.slice();
        newOrder.splice(insertAt,0,...block);
      } else {
        let lastPos = Math.max(...block.map(x=>ids.indexOf(x)));
        let insertAt;
        if (dir==="down") {
          let afterId=null;
          for (let i=lastPos+1;i<ids.length;i++) if (!block.includes(ids[i])) { afterId=ids[i]; break; }
          if (afterId===null) insertAt=remaining.length;
          else insertAt = remaining.indexOf(afterId)+1;
        } else if (dir==="bottom") insertAt=remaining.length;
        let newOrder2 = remaining.slice();
        newOrder2.splice(insertAt,0,...block);
        newOrder = newOrder2;
      }
      try { await rpc("reorder", {order:newOrder}); } catch(e){ addMsg.textContent=e.message; addMsg.className="msg err"; }
    });
  });
  queueBody.querySelectorAll("[data-chk]").forEach(cb=>{
    cb.addEventListener("click", (e)=>{
      const id = parseInt(cb.getAttribute("data-chk"),10);
      const idx = currentRows.findIndex(r=>r.id===id);
      if (e.shiftKey && lastCheckedId!==null) {
        const lastIdx = currentRows.findIndex(r=>r.id===lastCheckedId);
        if (lastIdx!==-1) {
          const [a,b] = [lastIdx, idx].sort((x,y)=>x-y);
          for (let i=a;i<=b;i++) selectedIds.add(currentRows[i].id);
        }
      } else {
        if (cb.checked) selectedIds.add(id); else selectedIds.delete(id);
        lastCheckedId = id;
      }
      updateSelectionUI();
      // не перерисовываем полностью, только обновляем чекбоксы
      queueBody.querySelectorAll("tr").forEach(tr=>{
        const tid = parseInt(tr.getAttribute("data-id"),10);
        tr.classList.toggle("selected", selectedIds.has(tid));
        const c = tr.querySelector("[data-chk]");
        if (c) c.checked = selectedIds.has(tid);
      });
      updateSelectionUI();
    });
  });
  // drag & drop - вешаем один раз на handles
  queueBody.querySelectorAll("[data-drag]").forEach(h=>{
    h.addEventListener("dragstart", e=>{
      const id = parseInt(h.getAttribute("data-drag"),10);
      if (selectedIds.has(id)) dragIds = currentRows.filter(r=>selectedIds.has(r.id)).map(r=>r.id);
      else dragIds = [id];
      isDragging = true;
      e.dataTransfer.effectAllowed="move";
      e.dataTransfer.setData("text/plain", String(id));
      ghost = document.createElement("div");
      ghost.className="drag-ghost";
      ghost.textContent = dragIds.length>1 ? `${dragIds.length} файлов` : currentRows.find(r=>r.id===id).label;
      document.body.appendChild(ghost);
      e.dataTransfer.setDragImage(ghost, 10, 10);
      queueBody.querySelectorAll("tr[data-id]").forEach(tr=>{
        const tid = parseInt(tr.getAttribute("data-id"),10);
        if (dragIds.includes(tid)) tr.classList.add("dragging");
      });
    });
    h.addEventListener("dragend", ()=>{
      isDragging = false;
      if (ghost && ghost.parentNode) ghost.parentNode.removeChild(ghost);
      ghost=null;
      dragIds=null;
      queueBody.querySelectorAll("tr").forEach(tr=>tr.classList.remove("dragging","drop-before","drop-after"));
    });
  });
  queueBody.querySelectorAll("tr[data-id]").forEach(tr=>{
    tr.addEventListener("dragover", e=>{
      e.preventDefault();
      if (!isDragging) return;
      const rect = tr.getBoundingClientRect();
      const before = (e.clientY - rect.top) < rect.height/2;
      queueBody.querySelectorAll("tr").forEach(x=>x.classList.remove("drop-before","drop-after"));
      tr.classList.add(before ? "drop-before" : "drop-after");
    });
    tr.addEventListener("dragleave", ()=> tr.classList.remove("drop-before","drop-after"));
    tr.addEventListener("drop", async e=>{
      e.preventDefault();
      tr.classList.remove("drop-before","drop-after");
      if (!dragIds) return;
      const targetId = parseInt(tr.getAttribute("data-id"),10);
      if (dragIds.includes(targetId)) { isDragging=false; return; }
      const rect = tr.getBoundingClientRect();
      const before = (e.clientY - rect.top) < rect.height/2;
      const ids = currentRows.map(r=>r.id);
      let remaining = ids.filter(x=>!dragIds.includes(x));
      let targetIdx = remaining.indexOf(targetId);
      if (!before) targetIdx++;
      let newOrder = remaining.slice();
      newOrder.splice(targetIdx,0,...dragIds);
      isDragging=false;
      if (ghost && ghost.parentNode) ghost.parentNode.removeChild(ghost);
      ghost=null;
      const localDragIds = dragIds;
      dragIds=null;
      queueBody.querySelectorAll("tr").forEach(x=>x.classList.remove("dragging","drop-before","drop-after"));
      try { await rpc("reorder", {order:newOrder}); } catch(err){ addMsg.textContent=err.message; addMsg.className="msg err"; }
    });
  });
  // глобальный отменщик правым кликом
  document.addEventListener("mousedown", (e)=>{
    if (isDragging && e.button===2) {
      isDragging=false;
      dragIds=null;
      if (ghost && ghost.parentNode) ghost.parentNode.removeChild(ghost);
      ghost=null;
      queueBody.querySelectorAll("tr").forEach(tr=>tr.classList.remove("dragging","drop-before","drop-after"));
    }
  });
  document.addEventListener("contextmenu", e=>{ if (isDragging) e.preventDefault(); });
}

async function pollState(){
  if (isDragging) return;
  try {
    const r = await api("/api/state");
    const j = await r.json();
    versionEl.textContent = j.version ? "v"+j.version : "";
    const c = j.counters||{}; cTotal.textContent=c.total||0; cDone.textContent=c.done||0; cFailed.textContent=c.failed||0;
    isPaused = !!j.paused;
    btnPause.textContent = isPaused ? "▶ Продолжить" : "⏸ Пауза";
    btnPause.title = isPaused ? "Запустить очередь" : "Остановить очередь";
    if (pausedBadge) pausedBadge.textContent = isPaused ? "Очередь остановлена" : "";
    if (doneBadge) {
      const rows = j.rows || [];
      const n = rows.filter(r=>r.state==="ok"||r.state==="skip"||r.state==="error").length;
      doneBadge.textContent = n > 0 ? "(" + n + ")" : "";
      btnClearCompleted.disabled = n === 0;
    }
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
    if (document.hidden || isDragging) return;
    pollState();
  }, 1000);
}
function stopPoll(){ if(pollTimer){ clearInterval(pollTimer); pollTimer=null; } }

loginForm.addEventListener("submit", (e)=>{
  e.preventDefault();
  const t = sanitizeToken(tokenInput.value);
  if (!t) { loginError.textContent="Введите токен"; loginError.classList.remove("hidden"); return; }
  if (/[^\x20-\x7E]/.test(t)) { loginError.textContent="В токене недопустимые символы. Скопируйте 64 hex-символа без пробелов."; loginError.classList.remove("hidden"); return; }
  if (!/^[0-9a-fA-F]{32,256}$/.test(t)) { loginError.textContent="Токен должен быть hex-строкой (как в консоли демона). Лишние символы удалены, проверьте длину."; loginError.classList.remove("hidden"); }
  token = t; localStorage.setItem(LS_TOKEN, token);
  tokenInput.value = t;
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
  try { await rpc(willPause ? "pause" : "resume", {}); } catch(e){ addMsg.textContent=e.message; addMsg.className="msg err"; }
});
btnClearCompleted.addEventListener("click", async ()=>{
  const done = currentRows.filter(r=>r.state==="ok"||r.state==="skip"||r.state==="error");
  if (done.length===0) { addMsg.textContent="Нет завершённых файлов"; addMsg.className="msg"; return; }
  if (!confirm(`Удалить ${done.length} завершённых файлов из списка?`)) return;
  for (const r of done) { try{ await rpc("remove", {id:r.id}); }catch(e){} }
});
btnBatchStop && btnBatchStop.addEventListener("click", async ()=>{
  const ids = [...selectedIds].filter(id=>{
    const r = currentRows.find(x=>x.id===id);
    return r && (r.state==="queued"||r.state==="prep"||r.state==="running");
  });
  if (!ids.length) { addMsg.textContent="Нет активных файлов среди выделенных"; addMsg.className="msg"; return; }
  for (let id of ids) try{ await rpc("cancel-file", {id}); }catch(e){}
  addMsg.textContent="Остановлено: "+ids.length; addMsg.className="msg ok";
});
const btnBatchStart = el("btn-batch-start");
btnBatchStart && btnBatchStart.addEventListener("click", async ()=>{
  const ids = [...selectedIds].filter(id=>{
    const r = currentRows.find(x=>x.id===id);
    return r && (r.state==="ok"||r.state==="skip"||r.state==="error");
  });
  if (!ids.length) { addMsg.textContent="Нет завершённых файлов среди выделенных"; addMsg.className="msg"; return; }
  try {
    const res = await rpc("restart", {ids});
    addMsg.textContent="Запущено снова: "+((res.restarted||[]).length)+" из "+ids.length;
    addMsg.className="msg ok";
  } catch(e){ addMsg.textContent=e.message; addMsg.className="msg err"; }
});
btnBatchDelete && btnBatchDelete.addEventListener("click", async ()=>{
  const ids = [...selectedIds];
  if (!ids.length) return;
  if (!confirm(`Удалить ${ids.length} выделенных файлов?`)) return;
  for (let id of ids) try{ await rpc("remove", {id}); }catch(e){}
  selectedIds.clear();
});
chkAll && chkAll.addEventListener("change", ()=>{
  if (chkAll.checked) currentRows.forEach(r=>selectedIds.add(r.id));
  else selectedIds.clear();
  lastCheckedId=null;
  // обновить без полного ререндера
  queueBody.querySelectorAll("tr").forEach(tr=>{
    const tid = parseInt(tr.getAttribute("data-id"),10);
    const c = tr.querySelector("[data-chk]");
    if (c) c.checked = selectedIds.has(tid);
    tr.classList.toggle("selected", selectedIds.has(tid));
  });
  updateSelectionUI();
});
el("btn-shutdown").addEventListener("click", async ()=>{
  if(!confirm("Выключить демон? Обработка активных файлов завершится, затем демон остановится.")) return;
  try{ await rpc("shutdown", {}); setConn(false,"Демон выключается..."); }catch(e){ addMsg.textContent=e.message; addMsg.className="msg err"; }
});

document.addEventListener("visibilitychange", ()=>{ if(!document.hidden) pollState(); });

(function init(){
  token = sanitizeToken(token);
  if (/[^\x20-\x7E]/.test(token)) token = "";
  try { localStorage.setItem(LS_TOKEN, token); } catch (e) {}
  if (token) { tokenInput.value = token; showApp(); }
  else {
    // Сервер может работать без авторизации (--no-auth): проверяем.
    fetch("/api/state").then(r=>{ if (r.ok) showApp(); else showLogin(); }).catch(()=>showLogin());
  }
})();
