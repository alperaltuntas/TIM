# TIM C++ Diagnostics & I/O — Development Plan

> Living design/plan document for the diagnostics and I/O rewrite. Version-controlled
> so that engineers, Claude sessions, and other agents share one source of truth.
> Update it as decisions change; it is the plan of record, not a snapshot.

## Context

TIM (`submodules/infra/TIM`) is turbo-stack's replacement infra library for MOM6: a trimmed
NCAR-FMS fork being incrementally rewritten in C++/AMReX to streamline GPU porting. FMS's
design is intertwined and over-complex. In TIM, we aim for simpler APIs, deeper modules i.e., better
complexity handling, better abstractions, and better decomposition of concepts such that
the end result is a more maintainable and efficient system. The first rewrite target is
**diagnostics and I/O**. Today those subsystems (`diag_manager/`, `fms2_io/`, `mpp/`) are
still live trimmed-FMS Fortran; the only end-to-end C++ port is `TIM::checksum`.

Decisions made:
- **I/O backend: PIO2 (NCAR ParallelIO)** — host-side parallel netCDF with decomposition
  rearrangement. (No library writes CF-netCDF from GPU memory; E3SM/ICON/NEMO all stage
  device→host then hand off to a parallel writer. AMReX has zero netCDF support.)
  **The backend must be swappable to SCORPIO (E3SM's PIO2 fork) quickly**: confine all
  backend calls to one seam and stick to the C-API subset the two libraries share.
- **Restart-spanning averaging**: FMS diag_manager loses partial accumulation at restart
  (a mid-month restart corrupts/splits monthly means). The new diag manager must persist
  and restore accumulator state across restarts — a required feature, not an afterthought.
- **Clean new C++ API** — FMS behavior is the spec, not the interface. MOM6's two wrapper
  files (`config_src/infra/TIM/MOM_diag_manager_infra.F90`, `MOM_io_infra.F90`) are the
  swap seams; their internals get rewritten onto the new bind(C) surface.
- **Classic diag_table parsing first**, internal config model YAML-extensible.
- **Prerequisites staged**: domain metadata bridged from FMS `mpp_domains` (not ported);
  time/calendar bridged first, native C++ `TIM::Time` when diag scheduling needs it.
- **Two-pass development ("design it twice" / build one to throw away)**: a quick,
  deliberately tactical prototype of the whole spine first — its sole purpose is to
  refine the design and this plan — then a fresh production implementation with the
  revised design. See "Prototype pass" below. The prototype lives on the
  `parallelio-prototype` branch and is never PR'd toward main.
- **Performance is a first-class requirement**, not a phase-4 afterthought: the prototype
  must probe performance at production scale (cesm_t232, 768 ranks) so that performance
  findings shape the production design rather than being tuned in after it.

## Status (2026-07-15) — production pass is a GO

- **Prototype CLOSED** (2026-07-09); findings + production plan in
  `docs/tim_diag_io_prototype_report.md` (commit `2338fae4`).
- **Post-close addendum landed on the prototype**: the domain-agnostic DofMap refactor
  (commits `e46d4863`, `e70b2e78`) — see "Domain-agnostic read layering" below and
  report design delta #9. Gates: DOF bit-identity vs captured golden PASS; get_vara
  broadcast-semantics probe PASS; blockDecomp round-trip (both threshold branches) PASS;
  ExternalField small+large PASS; double_gyre runs clean post-refactor (explicit
  pre/post `ocean.stats` A/B still to be confirmed); 768-rank cesm_t232 seam-timer
  non-regression job in flight at time of writing (baseline to match: 1.32 s @768/32
  iotasks, 1.51 s @128).
- **Project buy-in obtained** (PI/Co-PI, 2026-07-15). Commitments made in that review,
  now scheduled in this plan: an EARLY GPU D2H-staging benchmark (P0 item 5), a
  tuned-io_layout FMS A/B for baseline fairness, and the async-iotasks-on-idle-GPU-host-
  cores experiment (both Phase 3).
- **C++20 adopted project-wide** (mkmf templates, CMake, this doc's conventions; verified
  on nvc++ 25.9 / icpx 2025.2.1 / g++; C++23 deferred until nvhpc support matures).
- **Next**: fresh `parallelio` branch, PR series per report §3.3 (track A first). No
  prototype-branch merges; no plan/report/CLAUDE.md files in production PRs.

### Scope accounting — this project vs the total FMS replacement

Line counts of the FMS surface MOM6 needs (subsystem dirs in this fork; wrapper files
in MOM6 `config_src/infra/FMS2`), as of 2026-07-15:

| Subsystem | Lines | Status |
|---|---|---|
| fms2_io (file I/O) | 12,130 | **replaced** (this project) |
| diag_manager | 13,314 | **replaced** (this project) |
| time_manager | 3,723 | **ported** (TIM::Time; full cutover pending) |
| time_interp | 2,387 | **replaced** (ExternalField, this project) |
| axis_utils | 507 | **eliminated** (this project) |
| mpp (comms + domains + clocks + error) | ~20,000 live | remaining — the big one |
| horiz_interp | 5,351 | remaining |
| coupler types | 4,914 | remaining (NUOPC coupled runs) |
| data_override | 1,415 | remaining (stubbed; coupled runs need it) |
| constants, misc | ~200 | trivial |

This project ≈ **~50% of the total replacement scope by volume** (~32k of ~65k lines;
the wrapper-surface lens agrees: ~45% of the 7,067-line infra-wrapper surface). By risk
it was more than half (external-library integration + subtle semantics + a new
capability). Of the remaining half, **mpp_domains (halo exchange/decomposition) is most
of the substance** — the other performance- and GPU-critical subsystem (GPU-aware
exchanges; eventual AMReX DistributionMapping alignment, Phase 4). The tail
(horiz_interp, coupler types, data_override) is workmanlike by comparison.

## Key facts (verified during exploration)

- MOM6's ENTIRE FMS diag surface = 11 procedures + 4 constants in
  `MOM_diag_manager_infra.F90` (register_diag_field/static_field, send_data with weight,
  diag_send_complete, set_time_end, init/end, field_add_attribute, get_diag_field_id,
  diag_axis_init, get_diag_axis_name; DIAG_FIELD_NOT_FOUND, null_axis_id, EAST, NORTH).
  ~1,100 registered fields, ~1,200 post sites. **Weighted time-averaging/reduction happens
  in FMS** — the new diag manager must implement it. MOM6 owns vertical remapping,
  horizontal averaging, masking/staggering, diag buffers, restart bookkeeping — out of scope.
- MOM6's file-I/O surface = ~20 fms2_io primitives in `MOM_io_infra.F90` (open/close/
  flush, read_data ×34 uses, write_data, register_axis/field/attributes, variable
  queries, get_global_io_domain_indices, FmsNetcdfFile_t vs FmsNetcdfDomainFile_t).
- The two wrapper seams are independent **on the MOM6 side** (FMS diag_manager writes
  through its internal diag_output path, not through MOM_io_infra, so swapping one wrapper
  never touches the other) — but the cutover is ordered, not symmetric: the new DiagManager
  writes through TIM::IO, so I/O must land first, and during transition the FMS and TIM
  I/O stacks coexist in one executable.
- Decomposition metadata: MOM6's `MOM_domain_type` wraps FMS `domain2D` directly; extract
  via existing mpp accessors. Time: FMS `time_type` (days/seconds/ticks) via
  `MOM_time_manager.F90` (96 lines).
- TIM C++ conventions to follow: `tim/cpp` + `namespace TIM`, C-API triplet pattern of
  `tim_coms_infra_C_API.{h,cpp}` + `tim/fortran/*_interface.F90`; `amrex::ParallelDescriptor`
  is the MPI layer (AMReX already initialized on MOM's communicator,
  `MOM_coms_infra.F90:517`); errors via amrex::Abort; C++20 (nvc++ binds the standard;
  C++23 deferred until nvhpc support matures); mkmf sweeps all TIM .cpp into
  libTIM.a (build.sh threads AMReX-style include/link flags).
- Derecho: `parallelio/2.6.8` module exists for intel/gcc/nvhpc with PnetCDF + parallel
  netCDF4 (`libpioc`, `pio.h`, CMake configs). **Gotcha: the compiler-only-hash builds
  are mpi-serial — always resolve via `module load parallelio` under cray-mpich**, and
  under whatever ncarenv turbo-stack's build.sh currently loads (module pins were
  recently updated to latest defaults — re-verify the parallelio version at P0 time). Container/CI fallback: build PIO2 as a submodule via a
  `build-utils/pio-utils/Makefile` cloned from `build-utils/amrex-utils/Makefile`.
- Prior art: TIM remote branch `dev/ncar_add_pio` (~15k-line Fortran fms2_pio_io tree,
  "PIO domain write working") — semantic reference for decomp construction and filename
  conventions; not to be merged.
- Validation cases: `examples/double_gyre` (4 ranks, minutes, has diag_table + job-gpu.sh,
  now configured for 4-GPU runs) = per-PR gate; `examples/cesm_t232` (1/4° global, 768
  ranks on masked 25×40 layout, IO_LAYOUT 1,1, monthly means, `%4yr-%2mo` file templating)
  = milestone gate. `nccmp` module available. pFUnit/ctest infra exists in
  `turbo-stack/tests/`.
- **CMake build system MERGED** (PR #16, June 2026): TIM's `CMakeLists.txt` now
  `find_package(AMReX REQUIRED)` and builds a `tim` STATIC library from `tim/cpp` sources
  (explicit source list — new tim/cpp files must be ADDED to the target, unlike mkmf's
  auto-sweep), links `AMReX::amrex` + `MPI::MPI_CXX`, installs headers, exports
  `cmake/TIMConfig.cmake.in`; TIM has its own `build.sh` (install prefix, `--parallel`).
  mkmf via turbo-stack build.sh remains the production path.
- **C++ test infra EXISTS** (PR #18): `test_mom/` is a standalone CMake/ctest project
  running the AMReX-ported continuity kernels against captured Fortran fixtures.
  Reusable pieces: `test_mom/common/captured_io.{hpp,cpp}` (.meta/.bin fixture loader,
  incl. integer reads per PR #20) and `test_mom/common/amrex_assertions.{hpp,cpp}`;
  `mom/cpp/CMakeLists.txt` packages kernels as a standalone `mom_continuity` lib.
  Fixtures live in `/glade/work/altuntas/mom6_iturbo_data`; configure with
  `-DTEST_MOM_DATA_DIR`.

## Architecture (new code, all in TIM)

```
tim/cpp/core/tim_time.{hpp,cpp}       TIM::TimeStamp{days,seconds,ticks}, Calendar enum,
                                      advance/toDate/fromDate/intervalIn (native calendar math)
tim/cpp/core/tim_domain.{hpp,cpp}     TIM::Decomp2D (global sizes, compute/data extents in
                                      1-based global indices, symmetric flag, layout,
                                      io_layout, MPI comm) + Stagger enum{Center,EastFace,
                                      NorthFace,Corner} + globalNx/Ny, localExtent helpers
tim/cpp/io/tim_backend.{hpp,cpp}      TIM::IO backend seam: the ONLY translation unit(s)
                                      that include pio.h. Thin internal interface over the
                                      ~25 PIOc_* calls TIM needs (init/finalize, initdecomp,
                                      create/open/close, def_dim/def_var/put_att/get_att,
                                      inq_*, setframe, write/read_darray, put/get_vara).
                                      PIO2 and SCORPIO share this C API (SCORPIO is a PIO2
                                      fork; same PIOc_* names) — swapping = pointing
                                      PIO_INSTALL_PATH/module at SCORPIO + rebuilding, with
                                      any signature drift absorbed HERE, nowhere else.
                                      Rules that keep the swap cheap: no PIO types/ids in
                                      any public TIM::IO header (opaque ints only), no use
                                      of PIO2-only or SCORPIO-only extensions, iotype/
                                      rearranger names mapped through TIM enums.
tim/cpp/io/tim_iosystem.{hpp,cpp}     TIM::IO::IoSystem — iosysid lifecycle via the backend
                                      seam, one per communicator, iotasks from io_layout
                                      (env-overridable)
tim/cpp/io/tim_file.{hpp,cpp}         TIM::IO::File — THE deep module: open()->optional
                                      (merges exists+open), plain vs domain files unified,
                                      defineAxis/defineVar/putGlobalAtt, implicit enddef,
                                      record management inside write() (write_time_if_later
                                      semantics), case-insensitive findVar/read,
                                      writeAxes() writes global coords (kills
                                      get_global_io_domain_indices + .nc.XXXX filesets),
                                      metadata queries, checksum atts,
                                      setFilenameSuffix (filename_appendix). Reads layer on
                                      the DofMap core: readDistributed(DofMap) is the one
                                      deep read primitive; readDecomposed wraps it +
                                      DofMapCache::fromDomain (per staggered component);
                                      readReplicated returns a full field on every rank
                                      (block-collective + Allgatherv at/above the
                                      tim.io.replicated_read_threshold_mb size, a
                                      broadcasting get_var below it). writeDecomposed
                                      routes through fromDomain + writeDistributed (bytes
                                      unchanged). readSlab stays rank-independent
                                      (Backend::Serial) for MOM's per-PE region reads
tim/cpp/io/tim_dofmap.{hpp,cpp}       DofMap (opaque handle: PIO decomp id + local
                                      count) + DofMapCache — the DOMAIN-AGNOSTIC
                                      decomposition core. A PIO decomp is not a domain
                                      (PIOc_InitDecomp takes only gdims + per-rank dofs),
                                      so the cache owns three factories: fromDomain
                                      (dofs from a Decomp2D window; the read-component /
                                      write-partition families, the k*gnx*gny+(j-1)*gnx+i
                                      formula, symmetric +1 edges, masked layouts,
                                      precision-keyed decomps — all moved verbatim from
                                      the retired DecompCache, bit-identical), blockDecomp
                                      (contiguous 1-D block partition of a flattened array
                                      over the iosystem comm — no domain), and
                                      blockDecompRange (one flat sub-range, for striped
                                      3-D replicated reads). The cache is the sole creator
                                      of decompositions (InitDecomp is collective/expensive)
tim/cpp/diag/tim_diag_config.{hpp,cpp} DiagConfig{FileSpec,FieldSpec} + parseClassicDiagTable
                                      (YAML front-end slots in later against same model)
tim/cpp/diag/tim_diag_axis.{hpp,cpp}  AxisRegistry (value store; null_axis=0)
tim/cpp/diag/tim_diag_reduce.{hpp,cpp} Accumulator — weighted mean/min/max/none (+rms/pow
                                      later), buffers in amrex::The_Arena() (device-capable),
                                      accumulate via ParallelFor, finalizeToHost() = the ONE
                                      device→pinned-host staging point before PIO.
                                      Serializable: exposes state()/restore() over
                                      {partial buffer, wsum, count, window_start} so
                                      in-progress reductions survive restarts
tim/cpp/diag/tim_diag_manager.{hpp,cpp} DiagManager — registerField/post/endOfTimestep/
                                      setEndTime/end; field↔table matching, per-file output
                                      scheduling + new_file_freq rotation + %4yr-%2mo names
                                      (via TIM::Time), average_T1/T2/time_bnds, static
                                      fields, missing values; writes ONLY through TIM::IO.
                                      Restart-spanning averaging (FMS can't do this):
                                      saveState(path)/restoreState(path) persist every
                                      mid-window Accumulator + its window metadata to a
                                      diag restart file (e.g. RESTART/TIM.diag.res.nc,
                                      written through TIM::IO with the same Decomp2D;
                                      one variable per in-progress stream + wsum/count/
                                      window attrs). Window boundaries computed from the
                                      diag_table base date in absolute time — never from
                                      run start — so a mid-month restart resumes the same
                                      month's mean exactly. On restore: match streams by
                                      (module,field,file) name; missing state = fresh
                                      window (new diag_table entries just start clean)
tim/cpp/tim_io_C_API.{h,cpp}          extern "C": tim_domain_register(tim_domain_c*),
tim/cpp/tim_diag_C_API.{h,cpp}        tim_io_open/define/write/read/query/close (int
                                      handles), tim_diag_init/axis_init/register_field/
                                      post/send_complete/set_time_end/end; tim_time_c
                                      {days,seconds,ticks}; strings null-terminated
tim/fortran/tim_io_interface.F90      bind(C) modules (tim_coms_infra_interface.F90 pattern)
tim/fortran/tim_diag_interface.F90
```

Design rules: DiagManager contains zero netCDF/PIO calls; TIM::IO contains zero AMReX/
diag knowledge (pure host code). FMS-isms stay in the Fortran wrappers: EAST/NORTH→Stagger
mapping, logical-mask+rmask merge into one rmask, is_in/ie_in defaulting→LocalExtent,
null_axis_id→zero-axis registration, r4→r8 copies. `Post{data, extents, rmask, weight,
memspace}` is the core diag primitive — takes a raw pointer so both today's Fortran host
arrays and future device-resident MultiFab components work (wrapper flips memspace flag).

### Domain-agnostic read layering (post-refactor)

The read primitive takes an arbitrary **DofMap**; the MOM domain is one of the DofMap
factories, not a requirement. This inverts the old layering (where a read needed a
Decomp2D) so non-domain reads can stripe collectively at 1/36° scale instead of every
rank doing its own `nc_open`. Rules established while landing it:

- **Domain reads are unchanged by construction.** `fromDomain` produces the exact DOF
  lists the retired `DecompCache` did (verified bit-identical via a captured golden;
  `prototype/pio_spike/dof_{capture,identity}.cpp`), so the rearranger schedules and the
  bytes on disk are identical — domain-decomposed performance is unaffected.
- **`readReplicated` policy is size-tiered.** At/above `tim.io.replicated_read_threshold_mb`
  (env `TIM_REPLICATED_READ_THRESHOLD_MB`, default 8) it block-decomposes the flattened
  field, `read_darray`s it collectively, and `MPI_Allgatherv`s to replicate; below it, one
  `PIOc_get_vara_double` reads on the I/O root and broadcasts (proven in
  `prototype/pio_spike/probe_getvar_bcast.cpp`) — a tree bcast beats the allgather latency
  for small fields. Both branches leave the full field on every rank.
- **`Backend::Serial` is demoted to metadata/attribute/dimension/time-axis probes** plus
  the root side of any root+bcast. Its every-rank bulk read of external-forcing records is
  gone (`ExternalField` now flows through `File::readReplicated`); `readPlain`
  (scalar/1D replicated) now rides the broadcasting `get_var` on the held-open PIO handle.

**Flagged follow-ups (deferred):**
1. **Region `readSlab` stays rank-independent.** MOM's `read_field_{2d,3d}_region`
   (regridding) call `tim_io_read_slab` on every PE with *per-PE-differing* start/count, so
   it cannot become a collective/broadcasting read without deadlock or wrong data. It
   remains `Backend::Serial` (independent `nc_open` per PE). This is the one surviving
   every-rank bulk-read path; a future optimization could detect uniform args across ranks
   and route those through a collective `get_var`, but the seam cannot tell in general.
   (Deviation from the original refactor brief's "readSlab has no callers outside
   root+bcast", justified by verified MOM6 call-site collectivity.)
2. **Metadata/attribute/time-value queries stay every-rank** (`Backend::Serial`, bounded
   and small). A cheap root+bcast wrapper for these is a clean future win but not required.
3. **Int-displacement chunking of huge replicated reads.** `MPI_Allgatherv` displacements
   are `int`; a >INT_MAX-element replicated field (only a 3-D field ≳2 GB/rank, which
   nobody replicates) falls back to striping one z-level at a time (`blockDecompRange`).
   Realistic replicated fields (2-D at 1/36° ≈ 84 M elements) take the single-shot path.
4. **Diag-restart axis-coordinate variables hold uninitialized values** (pre-existing, not
   from this refactor). `TIM.diag.res.nc` defines index-dimension coordinate variables
   (`sx_c`, `sy_n`, `sx_e`, `sy_c`, `dim2`, `dim3`) but the diag manager's `saveState`
   never writes meaningful coordinates for them, so they carry heap garbage that differs
   run-to-run of the *same* binary. Found during the double_gyre A/B: all accumulator DATA
   (`acc0..acc9`) and every model output (`ocean.stats`, `MOM.res.nc`, history) are
   bit-identical run-to-run and before/after; only these cosmetic axis coords vary. Fix in
   the diag-restart writer (write the index coords, or omit the coordinate variables).

## Prototype pass (first; timeboxed ~2-3 weeks)

A quick end-to-end implementation of the whole spine (bridges → backend seam → File →
diag manager → MOM6 dispatch), deliberately tactical. It is DONE when it has answered
the exit questions below — not when it works well. Branch: `parallelio-prototype`; its
code is reference material for the production pass, never a starting point (exception:
boring proven parts — build glue, bind(C) marshalling, domain-extraction bridge — may be
carried over as-is). The abstraction-increment principle governing the production pass
(below) deliberately does NOT apply here.

**Exit questions the prototype must answer:**
1. PIO ergonomics: decomp construction for masked (cesm_t232 25×40/768) + symmetric
   staggered layouts; append-to-FMS-written files; livable error-handling mode;
   PIO2/SCORPIO shared-subset contract (absorbs the former standalone PIO spike).
2. C API shape: handle granularity, string/attribute crossing, where FMS-isms leak.
3. MOM6 dispatch: what the `file_type` dual-backend dispatch really looks like in
   `MOM_io_infra.F90`; any wrapper procedures that resist clean mapping.
4. FMS diag semantics: exact averaging-window boundaries, average_T1/T2/time_bnds
   values, month rollover, accumulation order needed for bit-identical means.
5. Restart-spanning state: workable restart-file format for mid-window accumulators;
   state volume at cesm_t232 scale.
6. **Performance at production scale**: history-write, restart-write, and
   restart/IC-read wallclock vs FMS baselines on cesm_t232 (768 ranks); BOX vs SUBSET
   rearranger and PNETCDF vs NETCDF4P at that scale; per-step overhead of register/post
   traffic with the real ~1,000-field load; diag accumulation overhead. Record numbers
   in the findings doc — these shape production design choices (decomp caching,
   buffering, iotask counts), not just tuning.

**Bar (asymmetric):** double_gyre is the correctness workhorse; cesm_t232 is exercised
for the performance probes and the masked-layout questions, not full parity. Chase
bit-identity ONLY on the restart-read path (cheap, highest signal); for diagnostics,
quantify diffs rather than eliminating them. One compiler; hardcode freely; mean +
snapshot reductions only; tests only where they answer an exit question.

**Deliverable:** a revised version of THIS document (interfaces corrected from
experience, risks retired/confirmed, estimates re-based) plus a findings/lessons doc
(the PR-distilled-lessons pattern of `generate_amrex_code/lessons.md`) — not the code.
Then the production pass below begins fresh.

## Phased roadmap (production pass — after the prototype)

**Increment principle: the units of development are complete abstractions, not
features.** Each phase implements a module's whole designed interface as one unit;
end-to-end feature demonstrations ("TIM reads a restart", "restart-spanning means") are
validation gates *inside* phases, never separate increments. This forbids shipping half
an interface shaped by its first feature (e.g. a read-only File) and bolting the rest on.
(The prototype pass is exempt by design; this principle governs production development.)

### P0 — Prerequisites
1. **Build glue (turbo-stack PR):** `module load parallelio` in build.sh for all three
   compilers; `PIO_INCLUDE_FLAGS`/`PIO_LINK_FLAGS` (`-lpioc` only) threaded like AMREX_*
   into libTIM mkmf and MOM6 link; `build-utils/pio-utils/Makefile` submodule build for
   containers. Gate: existing FMS2 + TIM builds unchanged, link succeeds.
2. **C++ unit tests:** the CMake base is already merged (PR #16) and `test_mom/` proves
   the pattern (PR #18). Remaining work: add `find_package` for ParallelIO to TIM's
   CMakeLists (module on Derecho ships CMake config packages), register the new
   `tim/cpp/{core,io,diag}` sources in the `tim` target as they land, and stand up
   `test_tim/` (or extend test_mom) as the ctest home for the new unit tests, reusing
   `test_mom/common/captured_io` + `amrex_assertions`. mkmf remains production; CMake
   is dev/test.
3. **PIO2 de-risking: absorbed by the prototype pass** (exit questions 1 and 6). The
   required outputs stand: the TIM::IO surface frozen against real PIO behavior, and the
   PIO2/SCORPIO shared-subset contract recorded (diff the PIOc_* signatures used against
   SCORPIO's headers, github.com/E3SM-Project/scorpio) — both land in the prototype
   findings doc before production Phase 1 starts.
4. **Bridges:** `tim_domain_register` bind(C) call from `MOM_domain_infra.F90` (extract
   via mpp_get_compute/global_domain, layout, symmetric offsets → Decomp2D registry);
   time crossing as tim_time_c + calendar enum (FMS stays the calendar oracle until 2a).
5. **Early GPU D2H-staging benchmark (PI-review commitment; promoted from Phase 3):**
   a standalone mini-app (amrex_mini_app pattern) on a GPU node measuring the two
   staging patterns for a realistic diag load (~hundreds of 2D/3D fields, double_gyre-
   to-t232-sized slabs): (a) per-timestep device→host copy of every posted field (the
   FMS-equivalent pattern) vs (b) device-side accumulation in `The_Arena` buffers with
   one pinned-host `finalizeToHost` per output window. Output: measured D2H traffic +
   wallclock ratio, and confirmation that the once-per-window pattern's payoff justifies
   the Accumulator device design before Phase 2 builds it. This de-risks the "GPU
   considerations not yet measured" concern with data, early.

### Phase 1 — TIM::IO File abstraction + MOM6 I/O cutover
The COMPLETE File interface (open/inquiry/read/metadata-definition/write) implemented as
one unit against the frozen spike contract — not a read-only slice extended later.
- TIM: tim/cpp/core + tim/cpp/io (whole module) + C API + unit tests.
- MOM6: bind(C) interface module; dispatch inside `MOM_io_infra.F90` — add a TIM handle
  member to `file_type` beside the FmsNetcdfDomainFile_t pointer; runtime namelist flag
  (default off) so one binary A/Bs both paths; per-file dispatch on the write side
  (MOM6 restart bookkeeping is native and flows entirely through this seam).
- **Read gate (FIRST MILESTONE: "TIM reads a MOM6 restart")**: double_gyre + cesm_t232
  restart-read through TIM → `ocean.stats`/log checksums **bit-identical** to FMS-read
  control (reading same bytes ⇒ identical evolution; the strongest cheap oracle; touches
  no output files; rollback = namelist flag). Validate this gate first — it exercises the
  whole spine while write code is still being finished.
- **Write gate**: cross-matrix (FMS-write→TIM-read, TIM-write→FMS-read); exact-restart
  test (N days vs N/2+restart+N/2); `nccmp -d -m -g -f` TIM vs FMS restart — data
  bit-identical (raw doubles), metadata diffs whitelisted.

### Phase 2 — Diag abstractions (config, reduction engine, manager)
- 2a: DiagConfig + diag_table parser (fixtures: verbatim double_gyre + cesm_t232
  diag_tables) + native TIM::Time calendar math (unit-tested against FMS time_manager
  as oracle).
- 2b: Accumulator — the complete abstraction including state()/restore() persistence as
  first-class state FROM THE START (not a later extension): replicate FMS
  accumulate-then-normalize order for bit-reproducible means; missing values, masks,
  average_T1/T2/time_bnds; serialization over {partial buffer, wsum, count, window_start}.
- 2c: DiagManager — complete interface including saveState/restoreState (diag restart
  file + window scheduling anchored to absolute calendar time) + rewrite of
  `MOM_diag_manager_infra.F90` internals + the MOM6 restart hook (`tim_diag_save_state`
  next to the driver's save_restart trigger; restore during `MOM_diag_manager_init` when
  a diag restart file exists; absence = cold start, so existing run scripts work
  unchanged). Cutover: whole-run backend flag (never per-field — exactly one backend
  owns history files per run).
- **Parity gate**: available_diags parity; double_gyre history nccmp (bit-identical
  target for snapshots and for means if op order matches; documented ≤1e-15 fallback
  per-field); then cesm_t232 monthly history parity incl. file rotation/naming.
- **Restart-spanning gate** (capability FMS lacks — no FMS behavior to match): double_gyre
  N-day run with multi-day means vs same run restarted mid-window → history files
  bit-identical (accumulation order preserved by construction, so strict identity is the
  right bar); repeat with a mid-month cesm_t232 restart.
- Capture/replay aid (extends lessons.md §8): capture (slab, weight, mask, time)
  sequences at the send_data_infra seam from a real run; replay into Accumulator; compare
  to FMS-written netCDF — isolates reduction bugs from I/O bugs. Build on the existing
  fixture format and loader (`test_mom/common/captured_io.{hpp,cpp}`) rather than
  inventing a new one; fixtures alongside the kernel captures in
  `/glade/work/altuntas/mom6_iturbo_data`.

### Phase 3 — Scale, GPU, performance
- Verification at scale rather than discovery: the prototype already measured cesm_t232
  768-rank performance and fixed rearranger/iotype choices; this phase confirms the
  production implementation meets or beats those prototype baselines (and FMS baselines
  via tim_profile/CPU_stats), plus `--offload` nvhpc builds each phase (PIO stays
  host-only; accumulation buffers flip to device via The_Arena).
- **Tuned-io_layout FMS A/B (PI-review commitment):** all prototype ratios are vs FMS at
  IO_LAYOUT=1,1 (CESM's actual config but not FMS's best case). Run the same seam-timed
  reads/writes against FMS with a tuned io_layout for an honest architectural comparison
  (expect FMS to close some of the read gap; writes still carry fileset + recombine cost).
- **Async-iotasks experiment (PI-review commitment):** PIO async mode with dedicated I/O
  ranks on the idle host cores of GPU nodes (~60/node) — overlap file I/O with GPU
  compute; measure vs the intracomm baseline. Also the natural on-ramp for evaluating
  SCORPIO's async service if PIO2's proves limiting.
- **1/36°-class write test:** a synthetic-or-real high-res case write at scale to verify
  the extrapolated feasibility claim (FMS ≈ 100 min vs TIM ≈ minutes for a ~1.2 TB
  restart) and to exercise the buffer-limit knob + Allgatherv level-striping paths under
  real memory pressure.

### Phase 4 — Deferred (explicit)
- Native domain2D replacement (Decomp2D producer switches to AMReX DistributionMapping),
  YAML config front-end, retiring FMS fms2_io/diag_manager sources.
- **Trimmed FMS Fortran keeps compiling throughout**: MOM_interp_infra, data_override,
  coupler_types still use FMS — coexistence is permanent for now (shared libnetcdf, no
  symbol clash; dispatch owns file handles so no filename can be double-written).

PR discipline (strangler-fig): each phase = TIM PR (dead-by-default code + unit tests) →
turbo-stack PR (build glue) → MOM6 PR (dispatch, default off). Flip defaults only after
gates pass on both examples. `--infra FMS2` never touched.

## Risks
1. FMS averaging-window edge semantics ((t0,t1] boundaries, average_T1/T2, month rollover)
   — biggest behavioral risk; mitigated by capture/replay oracle + A/B runs.
2. Wrong PIO build (mpi-serial hash) / netCDF-HDF5 stack mismatch — module-resolve under
   the same ncarenv stack; assert parallel capability at init.
3. Masked PE layouts + symmetric staggered +1 sizes in decomp dofs — dedicated unit tests;
   cross-check global dims vs FMS-written files.
4. PIO append to FMS-written files & collective put_var discipline for scalars — spike
   covers; all File methods collective by contract.
5. Bit-identity for averaged fields may need tolerance — snapshots/restarts stay strict.
6. PIO2/SCORPIO API drift (SCORPIO has diverged in places: async I/O tasks, ADIOS iotype,
   some added args) — mitigated by the backend seam + the spike's shared-subset contract;
   CI could later add a SCORPIO build of the spike to keep the seam honest.
   **The API subset is only the source-compatibility seam — behavior differs underneath.**
   SCORPIO is primarily an internal-behavior fork ("improvements in user data caching and
   aggregation algorithms", per its docs): both libraries copy write_darray data into
   internal buffers and defer real I/O, but SCORPIO caches data+metadata ops far more
   aggressively (different host-memory high-water and flush timing behind an identical
   API), its async service relocates buffering to dedicated I/O procs, and the ADIOS
   iotype adds ADIOS2 engine buffers + deferred netCDF conversion. Mitigations:
   (a) TIM's File contract tolerates any caching policy by construction — write() promises
   only "library took a copy"; durability is pinned to flush()/close(); callers can never
   assume drain timing; (b) any SCORPIO validation run must measure memory high-water,
   flush/durability timing, and close cost — not just file correctness; (c) the library
   buffer limit (e.g. PIOc_set_buffer_size_limit) becomes a TIM::Config knob when needed —
   at 1/36° a per-rank buffered 3D field slab is hundreds of MB between flushes, so the
   limit is a real tunable, not a footnote.
7. Diag-restart state volume (~one 2D/3D partial-sum buffer per actively-averaged stream;
   at cesm_t232 scale potentially hundreds of 3D fields) — written once per restart through
   the same parallel path as restarts themselves; acceptable, but monitor size/time and
   consider skipping streams whose window happens to close exactly at restart time.

## Verification
- Unit (ctest): parser fixtures, reduction property tests, decomp mapping (masked +
  symmetric cases), calendar math vs FMS oracle.
- Integration (MPI ctest 4–16 ranks): decomposed write/read round trip, FMS↔TIM cross-read.
- End-to-end: double_gyre every PR (minutes); cesm_t232 at milestone gates; `ocean.stats` +
  log checksums for solution invariance; `nccmp -d -m -g -f` for files; GPU double_gyre
  (`job-gpu.sh`) each phase.
- Restart-spanning averaging: continuous vs mid-window-restarted run → bit-identical
  time-mean history (double_gyre routinely, cesm_t232 mid-month restart at the Phase-2 gate).

## Critical files
- `submodules/MOM6/config_src/infra/TIM/MOM_io_infra.F90` — I/O seam (spec + rewrite target)
- `submodules/MOM6/config_src/infra/TIM/MOM_diag_manager_infra.F90` — diag seam
- `submodules/MOM6/config_src/infra/TIM/MOM_domain_infra.F90` — domain-bridge call site
- `submodules/infra/TIM/tim/cpp/` — all new C++ (core/, io/, diag/, C APIs)
- `submodules/infra/TIM/tim/cpp/tim_coms_infra*` — conventions precedent
- `/glade/work/altuntas/turbo-stack/build.sh` + `build-utils/amrex-utils/Makefile` — build glue
- `submodules/infra/TIM/CMakeLists.txt` + `test_mom/` — merged CMake/ctest base to extend
- TIM branch `dev/ncar_add_pio` — reference only (earlier Fortran PIO-under-fms2_io effort)
