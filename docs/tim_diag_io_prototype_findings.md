# Prototype findings — TIM diagnostics & I/O

Accumulating answers to the six exit questions in `tim_diag_io_plan.md` ("Prototype
pass"). Every entry cites how it was established (spike test, run, measurement).

## Q1 — PIO ergonomics

- **Module resolution (Derecho, ncarenv/25.10):** `parallelio/2.6.8` loads for
  intel/2025.2.1, gcc/14.3.0, nvhpc/25.9 but **requires `cray-mpich` loaded first**
  (a `module --force purge` environment fails; `module reset` environments are fine
  because cray-mpich is in the default set). Loading it auto-pulls
  `parallel-netcdf/1.14.1`. The module sets `NCAR_ROOT_PARALLELIO` (also `$PIO`);
  lib is `libpioc` (+`libpiof`, unused by us). Confirmed the cray-mpich (parallel)
  spack hash resolves, not the mpi-serial one.
- **Build:** compile/link with the `CC` craype wrapper: `-I$NCAR_ROOT_PARALLELIO/include
  -L$NCAR_ROOT_PARALLELIO/lib -lpioc`. Gotcha: ncarcompilers exports `CXX=icpx` into the
  environment, which silently overrides Makefile `CXX ?= CC` defaults — and bare `icpx`
  does not add MPI paths. Use `CC` explicitly.
- **Zero-cell (all-land) ranks:** `PIOc_InitDecomp` returns `PIO_EINVAL` (-36) if the
  compmap pointer is NULL, even with `maplen == 0` — an empty `std::vector::data()` is
  NULL, so a zero-maplen rank crashes unless a valid dummy pointer is passed. Same
  hazard for `write_darray`/`read_darray` buffers. The TIM backend seam must guard
  every pointer it forwards for the empty-rank case (cesm_t232's masked layout makes
  this a first-class path, not an edge case). Found via spike job 6657125.
- **Spike results (4 ranks, 2×2 layout, 8×6 global; job 6657147): ALL 12 PASS.**
  - Bitwise write→reopen→read roundtrips for {PNETCDF, NETCDF4P, NETCDF-serial} ×
    {BOX, SUBSET}.
  - Masked decomp (global holes + one zero-maplen rank) correct on PNETCDF/BOX and
    /SUBSET; unmapped cells receive the fill value passed to `PIOc_write_darray`
    (verified via collective `PIOc_get_vara_double`, whose result is valid on all
    ranks — usable for replicated reads).
  - Symmetric corner var (nx+1, ny+1; +1 row/col owned by edge ranks) roundtrips.
  - Append: `PIOc_openfile(PIO_WRITE)` + `inq_dimlen(time)` + `setframe(nrec)` +
    `write_darray` grows the record dim correctly on PNETCDF and NETCDF4P.
  - `PIO_RETURN_ERROR` mode: missing file → rc=-49, missing var → error code, library
    remains fully usable afterward. Error-code model is workable for optional-open
    semantics.
  - Still open within Q1: append to an *FMS-written* file (test when the spine reads
    real double_gyre output); behavior at 768 ranks (Q6).
- **Build-system note:** turbo-stack `list_paths` sweeps the whole TIM tree, so
  `prototype/pio_spike/main.cpp` (with its `main()`) will land in libTIM.a path_names.
  Archive members are pulled on demand so it is probably harmless, but verify on the
  first full `--infra TIM` build; if it collides, exclude `prototype/` from list_paths.
  Matrix: {PNETCDF, NETCDF4P, NETCDF-serial} × {BOX, SUBSET} roundtrips;
  masked map with global holes + one zero-maplen rank; symmetric corner var
  (nx+1, ny+1); append to existing file (PNETCDF + NETCDF4P); PIO_RETURN_ERROR
  ergonomics (missing file/var, library usable afterward).

- **Overlapping decomposition maps are FORBIDDEN in PIO — violently.**
  FMS fills every rank's full staggered compute window ([isc..iec+1], overlapping
  at shared edges via mpp_get_compute_domain's position shift on EVERY rank).
  Attempting to express that as a PIO decomp: `PIOc_InitDecomp` and
  `PIOc_InitDecomp_ReadOnly` return PIO_EINVAL on some ranks, and the BOX
  rearranger can heap-corrupt (glibc malloc abort) — verified by
  `prototype/pio_spike/probe_overlap.cpp`. **Solution: read each staggered window
  as up to 4 strictly disjoint pieces** (main block, east strip [iec+1]×[jsc..jec],
  north strip, NE point), each a cached decomp; strips are disjoint across ranks
  by construction. Production TIM::IO must bake this in.
- **MILESTONE PASSED — "TIM reads a MOM6 restart"** (double_gyre, 4 ranks,
  symmetric memory, gnu): restart continuation day 10→20 with all read_field_2d/3d
  traffic through TIM/PIO vs FMS control → `ocean.stats` bit-identical, final
  restarts byte-identical. Staggered (u/v) reads exercised the 4-piece scheme.
- **Pre-existing TIM bug found (NOT ours, needs separate fix):** restart
  continuation fails out-of-the-box on the TIM build — MOM_restart's stored
  field checksums (computed via the C++ TIM::checksum through mom_chksum) do not
  match on restore, even FMS-write→FMS-read with the same binary (h: stored
  3C51BC9E... vs recomputed DAF4DF94...). Restart runs evidently never exercised
  on this stack. Milestone runs used RESTART_CHECKSUMS_REQUIRED=False. Suspect
  TIM::checksum vs FMS mpp_chksum semantics (masking/unscale/window). File as an
  issue against tim_coms_infra.
- **Build gotcha:** mkmf's MOM6 link target does not depend on libTIM.a /
  libinfra-TIM.a — after a library-only change the binary silently stays stale;
  delete the MOM6 binary (or touch an object) to force relink.

## Q2 — C API shape (bind(C) surface)

- **Symmetric staggered decomposition convention (must match FMS to read its
  files):** staggered axis has nig+1 file points; file index p ↔ MOM global
  staggered index I = p−1; each rank owns p ∈ [isc, iec] and the east/north-most
  rank additionally owns p = nig+1. Interior ranks' high-edge staggered points are
  not read — MOM fills them by halo update afterward (same as FMS behavior).
  Payoff: the Fortran-side copy offset into caller arrays becomes uniform —
  0 for compute-sized arrays, isc−isd for halo (data-domain) arrays — identical
  for centered and staggered fields. The size-sniffing FMS does in
  `domain_offsets` reduces to one comparison.
- Prototype passes contiguous compute-window buffers across bind(C) (one copy in
  the wrapper). Ergonomic; revisit for production only if the copy shows up in Q6
  measurements.

## Q3 — MOM6 dispatch reality (MOM_io_infra.F90)

- All read variants now dispatch to TIM under TIM_IO_READ=1:
  read_field_{0d,1d,2d,3d,4d}, read_field_{0d,1d}_int (replicated PIO get_vara,
  int conversion in the wrapper), read_vector_{2d,3d} (two staggered reads with
  CGRID/BGRID/AGRID position mapping). NOT dispatched (still FMS): the region
  reads (read_field_{2d,3d}_region) — not on the restart path; scope when a real
  consumer appears (MOM_horizontal_regridding global slabs) — plus the query/
  metadata paths (open_file/get_file_times/get_file_fields).
- 4-d vars: file dims are (t,z2,z1,y,x); the flat file index is the same as a
  flattened nz=nz1*nz2 3-d read, but PIO requires the decomp ndims to match the
  variable, so the decomp is built with 4 gdims. (Untested until a 4d consumer
  appears — cesm_t232.)
- Coverage evidence (TIM_IO_DEBUG=1, double_gyre restart continuation, still
  bit-identical): 13 decomposed reads (h/sfc/ave_ssh center; u/u2/ubtav/diffu/CAu
  east-face; v/v2/vbtav/diffv/CAv north-face; nz=1 and nz=2) + 2 plain scalar
  reads (First_direction, DTBT). Corner, 4d, and read_vector paths await a
  consumer (cesm_t232).

## Q3b — Write path (restart writes)

- **WRITE GATE PASSED** (double_gyre, 4 ranks, symmetric, gnu): with TIM_IO_WRITE=1
  the entire MOM_io_infra write seam (open_file/write_metadata_axis/field/global/
  MOM_write_axis/write_field_0d..4d/close) routes through TIM/PIO. Full cross matrix
  bit-identical: TIM-written restart data+attrs == FMS's (only diff: FMS's legacy
  NumFilesInSet global att, intentionally dropped); FMS-reads-TIM-written and
  TIM-reads-TIM-written continuations bit-identical to control through day 30 incl.
  byte-identical day-30 restarts.
- Write decompositions must be TRUE PARTITIONS (PIO forbids overlap on writes too):
  staggered vars use main block + extra east/north point on the east/north-most rank
  only — a third decomp family beside the read components.
- A variable's staggering at write time is implicit in its registered dims — the file
  handle registry must remember each axis's kind/position (this is state the production
  File abstraction owns naturally).
- PIO createfile defaults to CLASSIC; pass PIO_64BIT_OFFSET to match FMS output format.
- Seam gotchas found: open_file allocates the FMS fileobj before any dispatch decision —
  TIM-owned closes must not let fms2_close_file touch the never-opened object; and
  changing file_type's layout requires wiping the MOM6-stage build dirs (mkmf does not
  track cross-stage .mod dependencies — stale objects segfault).

## Q6 — Write performance (cesm_t232, 128 ranks, intel)

- Seam-timed A/B, identical instrumentation bracketing write_field_2d/3d/4d AND
  close_file (PIO buffers darray writes; the flush lands in close, so close time
  counts as write time): 32 calls incl. the ~2.1 GB restart write —
  - FMS (fms2_io): max over ranks 10.07 s
  - TIM (PIO/PNETCDF, partition decomps): max 2.09 s — **4.8x faster**
  - (Excluding close, TIM's write CALLS alone are 0.27 s — do not quote that
    number; the flush is real work.)
  - ocean.stats identical; restart data identical on all active cells.
- **Accepted difference (hygiene)**: cells belonging to masked/eliminated tiles
  (written by no rank) read as 0 in TIM-written files vs the netCDF default
  fill (9.97e36) in FMS-written ones. Three fill mechanisms were ineffective
  through PIO 2.6.8's PNETCDF darray path (write_darray fillvalue,
  PIOc_def_var_fill, PIOc_set_fill) — unwritten sparse-file regions read as
  zeros. Model-invisible (eliminated tiles are never read by active ranks;
  cross-read gates pass). Production options: PIO fill investigation, or one
  explicit fill pass over the coverage complement (context can compute it).

## Q6 — Iotask sweep (cesm_t232 restart reads, 768 ranks, tactical code)

- TIM_PIO_NTASKS: 8 -> 5.48 s, **32 -> 1.32 s**, 96 -> 3.72 s, 192 (=nprocs/4
  default) -> 4.68 s. FMS reference: 3.15 s. **The entire "TIM slower at 768"
  result was the iotask default; at 32 iotasks TIM reads 2.4x faster than FMS
  at 768 ranks.** Too many iotasks costs far more than too few (rearranger
  fan-in + per-open collectives). Default now capped at 64; production policy
  should scale iotasks with data volume, not rank count.
- Operational hazard confirmed: queued PBS jobs resolve the binary path at
  START; the NTASKS=192 leg died exit-127 by racing a rebuild that had deleted
  the binary. Freeze binary copies for queued jobs (t112 jobs resubmitted
  against a frozen copy, read job with TIM_PIO_NTASKS=128).

## Q6 addendum — 768-rank read result (cesm_t232)

- At 768 ranks (batch, premium): FMS 3.151 s vs TIM 4.683 s max — **TIM 1.5x SLOWER**,
  reversing the 128-rank result (TIM 2x faster). Correctness still bit-identical.
  Suspects: default iotasks = nprocs/4 = 192 (likely far too many for ~2 GB of reads;
  rearranger fan-in overhead), and 51 collective PIOc_openfile/close cycles.
  Production design implication: iotask count must scale with DATA VOLUME, not rank
  count, and file opens should be amortized (MOM_restart reads ~50 vars from ONE file —
  the prototype reopens per read; the production File abstraction naturally holds the
  file open). Tuning runs queued (TIM_PIO_NTASKS sweep).

## Design iteration (second pass, 2026-07-07): which abstractions survived

The tactical spine was reimplemented behind the SAME C API as five components:
`core/tim_domain` (Decomp2D + Stagger + Window value types with all extent
logic as methods), `io/tim_backend` (only pio.h includer; typed thin wrapper
over the PIO2/SCORPIO-shared subset), `io/tim_iosystem` (explicit-lifetime
singleton owning the DecompCache; iotask policy isolated in one function),
`io/tim_decomp_cache` (owns the ReadComponent/WritePartition families), and
`io/tim_file` (deep, move-only RAII File: factories return optional, implicit
enddef, internal record management, file-stagger sniffing, axis/var registry).
All double_gyre gates (read, write, cross-read) and the cesm_t232 128-rank A/B
re-passed bit-identical. Lessons:
- **Decomp2D as the geometry oracle worked extremely well**: window /
  readComponents / writePartition as METHODS killed the triplicated index
  arithmetic of the tactical code; the C++ diff is dramatic.
- **Held-open File cache (adapter-level policy): 128-rank seam reads dropped
  1.75 s → 1.51 s vs FMS 4.45 s (2.9x)**; also the right structural answer to
  the 768-rank open/close overhead (sweep results pending).
- **RAII needs an explicit finalize seam anyway**: PIO resources must die
  before MPI_Finalize, and a Fortran-driven program has no scope for that —
  io_infra_end must call tim_io_finalize() (missing it aborts inside
  MPI_Finalize). RAII protects the error paths; the happy path needs the hook.
- **Singletons eliminated (ensemble requirement)**: IoSystem is a plain RAII
  object on an explicit communicator; File borrows its IoSystem; IoContext owns
  iosystem + domain registry + file caches. The bind(C) adapter holds ONE
  explicitly-created context per component (tim_io_init(localcomm) from
  MOM_infra_init / tim_io_finalize from io_infra_end) — CESM ensemble members
  each get their member-pelist iosystem. The only remaining static is the
  adapter's context pointer, which is the honest minimum at a bind(C) boundary
  where Fortran carries no handle; multiple components in one executable only
  need the adapter to grow a handle.
- **Keeping the C API stable made the redesign cheap to validate** (all gates
  reran unchanged) — evidence for the plan's "narrow bind(C) surface as the
  seam" bet.

## Q3c — Query paths, region reads, and the collectivity contract

- All remaining MOM_io_infra paths now dispatch through TIM: the read-handle
  queries (open READONLY / get_file_info / get_file_times / get_file_fields /
  field_exists / get_field_size / file_exists) and read_field_{2d,3d}_region.
- **Collectivity finding (deadlocked us; production-critical):** FMS serves all
  NON-domain file operations with rank-independent serial netCDF, and MOM
  exploits that by calling several of them on the ROOT PE ONLY (horizontal
  regridding reads global z-slabs on root, then broadcasts itself). A
  collective implementation (PIO get_vara / collective open) deadlocks there —
  one rank waits in a collective the others never enter. The backend seam now
  has an explicit split: Backend (collective PIO; decomposed reads/writes) vs
  Backend::Serial (per-rank plain netCDF; queries, plain reads, slabs). The
  production File abstraction must carry this contract in its interface docs:
  decomposed I/O is collective, everything path-based is rank-local.
- Domain registrations are not few/static: cold-start regridding creates
  temporary decompositions (blew a MAX=4 memo table); registries must grow.
- **COLD-START GATE PASSED** (cesm_t232 from WOA z-init, 128 ranks, intel):
  5 region reads through TIM; cold-start restart byte-identical to the FMS
  control — full initialization state identical.
- FMS oddity worth reporting upstream: read_field_3d_region's error header
  says "read_field_2d_region".

## Q6 — tx1_12 first attempt: TIM reads what in-tree FMS cannot

- The 2048-rank A/B failed as designed but taught two things:
  1. **The in-tree FMS CANNOT read the tx1_12 CESM restart in a
     symmetric-memory build**: `NetCDF: Start+count exceeds dimension bound`
     reading u (file has non-symmetric staggered axes, lonq = nig; symmetric
     FMS asks for nig+1). **TIM's file-stagger sniffing handles the same file**
     — the TIM leg sailed past every restart read and died much later on a
     standalone-config gap (SURFBAND_SOURCE=COUPLER invalid for the solo
     driver; fixed with #override USE_WAVES=False in the standalone example).
  2. FMS-vs-TIM read timing on this restart flavor is therefore impossible
     directly; job 6671898 bootstraps it: TIM reads the CESM restart and
     writes a fresh symmetric-convention restart, then FMS and TIM legs A/B
     on that file.

## Q4 — FMS diag semantics (windows, average_T1/T2, accumulation order)

- Answered: the full behavioral spec (12 reproduce-exactly rules with FMS
  source citations) lives in docs/fms_diag_semantics.md; Accumulator and
  DiagManager implement it and cite it. File-format details were additionally
  pinned against real FMS-written history files (double_gyre prog, cesm_t232
  h.sfc): diag files carry `axis`/`positive` axis attributes (NOT the
  `cartesian_axis`/`sense` pair fms2_io restarts use), time gets
  units-since-base + lowercase `calendar` + `bounds="time_bounds"`, the
  bounds dim/coordinate is `nbnd` (values 1,2), average_DT's units are the
  bare unit name, and every data var carries _FillValue AND missing_value
  typed by packing (registered missing else CMOR 1e20). FMS appends
  " time: mean|point|..." to the MOM-supplied cell_methods attribute.

## Q4/Q5 — Accumulator implemented (designed) + fp-model discipline finding

- Accumulator (tim/cpp/diag/tim_diag_reduce.*) implements the FMS spec exactly:
  scalar per-call counter, masked-point overwrite, rms weight-in-power,
  mask_variant per-point counters, rmask post-pass, +/-HUGE min/max init,
  empty-window EMPTY emission, diurnal samples, divide-at-output. Q5
  persistence (state()/restore()) is first-class; unit-checked incl. the
  restored-stream-bit-identical property. 13 property checks pass.
- **Bit-reproducibility build finding**: the intel makefile template sets
  -fp-model source/-no-fma for Fortran and C but CXXFLAGS was bare
  -std=c++17 — Intel's default fast fp-model + FMA contraction changes
  rounding of b += x*w (caught by the unit test failing at -O2 and passing
  at -fp-model=precise). CXXFLAGS now carries -fp-model precise -no-fma
  (intel) and -Mnofma -Kieee (nvhpc); gnu defaults are already strict.
  Any future C++ that does model arithmetic depends on this.

## Q5 — Restart-spanning accumulator state

- **Unit-level gate PASSED** (manager_test): a run that saveState()s at the
  middle of an averaging window and restores into a FRESH DiagManager
  produces records bit-identical to the continuous run — both the
  restart-spanning window and subsequent ones, including average_T1/T2.
- Design that makes it work: output windows are anchored on the diag_table
  base date and walked forward past init_time (FMS anchors at init_time,
  which is what corrupts its restarted windows). Identical to FMS whenever a
  run starts on a window boundary — always true in practice — and coherent
  across arbitrary restart points. The state file stores, per stream, the
  accumulation buffer (+ mask_variant counter) through the SAME decomposed
  write path as everything else, count0d/num_elements, and the window trio;
  matching is by (module, field, file, output_name), so table edits degrade
  to fresh windows instead of errors.
- Remaining for the integration gate: double_gyre continuous-vs-mid-window
  restart A/B through MOM6, and a mid-month cesm_t232 restart.

## DiagManager implemented (designed) + backend findings it forced

- tim/cpp/diag/tim_diag_manager.* is the deep module: registration/fan-out
  (case-insensitive, multi-file), the strict-> window scheduler with midpoint
  record times, EVERY_TIME/END_OF_RUN, the rollover trio state machine with
  the close_time/next_open drop gap, a literal port of FMS get_time_string
  for %-token filenames (unit-checked: ".0001-006" style suffixes incl.
  cumulative tokens), statics at file close, and the full FMS metadata set.
  Zero pio.h/AMReX; writes only through TIM::IO::File. AxisRegistry is a dumb
  value store (tim_diag_axis.hpp); all interpretation is in the manager.
- Verified end-to-end on 1 rank with real PIO (prototype/diag_probe/
  manager_test.cpp, `make manager_test CXX=CC`): means/snapshots bit-exact
  against replayed arithmetic, boundary-sample trigger semantics, >= end
  flush, statics, scalar streams, attribute plumbing, and the Q5 gate above.
- **Backend constraint found: PIO rejects (EINVAL) a write_darray fill value
  that differs from the variable's own _FillValue.** Once diag vars carry
  registered missing values as fills, the old hardwired NC_FILL_DOUBLE fill
  argument breaks every decomposed write. The fill now travels with the
  variable (File::VarInfo) through the seam.
- **Backend constraint found: the darray path does not convert types** —
  float (packing=2) variables need float-basetype decompositions. DecompCache
  keys now include precision; Backend::writeDArray converts the double
  buffer at the seam. Reads of float vars through readDecomposed would hit
  the same constraint (not yet exercised — restarts are all double).
- File gaps the manager exposed, now fixed: per-var numeric/text attribute
  puts, custom fill+missing on defineVar, rank-1 record variables in
  writePlain (average_T1 and scalar time series have ONLY the record dim),
  global-attribute reads for the state file.
- Known metadata diffs vs FMS files (whitelist candidates for the nccmp
  gate): time_bounds carries a default _FillValue (FMS: none), attribute
  order within vars differs, NumFilesInSet absent.
- Not in the prototype (fails cleanly at registration): regional output,
  diurnalNN, coarsening. The empty-window case writes the raw buffer with a
  warning where FMS errors (send_data) — FMS itself writes the raw buffer at
  diag end.
- C++ pitfall re-confirmed (third time): an in-class default argument
  `const Options& opts = {}` cannot use the class's own default member
  initializers — use a delegating ctor pair (IoSystem precedent).

## Q6 — Performance at production scale (cesm_t232, 768 ranks)

- **Read performance, 128 ranks (single node, interactive), cesm_t232 tx2_3v2
  (540×480×75, tripolar, AUTO_MASKTABLE, symmetric), intel build:** restart
  continuation reading the 2.1 GB MOM.res.nc + grid/topo files — 51 seam-timed
  read calls (identical instrumentation for both paths, bracketing the whole
  read_field/read_vector bodies):
  - FMS (fms2_io):  max over ranks 3.556 s (root 3.543 s)
  - TIM (PIO, PNETCDF iotype, BOX rearranger, 32 iotasks = nprocs/4): max 1.746 s
  - **TIM reads 2.0x faster**; the ~1.8 s saving is visible in total wallclock
    (53.1 s → 51.1 s). Correctness: final restarts byte-identical, ocean.stats
    identical.
  - Also validates the read spine on a tripolar masked-layout production config.
- 768-rank production numbers: batch job queued (same experiment, 6 nodes).
- Method notes: run-segment length for cesm_t232 must be set via ocean_solo_nml
  (months/days/hours) — DAYMAX alone does not bound the segment in this config.
  The gnu build segfaults on cesm_t232 (unrelated to TIM I/O; case has only ever
  run intel) — worth its own issue.

## Build-glue changes made (working trees, uncommitted)

- turbo-stack `build.sh`: `parallelio/2.6.8` appended to the three Derecho module
  lines; `PIO_INSTALL_PATH`/`PIO_INCLUDE_FLAGS`/`PIO_LINK_FLAGS` (`-lpioc`) derived
  from `NCAR_ROOT_PARALLELIO` (or preset `PIO_INSTALL_PATH` for containers) and
  threaded into the libTIM mkmf and MOM6 link stages (TIM infra only).
- TIM CMake `find_package` for PIO: deferred until spine code exists.
