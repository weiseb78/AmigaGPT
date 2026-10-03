# MorphOS: umgesetzte Stabilitätsmaßnahmen

Stand: **`master` 2.18**, Builds **8780–8785** (Quit/Geometrie/Relaunch-Lock).  
Ergänzt [HANDLUNGSANWEISUNG-MORPHOS-AGENT.md](HANDLUNGSANWEISUNG-MORPHOS-AGENT.md) §3 und §5 mit dem **aktuellen Code**. Nutzer-Übersicht: [MORPHOS-RELEASE-NOTES.md](MORPHOS-RELEASE-NOTES.md).

---

## 1. Ziel

Weniger **System-/App-Freezes** bei Quit, Neustart, Chat-Listenwechsel und großen Scintilla-Dokumenten.  
Diagnose über persistentes Lifecycle-Log (`AMIGAGPT:amigagpt_lifecycle.log`).

---

## 1b. Persistenz: ENVARC, AMIGAGPT, T:, Work:Tmp

| Ort | Persistent? | Inhalt |
| --- | ----------- | ------ |
| **ENVARC:** | Ja (über Neustart) | MUI: `ENVARC:mui/AmigaGPT.prefs` (`Application_Save`/`Load`); App-Zustand: `ENVARC:AmigaGPT/config.json` (Einstellungen/API-Keys), `ENVARC:AmigaGPT/last-conversation` (zuletzt gewählter Chat) |
| **AMIGAGPT:** | Ja (Daten-Volume) | `chat-history.json`, `image-history.json`, Bilder unter `images/` — **kein** UI-Zustand/Prefs mehr (Legacy: `config.json`, `last-conversation.txt` werden einmalig migriert); Lifecycle-Log `amigagpt_lifecycle.log` |
| **Work:Tmp/** | Ja (Festplatte) | MorphOS-Debug nach Hard-Reset: `amigagpt_stream.log`, `amigagpt_lifecycle.log` (Spiegel), `amigagpt_startup.last` / `amigagpt_shutdown.last` — siehe [PHASE-9-DEBUG-LOGS.md](PHASE-9-DEBUG-LOGS.md) |
| **T:** | Nein (RAM) | Nur noch Relaunch-Locks (`amigagpt_instance.lock`, `amigagpt_teardown.lock`) — nach Reset weg |

**Warum nicht nur MUI-ENVARC für die aktive Konversation?**  
`Application_Load` läuft in `createMainWindow()` **vor** `loadConversations()` — die NList ist noch leer; ein zweites Load **nach** `loadConversations()` hat die NList beim Restart kaputt gemacht (nicht wieder einführen). Beim Quit wird die Liste in `mainWindowPrepareShutdown()` **geleert**, **bevor** `Application_Save` — die aktive Zeile landet so oft nicht in `AmigaGPT.prefs`. Daher eigener Eintrag `ENVARC:AmigaGPT/last-conversation` (Chat-**Name**, nach `loadConversations()` per `restoreLastSelectedConversation()`).

Einmalige Migration: falls noch `AMIGAGPT:config.json` oder `AMIGAGPT:last-conversation.txt` existieren, werden sie beim ersten Lesen nach ENVARC übernommen (Lifecycle-Log: `config read fallback amigagpt` → `config migrate amigagpt to envarc`).

---

## 2. Quit / Shutdown

| Maßnahme | Datei / Funktion | Was |
| -------- | ---------------- | --- |
| **Prepare vor Dispose** | `gui.c` → `shutdownGUI()` | `mainWindowPrepareShutdown()` **vor** `MUIM_Application_Save` und `MUI_DisposeObject(app)` |
| **ENVARC nach Prepare** | `shutdownGUI()` | **Kein** `MUIM_Application_Save` beim Quit; Fenster-Geometrie in `config.json` (`mainWindowLeft/Top/Width/Height`) vor Teardown |
| **Kein Chat-Scintilla-Clear beim Quit** | `mainWindowPrepareShutdown()` | **Kein** `SCI_CLEARALL` / `SETTEXT ""` am Chat — kann OS einfrieren; MUI dispose räumt auf |
| **Kein Code-Scintilla-Clear beim Quit** | `codeBlocksViewerPrepareShutdown()` | Nur `KillNotify`, Zeiger nullen; Log: `scintilla skip clear` |
| **Kein `NList_Clear` Code-Viewer Shutdown** | `codeBlocksViewerPrepareShutdown()` | Clear nur bei Chat-Wechsel (`codeBlocksViewerDismiss`), nicht beim App-Ende |
| **Stream abbrechen** | `mainWindowPrepareShutdown()` | `openAIChatStreamRequestCancel()` — kein `finishChatStream` / `displayConversation` während Shutdown |
| **Deferred Styles abbrechen** | `chatOutputScintillaCancelDeferredStyles()` | Pending `PushMethod`-Styling wird verworfen |
| **Kein NewInput beim Quit** | `morphosFlushPendingPushMethods()` | Während Shutdown nur Pending-Flags löschen — **kein** `MUIM_Application_NewInput` (Reentry/Freeze) |
| **Config vor Teardown** | `main.c` → `cleanExit()` | `mainWindowCaptureGeometryForConfig()` + `writeConfig()` **vor** `shutdownGUI()` |
| **Geometrie wiederherstellen** | `morphosRunStartupDeferred()` | Nach Scintilla-Init + Fenster offen — **nicht** vor `Window_Open` (Restart-Bug 8780) |
| **Refresh-Queue leeren** | `mainWindowPrepareShutdown()` | `chatOutputRefreshPending` / `FromList` zurücksetzen |
| **Notify abklemmen** | `chatOutputScintillaDetachNotify()` | Vor Dispose, solange Hauptfenster noch offen |
| **Code-Viewer schließen** | `codeBlocksViewerCloseWindow()` in Prepare | **Nicht** erneut in `PrepareShutdown` (CloseRequest-Reentry) |
| **Pens vor Fenster zu** | `mainWindowReleasePens()` vor `MUIA_Window_Open, FALSE` | Screen noch gültig |
| **NList leeren (Hauptfenster)** | `mainWindowEmptyNList()` mit `MUIA_NList_Quiet` | Konversations-/Bilderliste vor Fenster-Close |
| **KillNotify Screen/Close** | `mainWindowPrepareShutdown()` | Kein ENVARC-Reload-Hook auf `MUIA_Window_Screen` |
| **Notify-Klasse nach Dispose** | `shutdownGUI()` | `chatOutputScintillaDisposeNotifyClass()` **nach** `MUI_DisposeObject` |
| **Post-Dispose-Cooldown** | `shutdownGUI()` | `Delay(150)` (~3 s) — MUI/Intuition soll fertig teardownen |
| **Teardown-Marker + Lock frei** | `morphos_relaunch.c` | `T:amigagpt_teardown.lock` während Dispose; Instance-Lock **sofort** zu Shutdown-Beginn frei (8782) |
| **Single-Instance-Lock** | `morphos_relaunch.c` | `T:amigagpt_instance.lock` mit Task-**Pointer** (nicht Name); Startup blockiert nur bei lebendem Peer |
| **Chat-Shutdown** | `MainWindow.c` / `ChatOutputScintilla.c` | Konversationswahl erst nach `morphos conversation select enabled`; vor Dispose Notify abklemmen, **keine** SCI-Befehle am Chat-Scintilla beim Quit |
| **Letzte Konversation** | `saveLastSelectedConversationName()` | Vor `currentConversation = NULL` in `mainWindowPrepareShutdown()` sowie bei Listenwahl → `ENVARC:AmigaGPT/last-conversation` |

---

## 3. Startup / Neustart

| Maßnahme | Datei | Was |
| -------- | ----- | --- |
| **Relaunch-Guard** | `main.c` + `morphos_relaunch.c` | Teardown-Wartezeit, Instance-Lock; **EasyRequest** bei Warten/Blockade; `T:amigagpt_startup.last` |
| **App-Create-Cooldown** | `gui.c` → `initVideo()` | `Delay(30)` vor erstem `ApplicationObject` |
| **App-Create-Retry** | `initVideo()` | Bis **12** Versuche; Delay 25, ab Versuch 5: **40** Ticks |
| **Einmal Application_Load** | `createMainWindow()` | **Kein** zweites Load nach `loadConversations()` (NList-Restart-Bug) |
| **Code-Fenster nach Load zu** | `createMainWindow()` | ENVARC darf Code-Viewer nicht vor Scintilla-Init öffnen |
| **Chat Scintilla prime/finish getrennt** | `createMainWindow()` + `morphosRunStartupDeferred()` | `chatOutputScintillaPrimeViewer` vor `OM_ADDMEMBER`; volles Init (`FinishViewerInit`) erst in erstem `NewInput` |
| **Code-Scintilla prime at startup** | `gui.c` | `codeBlocksViewerPrimeScintillaAtStartup()` — schweres Init nicht im ersten Fenster-Open |
| **Letzte Konversation wiederherstellen** | `restoreLastSelectedConversation()` nach `loadConversations()` | Liest `ENVARC:AmigaGPT/last-conversation`, setzt NList-Active per Name; Log: `restore last conversation ok` / `miss` |
| **Konversationswahl freigeben** | `morphosEnableConversationSelect()` nach `morphosRunStartupDeferred()` | Erst wenn Scintilla fertig; Log: `morphos conversation select enabled` |
| **Auto-Select Fallback** | `morphosEnableConversationSelect()` | Wenn weder ENVARC-Name noch aktive Zeile: erste Liste (`morphos conversation auto-select first`); sonst `auto-select active`; Laden per `PushMethod` (`conversation select deferred begin`) |
| **Fenster nach vorn** | `createMainWindow()` | `MUIM_Window_ToFront` nach `MUIA_Window_Open, TRUE` |

---

## 4. Chat-Anzeige (Scintilla)

| Maßnahme | Datei | Was |
| -------- | ----- | --- |
| **Deferred UI-Refresh** | `MainWindow.c` | `displayConversation` / Listenklick → `PushMethod` (`morphosScheduleChatOutputRefreshFromList`) — kein synchrones Scintilla-Update im NList-Hook |
| **Deferred Style-Apply** | `ChatOutputScintilla.c` | Nach `SCI_SETTEXT`: Styling per zweitem `PushMethod`; Scroll-Zeile (`firstVisibleLine`/`oldLineCount`) für Markdown-Toggle mit durchreichen |
| **Run-Length-Styling** | `chatOutputScintillaApplyRoleStyleBytes()` | `SCI_SETSTYLING` (Runs) statt `SCI_SETSTYLINGEX`; kein redundant `SCI_SETLEXER` vor Styling |
| **Kein Scroll bei Listenwechsel** | `chatOutputScintillaMorphosSkipViewport` | Kein `GOTOPOS`/`SCROLLCARET` bei NList-Chat-Wechsel und während Stream |
| **Schweres Scroll vermeiden** | `SetUtf8TextWithRoleStyles()` | Scroll ans Ende nur wenn nicht Skip und (≤ 24 KB oder kein Preserve); **Markdown-Toggle** (`preserveViewport`) stellt Scroll-Zeile auch bei großen Chats wieder her (`SETFIRSTVISIBLELINE`) |
| **Raw während Stream** | `morphosChatStreamRawScintillaRefresh` | Live-Antwort ohne Markdown-Parse pro Chunk |
| **Raw/Markdown per Menü** | `config.markdownFormatting` | Haken **aus** → dauerhaft Raw-Pfad; Haken **an** → Markdown außerhalb Stream (Nutzer steuert Stabilitätstest) |
| **Refresh-Race** | `morphosScheduleChatOutputRefresh()` | Setzt `FromList` nicht zurück, wenn bereits ein Listen-Refresh pending ist |
| **Conversation-Klick deferred** | `ConversationRowClicked` → `PushMethod` | Kein `displayConversation` direkt im NList-Notify |

---

## 5. Code-Blocks-Viewer

| Maßnahme | Datei | Was |
| -------- | ----- | --- |
| **Dismiss ohne Scintilla-Clear** | `codeBlocksViewerDismiss()` | Fenster zu + Liste leer; **kein** `CLEARALL` beim Chat-Wechsel |
| **ASL/ Menü deferred** | diverse Hooks | Modal-Dialoge nur über `PushMethod`, nicht aus `MUIV_Notify_Application` |
| **CloseRequest-Hook** | `gui.c` | Nur auf `CloseRequest`, **nicht** auf `MUIA_Window_Open FALSE` (Dispose-Reentry) |

---

## 6. Diagnose (Lifecycle-Log)

| Maßnahme | Datei | Was |
| -------- | ----- | --- |
| **Persistentes Log** | `streamlog.c` | `AMIGAGPT:amigagpt_lifecycle.log` + Spiegel `Work:Tmp/amigagpt_lifecycle.log` — nur wenn `debugLifecycleLog: true` in `config.json` (Default **aus**) |
| **Stream-Log (MorphOS)** | `streamlog.c` | `Work:Tmp/amigagpt_stream.log` wenn `debugStreamLog: true` — überlebt Hard-Reset (früher `T:`) |
| **Fein granulare Phasen** | überall `streamLogLifecycle()` | Startup, createMainWindow, chat settext/styling, shutdown, app-create retry |
| **KPrintF-Spiegel** | `streamLogLifecycle()` | `[AmigaGPT lifecycle]` auf Debug-Kanal (nur mit `debugLifecycleLog`) |

Für Restart-Stress-Tests: `"debugLifecycleLog": true` setzen, App neu starten, danach Block R (§8). Für Alltag beide Debug-Flags **false** lassen.

---

## 6b. Warum MCP noch geht, wenn die GUI tot ist

Beobachtung beim Chat-/Scintilla-Freeze: Maus/Tastatur und Workbench wirken tot („Application is meditating“ oder kompletter UI-Freeze, oft nur per Reset lösbar) — **Cursor/MCP (`mcpd`) auf Port 4322 antwortet trotzdem** (`fs_read`, `fs_list`, manchmal `exec_cmd` / `Reboot`).

### Getrennte Tasks

| Komponente | Task | Braucht Intuition / `NewInput`? |
| ---------- | ---- | ------------------------------- |
| **AmigaGPT** | eigener MUI-Prozess | Ja — Hauptschleife `MUIM_Application_NewInput`, Scintilla-Zeichen, Fenster, Eingabe |
| **mcpd** | eigener Hintergrund-Prozess (MCP-Daemon) | Nein für Datei/Netz-Kommandos — TCP-Server + DOS/`exec` |

MorphOS plant Tasks getrennt. Hängt **nur** der AmigaGPT-Task (oder der Input-/Redraw-Pfad, den er blockiert), bleibt der TCP-Stack und **mcpd** oft schedulbar. Deshalb kann der Agent von WSL aus Logs unter `Work:Tmp/` lesen und ggf. `Reboot` auslösen, obwohl lokal keine Maus mehr geht.

### Was typischerweise stecken bleibt

AmigaGPT (und klassische Amiga-UI) laufen kooperativ über Intuition/`input.device` und MUI. Schwere Arbeit **im UI-Task** — z. B. ein großer `SCI_APPENDTEXT` / Font-/Wrap-Lauf in Scintilla — blockiert lange oder dauerhaft:

- keine sinnvollen Input-Events mehr
- keine Fenster-Redraws
- andere MUI-Apps wirken oft mit „tot“, obwohl der Kernel und Netz noch laufen

Das ist **kein** Beweis, dass „das ganze OS tot“ ist — nur, dass der **GUI-/Input-Pfad** nicht mehr bedienbar ist. Ein echter Hard-Lock (langes `Forbid`/`Disable`, kaputtes `input.device` ohne Scheduling) kann MCP ebenfalls killen; wenn MCP noch antwortet, war es in der Praxis meist der GUI-Pfad.

### Diagnose-Nutzen

1. Flags an: `debugLifecycleLog` / `debugStreamLog` (siehe [PHASE-9-DEBUG-LOGS.md](PHASE-9-DEBUG-LOGS.md)).
2. Freeze reproduzieren → **nicht** erwarten, dass `T:`-Logs den Reset überleben.
3. Per MCP lesen: letzte Zeilen in `Work:Tmp/amigagpt_lifecycle.log` (und ggf. `AMIGAGPT:amigagpt_lifecycle.log`) — oft endet die Spur genau vor dem hängenden SCI-Befehl (z. B. `chat scintilla replace append begin` ohne `… done`).
4. Optional: MCP `Reboot` (explizit freigeben), danach Logs erneut lesen.

### Was MCP nicht rettet

- Screenshot/`SGrab`, wenn Intuition verkeilt ist
- „App sauber beenden“ ohne Reset, wenn AmigaGPT im Meditate steckt
- Inhalte nur in RAM (`T:`), die nie auf Platte geschrieben wurden

**Kurz:** MCP ≠ GUI. Daemon und Netz können leben, während AmigaGPT/Scintilla den Desktop eingefroren haben — genau deshalb persistente Logs unter `Work:Tmp/` / `AMIGAGPT:`.

---

## 6c. GUI-Task belasten — warum das gegen Desktop-Instinkt geht

**Gefühl (Windows/Linux/macOS):** Schwere Text-/Dokument-Arbeit gehört **nicht** in den UI-Thread. Richtig dort: Worker-Thread, `ILoader`/Background-Load, dann fertiges Dokument an die View hängen.

**MorphOS/MUI-Realität:** Es gibt keinen sicheren „UI-Thread + Worker für Scintilla“ wie bei Qt/Win32. MUI, Intuition und die App-Hauptschleife (`MUIM_Application_NewInput`) laufen im **selben Task**. Scintilla.mcc wird über `SCI_Command` **in diesem Task** bedient. Ein zweiter Thread, der in die Klasse schreibt, ist riskant bis undefiniert — auch wenn `pthread` unter MorphOS existiert.

### Was andere Apps typischerweise tun

| Umgebung | Typisches Muster |
| -------- | ---------------- |
| MorphOS Mail/Editor (YAM & Co.) | **TextEditor.mcc**, Text einmal setzen — kein SSE-Stream, kein Full-Rebuild großer Chat-Historien |
| Desktop-Scintilla (SciTE & Co.) | oft **inkrementell** oder `SCI_CREATELOADER` / `ILoader` aus einem **Hintergrund-Thread** |
| AmigaGPT Chat (MorphOS) | Workarounds im **UI-Task**, weil `CLEARALL`/`SETTEXT` auf großen Docs einfrieren |

AmigaGPT macht also **nicht** „was alle MorphOS-Apps so machen“, sondern kompensiert Scintilla.mcc-Grenzen plus Chat-Stream-Last.

### Warum kein Scintilla-Background-Load (`ILoader`)

1. **API/SDK:** `SCI_CREATELOADER` / `ILoader` ist für Desktop-Ports gedacht; im hier genutzten MorphOS-SDK liegt das nicht als klar nutzbare, dokumentierte App-API bereit.
2. **Thread-Modell:** Der Loader nützt vor allem, wenn `AddData` **außerhalb** des UI-Tasks läuft. Ohne sicheren Worker bleibt `AddData` im GUI-Task — dann ist es kein echter Background-Load.
3. **Danach trotzdem UI:** `ConvertToDocument` / `SETDOCPOINTER`, Wrap, Role-Styles, Hotspots laufen wieder im UI-Task — genau die teuren Schritte bei Periodensystem-Größe.
4. **Bisheriger Fokus:** Freeze-Mitigation (kein Live-Paint, Docswap + Chunk-`APPENDTEXT`, Yields) statt einer neuen Thread-/Loader-Architektur.

### Was der richtige MorphOS-Instinkt ist

Nicht: „Arbeit woanders hin verlagern“ (Desktop-Reflex).  
Sondern: **weniger und seltener** im GUI-Pfad.

| Regel | Praxis in AmigaGPT |
| ----- | ------------------ |
| Während SSE **nicht** malen | `morphosChatLiveScintillaUpdates` — Buffer nur im RAM |
| Am Ende **ein** Paint, so leicht wie möglich | Raw-Pfad, Chunk-`APPENDTEXT`, MUI-Yields dazwischen |
| Kein Markdown/Hotspot-Scan auf Riesen-Puffern live | Midi-Markdown/Links nach Stream; Hotspots bei großen Texten überspringen |
| Lifecycle-Log bei Freeze | Spur endet oft bei `chat scintilla replace append begin` ohne `… done` (§6b) |

**Phase 13** in [SCINTILLA-ARCHITECTURE.md](SCINTILLA-ARCHITECTURE.md) („Worker / UI-Batching“) meint deshalb **nicht** automatisch Desktop-`ILoader`, sondern höchstens: Arbeit in kleinere UI-Batches / kontrollierte Yields zerlegen — und nur, wenn R3 auf Hardware nicht reicht.

**Merksatz:** Instinkt „GUI-Thread nicht belasten“ ist richtig; auf MorphOS heißt die Antwort meist **Last reduzieren und stückeln**, nicht **in einen Worker verschieben**.

Typische **gute** Raw-Kette im Log:

```
chatOutput refresh raw path
chat scintilla settext done
chat scintilla apply styles defer scheduled
chat scintilla setstyling runs done
shutdown … process exit
```

Typisches **Restart-Problem** (Relaunch-Race, vor **8785**):

```
startup begin          ← neue Instanz
app create fail        ← alte Instanz noch nicht fertig mit MUI
… process exit         ← alte Instanz endet später
```

Ab **8785**: Instance-Lock früh frei, Task-Pointer, Lock-Retry — Stress-Restart hardware-validiert. Bei erneutem Fehler: `T:amigagpt_shutdown.last`, optional `debugLifecycleLog`.

Typisches **„kein Chat nach Restart“** (8753–8755, mit 8756+ behoben):

```
morphos conversation select enabled
shutdown begin           ← ohne conversation select deferred begin dazwischen
```

Ursache: keine aktive NList-Zeile und kein gespeicherter Chat-Name → mit `ENVARC:AmigaGPT/last-conversation` + Auto-Select-Fallback beheben.

**Nutzer:** Nach Quit auf `process exit` warten (**3–5 s**), dann neu starten; bei Hänger **nicht** doppelklicken.

---

## 7. Priorität / Restrisiken

**Höhere Priorität (normaler Betrieb):** Chat-Scintilla (`settext`/deferred styling), Markdown-Pfad, Stream — betrifft tägliche Nutzung.

**Timebox Restart-Race (dieser Stand):** `morphos_relaunch.c` — Teardown-Wartezeit, ~3 s Post-Dispose, Instance-Lock. Ziel: Stress-Restart und WB-Doppelstart; in normaler Nutzung selten.

**Noch offen:**

- **Markdown mit Haken an** nach Stream / bei sehr großen Chats → teurer Pfad (`markdown begin`); bei Freeze Markdown per Menü **aus** testen.
- **Scintilla-Wheel** zwischen Chat- und Code-Fenster — plattformbedingt, nur zurückhaltend workarounden.
- **GUI-/Scintilla-Freeze** — MCP oft noch erreichbar (§6b); nach Reboot `Work:Tmp/` + `AMIGAGPT:`-Logs lesen. Architektur-Kontext: §6c (GUI-Task vs. Desktop-`ILoader`).

---

## 8. Empfohlener Stabilitätstest (Nutzer)

**Block R (Restart mit Chat):**

1. Menü **Markdown-Formatierung aus** (Raw, in Config gemerkt); optional `"debugLifecycleLog": true` nur für diesen Test.
2. Log rotieren: `AMIGAGPT:amigagpt_lifecycle.log` löschen oder umbenennen.
3. Start → **bestehenden** Chat wählen (oder nach 8757: soll ohne Klick laden) → Quit → **3–5 s** Pause.
4. **15–20×** wiederholen; im Log pro Zyklus erwarten: `restore last conversation ok` oder `auto-select …`, dann `conversation select deferred begin` → `… deferred done`, dann `process exit`.
5. About-Version prüfen (z. B. **2.18.8785**).

**Block C (optional):** Eine Session, **10–20×** nur Chat in der Liste wechseln, **ohne** Restart.

**Weiteres:**

- Optional: großen Chat, Code-Viewer öffnen/schließen, dann wieder Block R.
- Wenn stabil: Markdown **an**, gleiche Schleife — vergleichen, ob nur Markdown regressiert.

---

## 9. Verwandte Commits / Bereiche

- Shutdown-Basis: `360a0dd`; Relaunch-Lock: `1f3264c` (8785)
- Scintilla-Lifecycle-Fixes: `MainWindow.c`, `ChatOutputScintilla.c`, `CodeBlocksViewer.c`, `gui.c`, `main.c`, `streamlog.c`
