# NEXT_STEP — GPU-Snapshot-Debugging: Stand und nächste Schritte

Stand: 2026-10-08 (Session). Dieser коordiniert die vier Repos; der
offene Graben lebt hier in gfxstream.

## Repos und Stände (alle auf `wip`)

| Repo | Zuletzt | Inhalt |
|---|---|---|
| valo-two | `valo: pre-checkpoint flush contract …` | Multi-Slot-TUI, IPC v2, Pre-Checkpoint-Flush (Walk über geladene DSOs, ruft `pre_checkpoint_flush`), Leftover-`restore_in_progress`-Fix, SIGSYS-Alloc-Guard, O_APPEND, E2E-Test `gpu_descriptor_set_restore` |
| mesa (Fork) | `gfxstream: flush pending batched descriptor sets …` | Guest-ICD: Registry für batched Sets, Export `pre_checkpoint_flush`, eigenes Version-Script `gfxstream_vk.sym` |
| gfxstream (Fork) | hier | Template-Op-Fix, Keep-Alive-Revert mit Begründung, Stale-Target-Logging (`target=0x…`), `getChildNodeIds` |
| rutabaga_gfx (Fork) | `kumquat: log per-resource reattach state …` | Multi-Slot (sigqueue/`--snapshot-base`), Cross-Slot-Archivierung, `[reattach]`-Instrumentierung |

Wrappers: `~/valo-tui-check/kumquat-wrapper` (batched),
`…-nobatch` (VulkanBatchedDescriptorSetUpdate:disabled — Bool-Features
nehmen nur enabled/disabled!), PATH-kumquat = cargo-install (gleich,
equal Source).

## Validiert funktioniert

- vkcube und HK: Save + Restore im einfachen Zyklus, visuell sauber
  (vkcube TUI + msg/freilaufend bestätigt).
- E2E `cargo valo test gpu_descriptor_set_restore`: grün.
- A/B Flush aus (`VALO_DISABLE_PRE_CHECKPOINT_FLUSH=1`, Kill-Switch
  ist aus dem Baum, 3 Zeilen zum Re-adden in
  `crates/valo/src/pre_checkpoint.rs`): Glitch/Crash-Klasse trat AUCH
  ohne Flush auf → Flush entlastet, präexistent.
- Batched vs nobatch: Deskriptor-Metriken in nobatch exakt 0 (keine
  lazy/stale/unbox-Fehler) — Deskriptor-Virtualisierung als
  Crash-Ursache exkulpiert.

## P1 — HK-Glitch-Klasse: fehlende Image-Views (43 Sets)

**Symptom:** Nach Restores visuelle Glitches (Farben). Im kumquat.log:
`snapshot load: skip stale descriptor write set=0x… target=0x9000…`
(~178 Skips, ~23–28 unique View-Handles, 43 Sets). Die betroffenen Sets
sind real gebunden (alle 37/37 in einer Session lazy-re-allociert).

**Bewiesen:**
- Die Views waren beim Save lebendig (`alives`-Weak-Pointer-Filter
  hat sie serialisiert — tote wären gefiltert).
- Ihre Creation-Ops fehlen im Replay-Fenster → `try_unbox` schlägt im
  Load fehl → Write wird gedroppt → Binding leer.
- vkcube zerstört nachweislich Images mit noch lebenden Views (8×
  Keep-Branch würde feuern).

**Sackgasse (nicht wieder anbauen!):** Zombie-Retention des
Image-Knotens (Commit `np` — reverted). Guest-Handles sind nicht
generations-sicher: bei Handle-Recycling no-opt `addHandles` am
vorhandenen Knoten, der neue Creation-Op geht nie in den Graphen, das
recycelte (lebende, serialisierte) Image kommt im Replay nicht zurück
→ fataler Unbox in der Load-Image-Sektion (vkcube + HK, jeder
Restore). Die Destroy-Kaskade ist es, die Recycling überhaupt
funktionieren lässt.

**Nächste Schritte (Messung vor Fix):**
1. Save-Seite instrumentieren (`vk_decoder_global_state.cpp`,
   Deskriptor-Sektion ~Zeile 830): für jeden serialisierten Write das
   Ziel (View/Buffer) auf `mReconstruction.getDepNode(boxed)` prüfen
   und fehlende loggen:
   `save: write target 0x… not in replay graph (set 0x…)` — identifiziert
   die Klasse bereits beim Save.
2. Removal-Instrumentierung: in
   `VkReconstruction::removeHandles` (oder
   `DependencyGraph::removeNodeAndDescendants`) loggen, wenn ein Knoten
   vom Typ `Tag_VkImageView` entfernt wird, inkl. Aufrufer — die
   verbleibenden Kandidaten: vkDestroyImage-Kaskade (Views überleben
   ihr Image), oder ein anderer Pfad.
3. HK-Repro (Menü → Save 1 → Spiel → Save 2/3 → Restore-Hüpfen bis
   Glitch). Korrelation: fehlt dem Zielview der Knoten schon beim
   Save (1) UND welcher Remove-Call ihn nahm (2)?
4. Danach Richtung:
   - **Gast-Lifetime fixen** (mesa gfxstream_vk_wsi: Views vor Images
     zerstören / WSI zerstört Images, während Game-Sets deren Views
     halten — invalid Vulkan, das live toleriert wird): sauberste
     Lösung.
   - **Load-seitig** prüfen, ob die betroffenen Sets vom Spiel pro
     Frame neu geschrieben werden (dann self-healt der Skip nach dem
     Resume — wenn Glitches bleiben, sind es write-once-Sets).

## P2 — kumquat Cross-Slot: „no blob memory" (resource 148)

**Symptom:** Zweiter Slot-Zyklus → `failed to get caching for resource
148: no blob memory` direkt nach `restore done` → GPUVM fault
(0x800128…) → DEVICE_LOST → SIGABRT (device_op_tracker, upstream).

**Bewiesen:** 148 existierte laut kumquat-Bookkeeping vor dem
Slot-2-Snapshot (steht nicht in der [detect]-Drop-Liste), wurde bei
Slot-1-Restores gedroppt und für Slot 2 archiviert
(`try_clone_for_archive`: kumquat-Buchhaltung + dup'd Fds, NICHT die
rutabaga-Ressource), und war nach dem zweiten Slot-2-Restore im C++
(`mBlobMemory` leer) tot.

**Nächste Schritte:**
1. HK-Lauf mit Menü-Hüpfen + In-Game-Saves; die
   `[reattach]`-Zeilen (kumquat_gpu, committed) zeigen pro Ressource
   `exported/dma_buf/mapping`. Für die Todes-Ressource:
   - `no exported blob — skipped` → Archiv-Kopie unvollständig
   - `exported … dma_buf=true mapping=true` → Reattach lief, C++
     hat es trotzdem nicht übernommen (ctx-/blob_id-Desync,
     ExternalObjectManager wird beim Load gelehrt)
2. Nicht reproduced in vkcube (überlebt den Doppelzyklus; alle
   beteiligten Ressourcen ctx 1) — Verdacht: ctx-4(VK)-spezifisch.
3. Bei Bestätigung „C++ nimmt es nicht auf": Reparaturrichtung ist
   die Registrierung in `ExternalObjectManager`
   (`stream_renderer_reattach_blob_descriptor` /
   `_reattach_blob_mapping`, virtio_gpu_gfxstream_renderer.cpp) —
   dort prüfen, ob re-insertierte Archiv-Ressourcen nach dem Load
   der C++-Ressourcenrekonstruktion noch gegen den (ctx_id, blob_id)
   laufen und ob die Load-Seite die Manager- state cleared.

## P3 — Geparkt / Betriebliches

- **/tmp-Quota-Kill:** kumquats Logging stirbt fatal auf vollem /tmp
  (println!-Write-Fehler → Rust-Panic → exit 101, Message verloren
  weil stderr voll ist; Backtrace über panic=abort-Build gesichert:
  `std::io::stdio::_print ← kumquat::main`). Hardening: Log-Writes
  dürfen den Prozess nicht töten. /tmp = tmpfs 16G, Per-User-Quota
  12.5G, ein HK-Slot ≈ 2G → nach Läufen Sessions löschen.
- **valo `--paused`-Freeze** (nur spawn/msg-Modus): Restore friert
  die Anzeige ein (TUI + freilaufend ok). Reentry:
  vkcube-reproduzierbar; Display-Kette nach Restore im pausierten
  Frame-Boundary-Zustand broken. Für visuelle msg-Tests `--paused`
  meiden.
- **/tmp-Saboteur:** Session-Dirs verschwinden sporadisch → Logs
  sofort nach Runs sichern (`~/valo-tui-check/*.log`).
- Stale-Target-Warnung hat jetzt `target=0x…` (committed) —
  behalten für P1/P2-Korrelation.

## Mess-Befehle (pro HK-Lauf)

```sh
S=<session>/gpu-0/kumquat.log
grep -ac "lazily re-allocating" $S        # batched: >0 normal (Load-Pfad), nobatch: 0
grep -ac "skip stale descriptor write" $S  # P1-Kennzahl (Glitch-Korrelation)
grep -ac "GPUVM\|DEVICE_LOST" $S          # P2-Kennzahl (Crash-Klasse)
grep -a "\[reattach\]" $S | tail -20      # P2-Zustände pro Restore
```
