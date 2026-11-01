Как пользоваться устройством: [README](https://github.com/TERMIN36/MeshCore/blob/v0.2.0-rc14/README.md).

Что изменилось в этой и прошлых версиях, с картинками экранов: [CHANGELOG](https://github.com/TERMIN36/MeshCore/blob/v0.2.0-rc14/CHANGELOG.md).

## Что нового в v0.2.0-rc14

Кандидат относительно `v1.17.1-0.1.4`: можно поставить и проверить. На заставке и в «о версии» — `t36-v0.2.0-rc14`.

### Автовыключение по батарее (opt-in)

Репитер / room / sensor: deep sleep по разряду **выкл. по умолчанию** — `set batt.shutdown on`. Веб **Питание** и prefs `batt_off_en` / `batt_off_mv`. Подробно: [CHANGELOG](https://github.com/TERMIN36/MeshCore/blob/v0.2.0-rc14/CHANGELOG.md#v020-rc14).

### 2S, калибровка, веб-панель

Шкала и `cal adc` для pack mV (T-Beam 1W). Блок **Питание**: Wi‑Fi sleep, АЦП, автовыключение. `help` / `?` в CLI.

В остальном поведение как в `v0.2.0-rc13`.
