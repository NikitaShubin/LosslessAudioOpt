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
    """Текст функции верхнего уровня по имени: от `function name(` до закрывающей
    скобки на нулевом отступе. Проще и надёжнее, чем brace-matching с учётом
    строковых литералов: в app.js нет вложенных объявлений функций с тем же
    именем, а отступы выдерживаются единообразно."""
    start = src.find("function %s(" % name)
    if start < 0:
        start = src.find("async function %s(" % name)
    assert start >= 0, "функция %s не найдена" % name
    m = re.compile(r"^function %s\(|^async function %s\(" % (name, name), re.M)
    end_m = re.compile(r"^\}", re.M)
    m2 = end_m.search(src, start)
    assert m2, "не найден конец функции %s" % name
    assert m.search(src, start), "не найдено объявление %s" % name
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


SCENARIOS = [
    ("W1", "W1  статусбар обновляется до раннего выхода", w1_statusbar_before_early_return),
    ("W2", "W2  нет раннего выхода мимо статусбара", w2_no_early_return_skips_calls),
    ("W3", "W3  удаление опрашивает состояние сразу", w3_removal_refreshes_immediately),
    ("W4", "W4  вшитый ассет не старше web/app.js", w4_embedded_assets_in_sync),
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