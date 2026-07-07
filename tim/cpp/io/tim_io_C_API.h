#pragma once
/* PROTOTYPE C API for the throwaway TIM I/O spine (read path). */

#ifdef __cplusplus
extern "C" {
#endif

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
