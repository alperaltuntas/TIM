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

## Q6 addendum — 768-rank read result (cesm_t232)

- At 768 ranks (batch, premium): FMS 3.151 s vs TIM 4.683 s max — **TIM 1.5x SLOWER**,
  reversing the 128-rank result (TIM 2x faster). Correctness still bit-identical.
  Suspects: default iotasks = nprocs/4 = 192 (likely far too many for ~2 GB of reads;
  rearranger fan-in overhead), and 51 collective PIOc_openfile/close cycles.
  Production design implication: iotask count must scale with DATA VOLUME, not rank
  count, and file opens should be amortized (MOM_restart reads ~50 vars from ONE file —
  the prototype reopens per read; the production File abstraction naturally holds the
  file open). Tuning runs queued (TIM_PIO_NTASKS sweep).

## Q4 — FMS diag semantics (windows, average_T1/T2, accumulation order)

- TBD.

## Q5 — Restart-spanning accumulator state

- TBD.

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
