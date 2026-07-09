!> PROTOTYPE bind(C) interface for the throwaway TIM I/O spine (read path).
!! Pattern: tim/fortran/tim_coms_infra_interface.F90.
module tim_io_interface

use, intrinsic :: iso_c_binding, only : c_int, c_double, c_char, c_null_char, &
                                        c_long_long, c_signed_char

implicit none ; private

public :: tim_io_init, tim_io_cfg_bool
public :: tim_io_register_domain, tim_io_read_decomposed
public :: tim_io_read_plain, tim_io_var_exists, tim_io_finalize
public :: tim_io_file_exists, tim_io_file_info, tim_io_file_times
public :: tim_io_file_var_name, tim_io_var_att, tim_io_var_sizes
public :: tim_io_read_slab
public :: tim_io_createfile, tim_io_def_axis, tim_io_def_var
public :: tim_io_put_global_att, tim_io_write_axis, tim_io_var_stagger
public :: tim_io_write_decomposed, tim_io_write_plain, tim_io_closefile
public :: tim_io_file_num_times, tim_io_file_time
public :: tim_extfield_init, tim_extfield_size, tim_extfield_missing
public :: tim_extfield_npts, tim_extfield_window, tim_extfield_interp
public :: cstr

interface
  subroutine tim_io_init(fcomm) bind(C, name="tim_io_init")
    import :: c_int
    integer(c_int), value :: fcomm
  end subroutine

  integer(c_int) function tim_io_cfg_bool(key, env, def) &
      bind(C, name="tim_io_cfg_bool")
    import :: c_int, c_char
    character(kind=c_char), intent(in) :: key(*), env(*)
    integer(c_int), value :: def
  end function

  integer(c_int) function tim_io_register_domain(nig, njg, isc, iec, jsc, jec, &
      symmetric) bind(C, name="tim_io_register_domain")
    import :: c_int
    integer(c_int), value :: nig, njg, isc, iec, jsc, jec, symmetric
  end function

  integer(c_int) function tim_io_read_decomposed(path, varname, domain_handle, &
      stagger, timelevel, nz, nz2, buf, file_sx, file_sy) &
      bind(C, name="tim_io_read_decomposed")
    import :: c_int, c_char, c_double
    character(kind=c_char), intent(in) :: path(*), varname(*)
    integer(c_int), value :: domain_handle, stagger, timelevel, nz, nz2
    real(c_double), intent(inout) :: buf(*)
    integer(c_int), intent(out) :: file_sx, file_sy
  end function

  integer(c_int) function tim_io_read_plain(path, varname, timelevel, n, buf) &
      bind(C, name="tim_io_read_plain")
    import :: c_int, c_char, c_double
    character(kind=c_char), intent(in) :: path(*), varname(*)
    integer(c_int), value :: timelevel, n
    real(c_double), intent(inout) :: buf(*)
  end function

  integer(c_int) function tim_io_var_exists(path, varname) &
      bind(C, name="tim_io_var_exists")
    import :: c_int, c_char
    character(kind=c_char), intent(in) :: path(*), varname(*)
  end function

  subroutine tim_io_finalize() bind(C, name="tim_io_finalize")
  end subroutine

  integer(c_int) function tim_io_file_exists(path) bind(C, name="tim_io_file_exists")
    import :: c_int, c_char
    character(kind=c_char), intent(in) :: path(*)
  end function

  integer(c_int) function tim_io_file_info(path, ndims, nvars, ntimes) &
      bind(C, name="tim_io_file_info")
    import :: c_int, c_char
    character(kind=c_char), intent(in) :: path(*)
    integer(c_int), intent(out) :: ndims, nvars, ntimes
  end function

  integer(c_int) function tim_io_file_times(path, buf, n) &
      bind(C, name="tim_io_file_times")
    import :: c_int, c_char, c_double
    character(kind=c_char), intent(in) :: path(*)
    real(c_double), intent(inout) :: buf(*)
    integer(c_int), value :: n
  end function

  integer(c_int) function tim_io_file_var_name(path, index1, out, maxlen) &
      bind(C, name="tim_io_file_var_name")
    import :: c_int, c_char
    character(kind=c_char), intent(in) :: path(*)
    integer(c_int), value :: index1, maxlen
    character(kind=c_char), intent(inout) :: out(*)
  end function

  integer(c_int) function tim_io_var_att(path, varname, att, out, maxlen) &
      bind(C, name="tim_io_var_att")
    import :: c_int, c_char
    character(kind=c_char), intent(in) :: path(*), varname(*), att(*)
    integer(c_int), value :: maxlen
    character(kind=c_char), intent(inout) :: out(*)
  end function

  integer(c_int) function tim_io_var_sizes(path, varname, sizes) &
      bind(C, name="tim_io_var_sizes")
    import :: c_int, c_char
    character(kind=c_char), intent(in) :: path(*), varname(*)
    integer(c_int), intent(inout) :: sizes(4)
  end function

  integer(c_int) function tim_io_read_slab(path, varname, start, nread, buf) &
      bind(C, name="tim_io_read_slab")
    import :: c_int, c_char, c_double
    character(kind=c_char), intent(in) :: path(*), varname(*)
    integer(c_int), intent(in) :: start(4), nread(4)
    real(c_double), intent(inout) :: buf(*)
  end function

  ! ---- write path (stateful file handles) ----
  integer(c_int) function tim_io_createfile(path, domain_handle, mode) &
      bind(C, name="tim_io_createfile")
    import :: c_int, c_char
    character(kind=c_char), intent(in) :: path(*)
    integer(c_int), value :: domain_handle, mode
  end function

  integer(c_int) function tim_io_def_axis(fh, name, kind_, position, n, units, &
      longname, cartesian, sense, has_sense) bind(C, name="tim_io_def_axis")
    import :: c_int, c_char
    integer(c_int), value :: fh, kind_, position, n, sense, has_sense
    character(kind=c_char), intent(in) :: name(*), units(*), longname(*), cartesian(*)
  end function

  integer(c_int) function tim_io_def_var(fh, name, dims_joined, units, longname, &
      std_name, pack_, checksum) bind(C, name="tim_io_def_var")
    import :: c_int, c_char
    integer(c_int), value :: fh, pack_
    character(kind=c_char), intent(in) :: name(*), dims_joined(*), units(*)
    character(kind=c_char), intent(in) :: longname(*), std_name(*), checksum(*)
  end function

  integer(c_int) function tim_io_put_global_att(fh, name, value) &
      bind(C, name="tim_io_put_global_att")
    import :: c_int, c_char
    integer(c_int), value :: fh
    character(kind=c_char), intent(in) :: name(*), value(*)
  end function

  integer(c_int) function tim_io_write_axis(fh, name, data_, n) &
      bind(C, name="tim_io_write_axis")
    import :: c_int, c_char, c_double
    integer(c_int), value :: fh, n
    character(kind=c_char), intent(in) :: name(*)
    real(c_double), intent(in) :: data_(*)
  end function

  integer(c_int) function tim_io_var_stagger(fh, name) &
      bind(C, name="tim_io_var_stagger")
    import :: c_int, c_char
    integer(c_int), value :: fh
    character(kind=c_char), intent(in) :: name(*)
  end function

  integer(c_int) function tim_io_write_decomposed(fh, name, buf, tstamp, has_tstamp) &
      bind(C, name="tim_io_write_decomposed")
    import :: c_int, c_char, c_double
    integer(c_int), value :: fh, has_tstamp
    character(kind=c_char), intent(in) :: name(*)
    real(c_double), intent(in) :: buf(*)
    real(c_double), value :: tstamp
  end function

  integer(c_int) function tim_io_write_plain(fh, name, data_, n, tstamp, has_tstamp) &
      bind(C, name="tim_io_write_plain")
    import :: c_int, c_char, c_double
    integer(c_int), value :: fh, n, has_tstamp
    character(kind=c_char), intent(in) :: name(*)
    real(c_double), intent(in) :: data_(*)
    real(c_double), value :: tstamp
  end function

  integer(c_int) function tim_io_closefile(fh) bind(C, name="tim_io_closefile")
    import :: c_int
    integer(c_int), value :: fh
  end function

  integer(c_int) function tim_io_file_num_times(fh) bind(C, name="tim_io_file_num_times")
    import :: c_int
    integer(c_int), value :: fh
  end function

  real(c_double) function tim_io_file_time(fh) bind(C, name="tim_io_file_time")
    import :: c_int, c_double
    integer(c_int), value :: fh
  end function
  integer(c_int) function tim_extfield_init(path, field, domain_handle, &
      fms_calendar, actual_name, name_len) bind(C, name="tim_extfield_init")
    import :: c_int, c_char
    character(kind=c_char), intent(in) :: path(*), field(*)
    integer(c_int), value :: domain_handle, fms_calendar, name_len
    character(kind=c_char), intent(out) :: actual_name(*)
  end function

  subroutine tim_extfield_size(handle, siz) bind(C, name="tim_extfield_size")
    import :: c_int
    integer(c_int), value :: handle
    integer(c_int), intent(out) :: siz(4)
  end subroutine

  real(c_double) function tim_extfield_missing(handle) &
      bind(C, name="tim_extfield_missing")
    import :: c_int, c_double
    integer(c_int), value :: handle
  end function

  integer(c_long_long) function tim_extfield_npts(handle) &
      bind(C, name="tim_extfield_npts")
    import :: c_int, c_long_long
    integer(c_int), value :: handle
  end function

  subroutine tim_extfield_window(handle, ni, nj, nz) &
      bind(C, name="tim_extfield_window")
    import :: c_int
    integer(c_int), value :: handle
    integer(c_int), intent(out) :: ni, nj, nz
  end subroutine

  integer(c_int) function tim_extfield_interp(handle, days, secs, buf, mask, &
      want_mask) bind(C, name="tim_extfield_interp")
    import :: c_int, c_double, c_signed_char
    integer(c_int), value :: handle, days, secs, want_mask
    real(c_double), intent(inout) :: buf(*)
    integer(c_signed_char), intent(inout) :: mask(*)
  end function

end interface

contains

!> Append the C string terminator to a Fortran string.
pure function cstr(s)
  character(len=*), intent(in) :: s
  character(len=len_trim(s)+1) :: cstr
  cstr = trim(s)//c_null_char
end function cstr

end module tim_io_interface
