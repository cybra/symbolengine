#include "symbolengine/net/Http.h"

#include "symbolengine/core/Text.h"

#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace symbolengine::http
{
namespace
{

std::wstring wideFrom(std::string_view text)
{
    if (text.empty())
        return {};

    std::wstring wide(static_cast<std::size_t>(
                          MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0)),
                      L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                        static_cast<int>(wide.size()));
    return wide;
}

std::string problemWith(const char* doing)
{
    const auto code = GetLastError();
    wchar_t* words = nullptr;
    const auto length =
        FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       GetModuleHandleW(L"winhttp.dll"), code, 0, reinterpret_cast<wchar_t*>(&words), 0, nullptr);
    std::string text = std::string(doing) + ": error " + std::to_string(code);

    if (length > 0 && words != nullptr)
    {
        const auto bytes =
            WideCharToMultiByte(CP_UTF8, 0, words, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
        std::string said(static_cast<std::size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, words, static_cast<int>(length), said.data(), bytes, nullptr, nullptr);
        text = std::string(text::trim(said));
    }

    LocalFree(words);
    return text;
}

struct CloseInternet
{
    void operator()(HINTERNET handle) const noexcept { WinHttpCloseHandle(handle); }
};

using Internet = std::unique_ptr<void, CloseInternet>;

}

bool available()
{
    return true;
}

Answer get(const Request& request, const std::function<bool(int status, std::string_view piece)>& received)
{
    Answer answer;
    auto wide = wideFrom(request.url);

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    parts.dwSchemeLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts) ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS))
    {
        answer.problem = "That isn't a web address: " + request.url;
        return answer;
    }

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    // What follows a # stays here, where the system's call keeps it with the query.
    std::wstring path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) +
                        std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    path.erase(std::min(path.find(L'#'), path.size()));

    const Internet session(WinHttpOpen(L"SymbolEngine", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS, 0));

    if (session == nullptr)
    {
        answer.problem = problemWith("Opening the connection");
        return answer;
    }

    const auto wait = static_cast<int>(request.timeout.count());
    WinHttpSetTimeouts(session.get(), wait, wait, wait, wait);

    const Internet connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    const Internet asked(connection != nullptr
                             ? WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                  WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                  parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                             : nullptr);

    if (asked == nullptr)
    {
        answer.problem = problemWith("Opening the connection");
        return answer;
    }

    std::wstring headers;

    for (const auto& [name, value] : request.headers)
        headers += wideFrom(name) + L": " + wideFrom(value) + L"\r\n";

    if (!WinHttpSendRequest(asked.get(), headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            static_cast<DWORD>(headers.size()), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(asked.get(), nullptr))
    {
        answer.problem = problemWith("Reaching the server");
        return answer;
    }

    DWORD status = 0, statusSize = sizeof status;

    if (WinHttpQueryHeaders(asked.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX))
        answer.status = static_cast<int>(status);

    // WinHTTP takes a connection lost partway for the answer's end, so what came is held to the length it said it was.
    wchar_t stated[32]{};
    DWORD statedSize = sizeof stated;
    const auto hasLength = WinHttpQueryHeaders(asked.get(), WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                               stated, &statedSize, WINHTTP_NO_HEADER_INDEX) != FALSE;
    const auto length = hasLength ? std::wcstoull(stated, nullptr, 10) : 0ULL;
    unsigned long long total = 0;

    std::vector<char> piece(1 << 16);

    for (;;)
    {
        DWORD read = 0;

        if (!WinHttpReadData(asked.get(), piece.data(), static_cast<DWORD>(piece.size()), &read))
        {
            answer.problem = problemWith("Reading the answer");
            return answer;
        }

        if (read == 0)
        {
            if (hasLength && total < length)
                answer.problem = "The connection was lost before the whole answer came.";

            return answer;
        }

        total += read;

        if (!received(answer.status, std::string_view(piece.data(), read)))
            return answer;
    }
}

}
