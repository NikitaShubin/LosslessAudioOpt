"use strict";
const LS_TOKEN = "llao_token";
let token = localStorage.getItem(LS_TOKEN) || "";
let pollTimer = null;
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
const addForm = el("add-form"), addPath = el("add-path"), addTarget = el("add-target");
const addRestore = el("add-restore"), addMsg = el("add-msg");
const queueBody = el("queue-body"), connStatus = el("conn-status");
const opMsgEl = el("op-msg");
function opmsg(text, cls) {
  if (!opMsgEl) return;
  opMsgEl.textContent = text;
  opMsgEl.className = "msg" + (cls ? " " + cls : "");
  opMsgEl.style.visibility = text ? "visible" : "hidden";
}
const btnStop = el("btn-stop"), btnResume = el("btn-resume"), btnClearCompleted = el("btn-clear-completed");
const chkAll = el("chk-all");
const btnBatchStop = el("btn-batch-stop"), btnBatchStart = el("btn-batch-start"), btnBatchDelete = el("btn-batch-delete");
const btnBatchTop = el("btn-batch-top"), btnBatchUp = el("btn-batch-up"), btnBatchDown = el("btn-batch-down"), btnBatchBottom = el("btn-batch-bottom");
const btnStatusbar = el("btn-statusbar"), statusbarFill = el("statusbar-fill"),
      statusbarLabel = el("statusbar-label"), tableWrap = el("table-wrap");
const btnSort = el("btn-sort");
let autoScrollOn = false;
let autoScrollBoost = false;
let lastAutoGoal = -1;
const POLL_PERIOD_MS = 1000;                 // фиксированный период полла (setInterval)

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
function excludedDots(excluded) {
  if (!excluded || !excluded.length) return "";
  return excluded.map(f=>`<span class="task-dot task-excluded" title="${esc(f)} — вне характеристик формата (caps)"></span>`).join("");
}
function esc(s){ return String(s).replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;").replace(/"/g,"&quot;"); }

function taskRunningCount(r){
  return (r.tasks||[]).filter(s=>s==="running").length;
}
function maybeAutoScroll(){
  if (!tableWrap || !btnStatusbar || !autoScrollOn) return;
  // Активность файла — число задач, обрабатываемых прямо сейчас (running).
  // Строки без идущих процессов в центр масс не включаются.
  let act = currentRows.filter(r=>r.state==="prep"||r.state==="running");
  let byTask = act.filter(r=>taskRunningCount(r) > 0);
  if (byTask.length) act = byTask;
  if (!act.length) return;
  // Вес строки — число одновременно идущих процессов.
  const weight = new Map();
  for (const r of act) weight.set(r.id, Math.max(1, taskRunningCount(r)));
  let wy = 0, wsum = 0;
  tableWrap.querySelectorAll("tbody tr[data-id]").forEach(tr=>{
    const id = parseInt(tr.getAttribute("data-id"),10);
    if (!weight.has(id)) return;
    const w = weight.get(id);
    wy += w * (tr.offsetTop + tr.offsetHeight/2);
    wsum += w;
  });
  if (wsum === 0) return;
  const centerMass = wy/wsum;
  const max = tableWrap.scrollHeight - tableWrap.clientHeight;
  const goal = clampTarget(centerMass - tableWrap.clientHeight/2, max);

  if (autoScrollBoost) {
    // Первичная доводка при включении — одно плавное движение к текущему центру.
    autoScrollBoost = false;
    lastAutoGoal = goal;
    tableWrap.scrollTo({top: goal, behavior: "smooth"});
    return;
  }
  // Вниз догоняем при любом заметном уходе центра (восходящий прогон новых
  // файлов), вверх — только когда активные строки вышли из видимой зоны:
  // это случается при резких действиях (clear-done, перемещение в начало),
  // иначе мелкие колебания центра дёргали бы скролл сам по себе.
  const scrollTop = tableWrap.scrollTop;
  const down = goal > scrollTop + 8;
  const up = scrollTop - goal > tableWrap.clientHeight / 2;
  if ((down || up) && Math.abs(goal - lastAutoGoal) > 8) {
    lastAutoGoal = goal;
    tableWrap.scrollTo({top: goal, behavior: "smooth"});
  }
}
function clampTarget(v, max){ return Math.max(0, Math.min(v, max)); }

// Глобальная доля выполненных «задач» по всей очереди. Задача — один вариант
// кодирования (optimize) или восстановление (restore); плюс у каждой строки
// перед кодированием идёт распаковка в WAV — она тоже отдельная задача в доле.
// У строк с построенным планом точное число задач (tasks.length). У строк до
// плана (queued/prep) — предварительная оценка: максимум вариантов сессии из
// formats/*.json (через /api/formats, `variants`), для restore — всегда 1.
// Оценка не засчитывает выполненные задачи: в done идёт только реально
// сделанное (ok/failed + уже прошедшая в этой фазе распаковка при известном
// плане). После построения плана строка переходит на точный подсчёт.
let formatsMaxTasks = 0;   // сумма encode.variants по включённым форматам (по JSON)
let formatsLoaded = false;
async function loadFormats(){
  try {
    const r = await api("/api/formats");
    const j = await r.json();
    const fmts = j.formats || [];
    formatsMaxTasks = fmts.reduce((s,f)=> s + (Number.isFinite(f.variants) ? f.variants : 0), 0);
  } catch(e){ /* 401 или сеть — оставим 0, строки без плана не в счёте */ }
  updateStatusbar(currentRows);
}
function updateStatusbar(rows){
  if (!btnStatusbar) return;
  let total = 0, done = 0;
  for (const r of rows || []) {
    const tasks = r.tasks || [];
    if (tasks.length) {
      // План построен — точный счёт: варианты + уже прошедшая распаковка.
      total += tasks.length + 1;
      done  += tasks.filter(t=>t==="ok"||t==="failed").length + 1;
    } else {
      // План ещё не построен — строка в объёме по оценке (максимум вариантов
      // сессии из formats/*.json; restore — всегда один целевой формат), в
      // done ничего не засчитываем — ни задачи, ни распаковка не сделаны.
      const est = r.mode === "restore" ? 1 : formatsMaxTasks;
      if (est > 0) total += est + 1;
    }
  }
  if (total === 0) {
    // Объём задач неизвестен (пустая очередь / формат не загружен) —
    // статусбар бесполезен.
    btnStatusbar.classList.add("hidden");
    return;
  }
  btnStatusbar.classList.remove("hidden");
  const pct = Math.round(done/total*100);
  if (statusbarFill) statusbarFill.style.width = pct + "%";
  if (statusbarLabel) {
    statusbarLabel.textContent = `${pct}% (${done}/${total})`;
    btnStatusbar.title = "Следить за активностью списка";
  }
}
function setAutoScroll(on){
  autoScrollOn = on;
  if (!on) lastAutoGoal = -1;
  if (btnStatusbar) btnStatusbar.classList.toggle("on", on);
  try { localStorage.setItem("llao_autoscroll", on ? "1" : "0"); } catch(e){}
  if (on) { autoScrollBoost = true; maybeAutoScroll(); }
}

async function moveBlock(block, dir){
  if (!block.length) return;
  const ids = currentRows.map(r=>r.id);
  const blockSet = new Set(block);
  let remaining = ids.filter(x=>!blockSet.has(x));
  let newOrder;
  if (dir==="up"||dir==="top") {
    const firstPos = Math.min(...block.map(x=>ids.indexOf(x)));
    let beforeId = null;
    for (let i=firstPos-1;i>=0;i--) if (!blockSet.has(ids[i])) { beforeId = ids[i]; break; }
    const insertAt = dir==="top" ? 0 : (beforeId===null ? 0 : remaining.indexOf(beforeId)+1);
    newOrder = remaining.slice();
    newOrder.splice(insertAt,0,...block);
  } else {
    const lastPos = Math.max(...block.map(x=>ids.indexOf(x)));
    let afterId=null;
    for (let i=lastPos+1;i<ids.length;i++) if (!blockSet.has(ids[i])) { afterId=ids[i]; break; }
    const insertAt = dir==="bottom" ? remaining.length : (afterId===null ? remaining.length : remaining.indexOf(afterId)+1);
    newOrder = remaining.slice();
    newOrder.splice(insertAt,0,...block);
  }
  try { await rpc("reorder", {order:newOrder}); } catch(e){ opmsg(e.message, "err"); }
}

function progressBar(r){
  const tasks = r.tasks || [];
  if (r.state==="queued"||r.state==="prep"||r.state==="stopped"||r.state==="error") {
    return `<span class="muted">—</span>`;
  }
  if (r.state==="ok") {
    const save = (r.pct!==undefined && r.pct!==null) ? r.pct.toFixed(1) : "0.0";
    // Оптимизация — процент выигрыша (зелёный), восстановление — размер может
    // вырасти (серый/нейтральный: это норма режима, а не ошибка).
    const cls = r.mode==="restore" ? "pct-val prog-restore" : "pct-val prog-ok";
    return `<span class="${cls}">${save}%</span>`;
  }
  const done = tasks.filter(t=>t==="ok"||t==="failed").length;
  const total = tasks.length;
  const pct = total ? (done/total*100) : 0;
  return `<span class="pct-val prog-run">${pct.toFixed(0)}%</span>`;
}

// Путь, показываемый в столбце «Файл». Когда результат лежит в целевой папке —
// выводим его относительно target_dir (без длинного префикса), иначе полный
// путь/метка строки. Граница сегмента проверяется, чтобы префикс-совпадение
// (напр. /d/tmp/2 → /d/tmp/22) не давало ложного «относительного» пути.
function displayPath(r){
  let p = (r.out_path && r.out_path!=="") ? r.out_path : r.label;
  const t = r.target_dir || "";
  if (t && p.startsWith(t) && p.length > t.length) {
    const ch = p.charAt(t.length);
    if (ch === "/" || ch === "\\") {
      const rel = p.slice(t.length + 1);
      if (rel) p = rel;
    }
  }
  return p;
}

function updateSelectionUI(){
  if (!chkAll) return;
  if (!currentRows.length) { chkAll.checked=false; chkAll.indeterminate=false; }
  else {
    const all = currentRows.every(r=>selectedIds.has(r.id));
    const some = currentRows.some(r=>selectedIds.has(r.id));
    chkAll.checked = all;
    chkAll.indeterminate = !all && some;
  }
  updateBatchButtons();
}

function updateBatchButtons(){
  if (!btnBatchStop || !btnBatchStart || !btnBatchDelete) return;
  const selRows = currentRows.filter(r=>selectedIds.has(r.id));
  const hasActiveSel = selRows.some(r=>r.state==="queued"||r.state==="prep"||r.state==="running");
  const hasRestartSel = selRows.some(r=>r.state==="stopped"||r.state==="error");
  const hasAnySel = selRows.length>0;
  btnBatchStop.disabled = !hasActiveSel;
  btnBatchStart.disabled = !hasRestartSel;
  btnBatchDelete.disabled = !hasAnySel;
  btnBatchStop.title = hasActiveSel ? "Остановить выделенные файлы" : "Нет активных файлов среди выделенных";
  btnBatchStart.title = hasRestartSel ? "Запустить выделенные" : "Нет файлов для запуска среди выделенных";
  btnBatchDelete.title = hasAnySel ? "Удалить выделенные файлы" : "Нет выделенных файлов";
  const n = currentRows.length;
  let firstSel = -1, lastSel = -1;
  for (let i=0;i<n;i++) {
    if (selectedIds.has(currentRows[i].id)) { if (firstSel<0) firstSel=i; lastSel=i; }
  }
  const canUp = firstSel>=0 && currentRows.slice(0,firstSel).some(r=>!selectedIds.has(r.id));
  const canDown = lastSel>=0 && currentRows.slice(lastSel+1).some(r=>!selectedIds.has(r.id));
  if (btnBatchTop && btnBatchUp && btnBatchDown && btnBatchBottom) {
    btnBatchTop.disabled = btnBatchUp.disabled = !canUp;
    btnBatchDown.disabled = btnBatchBottom.disabled = !canDown;
    btnBatchTop.title = btnBatchUp.title = canUp ? "Переместить выделенные выше" : "Переместить выделенные нельзя — нет места";
    btnBatchDown.title = btnBatchBottom.title = canDown ? "Переместить выделенные ниже" : "Переместить выделенные нельзя — нет места";
  }
}

function renderQueue(rows){
  if (isDragging) return;
  currentRows = rows || [];
  for (let id of [...selectedIds]) if (!currentRows.find(r=>r.id===id)) selectedIds.delete(id);
  if (!rows || rows.length===0) {
    queueBody.innerHTML = `<tr><td colspan="7" class="empty">Очередь пуста</td></tr>`;
    updateSelectionUI();
    return;
  }
  let html = "";
  for (let i=0;i<rows.length;i++){
    const r = rows[i];
    const pos = i+1;
    const bar = progressBar(r);
    const infos = r.task_infos || [];
    const tasks = (r.tasks||[]).map((st,idx)=>taskDot(st,idx, infos[idx])).join("") + excludedDots(r.excluded);
    const canUp = i>0, canDown = i<rows.length-1;
    let actions = `<span class="action-btns">`;
    if (r.state==="ok") {
      actions += `<button data-clear="${r.id}" class="icon-btn" title="Удалить завершённый файл из списка">🧹</button>`;
    } else if (r.state==="queued"||r.state==="prep"||r.state==="running") {
      actions += `<button data-stop="${r.id}" class="icon-btn" title="Остановить">⏹</button>`;
    } else {
      actions += `<button data-restart="${r.id}" class="icon-btn" title="Запустить">▶</button>`;
    }
    actions += `<button data-move="top" data-id="${r.id}" class="icon-btn" title="В начало" ${canUp?"":"disabled"}>⇤</button>`;
    actions += `<button data-move="up" data-id="${r.id}" class="icon-btn" title="Вверх" ${canUp?"":"disabled"}>↑</button>`;
    actions += `<button data-move="down" data-id="${r.id}" class="icon-btn" title="Вниз" ${canDown?"":"disabled"}>↓</button>`;
    actions += `<button data-move="bottom" data-id="${r.id}" class="icon-btn" title="В конец" ${canDown?"":"disabled"}>⇥</button>`;
    actions += `<button data-remove="${r.id}" class="icon-btn danger" title="Удалить">🗑</button>`;
    actions += `</span>`;
    const handle = `<span class="drag-handle" draggable="true" data-drag="${r.id}" title="Перетащите">≡</span>`;
    const chk = `<input type="checkbox" data-chk="${r.id}" ${selectedIds.has(r.id)?"checked":""}>`;
    const selClass = selectedIds.has(r.id) ? "selected" : "";
    // Статус строки задаётся фоном через класс tr.st-<state>; отдельного
    // столбца нет. clear-done и кнопки перезапуска остаются по оси действий.
    const badge = r.mode==="restore" ? `<span class="badge badge-restore" title="Восстановление">restore</span>` : "";
    const shown = displayPath(r);
    const exitErr = (r.last_error && (r.state==="stopped"||r.state==="error"))
      ? `<span class="warn" title="${esc(r.last_error)}">⚠</span> ` : "";
    html += `<tr data-id="${r.id}" class="${selClass} st-${r.state}"><td>${chk}</td><td>${handle}</td><td title="#${r.id}">${pos}</td><td>${badge}${exitErr}${esc(shown)}</td><td class="prog">${bar}</td><td><span class="tasks">${tasks}</span></td><td>${actions}</td></tr>`;
  }
  queueBody.innerHTML = html;
  updateSelectionUI();
  queueBody.querySelectorAll("[data-stop]").forEach(b=>{
    b.addEventListener("click", async ()=>{
      const id = parseInt(b.getAttribute("data-stop"),10);
      try { await rpc("cancel-file", {id}); } catch(e){ opmsg(e.message, "err"); }
    });
  });
  queueBody.querySelectorAll("[data-remove]").forEach(b=>{
    b.addEventListener("click", async ()=>{
      const id = parseInt(b.getAttribute("data-remove"),10);
      if (!confirm(`Удалить файл #${id} из очереди? Если он обрабатывается, обработка будет прервана.`)) return;
      try { await rpc("remove", {id}); selectedIds.delete(id); } catch(e){ opmsg(e.message, "err"); }
    });
  });
  queueBody.querySelectorAll("[data-clear]").forEach(b=>{
    b.addEventListener("click", async ()=>{
      const id = parseInt(b.getAttribute("data-clear"),10);
      const r = currentRows.find(x=>x.id===id);
      if (!r || r.state!=="ok") return;
      try { await rpc("remove", {id}); selectedIds.delete(id); } catch(e){ opmsg(e.message, "err"); }
    });
  });
  queueBody.querySelectorAll("[data-restart]").forEach(b=>{
    b.addEventListener("click", async ()=>{
      const id = parseInt(b.getAttribute("data-restart"),10);
      try {
        await rpc("restart", {ids:[id]});
      } catch(e){ opmsg(e.message, "err"); }
    });
  });
  queueBody.querySelectorAll("[data-move]").forEach(b=>{
    b.addEventListener("click", async ()=>{
      const id = parseInt(b.getAttribute("data-id"),10);
      const dir = b.getAttribute("data-move");
      const block = selectedIds.has(id) ? currentRows.filter(r=>selectedIds.has(r.id)).map(r=>r.id) : [id];
      await moveBlock(block, dir);
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
      try { await rpc("reorder", {order:newOrder}); } catch(err){ opmsg(err.message, "err"); }
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
  updateStatusbar(currentRows);
  maybeAutoScroll();
}

async function pollState(){
  if (isDragging) return;
  try {
    const r = await api("/api/state");
    const j = await r.json();
    versionEl.textContent = j.version ? "v"+j.version : "";
    const c = j.counters||{}; cTotal.textContent=c.total||0; cDone.textContent=c.done||0; cFailed.textContent=c.failed||0;
    isPaused = !!j.paused;
    const rows = j.rows || [];
    const hasActive = rows.some(r=>r.state==="queued"||r.state==="prep"||r.state==="running");
    const hasStopped = rows.some(r=>r.state==="stopped");
    btnStop.disabled = !hasActive;
    btnResume.disabled = !hasStopped;
    btnStop.title = hasActive ? "Остановить активные файлы" : "Нет активных файлов";
    btnResume.title = hasStopped ? "Запустить остановленные файлы" : "Нет остановленных файлов";
    const okCount = (j.rows||[]).filter(r=>r.state==="ok").length;
    btnClearCompleted.disabled = okCount === 0;
    btnClearCompleted.title = okCount > 0
      ? `Удалить только успешно завершённые (ok) — ${okCount}`
      : "Удалить только успешно завершённые (ok)";
    renderQueue(j.rows);
    maybeAutoScroll();
    // Оценка максимума задач для строк без плана грузится один раз из
    // /api/formats (числа вариантов берутся из formats/*.json, не из кода).
    if (!formatsLoaded) { formatsLoaded = true; loadFormats(); }
    setConn(true, isPaused ? "Пауза" : "Подключено");
    // Демон без авторизации (--no-auth): «Выйти» не нужен — токена нет.
    const btnLogout = el("btn-logout");
    if (btnLogout) btnLogout.classList.toggle("hidden", !!j.no_auth);
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

btnStatusbar && btnStatusbar.addEventListener("click", ()=> setAutoScroll(!autoScrollOn));
// Нижнее уведомление — клик по нему скрывает (оно висит без таймаута).
opMsgEl && opMsgEl.addEventListener("click", ()=> opmsg(""));
btnSort && btnSort.addEventListener("click", async ()=>{
  if (!currentRows.length) return;
  if (!confirm(`Сортировать очередь по полному пути (регистрозависимо)?`)) return;
  try {
    const res = await rpc("sort", {});
    opmsg("Отсортировано файлов: "+(res.sorted||0), "ok");
  } catch(e){ opmsg(e.message, "err"); }
});
if (tableWrap) {
  // Вмешательством человека считаем события ввода (колесо/тач/клавиатура/
  // скроллбар), а `scroll` не слушаем вовсе — иначе собственная прокрутка
  // браузерной анимацией распознавалась бы как ручная и гасила бы себя.
  tableWrap.addEventListener("wheel", ()=>{ if (autoScrollOn) setAutoScroll(false); }, {passive:true});
  tableWrap.addEventListener("touchstart", ()=>{ if (autoScrollOn) setAutoScroll(false); }, {passive:true});
  tableWrap.addEventListener("keydown", (e)=>{
    if (!autoScrollOn) return;
    if (!/^(ArrowUp|ArrowDown|PageUp|PageDown|Home|End|Space)$/.test(e.key)) return;
    setAutoScroll(false);
  });
  tableWrap.addEventListener("pointerdown", (e)=>{
    if (!autoScrollOn) return;
    // Клики по строкам (чекбокс, кнопки) автоскролл не трогаем; «вмешательство»
    // — это drag по скроллбару, когда событие приходит на сам контейнер.
    if (e.target !== tableWrap) return;
    setAutoScroll(false);
  });
}
// Изменение размеров окна/контейнера: желаемое положение (центр массы активных
// строк) не изменилось, но scrollTop, удерживающий его в кадре, зависит от
// видимой высоты — пересчитываем цель и без дедупа досрочиваем к ней.
let autoScrollResizeTimer = 0;
function onAutoScrollResize(){
  if (!autoScrollOn) return;
  if (autoScrollResizeTimer) return;  // уже запланирована доводка
  autoScrollResizeTimer = setTimeout(()=>{
    autoScrollResizeTimer = 0;
    autoScrollBoost = true;
    maybeAutoScroll();
  }, 100);
}
if (tableWrap) {
  // ResizeObserver отслеживает видимую высоту списка (меняется при ресайзе окна
  // и при развороте на весь экран); scrollHeight от добавления строк не влияет.
  if (typeof ResizeObserver !== "undefined") {
    new ResizeObserver(()=>onAutoScrollResize()).observe(tableWrap);
  } else {
    window.addEventListener("resize", onAutoScrollResize);
  }
}
(function initAutoScroll(){
  if (!btnStatusbar) return;
  let v = "0";
  try { v = localStorage.getItem("llao_autoscroll") || "0"; } catch(e){}
  setAutoScroll(v === "1");
})();

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
  const t = addTarget.value.trim();
  const isRestore = !!addRestore.checked;
  addMsg.textContent="Отправка..."; addMsg.className="msg";
  try {
    const args = {paths:[p], mode: isRestore ? "restore" : "optimize"};
    if (t) args.target_dir = t;
    const res = await rpc("add", args);
    const added = (res.added||[]).length, rej = (res.rejected||[]).length;
    let msg = `Добавлено: ${added}`; if(rej) msg+=`, отклонено: ${rej} (${(res.rejected[0]||{}).reason||""})`;
    addMsg.textContent=msg; addMsg.className="msg ok";
    if (added) { addPath.value=""; closeAddModal(); }
  } catch(err){ addMsg.textContent=err.message; addMsg.className="msg err"; }
});
btnStop.addEventListener("click", async ()=>{
  const hasRunning = currentRows.some(r=>r.state==="queued"||r.state==="prep"||r.state==="running");
  if (!hasRunning) {
    try{ await rpc("pause", {}); }catch(e){ return opmsg(e.message, "err"); }
    opmsg("Нет активных файлов — очередь на паузе", "ok");
    return;
  }
  if (!confirm("Остановить все активные файлы? Их процессы будут прерваны, файлы перейдут в состояние «остановлен».")) return;
  // cancel-all останавливает все активные файлы на стороне демона (список id
  // не передаём: состояние вкладки может отставать от очереди) и ставит паузу.
  try {
    const res = await rpc("cancel-all", {});
    opmsg("Остановлено активных: "+(res.cancelled||0)+". Очередь на паузе.", "ok");
  } catch(e){ opmsg(e.message, "err"); }
});
btnResume.addEventListener("click", async ()=>{
  const ids = currentRows.filter(r=>r.state==="stopped"||r.state==="error").map(r=>r.id);
  try{ await rpc("resume", {}); }catch(e){}
  if (ids.length) {
    try {
      const res = await rpc("restart", {ids});
opmsg("Запущено: "+((res.restarted||[]).length)+" из "+ids.length, "ok");
    } catch(e){ opmsg(e.message, "err"); }
  } else {
    opmsg("Очередь запущена", "ok");
  }
});
btnClearCompleted.addEventListener("click", async ()=>{
  const done = currentRows.filter(r=>r.state==="ok");
  if (done.length===0) { opmsg("Нет успешно завершённых файлов", ""); return; }
  if (!confirm(`Удалить ${done.length} успешно завершённых файлов из списка?`)) return;
  try {
    const res = await rpc("clear-done", {});
    opmsg("Удалено завершённых: "+(res.removed||0), "ok");
  } catch(e){ opmsg(e.message, "err"); }
});
btnBatchStop.addEventListener("click", async ()=>{
  const ids = [...selectedIds].filter(id=>{
    const r = currentRows.find(x=>x.id===id);
    return r && (r.state==="queued"||r.state==="prep"||r.state==="running");
  });
  if (!ids.length) { opmsg("Нет активных файлов среди выделенных", ""); return; }
  try {
    const res = await rpc("bulk-cancel", {ids});
    opmsg("Остановлено: "+(res.cancelled||0)+" из "+ids.length, "ok");
  } catch(e){ opmsg(e.message, "err"); }
});
btnBatchStart.addEventListener("click", async ()=>{
  // Перезапускаем только неактивные (stopped/error): активные уже работают,
  // их router restart остановил бы и ждал до 20с на каждый.
  const ids = [...selectedIds].filter(id=>{
    const r = currentRows.find(x=>x.id===id);
    return r && (r.state==="stopped"||r.state==="error");
  });
  if (!ids.length) { opmsg("Нет файлов для запуска среди выделенных", ""); return; }
  opmsg("Запуск выделенных...", "");
  try {
    const res = await rpc("restart", {ids});
    opmsg("Запущено: "+((res.restarted||[]).length)+" из "+ids.length, "ok");
  } catch(e){ opmsg(e.message, "err"); }
});
btnBatchDelete.addEventListener("click", async ()=>{
  const ids = [...selectedIds];
  if (!ids.length) return;
  if (!confirm(`Удалить ${ids.length} выделенных файлов? Если какие-то обрабатываются, обработка будет прервана.`)) return;
  try { await rpc("bulk-remove", {ids}); } catch(e){ opmsg(e.message, "err"); }
  selectedIds.clear();
});
const headMove = (btn, dir)=>{
  btn && btn.addEventListener("click", async ()=>{
    const block = currentRows.filter(r=>selectedIds.has(r.id)).map(r=>r.id);
    if (!block.length) { opmsg("Ничего не выбрано — отметьте файлы галками", ""); return; }
    await moveBlock(block, dir);
  });
};
headMove(btnBatchTop, "top");
headMove(btnBatchUp, "up");
headMove(btnBatchDown, "down");
headMove(btnBatchBottom, "bottom");
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
const addModal = el("add-modal"), addCancel = el("add-cancel"), btnAdd = el("btn-add");
function openAddModal(){
  addMsg.textContent=""; addMsg.className="msg";
  addModal.classList.remove("hidden");
  addPath.focus();
}
function closeAddModal(){ if (addModal) addModal.classList.add("hidden"); }
btnAdd && btnAdd.addEventListener("click", openAddModal);
addCancel && addCancel.addEventListener("click", closeAddModal);
addModal && addModal.addEventListener("click", (e)=>{ if (e.target === addModal) closeAddModal(); });
document.addEventListener("keydown", (e)=>{
  if (e.key === "Escape" && addModal && !addModal.classList.contains("hidden"))
    closeAddModal();
});
el("btn-shutdown").addEventListener("click", async ()=>{
  if(!confirm("Выключить демон? Обработка активных файлов завершится, затем демон остановится.")) return;
  try{ await rpc("shutdown", {}); setConn(false,"Демон выключается..."); }catch(e){ opmsg(e.message, "err"); }
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
