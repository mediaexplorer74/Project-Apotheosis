# Project Apotheosis — EdgeHTML Reborn

> Портирование современного **WebKit/WebCore** (webkitgtk-2.52.4) на **Windows 10 Mobile (ARM32, UWP)**.
> Возвращение JIT-ускоренного, GPU-синтезированного веб-браузера на Lumia 950.

## Статус

### Реальное устройство (Lumia 950, Win10M 15254)

| Функция | Статус |
|---------|--------|
| WTF + JavaScriptCore CLoop | ✅ |
| WebCore + Cairo программный рендеринг | ✅ |
| Интерактивная сессия (клик, формы, скролл, клавиатура) | ✅ |
| JSC JIT (~5-50× ускорение) | ✅ |
| GPU-рендеринг (ANGLE D3D11 FL9.3 + TextureMapper) | ✅ |
| Плавный скроллинг / pinch-to-zoom | ✅ |
| Интерфейс браузера (вкладки, адресная строка, настройки) | ✅ |
| Многоязычный UI (en/ru/zh) | ✅ |

### x64 PC отладочная сборка (в процессе)

| Компонент | Статус |
|-----------|--------|
| Зависимости (vcpkg 16 пакетов, ICU, SQLite, ANGLE) | ✅ Установлены |
| WebKit CMake конфигурация | ✅ Первый успех (29 июня) |
| WTF + bmalloc компиляция | ✅ Собраны (12+ WK_WINUWP патчей) |
| PAL заголовки | ✅ Сгенерированы |
| GNU драйвер (clang++) для AT&T asm файлов | ✅ Оба файла (LowLevelInterpreter + MacroAssemblerX86_64) скомпилированы |
| JavaScriptCore → `bin/JavaScriptCore.dll` | 🔄 Компиляция ~8/111, только предупреждения |
| CMake 4.0 пропущенные правила | ✅ `patch-build-ninja-gnu.ps1` авто-сканер |
| WebCore → `bin/WebCore.dll` | ❌ Ожидает JSC |
| Драйвер → `WebCoreDriver-x64.dll` | ❌ |
| Harness.appx | ❌ |

## Архитектура

```
Harness (UWP C++/CX приложение)
   · SwapChainPanel ← GPU | WriteableBitmap ← SW запасной вариант
        │  C ABI (WebCoreDriver.h)
WebCoreDriver (порт-слой)
   · Управление страницами/фреймами, обработка событий
   · Cairo программный | TextureMapper GPU-рендеринг
        │
WebKit / WebCore / JSC / WTF
   · WK_WINUWP патчи для ARM32 UWP App Container
```

## Репозиторий

Этот репозиторий отслеживает только **порт-слой и хост**, не содержит исходники WebKit (гигабайтного размера).

```
Src/
├── port/        ← WebCore драйвер, заглушки, скрипты сборки, тулчейны
├── harness/     ← UWP хост-приложение (C++/CX, XAML)
├── tools/       ← WDP развёртывание и диагностика
├── angle/include/ ← ANGLE заголовки
└── setenv.ps1   ← Настройка окружения
Doc/             ← Документация (PLAN, Summary, Wiki на 3 языках)
```

## Сборка

```powershell
. .\Src\setenv.ps1
pwsh -File Src/port/link-driver-gpu.ps1        # ARM32
pwsh -File Src/port/build-harness.ps1           # Appx
pwsh -File Src/tools/deploy-launch.ps1 -Ip ...  # Развернуть на Lumia
```

Для x64 отладки:
```powershell
. .\Src\setenv.ps1
Set-Item -Path env:APOTHEOSIS_ARCH -Value x64
ninja -C build-x64-gpu JavaScriptCore WebCore
pwsh -File Src/port/link-driver-gpu-x64.ps1
```

## Благодарности

- [Jimmyxiao2009/Project-Apotheosis](https://github.com/Jimmyxiao2009/Project-Apotheosis) — оригинальный проект
- [Reddit: Портирование WebKitGTK 2.52.4 на Windows 10 Mobile](https://www.reddit.com/r/windowsphone/comments/1ugn2kn/porting_webkitgtk_2524_to_windows_10_mobile/)
- WebKitGTK команда — вышестоящий движок

## Лицензия

MIT (порт-слой); LGPL-2.1/BSD (WebKit и зависимости).

---

*Как есть. Без поддержки. Только для исследований. DIY.*
