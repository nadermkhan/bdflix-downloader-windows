// BDFlixWin32.cpp - Compiled with MSVC C++20
// Compile: cl.exe /EHsc /std:c++20 /W4 BDFlixWin32.cpp /link user32.lib gdi32.lib comctl32.lib dwmapi.lib winhttp.lib

// --- Single File Manifest & Library Injections ---
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winhttp.lib")

#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <winhttp.h>
#include <wil/resource.h>
#include <wil/result.h>
#include <wil/com.h>

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <fstream>
#include <format>
#include <future>

// --- Windows 11 Fluent UI / DWM Constants ---
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif

enum DWM_SYSTEMBACKDROP_TYPE {
    DWMSBT_AUTO = 0,
    DWMSBT_NONE = 1,
    DWMSBT_MAINWINDOW = 2, // Mica
    DWMSBT_TRANSIENTWINDOW = 3, // Acrylic
    DWMSBT_TABBEDWINDOW = 4 // Mica Alt
};

// --- Custom WIL Wrappers for WinHTTP ---
namespace wil {
    using unique_hinternet = wil::unique_any<HINTERNET, decltype(&::WinHttpCloseHandle), ::WinHttpCloseHandle>;
}

// --- IDM-Level Segmented Download Engine ---
class SegmentedDownloader {
private:
    struct Segment {
        uint64_t start;
        uint64_t end;
        std::atomic<uint64_t> downloaded{0};
        std::atomic<bool> completed{false};
    };

    std::wstring m_url;
    std::wstring m_savePath;
    uint64_t m_totalSize = 0;
    int m_threadCount = 4; // Parallel connections
    std::vector<Segment> m_segments;
    std::vector<std::thread> m_workers;
    std::atomic<bool> m_isPaused{ false };
    std::atomic<bool> m_isCancelled{ false };

public:
    SegmentedDownloader(std::wstring url, std::wstring savePath, int threads = 4)
        : m_url(std::move(url)), m_savePath(std::move(savePath)), m_threadCount(threads) {}

    ~SegmentedDownloader() {
        Cancel();
        for (auto& t : m_workers) {
            if (t.joinable()) t.join();
        }
    }

    void Start() {
        std::thread([this]() {
            InitializeDownload();
        }).detach();
    }

    void Cancel() {
        m_isCancelled = true;
    }

    double GetProgress() const {
        if (m_totalSize == 0) return 0.0;
        uint64_t downloaded = 0;
        for (const auto& seg : m_segments) {
            downloaded += seg.downloaded;
        }
        return static_cast<double>(downloaded) / m_totalSize;
    }

private:
    void InitializeDownload() {
        // 1. Crack URL & Get File Size
        URL_COMPONENTS urlComp = { sizeof(urlComp) };
        wchar_t hostName[256];
        wchar_t urlPath[1024];
        urlComp.lpszHostName = hostName;
        urlComp.dwHostNameLength = ARRAYSIZE(hostName);
        urlComp.lpszUrlPath = urlPath;
        urlComp.dwUrlPathLength = ARRAYSIZE(urlPath);
        
        THROW_IF_WIN32_BOOL_FALSE(WinHttpCrackUrl(m_url.c_str(), 0, 0, &urlComp));

        wil::unique_hinternet hSession(WinHttpOpen(L"BDFlixWin32/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        wil::unique_hinternet hConnect(WinHttpConnect(hSession.get(), hostName, urlComp.nPort, 0));
        wil::unique_hinternet hRequest(WinHttpOpenRequest(hConnect.get(), L"HEAD", urlPath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, urlComp.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));

        THROW_IF_WIN32_BOOL_FALSE(WinHttpSendRequest(hRequest.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0));
        THROW_IF_WIN32_BOOL_FALSE(WinHttpReceiveResponse(hRequest.get(), NULL));

        wchar_t contentLength[32];
        DWORD size = sizeof(contentLength);
        if (WinHttpQueryHeaders(hRequest.get(), WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, contentLength, &size, WINHTTP_NO_HEADER_INDEX)) {
            m_totalSize = _wtoll(contentLength);
        }

        if (m_totalSize == 0) return; // Fallback to single thread stream not implemented in this snippet

        // 2. Pre-allocate file
        std::ofstream outfile(m_savePath, std::ios::binary);
        outfile.seekp(m_totalSize - 1);
        outfile.write("", 1);
        outfile.close();

        // 3. Partition Segments
        m_segments.resize(m_threadCount);
        uint64_t partSize = m_totalSize / m_threadCount;
        for (int i = 0; i < m_threadCount; ++i) {
            m_segments[i].start = i * partSize;
            m_segments[i].end = (i == m_threadCount - 1) ? m_totalSize - 1 : (i + 1) * partSize - 1;
        }

        // 4. Spin up workers
        for (int i = 0; i < m_threadCount; ++i) {
            m_workers.emplace_back(&SegmentedDownloader::DownloadSegmentWorker, this, i, hostName, urlPath, urlComp.nPort, urlComp.nScheme);
        }
    }

    void DownloadSegmentWorker(int index, const std::wstring& host, const std::wstring& path, INTERNET_PORT port, INTERNET_SCHEME scheme) {
        auto& seg = m_segments[index];
        wil::unique_hinternet hSession(WinHttpOpen(L"BDFlixWin32/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        wil::unique_hinternet hConnect(WinHttpConnect(hSession.get(), host.c_str(), port, 0));
        wil::unique_hinternet hRequest(WinHttpOpenRequest(hConnect.get(), L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, scheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));

        std::wstring rangeHeader = std::format(L"Range: bytes={}-{}", seg.start, seg.end);
        WinHttpAddRequestHeaders(hRequest.get(), rangeHeader.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
        
        if (!WinHttpSendRequest(hRequest.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) return;
        if (!WinHttpReceiveResponse(hRequest.get(), NULL)) return;

        std::fstream file(m_savePath, std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(seg.start);

        DWORD bytesAvailable = 0;
        DWORD bytesRead = 0;
        std::vector<char> buffer(8192);

        while (!m_isCancelled && !m_isPaused) {
            if (!WinHttpQueryDataAvailable(hRequest.get(), &bytesAvailable) || bytesAvailable == 0) break;
            if (WinHttpReadData(hRequest.get(), buffer.data(), std::min<DWORD>(bytesAvailable, (DWORD)buffer.size()), &bytesRead) && bytesRead > 0) {
                file.write(buffer.data(), bytesRead);
                seg.downloaded += bytesRead;
            } else {
                break;
            }
        }
        
        if (seg.downloaded == (seg.end - seg.start + 1)) {
            seg.completed = true;
        }
    }
};

// --- UI MainWindow ---
class MainWindow {
private:
    HWND m_hwnd = nullptr;
    HWND m_hListView = nullptr;
    HWND m_hProgressBar = nullptr;
    std::unique_ptr<SegmentedDownloader> m_activeDownload;

public:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        MainWindow* pThis = nullptr;
        if (uMsg == WM_NCCREATE) {
            CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
            pThis = reinterpret_cast<MainWindow*>(pCreate->lpCreateParams);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
            pThis->m_hwnd = hwnd;
        } else {
            pThis = reinterpret_cast<MainWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        }

        if (pThis) {
            return pThis->HandleMessage(uMsg, wParam, lParam);
        } else {
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
        }
    }

    void Create(HINSTANCE hInstance) {
        INITCOMMONCONTROLSEX icex = { sizeof(INITCOMMONCONTROLSEX), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS };
        InitCommonControlsEx(&icex);

        WNDCLASS wc = {0};
        wc.lpfnWndProc = MainWindow::WndProc;
        wc.hInstance = hInstance;
        wc.lpszClassName = L"BDFlixFluentClass";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        // Neutral background to let Mica shine through
        wc.hbrBackground = CreateSolidBrush(RGB(32, 32, 32)); 

        RegisterClass(&wc);

        m_hwnd = CreateWindowEx(
            0, L"BDFlixFluentClass", L"BDFlix Win32 Downloader",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
            nullptr, nullptr, hInstance, this
        );

        ApplyFluentDesign();

        // Setup UI
        m_hListView = CreateWindowEx(0, WC_LISTVIEW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
            20, 20, 740, 450, m_hwnd, nullptr, hInstance, nullptr);
            
        ListView_SetExtendedListViewStyle(m_hListView, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        
        LVCOLUMN lvc = {0};
        lvc.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
        lvc.pszText = (LPWSTR)L"Filename";
        lvc.cx = 400;
        ListView_InsertColumn(m_hListView, 0, &lvc);
        
        lvc.pszText = (LPWSTR)L"Status";
        lvc.cx = 200;
        ListView_InsertColumn(m_hListView, 1, &lvc);

        m_hProgressBar = CreateWindowEx(0, PROGRESS_CLASS, nullptr,
            WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            20, 490, 740, 20, m_hwnd, nullptr, hInstance, nullptr);

        // Dummy Data (Mocking the SearchEngine results from iOS)
        InsertMockItem(0, L"Avengers.Endgame.2019.1080p.mkv", L"Queued");
        InsertMockItem(1, L"Cyberpunk_Edgerunners_S01.zip", L"Queued");

        ShowWindow(m_hwnd, SW_SHOWDEFAULT);
        
        // Mock Starting a download
        StartMockDownload(L"http://172.16.50.7/DHAKA-FLIX-7/Avengers.Endgame.mkv", L"C:\\Temp\\Avengers.mkv");
        SetTimer(m_hwnd, 1, 100, nullptr); // Progress UI Update Timer
    }

private:
    void ApplyFluentDesign() {
        // 1. Enable Immersive Dark Mode
        BOOL isDarkMode = TRUE;
        DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &isDarkMode, sizeof(isDarkMode));

        // 2. Apply Mica Backdrop (Fluent Design)
        DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_MAINWINDOW;
        DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));

        // 3. Rounded Corners (Win11)
        int cornerPref = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
        
        // Extend client area into title bar for true Fluent feel
        MARGINS margins = {-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(m_hwnd, &margins);
    }

    void InsertMockItem(int index, const wchar_t* name, const wchar_t* status) {
        LVITEM lvi = {0};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = index;
        lvi.pszText = (LPWSTR)name;
        ListView_InsertItem(m_hListView, &lvi);
        ListView_SetItemText(m_hListView, index, 1, (LPWSTR)status);
    }

    void StartMockDownload(const std::wstring& url, const std::wstring& path) {
        ListView_SetItemText(m_hListView, 0, 1, (LPWSTR)L"Downloading (Multi-Part)...");
        m_activeDownload = std::make_unique<SegmentedDownloader>(url, path, 8); // 8 parallel connections
        m_activeDownload->Start();
    }

    LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
        switch (uMsg) {
        case WM_TIMER:
            if (m_activeDownload) {
                double pct = m_activeDownload->GetProgress();
                SendMessage(m_hProgressBar, PBM_SETPOS, (WPARAM)(pct * 100.0), 0);
                if (pct >= 1.0) {
                    ListView_SetItemText(m_hListView, 0, 1, (LPWSTR)L"Complete");
                    KillTimer(m_hwnd, 1);
                }
            }
            return 0;
        case WM_DESTROY:
            if (m_activeDownload) m_activeDownload->Cancel();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProc(m_hwnd, uMsg, wParam, lParam);
    }
};

// --- Entry Point ---
int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int) {
    // Initialize WIL/COM Error Handling
    wil::SetResultLoggingCallback([](wil::FailureInfo const& failure) noexcept {
        // Log telemetry/errors internally (IDM style stealth error handling)
        OutputDebugStringW(failure.pszMessage);
    });

    MainWindow win;
    win.Create(hInstance);

    MSG msg = {0};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
