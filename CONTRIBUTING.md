# Contributing

Contributions are welcome: bug reports, fixes, tests, documentation, and new parts that audio products need. For anything larger than a fix, open an issue first, so the design is agreed before the work starts. The [architecture](Docs/02_architecture.md) sets out the modules and the rules they follow, and the [ToDo](Docs/ToDo.md) shows what comes next.

## Building and testing

You need CMake 3.25 or later and a C++20 compiler. Continuous integration builds with the current Xcode, Visual Studio, GCC and Clang.

```sh
cmake --preset mac          # or windows, or linux
cmake --build --preset mac
ctest --preset mac
```

Everything read from outside has a fuzz target in `Tests/Fuzz`. A build with `SYMBOLENGINE_FUZZ`, as the `ci-fuzz` preset makes it with a clang that has libFuzzer, and `python3 Tests/Fuzz/run.py <build folder> <seconds>` run them. An input that breaks one goes in its folder in `Tests/Fuzz/Seeds`, so it stays a test on every system.

## Code

- C++20 and its standard library. A new dependency needs a permissive license (MIT, ISC, BSD, zlib, Boost or Apache 2.0) and is fetched by CMake, pinned by its checksum.
- Formatted with the repository's `.clang-format`.
- Every change comes with a test that fails without it.
- The audio thread never allocates, locks or waits.
- Warnings are errors in continuous integration, on every system.
- Each public function's comment says what it does, which thread may call it, and what it throws.

## Signing off

Each commit carries a `Signed-off-by` line with its author's name and email, certifying the [Developer Certificate of Origin 1.1](https://developercertificate.org/): that you made the change, or otherwise have the right to give it, under the project's MIT license. Contributions come in under the license they go out under, and there is no agreement to sign. `git commit -s` adds the line from your git settings, and `git rebase --signoff main` adds it to commits already made. Continuous integration checks every commit in a pull request.

## Security problems

Please report these privately, as [SECURITY.md](SECURITY.md) says, not in an issue.
