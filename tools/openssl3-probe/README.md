# openssl3.library probe (MorphOS)

Minimal HTTPS probe to compare **shared** (`openssl3.library`) vs **static** OpenSSL without AmigaGPT.

## Build (WSL)

```bash
make -C tools/openssl3-probe all
```

Outputs:

- `bin/openssl3_probe_shared` (~55 KB, `-lssl_shared -lcrypto_shared`)
- `bin/openssl3_probe_static` (~4.5 MB, `-lssl -lcrypto`)

## Deploy

Copy to MorphOS `RAM:` (or `Work:Tmp/`):

```text
RAM:openssl3_probe_shared
RAM:openssl3_probe_static
```

`Protect … +rwed` if needed.

## Run

```text
RAM:openssl3_probe_shared
RAM:openssl3_probe_shared --socket-first
RAM:openssl3_probe_shared --stop-after 5
RAM:openssl3_probe_static
```

`--stop-after N` completes steps **1..N** then exits. Steps:

1. start  
2. optional `OpenLibrary(bsdsocket)` (`--socket-first`)  
3. `OPENSSL_init_ssl`  
4. `SSL_CTX_new`  
5. `SSL_new`  
6. `gethostbyname`  
7. `socket` + `connect`  
8. `SSL_set_fd` + `SSL_connect`  
9. cleanup  
10. SUCCESS  

Last printed `[n] …` / `[n] OK` is the crash/hang frontier.
