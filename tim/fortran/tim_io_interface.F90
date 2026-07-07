!> PROTOTYPE bind(C) interface for the throwaway TIM I/O spine (read path).
!! Pattern: tim/fortran/tim_coms_infra_interface.F90.
module tim_io_interface

use, intrinsic :: iso_c_binding, only : c_int, c_double, c_char, c_null_char

implicit none ; private

public :: tim_io_register_domain, tim_io_read_decomposed
public :: tim_io_read_plain, tim_io_var_exists, tim_io_finalize
public :: tim_io_createfile, tim_io_def_axis, tim_io_def_var
public :: tim_io_put_global_att, tim_io_write_axis, tim_io_var_stagger
public :: tim_io_write_decomposed, tim_io_write_plain, tim_io_closefile
public :: tim_io_file_num_times, tim_io_file_time
public :: cstr

interface
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
end interface

contains

!> Append the C string terminator to a Fortran string.
pure function cstr(s)
  character(len=*), intent(in) :: s
  character(len=len_trim(s)+1) :: cstr
  cstr = trim(s)//c_null_char
end function cstr

end module tim_io_interface
