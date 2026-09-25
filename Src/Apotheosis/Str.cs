using Windows.ApplicationModel.Resources;

namespace Apotheosis
{
    /// <summary>
    /// Dual-source string loader: tries .resw ResourceLoader first, falls back to hardcoded kStr table.
    /// Matches the C++ harness pattern (GetStr() with .resw + kStr[lang][id] fallback).
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
            HistoryCleared, BookmarksCleared, DownloadsCleared, Footer,
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
                "EdgeHTML Reborn / Apotheosis — WebKit (WebCore) 2.52.4 — ARM32 UWP",
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
                "EdgeHTML Reborn / Apotheosis — WebKit (WebCore) 2.52.4 — ARM32 UWP",
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
                "EdgeHTML Reborn / Apotheosis — WebKit (WebCore) 2.52.4 — ARM32 UWP",
            },
        };

        static ResourceLoader _loader;
        static int _langIndex = 0; // 0=en, 1=ru, 2=zh

        static Str()
        {
            try { _loader = ResourceLoader.GetForCurrentView(); } catch { }
        }

        public static void SetLang(int lang)
        {
            _langIndex = lang < 0 || lang > 2 ? 0 : lang;
        }

        public static string Get(string key)
        {
            // Try .resw first
            if (_loader != null)
            {
                try
                {
                    string val = _loader.GetString(key);
                    if (!string.IsNullOrEmpty(val)) return val;
                }
                catch { }
            }
            // Fallback to hardcoded
            if (System.Enum.TryParse<Id>(key, out var id) && _langIndex < kStr.Length)
            {
                int idx = (int)id;
                if (idx < kStr[_langIndex].Length) return kStr[_langIndex][idx];
            }
            return key;
        }
    }
}
