# WClicker — Autoclicker для Wayland (Hyprland)

Аналог XClicker, але для Wayland. Використовує `ydotool` для симуляції кліків миші.

## Залежності

```bash
# Arch / Manjaro
sudo pacman -S gtk4 ydotool

# Ubuntu / Debian
sudo apt install libgtk-4-dev ydotool
```

## Запуск ydotoold (демон)

```bash
# Один раз вручну:
sudo ydotoold &

# Або через systemd (рекомендовано):
sudo systemctl enable --now ydotool
```

> **Увага:** `ydotoold` потрібен щоб `ydotool` міг інжектити події без root.
> Після `systemctl enable` більше не потрібно sudo для wclicker.

## Збірка

```bash
make
```

## Запуск

```bash
./wclicker
```

## Hotkey (F6 для старт/стоп)

Додай в `~/.config/hypr/hyprland.conf`:

```
bind = , F6, exec, pkill -SIGUSR1 wclicker
```

Після цього `hyprctl reload` і F6 буде тоглити автоклікер.

## Функції

- Інтервал між кліками (мс)
- Рандомізація інтервалу ±N мс
- Вибір кнопки: Ліва / Середня / Права
- Тип кліку: Одинарний / Подвійний / Утримання
- Час утримання для режиму Hold
- Обмеження кількості повторів (0 = нескінченно)
- Клік у фіксовану позицію (X/Y координати)
- GTK4 GUI
- Hotkey через SIGUSR1 (F6 в Hyprland)
- Лічильник кліків і CPS в реальному часі
