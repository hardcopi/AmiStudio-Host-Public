# Windows port (required before Host 1.0)

All OS calls go through `src/plat.h`. `src/plat_posix.c` is the Linux/macOS layer; `src/plat_win32.c` is the
Windows layer (Winsock 2, `_beginthreadex`, CreateProcess in a kill-on-close job object, BCryptGenRandom,
`%APPDATA%\AmiStudio-Host`, Ctrl-C/console-close handler). The rest of the Host is portable C.

**Status (Host 0.1):** `make win-check` compiles every source file with MinGW-w64 (Linux OpenSSL headers stand in
for the Windows ones, nothing is linked). It has **not** been linked or run on Windows yet.

## To finish
1. Build with OpenSSL for MinGW (MSYS2: `pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-openssl`), link
   `-lssl -lcrypto -lws2_32 -lbcrypt -lcrypt32 -lshell32`, static where possible. Add a `make windows` target.
2. CA certificates: load the Windows system store into the SSL_CTX (CertOpenSystemStore "ROOT" →
   X509_STORE_add_cert), since there is no /etc/ssl on Windows.
3. Private files: give the config dir an owner-only DACL (today it relies on %APPDATA% being per-user).
4. Run `test/run_tests.py` on Windows (it uses `openssl` and Python only; `lan_ip()` needs a Windows variant).
5. Background start: a Startup-folder shortcut or a Task Scheduler task "at log on" (a real Windows service is
   not needed: Chrome must run in the user's session). WinUAE/AmiKit users connect over 127.0.0.1.
6. Browser backend: needs a Windows build of asb-chromed (Browser repo) or the Host's future in-process module.
7. Package `amistudio-host-<ver>-windows-x64.zip` and publish with tools/publish.sh `--extra`.
