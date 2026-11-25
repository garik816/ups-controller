#!/usr/bin/env python3
"""Regenerate the resistor wiring SVGs (requires matplotlib; not a build dependency)."""
import argparse
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
matplotlib.rcParams.update({"svg.fonttype": "none", "svg.hashsalt": "ups-controller",
                           "font.family": "DejaVu Sans"})
import matplotlib.pyplot as plt
from matplotlib.patches import Circle, Rectangle, FancyBboxPatch

ROOT = Path(__file__).resolve().parent
INK = "#153043"
WIRE = "#147b75"
MUTED = "#526672"
BLUE = "#1459a1"
AMBER = "#87520e"
PANEL = "#f3f7f9"


def canvas(height):
    fig, ax = plt.subplots(figsize=(14, height / 100), dpi=100)
    fig.subplots_adjust(left=0, right=1, top=1, bottom=0)
    ax.set_xlim(0, 1400)
    ax.set_ylim(height, 0)
    ax.axis("off")
    fig.set_facecolor("white")
    return fig, ax


def text(ax, x, y, s, size=14, color=INK, weight="normal", ha="left", va="center"):
    ax.text(x, y, s, fontsize=size, color=color, fontweight=weight,
            ha=ha, va=va, linespacing=1.55)


def line(ax, xs, ys, color=WIRE, lw=2.3, **kw):
    ax.plot(xs, ys, color=color, linewidth=lw, solid_capstyle="round", **kw)


def dot(ax, x, y):
    ax.add_patch(Circle((x, y), 4.4, color=WIRE))


def terminal(ax, x, y):
    ax.add_patch(Circle((x, y), 5, facecolor="white", edgecolor=WIRE, linewidth=2))


def ground(ax, x, y, label=True):
    line(ax, [x, x], [y, y + 10])
    for dy, width in [(10, 32), (18, 22), (26, 10)]:
        line(ax, [x - width / 2, x + width / 2], [y + dy, y + dy])
    if label:
        text(ax, x + 26, y + 16, "GND", 12, MUTED)


def resistor_h(ax, x, y, value, name, width=100, color=WIRE):
    line(ax, [x, x + 20], [y, y], color=color)
    ax.add_patch(Rectangle((x + 20, y - 12), width - 40, 24,
                           facecolor="white", edgecolor=color, linewidth=2.3))
    line(ax, [x + width - 20, x + width], [y, y], color=color)
    text(ax, x + width / 2, y - 31, f"{name}  {value}", 13, color, ha="center")


def resistor_v(ax, x, y, value, name, height=95, label_side="right", color=WIRE):
    line(ax, [x, x], [y, y + 16], color=color)
    ax.add_patch(Rectangle((x - 12, y + 16), 24, height - 32,
                           facecolor="white", edgecolor=color, linewidth=2.3))
    line(ax, [x, x], [y + height - 16, y + height], color=color)
    shift = 23 if label_side == "right" else -23
    align = "left" if label_side == "right" else "right"
    text(ax, x + shift, y + height / 2 - 12, name, 12, color, ha=align)
    text(ax, x + shift, y + height / 2 + 12, value, 13, color, ha=align)


def capacitor_v(ax, x, y, name, value, height=95):
    middle = y + height / 2
    line(ax, [x, x], [y, middle - 5])
    line(ax, [x - 18, x + 18], [middle - 5, middle - 5])
    line(ax, [x - 18, x + 18], [middle + 5, middle + 5])
    line(ax, [x, x], [middle + 5, y + height])
    text(ax, x + 25, middle - 12, name, 12, WIRE)
    text(ax, x + 25, middle + 12, value, 13, WIRE)


def panel(ax, x, y, w, h, title, subtitle=None):
    ax.add_patch(FancyBboxPatch((x, y), w, h,
                              boxstyle="round,pad=0,rounding_size=14",
                              facecolor=PANEL, edgecolor="#d3e0e6", linewidth=1.2))
    text(ax, x + 24, y + 34, title, 18, BLUE, "bold")
    if subtitle:
        text(ax, x + 24, y + 66, subtitle, 12, MUTED)


def save(fig, name, preview_dir):
    svg_path = ROOT / (name + ".svg")
    fig.savefig(svg_path, format="svg", metadata={"Date": None})
    # Matplotlib leaves spaces after SVG path coordinates; keep tracked files clean.
    svg_text = svg_path.read_text(encoding="utf-8")
    svg_path.write_text("\n".join(line.rstrip() for line in svg_text.splitlines()) + "\n",
                        encoding="utf-8")
    if preview_dir:
        preview_dir.mkdir(parents=True, exist_ok=True)
        fig.savefig(preview_dir / (name + ".png"), dpi=100)
    plt.close(fig)


def input_channel(ax, x, y, signal, pin, top, bottom, n):
    # Each row is one independent signal. Only GND is common across rows.
    terminal(ax, x, y)
    text(ax, x, y - 42, signal, 14, INK, "bold")
    text(ax, x, y + 32, "0 / 5 В", 12, MUTED)
    line(ax, [x + 5, x + 85], [y, y])
    resistor_h(ax, x + 85, y, top, f"R{n}")
    line(ax, [x + 185, x + 470], [y, y])
    dot(ax, x + 235, y)
    dot(ax, x + 365, y)
    terminal(ax, x + 470, y)
    text(ax, x + 470, y - 35, pin, 14, INK, "bold", ha="right")
    resistor_v(ax, x + 235, y, bottom, f"R{n + 1}", label_side="left")
    capacitor_v(ax, x + 365, y, f"C{(n + 1)//2}", "10 нФ*", 95)
    line(ax, [x + 235, x + 365], [y + 95, y + 95])
    dot(ax, x + 235, y + 95)
    ground(ax, x + 235, y + 95)


def draw_inputs(preview):
    fig, ax = canvas(1110)
    text(ax, 50, 52, "UPS Controller · резистори дискретних входів", 26, weight="bold")
    text(ax, 50, 98, "Сигнали з активною одиницею: LOW = 0 В, HIGH = 5 В. Показано обидві альтернативні плати.", 14, MUTED)
    panel(ax, 40, 140, 650, 810, "NodeMCU v3 / ESP8266", "Окремий дільник 10 кОм / 15 кОм на кожен вхід")
    panel(ax, 710, 140, 650, 810, "Arduino Nano ATmega328P · 5 В", "Окремий резистор 1 кОм + підтяжка 10 кОм на кожен вхід")
    for y, signal, epin, npin, n in [
        (300, "ONLINE", "D2 / GPIO4", "D9", 1),
        (535, "BATTERY", "D5 / GPIO14", "D10", 3),
        (770, "STANDBY", "D6 / GPIO12", "D11", 5),
    ]:
        input_channel(ax, 80, y, signal, epin, "10 кОм", "15 кОм", n)
        input_channel(ax, 750, y, signal, npin, "1 кОм", "10 кОм", n + 6)
    text(ax, 80, 912, "На GPIO: 3,00 В за вхідних 5,00 В", 14, BLUE, "bold")
    text(ax, 750, 912, "На вході: 4,55 В за вхідних 5,00 В", 14, BLUE, "bold")
    text(ax, 50, 989, "Нижній резистор — обов’язкова підтяжка до GND. GPIO налаштовані як INPUT, без INPUT_PULLUP.", 14)
    text(ax, 50, 1023, "* C1–C6 по 10 нФ — необов’язкові фільтри; ставити біля плати. Резистори ±1%, 0,125 Вт або більше.", 13, MUTED)
    text(ax, 50, 1057, "Усі GND: плата + перевірена безпечна сигнальна земля UPS. Гальванічної розв’язки немає.", 13, AMBER)
    save(fig, "digital-inputs", preview)


def draw_battery(preview):
    fig, ax = canvas(1190)
    text(ax, 50, 52, "UPS Controller · вимірювання батареї 12 В", 26, weight="bold")
    text(ax, 50, 98, "Робочий проєктний діапазон батареї: 0…16 В; очікувана зарядна напруга до 14,4 В.", 15, MUTED)
    panel(ax, 40, 140, 1320, 470, "ESP8266 · зовнішній дільник 180 кОм / 10 кОм", "Цей зовнішній дільник застосовується також до плати без вбудованого дільника A0.")
    x0, y = 85, 300
    terminal(ax, x0, y)
    text(ax, x0, y - 42, "+ батареї", 14, weight="bold")
    line(ax, [x0 + 5, 205], [y, y])
    resistor_h(ax, 205, y, "180 кОм", "R13", 130)
    text(ax, 270, y + 37, "біля батареї", 11, MUTED, ha="center")
    line(ax, [335, 710], [y, y])
    dot(ax, 410, y); dot(ax, 565, y); terminal(ax, 710, y)
    text(ax, 700, y - 33, "A0", 17, BLUE, "bold", ha="right")
    resistor_v(ax, 410, y, "10 кОм", "R14", 112, "left")
    capacitor_v(ax, 565, y, "C7", "100 нФ", 112)
    line(ax, [410, 565], [412, 412]); dot(ax, 410, 412)
    ground(ax, 410, 412)
    terminal(ax, 85, 412); line(ax, [90, 180], [412, 412]); ground(ax, 180, 412)
    text(ax, 85, 382, "− батареї", 13, weight="bold")
    text(ax, 90, 489, "Без вбудованого дільника: A0 ≈ 0,758 В при 14,4 В", 13, BLUE, "bold")
    text(ax, 90, 526, "Без дільника на платі: коефіцієнт 19000 мВ.", 13)
    text(ax, 90, 561, "Зі вбудованими 220 кОм / 100 кОм: 62600 мВ.", 13)
    # Existing board network, shown only as a conditional reference model.
    ax.add_patch(Rectangle((750, 225), 560, 232, facecolor="white", edgecolor=MUTED,
                           linewidth=1.5, linestyle=(0, (5, 4))))
    text(ax, 775, 249, "ВЖЕ НА ПЛАТІ, ЯКЩО ВСТАНОВЛЕНО", 12, MUTED, "bold")
    line(ax, [715, 795], [y, y])
    resistor_h(ax, 795, y, "220 кОм", "Rint1", 125)
    line(ax, [920, 1225], [y, y]); dot(ax, 1030, y)
    terminal(ax, 1225, y)
    text(ax, 1258, y - 28, "TOUT · ADC 0…1 В", 12, BLUE, ha="right")
    resistor_v(ax, 1030, y, "100 кОм", "Rint2", 112)
    ground(ax, 1030, 412, False)
    text(ax, 770, 489, "Це модель штатного дільника NodeMCU.", 13, MUTED)
    text(ax, 770, 526, "Rint1 / Rint2 не додавати до монтажу.", 13, MUTED)
    text(ax, 770, 561, "A0 ≈ 0,736 В; TOUT ≈ 0,230 В при 14,4 В.", 13)

    panel(ax, 40, 635, 1320, 360, "Arduino Nano · зовнішній дільник 33 кОм / 10 кОм", "Для Nano ATmega328P із живленням 5 В та analogReference(DEFAULT).")
    y = 795
    terminal(ax, 85, y); text(ax, 85, y - 42, "+ батареї", 14, weight="bold")
    line(ax, [90, 205], [y, y])
    resistor_h(ax, 205, y, "33 кОм", "R15", 130)
    text(ax, 270, y + 37, "біля батареї", 11, MUTED, ha="center")
    line(ax, [335, 760], [y, y]); dot(ax, 410, y); dot(ax, 565, y)
    resistor_v(ax, 410, y, "10 кОм", "R16", 110, "left")
    capacitor_v(ax, 565, y, "C8", "100 нФ", 110)
    line(ax, [410, 565], [905, 905]); dot(ax, 410, 905); ground(ax, 410, 905)
    terminal(ax, 85, 905); line(ax, [90, 180], [905, 905]); ground(ax, 180, 905)
    text(ax, 85, 875, "− батареї", 13, weight="bold")
    terminal(ax, 760, y); text(ax, 760, y - 33, "A0", 17, BLUE, "bold", ha="right")
    text(ax, 835, 788, "При 14,4 В на батареї:", 15, weight="bold")
    text(ax, 835, 825, "A0 ≈ 3,349 В; струм дільника ≈ 0,335 мА.", 13)
    text(ax, 835, 867, "При AVCC = 5,000 В: коефіцієнт 21500 мВ.", 13)
    text(ax, 835, 910, "Опора — AVCC. Не вибирати INTERNAL 1,1 В.", 13, AMBER)
    text(ax, 90, 970, "Мінус батареї = GND контролера, тільки якщо таке з’єднання перевірене та допустиме.", 13, AMBER)
    text(ax, 50, 1035, "Усі резистори ±1%, 0,125 Вт або більше. C7/C8 — 100 нФ X7R, ≥16 В, біля A0 та GND плати.", 13)
    text(ax, 50, 1073, "Коефіцієнти 62600 / 19000 / 21500 мВ — масштаб перерахунку ADC, не допустима напруга батареї.", 13, AMBER, "bold")
    text(ax, 50, 1111, "Не підключати 12–14,4 В безпосередньо до A0, GPIO, 3V3 чи USB/VIN. Подільник не забезпечує розв’язку.", 13, AMBER)
    text(ax, 50, 1149, "Калібрувати за мультиметром. Від’єднувати сигнали й батарейний вхід перед знеструмленням контролера.", 13, MUTED)
    save(fig, "battery-divider", preview)


def draw_available_resistors(preview):
    """Alternative wiring using only 10k, 51k and 220k resistor values."""
    fig, ax = canvas(1720)
    text(ax, 50, 52, "UPS Controller · наявні резистори 10 / 51 / 220 кОм", 25, weight="bold")
    text(ax, 50, 98, "Окремий варіант монтажу: потрібні кілька резисторів одного номіналу. Базові схеми залишаються чинними.", 13, MUTED)

    panel(ax, 40, 145, 650, 650, "ESP8266 · цифрові входи 0 / 5 В", "Один канал: 4 × 10 кОм. Повторити незалежно для трьох входів.")
    y = 310
    terminal(ax, 85, y)
    text(ax, 85, y - 58, "Сигнал UPS", 13, weight="bold")
    line(ax, [90, 175], [y, y])
    resistor_h(ax, 175, y, "10 кОм", "E1", 110)
    line(ax, [285, 625], [y, y]); dot(ax, 350, y); dot(ax, 555, y)
    terminal(ax, 625, y); text(ax, 625, y - 35, "GPIO", 15, BLUE, "bold", ha="right")
    resistor_v(ax, 350, y, "10 кОм", "E2", 110, "left")
    dot(ax, 350, y + 110)
    line(ax, [270, 430], [y + 110, y + 110])
    resistor_v(ax, 270, y + 110, "10 кОм", "E3", 110, "left")
    resistor_v(ax, 430, y + 110, "10 кОм", "E4", 110, "right")
    line(ax, [270, 430], [y + 220, y + 220]); dot(ax, 350, y + 220)
    ground(ax, 350, y + 220)
    capacitor_v(ax, 555, y, "CE1", "10 нФ*", 220)
    ground(ax, 555, y + 220, False)
    text(ax, 85, 594, "Нижнє плече: 10 + (10 || 10) = 15 кОм", 14, BLUE, "bold")
    text(ax, 85, 632, "5,00 В → 3,00 В; струм сигналу 0,20 мА.", 13)
    text(ax, 85, 666, "HIGH: 2,735–3,275 В за умов, зазначених унизу.", 12, MUTED)
    text(ax, 85, 713, "ONLINE → D2 / GPIO4; BATTERY → D5 / GPIO14", 12)
    text(ax, 85, 747, "STANDBY → D6 / GPIO12. GPIO: INPUT.", 12)

    panel(ax, 710, 145, 650, 650, "Nano 5 В · цифрові входи 0 / 5 В", "Один канал: 10 кОм + 51 кОм. Повторити для трьох входів.")
    x, y = 750, 310
    terminal(ax, x, y); text(ax, x, y - 58, "Сигнал UPS", 13, weight="bold")
    line(ax, [x + 5, x + 85], [y, y])
    resistor_h(ax, x + 85, y, "10 кОм", "N1", 110)
    line(ax, [x + 195, x + 500], [y, y])
    dot(ax, x + 235, y); dot(ax, x + 390, y)
    terminal(ax, x + 500, y); text(ax, x + 500, y - 35, "Вхід Nano", 14, BLUE, "bold", ha="right")
    resistor_v(ax, x + 235, y, "51 кОм", "N2", 150, "left")
    capacitor_v(ax, x + 390, y, "CN1", "10 нФ*", 150)
    line(ax, [x + 235, x + 390], [y + 150, y + 150]); dot(ax, x + 235, y + 150)
    ground(ax, x + 235, y + 150)
    text(ax, 750, 553, "Нижні 51 кОм — підтяжка до GND.", 14, BLUE, "bold")
    text(ax, 750, 594, "5,00 В → 4,18 В; струм сигналу 0,082 мА.", 13)
    text(ax, 750, 632, "HIGH: 3,904–4,459 В за умов, зазначених унизу.", 12, MUTED)
    text(ax, 750, 679, "ONLINE → D9; BATTERY → D10; STANDBY → D11.", 12)
    text(ax, 750, 713, "Входи: INPUT, без INPUT_PULLUP.", 12)
    text(ax, 750, 747, "Для ESP8266 ця пара 10 / 51 кОм не підходить.", 12, AMBER)

    panel(ax, 40, 820, 650, 555, "ESP8266 · батарея 0…16 В", "Верхні 220 кОм, нижні 10 кОм. 100 нФ біля A0.")
    panel(ax, 710, 820, 650, 555, "Nano 5 В · батарея 0…16 В", "Верхні 51 кОм, нижні 10 кОм. Опора ADC — AVCC.")
    for x, top, topname, bottomname, capname in [
        (85, "220 кОм", "E5", "E6", "CE2"),
        (750, "51 кОм", "N3", "N4", "CN2"),
    ]:
        y = 995
        terminal(ax, x, y); text(ax, x, y - 58, "+ батареї", 13, weight="bold")
        line(ax, [x + 5, x + 85], [y, y])
        resistor_h(ax, x + 85, y, top, topname, 110)
        text(ax, x + 140, y + 36, "біля батареї", 11, MUTED, ha="center")
        line(ax, [x + 195, x + 500], [y, y])
        dot(ax, x + 260, y); dot(ax, x + 390, y)
        terminal(ax, x + 500, y); text(ax, x + 500, y - 35, "A0", 16, BLUE, "bold", ha="right")
        resistor_v(ax, x + 260, y, "10 кОм", bottomname, 110, "left")
        capacitor_v(ax, x + 390, y, capname, "100 нФ", 110)
        line(ax, [x + 260, x + 390], [y + 110, y + 110]); dot(ax, x + 260, y + 110)
        ground(ax, x + 260, y + 110)
        terminal(ax, x, y + 110); line(ax, [x + 5, x + 95], [y + 110, y + 110])
        ground(ax, x + 95, y + 110, False)
        text(ax, x, y + 78, "− батареї", 12, weight="bold")
    text(ax, 85, 1173, "Без дільника на платі: A0 ≈ 0,626 В при 14,4 В.", 12)
    text(ax, 85, 1211, "Прямий ADC 1 В: коефіцієнт 23000 мВ.", 13, BLUE, "bold")
    text(ax, 85, 1251, "Якщо на платі вже є 220 кОм / 100 кОм:", 12)
    text(ax, 85, 1287, "A0 ≈ 0,608 В; коефіцієнт 75800 мВ.", 13, BLUE, "bold")
    text(ax, 85, 1328, "Вбудовані резистори не додаються до монтажу.", 12, MUTED)
    text(ax, 750, 1173, "A0 ≈ 2,361 В при 14,4 В батареї.", 13)
    text(ax, 750, 1211, "За AVCC = 5,000 В: коефіцієнт 30500 мВ.", 13, BLUE, "bold")
    text(ax, 750, 1251, "Опір джерела ADC: 51 || 10 ≈ 8,36 кОм.", 13)
    text(ax, 750, 1287, "analogReference(DEFAULT), не INTERNAL 1,1 В.", 12, AMBER)
    text(ax, 750, 1328, "Для обох плат: калібрувати за мультиметром.", 12, MUTED)

    text(ax, 50, 1421, "ESP: 13 × 10 кОм + 1 × 220 кОм", 17, BLUE, "bold")
    text(ax, 750, 1421, "Nano: 4 × 10 кОм + 4 × 51 кОм", 17, BLUE, "bold")
    text(ax, 50, 1463, "Це комплект на одну плату з трьома сигналами та A0. Підтяжка сумісного relay IN: ще 1 × 10 кОм.", 13)
    text(ax, 50, 1503, "Умови розрахунку: резистори ±5%, сигнал HIGH 4,75…5,25 В, живлений контролер, низькоомний вихід UPS.", 13, MUTED)
    text(ax, 50, 1543, "Резистори ≥0,125 Вт; конденсатори ≥16 В. * 10 нФ — необов’язково; 100 нФ — рекомендований фільтр A0.", 13, MUTED)
    text(ax, 50, 1583, "Масштаби 75800 / 23000 / 30500 — коефіцієнти, не допустимі напруги. На батарейному вході: 0…16 В.", 13, AMBER, "bold")
    text(ax, 50, 1623, "Усі GND з’єднуються лише з перевіреною безпечною землею UPS. Дільники не забезпечують ізоляцію.", 13, AMBER)
    text(ax, 50, 1663, "Від’єднайте сигнали й батарейний відвід до знеструмлення контролера. Не подавайте 12–14,4 В прямо на A0.", 13, MUTED)
    save(fig, "available-resistors", preview)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preview-dir", type=Path, help="Optional PNG output directory for visual review")
    arguments = parser.parse_args()
    draw_inputs(arguments.preview_dir)
    draw_battery(arguments.preview_dir)
    draw_available_resistors(arguments.preview_dir)
