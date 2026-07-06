---
name: darktide-dt-cli
description: Use the Darktide dt-cli tool shipped with the LuaExec mod to execute Lua in a running Warhammer 40,000: Darktide game, stream mod-visible logs, check the tool version, diagnose LuaExec pipe availability, or interpret dt-cli exec/logs outputs.
---

# Darktide dt-cli

`dt-cli` is an external command-line client for the `LuaExec` Darktide mod. It connects to the running game through the local LuaExec named pipe and can execute Lua code or stream logs captured by the mod.

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

## Commands

Print the tool version:

```powershell
& $dtcli version
```

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

Stream new logs captured after the command starts:

```powershell
& $dtcli logs
```

`logs` is a long-running command. Stop it with the user's requested process-control method, such as Ctrl+C in an interactive shell or an explicit process stop in automation.

## Output Semantics

`exec` prints JSON to stdout. Always parse `ok` and `error` before trusting the result:

```json
{"ok":true,"output":"1","result":{"count":1,"values":[{"type":"number","value":1}]}}
```

On Lua errors, invalid input, empty stdin, or unavailable pipe, `exec` still prints JSON and exits with code `1`.

The standard pipe-unavailable error is:

```json
{"ok":false,"error":"Darktide pipe is not available. The game is not running, or LuaExec is not loaded."}
```

`logs` prints captured log lines to stdout. Diagnostic messages, such as dropped log-line counts, go to stderr.

## Logging Scope

`dt-cli logs` is not a full engine console-log tail. It streams logs that `LuaExec` captures from the Lua/mod layer, including:

- Lua `__print`, `__print_warning`, and `__print_error`.
- DMF mod logging methods.
- Crashify output captured by LuaExec.

It does not guarantee capture of native engine logs or `Application.*` output that bypasses Lua print hooks.

## Safety

Treat `exec` as arbitrary code execution inside the game Lua VM. Execute only trusted Lua. Keep snippets short and focused because execution happens on the game Lua side and can affect gameplay state.
