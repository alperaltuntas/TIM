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
                                int nz, int nz2, double* buf);

int tim_io_read_plain(const char* path, const char* varname,
                           int timelevel, int n, double* buf);

int tim_io_var_exists(const char* path, const char* varname);

void tim_io_finalize(void);

#ifdef __cplusplus
}
#endif
