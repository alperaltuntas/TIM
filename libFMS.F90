!***********************************************************************
!*                   GNU Lesser General Public License
!*
!* This file is part of the GFDL Flexible Modeling System (FMS).
!*
!* FMS is free software: you can redistribute it and/or modify it under
!* the terms of the GNU Lesser General Public License as published by
!* the Free Software Foundation, either version 3 of the License, or (at
!* your option) any later version.
!*
!* FMS is distributed in the hope that it will be useful, but WITHOUT
!* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
!* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
!* for more details.
!*
!* You should have received a copy of the GNU Lesser General Public
!* License along with FMS.  If not, see <http://www.gnu.org/licenses/>.
!***********************************************************************
!> @defgroup FMS FMS
!> @ingroup libfms
!> @brief A convenience module to use any FMS routines, functions, values
!> @author Ryan Mulhall
!!
!! @date 2/2021
!!
!! Imports all supported FMS modules so that any public interfaces,
!! variables or routines can be used via this module. Excludes mpp_io modules
!! and routines. Overloaded type operators/assignments cannot be imported individually
!! (ie. `use fms, only: OPERATOR(*)` includes any defined '*' operators within FMS).
!!
!! Remappings due to conflicts:
!!
!!           get_mosaic_tile_grid from mosaic2(fms2_io) => mosaic2_get_mosaic_tile_grid
!!
!!           read_data from interpolator_mod(fms2_io)   => interpolator_read_data
!!
!!           ZERO from interpolator_mod(mpp_parameter)  => INTERPOLATOR_ZERO
!!
!!           version from fms_mod                       => version_FMS
!!
!! Not in this module:
!!
!!                     axis_utils_mod, fms_io_mod, time_interp_external_mod
!!                     get_grid_version_mpp_mod, mpp_io_mod,
!!                     fms_mod(partial, old io excluded), drifters modules
!!                     constants_mod (FMSconstants should be used externally)
!!
!! A full list of supported interfaces and public types intended for use via
!! this module is provided in the [supported_interfaces.md](../../supported_interfaces.md)
!! file.

!> @file
!> @brief File for @ref FMS

!> @addtogroup FMS
!> @{
module fms

  !> import each FMS module's public routines/functions, interfaces, and variables
  !! done explicitly to avoid including any unwanted/depracated routines/modules

  !> affinity
  use fms_affinity_mod, only: fms_affinity_init, fms_affinity_get, &
                              fms_affinity_set

  !> astronomy
  use astronomy_mod, only: astronomy_init, get_period, set_period, &
                           set_orbital_parameters, get_orbital_parameters, &
                           set_ref_date_of_ae, get_ref_date_of_ae,  &
                           diurnal_solar, daily_mean_solar, annual_mean_solar,  &
                           astronomy_end, universal_time, orbital_time

  !> axis_utils

  !>block_control
  use block_control_mod, only: block_control_type, define_blocks, &
                               define_blocks_packed

  !> coupler
  use coupler_types_mod, only: coupler_types_init, coupler_type_copy, &
                               coupler_type_spawn, coupler_type_set_diags, &
                               coupler_type_write_chksums, coupler_type_send_data, &
                               coupler_type_data_override, &
                               coupler_type_increment_data, &
                               coupler_type_rescale_data, coupler_type_copy_data, &
                               coupler_type_redistribute_data, coupler_type_destructor, &
                               coupler_type_initialized, coupler_type_extract_data, &
                               coupler_type_set_data,coupler_type_copy_1d_2d, &
                               coupler_type_copy_1d_3d, coupler_3d_values_type, &
                               coupler_3d_field_type, coupler_3d_bc_type, &
                               coupler_2d_values_type, coupler_2d_field_type, &
                               coupler_2d_bc_type, coupler_1d_values_type, &
                               coupler_1d_field_type, coupler_1d_bc_type, &
                               ind_pcair, ind_u10, ind_psurf, ind_alpha, ind_csurf, &
                               ind_sc_no, ind_flux, ind_deltap, ind_kw, ind_flux0, &
                               ind_deposition, ind_runoff
  use ensemble_manager_mod, only: ensemble_manager_init, get_ensemble_id, get_ensemble_size, &
                               get_ensemble_pelist, ensemble_pelist_setup, &
                               get_ensemble_filter_pelist
  use atmos_ocean_fluxes_mod, only: atmos_ocean_fluxes_init, atmos_ocean_type_fluxes_init, &
                               aof_set_coupler_flux

  !> data_override

  !> diag_manager
  !! includes imports from submodules made public

  !> field manager
  use field_manager_mod, only: field_manager_init, field_manager_end, find_field_index, &
                         parse, fm_change_list, &
                         fm_dump_list, fm_exists, fm_get_index, &
                         fm_get_current_list, fm_get_length, fm_get_type, fm_get_value, &
                         fm_loop_over_list, fm_new_list, fm_new_value, &
                         fm_modify_name, fm_query_method, fm_find_methods, fm_copy_list, &
                         fm_field_name_len, fm_path_name_len, &
                         fm_string_len, fm_type_name_len, NUM_MODELS, NO_FIELD, &
                         MODEL_ATMOS, MODEL_OCEAN, MODEL_LAND, MODEL_ICE, MODEL_COUPLER, &
                         method_type, method_type_short, &
                         method_type_very_short, fm_list_iter_type, default_method
  use fm_util_mod, only: fm_util_start_namelist, fm_util_end_namelist, &
                         fm_util_check_for_bad_fields, fm_util_set_caller, &
                         fm_util_reset_caller, fm_util_set_no_overwrite, &
                         fm_util_reset_no_overwrite, fm_util_set_good_name_list, &
                         fm_util_reset_good_name_list, fm_util_get_length, &
                         fm_util_get_integer, fm_util_get_logical, fm_util_get_real, &
                         fm_util_get_string, fm_util_get_integer_array, &
                         fm_util_get_logical_array, fm_util_get_real_array, &
                         fm_util_get_string_array, fm_util_set_value, &
                         fm_util_set_value_integer_array, fm_util_set_value_logical_array, &
                         fm_util_set_value_real_array, fm_util_set_value_string_array, &
                         fm_util_set_value_integer, fm_util_set_value_logical, &
                         fm_util_set_value_real, fm_util_set_value_string, &
                         fm_util_get_index_list, fm_util_get_index_string, &
                         fm_util_default_caller

  !> fms2_io
  ! used via fms2_io
  ! fms_io_utils_mod, fms_netcdf_domain_io_mod, netcdf_io_mod,

  !> fms
  !! routines that don't conflict with fms2_io
  use fms_mod, only: fms_init, fms_end, error_mesg, fms_error_handler, check_nml_error, &
                     monotonic_array, string_array_index, clock_flag_default, &
                     print_memory_usage, write_version_number

  !> horiz_interp
  use horiz_interp_mod, only: horiz_interp, horiz_interp_new, horiz_interp_del, &
                              horiz_interp_init, horiz_interp_end
  use horiz_interp_type_mod, only: horiz_interp_type, assignment(=), CONSERVE, &
                              BILINEAR, SPHERICA, BICUBIC, stats
  !! used via horiz_interp
  ! horiz_interp_bicubic_mod, horiz_interp_bilinear_mod
  ! horiz_interp_conserve_mod, horiz_interp_spherical_mod

  !> memutils
  use memutils_mod, only: memutils_init, print_memuse_stats

  !> mosaic

  !> mpp
  use mpp_mod, only: stdin, stdout, stderr, &
                     stdlog, lowercase, uppercase, mpp_error, mpp_error_state, &
                     mpp_set_warn_level, mpp_sync, mpp_sync_self, mpp_set_stack_size, &
                     mpp_pe, mpp_npes, mpp_root_pe, mpp_set_root_pe, mpp_declare_pelist, &
                     mpp_get_current_pelist, mpp_set_current_pelist, &
                     mpp_get_current_pelist_name, mpp_clock_id, mpp_clock_set_grain, &
                     mpp_record_timing_data, get_unit, read_ascii_file, read_input_nml, &
                     mpp_clock_begin, mpp_clock_end, get_ascii_file_num_lines, &
                     mpp_record_time_start, mpp_record_time_end, mpp_chksum, &
                     mpp_max, mpp_min, mpp_sum, mpp_transmit, mpp_send, mpp_recv, &
                     mpp_sum_ad, mpp_broadcast, mpp_init, mpp_exit, mpp_gather, &
                     mpp_scatter, mpp_alltoall, mpp_type, mpp_byte, mpp_type_create, &
                     mpp_type_free, input_nml_file
  use mpp_parameter_mod,only:MAXPES, MPP_VERBOSE, MPP_DEBUG, ALL_PES, ANY_PE, NULL_PE, &
                     NOTE, WARNING, FATAL, MPP_WAIT, MPP_READY, MAX_CLOCKS, &
                     MAX_EVENT_TYPES, MAX_EVENTS, MPP_CLOCK_SYNC, MPP_CLOCK_DETAILED, &
                     CLOCK_COMPONENT, CLOCK_SUBCOMPONENT, CLOCK_MODULE_DRIVER, &
                     CLOCK_MODULE, CLOCK_ROUTINE, CLOCK_LOOP, CLOCK_INFRA, MAX_BINS, &
                     EVENT_ALLREDUCE, EVENT_BROADCAST, EVENT_RECV, EVENT_SEND, EVENT_WAIT, &
                     EVENT_ALLTOALL, EVENT_TYPE_CREATE, EVENT_TYPE_FREE, &
                     DEFAULT_TAG, COMM_TAG_1, COMM_TAG_2, COMM_TAG_3, COMM_TAG_4, &
                     COMM_TAG_5,  COMM_TAG_6,  COMM_TAG_7,  COMM_TAG_8, &
                     COMM_TAG_9,  COMM_TAG_10, COMM_TAG_11, COMM_TAG_12, &
                     COMM_TAG_13, COMM_TAG_14, COMM_TAG_15, COMM_TAG_16, &
                     COMM_TAG_17, COMM_TAG_18, COMM_TAG_19, COMM_TAG_20, &
                     MPP_FILL_INT, MPP_FILL_FLOAT, MPP_FILL_DOUBLE, &
                     GLOBAL_DATA_DOMAIN, CYCLIC_GLOBAL_DOMAIN, BGRID_NE, &
                     BGRID_SW, CGRID_NE, CGRID_SW, DGRID_NE, DGRID_SW, &
                     FOLD_WEST_EDGE, FOLD_EAST_EDGE, FOLD_SOUTH_EDGE, &
                     FOLD_NORTH_EDGE, WUPDATE, EUPDATE, SUPDATE, NUPDATE, &
                     XUPDATE, YUPDATE, BITWISE_EXACT_SUM, NON_BITWISE_EXACT_SUM, &
                     MPP_DOMAIN_TIME, WEST, EAST, SOUTH, NORTH, SCALAR_BIT, SCALAR_PAIR, &
                     BITWISE_EFP_SUM, NORTH_EAST, SOUTH_EAST, SOUTH_WEST, NORTH_WEST, &
                     AGRID, GLOBAL, CYCLIC, DOMAIN_ID_BASE, CENTER, CORNER, &
                     MAX_DOMAIN_FIELDS, MAX_TILES, ZERO, NINETY, MINUS_NINETY, &
                     ONE_HUNDRED_EIGHTY, NONBLOCK_UPDATE_TAG, EDGEUPDATE, EDGEONLY, &
                     NONSYMEDGEUPDATE, NONSYMEDGE
  use mpp_data_mod, only: stat, mpp_stack, ptr_stack, status, ptr_status, sync, &
                     ptr_sync, mpp_from_pe, ptr_from, remote_Data_loc, &
                     ptr_remote, mpp_domains_stack, ptr_domains_stack, &
                     mpp_domains_stack_nonblock, ptr_domains_stack_nonblock
  use mpp_memutils_mod, only: mpp_print_memuse_stats, mpp_mem_dump, &
                     mpp_memuse_begin, mpp_memuse_end
  use mpp_domains_mod, only: domain_axis_spec, domain1D, domain2D, DomainCommunicator2D, &
                     mpp_group_update_type, &
                     mpp_domains_set_stack_size, mpp_get_compute_domain, &
                     mpp_get_compute_domains, mpp_get_data_domain, &
                     mpp_get_global_domain, mpp_get_domain_components, &
                     mpp_get_layout, mpp_get_pelist, operator(.EQ.), operator(.NE.), &
                     mpp_domain_is_symmetry, mpp_domain_is_initialized, &
                     mpp_set_compute_domain, mpp_set_data_domain, mpp_set_global_domain, &
                     mpp_get_memory_domain, mpp_get_domain_shift, &
                     mpp_domain_is_tile_root_pe, mpp_get_tile_id, &
                     mpp_get_domain_extents, mpp_get_current_ntile, &
                     mpp_get_ntile_count, mpp_get_tile_npes, &
                     mpp_get_domain_root_pe, &
                     mpp_get_num_overlap, &
                     mpp_get_io_domain, mpp_get_domain_pe, &
                     mpp_get_domain_tile_root_pe, mpp_get_domain_name, &
                     mpp_get_io_domain_layout, mpp_copy_domain, &
                     mpp_get_domain_npes, &
                     mpp_clear_group_update, mpp_group_update_initialized, &
                     mpp_group_update_is_set, &
                     mpp_global_field, mpp_global_max, mpp_global_min, mpp_global_sum, &
                     mpp_broadcast_domain, &
                     mpp_domains_init, mpp_domains_exit, mpp_redistribute, &
                     mpp_update_domains, mpp_check_field, mpp_start_update_domains, &
                     mpp_complete_update_domains, mpp_create_group_update, &
                     mpp_do_group_update, mpp_start_group_update, &
                     mpp_complete_group_update, mpp_reset_group_update_field, &
                     mpp_get_boundary, &
                     mpp_define_layout, mpp_define_domains, &
                     mpp_modify_domain, mpp_define_mosaic, &
                     mpp_define_null_domain, mpp_mosaic_defined, &
                     mpp_define_io_domain, mpp_deallocate_domain, &
                     mpp_compute_extent, mpp_compute_block_extent, &
                     NULL_DOMAIN1D, NULL_DOMAIN2D

  !> platform
  use platform_mod, only: r8_kind, r4_kind, i8_kind, i4_kind, c8_kind, c4_kind, &
                          l8_kind, l4_kind, i2_kind, ptr_kind

  !> random_numbers
  use random_numbers_mod, only: randomNumberStream, initializeRandomNumberStream, &
                                getRandomNumbers, constructSeed

  !> string_utils
  use fms_string_utils_mod, only: string, fms_array_to_pointer, fms_pointer_to_array, &
                                  fms_find_my_string, fms_find_unique, fms_c2f_string, fms_cstring2cpointer, &
                                  string_copy

  !> time_interp

  !> time_manager
  use time_manager_mod, only: time_type, operator(+), operator(-), operator(*), &
                              operator(/), operator(>), operator(>=), operator(==), &
                              operator(/=), operator(<), operator(<=), operator(//), &
                              assignment(=), set_time, increment_time, decrement_time, &
                              get_time, interval_alarm, repeat_alarm, time_type_to_real, &
                              real_to_time_type, time_list_error, THIRTY_DAY_MONTHS, &
                              JULIAN, GREGORIAN, NOLEAP, NO_CALENDAR, INVALID_CALENDAR, &
                              set_calendar_type, get_calendar_type, set_ticks_per_second, &
                              get_ticks_per_second, set_date, get_date, increment_date, &
                              decrement_date, days_in_month, leap_year, length_of_year, &
                              days_in_year, day_of_year, month_name, valid_calendar_types, &
                              time_manager_init, print_time, print_date, set_date_julian, &
                              get_date_julian, get_date_no_leap, date_to_string
  use get_cal_time_mod, only: get_cal_time

  implicit none

#include <file_version.h>
  character(len=*), parameter, public :: version_FMS = version
  private :: version

end module fms
!> @}
! close documentation grouping
