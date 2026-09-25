"""Schemes of the on-air clock: one request, and the layouts nodes can form.

Usage:  python tools/screen_render/render_clock_schemes.py [out_dir]
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BG = (32, 34, 40)
CARD = (46, 49, 58)
INK = (236, 238, 242)
MUTED = (176, 182, 192)
ACCENT = (120, 186, 255)
OK = (126, 196, 140)
WARN = (232, 168, 96)
BAD = (214, 112, 112)
LINE = (150, 158, 170)


def font(size, bold=False):
    names = ("segoeuib.ttf", "segoeui.ttf") if bold else ("segoeui.ttf", "arial.ttf")
    if bold:
        names = ("segoeuib.ttf", "arialbd.ttf", "segoeui.ttf")
    for name in names:
        try:
            return ImageFont.truetype(os.path.join(r"C:\Windows\Fonts", name), size)
        except OSError:
            pass
    return ImageFont.load_default()


F = font(22)
FB = font(26, True)
FS = font(18)
FT = font(34, True)


def text(dr, xy, s, fill=INK, fnt=None):
    dr.text(xy, s, font=fnt or F, fill=fill)


def wrap(dr, s, fnt, width):
    lines, cur = [], ""
    for word in s.split():
        trial = word if not cur else cur + " " + word
        if dr.textlength(trial, font=fnt) <= width:
            cur = trial
        else:
            if cur:
                lines.append(cur)
            cur = word
    if cur:
        lines.append(cur)
    return lines


def box(dr, x, y, w, h, title, lines, fill=CARD, title_fill=ACCENT):
    dr.rounded_rectangle([x, y, x + w, y + h], 14, fill=fill)
    text(dr, (x + 16, y + 12), title, title_fill, FB)
    yy = y + 52
    for line in lines:
        text(dr, (x + 16, yy), line, INK, FS)
        yy += 26
    return yy


def arrow(dr, x1, y1, x2, y2, color=LINE):
    dr.line([(x1, y1), (x2, y2)], fill=color, width=3)
    import math
    ang = math.atan2(y2 - y1, x2 - x1)
    L = 12
    for da in (2.6, -2.6):
        dr.line([
            (x2, y2),
            (x2 - L * math.cos(ang + da), y2 - L * math.sin(ang + da)),
        ], fill=color, width=3)


def request_page():
    W, H = 1280, 860
    im = Image.new("RGB", (W, H), BG)
    dr = ImageDraw.Draw(im)
    text(dr, (40, 28), "Как узел спрашивает время", fnt=FT)
    text(dr, (40, 82), "Companion и Repeater. Комнатный сервер в этом не участвует.", MUTED, F)

    box(dr, 40, 150, 360, 250, "Кто спрашивает", [
        "Список clock.node, до 16 узлов.",
        "Спрашивает по одному, по кругу.",
        "Нет маршрута — берёт следующего.",
        "Первый раз через 20 секунд.",
        "Пока часов нет — каждые 2 минуты.",
        "Часы есть — раз в 6 часов.",
    ])
    box(dr, 880, 150, 360, 250, "Кто отвечает", [
        "Companion или Repeater,",
        "у которого время уже стоит.",
        "Пустые часы в ответ не отдаёт.",
        "Companion — не чаще раза в 5 с.",
        "Пакет идёт по известному пути,",
        "в том числе через репитеры.",
    ], title_fill=OK)

    arrow(dr, 400, 250, 870, 250, ACCENT)
    text(dr, (500, 214), "запрос, не флуд", ACCENT, FS)
    arrow(dr, 870, 310, 400, 310, OK)
    text(dr, (500, 322), "чужое время + половина пути", OK, FS)

    box(dr, 40, 460, 1200, 150, "Когда свои часы двигаются", [
        "Времени ещё не было — берётся ответ как есть.",
        "Время уже есть — только вперёд, и только если чужие часы впереди больше чем на 30 секунд.",
        "GPS ставит время один раз, пока часы пустые, и больше их не переписывает.",
    ], title_fill=WARN)

    box(dr, 40, 650, 1200, 170, "Маршрут", [
        "Путь уже известен (контакт или клиент репитера) — запрос идёт по нему, хопов может быть несколько.",
        "Путь неизвестен — только если этот узел только что слышали напрямую, без промежуточных.",
        "Широковещательно время не рассылается и само не ищется по всей сети.",
    ])
    return im


def card(dr, x, y, w, h, title, body, ok=True):
    dr.rounded_rectangle([x, y, x + w, y + h], 14, fill=CARD)
    bar = OK if ok else BAD
    dr.rectangle([x, y, x + 8, y + h], fill=bar)
    text(dr, (x + 24, y + 14), title, bar, FB)
    yy = y + 56
    for line in wrap(dr, body, FS, w - 40):
        text(dr, (x + 24, yy), line, INK, FS)
        yy += 26


def combinations_page():
    W, H = 1280, 1180
    im = Image.new("RGB", (W, H), BG)
    dr = ImageDraw.Draw(im)
    text(dr, (40, 28), "Какие схемы из серверов времени собираются", fnt=FT)
    text(dr, (40, 82), "Узел с уточненными часами сам отвечает другим. Список clock.node — это только кого он спрашивает.", MUTED, F)

    gap, left, top = 24, 40, 140
    cw, ch = (W - left * 2 - gap) // 2, 220
    items = [
        (True, "Один источник",
         "Телефон или GPS ставят время одному Companion. Остальные, и репитер, держат в списке только его. Он никого не спрашивает."),
        (True, "Репитер как часы района",
         "Один Companion с GPS или телефоном. Репитер спрашивает его. Все остальные спрашивают репитер: один известный путь вместо пачки дальних."),
        (True, "Цепочка",
         "A знает время. B спрашивает A. C спрашивает B. Каждый, получив время, начинает отвечать дальше. Часы по цепочке только догоняют вперёд."),
        (True, "Несколько источников",
         "В списке два или три узла. Опрос по кругу: кто не на связи, пропускается. Берётся ответ, который впереди больше чем на 30 секунд."),
        (True, "Два узла друг на друга",
         "Пока оба пустые, отвечать некому. Как только один получил время с телефона или GPS, второй его забирает. Дальше обновляется тот, кто отстал."),
        (False, "Круг из пустых часов",
         "Все ссылаются друг на друга, и ни у кого нет времени. Запросы уходят, ответов нет. Нужен хотя бы один узел с телефоном, GPS или командой set time."),
    ]
    for i, (ok, title, body) in enumerate(items):
        x = left + (i % 2) * (cw + gap)
        y = top + (i // 2) * (ch + gap)
        card(dr, x, y, cw, ch, title, body, ok)

    text(dr, (40, 1100), "Комнатный сервер время не раздаёт и сам по списку его не спрашивает.", MUTED, F)
    return im


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "docs", "screens")
    os.makedirs(out, exist_ok=True)
    request_page().save(os.path.join(out, "clock_request.png"))
    combinations_page().save(os.path.join(out, "clock_combinations.png"))
    print("saved", out)


if __name__ == "__main__":
    main()
