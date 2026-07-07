#pragma once
/* PROTOTYPE C API for the throwaway TIM I/O spine (read path). */

#ifdef __cplusplus
extern "C" {
#endif

/* Create the component's I/O context on this (Fortran) communicator; call
   from MOM_infra_init. Ensemble-safe: each member passes its own pelist. */
void tim_io_init(int fcomm);

/* Configuration lookup (TIM_input / ParmParse table; env overrides). */
int tim_io_cfg_bool(const char* key, const char* env, int def);

int tim_io_register_domain(int nig, int njg, int isc, int iec, int jsc,
                                int jec, int symmetric);

/* stagger: 0 center, 1 east-face, 2 north-face, 3 corner.
   timelevel: 1-based record or 0. buf: contiguous compute window, x fastest.
   returns 0 on success, PIO error code otherwise. */
int tim_io_read_decomposed(const char* path, const char* varname,
                                int domain_handle, int stagger, int timelevel,
                                int nz, int nz2, double* buf, int* file_sx,
                                int* file_sy);

int tim_io_read_plain(const char* path, const char* varname,
                           int timelevel, int n, double* buf);

int tim_io_var_exists(const char* path, const char* varname);

/* ---- inquiry (read files; held open in the context cache) ---- */
int tim_io_file_exists(const char* path);
int tim_io_file_info(const char* path, int* ndims, int* nvars, int* ntimes);
int tim_io_file_times(const char* path, double* buf, int n);
int tim_io_file_var_name(const char* path, int index1, char* out, int maxlen);
int tim_io_var_att(const char* path, const char* varname, const char* att,
                   char* out, int maxlen); /* nonzero rc when absent */
int tim_io_var_sizes(const char* path, const char* varname, int sizes[4]);
/* Replicated hyperslab; start/nread 1-based, Fortran dim order (x,y,z,t). */
int tim_io_read_slab(const char* path, const char* varname, const int start[4],
                     const int nread[4], double* buf);

void tim_io_finalize(void);

/* ---- write path (stateful file handles) ----
   mode: 0 write, 1 overwrite, 2 append. kind: 0=x, 1=y, 2=unlimited, 3=fixed.
   dims_joined: '\n'-separated axis names in Fortran order (x first). */
int tim_io_createfile(const char* path, int domain_handle, int mode);
int tim_io_def_axis(int fh, const char* name, int kind, int position, int n,
                    const char* units, const char* longname,
                    const char* cartesian, int sense, int has_sense);
int tim_io_def_var(int fh, const char* name, const char* dims_joined,
                   const char* units, const char* longname,
                   const char* std_name, int pack, const char* checksum);
int tim_io_put_global_att(int fh, const char* name, const char* value);
int tim_io_write_axis(int fh, const char* name, const double* data, int n);
int tim_io_var_stagger(int fh, const char* name);
int tim_io_write_decomposed(int fh, const char* name, const double* buf,
                            double tstamp, int has_tstamp);
int tim_io_write_plain(int fh, const char* name, const double* data, int n,
                       double tstamp, int has_tstamp);
int tim_io_closefile(int fh);
int tim_io_file_num_times(int fh);
double tim_io_file_time(int fh);

#ifdef __cplusplus
}
#endif
