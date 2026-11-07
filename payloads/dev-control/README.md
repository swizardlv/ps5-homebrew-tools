# dev-control

Public build name: `dev-control`. Resident dev daemon for fast iteration:
kill the running app and fetch the newest system screenshot without
re-sending a one-shot payload each time.

## Build and run

From the repository root, with `PS5_PAYLOAD_SDK` set (or use the
reference container):

```sh
make dev-control
python3 host/send_payload.py out/dev-control.elf \
  --host "$PS5_HOST" --log captures/dev-control-001.log
```

The payload stays resident after the loader connection closes; the log
ends at `dev-control: ready pid=... port=9098`. Send it once per boot.
It listens on TCP **9098**, one command per connection.

```sh
python3 host/dev_ctl.py --host "$PS5_HOST" ping
python3 host/dev_ctl.py --host "$PS5_HOST" list
python3 host/dev_ctl.py --host "$PS5_HOST" kill            # ACTIVE app
python3 host/dev_ctl.py --host "$PS5_HOST" kill PPSA12345
python3 host/dev_ctl.py --host "$PS5_HOST" kill eboot.bin
python3 host/dev_ctl.py --host "$PS5_HOST" shot -o capture.png
```

Typical re-test loop: `make dev-control`, send the new app payload,
then `python3 host/dev_ctl.py --host "$PS5_HOST" kill` before the next
send. Note the daemon itself is a separate payload; killing the app
does not kill the daemon.

## Protocol

One line per connection; the payload closes the connection after the
reply. Text commands reply with lines ending in `OK:...` or `ERR:...`.

| Command | Reply |
| --- | --- |
| `PING` | `OK:PONG` |
| `LIST` | `proc pid=.. ppid=.. stat=.. nthr=.. comm=.. title=..` per process, then `OK:LIST n=..` |
| `KILL:ACTIVE` | `OK:KILLED app=<id>` or `OK:KILLED pid=.. count=..` / `ERR:NOT_FOUND` |
| `KILL:PPSAxxxxx` | As above, by title id |
| `KILL:<pid>` | `OK:KILLED pid=.. count=..` / `ERR:NOT_FOUND` |
| `KILL:<comm\|title>` | `OK:KILLED pid=.. count=..` / `ERR:NOT_FOUND` |
| `SHOT` | `OK:SHOT <png\|jpg> <bytes>` + raw bytes, or `ERR:NO_SCREENSHOT` |

## Kill behaviour

Stage A (system path): loads `libSceSystemService.sprx` and resolves
`sceLncUtilGetAppIdOfRunningBigApp` / `sceSystemServiceGetAppIdOfRunningBigApp`,
`sceLncUtilGetAppId`, and `sceLncUtilKillApp` / `sceSystemServiceKillApp`
by `sceKernelDlsym`, so a foreground app is asked to exit through the
system. Every symbol is optional: a missing symbol falls through to
stage B instead of crashing.

Stage B (sysctl path): reuses the `process-snapshot` / `process-watch`
enumeration (13.60 `kinfo_proc` offsets: pid 72, ppid 76, stat 388,
comm 447, nthr 596) and `SIGKILL`s every process whose `comm` or
`title` (via `sceKernelGetAppInfo`) matches, or whose pid matches a
numeric target. The daemon never kills its own pid, and it refuses
(`ERR:REFUSED`) to `SIGKILL` Shell / SysCore processes: killing the
shell bricks the session.

## Screenshot behaviour and limits

`SHOT` streams the newest `.png` / `.jpg` file found by a recursive
scan under `/user/av_contents/photo/` — take a screenshot with the Create
button first. Real screenshots sit two levels down, at
`photo/NPXS40087/<TITLE_ID>/<3-hex>/<stamp>.jpg` (verified on the base-model
PS5 2026-11-07; an earlier top-level-only scan missed every real screenshot).
If no image file is found it answers `ERR:NO_SCREENSHOT` and the host tool
writes nothing.

This is **not** a live framebuffer capture. The SDK v0.44 ships no
screenshot library and this repository has no validated framebuffer
source, so no raw `/dev/dmem` or `kernel_copyout` path is attempted:
inventing one would return bytes that were never a frame. Adding a
live capture needs a validated capture source first.

## Compatibility and failures

Record offsets come from PS5 Pro 13.60 research. The tool does not
explicitly gate firmware/model. It makes no kernel-memory accesses,
hooks, or console-file writes beyond the shared SDK runtime's
behaviour. `ERR:QUERY_FAILED` reports a sysctl problem;
`ERR:UNKNOWN_CMD` an unrecognised command. The payload's stdout
(`dev-control: ready ...`) is only visible on the loader connection
that started it; later `dev_ctl.py` traffic is plain TCP to port 9098.
