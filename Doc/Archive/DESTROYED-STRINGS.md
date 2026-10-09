# Destroyed string literals — the `?` bytes

**Status:** classified and repaired 2026-09-18 (`0.1.10.0`). Nothing is open.

## What happened

Two files in the port layer carry string literals whose non-ASCII text has been replaced by literal
`?` bytes (`0x3F`). This is **not** a misdecoded-encoding problem and it is **not** mojibake: the bytes
are gone, so there is nothing left to recover. Hexdump, `Src/harness/MainPage.xaml.cpp`, the error-page
heading before the repair:

```
$ awk 'NR==1157' Src/harness/MainPage.xaml.cpp | od -c
... f o n t - s i z e : 3 8 p x ' > ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? <
```

It is also **not a file-wide encoding failure**, which is the fact that bounds the work. The same file
is genuine UTF-8 with working Cyrillic — the Russian row of the three-language string table
(`MainPage.xaml.cpp:1759`) hexdumps as `320 235 320 260 321 201 …` = `Настройки, Язык, …` and renders
correctly on screen. So whatever destroyed these literals touched **individual lines**, not the file:
the shape of an edit made through an ASCII-only pipeline (a PowerShell/sed round-trip, or an agent
rewriting a line with a writer that was not encoding-aware). That is why the audit below is a list of
nine literals and not a file-rewrite.

## The audit

Method: for both files, every line containing `???` was taken and split into "inside a string literal"
versus "inside a comment"; comment lines were then read by eye, because a quoted fragment inside a
comment (for example `"Найти"`) trips a naive test.

| File | Literal lines | Comment lines |
|---|---|---|
| `Src/harness/MainPage.xaml.cpp` | **10** | 205 |
| `Src/port/WebCoreDriver.cpp` | **1** | 281 |

The comment lines are upstream/fork heritage — Chinese in the driver, Russian and Chinese in the
harness — and per `CLAUDE.md` they are left alone except where a specific one is being edited. Only the
literals are user-visible, and only nine of the ten in the harness are text at all.

### The nine user-visible literals

| Line | Where it appears | What was there |
|---|---|---|
| `MainPage.xaml.cpp:1142` | built-in home page, tagline | 21 chars before `&middot; Windows 10 Mobile &middot; ARM32` |
| `MainPage.xaml.cpp:1144` | home page, headline paragraph | 45 chars |
| `MainPage.xaml.cpp:1146` | home page, section label | 12 chars |
| `MainPage.xaml.cpp:1147` | home page, capability list | 9 and 12 chars |
| `MainPage.xaml.cpp:1149` | home page, "try these" label | 6 chars |
| `MainPage.xaml.cpp:1157` | **error page heading** (`MakeErrorHtml`) | 21 chars |
| `MainPage.xaml.cpp:5592` | tab-switcher title | 6 chars, Russian |

The tenth is `MainPage.xaml.cpp:1432`, `return L"???";` — not destroyed text at all: it is the
deliberate "unknown module" placeholder in the fault-module resolver, and three `?` is what it always
meant. It is listed here so the next audit does not re-open it.

## How each was repaired

Because the originals are unrecoverable, these are **rewrites, not restorations**, and they are English
— matching the default `m_uiLang` row and the project's English-artifact rule.

- The six home-page and error-page literals now carry plain English text. The capability list was
  rewritten to something true of this engine rather than a guess at the old words:
  `HTTPS · TLS 1.3 · JavaScript · Web Storage · GPU compositing`. The error heading is
  `Could not load the page`.
- `5592` did not need a literal at all: `S_TAB_SWITCHER_TITLE` is the table entry that was already
  meant for it, and it is the right fix regardless of encoding, because a hardcoded Russian heading in a
  three-language UI is a bug on its own. It now reads `GetStr(m_uiLang, S_TAB_SWITCHER_TITLE)`.
- `WebCoreDriver.cpp:3566`, the one literal in the driver, is inside the scroll-probe dump and its
  parenthetical was mangled Chinese; it now reads `(expected 0,300)`, which is what the probe measures.

## What this does not close

**The home page and the error page are not localized.** They are built by `MakeHomeHtml` /
`MakeErrorHtml` as static `const char*` HTML, with no access to `m_uiLang`, so they will be English
whatever the user picked in Settings. Routing them through `GetStr` is the correct fix and is
deliberately not attempted here — it needs new string-table entries in three languages and a decision
about the two language names that appear as `Content="Русский"` / `Content="中文"` in the XAML. Left
as a known gap; the encoding repair above does not make it worse.

**The 486 mangled comment lines remain.** They are inert, and the rule that permits removing them
applies to them alone.
