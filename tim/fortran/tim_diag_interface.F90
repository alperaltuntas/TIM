!> PROTOTYPE bind(C) interface for the TIM diagnostics manager
!! (tim/cpp/diag/tim_diag_C_API.h). Pattern: tim_io_interface.F90.
module tim_diag_interface

use, intrinsic :: iso_c_binding, only : c_int, c_double, c_char, c_long_long, &
                                        c_null_char, c_ptr, c_null_ptr, c_loc

implicit none ; private

public :: tim_diag_init, tim_diag_active, tim_diag_axis_init
public :: tim_diag_axis_name, tim_diag_register_field, tim_diag_field_id
public :: tim_diag_attr_text, tim_diag_attr_ints, tim_diag_attr_reals
public :: tim_diag_post, tim_diag_send_complete, tim_diag_set_time_end
public :: tim_diag_end, tim_diag_save_state, tim_diag_restore_state
public :: cstr

interface
  integer(c_int) function tim_diag_init(fms_calendar, y, mo, d, h, mi, s) &
      bind(C, name="tim_diag_init")
    import :: c_int
    integer(c_int), value :: fms_calendar, y, mo, d, h, mi, s
  end function

  integer(c_int) function tim_diag_active() bind(C, name="tim_diag_active")
    import :: c_int
  end function

  integer(c_int) function tim_diag_axis_init(name, data, n, units, cart, &
      long_name, domain_handle, staggered, direction, edges_id, set_name) &
      bind(C, name="tim_diag_axis_init")
    import :: c_int, c_char, c_double
    character(kind=c_char), intent(in) :: name(*), units(*), cart(*)
    character(kind=c_char), intent(in) :: long_name(*), set_name(*)
    real(c_double), intent(in) :: data(*)
    integer(c_int), value :: n, domain_handle, staggered, direction, edges_id
  end function

  subroutine tim_diag_axis_name(id, buf, buflen) &
      bind(C, name="tim_diag_axis_name")
    import :: c_int, c_char
    integer(c_int), value :: id, buflen
    character(kind=c_char), intent(out) :: buf(*)
  end subroutine

  integer(c_int) function tim_diag_register_field(module_name, field, axes, &
      naxes, init_days, init_secs, long_name, units, standard_name, &
      interp_method, has_missing, missing_value, has_range, range_lo, &
      range_hi, mask_variant, is_static, area_id, volume_id) &
      bind(C, name="tim_diag_register_field")
    import :: c_int, c_char, c_double
    character(kind=c_char), intent(in) :: module_name(*), field(*)
    character(kind=c_char), intent(in) :: long_name(*), units(*)
    character(kind=c_char), intent(in) :: standard_name(*), interp_method(*)
    integer(c_int), intent(in) :: axes(*)
    integer(c_int), value :: naxes, init_days, init_secs
    integer(c_int), value :: has_missing, has_range, mask_variant, is_static
    integer(c_int), value :: area_id, volume_id
    real(c_double), value :: missing_value, range_lo, range_hi
  end function

  integer(c_int) function tim_diag_field_id(module_name, field) &
      bind(C, name="tim_diag_field_id")
    import :: c_int, c_char
    character(kind=c_char), intent(in) :: module_name(*), field(*)
  end function

  subroutine tim_diag_attr_text(field_id, name, value) &
      bind(C, name="tim_diag_attr_text")
    import :: c_int, c_char
    integer(c_int), value :: field_id
    character(kind=c_char), intent(in) :: name(*), value(*)
  end subroutine

  subroutine tim_diag_attr_ints(field_id, name, v, n) &
      bind(C, name="tim_diag_attr_ints")
    import :: c_int, c_char
    integer(c_int), value :: field_id, n
    character(kind=c_char), intent(in) :: name(*)
    integer(c_int), intent(in) :: v(*)
  end subroutine

  subroutine tim_diag_attr_reals(field_id, name, v, n) &
      bind(C, name="tim_diag_attr_reals")
    import :: c_int, c_char, c_double
    integer(c_int), value :: field_id, n
    character(kind=c_char), intent(in) :: name(*)
    real(c_double), intent(in) :: v(*)
  end subroutine

  !> rmask may be a null pointer (c_null_ptr) when absent.
  integer(c_int) function tim_diag_post(field_id, days, secs, data, npts, &
      rmask, weight) bind(C, name="tim_diag_post")
    import :: c_int, c_double, c_long_long, c_ptr
    integer(c_int), value :: field_id, days, secs
    real(c_double), intent(in) :: data(*)
    integer(c_long_long), value :: npts
    type(c_ptr), value :: rmask
    real(c_double), value :: weight
  end function

  subroutine tim_diag_send_complete() bind(C, name="tim_diag_send_complete")
  end subroutine

  subroutine tim_diag_set_time_end(days, secs) &
      bind(C, name="tim_diag_set_time_end")
    import :: c_int
    integer(c_int), value :: days, secs
  end subroutine

  integer(c_int) function tim_diag_end(days, secs) &
      bind(C, name="tim_diag_end")
    import :: c_int
    integer(c_int), value :: days, secs
  end function

  integer(c_int) function tim_diag_save_state(path) &
      bind(C, name="tim_diag_save_state")
    import :: c_int, c_char
    character(kind=c_char), intent(in) :: path(*)
  end function

  integer(c_int) function tim_diag_restore_state(path) &
      bind(C, name="tim_diag_restore_state")
    import :: c_int, c_char
    character(kind=c_char), intent(in) :: path(*)
  end function
end interface

contains

!> Append the C string terminator to a Fortran string.
pure function cstr(s)
  character(len=*), intent(in) :: s
  character(len=len_trim(s)+1) :: cstr
  cstr = trim(s)//c_null_char
end function cstr

end module tim_diag_interface
