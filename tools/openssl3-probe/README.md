# openssl3.library probe (MorphOS)

Minimal HTTPS probe to compare **shared** (`openssl3.library`) vs **static** OpenSSL without AmigaGPT.

## Build (WSL)

```bash
make -C tools/openssl3-probe all
```

Outputs (not in git; `make clean` removes `bin/`):

- `bin/openssl3_probe_shared` (~55 KB, `-lssl_shared -lcrypto_shared`)
- `bin/openssl3_probe_static` (~4.5 MB, `-lssl -lcrypto`)

## Deploy

Copy the built binaries to MorphOS `RAM:` (or `Work:Tmp/`):

```text
RAM:openssl3_probe_shared
RAM:openssl3_probe_static
```

`Protect … +rwed` if needed.

## Run

Default **opens** `bsdsocket.library` before TLS (`g_socket_first = 1`). `--socket-first` is a no-op.

```text
RAM:openssl3_probe_shared
RAM:openssl3_probe_shared --no-socket-first
RAM:openssl3_probe_shared --stop-after 5
RAM:openssl3_probe_static
```

`--stop-after N` completes steps **1..N** then exits (`end_step` in `openssl3_probe.c`). Steps:

1. start
2. `OpenLibrary(bsdsocket.library)` or skip (`--no-socket-first`)
3. `createSSLContext` (`OPENSSL_init_ssl` + `SSL_CTX_new`)
4. `SSL_new`
5. `gethostbyname`
6. `socket` + `connect`
7. `SSL_set_fd` + SNI + `SSL_connect`
8. cleanup
9. SUCCESS

Last printed `[n] …` / `[n] OK` is the crash/hang frontier.
