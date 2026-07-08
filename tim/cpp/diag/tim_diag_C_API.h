#ifndef TIM_DIAG_C_API_H
#define TIM_DIAG_C_API_H
/* extern "C" surface of the TIM diagnostics manager for the Fortran seam
 * (MOM_diag_manager_infra.F90). Mirrors the tim_io_C_API conventions:
 * null-terminated strings, int handles, days/seconds time pairs (FMS
 * time_type components), 0/1 flags. The adapter owns one DiagManager per
 * component, sharing tim_io_init's IoContext; tim_diag_end destroys it
 * (call before tim_io_finalize). */

#ifdef __cplusplus
extern "C" {
#endif

/* Parses ./diag_table and creates the manager. fms_calendar: FMS
 * time_manager calendar type int. y..s: reference date (prepend_date stamp;
 * y < 0 = none). Returns 0 on success. */
int tim_diag_init(int fms_calendar, int y, int mo, int d, int h, int mi,
                  int s);
int tim_diag_active(void); /* 1 when a manager exists and is healthy */

/* Axis registration. domain_handle: tim_io_register_domain handle (<0 =
 * replicated axis). staggered: FMS EAST/NORTH position in own direction.
 * edges_id: tim axis id of the edges axis (<=0 = none). Returns id >= 1,
 * or 0 for the null axis (n==0 with cart "N"). */
int tim_diag_axis_init(const char* name, const double* data, int n,
                       const char* units, const char* cart,
                       const char* long_name, int domain_handle,
                       int staggered, int direction, int edges_id,
                       const char* set_name);
void tim_diag_axis_name(int id, char* buf, int buflen);

/* Field registration. init_days < 0 = no registration time (statics).
 * has_missing/has_range are presence flags. area_id/volume_id are tim diag
 * field ids (< 0 = none). Returns the field id or -1 (DIAG_FIELD_NOT_FOUND:
 * the diag_table requests no output). */
int tim_diag_register_field(const char* module, const char* field,
                            const int* axes, int naxes, int init_days,
                            int init_secs, const char* long_name,
                            const char* units, const char* standard_name,
                            const char* interp_method, int has_missing,
                            double missing_value, int has_range,
                            double range_lo, double range_hi,
                            int mask_variant, int is_static, int area_id,
                            int volume_id);
int tim_diag_field_id(const char* module, const char* field);
void tim_diag_attr_text(int field_id, const char* name, const char* value);
void tim_diag_attr_ints(int field_id, const char* name, const int* v, int n);
void tim_diag_attr_reals(int field_id, const char* name, const double* v,
                         int n);

/* Post one sample. data: the field's window slice, x fastest, npts values
 * (checked against registration). rmask: optional real mask (< 0.5 =
 * masked), same layout, NULL when absent. days < 0 = untimed (statics).
 * Returns 1 on success (FMS send_data convention). */
int tim_diag_post(int field_id, int days, int secs, const double* data,
                  long long npts, const double* rmask, double weight);

void tim_diag_send_complete(void);
void tim_diag_set_time_end(int days, int secs);
/* Final flush + close + destroy the manager. Returns 0 on success. */
int tim_diag_end(int days, int secs);

/* Restart-spanning averaging (Q5). restore returns 0 = restored,
 * 1 = no state file (cold start), < 0 = error. */
int tim_diag_save_state(const char* path);
int tim_diag_restore_state(const char* path);

#ifdef __cplusplus
}
#endif
#endif /* TIM_DIAG_C_API_H */
