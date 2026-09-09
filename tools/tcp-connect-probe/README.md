# TCP connect probe (MorphOS)

Misst **blocking** vs **non-blocking** `connect()` unter MorphOS/`bsdsocket.library` — Grundlage für Abort während „Verbinden…“.

## HW-Ergebnis (2026-09-04)

Ursache von AmigaGPT **8849** bestätigt:

| Lauf | C-`errno` nach NB-`connect` | `Errno()` | NB wenn C-errno geprüft | NB wenn `Errno()` geprüft |
| ---- | --------------------------- | --------- | ----------------------- | ------------------------- |
| mit `SetErrnoPtr(&errno)` | 36 (`EINPROGRESS`) | 36 | OK | OK |
| **ohne** `SetErrnoPtr` (wie AmigaGPT heute) | **0 (stale)** | 36 | **FAIL** (Hard-Error) | OK |

Blackhole `192.0.2.1`: WaitSelect-Timeout nach N Sekunden, **ohne Reboot**.

**App-Fix (Branch `feature/morphos-connect-abort`):** `SetErrnoPtr` in `initOpenAIConnector`, MorphOS `connectSocketWithMuiPump()` (FIONBIO + `Errno()`/`errno` + WaitSelect + `SO_ERROR`, 45 s, MUI-Pump/Abort).

## Build (WSL)

```bash
make -C tools/tcp-connect-probe
```

Output (nicht im Git): `bin/tcp_connect_probe`

## Deploy

```text
RAM:tcp_connect_probe
```

## Run

```text
RAM:tcp_connect_probe
RAM:tcp_connect_probe --no-set-errno-ptr
RAM:tcp_connect_probe --blackhole --timeout 8
RAM:tcp_connect_probe --host example.com --port 80
```

### Schritte

| Step | Bedeutung |
| ---- | --------- |
| 1–4 | Start, `bsdsocket`, `SetErrnoPtr`, DNS |
| 10 | Blocking `connect` (wie AmigaGPT heute) |
| 20 | NB: `EINPROGRESS` aus **C-errno** |
| 30 | NB: `EINPROGRESS` aus **`Errno()`** |
| 90–99 | Cleanup / Ende |

`--blackhole` nutzt `192.0.2.1` (kein Blocking-Versuch).
