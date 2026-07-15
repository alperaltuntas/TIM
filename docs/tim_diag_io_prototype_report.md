# TIM Diagnostics & I/O Prototype — Final Report

**Date:** 2026-07-09
**Branches:** TIM/FMS-src and MOM6 `parallelio-prototype` (never to be PR'd to main)
**Companion documents:**
- `docs/tim_diag_io_plan.md` — the original plan of record (kept verbatim as the baseline this report evaluates)
- `docs/tim_diag_io_prototype_findings.md` — the running findings log (per-gate detail, raw numbers)
- `docs/fms_diag_semantics.md` — the FMS diag_manager behavioral spec extracted for the port
- `docs/tim_fms_io_elimination_notes.md` — the FMS I/O elimination (E1–E5) working notes

This report is the prototype's deliverable: it consolidates the goal, what was built,
what was learned, and a revised plan for the production implementation.

---

## 1. Original goal, principles, and plan

### 1.1 Goal

Replace MOM6's diagnostics and file-I/O infrastructure — today served by trimmed-FMS
Fortran (`diag_manager/`, `fms2_io/`, `time_interp/`, plus the FMS1 layers underneath) —
with a new C++ library inside TIM, built on a parallel-I/O backend, structured to
streamline the ongoing GPU port. FMS behavior is the specification; the FMS interface
is not.

### 1.2 Principles and constraints (as set at the start)

1. **Deep modules, narrow interfaces; errors defined out of existence.** Increments are
   complete abstractions, never features: a module's whole designed interface lands as
   one unit; end-to-end feature demos are validation gates inside a phase.
2. **FMS behavior is the spec, never the interface.** The MOM6 wrapper files under
   `config_src/infra/TIM/` are the swap seams; MOM6 source above them is untouched.
3. **Backend = PIO2 (NCAR ParallelIO), swappable to SCORPIO quickly.** All backend calls
   confined to one seam, restricted to the C-API subset the two libraries share.
4. **Restart-spanning time averaging is a required feature** (FMS loses partial
   accumulation at restart). Accumulator persistence is first-class from the start.
5. **Classic diag_table parsing first**, config model YAML-extensible.
6. **Bit-identity as the oracle.** `ocean.stats` + restart/history byte comparison
   against FMS controls at every gate; `double_gyre` per increment, `cesm_t232`
   (1/4°, masked tripolar, 768 ranks) at milestones.
7. **Ensemble-safe:** explicit communicators end-to-end, no singletons (added as a
   user-directed principle mid-prototype; now considered original scope).

### 1.3 The two-pass strategy

Decided 2026-07-07: development happens in **two passes**.

- **Pass 1 (this prototype):** a timeboxed (~2–3 week) tactical implementation of the
  *entire* spine — read, write, query, diagnostics, time interpolation, CESM
  integration, FMS-I/O elimination — on throwaway branches, whose deliverable is this
  report, not the code. It had to answer six exit questions:
  - **Q1** PIO2 ergonomics (decomps, masked layouts, append, error modes)
  - **Q2** the right bind(C) C-API shape
  - **Q3** MOM6 dispatch reality at the two wrapper seams
  - **Q4** exact FMS diag_manager semantics (windows, weights, rollover, metadata)
  - **Q5** restart-spanning accumulator state — feasible and bit-exact?
  - **Q6** performance at production scale vs FMS
- **Pass 2 (next):** a fresh, abstraction-first production implementation on a clean
  branch (`parallelio`), landed as a reviewable PR series to main, with no plan/report
  files in it.

### 1.4 Original phased roadmap (summary)

P0 build glue + PIO spike + domain/time bridges → Phase 1 complete `TIM::IO` File
abstraction with read gate ("TIM reads a MOM6 restart") and write gate (restart
cross-matrix) → Phase 2 diag abstractions (parser + calendar, Accumulator with
persistence, DiagManager) with parity and restart-spanning gates → Phase 3 scale/GPU →
Phase 4 deferred items (native domains, YAML, retiring FMS sources). Full text in
`docs/tim_diag_io_plan.md`.

---

## 2. Post mortem

### 2.1 What was built

The prototype went **further than planned**: the original scope ended at "diag tier +
performance numbers", but the momentum carried through full CESM integration and the
complete elimination of FMS I/O from the build (E1–E5). Final state:

- **`tim/cpp/core/`** — `TIM::Config` (sole ParmParse includer; file/env/default
  precedence), `TIM::Time` (literal `time_manager.F90` port: Gregorian/Julian/noleap/
  360-day, two-mode `increment_date`, `get_time_string`; calendar explicit everywhere,
  no calendar singleton), `TIM::Decomp2D` + `Stagger` (window/read-component/
  write-partition geometry as a value type).
- **`tim/cpp/io/`** — `Backend` (the only pio.h includer; collective PIO calls +
  `Backend::Serial` rank-independent plain-netCDF for queries/slabs), `IoSystem`
  (RAII on an explicit `MPI_Comm`), `DecompCache` (keyed by domain/stagger/nz/precision),
  `File` (deep move-only RAII: create/open/define/read/write/inquiry, file-determined
  stagger sniffing, record management), `IoContext` (owns iosystem + domain registry +
  file caches; one per component), `ExternalField` (full `time_interp_external2`
  replacement: units parsing, modulo climatology, `get_valid`, 2-slot record cache).
- **`tim/cpp/diag/`** — `DiagConfig` + classic diag_table parser, `Accumulator`
  (FMS-exact weighted reductions + serializable window state), `DiagManager`
  (registration/fan-out, strict-> scheduler with midpoint times, file rollover trio,
  `%`-token filenames, statics at close, `average_T1/T2/DT` + time bounds, CMOR fills,
  `saveState`/`restoreState`).
- **C APIs + Fortran interfaces** — `tim_io_C_API`, `tim_diag_C_API`,
  `tim/fortran/tim_io_interface.F90`, `tim_diag_interface.F90` (following the
  `tim_coms_infra` triplet precedent).
- **MOM6 `config_src/infra/TIM/`** — all five infra wrappers rewritten onto TIM and
  stripped of every FMS I/O `use`: `MOM_io_infra` (2910→1636 lines), 
  `MOM_diag_manager_infra` (885→700), `MOM_interp_infra`, `MOM_data_override_infra`
  (fail-loud stubs), plus native `parse_mask_table` in `MOM_domain_infra`. Q5 hooks in
  the solo driver and the NUOPC cap (save/restore `TIM.diag.res.nc` beside restarts).
- **FMS fork build** — E4/E5 removed `fms2_io`, `diag_manager`, `time_interp`,
  `data_override`, `axis_utils2`, mosaic2 Fortran, and the FMS1 `fms_io`/`mpp_io`/
  `read_mosaic` layers from the build (`.exclude` files + ~150 lines of consumer strips
  and dormant-preserving stubs). `tim_backend.o` is now the **sole netCDF-referencing
  object in libfms**; remaining FMS file I/O is mpp ASCII only (input.nml, logfiles).
- **CESM integration** — TIM builds and runs inside CESM
  (`SMS_D.TL319_t232.G_JRA_RYF.derecho_gnu.mom-tx3deg` 9/9 PASS with TIM read+write+
  diag+interp on by default): FMS buildlib sweeps `tim/`, Makefile.cesm PIO/netCDF
  includes, mom buildlib AMReX includes, `MOM6_INFRA_API=TIM` xml value + testmod,
  cime link glue (`-lamrex -lstdc++`), stochastic_physics CA-restart stubs.
- **Prototype test assets** — `prototype/pio_spike/` (12-test PIO ergonomics spike),
  `prototype/diag_probe/` (parse/reduce/time/manager/extfield unit probes, the manager
  test running real PIO on 1 rank).

### 2.2 Anatomy of changes — every repository touched

| # | Repository / checkout | Branch | State | Content |
|---|---|---|---|---|
| 1 | **TIM / FMS-src** (turbo-stack `submodules/infra/TIM` ≡ sandbox `libraries/FMS/src`, shared via `turbo-local` remotes) | `parallelio-prototype` | ~30 commits, `966625eb`..`c6877379` | All new C++ (`tim/cpp/{core,io,diag}`), C APIs, Fortran interfaces, spike + probes, docs, E4/E5 FMS-source elimination (`4f99cd26`, `22de85c5`) |
| 2 | **MOM6** (turbo-stack submodule ≡ sandbox clone, shared repo) | `parallelio-prototype` | ~20 commits, `8c58afb73`..`f820a0769` | infra/TIM wrapper rewrites + dispatch (later unconditional), solo-driver + NUOPC-cap Q5 hooks, FMS1/FMS2 no-op stubs, seam timers, native parse_mask_table (E3: `36cd2609b`, `aa67d9fa4`, `2944950fc`) |
| 3 | **turbo-stack** (superrepo) | — (no prototype branch) | **uncommitted working tree** | `build.sh` (parallelio module + `-lpioc`), makefile templates (intel/nvhpc fp-model `-Mnofma -Kieee` fix), example job scripts, **new `examples/cesm_t1_12`** (1/12° standalone case) |
| 4 | **cime** (sandbox) | detached HEAD (pinned checkout) | **uncommitted** | `CIME/Tools/Makefile`: `MOM6_INFRA_API==TIM` → `-lamrex -lstdc++` after `-lfms` |
| 5 | **components/mom wrapper** (sandbox, detached-HEAD external) | detached HEAD (pinned checkout) | **uncommitted** | `config_component.xml` (TIM valid value), `buildlib` (AMReX include), `testdefs/.../mom/tim` testmod |
| 6 | **FMS interface** (sandbox `libraries/FMS`) | `add_amrex_submodule` | `1164e92` committed; Makefile.cesm + src pointer **uncommitted** | buildlib: `tim/` Filepath additions + E4 Filepath excludes; Makefile.cesm PIO/netCDF INCLDIR |
| 7 | **stochastic_physics** (MOM external) | new `parallelio-prototype` | `681fd9e` | `update_ca.F90` CA-restart fms2_io stubs (CA is atmosphere-only; ocean SKEB untouched — validated with `DO_SKEB=True` active) |

The uncommitted trees (3–5) are a known liability, itemized in §3.3 as explicit
production work packages. "Detached HEAD" means the checkout sits on a pinned
commit rather than a branch (how CESM externals are checked out) — the listed
modifications exist in the working tree, but committing them requires creating a
branch first.

#### 2.2.1 Repository state at prototype close (verified 2026-07-09)

Latest commits, branches, and remotes, after the final sync-and-push pass. The two
dual-checkout repos (TIM, MOM6) were verified in sync between turbo-stack and the CESM
sandbox; turbo-stack pulls the sandbox clone via its `turbo-local` remote, and the
sandbox clone fast-forwards from the turbo-stack path.

| Repo | Branch | HEAD | Pushed to | Notes |
|---|---|---|---|---|
| TIM / FMS-src (both checkouts) | `parallelio-prototype` | this report's commit, atop merge `c6877379` | `altuntas` = github.com/alperaltuntas/TIM | Other remotes: `origin` = TURBO-ESM/TIM (branch not pushed there — prototype branch stays on the fork), ESCOMP/FMS, mwaxmonsky/TIM |
| MOM6 (both checkouts) | `parallelio-prototype` | `f820a0769` | `altuntas` = github.com/alperaltuntas/MOM6 | Other remotes: `origin` = TURBO-ESM/mom6, NCAR/MOM6 (`upstream`), mom-ocean, marshallward — none carry the branch |
| FMS interface (sandbox `libraries/FMS`) | `add_amrex_submodule` | `b9d52f2` (Makefile.cesm + src tag bump, atop `1164e92` E4 excludes) | `origin` = ESCOMP/TIM_interface | `upstream` = ESCOMP/FMS_interface (not pushed there) |
| stochastic_physics (sandbox external) | `parallelio-prototype` | `681fd9e` | **NOWHERE — unpushed** (sole copy on glade; path below) | Only remotes are shared repos (ESCOMP, NOAA-PSL); needs a personal fork or a deliberate ESCOMP push |
| turbo-stack | — (no prototype branch, by decision) | working tree only | n/a | build.sh, makefile templates, example job scripts, new `examples/cesm_t1_12`; MOM6/TIM submodule pointers floating ahead of the recorded gitlinks |
| cime (sandbox) | detached HEAD (pinned checkout; uncommitted modifications present) | working tree only | n/a | `CIME/Tools/Makefile` link glue uncommitted |
| components/mom wrapper (sandbox) | detached HEAD (pinned checkout; uncommitted modifications present) | working tree only | n/a | xml/buildlib/testmod uncommitted; MOM6 + stochastic_physics pointer updates pending |
| CESM sandbox root | — | pointer-level dirt only | n/a | Reflects the dirty subrepos above; resolves when they are committed and pinned |

The unpushed stochastic_physics commit exists only in the sandbox checkout at
`/glade/work/altuntas/cesm.sandboxes/turbo.cesm3_0_alpha09d.sbx/components/mom/externals/stochastic_physics`.

Unrelated dirt found during the sweep (pre-existing, NOT from this prototype):
turbo-stack `submodules/infra/FMS2` (`dev/turbo`) carries two uncommitted mpp include
changes from the GPU-offload work (`mpp_group_update.fh` `omp_offload` argument,
`mpp_chksum_int.fh` TRANSFER size fix), and `dev-utils/gcovlens` has local tooling
edits. Flagged so they are not mistaken for prototype fallout.

### 2.3 Exit questions — answered

**Q1 — PIO ergonomics: workable, with three hard rules discovered.**
(1) PIO **forbids overlapping decomposition maps** — EINVAL, and heap corruption under
the BOX rearranger. FMS's staggered-read convention (position shift on *every* rank)
overlaps, so TIM reads staggered windows as **4 disjoint components** and writes via
**true partitions** (the extra staggered row/column belongs to the east/north-most rank
only). (2) `write_darray`'s fill argument must **match the variable's own `_FillValue`**
or the write EINVALs — the fill has to travel with the variable's metadata. (3) darray
does **not type-convert**: single-precision variables need their own `PIO_REAL` decomps
(cache keyed by precision). Also: append to FMS-written files works; `PIO_RETURN_ERROR`
is a workable error mode; masked/zero-maplen decomps need non-NULL pointers; PNETCDF
open needs a NETCDF4P fallback.

**Q2 — C API shape: the triplet pattern scales.** Opaque int handles, null-terminated
strings, flat argument lists, config resolved at the composition boundary and injected
as plain options. One trap that must be carried into production: **Fortran-facing ids
must be 1-based** — MOM guards with `if (id > 0)`, and 0-based ids silently dropped the
first registered diag field.

**Q3 — MOM6 dispatch reality: two semantic contracts the design had missed.**
(1) **Collectivity:** FMS non-domain file operations are rank-independent and MOM calls
some of them on the **root PE only** (regridding slabs) — a collective implementation
deadlocks (confirmed in gdb). Hence the `Backend`/`Backend::Serial` split; the
production `File` must document per-method collectivity. (2) `file_exists` is used for
**ASCII** files too (input.nml!) — it cannot be a netCDF probe. Also: domain registries
must grow dynamically (regridding creates temporary decomps mid-run), and RAII still
needs an explicit finalize hook — PIO must die before `MPI_Finalize`.

**Q4 — FMS diag semantics: fully specified and reproduced bit-exactly.** The
reproduce-exactly rules are in `docs/fms_diag_semantics.md`; headline items: scalar
per-*call* weight counter; masked points overwrite accumulation with missing_value;
trigger is strict `time > next_output` at the top of send_data (the triggering sample
belongs to the *next* window); averaged record time = window midpoint; rollover is a
start/close/next-open trio with a data-drop gap; statics written at file close;
repeated text attributes **prepend**; FMS accumulates in default REAL = r8 in our
builds, so C++ double matches bit-for-bit.

**Q5 — restart-spanning averaging: proven, bit-exact, at three levels.** Unit
(manager_test mid-window save/restore ≡ continuous), solo driver (double_gyre 10-day
continuous vs 4+6 mid-window restart: **all history files bit-identical including the
restart-spanning mean**), and wired through the NUOPC cap for CESM. State =
`RESTART/TIM.diag.res.nc` written through the same TIM::IO path. One deliberate
divergence: TIM anchors averaging windows on the diag_table **base date** walked
forward (FMS anchors on init_time) — identical whenever runs start on window
boundaries, and the better semantic.

**Q6 — performance: TIM wins at every scale once iotasks are policy, not accident.**

| Measurement (cesm_t232, masked tripolar) | FMS | TIM | Ratio |
|---|---|---|---|
| Restart reads, 128 ranks (51 seam-timed reads, 2.1 GB restart) | 3.56 s | 1.75 s → **1.51 s** post-design-pass | 2.0–2.9× faster |
| Restart reads, 768 ranks (32 iotasks) | 3.15 s | **1.32 s** | 2.4× faster |
| Restart writes incl. close/flush, 128 ranks | 10.07 s | **2.09 s** | 4.8× faster |
| Full diag A/B segment runtime, 128 ranks | 111 s | **92 s** | diag side favorable |

The iotask sweep (8/32/96/192 iotasks → 5.48/1.32/3.72/4.68 s at 768 ranks) showed the
early "TIM 1.5× slower at 768 ranks" result was **entirely the nprocs/4 default**;
32 iotasks was optimal at both 128 and 768 ranks and is now the default.
At 1/12° (tx1_12, 4320×3240×75): TIM **read the ~140 GB split CESM restart at 2048
ranks** and initialized correctly — a file **in-tree FMS cannot read at all**
("start+count exceeds dimension bound" on its non-symmetric staggered axes; TIM's
file-stagger sniffing handles it). The FMS-vs-TIM timing A/B at that scale was never
obtained: the hand-built standalone zero-forcing case segfaults in its first dynamics
step (a model/config bug, not I/O — see §2.6).

### 2.4 What worked

- **The two-pass strategy itself.** The tactical pass surfaced at least six
  contract-level surprises (overlap ban, fill matching, no type conversion,
  collectivity, ASCII file_exists, 1-based ids) that would each have forced rework of a
  carefully-designed interface. The prototype bought the production design its facts.
- **The backend seam earned its keep empirically.** It absorbed the PNETCDF→NETCDF4P
  fallback, the Serial split, and the precision-keyed decomps without any caller
  changing; the SCORPIO-swap constraint (shared `PIOc_*` subset only) was never
  violated.
- **The C API seam enabled a full C++ redesign with zero Fortran churn.** The mid-
  prototype design pass (tactical `tim_io.cpp` → Decomp2D/Backend/IoSystem/DecompCache/
  File/IoContext) and the later singleton elimination both happened entirely behind an
  unchanged bind(C) surface.
- **Bit-identity as the gate.** Every increment was validated by byte comparison
  against an FMS control (`ocean.stats`, restarts, history files, `available_diags`).
  This caught real bugs cheaply and made "done" unambiguous. The planned capture/replay
  oracle for reduction math was **never needed** — end-to-end bit-identity subsumed it.
- **Literal porting for behavioral modules.** `TIM::Time` as a line-faithful
  `time_manager.F90` port and the Q4-spec-driven Accumulator/DiagManager achieved
  bit-identity on the first serious A/B. Reverse-engineering "reasonable" semantics
  would not have.
- **Riding the compiler during excision.** E3's mechanical gate-stripping (keep the TIM
  branch, drop the FMS tail) with the compiler as the checker made a 1500-line
  deletion safe; the full-suite gates (9/9 SMS, bit-identical stats with DO_SKEB
  active) confirmed it.
- **Performance headroom is real**: 2.4–4.8× over FMS at production scale with a
  host-only backend, before any tuning beyond iotask count.

### 2.5 What didn't work / wrong assumptions / surprises

- **"Coexistence is permanent for now" was wrong — pleasantly.** The plan assumed FMS
  fms2_io/diag_manager sources would remain live indefinitely (Phase 4 deferred).
  In practice, once the seams were TIM-complete, full elimination (E1–E5) took ~2 days
  and removed ~20k lines from the build. The dependency tail (coupler_types,
  field_manager, mosaic2, libFMS umbrella, stochastic_physics external) was tractable
  with dormancy-preserving stubs.
- **The iotask default nearly produced a wrong conclusion.** nprocs/4 made TIM look
  1.5× *slower* at 768 ranks; one sweep flipped it to 2.4× faster. Performance
  defaults are results, not guesses — and need re-measuring per scale class.
- **PIO's decomposition semantics were stricter than documented** (overlap ban, fill
  matching, no type conversion). All three were absorbed in the seam but each cost a
  debugging session; the spike should have probed decomp edge cases harder.
- **Masked-layout staggered writes have holes** (open production item): the write
  partition doesn't claim shared edges bordering *eliminated* tiles, so statics like
  `geolat_c` carry fill where FMS halo-gathers values. Needs mask-aware edge ownership
  in Decomp2D.
- **Infrastructure friction cost real time:** mkmf doesn't relink MOM6 on library
  changes (delete the binary) and doesn't track cross-stage `.mod` deps; PBS jobs
  resolve binaries at *start*, so queued A/Bs need frozen binary copies; watching
  `qstat` races queue transitions (watch TestStatus instead); the `.gitignore`
  `core` pattern silently excluded `tim/cpp/core` from early commits; CIME caches
  `valid_values` and Filepath in case/bldroot.
- **Pre-existing bugs discovered en route** (not ours, still open): TIM restart
  checksums mismatch on restore even FMS→FMS (`RESTART_CHECKSUMS_REQUIRED=False`
  workaround); gnu builds segfault on cesm_t232 standalone; upstream
  `read_field_3d_region` reuses the 2d error header; the intel makefile template
  fp-model gap (fixed in turbo-stack after the Accumulator property tests
  caught non-associative FMA drift).
- **The tx1_12 standalone case crashes in dynamics** — the one planned measurement not
  obtained (see §2.6).
- **Accepted, documented output divergences from FMS:** TIM-written files drop
  `NumFilesInSet` and add `_FillValue`/checksum attrs (data + FMS content checksums
  bit-identical — accepted as-is, user decision); TIM does not create fill-only files
  for never-written fields; window-anchor stamps differ for mid-window-start segments;
  masked-tile cells hold NC fill rather than 0.

### 2.6 Principles scorecard

| Principle | Verdict |
|---|---|
| FMS behavior is the spec | **Met** — bit-identity at every gate, incl. with SKEB active; 4 divergences documented and deliberate |
| Deep modules / complete abstractions | **Met after the design pass** — the first tactical spine violated it by design; the second pass restored it behind the unchanged C API |
| Backend swappable (PIO2↔SCORPIO) | **Met structurally** — single seam, shared subset; an actual SCORPIO build was never exercised (production item) |
| Restart-spanning averaging first-class | **Met and gated** at unit, solo-driver, and CESM-cap levels |
| Classic diag_table first, YAML-extensible | **Met** (parser handles all three example tables incl. legacy forms; YAML untouched) |
| Ensemble-safe, no singletons | **Met** — explicit comms end-to-end, per-component IoContext; a real multi-member run was never exercised |
| Performance ≥ FMS at production scale | **Exceeded** — 2.4× reads, 4.8× writes at t232 scale; 1/12° numbers not obtained |

### 2.7 Not taken on (explicitly out of prototype scope, still open)

- **Diag features MOM6/CESM doesn't currently exercise:** regional diag_table lines,
  diurnal reductions, coarsening/downsampling (sentinel id + warning today), `rms/pow`.
  Production needs an explicit support-or-reject decision per feature.
- **GPU residency:** the design keeps accumulation buffers device-capable
  (`The_Arena`/ParallelFor staging point in `finalizeToHost()`), but the prototype ran
  host-only throughout. No `--offload` validation of the diag tier.
- **1/12° performance numbers** (blocked by the standalone-case dynamics segfault; the
  cheap path is a zero-length segment — read restart → write restart, no stepping).
- **CESM archiving integration:** `TIM.diag.res.nc` is not in rpointer/st_archive, so
  ERS-style same-rundir restarts work but long CONTINUE chains cold-start the window
  (graceful degradation, no crash).
- **Native domain decomposition** (Decomp2D from AMReX DistributionMapping instead of
  the mpp bridge), **YAML config front-end**, **real data_override** (fail-loud stub is
  correct for CESM coupled mode), **coupler_types/mpp/time_manager bridges** (retained
  FMS scope by design), **MOM_netcdf.F90** (already FMS-free, MOM-native).
- **SCORPIO CI build of the spike/backend**, container `pio-utils` fallback build.
- **Repo hygiene:** turbo-stack/cime/mom-wrapper changes uncommitted;
  mask-aware write-edge ownership; the pre-existing restart-checksum bug.

### 2.8 What we'd do differently

1. **Put the collectivity contract in the design vocabulary from day one.** Every File
   method should declare collective vs rank-independent; the deadlock class was the
   most dangerous thing the prototype found.
2. **Spike decomposition edge cases, not just the happy path** — overlap, fills,
   type conversion, zero-maplen. The spike passed 12 tests and still missed the three
   rules that mattered most.
3. **Treat performance defaults as measurements**: budget the parameter sweep with the
   first benchmark, and freeze binaries for queued jobs from the start.
4. **Plan the seam boundary around FMS's *usage*, not its module map** — file_exists
   serving ASCII, root-only slab reads, and 1-based id guards were all usage facts,
   findable by reading call sites first.
5. **Do the elimination survey (E-series scope grep) before declaring coexistence
   permanent** — it turned a "Phase 4, someday" into two days of work, and the
   knowledge would have simplified earlier wrapper decisions.
6. **Skip the capture/replay investment** (it was planned, never needed) and lean on
   end-to-end bit-identity plus targeted property tests (which caught the fp-model
   flag gap — keep those).
7. **Validate one hand-built extreme-scale case interactively before queueing chains
   on it** — the tx1_12 dynamics crash burned the whole 2048-rank A/B budget.

---

## 3. Looking forward: the production plan

### 3.1 Rewrite vs harden — the decision

The plan of record prescribed a fresh implementation. Having built the prototype, a
blanket rewrite would now mostly re-type code that was *already re-designed once*
mid-prototype under the target principles. The recommendation is **differentiated**:

| Layer | Verdict | Rationale |
|---|---|---|
| `core/` (Config, Time, Decomp2D) | **Harden + re-land** | Post-design-pass, unit-tested, literal-port semantics; add mask-aware edge ownership to Decomp2D (the one known defect) |
| `io/` spine (Backend, IoSystem, DecompCache, File, IoContext) | **Harden + re-land** | Survived two redesigns behind a stable API; needs: per-method collectivity docs, error-policy unification (rc codes → amrex::Abort policy at the C API), removal of prototype fallbacks (implicit `MPI_COMM_WORLD` context) |
| `io/ExternalField` | **Harden + re-land** | Written once but against a written FMS spec, gated bit-identical; review for depth |
| `diag/` (Accumulator, DiagManager, DiagConfig) | **True second pass** | The deepest behavioral modules, written once at prototype speed; re-derive from `fms_diag_semantics.md` with the prototype as reference; decide regions/diurnal/coarsening support explicitly |
| C APIs + Fortran interfaces | **Regenerate** | Cheap, and the shape is now known; drop prototype-era knobs (retired gates, debug traces become a proper option) |
| MOM6 infra wrappers | **Rewrite clean** | They evolved by incremental excision and carry scar tissue (dead comments, ordering artifacts); the target end-state is known exactly, so a clean rewrite is *less* work than a cleanup diff, and far more reviewable |
| FMS-source elimination | **Re-apply mechanically** | E4/E5 are small, well-understood commits (`.exclude` files + consumer strips); cherry-pick and re-validate |

"Harden + re-land" means: the prototype file is the *reference*, but the production
commit is authored fresh on `parallelio` — reviewed as new code, with prototype
scaffolding (seam timers, retired dispatch gates, prototype/ probes) never carried
over. Nothing merges from `parallelio-prototype`.

### 3.2 Production design deltas (from prototype findings)

Carrying the architecture of the plan of record with these amendments:

1. **Decomp2D grows mask-aware edge ownership**: shared staggered edges bordering
   eliminated tiles are claimed by a live neighbor (fixes the write-hole divergence).
2. **File documents per-method collectivity**; `Backend::Serial` is a first-class
   design element, not a retrofit.
3. **Variable metadata owns its fill**; decomp cache keyed by (domain, stagger, nz,
   precision) — both now design facts, not discoveries.
4. **Windows anchor on diag_table base date** (documented divergence, kept).
5. **Iotask policy**: default 32, overridable, with a documented guidance note to
   re-sweep per scale class; revisit scaling with data volume at ≥1/12°.
6. **Ids crossing to Fortran are 1-based**; `file_exists` is format-agnostic.
7. **Explicit lifecycle**: `tim_io_init(comm)`/`tim_io_finalize` are mandatory; the
   implicit-context fallback is removed (abort instead).
8. **Diag restart file becomes a CESM citizen**: rpointer + st_archive integration is
   in scope for the production pass, not a caveat.
9. **Domain-agnostic DofMap core**: the decomp cache generalizes to a DofMap cache with
   three factories (domain, block, replicated-policy); `Backend::Serial` demoted to
   metadata probes; every bulk read is a striped collective. Concretely, `tim_decomp_cache`
   becomes `tim_dofmap` (`DofMap` opaque handle + `DofMapCache`): `fromDomain` reproduces
   the old DOF lists bit-identically (so domain-decomposed reads/writes are unchanged by
   construction — same schedules, same bytes; verified against a captured golden),
   `blockDecomp`/`blockDecompRange` add non-domain contiguous partitions. `File` grows
   `readDistributed(DofMap)` as the one deep read primitive (`readDecomposed` wraps it per
   staggered component) and `readReplicated`, whose policy is size-tiered on
   `tim.io.replicated_read_threshold_mb` (default 8): block-collective `read_darray` +
   `MPI_Allgatherv` at/above, a broadcasting `get_var` (verified to read-on-root-and-bcast)
   below. `ExternalField`'s replicated branch and the scalar/1-D `readPlain` path stop
   doing every-rank `nc_open`. Two carried-forward facts: (a) region `readSlab` stays
   rank-independent `Backend::Serial` because MOM's regridding calls it every-PE with
   per-PE-differing hyperslabs (collective would deadlock) — the one surviving every-rank
   bulk read; (b) metadata/attribute/time-value queries stay every-rank (bounded, small) —
   both are flagged follow-ups, not blockers.

### 3.3 Work decomposition — PR series on `parallelio`

Ordered by dependency; each PR is a complete abstraction with its own ctest coverage
(`test_tim/` following the `test_mom` pattern), dead-by-default until its MOM6 consumer
lands; double_gyre bit-identity gate per PR, cesm_t232 at the milestone gates.

**Track A — TIM library (sequential spine):**
- **A1. Build glue + test scaffold.** turbo-stack: parallelio module + PIO flags in
  build.sh (commit the currently-floating changes); TIM CMake: `find_package` for
  ParallelIO, `test_tim/` ctest home. *Gate: FMS2 and TIM builds unchanged.*
  - Modified: turbo-stack `build.sh`,
    `build-utils/makefile-templates/ncar-{intel,gnu,nvhpc}.mk` (fp-model discipline);
    TIM `CMakeLists.txt`.
  - Added: TIM `test_tim/CMakeLists.txt` + common test scaffolding (fixture loader
    reused from `test_mom/common/`); optionally
    turbo-stack `build-utils/pio-utils/Makefile` (container fallback).
- **A2. `core/`: Config + Time + Decomp2D** (with mask-aware edges) + unit tests.
  - Added: `tim/cpp/core/tim_config.{hpp,cpp}`, `tim/cpp/core/tim_time.{hpp,cpp}`,
    `tim/cpp/core/tim_domain.{hpp,cpp}`;
    `test_tim/test_{config,time,decomp}.cpp`.
  - Modified: `CMakeLists.txt` (register sources; mkmf sweeps automatically).
- **A3. `io/` spine: Backend(+Serial) + IoSystem + DofMapCache + File + IoContext**,
  one PR (one abstraction), stacked-review-friendly commits per class; C API + Fortran
  interface; MPI ctest (round-trip, masked+symmetric decomps, FMS cross-read). File's read
  surface is the domain-agnostic layering: `readDistributed(DofMap)` deep primitive,
  `readDecomposed` as its per-component wrapper, `readReplicated` (threshold-tiered
  block-collective / broadcasting get_var). DofMapCache carries three factories
  (fromDomain — bit-identical to the prototype decomp cache — plus blockDecomp /
  blockDecompRange).
  - Added: `tim/cpp/io/tim_backend.{hpp,cpp}`, `tim/cpp/io/tim_iosystem.{hpp,cpp}`,
    `tim/cpp/io/tim_dofmap.{hpp,cpp}` (was `tim_decomp_cache`, generalized),
    `tim/cpp/io/tim_file.{hpp,cpp}`,
    `tim/cpp/io/tim_io_context.hpp`,
    `tim/cpp/io/tim_io_C_API.{h,cpp}` (+ `tim_io_C_API_internal.hpp`),
    `tim/fortran/tim_io_interface.F90`;
    `test_tim/test_io_roundtrip.cpp` (MPI), `test_tim/test_dofmap.cpp` (fromDomain
    DOF identity + blockDecomp round-trip + get_var broadcast semantics),
    `test_tim/test_fms_cross_read.cpp`.
  - Modified: `CMakeLists.txt`.
- **A4. `io/ExternalField`** + its unit test (synthetic modulo-climatology fixture). The
  replicated (non-domain) branch reads through `File::readReplicated` on a PIO handle
  opened unconditionally — no more every-rank `nc_open`/`readSlab`; the test exercises both
  threshold branches (small get_var, large block+Allgatherv) on a float field.
  - Added: `tim/cpp/io/tim_external_field.{hpp,cpp}`;
    `test_tim/test_external_field.cpp`.
  - Modified: `tim/cpp/io/tim_io_C_API.{h,cpp}`, `tim/fortran/tim_io_interface.F90`
    (the `tim_extfield_*` surface), `CMakeLists.txt`.
- **A5. `diag/`: DiagConfig + Accumulator + DiagManager** (second-pass rewrite) +
  parser/property/manager tests.
  - Added: `tim/cpp/diag/tim_diag_config.{hpp,cpp}`,
    `tim/cpp/diag/tim_diag_reduce.{hpp,cpp}` (Accumulator),
    `tim/cpp/diag/tim_diag_manager.{hpp,cpp}` (incl. axis registry),
    `tim/cpp/diag/tim_diag_C_API.{h,cpp}`, `tim/fortran/tim_diag_interface.F90`;
    `test_tim/test_diag_{parse,reduce,manager}.cpp` (manager test runs real PIO,
    incl. the mid-window save/restore unit gate).
  - Modified: `CMakeLists.txt`.

**Track B — MOM6 (each depends on the matching A-item; all under
`config_src/infra/TIM/` unless noted):**
- **B1** (after A3): `MOM_io_infra.F90` rewritten clean onto TIM (no FMS I/O from the
  start — the end state of E3, authored directly). *Milestone gate: read + write +
  cross-matrix bit-identity, double_gyre + cesm_t232.*
  - Modified: `MOM_io_infra.F90` (full rewrite),
    `MOM_domain_infra.F90` (TIM domain registration + native `parse_mask_table`),
    `MOM_coms_infra.F90` (`MOM_infra_init` creates the TIM I/O context on the
    component communicator; `io_infra_end` finalizes it),
    `MOM_ensemble_manager_infra.F90` (filename-suffix setter onto TIM).
- **B2** (after A4): `MOM_interp_infra` + data_override stubs. *Gate: SSS-restoring
  A/B bit-identity.*
  - Modified: `MOM_interp_infra.F90` (ExternalField dispatch; horiz_interp_mod
    retained as compute), `MOM_data_override_infra.F90` (fail-loud stubs).
- **B3** (after A5): `MOM_diag_manager_infra` + solo/NUOPC Q5 hooks + FMS1/FMS2 no-op
  stubs. *Milestone gates: history parity incl. rollover; restart-spanning
  bit-identity; cesm_t232 monthly A/B.*
  - Modified: `MOM_diag_manager_infra.F90` (full rewrite),
    `config_src/infra/FMS1/MOM_diag_manager_infra.F90` and
    `config_src/infra/FMS2/MOM_diag_manager_infra.F90` (no-op
    save/restore stubs — uniform infra interface),
    `config_src/drivers/solo_driver/MOM_driver.F90` and
    `config_src/drivers/nuopc_cap/mom_ocean_model_nuopc.F90`
    (`TIM.diag.res.nc` save beside restarts, restore at init).

**Track C — integration (parallel to late Track B):**
- **C1. FMS-source elimination** (re-apply E4/E5 + stochastic_physics stub). *Gate:
  full SMS suite + bit-identical stats with DO_SKEB; symbol audit (tim_backend sole
  netCDF object).*
  - Modified (FMS fork): `fms/fms.F90`, `coupler/coupler_types.F90`,
    `coupler/ensemble_manager.F90`, `field_manager/field_manager.F90`,
    `horiz_interp/horiz_interp_conserve.F90`, `libFMS.F90`.
  - Added (FMS fork): `fms/.exclude`, `mpp/.exclude`, `mosaic2/.exclude`
    (mkSrcfiles drop lists: fms_io, mpp_io, read_mosaic, grid2/mosaic2 Fortran).
  - Deleted (FMS fork): `coupler/stock_constants.F90` (dead, zero consumers);
    the `fms2_io/`, `diag_manager/`, `time_interp/`, `data_override/`,
    `axis_utils2/` directories leave the build via Filepath (C2) — source
    deletion itself can follow once the suite is green.
  - Modified (stochastic_physics external): `update_ca.F90` (CA-restart stubs).
- **C2. CESM wiring PRs to their own repos:** *(needs proper forks/branches — cime
  and the mom wrapper are currently detached-HEAD).*
  - Modified (FMS_interface): `buildlib` (tim/ Filepath additions + eliminated-dir
    excludes), `Makefile.cesm` (PIO/netCDF INCLDIR), `src` submodule pointer.
  - Modified (mom wrapper): `cime_config/config_component.xml` (TIM in
    `MOM6_INFRA_API`), `cime_config/buildlib` (AMReX include dir),
    `cime_config/config_archive.xml` (+ cap rpointer logic for `TIM.diag.res.nc`
    archiving), `MOM6` and `externals/stochastic_physics` pointers.
  - Added (mom wrapper): `cime_config/testdefs/testmods_dirs/mom/tim/shell_commands`.
  - Modified (cime): `CIME/Tools/Makefile` (`-lamrex -lstdc++` after `-lfms` when
    `MOM6_INFRA_API=TIM`).
- **C3. Scale + GPU tail:** 768-rank suite, tx1_12 zero-length-segment read/write A/B
  (and hand the dynamics segfault to the model side), `--offload` nvhpc build of the
  diag tier, ensemble smoke test, SCORPIO build of the backend as a CI keep-honest job.
  - Modified: `tim/cpp/diag/tim_diag_reduce.{hpp,cpp}` (device-resident accumulation
    buffers via `The_Arena`/ParallelFor, host staging in `finalizeToHost()`), CI
    config for the SCORPIO backend build, turbo-stack `examples/*/job-*.sh` scripts;
    turbo-stack `examples/cesm_t1_12/` committed as a maintained example.

Reviewability is the organizing constraint: no PR mixes a new abstraction with a
consumer cutover; every MOM6 PR is behavior-gated against an FMS control; the
elimination PR is pure deletion + stubs. The prototype's numbers say the payoff is
already banked — 2.4–4.8× I/O at production scale, restart-spanning means FMS cannot
produce, and a 1/12° capability FMS does not have.
