using Windows.ApplicationModel.Resources;

namespace Apotheosis
{
    /// <summary>
    /// UI strings. One table, three languages, chosen at runtime.
    ///
    /// Apotheosis 2026-10-03 -- the .resw is no longer consulted, and that is the fix, not a
    /// regression. This used to try ResourceLoader first and fall back to the table below, on
    /// the theory that x:Uid and .resw were the UWP-correct route. They are, for an app whose
    /// language is decided before launch. They are not for this one, for two measured reasons:
    ///
    ///   1. ResourceLoader.GetForCurrentView() is bound to the VIEW language (en-US here), so
    ///      it answered every key in English no matter what the user picked -- and it answered
    ///      for ~80 of the keys, because the .resw files use the same bare names as this table.
    ///      The Russian column was unreachable in practice, which is exactly the reported
    ///      symptom: "the language does not repaint the UI".
    ///   2. The XAML side was worse. x:Uid="SettingsTitle" looks up "SettingsTitle.Text" in the
    ///      .resw; the files define "SettingsTitle" with no property suffix, so the lookup missed
    ///      and the label rendered EMPTY. The settings page shipped with uncaptioned controls.
    ///
    /// So: the table is authoritative, SetLang is a live switch, and the XAML carries x:Name
    /// instead of x:Uid so RetranslateUi() in MainPage.xaml.cs can apply every label again
    /// without a restart. Add a key here and to all three columns; the arrays are positional
    /// and the enum is the index, so append -- never insert.
    /// </summary>
    static class Str
    {
        enum Id : int
        {
            AppTitle, Loading, Home, LoadFailed, LoadTimeout, Processing, Cancelled, RenderError,
            FindInPage, UrlPlaceholder, Back, Forward, Reload, Bookmark, BookmarkThisPage,
            NewTab, DesktopSite, MobileSite, FindInPageAction, Share, CopyLink, DownloadPage,
            Bookmarks, History, Downloads, Settings, Menu, Tabs, Done, GPU,
            DefaultSearchEngine, InterfaceLanguage, HomepageUrl, RequestDesktopSite,
            CustomUserAgent, DefaultZoom, ConcurrentTabs, EnableGPU, EnableGPUNow,
            ClearData, ClearHistory, ClearAllBookmarks, ClearDownloads,
            Diagnostics, ExportDebugLogs, AboutUpdate, CheckForUpdate, Version,
            HistoryCleared, BookmarksCleared, DownloadsCleared,
            General, Advanced, VerboseLogging,
            Footer,
            // Appended 2026-10-03 with the RetranslateUi() pass. The strings above this line were
            // partly written into the .resw files as bare names, which is what made that route
            // look like it worked; these have no .resw counterpart at all.
            Favorites, Bookmarked, BookmarkRemoved, RemoveBookmark, NothingToShare, LinkCopied,
            Clear, DownloadThisPage, NoBookmarks, NoHistory, NoDownloads,
            LoggingOnHint, LoggingOffHint, FrequentSites, HomeHint,
            On, Off,
            // Appended 2026-10-03 (second pass). Both target platforms have a real touchscreen, so
            // the on-screen scroll pair is clutter on the device it was written for; it needs a
            // switch. Appended, not inserted -- the arrays are positional.
            ShowScrollButtons,
            FitPageToWindow,
            AddressBarSuggestions,
            // Appended 2026-10-22 with the register-only Headless placeholder (ROADMAP.md,
            // "Beyond 1.0"). Append rule holds: enum last, all three columns same order.
            HeadlessSection,
            HeadlessHint,
            HeadlessSummary,
            HeadlessSpeak,
            COUNT
        }

        // 0 = en, 1 = ru, 2 = zh
        static readonly string[][] kStr = new string[][]
        {
            // English
            new string[] {
                "EdgeHTML Reborn", "Loading", "Home", "Load Failed", "Load Timeout", "Processing…", "Cancelled", "Render Error",
                "Find in page", "Search or enter URL", "Back", "Forward", "Reload", "Bookmark", "Bookmark This Page",
                "New Tab", "Desktop Site", "Mobile Site", "Find in Page", "Share", "Copy Link", "Download Page",
                "Bookmarks", "History", "Downloads", "Settings", "Menu", "Tabs", "Done", "GPU",
                "Default Search Engine", "Interface Language", "Homepage URL (leave empty for default)", "Request Desktop Site on Startup",
                "Custom User-Agent (empty = use toggle; reload to apply)", "Default Zoom", "Concurrent multi-engine tabs (placeholder)",
                "Enable GPU on Startup (auto after first page)", "Enable GPU Now (restart to revert)",
                "Clear Data", "Clear History", "Clear All Bookmarks", "Clear Downloads",
                "Diagnostics", "Export Debug Logs / Crash Dumps", "About / Update", "Check for Update (GitHub Releases)", "Version",
                "History Cleared", "Bookmarks Cleared", "Downloads Cleared",
                "General", "Advanced", "Verbose logging",
                "EdgeHTML Reborn / Apotheosis — WebKit (WebCore) 2.52.4 — ARM32 UWP",
                "Favorites", "Bookmarked", "Bookmark Removed", "Remove Bookmark", "Nothing to Share", "Link Copied",
                "Clear", "Download This Page", "No Bookmarks", "No History", "No Downloads",
                "setup-trace.txt is being written to LocalState.",
                "Tracing is off. LocalState files stop growing; setup-trace.txt will not record new entries.",
                "Frequent Sites", "Enter a URL in the address bar above.",
                "On", "Off",
                "On-screen scroll buttons",
                "Fit the page to the window",
                "Show suggestions from history and sites",
                "Headless mode (experimental)",
                "Placeholder: the choice is saved, but nothing is applied yet — see ROADMAP.md, Beyond 1.0.",
                "Summary",
                "Speak",
            },
            // Russian
            new string[] {
                "EdgeHTML Reborn", "Загрузка", "Главная", "Ошибка загрузки", "Превышено время загрузки", "Обработка…", "Отменено", "Ошибка рендеринга",
                "Поиск на странице", "Поиск или введите URL", "Назад", "Вперёд", "Обновить", "Закладка", "Добавить в закладки",
                "Новая вкладка", "Полная версия", "Мобильная версия", "Найти на странице", "Поделиться", "Копировать ссылку", "Скачать страницу",
                "Закладки", "История", "Загрузки", "Настройки", "Меню", "Вкладки", "Готово", "GPU",
                "Поисковая система по умолчанию", "Язык интерфейса", "Адрес домашней страницы (пусто = по умолчанию)", "Полная версия при запуске",
                "Пользовательский User-Agent (пусто = по переключателю; обновите страницу)", "Масштаб по умолчанию", "Мульти-вкладки (заглушка)",
                "Включить GPU при запуске (после первой страницы)", "Включить GPU сейчас (перезапуск отключит)",
                "Очистить данные", "Очистить историю", "Очистить все закладки", "Очистить загрузки",
                "Диагностика", "Экспорт логов / дампов", "О программе / Обновление", "Проверить обновления (GitHub)", "Версия",
                "История очищена", "Закладки очищены", "Загрузки очищены",
                "Основные", "Продвинутые", "Подробный лог",
                "EdgeHTML Reborn / Apotheosis — WebKit (WebCore) 2.52.4 — ARM32 UWP",
                "Избранное", "В закладках", "Закладка удалена", "Удалить закладку", "Нечего опубликовать", "Ссылка скопирована",
                "Очистить", "Скачать эту страницу", "Закладок нет", "История пуста", "Загрузок нет",
                "setup-trace.txt записывается в LocalState.",
                "Трассировка выключена: файлы LocalState не растут, setup-trace.txt не пополняется.",
                "Частые сайты", "Введите адрес в строке выше.",
                "Вкл.", "Откл.",
                "Экранные кнопки прокрутки",
                "Вписывать страницу в окно",
                "Подсказки из истории и сайтов",
                "Режим Headless (экспериментальный)",
                "Заглушка: выбор сохраняется, но пока ничего не применяется — см. ROADMAP.md, Beyond 1.0.",
                "Резюме",
                "Озвучка",
            },
            // Chinese
            new string[] {
                "EdgeHTML Reborn", "加载中", "主页", "加载失败", "加载超时", "处理中…", "已取消", "渲染错误",
                "页面内查找", "搜索或输入网址", "后退", "前进", "刷新", "书签", "添加书签",
                "新标签页", "桌面版网站", "移动版网站", "页面内查找", "分享", "复制链接", "下载页面",
                "书签", "历史记录", "下载", "设置", "菜单", "标签页", "完成", "GPU",
                "默认搜索引擎", "界面语言", "主页网址（留空为默认）", "启动时请求桌面版网站",
                "自定义 User-Agent（留空=使用开关；刷新后生效）", "默认缩放", "多标签页（占位符）",
                "启动时启用 GPU（首次打开页面后自动）", "立即启用 GPU（重启后恢复）",
                "清除数据", "清除历史记录", "清除所有书签", "清除下载",
                "诊断", "导出调试日志 / 崩溃转储", "关于 / 更新", "检查更新（GitHub Releases）", "版本",
                "历史记录已清除", "书签已清除", "下载已清除",
                "常规", "高级", "详细日志",
                "EdgeHTML Reborn / Apotheosis — WebKit (WebCore) 2.52.4 — ARM32 UWP",
                "收藏夹", "已加书签", "书签已删除", "删除书签", "无可分享", "链接已复制",
                "清除", "下载此页面", "没有书签", "没有历史记录", "没有下载",
                "setup-trace.txt 正在写入 LocalState。",
                "跟踪已关闭：LocalState 文件停止增长，setup-trace.txt 不再记录新条目。",
                "常用网站", "请在上方地址栏输入网址。",
                "开", "关",
                "屏幕滚动按钮",
                "页面适配窗口",
                "显示来自历史和网站的联想",
                "无头模式(实验性)",
                "占位符:选择会被保存,但暂不生效——见 ROADMAP.md,Beyond 1.0。",
                "摘要",
                "朗读",
            },
        };

        static int _langIndex = 0; // 0=en, 1=ru, 2=zh

        public static void SetLang(int lang)
        {
            _langIndex = lang < 0 || lang > 2 ? 0 : lang;
        }

        public static int Lang => _langIndex;

        public static string Get(string key)
        {
            if (System.Enum.TryParse<Id>(key, out var id))
            {
                int idx = (int)id;
                if (idx < kStr[_langIndex].Length) return kStr[_langIndex][idx];
            }
            // Not a key this table knows. Returning the key itself keeps the gap visible on
            // screen instead of painting an empty label, which is how the x:Uid defect hid.
            return key;
        }
    }
}
