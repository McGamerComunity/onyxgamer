# Onyx

Remote-Gaming für mehrere gleichzeitige Windows-Benutzersitzungen auf einem Host. Jeder Spieler bekommt einen eigenen Agenten in seiner Sitzung. Siehe `docs/architecture-plan.md` für Architektur, Risiken und Roadmap.

Die Anwendung nutzt nur Sitzungen, die Windows bereits bereitstellt. Sie ändert nichts am Mehrbenutzerbetrieb des Systems.

## Stand
| Meilenstein | Status |
|---|---|
| M0a `tools/sessionprobe` (Machbarkeitstest) | geschrieben, ungetestet |
| M1 Dienst + Agent + gesicherte Pipe | geschrieben, **nicht kompiliert und nicht getestet** (nur Linux-Umgebung) |
| M2 Capture, M3 Encoder, M4 Audio, M5 Eingaben, M6 Netzwerk | offen |

## Bauen (Visual Studio 2022, Windows SDK 10.0.17134+)
```
cmake -S . -B build -A x64
cmake --build build --config Release
```
Erzeugt `onyx_service.exe`, `onyx_agent.exe`, `sessionprobe.exe` in `build\Release`. Beide Onyx-Programme müssen im selben Ordner liegen. Der Ordner sollte nur von Administratoren beschreibbar sein (z. B. unter `C:\Program Files\Onyx`), weil der Dienst die Agent-Datei im Benutzerkontext startet.

## Benutzen (M1)
1. Als Administrator: `onyx_service.exe --install`
2. Datei `C:\ProgramData\Onyx\config.ini` anlegen mit einer Zeile: `allowed_users=alice,bob` (Kontonamen ohne Domäne). Ohne Eintrag startet kein Agent.
3. `sc start OnyxStream`
4. Beide Benutzer sind bereits angemeldet. Logs: `C:\ProgramData\Onyx\logs\service.log` und je Sitzung `%LOCALAPPDATA%\Onyx\agent-<Sitzung>.log`.

Zum Debuggen im Vordergrund: als SYSTEM (z. B. `PsExec -s -i`) `onyx_service.exe --console`.

## Was M1 prüft
- Für jede erlaubte Sitzung (aktiv oder getrennt) läuft genau ein Agent im Benutzerkontext, nicht als SYSTEM.
- Agent beenden oder abstürzen lassen: Dienst startet ihn neu (Backoff, maximal 5 Starts in 5 Minuten).
- Abmeldung des Benutzers: Agent wird nicht mehr gestartet.
- Ein beliebiger Prozess, der sich auf die Pipe verbindet, wird abgelehnt (Prüfung von Prozess-ID und Sitzung gegen den selbst gestarteten Agenten).
