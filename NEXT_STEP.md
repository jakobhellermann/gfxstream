# NEXT_STEP — GPU-Snapshot-Debugging: Stand und nächste Schritte

Stand: 2026-10-08 (Session). Koordiniert die vier Repos; der offene
Graben lebt hier in gfxstream.

## Architektur: wer ist wessen Upstream, wer verwaltet was

```
Gast (valo-Prozess: Spiel + Mesa-ICD)
   │ gpu.sock (VIRTGPU_KUMQUAT-Transport)
   ▼
kumquat (kumquat/server)          — EIGENER Code, kein Upstream
   │ nutzt als Library:
   rutabaga_gfx (src/rutabaga_core.rs) — UPSTREAM crosvm
   │   (Rutabaga::restore = destroy-all/recreate-all, unangetastet)
   │ Gfxstream-Komponente per C-ABI (stream_renderer_*):
   ▼
gfxstream host (C++, libgfxstream_backend.so) — UPSTREAM google/gfxstream
   │   + eigener Fork (Snapshot-Patches)
   ▼
RADV (echter Host-Treiber)
```

**Zwei Ressourcen-Schichten, beide nötig:**

- **gfxstream C++** = die Grafik-Objekte (Vulkan-State, ColorBuffer,
  Deskriptoren). Der ganze Save/Load-Replay. Konsument.
- **kumquat Rust** (`kumquat_gpu/mod.rs`, `self.resources`) = die
  Transport-Buchhaltung + **Fd-Halterung** (Dmabuf-Exports,
  Mappings). Zulieferer. Existiert, weil upstream die Fd-Rettung
  explizit der VMM-Seite überlässt („ModeGfxstream: WiP support …
  it's the VMM's responsibility to re-attach backing iovecs and
  re-map the memory") und kumquat kein crosvm-VMM ist — es ersetzt
  crosms Descriptor-Table-Rolle.

**Brücke = Reattach:** kumquat registriert Fds beim C++
ExternalObjectManager (Schritt 2), BEVOR rutabaga.restore() den
C++-Load startet (Schritt 3), dessen Replay die Deskriptoren
konsumiert (je Restore, daher "every restore" in adfba02/b04fbad).

**Warum die Fds kumquats Kernproblem sind:** valo snapshottet den
Gast als Memory-Image, überspringt aber **file-backed Mappings**
(dmabuf-gemappter GPU-Speicher — siehe die skip-Zähler im
valo.log). Die Inhalte leben in Kernel-Dmabufs, deren letzte
Referenzen kumquat hält. Fd verloren → Kernel freed → wiederher-
gestellte Gast-Mappings zeigen auf freien Speicher → die
GPUVM-Fault-Klasse (0x800128…). Die Dmabufs selbst sind bereits das
kernel-seitige „Descriptor-Table"-Objekt (refcounted, fd-unabhängig)
— es fehlt nur eine explizite Halterung (siehe P2-Redesign).

**Warum Multi-Slot neue Probleme macht, konsekutive nicht:**
Single-Slot kennt den Zustand „jetzt tot, später wieder gebraucht"
nicht — Ressourcen sind älter als der Restore-Punkt (leben
ununterbrochen weiter) oder jünger als der letzte Save (Drop,
niemand vermisst sie; der rewound Gast erstellt Äquivalente mit
frischen Ids). Multi-Slot erzeugt genau diesen Zustand: Slot-1-
Restore tötet Ressourcen, die Slot 2s Buchhaltung noch braucht →
Drop/Archive/Re-Insert-Zyklus → der zweite Zyklus leakt (P2).

## Repos und Stände (alle auf `wip`)

| Repo | Zuletzt | Inhalt |
|---|---|---|
| valo-two | `valo: pre-checkpoint flush contract …` | Multi-Slot-TUI, IPC v2, Pre-Checkpoint-Flush (Walk über geladene DSOs, ruft `pre_checkpoint_flush`), Leftover-`restore_in_progress`-Fix, SIGSYS-Alloc-Guard, O_APPEND, E2E-Test |
| mesa (Fork) | `gfxstream: flush pending batched descriptor sets …` | Guest-ICD: Registry für batched Sets, Export `pre_checkpoint_flush`, Version-Script `gfxstream_vk.sym` |
| gfxstream (Fork) | hier | Template-Op-Fix, Keep-Alive-Revert mit Begründung (Handle-Recycling-Invariante!), Stale-Target-Logging, `getChildNodeIds`, dieses Dokument |
| rutabaga_gfx (Fork) | `kumquat: log per-resource reattach state …` | Multi-Slot (sigqueue/`--snapshot-base`), Cross-Slot-Archivierung, `[reattach]`-Instrumentierung |

Wrapper: `~/valo-tui-check/kumquat-wrapper` (batched), `…-nobatch`
(Bool-Features: nur enabled/disabled!), PATH-kumquat = cargo-install.

## Validiert funktioniert

- vkcube + HK: Save/Restore im einfachen Zyklus visuell sauber.
- E2E `cargo valo test gpu_descriptor_set_restore`: grün.
- A/B Flush aus: Glitch/Crash-Klasse auch ohne Flush → Flush
  entlastet. Nobatch: Deskriptor-Metriken exakt 0 →
  Deskriptor-Virtualisierung als Crash-Ursache exkulpiert.
- **Offene Validierung zum Flush:** der ORIGINALE Blackscreen-Repro
  (lange Session, wiederholte Saves) nie mit/ohne Flush A/B-getestet
  — erst das beantwortet „war der Flush nötig?" endgültig. Er ist
  billige Versicherung (Save deterministisch vollständig); behalten.

## P1 — HK-Glitch-Klasse: fehlende Image-Views (43 Sets)

**Symptom:** Glitches nach Restores. Im kumquat.log ~178
`skip stale descriptor write … target=0x9000…` (23–28 Views, 43 Sets,
alle real gebunden).

**Bewiesen:** Views lebten beim Save (`alives`-Filter serialisierte
sie); ihre Creation-Ops fehlen im Replay-Fenster → Load droppt die
Writes → Bindings leer. vkcube zerstört nachweislich Images mit
lebenden Views.

**Sackgasse (nicht wieder anbauen!):** Zombie-Retention des
Image-Knotens (reverted, Commit `np`). Guest-Handles sind nicht
generations-sicher: Handle-Recycling no-opt `addHandles` am
existierenden Knoten → der neue Creation-Op geht nie in den Graphen →
fataler Unbox in der Load-Image-Sektion (vkcube + HK, jeder Restore).
Die Destroy-Kaskade ist es, die Recycling funktionieren lässt.

**Nächste Schritte (Messung vor Fix):**
1. Save-Seite instrumentieren (Deskriptor-Sektion ~Zeile 830): für
   jeden serialisierten Write das Ziel auf
   `mReconstruction.getDepNode(boxed)` prüfen, fehlende loggen.
2. Removal-Instrumentierung in `VkReconstruction::removeHandles`:
   loggen, wenn ein `Tag_VkImageView`-Knoten entfernt wird, inkl.
   Aufrufer (Kandidat: vkDestroyImage-Kaskade — Views überleben
   ihr Image).
3. HK-Repro (Menü → Save 1 → Spiel → Save 2/3 → Restore-Hüpfen bis
   Glitch) → Korrelation.
4. Dann Richtung: Guest-Lifetime fixen (mesa gfxstream_vk_wsi:
   Views vor Images zerstören) — oder prüfen, ob die betroffenen
   Sets vom Spiel pro Frame neu geschrieben werden (dann self-healt
   der Skip und die Ursache liegt woanders).

## P2 — kumquat Cross-Slot: „no blob memory" (resource 148)

**Symptom:** zweiter Slot-Zyklus → `failed to get caching …: no blob
memory` direkt nach `restore done` → GPUVM fault → DEVICE_LOST →
SIGABRT. Reproduzierbar HK (Save 1 Menü → Save 2 Spiel → 1 → 2 → …);
vkcube überlebt (alle Ressourcen ctx 1 — Verdacht: ctx-4-spezifisch).

**Bewiesen:** 148 existierte vor Slot-2s-Snapshot (nicht in der
[detect]-Drop-Liste), wurde bei Slot-1-Restores gedroppt+archiviert
(`try_clone_for_archive` = Buchhaltung + dup'd Fds, NICHT die
rutabaga-Ressource), nach dem zweiten Slot-2-Restore war die C++-
Seite (`mBlobMemory`) leer.

**Kurzfristig:** HK-Lauf mit Menü-Hüpfen + In-Game-Saves; die
`[reattach]`-Zeilen zeigen pro Ressource `exported/dma_buf/mapping`
— für die Todes-Ressource: `no exported blob — skipped` → Archiv
unvollständig; `exported … true …` → Reattach lief, C++ nahm es
nicht auf (ExternalObjectManager wird beim Load gelehrt — dort
weitergraben: `stream_renderer_reattach_blob_descriptor`,
virtio_gpu_gfxstream_renderer.cpp).

**Mittelfristig — Architektur-Redesign (empfohlen):** statt das
zweite-Zyklus-Loch der Live-Map-Emulation zu patchen, das fehlende
crosvm-VMM-Piece in crosms Form nachrüsten:

```
blob_table:  blob_id → fd        (prozesslebig, refcounted über Slots)
slot_state:  serialisierte Re-Attach-Map je Slot (Fd-Referenzen
             als blob_id — wie rutabaga seine Map per Dir serialisiert)
restore  =   Map-Swap + Re-Attach aus der Tabelle
```

Der Zustand „jetzt tot, später gebraucht" entfällt strukturell;
Dmabuf-Inhalten genügt eine einzige Halterung. Die bestehenden
Pfade (Rutabaga::restore, stream_renderer_restore, Reattach-Loop)
bleiben — nur die kumquat-Ebene über der rutabaga-Map bekommt
Swap-Semantik statt Drop/Archive/Reinsert.

**Historie (warum die Schicht so aussieht wie sie aussieht):**
upstream destroy-all/recreate-all setzt crosms Descriptor-Table
voraus. Nachgebaut, Commit für Commit: `d75d089` (Blob-Backing) →
`eeafae9`/`adfba02` (Mappings je Restore) → `2999bcb`/`b04fbad`
(Deskriptoren je Restore + ExternalObjectManager-Pfad) →
`99c6a69` (Wrapper-Bookkeeping + single-slot-Archiv, Grund:
Fd-Halterung + InvalidResourceId) → `2605251` (Multi-Slot) →
`da38f70` (Cross-Slot-Archiv — hat jetzt das zweite-Zyklus-Loch).

## P3 — Geparkt / Betriebliches

- **/tmp-Quota-Kill:** kumquats Logging stirbt fatal auf vollem
  /tmp (println!-Write-Fehler → Panic → exit 101; Message verloren,
  stderr ist das volle Ziel). Hardening: Log-Writes dürfen den
  Prozess nicht töten. /tmp 16G, Quota 12.5G, HK-Slot ≈ 2G.
- **valo `--paused`-Freeze** (nur spawn/msg-Modus): Restore friert
  die Anzeige ein (TUI + freilaufend ok) — Display-Kette im pausierten
  Frame-Boundary-Zustand broken. Visuelle msg-Tests ohne `--paused`.
- **/tmp-Saboteur:** Session-Dirs verschwinden sporadisch → Logs
  sofort sichern (`~/valo-tui-check/*.log`).
- Kill-Switch `VALO_DISABLE_PRE_CHECKPOINT_FLUSH` ist aus dem
  valo-Baum (3 Zeilen zum Re-adden in pre_checkpoint.rs, falls der
  Blackscreen-A/B aus „Validiert funktioniert" laufen soll).

## Mess-Befehle (pro HK-Lauf)

```sh
S=<session>/gpu-0/kumquat.log
grep -ac "lazily re-allocating" $S          # batched >0 (Load-Pfad) normal, nobatch 0
grep -ac "skip stale descriptor write" $S   # P1-Kennzahl
grep -ac "GPUVM\|DEVICE_LOST" $S            # P2-Kennzahl
grep -a "\[reattach\]" $S | tail -20       # P2-Zustände pro Restore
```
