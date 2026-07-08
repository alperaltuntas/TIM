// C API adapter for TIM diagnostics: extern "C" surface over DiagManager.
// One explicitly-managed manager per component (no singletons in the C++
// layer), created by tim_diag_init on the IoContext tim_io_init built and
// destroyed by tim_diag_end — which MOM calls (diag_manager_end) before
// io_infra_end calls tim_io_finalize, so the borrow order is safe.

#include "tim_diag_C_API.h"

#include "../io/tim_io_C_API_internal.hpp"
#include "../io/tim_io_context.hpp"
#include "tim_diag_manager.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using TIM::Diag::DiagConfig;
using TIM::Diag::DiagManager;

// The manager keeps a reference to its config; both live here together.
std::unique_ptr<DiagConfig> g_cfg;
std::unique_ptr<DiagManager> g_mgr;

TIM::TimeStamp ts(int days, int secs) {
  return TIM::TimeStamp{days, secs, 0};
}

std::string str(const char* s) { return s ? std::string(s) : std::string(); }

// The Fortran-facing field ids are 1-based: MOM treats id <= 0 as "not
// registered" (`if (id > 0) call post_data(...)`), so the manager's 0-based
// indices shift by one at this boundary.
int toFortranId(int mgr_id) { return mgr_id < 0 ? mgr_id : mgr_id + 1; }
int toManagerId(int f_id) { return f_id <= 0 ? TIM::Diag::kFieldNotFound : f_id - 1; }

}  // namespace

extern "C" {

int tim_diag_init(int fms_calendar, int y, int mo, int d, int h, int mi,
                  int s) {
  if (g_mgr) return 0;

  TIM::Calendar cal;
  if (!TIM::calendarFromFms(fms_calendar, &cal)) {
    std::fprintf(stderr, "tim_diag_init: bad calendar type %d\n", fms_calendar);
    return -1;
  }
  std::string err;
  g_cfg = std::make_unique<DiagConfig>();
  auto parsed = TIM::Diag::parseClassicDiagTable("diag_table", &err);
  if (!parsed) {
    std::fprintf(stderr, "tim_diag_init: %s\n", err.c_str());
    g_cfg.reset();
    return -1;
  }
  *g_cfg = std::move(*parsed);

  // Provisional run start = the diag_table base date; the first field
  // registration that carries init_time advances it (see registerField).
  TIM::TimeStamp init;
  if (!TIM::fromDate(cal, g_cfg->base_date, &init, &err)) {
    std::fprintf(stderr, "tim_diag_init: base date: %s\n", err.c_str());
    g_cfg.reset();
    return -1;
  }

  DiagManager::Options opts;
  opts.prepend_date = y >= 0;  // FMS: prepend only when init got time_init
  if (y >= 0) {
    // The prepend stamp uses init_time (set at first registration); the
    // reference date is recorded through init when provided.
    TIM::DateFields ref{y, mo, d, h, mi, s};
    TIM::TimeStamp t;
    if (TIM::fromDate(cal, ref, &t, &err) && t > init) init = t;
  }
  g_mgr = std::make_unique<DiagManager>(*g_cfg, cal, init,
                                        TIM::IO::currentIoContext().sys(),
                                        opts);
  if (!g_mgr->ok()) {
    std::fprintf(stderr, "tim_diag_init: %s\n", g_mgr->error().c_str());
    g_mgr.reset();
    g_cfg.reset();
    return -1;
  }
  return 0;
}

int tim_diag_active(void) { return (g_mgr && g_mgr->ok()) ? 1 : 0; }

int tim_diag_axis_init(const char* name, const double* data, int n,
                       const char* units, const char* cart,
                       const char* long_name, int domain_handle,
                       int staggered, int direction, int edges_id,
                       const char* set_name) {
  if (!g_mgr) return -1;
  TIM::Diag::AxisSpec a;
  a.name = str(name);
  a.values.assign(data, data + (n > 0 ? n : 0));
  a.units = str(units);
  a.cartesian = str(cart);
  for (auto& c : a.cartesian) c = (char)std::toupper((unsigned char)c);
  a.long_name = str(long_name);
  a.direction = direction;
  a.edges = edges_id > 0 ? edges_id : TIM::Diag::kNullAxis;
  a.set_name = str(set_name);
  a.staggered = staggered != 0;
  if (domain_handle >= 0) {
    a.domain_key = domain_handle;
    a.domain = TIM::IO::currentIoContext().domain(domain_handle);
  }
  return g_mgr->defineAxis(a);
}

void tim_diag_axis_name(int id, char* buf, int buflen) {
  const std::string n = g_mgr ? g_mgr->axisName(id) : std::string();
  std::snprintf(buf, (size_t)buflen, "%s", n.c_str());
}

int tim_diag_register_field(const char* module, const char* field,
                            const int* axes, int naxes, int init_days,
                            int init_secs, const char* long_name,
                            const char* units, const char* standard_name,
                            const char* interp_method, int has_missing,
                            double missing_value, int has_range,
                            double range_lo, double range_hi,
                            int mask_variant, int is_static, int area_id,
                            int volume_id) {
  if (!g_mgr) return TIM::Diag::kFieldNotFound;
  TIM::Diag::FieldOptions o;
  o.long_name = str(long_name);
  o.units = str(units);
  o.standard_name = str(standard_name);
  o.interp_method = str(interp_method);
  if (has_missing) o.missing_value = missing_value;
  if (has_range) o.valid_range = std::make_pair(range_lo, range_hi);
  o.mask_variant = mask_variant != 0;
  o.is_static = is_static != 0;
  o.area_field = toManagerId(area_id);
  o.volume_field = toManagerId(volume_id);

  std::vector<int> ax(axes, axes + (naxes > 0 ? naxes : 0));
  if (init_days >= 0) {
    const TIM::TimeStamp t = ts(init_days, init_secs);
    return toFortranId(g_mgr->registerField(str(module), str(field), ax, o, &t));
  }
  return toFortranId(g_mgr->registerField(str(module), str(field), ax, o));
}

int tim_diag_field_id(const char* module, const char* field) {
  return g_mgr ? toFortranId(g_mgr->fieldId(str(module), str(field)))
               : TIM::Diag::kFieldNotFound;
}

void tim_diag_attr_text(int field_id, const char* name, const char* value) {
  if (g_mgr) g_mgr->addAttribute(toManagerId(field_id), str(name), str(value));
}
void tim_diag_attr_ints(int field_id, const char* name, const int* v, int n) {
  if (g_mgr) g_mgr->addAttribute(toManagerId(field_id), str(name), v, n);
}
void tim_diag_attr_reals(int field_id, const char* name, const double* v,
                         int n) {
  if (g_mgr) g_mgr->addAttribute(toManagerId(field_id), str(name), v, n);
}

int tim_diag_post(int field_id, int days, int secs, const double* data,
                  long long npts, const double* rmask, double weight) {
  if (!g_mgr) return 0;
  const int mid = toManagerId(field_id);
  const long long want = g_mgr->fieldNpts(mid);
  if (want >= 0 && npts != want) {
    std::fprintf(stderr,
                 "tim_diag_post: field %d got %lld points, expected %lld\n",
                 field_id, npts, want);
    return 0;
  }
  std::string err;
  const bool ok = g_mgr->post(mid, ts(days < 0 ? 0 : days, secs), data,
                              nullptr, rmask, weight, &err);
  if (!ok) std::fprintf(stderr, "tim_diag_post: %s\n", err.c_str());
  return ok ? 1 : 0;
}

void tim_diag_send_complete(void) {
  if (g_mgr) g_mgr->sendComplete();
}

void tim_diag_set_time_end(int days, int secs) {
  if (g_mgr) g_mgr->setTimeEnd(ts(days, secs));
}

int tim_diag_end(int days, int secs) {
  if (!g_mgr) return 0;
  const int rc = g_mgr->end(ts(days, secs));
  g_mgr.reset();
  g_cfg.reset();
  return rc;
}

int tim_diag_save_state(const char* path) {
  return g_mgr ? g_mgr->saveState(str(path)) : -1;
}

int tim_diag_restore_state(const char* path) {
  return g_mgr ? g_mgr->restoreState(str(path)) : -1;
}

}  // extern "C"
