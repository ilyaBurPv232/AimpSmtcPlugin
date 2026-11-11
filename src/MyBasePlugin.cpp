// 1. WinAPI — всегда первым
#include <windows.h>
#include <unknwn.h>
#include <roapi.h>
#include <systemmediatransportcontrolsinterop.h>
#include <shlobj.h>

// 2. Стандартная библиотека
#include <fstream>
#include <string>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>

// 3. C++/WinRT — ПОСЛЕ windows.h, но ДО AIMP
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Foundation.Collections.h>

// 4. AIMP SDK — строго через "..."
#include "apiPlugin.h"
#include "apiCore.h"
#include "apiMessages.h"
#include "apiPlayer.h"
#include "apiFileManager.h"
#include "apiObjects.h"
#include "apiAlbumArt.h"

static const GUID IID_IAIMPPlugin_Manual =
        { 0x41494D50, 0x506C, 0x7567, { 0x69, 0x6E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } };

// ---------------------------------------------------------------------------
// Глобалы
// ---------------------------------------------------------------------------
winrt::Windows::Media::SystemMediaTransportControls g_smtc = nullptr;
HWND g_smtcHwnd = nullptr;
std::ofstream g_logFile;
std::mutex g_logMutex;

static DWORD g_msgInitSmtc = 0;
static std::atomic<int> g_smtcInitResult{ 0 };
IAIMPServicePlayer* g_player = nullptr;

// Event-токены для отписки
winrt::event_token g_buttonPressedToken{};
winrt::event_token g_seekRequestedToken{};

// ---------------------------------------------------------------------------
// Логирование (только если рядом с DLL лежит debug.txt)
// ---------------------------------------------------------------------------
void Log(const std::wstring& message) {
    static bool debugEnabled = []() {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        std::wstring dir(path);
        dir = dir.substr(0, dir.find_last_of(L"\\/"));
        std::wstring flagPath = dir + L"\\debug.txt";

        DWORD attrs = GetFileAttributesW(flagPath.c_str());
        return (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));
    }();

    if (!debugEnabled) return;

    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_logFile.is_open()) {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &path))) {
            std::wstring logPath = std::wstring(path) + L"\\AimpSmtc_Log.txt";
            CoTaskMemFree(path);
            g_logFile.open(logPath, std::ios::out | std::ios::app);
        }
    }
    if (g_logFile.is_open()) {
        int n = WideCharToMultiByte(CP_UTF8, 0, message.c_str(), (int)message.length(), NULL, 0, NULL, NULL);
        std::string s(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, message.c_str(), (int)message.length(), &s[0], n, NULL, NULL);
        g_logFile << s << std::endl;
        g_logFile.flush();
    }
}
void LogHr(const wchar_t* where, HRESULT hr) {
    wchar_t buf[256]; swprintf_s(buf, L"%s: HRESULT=0x%08X", where, (unsigned)hr); Log(buf);
}
void LogHresultException(const wchar_t* where, const winrt::hresult_error& ex) {
    wchar_t buf[512];
    swprintf_s(buf, L"%s: winrt::hresult_error 0x%08X (%s)",
               where, (unsigned)ex.code(), ex.message().c_str());
    Log(buf);
}

// ---------------------------------------------------------------------------
// Поиск главного окна AIMP
// ---------------------------------------------------------------------------
struct WindowSearchData { DWORD targetPid; HWND mainWindow; };
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* data = reinterpret_cast<WindowSearchData*>(lParam);
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
    if (pid == data->targetPid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
        char title[256]; GetWindowTextA(hwnd, title, sizeof(title));
        if (strlen(title) > 0) { data->mainWindow = hwnd; return FALSE; }
    }
    return TRUE;
}
HWND FindAimpMainWindow() {
    WindowSearchData d{ GetCurrentProcessId(), nullptr };
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&d));
    return d.mainWindow;
}

// ---------------------------------------------------------------------------
// Утилиты
// ---------------------------------------------------------------------------
std::wstring AimpStringToWString(IAIMPString* s) {
    if (!s) return L"";
    return std::wstring(s->GetData(), s->GetLength());
}

void UpdatePlaybackStatus(winrt::Windows::Media::MediaPlaybackStatus status) {
    if (!g_smtc) return;
    try { g_smtc.PlaybackStatus(status); }
    catch (...) {}
}

// ---------------------------------------------------------------------------
// Конвертация обложки: IAIMPImageContainer -> RandomAccessStreamReference
// ---------------------------------------------------------------------------
winrt::Windows::Storage::Streams::RandomAccessStreamReference
ContainerToStreamRef(IAIMPImageContainer* container) {
    if (!container) return nullptr;
    try {
        DWORD size = container->GetDataSize();
        BYTE* data = container->GetData();
        if (!data || size == 0) return nullptr;

        winrt::Windows::Storage::Streams::InMemoryRandomAccessStream ras;
        winrt::Windows::Storage::Streams::DataWriter writer(ras);
        writer.WriteBytes(winrt::array_view<const uint8_t>(data, data + size));
        writer.StoreAsync().get();
        writer.FlushAsync().get();
        ras.Seek(0);

        return winrt::Windows::Storage::Streams::RandomAccessStreamReference::CreateFromStream(ras);
    }
    catch (...) { return nullptr; }
}

// ---------------------------------------------------------------------------
// Обновление метаданных (включая обложку)
// ---------------------------------------------------------------------------
void UpdateMetadataFromPlayer() {
    if (!g_smtc || !g_player) return;

    IAIMPFileInfo* info = nullptr;
    HRESULT hr = g_player->GetInfo(&info);
    if (FAILED(hr) || !info) return;

    std::wstring title, artist, album;
    IAIMPString* s = nullptr;

    if (SUCCEEDED(info->GetValueAsObject(AIMP_FILEINFO_PROPID_TITLE, IID_IAIMPString, (void**)&s)) && s) {
        title = AimpStringToWString(s); s->Release();
    }
    if (SUCCEEDED(info->GetValueAsObject(AIMP_FILEINFO_PROPID_ARTIST, IID_IAIMPString, (void**)&s)) && s) {
        artist = AimpStringToWString(s); s->Release();
    }
    if (SUCCEEDED(info->GetValueAsObject(AIMP_FILEINFO_PROPID_ALBUM, IID_IAIMPString, (void**)&s)) && s) {
        album = AimpStringToWString(s); s->Release();
    }

    // Обложка — пробуем взять из файла
    winrt::Windows::Storage::Streams::RandomAccessStreamReference thumb{ nullptr };
    IAIMPImageContainer* container = nullptr;
    if (SUCCEEDED(info->GetValueAsObject(AIMP_FILEINFO_PROPID_ALBUMART,
                                         IID_IAIMPImageContainer, (void**)&container)) && container) {
        thumb = ContainerToStreamRef(container);
        container->Release();
    }
    info->Release();

    if (title.empty() && artist.empty() && album.empty() && !thumb) return;

    try {
        auto updater = g_smtc.DisplayUpdater();
        updater.Type(winrt::Windows::Media::MediaPlaybackType::Music);
        auto mp = updater.MusicProperties();
        mp.Title(winrt::hstring(title));
        mp.Artist(winrt::hstring(artist));
        mp.AlbumTitle(winrt::hstring(album));

        if (thumb) updater.Thumbnail(thumb);

        updater.Update();
    }
    catch (...) {}
}

// ---------------------------------------------------------------------------
// Таймлайн
// ---------------------------------------------------------------------------
void UpdateTimelineFromPlayer() {
    if (!g_smtc || !g_player) return;
    try {
        DOUBLE pos = 0.0, dur = 0.0;
        g_player->GetPosition(&pos);
        g_player->GetDuration(&dur);
        const INT64 ticksPerSec = 10'000'000LL;
        auto timeline = winrt::Windows::Media::SystemMediaTransportControlsTimelineProperties();
        timeline.StartTime(winrt::Windows::Foundation::TimeSpan{ 0 });
        timeline.MinSeekTime(winrt::Windows::Foundation::TimeSpan{ 0 });
        timeline.Position(winrt::Windows::Foundation::TimeSpan{ (INT64)(pos * ticksPerSec) });
        timeline.MaxSeekTime(winrt::Windows::Foundation::TimeSpan{ (INT64)(dur * ticksPerSec) });
        timeline.EndTime(winrt::Windows::Foundation::TimeSpan{ (INT64)(dur * ticksPerSec) });
        g_smtc.UpdateTimelineProperties(timeline);
    }
    catch (...) {}
}

// ---------------------------------------------------------------------------
// Синхронизация состояния при старте
// ---------------------------------------------------------------------------
void SyncCurrentTrackWithSMTC() {
    if (!g_smtc || !g_player) return;

    INT32 state = g_player->GetState();
    if (state == AIMP_PLAYER_STATE_PLAYING) UpdatePlaybackStatus(winrt::Windows::Media::MediaPlaybackStatus::Playing);
    else if (state == AIMP_PLAYER_STATE_PAUSED) UpdatePlaybackStatus(winrt::Windows::Media::MediaPlaybackStatus::Paused);
    else UpdatePlaybackStatus(winrt::Windows::Media::MediaPlaybackStatus::Stopped);

    UpdateMetadataFromPlayer();
    UpdateTimelineFromPlayer();
}

// ---------------------------------------------------------------------------
// ЯДРО: Инициализация SMTC
// ---------------------------------------------------------------------------
bool TryInitializeSMTC() {
    HWND aimpWindow = FindAimpMainWindow();
    if (!aimpWindow) return false;

    if (g_smtc && g_smtcHwnd != aimpWindow) {
        g_smtc = nullptr;
        g_smtcHwnd = nullptr;
    }
    if (g_smtc && g_smtcHwnd == aimpWindow) return true;

    try {
        auto factory = winrt::get_activation_factory<winrt::Windows::Media::SystemMediaTransportControls, IActivationFactory>();
        auto interop = factory.as<ISystemMediaTransportControlsInterop>();
        winrt::com_ptr<winrt::Windows::Media::ISystemMediaTransportControls> smtcRaw;

        HRESULT hr = interop->GetForWindow(
                aimpWindow,
                winrt::guid_of<winrt::Windows::Media::ISystemMediaTransportControls>(),
                smtcRaw.put_void());

        if (FAILED(hr)) return false;

        g_smtc = smtcRaw.as<winrt::Windows::Media::SystemMediaTransportControls>();
        g_smtcHwnd = aimpWindow;

        g_smtc.IsEnabled(true);
        g_smtc.IsPlayEnabled(true);
        g_smtc.IsPauseEnabled(true);
        g_smtc.IsNextEnabled(true);
        g_smtc.IsPreviousEnabled(true);
        g_smtc.IsStopEnabled(true);

        // --- Кнопки Windows -> AIMP ---
        g_buttonPressedToken = g_smtc.ButtonPressed(
                [](auto const&, auto const& args) {
                    if (!g_player) return;
                    using Btn = winrt::Windows::Media::SystemMediaTransportControlsButton;
                    switch (args.Button()) {
                        case Btn::Play:     g_player->Resume();   break;
                        case Btn::Pause:    g_player->Pause();    break;
                        case Btn::Next:     g_player->GoToNext(); break;
                        case Btn::Previous: g_player->GoToPrev(); break;
                        case Btn::Stop:     g_player->Stop();     break;
                        default: break;
                    }
                });

        // --- Перемотка ползунком из Windows -> AIMP ---
        g_seekRequestedToken = g_smtc.PlaybackPositionChangeRequested(
                [](auto const&, auto const& args) {
                    if (!g_player) return;
                    g_player->SetPosition((DOUBLE)args.RequestedPlaybackPosition().count() / 10'000'000.0);
                });

        SyncCurrentTrackWithSMTC();
        return true;
    }
    catch (...) { return false; }
}

// ---------------------------------------------------------------------------
// Фоновый поток для перехвата окна
// ---------------------------------------------------------------------------
void StartInitThread() {
    std::thread([]() {
        for (int i = 0; i < 50; ++i) {
            if (TryInitializeSMTC()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }).detach();
}

// ---------------------------------------------------------------------------
// Хук
// ---------------------------------------------------------------------------
class MyMessageHook : public IAIMPMessageHook {
    ULONG refCount = 1;
public:
    HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IAIMPMessageHook) {
            *ppv = static_cast<IAIMPMessageHook*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG WINAPI AddRef() override { return InterlockedIncrement(&refCount); }
    ULONG WINAPI Release() override {
        ULONG r = InterlockedDecrement(&refCount);
        if (r == 0) delete this;
        return r;
    }

    void WINAPI CoreMessage(DWORD Message, INT32 Param1, void* Param2, HRESULT* Result) override {
        if (g_msgInitSmtc && Message == g_msgInitSmtc) {
            bool ok = TryInitializeSMTC();
            if (Result) *Result = ok ? S_OK : E_FAIL;
            return;
        }

        if (Message == AIMP_MSG_EVENT_STREAM_START ||
            Message == AIMP_MSG_EVENT_STREAM_START_SUBTRACK ||
            Message == AIMP_MSG_EVENT_PLAYING_FILE_INFO) {
            UpdateMetadataFromPlayer();
            UpdateTimelineFromPlayer();
        }
        else if (Message == AIMP_MSG_EVENT_PLAYER_STATE) {
            if (Param1 == AIMP_PLAYER_STATE_PLAYING) UpdatePlaybackStatus(winrt::Windows::Media::MediaPlaybackStatus::Playing);
            else if (Param1 == AIMP_PLAYER_STATE_PAUSED) UpdatePlaybackStatus(winrt::Windows::Media::MediaPlaybackStatus::Paused);
            else UpdatePlaybackStatus(winrt::Windows::Media::MediaPlaybackStatus::Stopped);
            UpdateTimelineFromPlayer();
        }
        else if (Message == AIMP_MSG_EVENT_PLAYER_UPDATE_POSITION ||
                 Message == AIMP_MSG_EVENT_PLAYER_UPDATE_POSITION_HR) {
            UpdateTimelineFromPlayer();
        }
    }
};

// ---------------------------------------------------------------------------
// Плагин
// ---------------------------------------------------------------------------
class MyPlugin : public IAIMPPlugin {
    ULONG refCount = 1;
    IAIMPServiceMessageDispatcher* dispatcher = nullptr;
    IAIMPCore* core = nullptr;
    MyMessageHook* hook = nullptr;
public:
    HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || IsEqualGUID(riid, IID_IAIMPPlugin_Manual)) {
            *ppv = static_cast<IAIMPPlugin*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG WINAPI AddRef() override { return InterlockedIncrement(&refCount); }
    ULONG WINAPI Release() override {
        ULONG r = InterlockedDecrement(&refCount);
        if (r == 0) delete this;
        return r;
    }
    PChar WINAPI InfoGet(int Index) override {
        switch (Index) {
            case AIMP_PLUGIN_INFO_NAME:   return const_cast<PChar>(L"AIMP SMTC Plugin");
            case AIMP_PLUGIN_INFO_AUTHOR: return const_cast<PChar>(L"Burcevi");
            default: return nullptr;
        }
    }
    DWORD WINAPI InfoGetCategories() override { return AIMP_PLUGIN_CATEGORY_ADDONS; }

    HRESULT WINAPI Initialize(IAIMPCore* Core) override {
        if (!Core) return E_INVALIDARG;

        try { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
        catch (...) {}

        core = Core; core->AddRef();

        g_smtc = nullptr;
        g_smtcHwnd = nullptr;
        g_msgInitSmtc = 0;

        HRESULT hr = core->QueryInterface(IID_IAIMPServiceMessageDispatcher, (void**)&dispatcher);
        if (FAILED(hr)) return E_FAIL;

        g_msgInitSmtc = dispatcher->Register(const_cast<PChar>(L"AimpSmtc_InitMessage"));

        hook = new MyMessageHook();
        dispatcher->Hook(hook);

        hr = core->QueryInterface(IID_IAIMPServicePlayer, (void**)&g_player);

        if (!TryInitializeSMTC()) {
            StartInitThread();
        }

        return S_OK;
    }

    HRESULT WINAPI Finalize() override {
        try {
            if (g_smtc) {
                g_smtc.ButtonPressed(g_buttonPressedToken);
                g_smtc.PlaybackPositionChangeRequested(g_seekRequestedToken);
            }
        } catch (...) {}

        g_smtc = nullptr;
        g_smtcHwnd = nullptr;

        if (g_player) { g_player->Release(); g_player = nullptr; }
        if (dispatcher && hook) dispatcher->Unhook(hook);
        if (hook) { hook->Release(); hook = nullptr; }
        if (dispatcher) { dispatcher->Release(); dispatcher = nullptr; }
        if (core) { core->Release(); core = nullptr; }

        return S_OK;
    }

    void WINAPI SystemNotification(int NotifyID, IUnknown* Data) override {}
};

extern "C" HRESULT WINAPI AIMPPluginGetHeader(IAIMPPlugin** Plugin) {
    if (!Plugin) return E_POINTER;
    *Plugin = new MyPlugin();
    return S_OK;
}