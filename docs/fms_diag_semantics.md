# Classic FMS diag_manager — behavioral spec for bit-identical reimplementation

Extracted 2026-07-07 from this repo's diag_manager/ sources (all citations are
file:line into diag_manager/). Classic path = USE_REFACTORED_SEND=.false.
(MOM6 default). This is the design contract for TIM's Accumulator/DiagManager.

## Executive summary of reproduce-exactly rules

1. ACCUMULATION (average=.true., the MOM6 norm): per send_data call,
   buffer(pt) += field(pt)*weight for unmasked points; the normalization
   counter count_0d is a SCALAR per (output field, diurnal sample):
   count_0d += weight ONCE PER CALL if ANY point is unmasked — NOT per point.
   Division by count_0d happens at output time (writing_field), not during
   accumulation. rms: accumulate (x*w)**2, divide by sum(w), then sqrt —
   the weight is raised to the power too.
2. MASKED POINTS OVERWRITE: a masked point's buffer entry is set to
   missing_value at that call — destroying any prior accumulation for that
   cell. Only mask_variant mode keeps a per-point counter and per-cell
   division. This asymmetry is intentional-looking and must be reproduced.
3. TRIGGER: at the TOP of send_data, strictly `time > next_output` closes the
   window; the triggering call's own sample lands in the NEXT window. A
   sample at exactly next_output accumulates into the current window.
4. TIME COORDINATE: averaged fields' record time = (last_output+next_output)/2
   (midpoint); snapshots' = next_output. average_T1/T2 = window start/end as
   "units since base_date" reals; average_DT = T2-T1; time_bnds = [T1,T2].
5. next_output advance: next_output = next_next_output;
   next_next_output = diag_time_inc(next_next_output, freq, units), with
   months/years deferring to calendar increment_date (variable month lengths).
6. END OF RUN: closing_file writes partial windows with `time >= next_output`
   (note >=, unlike send_data's >), at_diag_end skips advance/reset;
   freq=-1 (END_OF_RUN) fields accumulate the whole run into one record;
   never-written fields get a FILL_VALUE (9.96921e36) record.
7. FILE ROLLOVER: start_time/close_time/next_open trio; data with
   close_time < time < next_open is DROPPED; new file at time > next_open
   resets time_index; filename = base truncated at first %<digit> + '.' +
   '-'-joined %Nyr/%Nmo/%Ndy/%Nhr/%Nmi/%Nsc tokens stamped with
   filename_time (begin/middle/end per filename_time_bounds; default record
   time); prepend_date (YYYYMMDD.) only when diag_manager_init got time_init.
8. STATICS: written once per file at close/rollover, no time record.
9. MISSING VALUE attrs: _FillValue AND missing_value both written; the
   registered missing value, else CMOR 1.0e20.
10. PRECISION: FMS accumulates in default REAL — with -fdefault-real-8 (our
    builds) that is double; a C++ double accumulator matches bit-for-bit.
11. count_0d/buffers per diurnal sample (diurnalNN); buffer init: 0.0 for
    mean/sum/point, -HUGE for max, +HUGE for min.
12. time_sampling column is parsed and IGNORED. packing>2 FATALs in fms2_io.
    Same input field fans out to independent buffers per (field,file) pair.

Full agent-extracted spec with quoted Fortran and line numbers follows.

(full spec appended from agent report — see git history of this commit)
