# Handoff — 2026-09-17 вечер (сессия прервана лимитом модели)

## Состояние на момент остановки (все факты проверены)

- Установлена **0.1.9.96** (in-place, LocalState сохранён). Engine WebCore.dll содержит
  calc-диагностику `[APO calc] missing CalculationValue` (SHA256 569B06E3...),
  Harness.exe содержит оба GL-guard'а (SHA256 BBFEA59C...). AppX-проверка пройдена
  (appx-096-verification.txt).
- **Первый полный user-mode dump в истории проекта сохранён**:
  `crash\x64-acceptance-0.1.9.95-20260917-181409\capture-20260917-193915\dzen-diag.dmp` (835 MB).
  ВАЖНО: дамп поймал AV в `Harness!ApoReadModuleName` (watchdog, из-за бага PE32/PE32+),
  а НЕ CSS-краш. Использовать как причину CSS-краша нельзя.
- .95 GL fail-closed guard подтверждён в работе: 78 fallback-сообщений, 0 readPixels,
  Cairo рендерил (nonWhite 69828/219610), **пользователь ВИДЕЛ контент dzen.ru**.
- **CSS-краш 0xc0000005 @ WebCore.dll+0x204901f НЕ исправлен** и НЕ доказан:
  гипотеза ValueMap (stale handle) остаётся гипотезой; guard .96 стоит только в
  nonNanCalculatedValue(ZoomFactor), а стек .95 идёт через ZoomNeeded (не покрыт).

## Сделано в последний час (проверено)

1. **Найден и исправлен реальный баг диагностики**: `ApoReadModuleName` читала
   export-directory по PE32-смещению 96 даже на x64 PE32+ (нужно 112) без границ —
   источник AV в watchdog под отладчиком.
2. Новый заголовок `Src\harness\MappedPeName.h` — безопасный парсер PE32/PE32+
   (проверки границ, формат по magic, SEH остаётся снаружи). Использован в
   `ApoReadModuleName(base, mappedSize, out, cb)`; вызов в `ApoBuildImageList`
   передаёт размер образа.
3. **Тесты: PASS 2063 checks** (PE32+PE32+, усечение 1024 вариантов, битые заголовки):
   `crash\pe-name-validation-20260917-201037\` (MappedPeNameTests.cpp → PASS).
4. Harness пересобран без движка: **SUCCESS**, appx .96 пересоздан 20:1x.
   Манифест остался 0.1.9.96 — **при установке использовать backup/uuid или бампнуть .97**.

## Управление навигацией (уточнено, не путать)
- `Assets\navseq.txt` (в пакете) — автоплей, `enabled=1/0` ВНУТРИ файла; копия в
  LocalState\navseq-active.txt (это выход, НЕ переключатель).
- `LocalState\nav.txt` — одна навигация по изменению файла (navwatch, опрос 1 с).
- `settings.ini`: ua=0 — мобильный iPhone UA (по умолчанию), ua=1 — desktop Edge UA
  (переключатель больших белых полей мобильной раскладки — проверять отдельно!).

## Следующие шаги (по приоритету)
1. Бампнуть версию до 0.1.9.97, установить свежий appx in-place (LocalState бэкап),
   проверить хеши установленного EXE (новый Harness.exe содержит фикс PE-парсера).
2. Прогнать navseq .97: если краш 0x204901f повторится — теперь watchdog НЕ должен
   умирать раньше; ловить AV cdb-attach (sxi bpe + sxe av + .dump /ma) или ждать WER.
3. Если краш воспроизведётся с новым стеком — символизация по локальному PDB;
   сравнить путь с .93/.95 (anim-extent vs style-change).
4. Отдельно (не смешивать): desktop-UA тест (`ua=1`) для белых полей; ZoomNeeded-логи
   calc-пути — только после устойчивого воспроизведения.
5. Обновить Doc/STATUS-2026-09-17.md: .96 build success, PE-parser fix + 2063 теста,
   дамп-захват работает, CSS-краш всё ещё открыт.

## Не утверждать как факты
- «Guard починил краш» — нет данных; .96 в браузерном сценарии ещё не гонялась.
- Причина 0x204901f — не установлена; ValueMap-гипотеза не подтверждена и не опровергнута.
