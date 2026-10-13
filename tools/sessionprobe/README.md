# sessionprobe (M0a)

Kleines Diagnosetool für die Machbarkeitsfrage: Was kann eine bestimmte Windows-Benutzersitzung wirklich?
Es verändert nichts am System und enthält keinen Streaming-Code.

**Status:** Der Code ist geschrieben, aber nicht kompiliert und nicht getestet (kein Windows-Compiler in der Entwicklungsumgebung). Erwarte beim ersten Build kleine Compilerfehler.

## Was es pro Sekunde in eine JSON-Zeile schreibt
- `session`, `wts` (Active/Disconnected/...), `remote` (Sitzung ist RDP), `consoleSession`
- `render`: verwendeter GPU-Adapter, `software:true` wenn Windows auf WARP/Basic Render Driver zurückfällt, gerenderte Frames/s
- `dd`: DXGI Desktop Duplication: HRESULT beim Anlegen (`createHr`), letzter Fehler, gelieferte Frames/s, Timeouts, `accessLost`-Zähler, mittlere Bildhelligkeit (0 = schwarzes Bild)
- `audio`: WASAPI-Loopback auf den Standard-Ausgang der Sitzung: HRESULT, Geräte-ID, Pakete, Spitzenpegel. Das Tool spielt dazu selbst einen Ton (Frequenz = 440 + 110 * (Sitzungs-ID % 6) Hz)
- `wgcSupported`: nur `GraphicsCaptureSession::IsSupported()`. Ein echter Windows.Graphics.Capture-Frametest folgt als nächster Schritt.

Die Ausgabe geht auf stdout und an `probe_session<ID>.jsonl` im aktuellen Ordner.

## Bauen (Visual Studio Developer Command Prompt, Windows SDK 10.0.17134 oder neuer)
```
cl /std:c++17 /EHsc /O2 main.cpp
```

## Testablauf (in beiden Sitzungen, A und B)
1. `sessionprobe.exe 600` in Sitzung A und in Sitzung B starten (je 10 Minuten).
2. Beobachten: beide verbunden/aktiv.
3. Per RDP von Sitzung A trennen (Fenster schließen, nicht abmelden), 2 Minuten warten, wieder verbinden.
4. Sitzung sperren (Win+L), 1 Minute warten, entsperren.
5. Sitzung B parallel laufen lassen und prüfen, dass `session`, Helligkeit und Töne je Sitzung eigenständig bleiben.
6. Die `.jsonl`-Dateien und folgende Angaben zurückgeben: Windows-Build, NVIDIA-Treiberversion, wie die Mehrfachsitzungen bereitgestellt wurden, ob Sitzung A lokal oder per RDP lief.

## Wie die Ergebnisse zu lesen sind
- `software:true` oder fremder Adapter in einer Sitzung: Hardware-Rendering dort fehlt (No-Go laut Plan).
- `dd.frames` bleibt über 0, `brightness` > 0 nach dem Trennen: Capture lebt in getrennter Sitzung.
- `dd.createHr` ungleich `0x00000000` oder `accessLost` steigt dauerhaft: Capture verloren, HRESULT notieren.
- `audio.initHr` ungleich 0 oder `peak` bleibt 0 trotz Ton: Kein nutzbarer Audio-Endpunkt in diesem Zustand.
- Hinweis: `Beep` kann je nach Windows-Konfiguration stumm sein. Wenn `peak` überall 0 ist, zuerst prüfen, ob die verbundene Sitzung den Ton hört, bevor man auf einen Fehler schließt.

## Durchlauf B: Notfallplan "Loopback-RDP" (nur testen, wenn Durchlauf A bei Trennen scheitert)
Idee: Der Host verbindet sich per Standard-RDP mit sich selbst, damit Windows die Sitzung als verbunden führt.
1. Auf dem Host Remotedesktopverbindung (`mstsc`) öffnen und sich mit Benutzer A auf den eigenen Rechner verbinden, ggf. über die Adresse `127.0.0.2` (Loopback-Namen unterscheiden sich je nach Konfiguration).
2. Dort `sessionprobe.exe 600` starten, die Loopback-Verbindung minimieren statt zu trennen (Fenster nicht schließen).
3. Prüfen, ob `dd.frames` und `render.software=false` über die ganze Zeit bleiben.
4. Ob das auf deinem Setup überhaupt zulässig und möglich ist, hängt davon ab, wie die Mehrfachsitzungen bereitgestellt werden. Die Anwendung ändert daran nichts.
