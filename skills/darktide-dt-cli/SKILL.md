---
name: darktide-dt-cli
description: "Execute Lua code in a running Warhammer 40,000: Darktide game or read its logs with the dt-cli tool shipped with LuaExec. Use when the user asks to inspect or change live game state through Lua, retrieve recent logs, or follow new logs in real time."
---

# Darktide dt-cli

`dt-cli` is the external command-line client shipped with the `LuaExec` Darktide mod. It executes Lua code and reads game logs.

## Locate The Executable

The Darktide game directory is the only installation-specific input. Distributed installs are expected to place `dt-cli.exe` in the installed mod's `LuaExec/bin` directory:

```text
LuaExec/bin/dt-cli.exe
```

Resolve `<LuaExec bin>` from the Darktide game directory:

1. First identify the user's Darktide game directory.
2. From that directory, use `<game>/mods/LuaExec/bin/dt-cli.exe`.
3. Verify that `dt-cli.exe` exists at the derived path before running it.
4. If the game directory is not known, ask the user for the Darktide game directory. Do not scan entire drives.

In PowerShell, store the resolved executable path in a variable and pass arguments as separate command arguments:

```powershell
$dtcli = '<LuaExec bin>\dt-cli.exe'
& $dtcli version
```

## Preconditions

Before using game-connected commands:

- Darktide must be running.
- The `LuaExec` mod must be loaded.
- The current execution environment must be allowed to connect to local named pipes.

If an environment sandbox returns access denied for the named pipe, rerun the same command outside that sandbox or with the required approval. Do not treat sandbox access denial as proof that the game or mod is broken.

## Execute Lua

Execute one line of Lua in the game:

```powershell
& $dtcli exec 'return 1'
```

Execute multiline Lua through stdin:

```powershell
@'
return Managers and type(Managers)
'@ | & $dtcli exec --stdin
```

`exec` prints JSON to stdout. Always parse `ok` and `error` before trusting the result:

```json
{"ok":true,"output":"1","result":{"count":1,"values":[{"type":"number","value":1}]}}
```

On Lua errors, invalid input, empty stdin, or an unavailable pipe, `exec` prints an error JSON object and exits with code `1`:

```json
{"ok":false,"error":"Darktide pipe is not available. The game is not running, or LuaExec is not loaded."}
```

Treat `exec` as arbitrary code execution inside the game Lua VM. Execute only trusted Lua and keep snippets focused because they can change live game state.

## Read Logs

Print the latest 10 retained log lines and exit:

```powershell
& $dtcli logs
```

Choose the historical line count:

```powershell
& $dtcli logs -n 100
```

Print the latest 10 lines and continue following:

```powershell
& $dtcli logs -f
```

Follow only future lines:

```powershell
& $dtcli logs -n 0 -f
```

`logs` prints log lines to stdout and diagnostics to stderr. Without `-f`, it exits after the requested history. With `-f`, it continues printing new lines.

`logs` reads only the history retained by the running Darktide process. It cannot read earlier game sessions or existing log files. If less history is available than requested with `-n`, it prints the available lines.

## Multiple Processes

If more than one Darktide process is available to `dt-cli`, list them with:

```powershell
& $dtcli ps
```

The output contains the process ID, uptime, and full executable path:

```text
PID    UPTIME   PATH
18432  00:42:17 E:\SteamLibrary\steamapps\common\Warhammer 40,000 DARKTIDE\binaries\Darktide.exe
```

Select the target process with the global `--pid` or `-p` option before `exec` or `logs`:

```powershell
& $dtcli --pid 18432 exec 'return 1'
& $dtcli -p 18432 logs -f
```

## Print The Version

```powershell
& $dtcli version
```

`version` prints the installed `dt-cli` version and exits.
