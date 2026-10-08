// ==WindhawkMod==
// @id              potplayer-discord-rich-presence
// @name            PotPlayer Discord Rich Presence
// @description     PotPlayer Rich Presence with TMDB artwork, movie/TV lookup, timestamps, PotPlayer icon and pause-aware timing.
// @version         2.2
// @author          Custom
// @include         PotPlayerMini64.exe
// @include         PotPlayerMini.exe
// ==/WindhawkMod==

// ==WindhawkModReadme==

// ==WindhawkModSettings==
/*
- application_id: "000000000000000000"
  $name: Discord Application ID
  $description: Your Discord Developer Application ID

- tmdb_token: ""
  $name: TMDB API Read Access Token
  $description: TMDB Bearer/API Read Access Token

- small_image_asset: "potplayer"
  $name: PotPlayer Small Image Asset
  $description: Discord Rich Presence asset key for your PotPlayer icon

- language: "en-US"
  $name: TMDB Language
  $description: Language used for TMDB searches
*/
// ==/WindhawkModSettings==


#include <windows.h>
#include <winhttp.h>

#include <stdint.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <cstdio>


// ============================================================
// Globals
// ============================================================

static std::wstring g_applicationId;
static std::wstring g_tmdbToken;
static std::wstring g_smallImageAsset;
static std::wstring g_tmdbLanguage = L"en-US";

static HANDLE g_workerThread = nullptr;
static volatile LONG g_stopWorker = 0;

static HANDLE g_discordPipe = INVALID_HANDLE_VALUE;
static bool g_discordConnected = false;

static uint64_t g_nonce = 1;

static std::wstring g_currentTitle;
static std::wstring g_currentSearchTitle;

static int64_t g_currentDuration = -1;
static int64_t g_currentPosition = -1;

static int64_t g_startTimestamp = 0;

static std::string g_cachedPosterUrl;
static bool g_tmdbLookupAttempted = false;


// ============================================================
// PotPlayer commands
// ============================================================

static constexpr UINT PP_GET_DURATION = 0x5002;
static constexpr UINT PP_GET_POSITION = 0x5004;
static constexpr UINT PP_GET_PLAY_STATUS = 0x5006;

// [Unverified] Based on community PotPlayer WM_USER documentation:
// -1 = stopped, 1 = paused, 2 = playing.
static constexpr int PP_STATUS_PAUSED = 1;
static constexpr int PP_STATUS_PLAYING = 2;


// ============================================================
// Dynamic WinHTTP
// ============================================================

typedef HINTERNET (WINAPI* PFN_WinHttpOpen)(
    LPCWSTR,
    DWORD,
    LPCWSTR,
    LPCWSTR,
    DWORD
);

typedef HINTERNET (WINAPI* PFN_WinHttpConnect)(
    HINTERNET,
    LPCWSTR,
    INTERNET_PORT,
    DWORD
);

typedef HINTERNET (WINAPI* PFN_WinHttpOpenRequest)(
    HINTERNET,
    LPCWSTR,
    LPCWSTR,
    LPCWSTR,
    LPCWSTR,
    LPCWSTR*,
    DWORD
);

typedef BOOL (WINAPI* PFN_WinHttpSetTimeouts)(
    HINTERNET,
    int,
    int,
    int,
    int
);

typedef BOOL (WINAPI* PFN_WinHttpAddRequestHeaders)(
    HINTERNET,
    LPCWSTR,
    DWORD,
    DWORD
);

typedef BOOL (WINAPI* PFN_WinHttpSendRequest)(
    HINTERNET,
    LPCWSTR,
    DWORD,
    LPVOID,
    DWORD,
    DWORD,
    DWORD_PTR
);

typedef BOOL (WINAPI* PFN_WinHttpReceiveResponse)(
    HINTERNET,
    LPVOID
);

typedef BOOL (WINAPI* PFN_WinHttpQueryDataAvailable)(
    HINTERNET,
    LPDWORD
);

typedef BOOL (WINAPI* PFN_WinHttpReadData)(
    HINTERNET,
    LPVOID,
    DWORD,
    LPDWORD
);

typedef BOOL (WINAPI* PFN_WinHttpQueryHeaders)(
    HINTERNET,
    DWORD,
    LPCWSTR,
    LPVOID,
    LPDWORD,
    LPDWORD
);

typedef BOOL (WINAPI* PFN_WinHttpCloseHandle)(
    HINTERNET
);


static HMODULE g_winHttpModule = nullptr;

static PFN_WinHttpOpen pWinHttpOpen = nullptr;
static PFN_WinHttpConnect pWinHttpConnect = nullptr;
static PFN_WinHttpOpenRequest pWinHttpOpenRequest = nullptr;
static PFN_WinHttpSetTimeouts pWinHttpSetTimeouts = nullptr;
static PFN_WinHttpAddRequestHeaders pWinHttpAddRequestHeaders = nullptr;
static PFN_WinHttpSendRequest pWinHttpSendRequest = nullptr;
static PFN_WinHttpReceiveResponse pWinHttpReceiveResponse = nullptr;
static PFN_WinHttpQueryDataAvailable pWinHttpQueryDataAvailable = nullptr;
static PFN_WinHttpReadData pWinHttpReadData = nullptr;
static PFN_WinHttpQueryHeaders pWinHttpQueryHeaders = nullptr;
static PFN_WinHttpCloseHandle pWinHttpCloseHandle = nullptr;


// ============================================================
// Load WinHTTP dynamically
// ============================================================

static bool LoadWinHttp()
{
    if (g_winHttpModule)
        return true;

    g_winHttpModule =
        LoadLibraryW(L"winhttp.dll");

    if (!g_winHttpModule)
    {
        Wh_Log(L"Failed to load winhttp.dll.");
        return false;
    }

#define LOAD_WINHTTP_FUNCTION(name)                         \
    p##name = reinterpret_cast<PFN_##name>(                \
        GetProcAddress(g_winHttpModule, #name)             \
    );                                                       \
    if (!p##name)                                            \
    {                                                        \
        FreeLibrary(g_winHttpModule);                       \
        g_winHttpModule = nullptr;                          \
        return false;                                       \
    }

    LOAD_WINHTTP_FUNCTION(WinHttpOpen);
    LOAD_WINHTTP_FUNCTION(WinHttpConnect);
    LOAD_WINHTTP_FUNCTION(WinHttpOpenRequest);
    LOAD_WINHTTP_FUNCTION(WinHttpSetTimeouts);
    LOAD_WINHTTP_FUNCTION(WinHttpAddRequestHeaders);
    LOAD_WINHTTP_FUNCTION(WinHttpSendRequest);
    LOAD_WINHTTP_FUNCTION(WinHttpReceiveResponse);
    LOAD_WINHTTP_FUNCTION(WinHttpQueryDataAvailable);
    LOAD_WINHTTP_FUNCTION(WinHttpReadData);
    LOAD_WINHTTP_FUNCTION(WinHttpQueryHeaders);
    LOAD_WINHTTP_FUNCTION(WinHttpCloseHandle);

#undef LOAD_WINHTTP_FUNCTION

    return true;
}


static void UnloadWinHttp()
{
    if (g_winHttpModule)
    {
        FreeLibrary(g_winHttpModule);
        g_winHttpModule = nullptr;
    }

    pWinHttpOpen = nullptr;
    pWinHttpConnect = nullptr;
    pWinHttpOpenRequest = nullptr;
    pWinHttpSetTimeouts = nullptr;
    pWinHttpAddRequestHeaders = nullptr;
    pWinHttpSendRequest = nullptr;
    pWinHttpReceiveResponse = nullptr;
    pWinHttpQueryDataAvailable = nullptr;
    pWinHttpReadData = nullptr;
    pWinHttpQueryHeaders = nullptr;
    pWinHttpCloseHandle = nullptr;
}


// ============================================================
// String conversion
// ============================================================

static std::string WideToUtf8(
    const std::wstring& text)
{
    if (text.empty())
        return {};

    int required =
        WideCharToMultiByte(
            CP_UTF8,
            0,
            text.c_str(),
            static_cast<int>(text.size()),
            nullptr,
            0,
            nullptr,
            nullptr
        );

    if (required <= 0)
        return {};

    std::string result(
        required,
        '\0'
    );

    WideCharToMultiByte(
        CP_UTF8,
        0,
        text.c_str(),
        static_cast<int>(text.size()),
        result.data(),
        required,
        nullptr,
        nullptr
    );

    return result;
}


static std::wstring Utf8ToWide(
    const std::string& text)
{
    if (text.empty())
        return {};

    int required =
        MultiByteToWideChar(
            CP_UTF8,
            0,
            text.c_str(),
            static_cast<int>(text.size()),
            nullptr,
            0
        );

    if (required <= 0)
        return {};

    std::wstring result(
        required,
        L'\0'
    );

    MultiByteToWideChar(
        CP_UTF8,
        0,
        text.c_str(),
        required,
        result.data(),
        required
    );

    return result;
}


// ============================================================
// JSON escaping
// ============================================================

static std::string JsonEscape(
    const std::string& input)
{
    std::string output;

    output.reserve(
        input.size() + 16
    );

    for (unsigned char c : input)
    {
        switch (c)
        {
        case '\"':
            output += "\\\"";
            break;

        case '\\':
            output += "\\\\";
            break;

        case '\b':
            output += "\\b";
            break;

        case '\f':
            output += "\\f";
            break;

        case '\n':
            output += "\\n";
            break;

        case '\r':
            output += "\\r";
            break;

        case '\t':
            output += "\\t";
            break;

        default:
            if (c < 0x20)
            {
                char buffer[8];

                sprintf_s(
                    buffer,
                    "\\u%04x",
                    static_cast<unsigned int>(c)
                );

                output += buffer;
            }
            else
            {
                output +=
                    static_cast<char>(c);
            }

            break;
        }
    }

    return output;
}


// ============================================================
// URL encoding
// ============================================================

static bool IsUrlSafe(
    unsigned char c)
{
    return
        (c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '-' ||
        c == '_' ||
        c == '.' ||
        c == '~';
}


static std::string UrlEncode(
    const std::string& input)
{
    static const char hex[] =
        "0123456789ABCDEF";

    std::string output;

    output.reserve(
        input.size() * 2
    );

    for (unsigned char c : input)
    {
        if (IsUrlSafe(c))
        {
            output +=
                static_cast<char>(c);
        }
        else
        {
            output += '%';

            output +=
                hex[(c >> 4) & 0x0F];

            output +=
                hex[c & 0x0F];
        }
    }

    return output;
}


// ============================================================
// String helpers
// ============================================================

static std::wstring Trim(
    const std::wstring& value)
{
    size_t start = 0;

    while (
        start < value.size() &&
        iswspace(value[start])
    )
    {
        ++start;
    }

    size_t end =
        value.size();

    while (
        end > start &&
        iswspace(value[end - 1])
    )
    {
        --end;
    }

    return value.substr(
        start,
        end - start
    );
}


static std::wstring ToLower(
    std::wstring value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](wchar_t c)
        {
            return static_cast<wchar_t>(
                towlower(c)
            );
        }
    );

    return value;
}


// ============================================================
// Year extraction
// ============================================================

static int ExtractYear(
    const std::wstring& title)
{
    for (
        size_t i = 0;
        i + 4 <= title.size();
        ++i
    )
    {
        if (
            iswdigit(title[i]) &&
            iswdigit(title[i + 1]) &&
            iswdigit(title[i + 2]) &&
            iswdigit(title[i + 3])
        )
        {
            int year =
                (title[i] - L'0') * 1000 +
                (title[i + 1] - L'0') * 100 +
                (title[i + 2] - L'0') * 10 +
                (title[i + 3] - L'0');

            if (
                year >= 1900 &&
                year <= 2099
            )
            {
                return year;
            }
        }
    }

    return 0;
}


// ============================================================
// Clean title for TMDB
// ============================================================

static std::wstring CleanMediaTitle(
    const std::wstring& original)
{
    std::wstring title =
        original;

    // Remove PotPlayer suffix.
    const std::wstring suffix =
        L" - PotPlayer";

    if (
        title.size() >= suffix.size()
    )
    {
        size_t pos =
            title.size() -
            suffix.size();

        if (
            _wcsicmp(
                title.c_str() + pos,
                suffix.c_str()
            ) == 0
        )
        {
            title.resize(pos);
        }
    }

    // Remove video extension.
    const wchar_t* extensions[] =
    {
        L".mkv",
        L".mp4",
        L".avi",
        L".mov",
        L".wmv",
        L".flv",
        L".webm",
        L".m4v",
        L".ts",
        L".m2ts",
        L".mpg",
        L".mpeg"
    };

    for (
        const wchar_t* extension :
        extensions
    )
    {
        size_t length =
            wcslen(extension);

        if (title.size() >= length)
        {
            size_t pos =
                title.size() - length;

            if (
                _wcsicmp(
                    title.c_str() + pos,
                    extension
                ) == 0
            )
            {
                title.resize(pos);
                break;
            }
        }
    }

    // Detect S01E01 style TV filenames.
    for (
        size_t i = 0;
        i + 6 <= title.size();
        ++i
    )
    {
        if (
            (title[i] == L'S' ||
             title[i] == L's') &&
            iswdigit(title[i + 1]) &&
            iswdigit(title[i + 2]) &&
            (title[i + 3] == L'E' ||
             title[i + 3] == L'e') &&
            iswdigit(title[i + 4]) &&
            iswdigit(title[i + 5])
        )
        {
            title =
                title.substr(0, i);

            break;
        }
    }

    // Replace filename separators.
    for (wchar_t& c : title)
    {
        if (
            c == L'.' ||
            c == L'_' ||
            c == L'+'
        )
        {
            c = L' ';
        }
    }

    title =
        Trim(title);

    // Remove common release metadata.
    const wchar_t* markers[] =
    {
        L" 2160p",
        L" 1080p",
        L" 720p",
        L" 576p",
        L" 480p",
        L" 4k",
        L" bluray",
        L" blu ray",
        L" web-dl",
        L" web dl",
        L" webrip",
        L" web rip",
        L" hdr10",
        L" hdr",
        L" dolby vision",
        L" x264",
        L" x265",
        L" h264",
        L" h265",
        L" hevc",
        L" av1",
        L" remux",
        L" proper",
        L" extended",
        L" unrated",
        L" directors cut",
        L" dual audio",
        L" multi audio"
    };

    std::wstring lower =
        ToLower(title);

    size_t earliest =
        std::wstring::npos;

    for (
        const wchar_t* marker :
        markers
    )
    {
        size_t pos =
            lower.find(marker);

        if (
            pos !=
            std::wstring::npos &&
            (
                earliest ==
                std::wstring::npos ||
                pos < earliest
            )
        )
        {
            earliest = pos;
        }
    }

    if (
        earliest !=
        std::wstring::npos
    )
    {
        title =
            title.substr(
                0,
                earliest
            );
    }

    return Trim(title);
}


// ============================================================
// HTTP GET using dynamically loaded WinHTTP
// ============================================================

static bool HttpGet(
    const std::wstring& host,
    const std::wstring& path,
    std::string& response)
{
    response.clear();

    if (!LoadWinHttp())
        return false;

    HINTERNET session =
        pWinHttpOpen(
            L"PotPlayer-Discord-Rich-Presence/2.1",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0
        );

    if (!session)
        return false;

    pWinHttpSetTimeouts(
        session,
        5000,
        5000,
        5000,
        5000
    );

    HINTERNET connection =
        pWinHttpConnect(
            session,
            host.c_str(),
            INTERNET_DEFAULT_HTTPS_PORT,
            0
        );

    if (!connection)
    {
        pWinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request =
        pWinHttpOpenRequest(
            connection,
            L"GET",
            path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE
        );

    if (!request)
    {
        pWinHttpCloseHandle(connection);
        pWinHttpCloseHandle(session);
        return false;
    }

    bool success = false;

    // Accept JSON.
    pWinHttpAddRequestHeaders(
        request,
        L"Accept: application/json\r\n",
        static_cast<DWORD>(-1L),
        WINHTTP_ADDREQ_FLAG_ADD
    );

    // TMDB Bearer token.
    if (!g_tmdbToken.empty())
    {
        std::wstring auth =
            L"Authorization: Bearer " +
            g_tmdbToken;

        pWinHttpAddRequestHeaders(
            request,
            auth.c_str(),
            static_cast<DWORD>(-1L),
            WINHTTP_ADDREQ_FLAG_ADD
        );
    }

    if (
        pWinHttpSendRequest(
            request,
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            nullptr,
            0,
            0,
            0
        )
    )
    {
        if (
            pWinHttpReceiveResponse(
                request,
                nullptr
            )
        )
        {
            DWORD statusCode = 0;
            DWORD statusSize =
                sizeof(statusCode);

            pWinHttpQueryHeaders(
                request,
                WINHTTP_QUERY_STATUS_CODE |
                    WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &statusCode,
                &statusSize,
                WINHTTP_NO_HEADER_INDEX
            );

            if (statusCode == 200)
            {
                success = true;

                while (true)
                {
                    DWORD available = 0;

                    if (
                        !pWinHttpQueryDataAvailable(
                            request,
                            &available
                        )
                    )
                    {
                        success = false;
                        break;
                    }

                    if (available == 0)
                        break;

                    std::vector<char> buffer(
                        available
                    );

                    DWORD bytesRead = 0;

                    if (
                        !pWinHttpReadData(
                            request,
                            buffer.data(),
                            available,
                            &bytesRead
                        )
                    )
                    {
                        success = false;
                        break;
                    }

                    if (bytesRead > 0)
                    {
                        response.append(
                            buffer.data(),
                            bytesRead
                        );
                    }
                }
            }
            else
            {
                Wh_Log(
                    L"TMDB HTTP status: %lu",
                    statusCode
                );
            }
        }
    }

    pWinHttpCloseHandle(request);
    pWinHttpCloseHandle(connection);
    pWinHttpCloseHandle(session);

    return success;
}


// ============================================================
// Minimal JSON string extraction
// ============================================================

static std::string JsonExtractString(
    const std::string& json,
    const std::string& key,
    size_t startPosition = 0)
{
    std::string search =
        "\"" + key + "\":";

    size_t keyPos =
        json.find(
            search,
            startPosition
        );

    if (
        keyPos ==
        std::string::npos
    )
    {
        return {};
    }

    size_t valuePos =
        keyPos + search.size();

    while (
        valuePos < json.size() &&
        (
            json[valuePos] == ' ' ||
            json[valuePos] == '\t' ||
            json[valuePos] == '\r' ||
            json[valuePos] == '\n'
        )
    )
    {
        ++valuePos;
    }

    if (
        valuePos >= json.size() ||
        json[valuePos] != '"'
    )
    {
        return {};
    }

    ++valuePos;

    std::string result;
    bool escaped = false;

    for (
        size_t i = valuePos;
        i < json.size();
        ++i
    )
    {
        char c =
            json[i];

        if (escaped)
        {
            switch (c)
            {
            case '"':
                result += '"';
                break;

            case '\\':
                result += '\\';
                break;

            case '/':
                result += '/';
                break;

            case 'n':
                result += '\n';
                break;

            case 'r':
                result += '\r';
                break;

            case 't':
                result += '\t';
                break;

            default:
                result += c;
                break;
            }

            escaped = false;
            continue;
        }

        if (c == '\\')
        {
            escaped = true;
            continue;
        }

        if (c == '"')
            break;

        result += c;
    }

    return result;
}


// ============================================================
// Find first TMDB poster
// ============================================================

static std::string FindPosterPath(
    const std::string& json)
{
    size_t resultsPos =
        json.find("\"results\"");

    if (
        resultsPos ==
        std::string::npos
    )
    {
        return {};
    }

    size_t position =
        resultsPos;

    while (true)
    {
        size_t posterPos =
            json.find(
                "\"poster_path\"",
                position
            );

        if (
            posterPos ==
            std::string::npos
        )
        {
            return {};
        }

        std::string poster =
            JsonExtractString(
                json,
                "poster_path",
                posterPos
            );

        if (!poster.empty())
            return poster;

        position =
            posterPos + 13;
    }
}


// ============================================================
// Build TMDB poster URL
// ============================================================

static std::string BuildPosterUrl(
    const std::string& posterPath)
{
    if (posterPath.empty())
        return {};

    return
        "https://image.tmdb.org/t/p/w500" +
        posterPath;
}


// ============================================================
// TMDB search
// ============================================================

static std::string TmdbSearch(
    const std::wstring& title,
    bool tv,
    int year)
{
    if (title.empty())
        return {};

    std::string query =
        WideToUtf8(title);

    std::string path;

    if (tv)
    {
        path =
            "/3/search/tv?query=";
    }
    else
    {
        path =
            "/3/search/movie?query=";
    }

    path +=
        UrlEncode(query);

    path +=
        "&language=";

    path +=
        UrlEncode(
            WideToUtf8(
                g_tmdbLanguage
            )
        );

    path +=
        "&include_adult=false";

    if (year > 0)
    {
        path +=
            "&year=";

        path +=
            std::to_string(year);
    }

    std::string response;

    if (
        !HttpGet(
            L"api.themoviedb.org",
            Utf8ToWide(path),
            response
        )
    )
    {
        return {};
    }

    return FindPosterPath(response);
}


// ============================================================
// TMDB artwork lookup
// ============================================================

static std::string GetTmdbArtwork(
    const std::wstring& cleanTitle)
{
    if (cleanTitle.empty())
        return {};

    // Cache.
    if (
        g_tmdbLookupAttempted &&
        g_currentSearchTitle ==
            cleanTitle
    )
    {
        return g_cachedPosterUrl;
    }

    g_tmdbLookupAttempted = true;

    g_currentSearchTitle =
        cleanTitle;

    g_cachedPosterUrl.clear();

    int year =
        ExtractYear(cleanTitle);

    // --------------------------------------------------------
    // Movie search
    // --------------------------------------------------------

    std::string poster =
        TmdbSearch(
            cleanTitle,
            false,
            year
        );

    // --------------------------------------------------------
    // Movie search without year
    // --------------------------------------------------------

    if (
        poster.empty() &&
        year > 0
    )
    {
        std::wstring titleWithoutYear =
            cleanTitle;

        for (
            size_t i = 0;
            i + 4 <=
                titleWithoutYear.size();
            ++i
        )
        {
            if (
                iswdigit(
                    titleWithoutYear[i]
                ) &&
                iswdigit(
                    titleWithoutYear[i + 1]
                ) &&
                iswdigit(
                    titleWithoutYear[i + 2]
                ) &&
                iswdigit(
                    titleWithoutYear[i + 3]
                )
            )
            {
                titleWithoutYear.erase(
                    i,
                    4
                );

                break;
            }
        }

        titleWithoutYear =
            Trim(titleWithoutYear);

        poster =
            TmdbSearch(
                titleWithoutYear,
                false,
                0
            );
    }

    // --------------------------------------------------------
    // TV fallback
    // --------------------------------------------------------

    if (poster.empty())
    {
        poster =
            TmdbSearch(
                cleanTitle,
                true,
                year
            );
    }

    // --------------------------------------------------------
    // TV fallback without year
    // --------------------------------------------------------

    if (
        poster.empty() &&
        year > 0
    )
    {
        poster =
            TmdbSearch(
                cleanTitle,
                true,
                0
            );
    }

    if (poster.empty())
    {
        Wh_Log(
            L"TMDB: no poster found for \"%s\"",
            cleanTitle.c_str()
        );

        return {};
    }

    g_cachedPosterUrl =
        BuildPosterUrl(poster);

    Wh_Log(
        L"TMDB artwork found for \"%s\"",
        cleanTitle.c_str()
    );

    return g_cachedPosterUrl;
}


// ============================================================
// Discord IPC
// ============================================================

static bool WriteAll(
    const void* data,
    DWORD size)
{
    const BYTE* pointer =
        static_cast<const BYTE*>(data);

    while (size > 0)
    {
        DWORD written = 0;

        if (
            !WriteFile(
                g_discordPipe,
                pointer,
                size,
                &written,
                nullptr
            )
        )
        {
            return false;
        }

        if (written == 0)
            return false;

        pointer += written;
        size -= written;
    }

    return true;
}


static bool SendDiscordFrame(
    uint32_t opcode,
    const std::string& json)
{
    if (
        g_discordPipe ==
        INVALID_HANDLE_VALUE
    )
    {
        return false;
    }

    uint32_t header[2];

    header[0] = opcode;

    header[1] =
        static_cast<uint32_t>(
            json.size()
        );

    if (
        !WriteAll(
            header,
            sizeof(header)
        )
    )
    {
        return false;
    }

    if (!json.empty())
    {
        if (
            !WriteAll(
                json.data(),
                static_cast<DWORD>(
                    json.size()
                )
            )
        )
        {
            return false;
        }
    }

    return true;
}


static void DisconnectDiscord()
{
    if (
        g_discordPipe !=
        INVALID_HANDLE_VALUE
    )
    {
        CloseHandle(
            g_discordPipe
        );

        g_discordPipe =
            INVALID_HANDLE_VALUE;
    }

    g_discordConnected =
        false;
}


// ============================================================
// Discord connection
// ============================================================

static bool ConnectDiscord()
{
    DisconnectDiscord();

    for (
        int pipeNumber = 0;
        pipeNumber < 10;
        ++pipeNumber
    )
    {
        wchar_t pipeName[128];

        swprintf_s(
            pipeName,
            L"\\\\.\\pipe\\discord-ipc-%d",
            pipeNumber
        );

        HANDLE pipe =
            CreateFileW(
                pipeName,
                GENERIC_READ |
                    GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                0,
                nullptr
            );

        if (
            pipe ==
            INVALID_HANDLE_VALUE
        )
        {
            continue;
        }

        g_discordPipe =
            pipe;

        g_discordConnected =
            true;

        std::string handshake =
            "{\"v\":1,\"client_id\":\"" +
            JsonEscape(
                WideToUtf8(
                    g_applicationId
                )
            ) +
            "\"}";

        if (
            !SendDiscordFrame(
                0,
                handshake
            )
        )
        {
            DisconnectDiscord();
            continue;
        }

        Sleep(100);

        Wh_Log(
            L"Connected to Discord IPC."
        );

        return true;
    }

    Wh_Log(
        L"Could not connect to Discord IPC."
    );

    return false;
}


// ============================================================
// Clear Discord activity
// ============================================================

static void ClearDiscordActivity()
{
    if (!g_discordConnected)
        return;

    std::string json =
        "{"
            "\"cmd\":\"SET_ACTIVITY\","
            "\"args\":{"
                "\"pid\":" +
                std::to_string(
                    GetCurrentProcessId()
                ) +
                ","
                "\"activity\":null"
            "},"
            "\"nonce\":\"" +
            std::to_string(
                g_nonce++
            ) +
            "\""
        "}";

    if (
        !SendDiscordFrame(
            1,
            json
        )
    )
    {
        DisconnectDiscord();
    }
}


// ============================================================
// Set Discord activity
// ============================================================

static bool SetDiscordActivity(
    const std::string& title,
    const std::string& posterUrl,
    int64_t startTimestamp,
    int64_t endTimestamp)
{
    if (!g_discordConnected)
    {
        if (!ConnectDiscord())
            return false;
    }

    std::string largeImage;

    if (!posterUrl.empty())
    {
        // TMDB external URL.
        largeImage =
            posterUrl;
    }
    else
    {
        // Fallback to uploaded asset.
        largeImage =
            WideToUtf8(
                g_smallImageAsset
            );

        if (largeImage.empty())
            largeImage = "potplayer";
    }

    std::string smallImage =
        WideToUtf8(
            g_smallImageAsset
        );

    if (smallImage.empty())
        smallImage = "potplayer";

    std::string json =
        "{"
            "\"cmd\":\"SET_ACTIVITY\","
            "\"args\":{"
                "\"pid\":" +
                std::to_string(
                    GetCurrentProcessId()
                ) +
                ","
                "\"activity\":{"

                    "\"type\":3,"

                    "\"details\":\"" +
                    JsonEscape(title) +
                    "\","

                    "\"state\":\"Watching on PotPlayer\","

                    "\"timestamps\":{"
                        "\"start\":" +
                        std::to_string(
                            startTimestamp
                        ) +
                        ","
                        "\"end\":" +
                        std::to_string(
                            endTimestamp
                        ) +
                    "},"

                    "\"assets\":{"

                        "\"large_image\":\"" +
                        JsonEscape(
                            largeImage
                        ) +
                        "\","

                        "\"large_text\":\"" +
                        JsonEscape(title) +
                        "\","

                        "\"small_image\":\"" +
                        JsonEscape(
                            smallImage
                        ) +
                        "\","

                        "\"small_text\":\"PotPlayer\""

                    "}"

                "}"
            "},"
            "\"nonce\":\"" +
            std::to_string(
                g_nonce++
            ) +
            "\""
        "}";

    if (
        !SendDiscordFrame(
            1,
            json
        )
    )
    {
        DisconnectDiscord();

        return false;
    }

    return true;
}


// ============================================================
// Unix timestamp
// ============================================================

static int64_t GetUnixTime()
{
    FILETIME fileTime;

    GetSystemTimeAsFileTime(
        &fileTime
    );

    ULARGE_INTEGER value;

    value.LowPart =
        fileTime.dwLowDateTime;

    value.HighPart =
        fileTime.dwHighDateTime;

    constexpr uint64_t UNIX_EPOCH =
        116444736000000000ULL;

    if (
        value.QuadPart <
        UNIX_EPOCH
    )
    {
        return 0;
    }

    return static_cast<int64_t>(
        (
            value.QuadPart -
            UNIX_EPOCH
        ) /
        10000000ULL
    );
}


// ============================================================
// PotPlayer
// ============================================================

static HWND FindPotPlayer()
{
    HWND hwnd =
        FindWindowW(
            L"PotPlayer64",
            nullptr
        );

    if (!hwnd)
    {
        hwnd =
            FindWindowW(
                L"PotPlayer",
                nullptr
            );
    }

    return hwnd;
}


static std::wstring GetPotPlayerTitle(
    HWND hwnd)
{
    wchar_t title[2048] = {};

    GetWindowTextW(
        hwnd,
        title,
        ARRAYSIZE(title)
    );

    std::wstring result(title);

    if (result.empty())
        return {};

    const std::wstring suffix =
        L" - PotPlayer";

    if (
        result.size() >=
        suffix.size()
    )
    {
        size_t position =
            result.size() -
            suffix.size();

        if (
            _wcsicmp(
                result.c_str() +
                    position,
                suffix.c_str()
            ) == 0
        )
        {
            result.resize(
                position
            );
        }
    }

    if (
        _wcsicmp(
            result.c_str(),
            L"PotPlayer"
        ) == 0
    )
    {
        return {};
    }

    return result;
}


// ============================================================
// PotPlayer playback
// ============================================================

static int64_t GetPotPlayerPosition(
    HWND hwnd)
{
    LRESULT result =
        SendMessageW(
            hwnd,
            WM_USER,
            PP_GET_POSITION,
            0
        );

    if (result < 0)
        return -1;

    return static_cast<int64_t>(
        result
    );
}


static int GetPotPlayerPlayStatus(
    HWND hwnd)
{
    LRESULT result =
        SendMessageW(
            hwnd,
            WM_USER,
            PP_GET_PLAY_STATUS,
            0
        );

    return static_cast<int>(result);
}


static int64_t GetPotPlayerDuration(
    HWND hwnd)
{
    LRESULT result =
        SendMessageW(
            hwnd,
            WM_USER,
            PP_GET_DURATION,
            0
        );

    if (result <= 0)
        return -1;

    return static_cast<int64_t>(
        result
    );
}


// ============================================================
// Reset state
// ============================================================

static void ResetMediaState()
{
    g_currentTitle.clear();
    g_currentSearchTitle.clear();

    g_currentDuration = -1;
    g_currentPosition = -1;

    g_startTimestamp = 0;

    g_cachedPosterUrl.clear();

    g_tmdbLookupAttempted =
        false;
}


// ============================================================
// Update presence
// ============================================================

static void UpdatePresence()
{
    HWND hwnd =
        FindPotPlayer();

    if (!hwnd)
    {
        if (!g_currentTitle.empty())
        {
            ClearDiscordActivity();

            ResetMediaState();
        }

        return;
    }

    std::wstring title =
        GetPotPlayerTitle(hwnd);

    if (title.empty())
    {
        if (!g_currentTitle.empty())
        {
            ClearDiscordActivity();

            ResetMediaState();
        }

        return;
    }

    int64_t position =
        GetPotPlayerPosition(hwnd);

    int64_t duration =
        GetPotPlayerDuration(hwnd);

    int playStatus =
        GetPotPlayerPlayStatus(hwnd);

    bool paused =
        playStatus == PP_STATUS_PAUSED;

    if (
        position < 0 ||
        duration <= 0
    )
    {
        return;
    }

    bool newVideo =
        title !=
            g_currentTitle ||
        g_currentDuration <= 0;

    bool seeked =
        !newVideo &&
        llabs(
            position -
            g_currentPosition
        ) > 5000;

    // --------------------------------------------------------
    // New video
    // --------------------------------------------------------

    if (newVideo)
    {
        g_currentTitle =
            title;

        g_currentDuration =
            duration;

        g_currentPosition =
            position;

        g_tmdbLookupAttempted =
            false;

        g_currentSearchTitle.clear();

        g_cachedPosterUrl.clear();

        int64_t now =
            GetUnixTime();

        int64_t positionSeconds =
            position / 1000;

        g_startTimestamp =
            now -
            positionSeconds;

        std::wstring cleanTitle =
            CleanMediaTitle(title);

        std::string posterUrl =
            GetTmdbArtwork(
                cleanTitle
            );

        // Use cleaned title for Discord.
        std::string displayTitle =
            WideToUtf8(
                cleanTitle.empty()
                    ? title
                    : cleanTitle
            );

        int64_t endTimestamp =
            g_startTimestamp +
            duration / 1000;

        SetDiscordActivity(
            displayTitle,
            posterUrl,
            g_startTimestamp,
            endTimestamp
        );

        return;
    }

    // --------------------------------------------------------
    // Pause handling
    // --------------------------------------------------------

    // While paused, Discord keeps counting from the timestamp we gave it.
    // Re-anchor the start timestamp to the current media position on each
    // poll so the displayed elapsed time remains frozen. When playback
    // resumes, the same timestamp naturally continues from that position.
    if (paused)
    {
        int64_t now =
            GetUnixTime();

        int64_t positionSeconds =
            position / 1000;

        g_startTimestamp =
            now -
            positionSeconds;
    }

    // --------------------------------------------------------
    // Seek detection
    // --------------------------------------------------------

    if (seeked && !paused)
    {
        int64_t now =
            GetUnixTime();

        int64_t positionSeconds =
            position / 1000;

        g_startTimestamp =
            now -
            positionSeconds;
    }

    g_currentPosition =
        position;

    g_currentDuration =
        duration;

    int64_t endTimestamp =
        g_startTimestamp +
        duration / 1000;

    std::wstring cleanTitle =
        CleanMediaTitle(title);

    std::string displayTitle =
        WideToUtf8(
            cleanTitle.empty()
                ? title
                : cleanTitle
        );

    SetDiscordActivity(
        displayTitle,
        g_cachedPosterUrl,
        g_startTimestamp,
        endTimestamp
    );
}


// ============================================================
// Worker
// ============================================================

static DWORD WINAPI WorkerThread(
    LPVOID)
{
    Sleep(1000);

    while (
        InterlockedCompareExchange(
            &g_stopWorker,
            0,
            0
        ) == 0
    )
    {
        UpdatePresence();

        // Poll every 2 seconds.
        for (
            int i = 0;
            i < 20;
            ++i
        )
        {
            if (
                InterlockedCompareExchange(
                    &g_stopWorker,
                    0,
                    0
                ) != 0
            )
            {
                break;
            }

            Sleep(100);
        }
    }

    ClearDiscordActivity();

    DisconnectDiscord();

    return 0;
}


// ============================================================
// Windhawk initialization
// ============================================================

BOOL Wh_ModInit()
{
    Wh_Log(
        L"PotPlayer Discord Rich Presence v2.1: Init"
    );

    // --------------------------------------------------------
    // Discord Application ID
    // --------------------------------------------------------

    PCWSTR setting =
        Wh_GetStringSetting(
            L"application_id"
        );

    if (setting)
    {
        g_applicationId =
            setting;

        Wh_FreeStringSetting(
            setting
        );
    }

    // --------------------------------------------------------
    // TMDB token
    // --------------------------------------------------------

    setting =
        Wh_GetStringSetting(
            L"tmdb_token"
        );

    if (setting)
    {
        g_tmdbToken =
            setting;

        Wh_FreeStringSetting(
            setting
        );
    }

    // --------------------------------------------------------
    // PotPlayer icon asset
    // --------------------------------------------------------

    setting =
        Wh_GetStringSetting(
            L"small_image_asset"
        );

    if (setting)
    {
        g_smallImageAsset =
            setting;

        Wh_FreeStringSetting(
            setting
        );
    }

    if (
        g_smallImageAsset.empty()
    )
    {
        g_smallImageAsset =
            L"potplayer";
    }

    // --------------------------------------------------------
    // TMDB language
    // --------------------------------------------------------

    setting =
        Wh_GetStringSetting(
            L"language"
        );

    if (setting)
    {
        g_tmdbLanguage =
            setting;

        Wh_FreeStringSetting(
            setting
        );
    }

    // --------------------------------------------------------
    // Validate Discord ID
    // --------------------------------------------------------

    if (
        g_applicationId.empty() ||
        g_applicationId ==
            L"000000000000000000"
    )
    {
        Wh_Log(
            L"Discord Application ID is not configured."
        );

        return TRUE;
    }

    // --------------------------------------------------------
    // Validate TMDB token
    // --------------------------------------------------------

    if (g_tmdbToken.empty())
    {
        Wh_Log(
            L"WARNING: TMDB token is empty."
        );

        Wh_Log(
            L"TMDB artwork will be unavailable."
        );
    }

    // --------------------------------------------------------
    // Load WinHTTP
    // --------------------------------------------------------

    if (!LoadWinHttp())
    {
        Wh_Log(
            L"WARNING: WinHTTP could not be loaded."
        );
    }

    InterlockedExchange(
        &g_stopWorker,
        0
    );

    g_workerThread =
        CreateThread(
            nullptr,
            0,
            WorkerThread,
            nullptr,
            0,
            nullptr
        );

    if (!g_workerThread)
    {
        Wh_Log(
            L"Failed to create worker thread."
        );

        UnloadWinHttp();

        return FALSE;
    }

    return TRUE;
}


// ============================================================
// Windhawk uninitialization
// ============================================================

void Wh_ModUninit()
{
    Wh_Log(
        L"PotPlayer Discord Rich Presence v2.1: Uninit"
    );

    InterlockedExchange(
        &g_stopWorker,
        1
    );

    if (g_workerThread)
    {
        WaitForSingleObject(
            g_workerThread,
            5000
        );

        CloseHandle(
            g_workerThread
        );

        g_workerThread =
            nullptr;
    }

    ClearDiscordActivity();

    DisconnectDiscord();

    UnloadWinHttp();
}
