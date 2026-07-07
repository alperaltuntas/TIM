!> PROTOTYPE bind(C) interface for the throwaway TIM I/O spine (read path).
!! Pattern: tim/fortran/tim_coms_infra_interface.F90.
module tim_io_interface

use, intrinsic :: iso_c_binding, only : c_int, c_double, c_char, c_null_char

implicit none ; private

public :: tim_io_register_domain, tim_io_read_decomposed
public :: tim_io_read_plain, tim_io_var_exists, tim_io_finalize
public :: cstr

interface
  integer(c_int) function tim_io_register_domain(nig, njg, isc, iec, jsc, jec, &
      symmetric) bind(C, name="tim_io_register_domain")
    import :: c_int
    integer(c_int), value :: nig, njg, isc, iec, jsc, jec, symmetric
  end function

  integer(c_int) function tim_io_read_decomposed(path, varname, domain_handle, &
      stagger, timelevel, nz, buf) bind(C, name="tim_io_read_decomposed")
    import :: c_int, c_char, c_double
    character(kind=c_char), intent(in) :: path(*), varname(*)
    integer(c_int), value :: domain_handle, stagger, timelevel, nz
    real(c_double), intent(inout) :: buf(*)
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
end interface

contains

!> Append the C string terminator to a Fortran string.
pure function cstr(s)
  character(len=*), intent(in) :: s
  character(len=len_trim(s)+1) :: cstr
  cstr = trim(s)//c_null_char
end function cstr

end module tim_io_interface
