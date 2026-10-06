# Dyson fan web control

A phone-friendly control page for a Dyson purifier fitted with
[Dyson_UART_parser](https://github.com/Alex-Trusk/Dyson_UART_parser), which reads the fan
over its diagnostics UART and sends IR commands.

That firmware's HTTP API sends no CORS headers, so a browser page can't call it directly.
This project serves the page and relays a fixed set of commands to it.

- `index.html`: the control page, with live temperature, humidity, PM2.5/PM10, power, speed 1–10 and modes.
- `firmware/`: PlatformIO project for an ESP32-C3 that hosts the page (the main way to run it).
- `app.py`: the same relay in Python (standard library only), for running on a PC.

The firmware's `On` and `Off` check the fan's state before sending, because the fan firmware
only has an on/off toggle.

## ESP32-C3 firmware

```sh
cd firmware
pio run -t upload
```

The build embeds `../index.html` into the firmware (`embed_page.py` generates `src/page.h`).

**First setup or moving to a new Wi-Fi network:** if the board can't join its saved network
(3 attempts), it opens a hotspot called `Dyson-Fan-Setup` (password `dysonsetup`). Join it from
a phone, choose a network in the captive portal and save. Holding **BOOT** for 5 seconds forgets
Wi-Fi and reopens setup. If nobody completes setup within 5 minutes, the board restarts and
retries the saved network.

**Finding the fan:** the fan controller's IP address is remembered (default `10.0.44.168`, or set in
the setup portal). If it stops answering, the board scans its /24 subnet for a device reporting
`"dev_name": "Dyson ESP Purifier"` and saves the new address.

Open `http://dyson-fan.local/` or the board's IP address. `.local` only works on the same subnet.

## PC version

```sh
FAN_URL=http://10.0.44.168 PORT=8090 python3 app.py
```

## Fan API (Dyson_UART_parser)

- `GET /api/states/sensors` returns `state`, `temperature`, `humidity`, `vent_level`, `pm25_level` and `pm10_level`.
- `POST /api/controls` takes one of these bodies:
  - `{"attributes": "on_off" | "auto_mode_on" | "auto_mode_off" | "night_mode" | "sw_rotation" | "direction"}`
  - `{"set_fan": 1..10}`
  - `{"set_led_mode": n}`
