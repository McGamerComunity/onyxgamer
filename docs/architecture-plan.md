# Multi-Session Remote-Gaming für Windows: Architektur- und Machbarkeitsplan

## Kontext

Ziel: Mehrere Spieler spielen gleichzeitig auf einem Windows-Host, jeder in seiner eigenen, bereits vorhandenen Windows-Benutzersitzung, mit eigenem Video, Audio und Eingaben. Gamingtauglicher Transport statt RDP-Bildübertragung.

Die Anwendung stellt Mehrbenutzerbetrieb nicht her. Sie erkennt nur, was Windows bereitstellt, und meldet konkrete Fehler. Sie macht keine termsrv-Patches, keinen RDP-Wrapper, keine Umgehung von Sitzungsbeschränkungen und keine Editions-Ablehnung.

Stand des Repos: `onyxgamer` ist leer (kein Commit). Alles startet bei null.

**Wissensstand dieser Antwort:** Ich habe in dieser Sitzung nicht im Web recherchiert. Alle Aussagen stammen aus Vorwissen. Jede Quelle unten ist ein Verweis zum Nachprüfen, kein Beleg, den ich gelesen hätte. Jede Aussage ist gekennzeichnet:
- **[B]** dokumentiert (Microsoft/Projektdoku, nachzulesen)
- **[A]** begründete Annahme
- **[T]** muss experimentell getestet werden

---

## 1. Machbarkeitseinschätzung

**Gesichert [B]**
- Sitzungen, Benutzer und Zustände lassen sich per WTS-API erkennen (`WTSEnumerateSessionsW`/`Ex`, `WTSQuerySessionInformationW`, `WTSRegisterSessionNotification`, `SERVICE_CONTROL_SESSIONCHANGE`).
- Ein SYSTEM-Dienst kann einen Prozess im Token eines Benutzers in dessen Sitzung starten: `WTSQueryUserToken` (braucht `SeTcbPrivilege`), `CreateEnvironmentBlock`, `CreateProcessAsUserW` mit `lpDesktop = "winsta0\\default"`.
- Dienste laufen in Session 0, isoliert von interaktiven Desktops. Capture, Audio und Input müssen deshalb im Agenten in der Benutzersitzung laufen, nie im Dienst.
- Named Pipes liefern die Client-Sitzung: `GetNamedPipeClientSessionId`, `GetNamedPipeClientProcessId`, ACLs auf der Pipe.
- DXGI Desktop Duplication und Windows.Graphics.Capture (WGC) sind dokumentierte Capture-APIs. Beide hängen am Desktop und Output der aufrufenden Sitzung.
- `SendInput` wirkt nur auf den Eingabe-Desktop der aufrufenden Sitzung und unterliegt UIPI (Prozess mit niedrigerer Integrität kann nicht in höhere injizieren).

**Unklar [T]. Das ist der eigentliche Kern des Projekts**
- Rendern Spiele in einer getrennten oder nicht-konsolen Sitzung auf der echten GPU (Hardware-D3D), oder fällt Windows auf WARP/Basic Display zurück?
- Liefert Desktop Duplication oder WGC in einer getrennten Sitzung noch Frames, oder gibt es `DXGI_ERROR_ACCESS_LOST`, `E_ACCESSDENIED`, `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE` oder schwarze Frames?
- Gibt es in einer getrennten Sitzung einen nutzbaren Audio-Endpunkt (WASAPI-Loopback)?
- Funktionieren Vollbild, Borderless und Auflösungswechsel ohne echten Monitor in der Sitzung?
- Sind virtuelle Controller (XInput/HID) sitzungsbezogen oder systemweit?
- Wie viele parallele Hardware-Encode-Sessions erlaubt der konkrete Treiber (NVIDIA: Limit je Treiber und GPU, siehe NVIDIA Support Matrix)?

**Mögliche Projektverhinderer (Priorität absteigend)**
1. Getrennte Sitzungen liefern keinen Hardware-gerenderten, capturebaren Desktop. Dann bleibt nur ein dauerhaft verbundener RDP-Client (siehe unten) oder ein anderer Mechanismus, der eine Anzeige an die Sitzung hängt.
2. Nur die Konsolensitzung ist am physischen Display/GPU-Output angebunden. Andere Sitzungen laufen über den RDP-Anzeigepfad. Ob GPU-Rendern und -Capture dort robust sind, hängt von Treiber, GPU und Richtlinien ab (`Remote Session Environment`-Policy für Hardware-Grafikadapter in RDS-Sitzungen **[T]**).
3. Audio-Endpunkt-Verfügbarkeit pro Sitzung **[T]**.
4. Anti-Cheat: Kernel-Anti-Cheat verweigert oft Remote-Sitzungen oder virtuelle Treiber. Das ist eine harte Spielegrenze, kein lösbares Problem. Keine universelle Kompatibilität zusagen.

**Grundlegende Grenze ohne VMs, ausdrücklich benannt:** Bleibt Blocker 1 oder 2 bestehen, gibt es ohne VMs (oder ohne zusätzliche Hardware-Display-Attachments) keine saubere Lösung. Das ist dann ein No-Go für die Kernarchitektur, nicht ein Anlass zum stillen VM-Wechsel. Die Entscheidung läge bei dir.

**Hinweis zu Windows Pro:** Fast User Switching erzeugt auf Pro mehrere Sitzungen, aber nur eine ist an der Konsole aktiv. Die übrigen stehen im Zustand `WTSDisconnected`. Genau dieser Zustand ist der zentrale Testfall, nicht ein Randfall.

---

## 2. Empfohlene Architektur

Kritische Prüfung des Vorschlags: Die Grundstruktur (Dienst + Agent pro Sitzung + Client) ist richtig und folgt aus Session-0-Isolation. Zwei Änderungen:

1. **Medienpfad am Dienst vorbei.** Der Dienst vermittelt nur Steuerung (Sitzungsliste, Agent-Start, Token-Ausgabe). Video, Audio und Input laufen direkt zwischen Client und Agent. Der privilegierte Dienst ist so nie im Datenpfad und vergrößert die Angriffsfläche nicht.
2. **M0 vor Eigenbau:** Erst unveränderte Referenzsoftware (Sunshine/Moonlight) pro Sitzung testen, um OS-Grenzen von eigenen Fehlern zu trennen (Abschnitt 4 und 7).

```
 [Client A: UI+Decoder+Input]            [Client B]
        |  (mTLS Steuerung)  |UDP media (SRTP-artig)
        v                    v
 +-------------------- Windows-Host --------------------------+
 | Session 0:   OnyxService (SYSTEM-eingeschränkt)            |
 |   - WTS-Erkennung, Policy, Pairing-Store                   |
 |   - startet Agent via WTSQueryUserToken + CreateProcessAsUser
 |   - Control-API (mTLS, nur whitelisted Aktionen)           |
 |          | Named Pipe (ACL, SessionId-Prüfung)             |
 | Session 1:  Agent A (User A, normale Integrität)           |
 |   Capture -> GPU-Encoder -> Paketierung -> UDP-Port A      |
 |   WASAPI-Loopback -> Opus -> UDP                           |
 |   Input-Injektor (SendInput, ggf. virtueller Controller)   |
 |   Spiel A (Kindprozess, Job Object)                        |
 | Session 2:  Agent B (User B)  ... analog, eigener Port     |
 +------------------------------------------------------------+
```

Datenfluss pro Frame: Spiel → Desktop-Komposition der Sitzung → Capture-Textur (GPU) → Encoder (gleiche GPU, D3D11-Textur direkt registriert) → Paketierung mit FEC → UDP → Client-Decoder → Anzeige. Eingaben laufen gegenläufig und mit Sequenznummern.

**Sitzungs-/Agent-Lebenszyklus**
- Erkennung: Enumeration beim Start plus Session-Change-Events (Logon, Logoff, Connect, Disconnect, Lock, Unlock).
- Agent-Start: Token per `WTSQueryUserToken`, `DuplicateTokenEx` zu Primary Token, `CreateEnvironmentBlock`, Arbeitsverzeichnis im Benutzerprofil, `CreateProcessAsUserW` auf `winsta0\default`. Agent läuft im Benutzerkontext, nicht als SYSTEM. Der Dienst hält `SeTcbPrivilege` nur für den Tokenabruf.
- Absturz: Dienst überwacht das Prozesshandle, startet mit Backoff neu, begrenzt Neustarts.
- Abmeldung: Agent wird beendet, Stream wird sauber geschlossen, Pairing bleibt gültig, aber ohne Sitzung kein Stream.
- Lock/Secure Desktop (UAC, Sperrbildschirm): Capture verliert den Desktop. Agent erkennt den Zustand (`DXGI_ERROR_ACCESS_LOST`), sendet "Pause"-Frame bzw. Status, initialisiert neu und friert nie ein.
- Fremdzugriff verhindern: Jede Client-Identität (Zertifikat) wird im Pairing-Store genau einer `(SessionId, User-SID)` zugeordnet. Der Agent bekommt vom Dienst über die Pipe nur ein kurzlebiges Einmal-Token für genau seine Sitzung. Der Dienst prüft per `GetNamedPipeClientSessionId`, dass der Agent wirklich in der Sitzung läuft, für die er sich meldet. Der Client kann nie eine SessionId vorgeben, nur ein Token vorlegen.

---

## 3. Technologieentscheidung

| Schicht | Wahl | Begründung |
|---|---|---|
| Agent (Capture, Encode, Audio, Input, Netzwerk) | C++20, D3D11/DXGI, WASAPI, NVENC/AMF/QSV-SDKs (oder FFmpeg-Hardware-Pfade als Start) | Direkter GPU- und Win32-Zugriff, keine GC-Pausen, die Referenzprojekte sind ebenfalls C++ |
| Dienst | C++ (gemeinsame Basis mit Agent) oder Rust (`windows`-Crate) | Wenig Laufzeitabhängigkeiten, klar begrenzter Code mit Privilegien |
| Verwaltungs-UI | C#/WinUI oder lokales Web-UI, später | Nicht latenzkritisch, spricht nur mit der Control-API des Dienstes |
| Client | Start: Moonlight-Client (unverändert); später eigener Client auf Basis von FFmpeg/SDL oder WebRTC | Client ist nicht der Engpass der Machbarkeit |
| Transport | Start: Sunshine/Moonlight-Protokoll (RTP über UDP, AES-GCM); eigene UDP-Lösung nur falls nötig | WebRTC ist schwergewichtig (libwebrtc), erleichtert aber Browser-Clients und NAT-Traversal fürs Internet. Eigenes UDP-Protokoll lohnt erst, wenn Sunshine-Protokoll Anforderungen nicht deckt |

Rückfallregeln für Transport: Bei Netzstau veraltete Frames verwerfen statt puffern, Encoder-Bitrate per Feedback senken, bei Paketverlust Intra-Refresh oder Keyframe-Anforderung statt Retransmit-Warteschlange.

---

## 3a. Bewertung vorhandener Projekte

| Option | Beschreibung | Bewertung |
|---|---|---|
| a) Unverändert | Je Sitzung eine eigene Sunshine-Instanz als Benutzerprozess mit eigenem Konfigverzeichnis, eigenen Ports und eigenem Pairing, Moonlight als Client | Kein Aufwand, ideal als M0-Messlatte. Ob Sunshine mehrere Instanzen parallel und in Nicht-Konsolensitzungen stabil betreibt, ist **ungeprüft [T]**. Ich behaupte nicht, dass getrennte RDP-Sitzungen unterstützt werden |
| b) Fork | Sunshine anpassen (Sitzungs-Handling, Mehrinstanz) | Plausibel, wenn M0 zeigt, dass nur Sitzungsverwaltung fehlt. GPL-3.0 |
| c) Komponenten | z. B. moonlight-common-c, Capture/Encode-Teile | Folgt der gleichen Lizenz |
| d) Eigenbau | Alles selbst | Höchster Aufwand, volle Kontrolle, freie Lizenzwahl |

Lizenzen **[B, nachprüfen]**: Sunshine (LizardByte/Sunshine) und die Moonlight-Clients bzw. moonlight-common-c stehen unter GPL-3.0. Jede Weitergabe abgeleiteter Binärdateien verlangt Quellcode-Offenlegung unter GPL. Eigenbau ohne Sunshine-Code vermeidet das.

Empfehlung: **Eigene Oberfläche/Verwaltung mit bestehendem Streaming-Kern (a→b) ist deutlich sinnvoller als sofortiger Vollneubau**, solange M0 zeigt, dass der Kern in mehreren Sitzungen läuft. Scheitert M0 an Sunshine-Eigenheiten statt am OS, ist ein gezielter Fork wahrscheinlich günstiger als Eigenbau. Scheitert M0 am OS, hilft kein Fork.

---

## 4. Zentrale technische Fragen im Detail

**Grafik und Capture**
- Lokale Konsolensitzung: Hardware-D3D, DD/WGC funktionieren **[B]**.
- Aktive RDP-Sitzung: Anzeige läuft über den RDP-Anzeigepfad. GPU-Rendering in RDP-Sitzungen ist per Richtlinie möglich **[A]**, Verhalten von DD/WGC dort **[T]**.
- Getrennte RDP-Sitzung: Prozesse laufen weiter, aber es hängt keine Anzeige mehr an der Sitzung. Frame-Lieferung von DD/WGC, GPU-Zuweisung und Fullscreen sind **[T]**.
- Gesperrt: Der Eingabedesktop wechselt zum Winlogon-Desktop. DD verliert den Zugriff **[B]** (Verhalten ist dokumentiert, Details testen).
- Virtuelle Display-Treiber (Indirect Display Driver, IddCx): Sie fügen einer Sitzung einen Monitor hinzu, schaffen aber keine neue Benutzersitzung. Sie könnten helfen, wenn die getrennte Sitzung keinen Output mehr hat **[A]**. Ob ein IddCx-Monitor pro Sitzung unabhängig sichtbar ist oder systemweit bleibt, ist **[T]**. Zusätzliche Monitore und virtuelle Desktops sind keine Benutzersitzungen und ersetzen sie nicht.
- Fullscreen exklusiv ist im Streaming-Kontext problematisch, Borderless ist der robuste Standard. Auflösungswechsel müssen im Agenten erkannt (`DXGI_ERROR_ACCESS_LOST`, Mode-Change) und der Encoder neu konfiguriert werden.

**Encoding**
- Start: H.264 per NVENC/AMF/QSV, GOP lang mit Intra-Refresh, keine B-Frames, CBR/VBR niedrig gepuffert. HEVC/AV1 später, Fähigkeitsabfrage per Treiber.
- Zero-Copy-Ziel: Capture-Textur und Encoder auf derselben GPU, D3D11-Textur direkt dem Encoder übergeben. Cross-GPU-Kopien (z. B. iGPU rendert, dGPU encodet) nur als Fallback.
- Mehrere Agenten gleichzeitig: Session-Limits und Treiberverhalten werden beim Start per Probe geprüft und angezeigt, nicht angenommen.
- LAN-Prototypziele (Vorschlag, anpassbar): 1080p60, 20–50 Mbit/s; Capture-bis-Sendebeginn p95 ≤ 10 ms; geschätzte Ende-zu-Ende-Latenz im LAN ≤ 60 ms p95.

**Audio**
- WASAPI-Loopback auf den Standard-Render-Endpunkt der Sitzung **[B]**. Pro Sitzung existiert eine eigene Audio-Umgebung. Ob nach RDP-Trennung ein Endpunkt bleibt, ist **[T]**; RDP-Audioumleitung erzeugt den Endpunkt nur bei verbundenem Client **[A]**.
- Falls dann kein Endpunkt existiert: virtuelles Audiogerät nötig, und es muss sitzungsgetrennt sichtbar sein **[T]**, sonst Audio-Mischung. Prüfung per Testtöne: A spielt 440 Hz, B spielt 880 Hz, Spektrum je Stream darf nur die eigene Frequenz enthalten.
- Mikrofon: spätere Ausbaustufe.

**Eingaben**
- `SendInput` im Agenten erreicht nur den Desktop der eigenen Sitzung. Agent und Spiel laufen unter demselben Benutzer und gleicher Integrität (kein Admin-Spiel), sonst blockiert UIPI.
- Relative Mausbewegung: `MOUSEEVENTF_MOVE` ohne Absolute-Flag; für Spiele mit Raw Input prüfen **[T]**; Cursor-Clipping und Fokus des Spiels beachten.
- Controller: Systemweite virtuelle Treiber (z. B. ViGEmBus, laut Projekt eingestellt **[prüfen]**) sind vermutlich nicht sitzungsgetrennt, dann würde Spieler B Controller von A sehen **[T]**. Alternative: eigener sitzungsbewusster HID-Treiber (großer Aufwand, Signierung) oder zuerst nur Maus/Tastatur.
- Kein Anti-Cheat-Umgehungsversuch. Spiele mit Kernel-Anti-Cheat werden als nicht unterstützt markiert.

**Sicherheit/Netzwerk**
- Pairing nach Moonlight-Muster (PIN, gegenseitig gepinnte Zertifikate), mTLS auf der Control-API, Medienverschlüsselung mit AES-GCM.
- Der Dienst führt nur eine feste Whitelist aus (Agent starten/stoppen, vorregistrierte App-ID starten). Keine Pfade, Kommandozeilen oder Skripte vom Client.
- Wiederverbindung: neues Token, gleiche Zertifikat→(SessionId,SID)-Bindung, gleiche Sitzung nur nach erneuter Prüfung, dass SID und SessionId noch übereinstimmen (Sitzungs-ID kann nach Abmeldung neu vergeben werden).

---

## 5. Machbarkeits-Prototyp (vor dem Produkt)

**Aufbau:** Ein Host, zwei Benutzer (A, B), beide angemeldet. Je Sitzung ein D3D11-Testprogramm (rotierender Würfel mit Frame-Zähler, Sitzungs-ID und Sitzungsfarbe im Bild; Ton mit Sitzungsfrequenz) und, sobald verfügbar, ein echtes Spiel. Zuerst M0a (reine Probe), dann M0b (Referenz-Sunshine pro Sitzung).

**Go-/No-Go (messbar, Vorschlagswerte)**

| Kriterium | Go | No-Go |
|---|---|---|
| Hardware-Rendering je Sitzung | Testprogramm nutzt in beiden Sitzungen die reale GPU (per PresentMon/DXGI-Adapterabfrage) | Fällt in einer Sitzung auf WARP/Basic Display zurück |
| Capture unabhängig | Beide Sitzungen liefern parallel ≥ 55 fps bei 1080p60 für 30 min; Bildinhalt trägt je die richtige Sitzungs-ID | Schwarze Frames, Access-Lost-Schleifen oder Bild der falschen Sitzung |
| Nach RDP-Trennung | Stream läuft ≥ 10 min weiter, ≥ 90 % der Soll-fps, kein Absturz, Hardware-Rendering bleibt | Capture bricht dauerhaft ab und ist nicht ohne Zusatzmechanismus wiederherstellbar |
| Audio getrennt | Spektralanalyse: jeder Stream enthält nur die eigene Frequenz | Fremdes Audio hörbar oder kein Endpunkt |
| Eingabe getrennt | 1000 markierte Eingabeereignisse je Sitzung landen zu 100 % nur in der eigenen | Auch nur ein Ereignis in der fremden Sitzung |
| Encoder parallel | Zwei gleichzeitige Hardware-Encodes ohne Fehler; Capture→Encode p95 ≤ 10 ms | Encoder-Session-Limit oder Treiberfehler bei zwei Instanzen |
| Stabilität | Lock/Unlock und erneutes Verbinden: Stream erholt sich selbstständig innerhalb 5 s | Hängt dauerhaft |

Jedes No-Go wird dokumentiert (HRESULT, Treiberversion, Zustand) und gilt als Ergebnis, nicht als Fehlschlag.

---

## 6. Roadmap (kleine, unabhängig testbare Meilensteine)

1. **M0a – Session-Probe:** Konsolentool pro Sitzung: WTS-Zustand, DXGI-Adapter/Outputs, `DuplicateOutput`-Ergebnis mit HRESULT, `GraphicsCaptureSession::IsSupported`, WASAPI-Endpunkte, Hardware- vs. WARP-Gerät. Läuft in den Zuständen aktiv/getrennt/gesperrt.
2. **M0b – Referenz:** Unverändertes Sunshine/Moonlight pro Sitzung (eigene Ports/Konfig) als Vergleichsmessung.
3. **M1 – Dienst + Agent-Start:** Sitzungserkennung, Agent im Benutzerkontext, Named-Pipe-IPC mit Sitzungsprüfung, Absturz-/Logoff-Behandlung.
4. **M2 – Capture je Sitzung → Datei:** Agent schreibt erfasste Frames lokal, noch ohne Netzwerk. Beweist Sitzungstrennung des Bildes.
5. **M3 – Hardware-Encode, zwei Instanzen parallel**, lokale Latenzmessung.
6. **M4 – Audio** (Loopback, Opus) mit Sitzungstrennungstest.
7. **M5 – Eingaben** (Maus/Tastatur), Trenntest, danach Controller-Spike.
8. **M6 – Netzwerk-Transport und Pairing**, Backpressure, Frame-Dropping, Reconnect.
9. **M7 – Verwaltungs-UI**, Diagnose-Seite (Fähigkeiten und Fehlermeldungen), Komfort.
10. **M8 – Härtung:** Fuzzing der Control-API, Privilegien-Audit, Langzeittest.

Reihenfolge gewollt: Sitzungstrennung beweisen (M0–M2), dann Streaming optimieren (M3–M6), dann UI (M7).

---

## 7. Testmatrix

Achsen: **Edition** (Server / Pro mit extern bereitgestellten Mehrfachsitzungen) × **Sitzungszustand** (aktiv lokal, aktiv RDP, getrennt, gesperrt) × **GPU** (NVIDIA / AMD / Intel) × **Nutzerzahl** (1 / 2) × **Anzeigemodus** (Fenster / Borderless / Vollbild).

Pflichtumfang (Vollmatrix ist unrealistisch):
- Server × (aktiv RDP, getrennt, gesperrt) × NVIDIA × 2 Nutzer × (Fenster, Borderless)
- Pro × (aktiv lokal, getrennt, gesperrt) × NVIDIA × 2 Nutzer × (Fenster, Borderless)
- Danach AMD und Intel jeweils mit dem schlechtesten Zustand (getrennt)
- Vollbild-exklusiv nur als Stichprobe, Erwartung: eingeschränkt
- Einzelnutzer-Referenz je Konfiguration (Beweis, dass Mehrnutzer die eigentliche Variable ist)

Pro Zelle: Hardware-Rendering ja/nein, Capture-Methode, fps, Latenz, Audio, Input, Erholung nach Zustandswechsel.

---

## 8. Größte Risiken (nach Einfluss auf Machbarkeit)

1. Getrennte/Nicht-Konsolen-Sitzung ohne GPU-Rendering oder Capture (Projektkiller).
2. Audio-Endpunkt fehlt oder wird sitzungsübergreifend geteilt.
3. Virtuelle Treiber (Display, Audio, Controller) sind systemweit statt sitzungsgetrennt.
4. GPU-Encoder-Sessionlimits und Treiberunterschiede.
5. Anti-Cheat und Spielekompatibilität.
6. Treiber-/Windows-Updates ändern Verhalten (Wartungsaufwand).
7. GPL-Folgen bei Fork.
8. Latenz-Spitzen bei GPU-Konkurrenz zwischen den Sitzungen (zwei Spiele plus zwei Encoder auf einer GPU).

**Offene Fragen an dich (nur architekturrelevante):**
- Welche GPU(s) stehen im Testhost (Hersteller/Modell, eine oder mehrere)? Das bestimmt Encoder-Pfad und Limit-Tests.
- Welche Windows-Version/Edition und welche Methode stellt die Mehrfachsitzungen bereit? (Nur zur Testdokumentation, nicht zur Umsetzung durch die Anwendung.)
- Darf ein Test-Client (z. B. dauerhaft verbundener RDP-Client) als Fallback für "keine Anzeige in getrennter Sitzung" eingeplant werden, oder zählt nur echtes Trennen?

Annahmen bis zur Antwort: eine NVIDIA-dGPU, Windows Server 2022 oder Windows 11 Pro, LAN, 1080p60, Maus/Tastatur zuerst.

---

## 9. Konkreter nächster Schritt

**M0a: `sessionprobe.exe` (ca. 300 Zeilen C++).** Es ist der kleinste Test, der die Projektfrage beantwortet, bevor ein Encoder oder eine Netzwerkzeile existiert. Es rendert einen sichtbaren D3D11-Würfel mit Frame-Zähler, versucht gleichzeitig `DuplicateOutput`, WGC und WASAPI-Loopback und schreibt pro Sekunde eine JSON-Zeile (Sitzungs-ID, WTS-Zustand, verwendeter Adapter, HRESULTs, erfasste Frames/s, mittlere Frame-Helligkeit zur Schwarzbild-Erkennung, Audio-Pegel). Es wird per Hand in beiden Sitzungen gestartet und durch die Zustände aktiv → getrennt → gesperrt → entsperrt geführt.

Ergebnis: eine Tabelle, die die Roadmap entweder bestätigt (weiter mit M0b/M1) oder die Architektur zurückweist. Kann ich nicht auf diesem Linux-Container ausführen. Ich erstelle den Code, du führst ihn auf dem Windows-Testhost aus und gibst mir die Logs.

## Verifikation dieses Plans

Dieser Plan enthält keinen Anwendungscode. Er ist vollständig, wenn Go-/No-Go-Kriterien, Testmatrix und das M0a-Ergebnisformat von dir freigegeben sind. Das Quellenverzeichnis zum Gegenlesen:
- Microsoft Learn: `WTSEnumerateSessionsW`, `WTSQueryUserToken`, `CreateProcessAsUserW`, `GetNamedPipeClientSessionId`, Desktop Duplication API, Screen capture (Windows.Graphics.Capture), `SendInput`/UIPI, Indirect Display Driver Model (IddCx), Session-0-Isolation, WASAPI Loopback Recording
- NVIDIA Video Encode and Decode GPU Support Matrix (Encoder-Sessionlimits)
- github.com/LizardByte/Sunshine, github.com/moonlight-stream (Lizenz GPL-3.0), github.com/nefarius/ViGEmBus (Projektstatus)

---

## 10. Verbindungsabbruch-Policy (Nutzerwunsch)

Gilt für das Trennen des **Streaming-Clients**. Die Windows-Sitzung bleibt dabei unverändert verbunden; der Agent erfasst und kodiert weiter und verwirft die Daten, solange kein Client da ist.

| Phase | Standarddauer | Verhalten |
|---|---|---|
| Halten | 0–3 min | Volle Auflösung und Qualität, Reconnect nahtlos |
| Sparmodus | 3–8 min | Auflösung 800x600, niedrige Bitrate/Framerate; Spiel wird möglichst pausiert (Prozess suspendieren oder Framerate-Limit), da 800x600 allein die Spiel-GPU-Last nicht senkt |
| Aufräumen | danach | Spiel geordnet beenden (CloseMainWindow, dann Timeout), Agent beenden. Windows-Abmeldung nur, wenn für den Nutzer aktiviert (Standard: aus, da ungespeicherte Daten verloren gehen und die Anwendung keine Sitzungen neu erzeugt) |

- Zeiten und Abmelde-Option pro Nutzer konfigurierbar.
- Reconnect in Phase 2 stellt die Auflösung wieder her und hebt die Pausierung auf.
- Auflösungswechsel in der Sitzung per `ChangeDisplaySettingsEx` ist **[T]**: Ob das ohne echten Monitor in der Sitzung funktioniert, prüft ein Folgetest.

**Getrennte RDP-Verbindung der Sitzung** ist ein anderer Fall und kann nicht von der Anwendung vorgetäuscht werden. Ein dauerhaft verbundener RDP-Client als Attrappe ist nur der Notfallplan, falls der Test (M0a) zeigt, dass Capture in getrennten Sitzungen ausfällt.
