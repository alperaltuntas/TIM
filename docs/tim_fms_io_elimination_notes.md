# TIM development state + FMS I/O elimination plan (working notes)

Running state notes for the TIM prototype and the FMS I/O elimination
(E1-E4). Prototype-branch working document — NOT for the eventual PR to
main. The authoritative sections for continuing work are at the BOTTOM
(most recent last): "E3 PART 2 REMAINING" has the MOM_io_infra inventory
and strategy; "E3 PART 2a DONE" is the latest completed step.


On 2026-07-02 we researched and finalized a development plan for rewriting TIM's
diagnostics and I/O in C++ (deep modules; user removed explicit Ousterhout name-drops
from docs but the principles stand). **Plan of record: `docs/tim_diag_io_plan.md`,
committed on TIM branch `parallelio-prototype` (966625eb).** Strategy decided 2026-07-07:
TWO-PASS development — a timeboxed (~2-3 wk) tactical prototype of the whole spine on
`parallelio-prototype` whose deliverable is a REVISED plan + findings doc (never PR'd to
main; must answer explicit exit questions incl. production-scale PERFORMANCE on
cesm_t232/768 ranks — performance is a first-class requirement), then a fresh
abstraction-first production implementation on `parallelio` (clean, no doc/plan files
in its eventual PR to main). `CLAUDE.md` + `add_tim_cpp_module` skill remain UNTRACKED
working-tree files (also committed on branch `diag-io-plan-docs`, 39a91939).
IMPORTANT: user directed 2026-07-07: do NOT commit anything in these repos without
their explicit okay (plan-file commit on the prototype branch was explicitly approved).

UPDATE 2026-07-07 later: user OK'd committing freely on the prototype branches.
**FIRST MILESTONE PASSED**: "TIM reads a MOM6 restart" — double_gyre restart
continuation with read_field_2d/3d through TIM/PIO is bit-identical to FMS control
(ocean.stats + final restarts). Committed: TIM f2ede655 (tim/cpp/io spine + spike +
findings), MOM6 8c58afb73 (dispatch). turbo-stack build.sh changes still UNCOMMITTED
(no prototype branch there). Key findings: PIO forbids overlapping decomp maps
(EINVAL + BOX-rearranger heap corruption) → staggered windows read as 4 disjoint
pieces; FMS staggered convention = mpp position shift on EVERY rank (overlap);
PRE-EXISTING BUG: TIM restart checksums (TIM::checksum via mom_chksum) mismatch on
restore even FMS→FMS — milestone used RESTART_CHECKSUMS_REQUIRED=False; mkmf does
not relink MOM6 on lib changes (delete binary to force). All read variants dispatched
(TIM a3ad4bfc, MOM6 c5b9a0e77; region reads + metadata queries still FMS).
**PERF RESULT (2026-07-07): TIM/PIO reads 2.0x faster than FMS on cesm_t232 at 128
ranks** (51 seam-timed reads incl. 2.1 GB restart: FMS 3.556s vs TIM 1.746s max;
restarts byte-identical; tripolar+masked+symmetric validated; findings committed
32426108; seam timers in MOM6 ab0e56566). 768-rank batch job 6658459 queued for
production numbers (staged dirs ct768_* in /glade/derecho/scratch/altuntas/tim_proto).
Gotcha: cesm_t232 segment length must be set via ocean_solo_nml months/days/hours,
NOT DAYMAX; gnu build segfaults on cesm_t232 (pre-existing, intel-only case).
tx1_12 (1/12°, 4320×3240×75): standalone example created at
turbo-stack/examples/cesm_t1_12 (from gmarques' G-case CaseDocs; pristine copies
kept as *.orig; zero forcing; minimal diag_table, CESM original = diag_table.orig;
INPUT + 20-file ~140GB split restart symlinked from gmarques' run dir; year-0018
restart, ocean_solo.res crafted; turbo-stack repo — UNCOMMITTED, no prototype
branch there). Read-perf A/B jobs queued: 6658459 (ct768, 768 ranks tx2_3) and
6659782 (t112, 2048 ranks tx1_12, 16 nodes); job scripts self-report seam timings
into tim_ct768.o*/tim_t112.o* + walltime.txt in run dirs under
/glade/derecho/scratch/altuntas/tim_proto. TIM iotype open fallback
PNETCDF→NETCDF4P committed (d0c842e8).
**WRITE GATE PASSED 2026-07-07** (TIM 80a31fb5, MOM6 144a2f0ab): TIM_IO_WRITE=1 routes
the whole write seam through PIO; double_gyre full cross matrix bit-identical (TIM
restart data == FMS incl. attrs, only NumFilesInSet dropped; FMS-reads-TIM and
TIM-reads-TIM continuations identical thru day 30). Key write findings: write decomps
must be true partitions (extra staggered point to east/north-most rank only);
stagger recovered from registered dims (per-file axis registry); PIO_64BIT_OFFSET
for FMS format parity; open_file's preallocated fileobj must be shielded from
fms2_close_file; mkmf doesn't track cross-stage .mod deps (wipe MOM6 stage dirs after
file_type layout changes). PERF at 768 ranks: TIM reads 1.5x SLOWER than FMS
(4.68 vs 3.15s; reverses 128-rank 2x win) — suspects: iotasks=nprocs/4 too many +
per-read open/close; tuning sweep queued (job 6662517, TIM_PIO_NTASKS 8/32/96/192).
tx1_12 A/B jobs 6660148/6660149 queued (16 nodes premium, "Qlist" wait). Interactive
tx1_12 debugging done: config valid to OOM wall; 42GB/5-var read test values-verified.
**DESIGN PASS DONE 2026-07-07** (user-directed: reimplement with OOP/RAII/SOLID/deep
modules as a learning exercise, still on parallelio-prototype): TIM 499652fa, MOM6
42ef04a3f. New structure behind the UNCHANGED C API: core/tim_domain (Decomp2D value
type owns window/readComponents/writePartition geometry), io/tim_backend (only pio.h
includer = SCORPIO seam), io/tim_iosystem (explicit-lifetime singleton, iotask policy
isolated), io/tim_decomp_cache (2 families), io/tim_file (deep move-only RAII File),
io/tim_io_C_API.cpp (adapter + held-open read-file cache). Old tim_io.{hpp,cpp}
deleted. All gates re-passed bit-identical; 128-rank t232 reads now 2.9x faster than
FMS (1.51 vs 4.45s, cache effect). Key lesson: RAII still needs an explicit finalize
hook (io_infra_end → tim_io_finalize) — PIO must die before MPI_Finalize.
Design lessons in findings doc "Design iteration" section.
**SINGLETONS ELIMINATED 2026-07-07 (user-directed, CESM ensembles)**: TIM c473c7aa +
1f181dc7, MOM6 1ef7f3516. IoSystem = plain RAII on explicit MPI_Comm; File borrows
IoSystem; new IoContext (tim_io_context.hpp) owns iosystem+domains+file caches; C API
adapter holds ONE explicitly-created context via tim_io_init(fcomm) called from
MOM_infra_init(localcomm) (ensemble members → per-member iosystems), destroyed by
tim_io_finalize from io_infra_end. All gates re-passed (gnu); intel rebuilt for queued
jobs. **WRITE PERF measured 2026-07-07**: cesm_t232 128 ranks, close/flush counted: FMS
10.07s vs TIM 2.09s = 4.8x faster (TIM 7064e40d, MOM6 8cf08f9b0 timers). Accepted
diff: masked-tile cells 0 vs NC-fill (3 PIO fill mechanisms ineffective; documented).
**IOTASK SWEEP (768 ranks)**: 8/32/96/192 iotasks → 5.48/1.32/3.72/4.68s vs FMS
3.15s — old "1.5x slower" was ENTIRELY the nprocs/4 default; 32 iotasks = 2.4x
faster than FMS. Default now capped at 64 (dbd17918); production: scale with data
volume. HAZARD: queued jobs resolve binary at start — freeze copies
($BASE/MOM6_t112_frozen); t112 jobs resubmitted: 6663445 control, 6663446 timread
(TIM_PIO_NTASKS=128). **QUERY+REGION DISPATCH + TIM::Config DONE 2026-07-07 late** (TIM 910cf026, MOM6
7065db1fb + earlier): open_file(READONLY)/get_file_info/times/fields/field_exists/
get_field_size/file_exists + read_field_2d/3d_region all dispatch through TIM
(File gained inquiry methods + readSlab; region reads = replicated hyperslabs).
Double_gyre gates PASS incl. combined TIM_IO_READ+WRITE run. Gotchas fixed: FMS
file_exists is used for ASCII files (input.nml!) → INQUIRE-based; upstream
read_field_3d_region reuses "2d_region" error header (report upstream);
**.gitignore 'core' pattern silently excluded tim/cpp/core — tim_domain.hpp was
MISSING from earlier commits; fixed + recovered in 910cf026.**
TIM::Config (core/tim_config.*): sole ParmParse includer; resolve-then-inject at
composition boundary; keys tim.io.{read,write,debug,pio_ntasks}; ./TIM_input lazily
addfile'd; env vars override; validated (file-driven run bit-identical, env defeats
file). User plans standalone PR: suggested tim_config + refactor tim_profile as
main-compatible demo; PR pushback arguments documented in conversation (empirical
seam payoffs: PIO seam absorbed 2 surprises, C API seam enabled full C++ rewrite
with zero Fortran churn; ParmParse global-table analysis).
IN FLIGHT at node expiry: cold-start control leg DONE (ct_gen, current intel binary);
TIM leg re-queued as job 6664116 (premium, frozen binary MOM6_coldtim_frozen,
self-reports region-read count + restart byte-compare into t232_coldtim.o*).
t112 pair still queued: 6663445/6663446. Remaining FMS traffic after this: diag
history (next work), forcing/interp paths (scoped separately), MOM_netcdf direct.
**COLD-START/REGION GATE PASSED 2026-07-07 evening** (TIM 910376be, MOM6 7185fb75e):
cesm_t232 cold start from WOA z-init, TIM_IO_READ=1 → 5 region reads via TIM,
restart byte-identical to FMS control. TWO CRITICAL SEMANTIC FINDINGS on the way:
(1) **collectivity contract** — FMS non-domain file ops are rank-independent and MOM
calls some ROOT-ONLY (regridding slabs); collective PIO there DEADLOCKS (confirmed
via gdb). Backend now split: Backend (collective PIO, decomposed I/O) vs
Backend::Serial (per-rank plain netCDF: queries/plain/slab reads). Production File
must document per-method collectivity. (2) domain registries must grow (regridding
creates temp decomps; MAX_TIM_DOMAINS now 64). Job 6664116 qdel'd (ran interactively
instead). **I/O SPINE COMPLETE (task 3)**: everything reachable through MOM_io_infra
is now TIM/PIO. Remaining FMS: diag history, forcing/interp, MOM_netcdf direct.
Still queued: t112 pair 6663445/6663446 (2048-rank perf).
**DIAG TIER STARTED 2026-07-07 night (user: designed components directly, no tactical
diag pass)**: DiagConfig+classic parser committed (0a83dba0; parses double_gyre/
t232/t112 tables incl. legacy .true./.false. reductions; core/tim_time.hpp seeded).
**Q4 ANSWERED** (04db3dd3, docs/fms_diag_semantics.md): key reproduce-exactly rules —
scalar per-CALL weight counter (count_0d += w once per call if any point unmasked,
NOT per point), divide at output, masked points OVERWRITE accumulation with
missing_value (only mask_variant has per-point counters), trigger = strict
time > next_output at TOP of send_data (triggering sample → NEXT window),
averaged record time = window MIDPOINT, average_T1/T2/time_bnds = window bounds,
next_output via calendar increment_date, end-of-run flush uses >=, rollover
start/close/next_open trio with data-drop gap, filename %-token stamping w/
filename_time, statics written at file close, _FillValue+missing_value both
(CMOR 1e20 default), FMS accumulates in default REAL (=r8 in our builds → C++
double matches). t112 failures diagnosed: FMS CANNOT read tx1_12 restart
(nonsym staggered axes) while TIM CAN (580cfc2e); USE_WAVES=False added for
standalone; bootstrap chain job 6671898 queued (TIM reads CESM restart, writes
fresh restart, then FMS-vs-TIM A/B). examples/cesm_t1_12 still uncommitted
(turbo-stack has no prototype branch).
DIAG TIER as of 2026-07-08: Accumulator committed (f914eee2, 13 property
checks; caught the CXXFLAGS fp-model gap — intel/nvhpc templates fixed in
turbo-stack AND turbo-stack-iturbo; iturbo's OFFLOAD path also fixed:
CXXFLAGS += -Mnofma -Kieee made unconditional + nvcc-cxx-wrap.sh strips
-Kieee and re-injects both via -Xcompiler; iturbo intel template still has
the gap — user's call). TIM::Time calendar math committed (b374d6e3): literal
time_manager.F90 port (Gregorian 400-yr cycle, Julian mod-4, two-mode
increment_date incl. Jan31+1mo error), Calendar explicit everywhere,
addInterval = diag_time_inc; exhaustive roundtrip tests pass.
**DiagManager committed (8caf9063)**: full deep module (fan-out, strict->
scheduler w/ midpoint times, rollover trio + drop gap, get_time_string port,
statics at close, average_T*/time_bounds/nbnd, CMOR 1e20 fills, cell_methods
time suffix; saveState/restoreState). manager_test (1 rank, real PIO) ALL
PASS incl. **Q5 unit gate: mid-window save+restore bit-identical to
continuous run**. Deliberate divergence: windows anchored on diag_table BASE
DATE walked past init_time (FMS anchors init_time), = FMS when runs start on
boundaries. Backend findings: PIO EINVALs write_darray fills that mismatch
the var's _FillValue (fill now travels in File::VarInfo); darray does NOT
type-convert → float vars need PIO_REAL decomps (DecompCache keyed by
precision). File gained putVarAtt (text/double/int), defineVar fill_missing,
rank-1 record vars in writePlain, globalAttText. Not in prototype: regions,
diurnal, coarsening. Build tests: prototype/diag_probe/Makefile
(`make manager_test CXX=CC` with parallelio modules).
**DIAG SEAM DONE + double_gyre GATE PASSED (2026-07-08)**: tim_diag C API
(93e78b95, TIM) + MOM_diag_manager_infra rewrite (2e2b6b585, MOM6): whole-run
dispatch on tim.diag/TIM_DIAG. double_gyre 4-rank gnu A/B: **all 3 history
files DATA-IDENTICAL** (snapshots + 5-day means + cont), ocean.stats +
available_diags identical, metadata identical except NumFilesInSet (accepted).
Traps recorded in findings: Fortran diag ids MUST be 1-based (MOM's id>0
guard silently dropped the first field); mediator registers dsamp/coarsened
axes unconditionally → sentinel axis id -2; run start pinned by first
registration's init_time (diag_manager_init has no time arg); bounds var =
<time_axis_name>_bounds w/ axis-derived long_name + CMOR fill; repeated text
attrs PREPEND (cell_methods order); mkmf needs `rm MOM6` before rebuild to
relink. MOM_diag_save_state/restore_state exported (Q5 driver hook pending).
**ALL DIAG GATES RUN (2026-07-08)**: rollover A/B (dg, 5-day rotation): 6
files bit-identical incl. rotated names. **Q5 THROUGH-MOM6 PASSED**: solo
driver hook (MOM6 5a13c5d05) saves RESTART/TIM.diag.res.nc beside
save_MOM_restart + restores INPUT/TIM.diag.res.nc after
finish_MOM_initialization; dg 10-day continuous vs 4+6 restart (mid-window):
ALL history files bit-identical incl. restart-spanning mean (restart legs
need RESTART_CHECKSUMS_REQUIRED=False — pre-existing checksum bug; also
input_filename='r' in input.nml). cesm_t232 128-rank intel A/B: ocean.stats
identical, TIM 92s vs FMS 111s total (Q6 diag-side favorable). Three
documented divergences from that segment (restart at day 0.25 = mid-window,
atypical): (1) window-anchor stamps differ (design; coincide on boundary
starts); (2) FMS creates fill-only files for never-written fields
(h.native/h.z), TIM doesn't create no-data files; (3) **masked-layout
staggered writes have holes** — writePartition doesn't claim shared edges
bordering ELIMINATED tiles (statics geolat_c etc. 1e20 vs FMS halo-gathered
values) → production-pass item: mask-aware edge ownership in Decomp2D.
Remaining: harvest t112 chain 6671898 (still queued); prototype deliverable
(task 6: consolidate findings, revise plan of record, rewrite-vs-harden
decision). Findings committed through 2c1288e7.

PROTOTYPE STATE as of 2026-07-07 (branches: TIM + MOM6 both on
`parallelio-prototype`):
- Findings doc: TIM docs/tim_diag_io_prototype_findings.md (untracked). PIO spike
  (TIM prototype/pio_spike/) PASSED all 12 tests (masked+zero-maplen decomp needs
  non-NULL pointers; append works; PIO_RETURN_ERROR workable).
- Build glue: turbo-stack build.sh loads parallelio/2.6.8 + threads -lpioc (working
  tree). Full `./build.sh --compiler gnu --infra TIM --override` build SUCCEEDED.
- Read spine: TIM tim/cpp/io/tim_io.{hpp,cpp}+tim_io_C_API.h (namespace TIM::IO,
  no "proto" in names per user), tim/fortran/tim_io_interface.F90; MOM6
  MOM_io_infra.F90 dispatches read_field_2d/3d to TIM when env TIM_IO_READ=1
  (helpers at end of module; uniform offset rule; FMS symmetric convention =
  extra staggered point on east/north-most rank).
- IN FLIGHT: A/B milestone test staged at /glade/derecho/scratch/altuntas/tim_proto
  (dg_control + dg_timread continue same double_gyre restart to day 20; job-ab.sh
  diffs ocean.stats + restarts; PBS job 6657665 queued — if user has an interactive
  node, qdel it and run job-ab.sh body directly with mpiexec).
- Next after milestone: dispatch read_field_0d/1d/4d + read_vector; then write path;
  then diag manager probe (Q4/Q5); then cesm_t232 perf (Q6).
- Gotchas: parallelio module needs cray-mpich loaded first; ncarcompilers exports
  CXX=icpx overriding Makefile defaults (use CC); mpiexec needs a compute node.
(`~/.claude/plans/tim-is-our-replacement-wiggly-torvalds.md` is the historical draft.)

Decisions made by the user:
- I/O backend: PIO2 (NCAR ParallelIO; `parallelio/2.6.8` module on Derecho — beware the
  compiler-only-hash builds are mpi-serial, resolve via `module load parallelio`).
- Clean new C++ API (FMS behavior is the spec, not the interface); MOM6's
  `config_src/infra/TIM/MOM_io_infra.F90` and `MOM_diag_manager_infra.F90` are the swap seams.
- Classic diag_table parsing first, YAML-extensible config model.
- Domain metadata bridged from FMS mpp_domains (TIM::Decomp2D value type); time bridged
  first, native TIM::Time calendar math when diag scheduling needs it (Phase 3a).
- Backend must be quickly swappable PIO2↔SCORPIO: all PIOc_* calls confined to a single
  backend seam (tim/cpp/io/tim_backend.*), restricted to the API subset both share.
- New diag manager MUST support history averaging across restarts (FMS can't): Accumulator
  state persisted to a diag restart file (RESTART/TIM.diag.res.nc) and restored at init;
  window boundaries from diag_table base date in absolute time. Acceptance: continuous vs
  mid-window-restarted run give bit-identical time means.

Roadmap (restructured 2026-07-06 per user's Ousterhout critique — increments must be
complete ABSTRACTIONS, not features; feature demos are validation gates inside phases):
P0 build glue (PIO flags in build.sh + pio-utils submodule fallback) → CMake/ctest for
C++ tests (branch `192-feature-cmake-build-system-for-TIM`) → 1-week PIO2 spike →
domain/time bridges; Phase 1 = COMPLETE TIM::IO File abstraction (read+write as one
unit) with two gates: read gate = first milestone "TIM reads a MOM6 restart"
(bit-identical ocean.stats), then write gate (restart cross-compat); Phase 2 = diag
abstractions (parser+calendar, Accumulator with persistence first-class from the start,
DiagManager incl. restart hook) with parity gate + restart-spanning gate; Phase 3 =
scale/GPU; Phase 4 deferred (native domains, YAML, retire FMS diag/fms2_io).
Validation: double_gyre per PR, cesm_t232 at milestones, nccmp diffs, capture/replay
for reduction math. Also corrected: the two MOM6 wrapper seams are independent on the
MOM6 side, but cutover is ordered (diag writes through TIM::IO, so I/O lands first).
Scope doc for management: /glade/u/home/altuntas/tim_diag_io_scope.md (user-edited,
15 person-weeks, work packages restructured the same abstraction-first way).

Useful references found: TIM remote branch `dev/ncar_add_pio` (~15k-line earlier Fortran
PIO-under-fms2_io effort — semantic reference only, don't merge).

CESM INTEGRATION (2026-07-08, sandbox turbo.cesm3_0_alpha09d.sbx): TIM io+diag
now BUILD-WIRED into CESM. Five edits (all in the sandbox; the case's Tools/
Makefile is a SYMLINK to cime source so editing cime updates the case):
1. libraries/FMS/buildlib — append nested tim/cpp/{core,io,diag} + tim/fortran
   to Filepath (mkSrcfiles is NON-recursive; top-level src/* glob misses them).
2. libraries/FMS/Makefile.cesm — INCLDIR += -I$(PIO_INCDIR) -I$(NETCDF_PATH)/
   include (only tim_backend.cpp needs pio.h/netcdf.h; both from Macros.make,
   which uses SYSTEM parallelio/2.6.8 — same one I prototyped against; model
   already links pio so it's compile-only).
3. components/mom/cime_config/buildlib — when infra_api=="TIM", add
   -I<fmsbuilddir>/amrex/install/include (MOM6 infra/TIM array_mod.F90 uses
   amrex_mempool_module).
4. components/mom/cime_config/config_component.xml — add TIM to
   MOM6_INFRA_API valid_values (also edit the CASE's env_build.xml valid_values,
   cached at setup, then xmlchange MOM6_INFRA_API=TIM).
5. cime/CIME/Tools/Makefile — in USE_FMS block, if MOM6_INFRA_API==TIM append
   -L<...>/FMS/amrex/install/lib -lamrex -lstdc++ AFTER -lfms (static link order).
Build succeeded (gnu, SMS_D.TL319_t232.G_JRA_RYF); cesm.exe has tim_diag_init/
tim_io_init/tim_io_read_decomposed; libfms.a has all 7 TIM cpp objects.
Also added Q5 NUOPC cap hook (mom_ocean_model_nuopc.F90): MOM_diag_restore_state
before diag_mediator_close_registration, MOM_diag_save_state in
ocean_model_restart (present(restartname) CESM path) + ocean_model_save_restart,
file RESTART/TIM.diag.res.nc. CAVEAT: CESM rpointer/st_archive doesn't stage
that file → ERS (same-rundir) restarts work, long continue runs cold-start the
window (degrades to FMS-equiv, no crash). Runtime: RUNDIR/TIM_input with
tim.io.{read,write}=1, tim.diag=1, tim.io.pio_ntasks=32 (or env TIM_IO_*/
TIM_DIAG). DEBUG builds: g++/CC tolerates the gfortran-ish CXXFLAGS
(-ffpe-trap etc.) fine (test-compiled all 10 sources). Known file diffs from
double_gyre gate still apply (NumFilesInSet, fill-only files, masked-edge holes).

CESM GOTCHA (2026-07-08): the NUOPC cap (mom_ocean_model_nuopc.F90) is compiled
for ALL infra APIs, so its unconditional `use MOM_diag_manager_infra, only:
MOM_diag_save_state, MOM_diag_restore_state` broke the FMS2/FMS1 build
("symbol not found in module"). Fix: added no-op stubs (save = do-nothing,
restore = .false.) to BOTH config_src/infra/FMS1 and FMS2
MOM_diag_manager_infra.F90 + made them public. Design principle: the infra
layer must present ONE uniform interface across backends. create_test defaults
to MOM6_INFRA_API=FMS2 (no testmod sets TIM), so a TIM create_test run needs a
testmod or post-create xmlchange MOM6_INFRA_API=TIM. Both builds verified:
TIM case 3f7gr7 (symbols in exe) and FMS2 case eparch (stubs) build clean.

FMS I/O ELIMINATION (target #2, approved 2026-07-09; steps E1-E4):
Scope facts (agent surveys): time_interp seam hard-codes ongrid=.true. (NO
horizontal interp at runtime); only per-timestep consumer in G_JRA = SSS
restoring (+optionally CHL); horiz_interp = COMPUTE on data read via
MOM_read_data (not I/O — stays FMS, like mpp/time_manager/coupler bridges);
data_override never initialized in CESM; coupler_types dormant (num_bcs=0,
set_diags never called without gas_fields_ocn); NO FMS I/O leaks in src/
(MOM_netcdf.F90 is MOM-native libnetcdf, already FMS-free, out of scope).
**E1 DONE (TIM 7f86a98b, MOM6 ddf7e4601)**: TIM::IO::ExternalField
(tim/cpp/io/tim_external_field.*) = time_interp_external2 replacement:
get_cal_time floor/truncate parsing, modulo climatology bracketing
(time_interp_list modtime=YEAR: records' years->1, set_modtime Feb29 fix,
end wraparound weights w2=(T-Te)//(Period-Td) etc.), time_divide in double,
blend r1*(1-w2)+r2*w2 where BOTH valid else var missing (_FillValue>
missing_value>missing>NC fill), full get_valid rule (fill-derived 2-ULP
range), 2-slot record cache (evict-partner-protected), decomposed via
File::readDecomposed / replicated via Serial::readSlab (readPlain is 0d/1d
only!). Serial gained attDouble+timeName. C API tim_extfield_* +
tim_io_interface additions; g_extfields cleared in tim_io_finalize before
ctx. MOM_interp_infra dispatches on tim.io.interp/TIM_IO_INTERP (default 1);
FMS centering isw=(nx-nxw)/2+1 replicated; horz_interp-present under TIM =
FATAL; MOM_io_infra gained tim_get_domain2d_handle (raw mpp domain, sym=0 —
external fields are Center so sym moot). extfield_test (diag_probe Makefile,
gnu modules) ALL PASS. **E1 GATE PASSED: G_JRA tx3deg 5-day, RESTORE_SALINITY
=True: ocean.stats BIT-IDENTICAL FMS-interp vs TIM-interp.**
**E2 DONE (MOM6 2eb56b802)**: MOM_data_override_infra = fail-loud stubs, no
data_override_mod use; builds clean.
**E3 REMAINING**: excise FMS branches/uses from infra wrappers so no
Fortran `use` of fms2_io/diag_manager/diag_axis/time_interp_external2/
netcdf_io remains: MOM_diag_manager_infra (easy — TIM complete; need local
EAST/NORTH/null_axis_id/DIAG_FIELD_NOT_FOUND constants replacing
diag_axis/diag_data imports; check FMS values: EAST/NORTH from mpp?);
MOM_interp_infra (drop time_interp_external2+netcdf_io uses; keep
horiz_interp_mod = compute); MOM_io_infra = THE BIG ONE: file_type wraps
FmsNetcdfDomainFile_t/FmsNetcdfFile_t, axistype/fieldtype wrap FMS types,
open_ASCII_file/mpp ASCII paths — needs native handle types + plain-Fortran
ASCII. **E4 REMAINING**: drop fms2_io/diag_manager/time_interp/data_override
sources from libfms buildlib Filepath; expect intra-FMS blockers:
coupler_types_mod uses diag_manager+fms2_io (we own the trimmed fork — stub
its set_diags/register_restarts), horiz_interp_mod + mosaic2 internal deps —
link step reveals; then full SMS suite. All work in cesm sandbox clones
(parallelio-prototype branches, shared repo with turbo-stack via
turbo-local); turbo-stack checkouts NOT yet synced with E1/E2 commits.

E3 PART 1 DONE (MOM6 36cd2609b, 2026-07-09): MOM_interp_infra +
MOM_diag_manager_infra contain ZERO FMS I/O uses; TIM unconditional (gates
tim.io.interp/tim.diag retired). diag EAST/NORTH = MOM_domain_infra
EAST_FACE/NORTH_FACE re-export (same mpp constants); null_axis_id=0,
DIAG_FIELD_NOT_FOUND=-1 native params. Gate-stripping done mechanically
(python: keep tim_diag_on() gate body, drop FMS tail to procedure end,
then-nesting counter excluding elseif). Validation: full SMS 9/9 PASS +
ocean.stats bit-identical pre/post.
**E3 PART 2 REMAINING — MOM_io_infra** (the big one). Inventory done: 39
procedures reference FMS. Categories: (a) gated TIM-handles-all → mechanical
tail-drop like diag: read_field_0d/1d/2d/3d/4d/_int, read_vector_2d/3d,
read_field_2d/3d_region, field_exists, get_field_size, get_file_info/times/
fields, write_field_0d..4d, write_metadata_axis/field/global, MOM_write_axis,
MOM_file_exists, get_filename_suffix, open_file (CAREFUL: two mid-function
gates, FMS interleaved — not top-gated); (b) FMS-only HELPERS that become
dead after tail-drop → DELETE: find_varname_in_file, prepare_to_read_var,
categorize_axes, MOM_register_variable_axes, find_unlimited_dimension_name,
write_time_if_later; (c) needs native replacement: FMS_file_exists (→
INQUIRE), close_file_type/flush_file (drop fileobj branches; TIM handles),
check_namelist_error (fms check_nml_error → native iostat logic),
write_version (write_version_number → root-PE write), open_ASCII_file
(already FMS-free); (d) TYPE SURGERY: file_type drops
FmsNetcdfDomainFile_t/FmsNetcdfFile_t ptr members; axistype/fieldtype wrap
FMS types — make native (name + data). Approach: chunked reads + per-region
rewrites, compiler as checker; then E4 (drop fms2_io/diag_manager/
time_interp/data_override from FMS buildlib Filepath; stub coupler_types_mod
internal diag/fms2_io uses in our fork; expect mosaic2/horiz_interp intra-dep
surprises at link). Validation recipe: case.build + SMS + ocean.stats
bit-compare (preE3 copy at scratchpad/ocean.stats.preE3; but scratchpad is
session-specific — regenerate control from current build if needed).

E3 PART 2a DONE (MOM6 36cd2609b + this: fms_mod/fms_io_utils excised from
MOM_io_infra; builds clean; commit "E3 (part 2a)"): FMS_file_exists=INQUIRE,
check_namelist_error native (IOstat>0 fatal, <0 tolerated=absent optional
group), write_version native root-PE banner, filename appendix = module var
tim_filename_suffix + set_filename_suffix (MOM_ensemble_manager_infra setter
re-pointed off fms2_io). REMAINING in MOM_io_infra: 11 `use fms2_io_mod`
lines + gated FMS tails + dead helpers + type surgery (file_type FmsNetcdf*
members, axistype/fieldtype natives) — see E3 PART 2 inventory above. SMS
validation still pending for 2a (compile-proven only; run it at next stage).
t112 chain 6671898 RESULT: TIM read of 140GB tx1_12 CESM restart at 2048
ranks SUCCEEDED (step-0 ocean.stats sane; FMS cannot read this file), but
model SEGFAULTED in first dynamics step (step_mom_dynamics MOM.F90:1386,
abort 11, no FATAL) -> bootstrap restart never written, perf A/B legs never
ran. Standalone-case dynamics bug, not I/O. Cheap Q6 option: zero-length
segment (read restart -> write restart, no stepping) gives the 140GB
read+write A/B without dynamics.
