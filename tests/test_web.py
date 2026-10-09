#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Тесты веб-интерфейса (`web/app.js`) на уровне исходного текста.

Полноценного браузерного прогона здесь нет: node в среде не бывает, а веб-UI
впаивается в бинарник сборкой (`tools/embed_assets.py`). Поэтому проверяются
свойства, которые ломаются молча и стоят дорого: вызовы, обязательные на каждом
пути, и отсутствие ранних выходов, мимо которых эти вызовы не доходят.

Конкретный класс регрессий, который закрывает W2: общий статусбар
(`updateStatusbar`) пересчитывается из строк на клиенте и раньше вызывался
только в конце `renderQueue()`. Ветка пустой очереди выходит раньше, и после
удаления последних строк бар оставался со старыми числами — лечилось только F5
(F5 звал `updateStatusbar` из `loadFormats`, то есть спасал случайный третий
путь, а не правильный). Теперь вызов стоит до ранних выходов.

Покрытие:
    W1  renderQueue обновляет статусбар до раннего выхода (не в хвосте)
    W2  у renderQueue нет раннего выхода, мимо которого проходят вызовы
    W3  удаление строк дёргает pollState сразу, а не ждёт ближайшего полла
    W4  вшитые ассеты соответствуют web/ (иначе правка JS не доедет до бинарника)
    W5  опрос идёт дельтами /api/events, полный /api/state — только по resync
    W6  разбор ответа даёт внятную ошибку, а не сырой SyntaxError

Запуск:
    python3 tests/test_web.py
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "web", "app.js")
EMBED = os.path.join(ROOT, "src", "web_assets_data.cpp")
WEB_DIR = os.path.join(ROOT, "web")

RESULTS = []


def app_src():
    with open(APP, encoding="utf-8") as f:
        return f.read()


def func_body(src, name):
    """Текст функции верхнего уровня по имени: от объявления до закрывающей
    скобки на нулевом отступе.

    Учитывает и обычное `function name(`, и `async function name(`: асинхронных
    функций в интерфейсе большинство, и искать только по первому виду означало
    бы молча не найти половину проверяемых мест.
    """
    m = re.search(r"^(?:async\s+)?function\s+%s\s*\(" % re.escape(name), src, re.M)
    assert m, "функция %s не найдена" % name
    start = m.start()
    end_m = re.compile(r"^\}", re.M)
    m2 = end_m.search(src, start)
    assert m2, "не найден конец функции %s" % name
    return src[start:m2.start()]


def scenario(name, fn):
    try:
        fn()
        RESULTS.append((name, True, ""))
    except AssertionError as exc:
        RESULTS.append((name, False, str(exc)))
    except Exception as exc:  # noqa: BLE001 — в тесте это просто «ошибка сценария»
        RESULTS.append((name, False, "%s: %s" % (type(exc).__name__, exc)))


def w1_statusbar_before_early_return():
    """Статусбар пересчитывается до раннего выхода, а не в хвосте функции."""
    src = app_src()
    body = func_body(src, "renderQueue")
    call = body.find("updateStatusbar(")
    assert call >= 0, "renderQueue не обновляет статусбар вовсе:\n%s" % body
    empty = body.find("rows.length===0")
    assert empty >= 0, "в renderQueue нет ветки пустой очереди:\n%s" % body
    assert call < empty, (
        "вызов updateStatusbar идёт после ветки пустой очереди — на этом пути бар "
        "останется со старыми числами:\n%s" % body)


def w2_no_early_return_skips_calls():
    """Между началом функции и вызовом статусного бара нет раннего выхода.

    Исключение — guard `if (isDragging) return;` в самом начале: во время
    перетаскивания перерисовка замирает целиком (полл тоже пропускается), и бар
    замирает вместе с таблицей. Это осознанная заморозка, а не пропущенный
    вызов; любой другой `return` до обновления — уже дефект.
    """
    src = app_src()
    body = func_body(src, "renderQueue")
    call = body.find("updateStatusbar(")
    assert call >= 0, "renderQueue не обновляет статусбар:\n%s" % body
    head = body[:call]
    stray = [ln.strip() for ln in head.splitlines()
             if re.match(r"^\s*(if\s*\(.*\)\s*)?return\b", ln)
             and "isDragging" not in ln]
    assert not stray, (
        "в renderQueue есть ранний выход до обновления статусбара: %r — "
        "эта ветка оставит бар со старыми числами" % stray)


def w3_removal_refreshes_immediately():
    """После удаления строк состояние опрашивается сразу, не дожидаясь полла."""
    src = app_src()
    for call in ('rpc("remove"', 'rpc("clear-done"', 'rpc("bulk-remove"'):
        assert call in src, "нет обработчика %s" % call
    # Для каждого вызова удаления после catch(...) должен идти pollState().
    for m in re.finditer(r'rpc\("(remove|clear-done|bulk-remove)"', src):
        tail = src[m.end():m.end() + 400]
        # граница обработчика — следующая функция верхнего уровня
        nxt = re.search(r"^(async )?function |^\}\)", tail, re.M)
        if nxt:
            tail = tail[:nxt.start()]
        assert "pollState()" in tail, (
            "после %s нет мгновенного pollState() — интерфейс будет показывать "
            "старые значения до ближайшего полла (до секунды)" % m.group(0))


def w4_embedded_assets_in_sync():
    """Вшитые ассеты соответствуют web/: иначе правка JS не попадёт в бинарник."""
    if not os.path.exists(EMBED):
        return "skipped: src/web_assets_data.cpp нет (собирается make-ом)"
    src = app_src()
    # В сгенерированном файле app.js лежит как байты в zip-контейнере, поэтому
    # сравниваем не текст, а сам факт перегенерации: если app.js новее вшитого
    # ассета, значит про make забыли и правка в бинарник не попала.
    newer = os.path.getmtime(APP) > os.path.getmtime(EMBED) + 1
    assert not newer, (
        "web/app.js новее src/web_assets_data.cpp — про make (или "
        "tools/embed_assets.py) забыли, бинарник соберётся со старым интерфейсом")
    assert "function renderQueue" in src, "не найдено объявление renderQueue"


def w5_polling_uses_events_deltas():
    """Раз в секунду UI не должен тянуть полное состояние.

    На большой библиотеке /api/state — это megabytes (task_infos на каждую
    строку), и по узкому каналу ответ либо не доходит, либо приходит
    обрезанным: страница показывает «список пуст», хотя демон отвечал секунду
    назад. Дельты через /api/events — десятки байт, когда ничего не изменилось.
    """
    src = app_src()
    body = func_body(src, "pollState")
    assert "/api/events" in body, \
        "pollState не ходит в /api/events — полное состояние качается каждый тик:\n%s" % body
    assert "since=" in body, "в запросе дельт нет параметра since (все события разом):\n%s" % body
    assert "resync" in body, \
        "pollState не смотрит на resync — клиент, отставший дальше буфера событий, " \
        "останется с устаревшим списком:\n%s" % body
    assert "needFullState" in body, \
        "нет флага needFullState — непонятно, когда состояние нужно целиком:\n%s" % body


def w6_response_parse_reports_failure():
    """Ошибка разбора должна называть причину, а не падать сырым SyntaxError."""
    src = app_src()
    assert "function jsonOf(" in src, "нет обёртки разбора ответа"
    body = func_body(src, "jsonOf")
    assert "await r.text()" in body, \
        "jsonOf должен читать тело текстом: пустой или оборванный ответ иначе " \
        "даёт невнятный SyntaxError:\n%s" % body
    for call_site in ('"/api/state"', '"/api/events"', '"/rpc"'):
        if call_site == '"/api/events"':
            continue
        assert call_site in src, "нет обращения к %s" % call_site
    # Сырой r.json() в UI больше не осталось: он и давал «Unexpected end of JSON input».
    leftovers = [ln.strip() for ln in src.splitlines() if ".json()" in ln]
    assert not leftovers, "остались прямые вызовы r.json() без диагностики: %r" % leftovers


def w7_variant_error_in_tooltip():
    """Причина падения варианта должна быть видна в тултипе красной точки."""
    src = app_src()
    body = func_body(src, "taskDot")
    assert "info.error" in body, (
        "taskDot игнорирует info.error — красная точка варианта молчит, и "
        "пользователю остаётся причина ошибки файла целиком вместо причины "
        "конкретного варианта:\n%s" % body)
    assert "Причина" in body, \
        "в тултипе нет подписи к причине (\nПричина: ...):\n%s" % body


def w8_no_full_path_in_row_tooltip():
    """Тултип строки не должен содержать полный путь.

    Подсказка из полного пути превращалась в многострочную простыню и перекрывала
    весь экран, хотя в самой ячейке рядом уже виден тот же путь, урезанный до
    target_dir. Полный путь остаётся в CLI и в /api/state (поле path).
    """
    src = app_src()
    body = func_body(src, "renderQueue")
    assert "fullPathFor" not in body, (
        "renderQueue снова кладёт полный путь в title строки:\n%s" % body)
    assert "shortPathFor" in body, \
        "renderQueue не использует shortPathFor — тултип строки пустой или полный"
    fn = func_body(src, "shortPathFor")
    assert "target_dir" in fn, \
        "shortPathFor не урезает по target_dir — длинные пути останутся длинными"
    assert "function displayPath" in src, "нет displayPath"


def w9_stats_page_wired():
    """Страница эффективности связана: роут, разметка, скрипт и переходы.

    Графики живут на отдельной странице /stats, а не оверлеем над очередью: у
    очереди своя прокрутка, выделение и автоскролл, и перерисовывать её ради
    графиков нельзя. Регрессия здесь молчаливая — без роута страница просто не
    откроется, без ссылки на неё не найти с очереди.
    """
    src = open(os.path.join(ROOT, "src", "http_api.cpp"), encoding="utf-8").read()
    assert 'svr.Get("/stats"' in src, "в http_api.cpp нет роута /stats"
    assert '"stats.html"' in src, "роут /stats не отдаёт stats.html"
    html = open(os.path.join(WEB_DIR, "stats.html"), encoding="utf-8").read()
    assert 'src="/static/stats.js"' in html, "страница не подключает stats.js"
    assert 'id="stats-chart"' in html, "нет контейнера диаграммы"
    js = open(os.path.join(WEB_DIR, "stats.js"), encoding="utf-8").read()
    assert "/api/stats" in js, "stats.js не ходит в /api/stats"
    assert 'href="/"' in html, "со страницы эффективности нет ссылки на очередь"
    queue = open(os.path.join(WEB_DIR, "index.html"), encoding="utf-8").read()
    assert 'href="/stats"' in queue, "с очереди нет ссылки на эффективность"


def w10_negative_mean_still_drawn():
    """Метод с отрицательным средним обязан остаться на диаграмме.

    Средние бывают отрицательными: ALAC на этой библиотеке в среднем УВЕЛИЧИВАЕТ
    файл относительно несжатого оригинала, TTA тоже. Засечка на среднем не должна
    попадать под условие по знаку — иначе метод с отрицательным средним останется
    без маркера, то есть пропадёт ровно тогда, когда его результат интересен.
    Раньше тело свечи считалось как y1 - y0, и у отрицательного среднего высота
    становилась отрицательной, а rect с отрицательным height не рисуется.
    """
    src = open(os.path.join(WEB_DIR, "stats.js"), encoding="utf-8").read()
    chart = func_body(src, "renderStatsChart")
    assert "stats-meanline" in chart, "засечка на среднем не рисуется"
    assert "stats-candle--neg" in chart, "нет отдельного оформления отрицательного среднего"
    assert "m.mean < 0" in chart, "отрицательное сред ничем не выделяется"
    assert not re.search(r"if\s*\([^)]*mean\s*>\s*0[^)]*\)[^{]*\{[^}]*stats-meanline", chart), \
        "засечка на среднем рисуется только для положительного значения"


def w11_stats_not_in_queue_page():
    """Эффективность не должна быть частью основного окна очереди.

    Заказчик попросил отдельную страницу: пока графики жили оверлеем, открытие
    панели сбрасывало автоскролл и выделение очереди. Проверяем, что в разметке
    очереди нет ни графиков, ни их скрипта, а страница графиков не тащит в себя
    таблицу очереди.
    """
    queue = open(os.path.join(WEB_DIR, "index.html"), encoding="utf-8").read()
    assert "stats-overlay" not in queue, "оверлей статистики вернулся в очередь"
    assert "stats.js" not in queue, "очередь подключает скрипт статистики"
    assert 'id="stats-chart"' not in queue, "в очереди снова есть контейнер диаграммы"
    page = open(os.path.join(WEB_DIR, "stats.html"), encoding="utf-8").read()
    assert 'id="queue-table"' not in page, "страница эффективности тащит таблицу очереди"


def w12_no_duplicate_functions():
    """Верхнеуровневых функций с одинаковым именем быть не должно.

    В JavaScript повторное объявление функции не считается ошибкой: последнее
    определение молча выигрывает у предыдущего. На странице эффективности это
    уже стоило двух сломанных графиков — старая statsDomain от ±2σ перекрыла
    новую, построенную на перцентилях, и ось уезжала в −113 %, а рядом лежали
    ещё две копии statsTip. Никакого сообщения при этом не появляется.
    """
    for name in ("app.js", "stats.js"):
        src = open(os.path.join(WEB_DIR, name), encoding="utf-8").read()
        found = re.findall(r"^(?:async\s+)?function\s+([A-Za-z_$][\w$]*)", src, re.M)
        dupes = sorted({f for f in found if found.count(f) > 1})
        assert not dupes, "%s: функции определены повторно (%s)" % (name, ", ".join(dupes))


def w13_response_parsed_once():
    """Ответ сервера должен разбираться ровно один раз.

    api() отдаёт Response, а разбор делает jsonOf() на стороне вызова. Если
    api() разбирает JSON сам, то обёртка jsonOf(await api(...)) получает вместо
    ответа обычный объект и падает на r.text — ошибка видна только в браузере,
    а в среде нет ни node, ни браузера, чтобы поймать её тестом.

    Требование «каждый вызов обёрнут в jsonOf» сюда не годится: в app.js
    половина мест разбирает ответ иначе (там свой rpc() со своим контрактом).
    Проверяем ровно два инварианта, которые ломали страницу по-настоящему.
    """
    for name in ("app.js", "stats.js"):
        src = open(os.path.join(WEB_DIR, name), encoding="utf-8").read()
        m = re.search(r"^async function api\s*\([^)]*\)\s*\{", src, re.M)
        assert m, "%s: не найдена функция api()" % name
        end_m = re.compile(r"^\}", re.M)
        body = src[m.end():end_m.search(src, m.end()).start()]
        assert "jsonOf(" not in body, \
            "%s: api() разбирает JSON сам — обёртка jsonOf(await api(...)) упадёт на r.text" % name

    stats = open(os.path.join(WEB_DIR, "stats.js"), encoding="utf-8").read()
    calls = list(re.finditer(r"await api\(", stats))
    assert calls, "stats.js ни разу не ходит в api()"
    for call in calls:
        before = stats[max(0, call.start() - 20):call.start()]
        assert "jsonOf(" in before, \
            "stats.js: await api() вне jsonOf() — ответ будет разобран некорректно"


def w14_one_plane_shared_axis():
    """Все задачи обязаны лежать в одной системе координат.

    Заказчик требовал сравнивать «кто выше кого и на сколько». Это возможно
    только в одной плоскости: общая ось экономии по вертикали, задачи
    разложены по горизонтали, а высота столбиков нормирована по общему
    максимуму. Раньше был фиксированный viewBox с preserveAspectRatio — он
    вписывался по меньшей стороне, оставляя половину окна пустой.
    """
    src = open(os.path.join(WEB_DIR, "stats.js"), encoding="utf-8").read()
    chart = func_body(src, "renderStatsChart")
    # Границы оси объявлены на уровне модуля, раскладка — внутри функции.
    assert "const SAV_LO" in src and "const SAV_HI" in src, "нет общей оси экономии"
    assert "SAV_LO" in chart and "SAV_HI" in chart, "функция не использует общую ось"
    assert "const COL = (W - ML - MR) / items.length" in chart, \
        "задачи не разложены по общей оси на равные колонки"
    # Проверяем именно вызов setAttribute: слово может встречаться в комментарии,
    # и искать его отсутствие бессмысленно.
    assert 'setAttribute("preserveAspectRatio"' not in chart, \
        "viewBox фиксирован: изображение вписывается по меньшей стороне и пустые поля"
    # График обязан подстраиваться под окно, а не под фиксированный viewBox.
    assert "box.clientWidth" in chart and "box.clientHeight" in chart, \
        "размеры берутся не из контейнера"
    # Свечи и гистограммы — вертикальные, обе части на одной оси экономии.
    assert "stats-candle" in chart, "свеча не рисуется"
    assert "quantileFromHist(m, 0.25)" in chart and "quantileFromHist(m, 0.75)" in chart, \
        "тело свечи строится не по межквартильному размаху"
    assert "m.hist.bins" in chart, "гистограмма распределения не рисуется"


def w15_shape_classes_do_not_collide():
    """Классы фигур графика не должны совпадать с классами вёрстки страницы.

    Ровно на этом стоял весь график: тело свечи называлось stats-body — так же,
    как контейнер страницы со стилями display:flex и height:calc(100vh - 56px).
    Прямоугольник получал flex-раскладку и растягивался на всю высоту окна
    вместо своих 160 пикселей — свечи превращались в зелёные полосы во всю
    высоту, и никакого графика не было видно. Ошибка молчаливая: разметка верная,
    числа верные, а на экране мусор.
    """
    css = open(os.path.join(WEB_DIR, "style.css"), encoding="utf-8").read()
    shape_classes = ("stats-candle", "stats-hist", "stats-band", "stats-wick",
                     "stats-meanline", "stats-clip", "stats-sep")
    for name in shape_classes:
        for rule in re.findall(r"\.%s\s*\{([^}]*)\}" % re.escape(name), css):
            assert "display:flex" not in rule and "height:calc" not in rule, \
                ".%s объявлен как элемент вёрстки, а используется как фигура SVG" % name
    # Имя фигуры свечи не должно совпадать с именем контейнера страницы.
    assert ".stats-body" not in css, "класс stats-body снова занят контейнером страницы"
    page = open(os.path.join(WEB_DIR, "stats.html"), encoding="utf-8").read()
    assert 'class="stats-page"' in page, "контейнер страницы должен быть stats-page"
    js = open(os.path.join(WEB_DIR, "stats.js"), encoding="utf-8").read()
    assert '"stats-candle"' in js, "тело свечи должно иметь класс stats-candle"
    assert '"stats-body' not in js, "в разметке фигур снова встречается stats-body"


def w16_filters_do_not_reflow():
    """Набор кнопок фильтра и их размеры не должны зависеть от нажатия.

    Жалоба была на перестановку кнопок при переключении фильтра. Причин две:
    счётчик треков попал в саму подпись, поэтому менялась ширина текста и вся
    полоса перетекала; и кнопки рисовались по facet_counts, где значения уже
    отфильтрованы, поэтому часть кнопок исчезала, а набор менялся.

    Теперь набор берётся из facet_values (всегда один и тот же), счётчик живёт
    в отдельном блоке фиксированной ширины, а состояние меняет только цвет.
    """
    src = open(os.path.join(ROOT, "src", "stats.cpp"), encoding="utf-8").read()
    assert '"facet_values"' in src, "сервер не отдаёт постоянный набор значений грани"
    js = open(os.path.join(WEB_DIR, "stats.js"), encoding="utf-8").read()
    fn = func_body(js, "renderStatsFilters")
    assert "facet_values" in fn, "кнопки рисуются не по постоянному набору"
    assert "chip__count" in fn, "счётчик не вынесен в отдельный блок"
    assert "chip__label" in fn, "подпись не отделена от счётчика"
    css = open(os.path.join(WEB_DIR, "style.css"), encoding="utf-8").read()
    count = re.search(r"\.chip__count\s*\{([^}]*)\}", css)
    assert count, "нет правила для счётчика"
    assert "min-width" in count.group(1) and "tabular-nums" in count.group(1), \
        "счётчик не имеет фиксированной ширины: цифры разной длины меняют размер кнопки"
    chip = re.search(r"\n\.chip\s*\{([^}]*)\}", css)
    assert chip, "нет базового правила кнопки"
    assert "padding" in chip.group(1), "у кнопки нет явных отступов"



SCENARIOS = [
    ("W1", "W1  статусбар обновляется до раннего выхода", w1_statusbar_before_early_return),
    ("W2", "W2  нет раннего выхода мимо статусбара", w2_no_early_return_skips_calls),
    ("W3", "W3  удаление опрашивает состояние сразу", w3_removal_refreshes_immediately),
    ("W4", "W4  вшитый ассет не старше web/app.js", w4_embedded_assets_in_sync),
    ("W5", "W5  опрос дельтами, полный state — по resync", w5_polling_uses_events_deltas),
    ("W6", "W6  внятная ошибка разбора вместо SyntaxError", w6_response_parse_reports_failure),
    ("W7", "W7  причина падения варианта в тултипе точки", w7_variant_error_in_tooltip),
    ("W8", "W8  в тултипе строки нет полного пути", w8_no_full_path_in_row_tooltip),
    ("W9", "W9  страница эффективности связана и достижима", w9_stats_page_wired),
    ("W10", "W10 отрицательное среднее остаётся на диаграмме", w10_negative_mean_still_drawn),
    ("W11", "W11 эффективность вынесена из окна очереди", w11_stats_not_in_queue_page),
    ("W12", "W12 нет повторных определений функций", w12_no_duplicate_functions),
    ("W13", "W13 ответ разбирается ровно один раз", w13_response_parsed_once),
    ("W14", "W14 все методы на одной плоскости", w14_one_plane_shared_axis),
    ("W15", "W15 классы фигур не пересекаются с вёрсткой", w15_shape_classes_do_not_collide),
    ("W16", "W16 кнопки фильтра не переставляются", w16_filters_do_not_reflow),
]


def main():
    for key, desc, fn in SCENARIOS:
        try:
            fn()
            RESULTS.append((desc, True, ""))
        except AssertionError as exc:
            RESULTS.append((desc, False, str(exc)[:800]))
        except Exception as exc:  # noqa: BLE001
            RESULTS.append((desc, False, "%s: %s" % (type(exc).__name__, exc)))
    failed = sum(1 for _, ok, _ in RESULTS if not ok)
    print()
    print("%-72s %s" % ("Проверка", "Результат"))
    print("-" * 85)
    for desc, ok, detail in RESULTS:
        print("%-72s %s" % (desc, "OK" if ok else "FAIL"))
        if not ok:
            print("        " + detail.replace("\n", "\n        "))
    print("Итого: %d/%d прошло" % (len(RESULTS) - failed, len(RESULTS)))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
