# Project Apotheosis — EdgeHTML Reborn (RU)

> **Сначала прочитайте:** [Актуальный статус — 2026-09-17](Doc/STATUS-2026-09-17.md).
> Августовское введение и таблицы ниже — исторические записи, не проверка версии .93.


> Перенос современного **WebKit/WebCore** (webkitgtk-2.52.4) на **Windows 10 Mobile (ARM32, UWP)**.
> Возвращаем JIT-ускоренный и GPU-композитируемый веб-браузинг на Lumia 950.
> **[Авг 2026 — линия x64-uwp: appx v0.1.8.51 запускается на Win11; рендерится реальный текст, включая CJK; зависание CJK устранено (баг пустого сентинела hash-таблицы); весь набор из 8 тестов проходит; GPU-растяжение всего контента УСТРАНЕНО через `WebCoreGpuResize` (поверхность теперь следует за ContentArea — проверено `contents=1024x694`, квадраты 200×200 остаются квадратами). GitHub/CI — ОПЦИОНАЛЬНО; вся ARM32-разработка/сборка полностью локальна на dev-машине. Далее: максимальная синхронизация ARM32↔x64 (одинаковое дерево файлов / имена скриптов / общая версия appx), затем пересборка ARM32 для Lumia 950.](**)**

> **25 августа 2026 — lightweight mode работает на телефоне.** ya.ru загружается быстро и стабильно
> (JS отключён в настройках). example.com и news.ycombinator.com рендерятся полностью.
> Причина мгновенной смерти была найдена бисекцией: три наложенных дефекта -- синхронный curl
> на движковом треде при пропадании радио, пересборка CA-store при каждой загрузке, и JIT-исполнение
> внутри AppContainer. Все три исправлены. Полный JS требует дальнейшего исследования JSC.
> См. Doc/PUMPLOOP-SILENT-DEATH.md.

## Текущий статус — 2026-09-17

> **17 сентября 2026 — статус x64.** `0.1.9.93` (Release x64) собрана и установлена **на месте**
> (существующий пакет обновлён через `Add-AppxPackage`, LocalState сохранён; это была не чистая
> пересборка). Первые простые загрузки вернули rc=0 — **это не визуальное доказательство**.
> GPU-readback показал `makeCurrent=0` (проигнорирован драйвером), `GL_VERSION` NULL, `fbo=0
> tex=0`, но при этом «успех»; GPU-рендеринг **не валидирован**. Позже произошёл краш
> **0xc0000005 по адресу WebCore+0x204901f** — символизация (только адрес, не стек) указала на
> **обращение к std::variant в CSS StyleCalculationTree**; это **НЕ** доказанная причина JSC/EGL.
> **Дамп краша отсутствует.** План: fail-closed обработка ошибок GPU, сбор живых дампов,
> независимые тесты рендеринга/JS; ARM32 позже; Git — по желанию владельца. ⚠️ Текущий
> `x64-cycle.ps1` удаляет установленный пакет вопреки своему заголовку — не использовать для
> обновления с сохранением данных. Подробности: [STATUS-2026-09-17.md](Doc/STATUS-2026-09-17.md).

Текущая конфигурация: ICU 78 из vcpkg; SDK 19041 для harness / 26100 для движка x64;
MSVC 14.44. Сегодня не было Debug-сборки harness или чистой пересборки. Исправления .94
не проверены; результаты CDB-сеанса родительской сессии ещё не включены в этот документ.

## Исторические записи — август 2026 и ранее

Старые утверждения сохранены, но не перепроверены для .93. Прежние выводы о JSC/JIT
и отметки GPU не доказывают причину нового краша или корректный вывод изображения.



---

## Статус

### Реальное устройство (Lumia 950, Win10M 15254) — проверено ранее

| Возможность | Статус |
|-------------|--------|
| WTF + JavaScriptCore CLoop | ✅ |
| WebCore + программный рендер Cairo | ✅ |
| Живая интерактивная сессия (клик, формы, скролл, клавиатура) | ✅ |
| JSC JIT (~5-50× ускорение) | ✅ |
| GPU-композитинг (ANGLE D3D11 FL9.3 + TextureMapper) | ✅ |
| Плавный скролл / pinch-zoom | ✅ |
| Оболочка браузера (вкладки, адресная строка, настройки) | ✅ |
| Многоязычный UI (en/ru/zh) | ✅ |

### x64-uwp Debug Build (Авг 2026)

| Компонент | Статус |
|-----------|--------|
| CMake configure WebKit (x64-uwp, GPU + JIT + FTL_JIT) | ✅ |
| WTF + bmalloc + PAL + JavaScriptCore + WebCore | ✅ |
| Драйвер порта (`WebCoreDriver-gpu.lib`) | ✅ |
| **Harness appx** (manifest `0.1.8.51`) | ✅ **собирается, регистрируется, запускается в AppContainer Win11**; грузит реальные страницы — `example.com` → `rc=0`, `title=Example Domain` (см. лог) |
| **Активация XAML** | ✅ **починена: обход XBF, code-only UI fallback** |
| **Загрузка DLL движка + инициализация JSC + `Page::create` + первый проход стилей** | ✅ |
| **Рендер текста на x64** | ✅ `about:home` рисует сглаженные глифы (краш шрифта Tier-3 починен 10.08, маршрутизация complex-text-path — 11.08) |
| **Текст CJK** | ✅ **зависание найдено и устранено (16.08)** — бесконечный цикл `FontDataCacheKeyTraits::emptyValue` (hash-сентинел); системный фолбэк simhei через принудительное бандл-лицо; 8/8 тестов проходят |
| **Реальные сайты** | ✅ `example.com` грузится (`rc=0`); лестница `file://` → `http(s)://` → крупные сайты в работе |
| **GPU-композитинг (ANGLE D3D11 FL9.3)** | ✅ **рендер без искажений** — растяжение контента УСТРАНЕНО 16.08 через `WebCoreGpuResize` (surface пересоздаётся под размер ContentArea на движковом потоке; проверено `contents=1024x694`, многократные ресайзы `rc=0`, без краша). Валидация на реальном устройстве (ARM32) ожидается |
| **Тулчейн ARM32** | 🔓 разблокирован — установить `Microsoft.VisualStudio.Component.VC.14.38.17.8.ARM` (каталог); вся ARM32-разработка/сборка **полностью локальна** на dev-машине |

> **Восстановление тулчейна (14.08.2026)**: после переустановки ОС линия x64-uwp пересобрана с нуля
> (LLVM 22.1.8 / CMake 4.0.1 / Ninja / Python 3.12 / vcpkg 13 портов / ICU75 import libs / harfbuzz /
> Ruby 3.4.10 + MSYS2 / perl / ccache). DLL движка пересобираются
> `ninja -C build-x64-gpu WebCore -j1` (задача `ApotheosisNinja`). Подробности в `Doc/Summary.md`.

### Ключевые прорывы (июль–авг 2026)

1. **Исправление BCryptGenRandom NTSTATUS** — инверсия логики роняла инициализацию JSC при успехе. Исправлено в `RandomDevice.cpp`.
2. **Обход активации XAML** — пропуск `Frame::Navigate`/`LoadComponent` (null type-provider в AppContainer); прямое создание страницы.
3. **WebCoreRenderHtml SW-конвейер (июль)** — HTML→layout→Cairo→bitmap→экран проверен на x64.
4. **Восстановление тулчейна (авг)** — после переустановки Windows восстановлена линия x64-uwp (`reinstall-env.ps1`, ICU75 import libs, harfbuzz import lib, fontconfig убран — 0 ссылок). Полная сборка зелёная.
5. **Запуск appx v0.1.8.25** — инициализация движка проходит XAML/JSC/`Page::create`; краш локализован в разрешении шрифтов.
6. **CJK-зависание найдено и устранено (16.08)** — «странная высота шрифта / CJK tofu» оказалось бесконечным циклом в `FontDataCacheKeyTraits::emptyValue()`: ctor `FontPlatformData` в WK_WINUWP давал size-0 сентинелу живой `m_scaledFont`, поэтому memset-0 слот никогда не сравнивался с ним и линейный пробинг крутился вечно. Исправлено: `m_scaledFont` остаётся null для size 0 (`if (size)`, `FontPlatformData.cpp:182`). CJK теперь резолвится через принудительное бандл-лицо simhei (`stubs-font-uwp.cpp` + `apotheosisSetForcedBundledFontName`). Весь набор из 8 тестов проходит (single/mixed/wan-meta/kana-n/br-kana/mixed-e/br-euro/cjk-b — все ALIVE, `gdc-> c=U+4E00 g=1078 ok=1`).
7. **GPU-растяжение всего контента (УСТРАНЕНО 16.08)** — `WebCoreGpuInit` создавал swapchain ANGLE один раз в 720×1080, и GPU-режим его никогда не ресайзил; `SwapChainPanel` растягивал его до ContentArea, и любое несовпадение пропорций делало весь контент шире. Исправлено через `WebCoreGpuResize` (`WebCoreDriver.cpp`): harness постит resize-джобу (свежий `PropertySet` + `EGLRenderSurfaceSizeProperty`), которая пересоздаёт GLContext/surface + TextureMapper на движковом потоке и затем `finishInteractionPaint`. Проверено на x64: `contents=1024x694` после ресайза, rc=0 на множестве живых циклов, квадраты 200×200 остаются квадратами. Шрифты всегда были корректны.

---

## Архитектура

```
Harness (UWP C++/CX App)
   · SwapChainPanel ← GPU | WriteableBitmap ← SW fallback
        │  C ABI (WebCoreDriver.h)
WebCoreDriver (порт-слой)
   · Управление Page/frame, диспетчеризация событий
   · Cairo (программный) | TextureMapper (GPU)
        │
WebKit / WebCore / JSC / WTF
   · WK_WINUWP-патчи для ARM32 UWP App Container
```

## Структура репозитория

```
Src/
├── port/         ← драйвер WebCore, stubs, скрипты сборки, тулчейны
├── harness/      ← UWP host app (C++/CX, XAML)
├── tools/        ← WDP deploy + диагностика
├── angle/include/← заголовки ANGLE
├── Apotheosis/   ← альтернативная сборка (C#, эксперимент)
├── setenv.ps1    ← настройка окружения
Doc/              ← архитектура, планы, заметки по передаче
```

## Быстрая сборка (x64-gpu, после изменения кода)

```powershell
. .\Src\setenv.ps1
msbuild Src/harness/Harness.vcxproj /p:Configuration=Release /p:Platform=x64 /p:AppxPackage=true /t:Build /v:minimal
```

Если нужна пересборка DLL движка (изменение в WebKit):

```powershell
. .\Src\setenv.ps1
ninja -C build-x64-gpu WebCore -j1
# Затем пересборка harness (выше)
```

### Деплой и запуск

```powershell
# Установка AppX
PowerShell -ExecutionPolicy Bypass -File "Src\harness\AppPackages\Harness\Harness_0.1.8.51_x64_Test\Add-AppDevPackage.ps1"
# Запуск
cmd /c start shell:AppsFolder\EdgeHTMLReborn.Harness_edmb40rfkwsbg!App
# Лог
Get-Content "$env:LOCALAPPDATA\Packages\EdgeHTMLReborn.Harness_edmb40rfkwsbg\LocalState\log.txt"
```

> **ВАЖНО:** 4 ГБ RAM — перезагружайтесь перед крупными сборками. Только `ninja -j1`. Файл порта `WebCoreDriver.cpp` может выдать `fatal error C1060` при нехватке памяти.

---

## Присоединяйтесь!

**Apotheosis — это возрождение экосистемы Windows 10 Mobile.** Если у вас есть опыт в C++/CX, WebKit, DirectX, ARM32 кросс-компиляции или просто энтузиазм — вы нам нужны!

- **Нам нужны**: C++ разработчики, тестировщики (особенно с живыми Lumia 950/950 XL), UI/UX дизайнеры, специалисты по WebKit
- **Что делать**: чинить баги, тестировать сборки, документировать, портировать
- **Как помочь**: форкните репозиторий, шлите PR, открывайте issues, пишите на Reddit (r/windowsphone)

---

## Roadmap

> **Политика (авг 2026)**: все задачи, связанные с GitHub (CI, GitHub Actions, авторелизы, PR-конвейеры,
> раннеры) — **ОПЦИОНАЛЬНЫ / некритичны**. Все ARM32-задачи разработки/сборки — **полностью локальны**
> на dev-машине (без облачных раннеров). Порядок: локальная проверка x64 → локальные ARM32 артефакты →
> ручная валидация на реальном устройстве → затем (опционально) любая CI/GitHub-автоматизация.

- **Синхронизация ARM32↔x64** (ВЫПОЛНЕНО 16.08): каталоги ARM32 переименованы `build-clang-*` → `build-arm32-*` (соответствует `build-x64-gpu` = `build-<arch>-<variant>`); суффиксы obj унифицированы (GPU `.arm32.obj` / JIT `.arm32-jit.obj` / soft `.arm32-soft.obj`, x64 `.x64.obj`); `recompile-stubs-arm32.ps1` → `recompile-stubs-x64.ps1`. Обе архитектуры собираются из ОДНОГО `Package.appxmanifest` → **одинаковая версия appx** для x64 и ARM32.
- **Локальный x64** (текущее): GPU-растяжение УСТРАНЕНО (`WebCoreGpuResize`). Продолжать лестницу сайтов (Bing/GitHub/Apple), интерактив Enter/SPA/docs; подчистить диагностику harness.
- **Локальный ARM32**: полная пересборка (reconfigure `build-arm32-gpu` + многочасовая `ninja -j1` — старое дерево ARM32 стёрто переустановкой ОС) → линковка драйвера → ARM32 appx (та же версия). Разница в ширине экрана Lumia неважна: поверхность всегда равна ContentArea, любой аспект рендерится без искажений.
- **Реальное устройство (ручное)**: деплой ARM32 appx на Lumia 950 через `Deploy-Robust.ps1` + WDP; проверить ту же лестницу сайтов + GPU.
- **(Опционально) GitHub CI** — `windows-latest` (7 ГБ RAM) собирает x64 быстрее 4 ГБ dev-бокса (`ninja -j8` ≈ 40 мин на сборку движка); авторелизы; fork→PR→автосборка. Только после того, как локальные линии зелёные.

---

## Авторы (Credits)

- [Jimmyxiao2009/Project-Apotheosis](https://github.com/Jimmyxiao2009/Project-Apotheosis) — оригинальный проект
- [Reddit: Porting WebKitGTK 2.52.4 to Windows 10 Mobile](https://www.reddit.com/r/windowsphone/comments/1ugn2kn/porting_webkitgtk_2524_to_windows_10_mobile/)
- Команда WebKitGTK — upstream-движок

## Лицензия

MIT (порт-слой); LGPL-2.1/BSD (upstream WebKit + зависимости).

---

*As is. No support. RnD only. DIY.*
[RU] Авг 16 2026

---

### ИИ-агенты

- **opencode** — https://opencode.ai · модель `deepseek-v4-flash-free` (opencode/deepseek-v4-flash-free) · авг 2026
- **Claude Opus 5** — *(добавится позже)*
