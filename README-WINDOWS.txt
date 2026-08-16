ccode for Windows (WinXP SP2+ / Win7, 32-bit)
=============================================

Files:
  ccode.exe       Single binary: TUI (console renderer), interactive REPL,
                  one-shot CLI and JSON mode
  ccode-cli.exe   Same engine, CLI-only entry point
  cacert.pem      Mozilla CA bundle - REQUIRED for HTTPS (keep next to the exe)

Quick start (cmd.exe):
  set CCODE_API_BASE=https://your-api-endpoint/v1
  set CCODE_API_KEY=sk-...
  set CCODE_MODEL=your-model
  ccode.exe                      (fullscreen TUI on a console)
  ccode.exe --no-tui             (line-based REPL instead of the TUI)
  ccode.exe -p "hello"           (one-shot prompt)
  ccode.exe --json               (JSON Lines backend on stdin/stdout)

TUI keys:
  Enter submit · arrows/Home/End edit · PgUp/PgDn scroll · Ctrl-L redraw
  Ctrl-C cancel/exit · /help /clear /thinking /reasoning /exit

Notes for XP/Win7:
  * TLS is handled by built-in mbedTLS (TLS 1.2), independent of the OS's
    Schannel, so modern HTTPS APIs work even on XP. cacert.pem must sit next
    to ccode.exe (or set CCODE_CA_FILE to its full path).
  * The TUI renders through the classic console API (16-color attribute
    palette; truecolor themes are approximated). Unicode text is written as
    UTF-16; glyphs depend on the console font (raster fonts on XP lack some
    symbols, e.g. the ● marker may show as ?).
  * Interactive input reads console key events; pasting multi-byte text
    follows the console's own encoding.
  * The bash tool runs through cmd.exe /c. run_command searches PATH for
    .exe/.cmd/.bat. git tools need git.exe on PATH.
  * When stdin/stdout are redirected (pipes/files), ccode.exe automatically
    falls back to the plain REPL so it stays scriptable.
  * Sessions are stored under %USERPROFILE%\.ccode\sessions (or
    CCODE_SESSION_DIR).
  * Sub-agents run sequentially instead of in parallel processes
    (no fork() on Windows); command timeout/cancel uses TerminateProcess,
    so a killed cmd.exe may leave its own children behind.

Build (Ubuntu/Debian):
  sudo apt install gcc-mingw-w64-i686
  make WIN32=1 ccode ccode-cli
