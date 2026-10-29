Как пользоваться устройством: [README](https://github.com/TERMIN36/MeshCore/blob/v0.2.0-rc13/README.md).

Что изменилось в этой и прошлых версиях, с картинками экранов: [CHANGELOG](https://github.com/TERMIN36/MeshCore/blob/v0.2.0-rc13/CHANGELOG.md).

## Что нового в v0.2.0-rc13

Кандидат относительно `v1.17.1-0.1.4`: можно поставить и проверить. На заставке и в «о версии» — `t36-v0.2.0-rc13`.

### Калибровка батареи (Companion)

`get/set adc.multiplier` в консоли Companion (чат **Console** на устройстве и экран **Консоль** в клиенте). `0.0` — множитель платы, иначе **0.0–10.0**. Подробно: [CHANGELOG](https://github.com/TERMIN36/MeshCore/blob/v0.2.0-rc13/CHANGELOG.md#v020-rc13).

### Калибровка по мультиметру

Команда **`cal adc <mV>`** (500–5000) на репитере, комнатном сервере, датчике и Companion — пересчёт `adc.multiplier` по замеру на ячейке. См. [cli_commands.md](https://github.com/TERMIN36/MeshCore/blob/v0.2.0-rc13/docs/cli_commands.md).

В остальном поведение как в `v0.2.0-rc12`.
