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


def w9_stats_panel_wired():
    """Кнопка в шапке, панель и её обработчики должны быть связаны.

    Панель эффективности кодеков — не вкладка, а оверлей поверх очереди: за её
    время просмотра очередь не перерисовывается, поэтому выделение и прокрутка
    сохраняются. Регрессия здесь молчаливая — если разъедутся id, панель просто
    не откроется.
    """
    src = app_src()
    html = open(os.path.join(WEB_DIR, "index.html"), encoding="utf-8").read()
    for eid in ("btn-stats", "stats-overlay", "stats-filters", "stats-chart"):
        assert 'id="%s"' % eid in html, "в index.html нет #%s" % eid
        assert '"%s"' % eid in src, "app.js не обращается к #%s" % eid
    assert 'id="btn-stats-close"' in html, "в index.html нет кнопки закрытия панели"
    assert "/api/stats" in src, "панель не ходит в /api/stats"


def w10_negative_mean_still_drawn():
    """Свеча с отрицательным средним должна рисоваться, а не исчезать.

    Средние бывают отрицательными: alac на этой библиотеке в среднем УВЕЛИЧИВАЕТ
    файл (-8.6 %), tta тоже. Если высоту тела считать как y1 - y0 без модуля,
    у отрицательного среднего высота станет отрицательной, а rect с
    отрицательным height не рисуется — метод молча пропадёт с диаграммы ровно
    тогда, когда его результат интересен.
    """
    src = app_src()
    body = func_body(src, "renderStatsChart")
    assert "Math.abs(" in body, "высота свечи не берётся по модулю"
    assert "stats-body--neg" in body, "нет отдельного класса для отрицательного среднего"
    assert "y(Math.abs(" not in body, "подозрение: модуль применён к значению, а не к высоте"


def w11_stats_not_shown_by_default():
    """Панель эффективности не должна быть основным окном.

    Требование заказчика: статистика не показывается по дефолту. Панель обязана
    быть скрыта в разметке, иначе она перекроет очередь при загрузке страницы.
    """
    html = open(os.path.join(WEB_DIR, "index.html"), encoding="utf-8").read()
    m = re.search(r'<div id="stats-overlay"[^>]*>', html)
    assert m, "нет контейнера #stats-overlay"
    assert "hidden" in m.group(0), "панель статистики открыта по умолчанию: %s" % m.group(0)
    src = app_src()
    assert "el(\"btn-stats\")" in src, "нет кнопки открытия панели"


SCENARIOS = [
    ("W1", "W1  статусбар обновляется до раннего выхода", w1_statusbar_before_early_return),
    ("W2", "W2  нет раннего выхода мимо статусбара", w2_no_early_return_skips_calls),
    ("W3", "W3  удаление опрашивает состояние сразу", w3_removal_refreshes_immediately),
    ("W4", "W4  вшитый ассет не старше web/app.js", w4_embedded_assets_in_sync),
    ("W5", "W5  опрос дельтами, полный state — по resync", w5_polling_uses_events_deltas),
    ("W6", "W6  внятная ошибка разбора вместо SyntaxError", w6_response_parse_reports_failure),
    ("W7", "W7  причина падения варианта в тултипе точки", w7_variant_error_in_tooltip),
    ("W8", "W8  в тултипе строки нет полного пути", w8_no_full_path_in_row_tooltip),
    ("W9", "W9  панель эффективности связана с кнопкой и /api/stats", w9_stats_panel_wired),
    ("W10", "W10 отрицательное среднее рисуется, а не исчезает",
     w10_negative_mean_still_drawn),
    ("W11", "W11 панель статистики скрыта по умолчанию", w11_stats_not_shown_by_default),
]


def main():
    for _k, _d, fn in SCENARIOS:
        scenario(_k, fn)

    print()
    print("%-62s %s" % ("Сценарий", "Результат"))
    print("-" * 72)
    failed = 0
    for name, ok, detail in RESULTS:
        print("%-62s %s" % (name, "OK " if ok else "FAIL"))
        if not ok:
            failed += 1
            print("        " + detail.replace("\n", "\n        "))
    print("Итого: %d/%d прошло" % (len(RESULTS) - failed, len(RESULTS)))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()