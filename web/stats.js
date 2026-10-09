// ---------------------------------------------------------------------------
// Эффективность кодеков — отдельная страница /stats
// ---------------------------------------------------------------------------
//
// Показывает средний уровень сжатия каждым методом по всему обработанному
// материалу. Совокупность одна — все обработанные треки; разбивки по альбомам
// нет, она ничего не сообщает о кодеке. Вместо неё фильтры по свойствам
// трека: разрядность, частота, каналы и длительность заметно двигают
// эффективность, поэтому это отбор внутри совокупности.
//
// У каждого метода своя карточка. Ось экономии общая, чтобы распределения
// читались друг против друга; высота столбцов нормирована по своему методу —
// сравнивается форма, а абсолютные числа подписаны. Семейство кодека
// (engine.kind из formats/*.json) отражено только фоном карточки.

const LS_TOKEN = "llao_token";
const el = id => document.getElementById(id);

let token = "";
try { token = localStorage.getItem(LS_TOKEN) || ""; } catch (e) {}

function showLogin(msg) {
  el("login").classList.remove("hidden");
  el("stats-page").classList.add("hidden");
  if (msg) {
    const e2 = el("login-error");
    e2.textContent = msg;
    e2.classList.remove("hidden");
  }
}
function showApp() {
  el("login").classList.add("hidden");
  el("stats-page").classList.remove("hidden");
}

// Ответ разбираем сами, а не r.json(): сырой SyntaxError на экране ничего
// не объясняет, а пустой ответ — самая частая поломка на обрыве соединения.
async function jsonOf(r, what) {
  const text = await r.text();
  if (!text || !text.trim())
    throw new Error((what || "ответ") + ": пустой ответ сервера (соединение оборвано или ответ не дошёл)");
  try {
    return JSON.parse(text);
  } catch (e) {
    throw new Error((what || "ответ") + ": не разобран JSON (" + text.length +
      " байт, " + String(e.message || e).slice(0, 60) + ")");
  }
}

// api() отдаёт Response, а не разобранный объект: разбор делает jsonOf() на
// стороне вызова. Если api() разбирает JSON сам, а вызов оборачивает ответ в
// jsonOf() ещё раз, тот получает вместо ответа обычный объект и падает на
// r.text — ровно то, что случилось.
async function api(path) {
  const headers = {};
  if (token) headers["Authorization"] = "Bearer " + token;
  const r = await fetch(path, { headers });
  if (r.status === 401) throw new Error("токен не принят");
  if (!r.ok) throw new Error("HTTP " + r.status);
  return r;
}

// ---------------------------------------------------------------------------
// Панель эффективности кодеков (/api/stats)
// ---------------------------------------------------------------------------
//
// Средний уровень сжатия по каждому методу на всём обработанном материале.
// Фильтры — свойства трека (разрядность, частота, каналы, длительность):
// они заметно двигают эффективность, поэтому это отбор внутри совокупности.
// Разбивки по альбомам нет — она ничего не сообщает о кодеке.
//
// Ось X — методы, ось Y — экономия. Тело свечи идёт от нуля к среднему,
// фитиль — ±σ: полный min..max на большой выборке упирается в единичные
// выбросы и делает шкалу нечитаемой. Под свечой — распределение по бинам.

let statsFilter = { bits: 0, rate: 0, ch: 0, dur: -1 };
let statsData = null;

function statsQuery() {
  const p = new URLSearchParams();
  if (statsFilter.bits) p.set("bits", statsFilter.bits);
  if (statsFilter.rate) p.set("rate", statsFilter.rate);
  if (statsFilter.ch) p.set("ch", statsFilter.ch);
  if (statsFilter.dur >= 0) p.set("dur", statsFilter.dur);
  return p.toString();
}

// Подписи значений граней. Сервер отдаёт числа, а не текст: подписи живут
// здесь, потому что веб по-русски, а /api/stats общий.
const STATS_FACET_LABEL = {
  bits: "Разрядность", sample_rate: "Частота", channels: "Каналы",
  duration: "Длительность", all: "Все треки",
};
function facetValueLabel(facet, v, buckets) {
  if (facet === "bits") return v + " бит";
  if (facet === "sample_rate") return (v / 1000).toFixed(v % 1000 ? 3 : 0).replace(".", ",") + " кГц";
  if (facet === "channels") return v === 1 ? "моно" : v === 2 ? "стерео" : v + " кан.";
  if (facet === "duration") {
    const bs = buckets || [];
    const b = bs.find(x => x.id === v);
    if (!b) return "корзина " + v;
    // Границы приходят в миллисекундах; последняя корзина открытая.
    const fromMin = Math.round(b.from_ms / 60000);
    if (fromMin === 0) return "до 1 мин";
    const next = bs.find(x => x.id === v + 1);
    if (!next) return "от " + fromMin + " мин";
    return fromMin + "\u2013" + Math.round(next.from_ms / 60000) + " мин";
  }
  return String(v);
}

function renderStatsFilters(d) {
  const box = el("stats-filters");
  if (!box) return;
  box.textContent = "";
  const cur = { bits: statsFilter.bits, sample_rate: statsFilter.rate,
                channels: statsFilter.ch, duration: statsFilter.dur };
  for (const f of d.facets || []) {
    const wrap = document.createElement("div");
    wrap.className = "stats-facet";
    const t = document.createElement("span");
    t.className = "stats-facet__title";
    t.textContent = STATS_FACET_LABEL[f.facet] || f.facet;
    wrap.appendChild(t);
    // «Все треки» кодируется нулём у числовых граней и -1 у длительности:
    // там лишний 0 — полноценная корзина «до минуты».
    const allVal = f.facet === "duration" ? -1 : 0;
    const vals = [{ value: allVal, files: d.in_sample, label: STATS_FACET_LABEL.all }];
    for (const v of f.values)
      vals.push({ value: v.value, files: v.files,
                  label: facetValueLabel(f.facet, v.value, d.duration_buckets) });
    for (const v of vals) {
      const b = document.createElement("button");
      b.className = "chip" + (cur[f.facet] === v.value ? " chip--on" : "");
      b.textContent = v.label + " · " + v.files;
      b.title = v.files + " треков";
      b.addEventListener("click", () => {
        statsFilter[f.facet === "bits" ? "bits" : f.facet === "sample_rate" ? "rate"
                     : f.facet === "channels" ? "ch" : "dur"] = v.value;
        loadStats();
      });
      wrap.appendChild(b);
    }
    box.appendChild(wrap);
  }
}

// Ось экономии: слева зона «вырос», дальше 0…100 % от несжатого оригинала.
const SAV_LO = 0;
const SAV_HI = 1.00;

function statsTip(m) {
  const pct = x => (x * 100).toFixed(2).replace(".", ",") + " %";
  return m.name + " (" + m.format + ")\n"
    + "среднее сжатие: " + pct(m.mean) + "\n"
    + "разброс σ: ±" + pct(m.stddev) + "\n"
    + "размах: " + pct(m.min) + " … " + pct(m.max) + "\n"
    + "треков учтено: " + m.considered
    + (m.not_applicable ? ", неприменимо: " + m.not_applicable : "") + "\n"
    + "выиграл: " + m.wins;
}

// Квартиль распределения по бинам. Тело свечи строим по межквартильному
// размаху, а НЕ по ±σ: разброс у этих кодеков около 30 %, и свеча с телом
// «среднее ± σ» выглядит плитой во всю высоту колонки — различить в ней
// ничего нельзя. Квартильный размах — это тот же ящик, что в обычном box plot:
// он компактный, а выбросы честно уходят на фитиль.
function quantileFromHist(m, q) {
  const bins = m.hist.bins, grew = m.hist.grew || 0;
  const total = grew + bins.reduce((a, b) => a + b, 0);
  if (!total) return 0;
  const want = q * total;
  if (want <= grew) return -0.05;
  let acc = grew;
  for (let i = 0; i < bins.length; i++) {
    if (want <= acc + bins[i]) return (i + 0.5) / bins.length;
    acc += bins[i];
  }
  return 1;
}

function statsTipVariant(v) {
  const pct = x => (x * 100).toFixed(2).replace(".", ",") + " %";
  return v.name + " · задание " + v.key + "\n"
    + "среднее сжатие: " + pct(v.mean) + "\n"
    + "разброс σ: ±" + pct(v.stddev) + "\n"
    + "размах: " + pct(v.min) + " … " + pct(v.max) + "\n"
    + "треков: " + v.considered + " · выиграл: " + v.wins;
}

// Рисуем по ЗАДАНИЯМ («формат:вариант»), их десятки, а не по методам: у
// OptimFROG одиннадцать пресетов, у WavPack двадцать три варианта, и среднее по
// кодеку как раз скрывает, почему он проигрывает сам себе.
//
// Две вертикальные части на задачу, обе на оси экономии и обе подписаны:
//
//	сверху свеча — тело от среднего−σ до среднего+σ, тонкий фитиль до полного
//	  размаха, яркая поперечная засечка на среднем;
//	снизу вертикальная гистограмма распределения по 5 %, нарисованная внутри
//	  ширины колонки, поэтому повторяет свечу и стоит под ней.
//
// Всё в одной системе координат: задачи идут по оси X, экономия по Y, поэтому
// сравнить две задачи между собой можно прямо, без чтения подписей.
function renderStatsChart(d) {
  const box = el("stats-chart");
  if (!box) return;
  box.textContent = "";
  const vs = (d.variants || []).filter(v => v.considered > 0);
  const ms = (d.methods || []).filter(m => m.considered > 0);
  const items = vs.length ? vs : ms.map(m => Object.assign({}, m, {
    key: m.format, variant: "", family: m.family, name: m.name,
  }));
  if (!items.length) {
    box.textContent = "Нет данных под этим фильтром.";
    return;
  }

  const NS = "http://www.w3.org/2000/svg";
  // Разметка ровно под окно: viewBox совпадает с размером контейнера в пикселях,
  // поэтому изображение занимает всю доступную область и НИКОГДА не требует
  // полос прокрутки. Раньше был фиксированный viewBox с preserveAspectRatio —
  // он вписывался по меньшей стороне и оставлял половину окна пустой, а при
  // 76 колонках подписи ещё и не помещались.
  const W = Math.max(box.clientWidth || 1200, 640);
  const HH = Math.max(box.clientHeight || 520, 340);
  const ML = 58, MR = 14;
  const COL = (W - ML - MR) / items.length;
  const TOP = 30;                    // полоса названий кодеков
  const AXT = 26;                    // подписи оси экономии
  // Отдельной полосы под гистограммы больше нет: распределение повёрнуто и
  // нанесено вдоль свечи, на ту же ось экономии. Остаётся только место под
  // подписи задач.
  const LABEL_H = Math.round(HH * 0.16);
  const CANDLE_H = Math.max(200, HH - TOP - AXT - LABEL_H);
  const H = HH;

  const MT = TOP + AXT;
  const ySav = v => MT + CANDLE_H * (1 - (v - SAV_LO) / (SAV_HI - SAV_LO));
  const colX = k => ML + COL * k;

  const svg = document.createElementNS(NS, "svg");
  svg.setAttribute("viewBox", "0 0 " + W + " " + H);
  svg.setAttribute("width", W);
  svg.setAttribute("height", H);
  svg.setAttribute("class", "stats-svg");
  svg.setAttribute("role", "img");
  svg.setAttribute("aria-label", "Средний уровень сжатия по задачам");
  const add = (tag, attrs, text) => {
    const e = document.createElementNS(NS, tag);
    for (const k in attrs) e.setAttribute(k, attrs[k]);
    if (text != null) e.textContent = text;
    svg.appendChild(e);
    return e;
  };
  // Подсказка средствами SVG: <title> внутри фигуры показывается браузером
  // без единой строки обработчиков на элемент.
  const tip = (node, text) => {
    const t = document.createElementNS(NS, "title");
    t.textContent = text;
    node.appendChild(t);
  };

  // Полосы семейств и подписи кодеков над группами.
  let i = 0;
  while (i < items.length) {
    let j = i;
    while (j + 1 < items.length && items[j + 1].format === items[i].format) j++;
    if ((items[i].family || "") === "ffmpeg")
      add("rect", { x: colX(i), y: MT, width: COL * (j - i + 1),
                    height: CANDLE_H, class: "stats-band" });
    const gname = items[i].name || items[i].format;
    const gx = colX(i) + COL * (j - i + 1) / 2;
    // Название кодека вписывается в ширину своей группы, иначе соседние
    // наезжают друг на друга.
    const gw = COL * (j - i + 1) - 10;
    let shown = gname;
    const fit = Math.floor(gw / 6.4);
    if (shown.length > fit) shown = shown.slice(0, Math.max(3, fit - 1)) + "\u2026";
    const gt = add("text", { x: gx, y: TOP - 10, class: "stats-group",
                             "text-anchor": "middle" }, shown);
    tip(gt, gname);
    add("line", { x1: colX(i), y1: TOP - 4, x2: colX(i), y2: MT + CANDLE_H,
                  class: "stats-sep" });
    i = j + 1;
  }

  // Сетка по оси экономии.
  for (const v of [0, 0.25, 0.5, 0.75, 1.0]) {
    add("line", { x1: ML, y1: ySav(v), x2: ML + COL * items.length, y2: ySav(v),
                  class: v === 0 ? "stats-zero" : "stats-grid" });
    add("text", { x: ML - 10, y: ySav(v) + 6, class: "stats-axis", "text-anchor": "end" },
        Math.round(v * 100) + "%");
  }
  add("line", { x1: ML, y1: ySav(0), x2: ML, y2: MT + CANDLE_H, class: "stats-axisline" });


  // Шаг подписей: на колонку шириной COL помещается примерно COL/6 названий.
  const labelStep = Math.max(1, Math.ceil(66 / Math.max(COL, 1)));

  items.forEach((m, k) => {
    const cx = colX(k) + COL / 2;
    const half = COL / 2;

    // Левая половина колонки — распределение, повёрнутое на 90°: ось бинов
    // вертикальная и ОБЩАЯ со свечой, поэтому «хвост» распределения сразу
    // виден на той же шкале процентов, а не в отдельной полосе внизу.
    const bins = m.hist.bins;
    let hm = 1;
    for (const b of bins) hm = Math.max(hm, b);
    const hx = cx - half + 1;            // от левого края колонки
    const hwid = half * 0.78;            // максимальная длина столбика
    bins.forEach((c, bi) => {
      if (!c) return;
      const yTop = ySav((bi + 1) * 0.05), yBot = ySav(bi * 0.05);
      const len = hwid * (c / hm);
      if (len < 0.7) return;
      // Столбик занимает меньше половины бина и центрируется в нём: иначе
      // двадцать один бин слипаются в сплошной блок и свеча за ними пропадает.
      const bh = Math.max(1, (yBot - yTop) * 0.45);
      const r = add("rect", { x: hx, y: (yTop + yBot) / 2 - bh / 2, width: len,
                              height: bh, class: "stats-hist" });
      tip(r, m.key + ": " + (bi * 5) + "–" + ((bi + 1) * 5) + " % — " + c + " треков");
    });

    // Правая половина — свеча: тело по межквартильному размаху, фитиль до
    // полного размаха, белая засечка на среднем. Ось та же самая.
    const kx = cx + half * 0.52;
    const q1 = quantileFromHist(m, 0.25), q3 = quantileFromHist(m, 0.75);
    add("line", { x1: kx, y1: ySav(Math.min(SAV_HI, m.max)),
                  x2: kx, y2: ySav(Math.max(SAV_LO, m.min)),
                  class: "stats-wick" });
    const body = add("rect", {
      x: kx - half * 0.30, y: ySav(q3),
      width: Math.max(2, half * 0.60), height: Math.max(3, ySav(q1) - ySav(q3)),
      class: "stats-candle" + (m.mean < 0 ? " stats-candle--neg" : ""),
    });
    tip(body, (m.key && m.variant ? statsTipVariant(m) : statsTip(m))
        + "\nсередина 50 %: " + Math.round(q1 * 100) + "…"
        + Math.round(q3 * 100) + " %");
    add("line", { x1: kx - half * 0.44, y1: ySav(m.mean),
                  x2: kx + half * 0.44, y2: ySav(m.mean), class: "stats-meanline" });
    if (m.max > SAV_HI || m.min < SAV_LO)
      add("path", { d: "M " + (kx + half * 0.44) + " " + ySav(Math.min(SAV_HI, m.max)) +
                    " l 6 -4 v 8 z", class: "stats-clip" });

    // --- подписи
    // Подпись задачи: на 76 колонках все не помещаются, поэтому выводим через
    // шаг, а полное имя и число остаются в подсказке.
    if (k % labelStep === 0) {
      const nm = add("text", {
        x: cx, y: MT + CANDLE_H + 16, class: "stats-name",
        transform: "rotate(-68 " + cx + " " + (MT + CANDLE_H + 16) + ")",
      }, m.variant || m.format);
      tip(nm, (m.key && m.variant ? statsTipVariant(m) : statsTip(m)));
      add("text", { x: cx, y: MT + CANDLE_H + 28, class: "stats-mean",
                    transform: "rotate(-68 " + cx + " " + (MT + CANDLE_H + 28) + ")" },
          (m.mean * 100).toFixed(1) + "%");
    }
  });

  box.appendChild(svg);
  el("stats-sub").textContent =
    "заданий: " + items.length + " в " + new Set(items.map(m => m.format)).size
    + " кодеках · треков в выборке: "
    + d.in_sample + " из " + d.files + " · проценты от несжатого оригинала (WAV с тегами)";
  el("stats-legend").textContent =
    "По оси X — задачи «кодек:вариант», по оси Y — экономия относительно несжатого "
    + "оригинала; обе половины каждой колонки стоят на этой одной оси. Слева "
    + "распределение по 5 %, повёрнутое на 90° (длина столбика — число треков), "
    + "справа свеча: тело по межквартильному размаху, фитиль до полного размаха, "
    + "засечка на среднем. Фон колонок — семейство кодека, ffmpeg выделен.";
}

async function loadStats() {
  try {
    const d = await jsonOf(await api("/api/stats?" + statsQuery()), "сводку кодеков");
    if (!d || !d.methods) return;
    statsData = d;
    renderStatsFilters(d);
    renderStatsChart(d);
  } catch (e) {
    const box = el("stats-chart");
    if (box) box.textContent = "Не удалось загрузить сводку: " + e.message;
  }
}

function openStats() {
  el("stats-overlay").classList.remove("hidden");
  loadStats();
}
function closeStats() {
  el("stats-overlay").classList.add("hidden");
  hideStatsTip();
}
el("btn-stats") && el("btn-stats").addEventListener("click", ()=>{
  el("stats-overlay").classList.contains("hidden") ? openStats() : closeStats();
});
el("btn-stats-close") && el("btn-stats-close").addEventListener("click", closeStats);
el("stats-overlay") && el("stats-overlay").addEventListener("click", e=>{
  if (e.target === el("stats-overlay")) closeStats();
});
document.addEventListener("keydown", e=>{
  if (e.key === "Escape" && !el("stats-overlay").classList.contains("hidden")) closeStats();
});
addEventListener("resize", ()=>{ if (statsData && !el("stats-overlay").classList.contains("hidden")) renderStatsChart(statsData); });


el("login-form").addEventListener("submit", e2 => {
  e2.preventDefault();
  const t = (el("token-input").value || "").trim();
  if (!t) { showLogin("Введите токен"); return; }
  if (!/^[0-9a-fA-F]{32,256}$/.test(t)) {
    showLogin("Токен должен быть hex-строкой (как в консоли демона).");
    return;
  }
  token = t;
  try { localStorage.setItem(LS_TOKEN, token); } catch (e3) {}
  showApp();
  loadStats();
});

el("btn-stats-refresh").addEventListener("click", () => loadStats());
el("btn-stats-shutdown").addEventListener("click", async () => {
  if (!confirm("Выключить демон? Обработка активных файлов завершится, затем демон остановится.")) return;
  try {
    await fetch("/rpc", { method: "POST",
      headers: { "Content-Type": "application/json", Authorization: "Bearer " + token },
      body: JSON.stringify({ cmd: "shutdown", args: {} }) });
  } catch (e) {}
});

addEventListener("resize", () => { if (statsData) renderStatsChart(statsData); });

// Автообновление, пока вкладка открыта: цифры на странице эффективности
// должны догонять прогон, но не чаще, чем раз в полминуты.
setInterval(() => { if (token && !el("stats-page").classList.contains("hidden")) loadStats(); }, 30000);

(function init(){
  el("token-input").value = token;
  if (token) { showApp(); loadStats(); return; }
  fetch("/api/stats").then(r => { if (r.ok) { showApp(); loadStats(); } else showLogin(); })
    .catch(() => showLogin());
})();
