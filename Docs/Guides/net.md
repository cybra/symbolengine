# Fetching from the web

The net module fetches from web servers through the system's own client, NSURLSession on a Mac and WinHTTP on Windows. Your program uses the computer's certificates and proxies and its way of following redirects, and carries no network library of its own.

```cmake
target_link_libraries(my_app PRIVATE SymbolEngine::net)
```

[`Examples/Fetch`](../../Examples/Fetch/Fetch.cpp) is a whole program in about 40 lines, built on a Mac and Windows. It fetches an address and prints the status and size of what came back. Everything below is about what it does, and what a larger program does beyond it.

```cpp
#include <symbolengine/net/Http.h>

using namespace symbolengine;
```

That one header is the whole module, in the namespace `symbolengine::http`. It gets what is at an address. It has no way to send a body or to read the headers of an answer.

## Fetching

```cpp
std::string body;

const auto answer = http::get({"https://example.com/notes.txt"}, [&body](int, std::string_view piece) {
    body += piece;
    return true;
});
```

`get` waits until the answer has come. Each piece goes to your function as it arrives, in order, on a thread of the system's while `get` waits, so what only your function and the code after `get` touch needs no lock. Give back `true` to carry on and `false` to stop.

As it waits, call it on a thread of your own in a program with windows, and never on an audio thread.

## What came back

Look at both `problem` and `status`:

| `status` | `problem` | What happened |
|---|---|---|
| 0 | Set | No server answered: the address isn't a web address, nothing listens there, the name doesn't exist or time ran out |
| A status | Empty | The whole answer came, or your function stopped it |
| A status | Set | The answer stopped short. Pieces may have been held back that never reached your function, so what you hold says nothing of how much came |

A 404 or a 500 is an answer with no problem, and its body reaches your function as any other does. Redirects are followed, and `status` is the last server's.

```cpp
if (!answer.problem.empty())
    showError(answer.problem);
else if (answer.status != 200)
    showError("The server answered " + std::to_string(answer.status));
```

## Saving what comes

Each piece comes with the status, so a download can leave out an error page, and stop as soon as its file can't be written:

```cpp
std::ofstream file(path, std::ios::binary);

const auto answer = http::get({url}, [&file](int status, std::string_view piece) {
    if (status != 200)
        return false;

    file.write(piece.data(), static_cast<std::streamsize>(piece.size()));
    return file.good();
});
```

Stopping leaves `problem` empty, so check `status` and `file.good()` afterward as well.

## Headers and time limits

```cpp
http::Request request{"https://example.com/updates.json"};
request.headers = {{"User-Agent", "My App/1.0"}, {"Accept", "application/json"}};
request.timeout = std::chrono::seconds(10);
```

`timeout`, 30 seconds unless you change it, is how long the connection may take and then each wait for more of the answer. A slow server that keeps sending can run far past it. For a limit on the whole fetch, look at the clock in your function and give back `false`.

## Addresses

An address is http or https, already escaped. `escaped` makes one part of a path safe, escaping every character but letters, digits and `-._~`. A slash is among those it escapes, so give it one part at a time:

```cpp
const auto url = "https://example.com/files/" + http::escaped("a b/c.txt");
```

That gives `https://example.com/files/a%20b%2Fc.txt`.

`fileAddress` and `fileAt` turn a file's full path into a `file://` address and back, for what wants a file as an address, such as a web page. `fileAt` gives nothing for any other kind of address, and `get` answers a `file://` address with a problem.

```cpp
const auto address = http::fileAddress(path);

if (const auto file = http::fileAt(address))
    openDocument(*file);
```

## Other systems

`available` says whether this system can fetch. On anything but a Mac or Windows it is false and `get` gives status 0 and a problem saying so, so a program that fetches still builds and runs there:

```cpp
if (!http::available())
    hideUpdateCheck();
```

## Testing

The module has no fake. Its own tests run a small web server on the computer's loopback address, each test scripting what it sends: statuses, redirects, a long answer in pieces, a connection cut partway and a server that says nothing. [`Modules/net/Tests/TestServer.h`](../../Modules/net/Tests/TestServer.h) and its source file aren't part of the module, and you can copy them for your own tests:

```cpp
TestServer server([](const std::string&, const TestServer::Send& send) {
    send("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
});

const auto answer = http::get({server.address("/missing")}, [](int, std::string_view) { return true; });
```

`answer.status` is then 404. [`ServedTests.cpp`](../../Modules/net/Tests/ServedTests.cpp) has a test for each case in the table above.

## What's in the reference

| Header | What it gives you |
|---|---|
| `Http.h` | `get`, `Request`, `Answer`, `available`, `escaped`, `fileAddress`, `fileAt` |

The [architecture's table of modules](../02_architecture.md#modules) says what the module is built on.
