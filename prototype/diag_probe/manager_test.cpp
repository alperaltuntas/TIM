// DiagManager end-to-end checks: one rank, real PIO underneath, verified by
// re-reading the produced files through Backend::Serial (plain netCDF) —
// an independent code path from the one that wrote them.
//
// Covers: window scheduler (strict > trigger, midpoint record times),
// weighted means, snapshots incl. end-of-run >= flush, statics at close,
// FMS filename stamping, average_T1/T2/DT + time_bounds, attribute
// plumbing (_FillValue/missing_value typed by packing, cell_methods time
// suffix, time_avg_info, axis/positive/calendar), scalar streams, and the
// Q5 restart-spanning gate: continuous vs save+restore mid-window runs
// produce bit-identical records.
#include "../../tim/cpp/diag/tim_diag_manager.hpp"
#include "../../tim/cpp/io/tim_iosystem.hpp"

#include <mpi.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

using namespace TIM;
using namespace TIM::Diag;

static int failures = 0;
#define CHECK(cond, what)                                        \
  do {                                                           \
    if (!(cond)) {                                               \
      std::printf("FAIL %s (%s:%d)\n", what, __FILE__, __LINE__); \
      ++failures;                                                \
    }                                                            \
  } while (0)

namespace {

const int NX = 8, NY = 6, NZ = 2;

void writeDiagTable(const char* path) {
  std::ofstream t(path);
  t << "\"Manager Test\"\n1 1 1 0 0 0\n"
    << "\"avg_%4yr_%2mo\", 1, \"days\", 1, \"days\", \"time\", 1, \"months\"\n"
    << "\"snap\", 2, \"days\", 1, \"days\", \"Time\"\n"
    << "\"ocn\", \"sst\", \"sst\", \"avg_%4yr_%2mo\", \"all\", \"mean\", \"none\", 1\n"
    << "\"ocn\", \"tke\", \"tke\", \"avg_%4yr_%2mo\", \"all\", \"mean\", \"none\", 1\n"
    << "\"ocn\", \"u\", \"u\", \"snap\", \"all\", \"none\", \"none\", 2\n"
    << "\"ocn\", \"geolat\", \"geolat\", \"snap\", \"all\", \".false.\", \"none\", 1\n";
}

TimeStamp at(double days) {
  const long long s = (long long)std::llround(days * 86400.0);
  return TimeStamp{(int)(s / 86400), (int)(s % 86400), 0};
}

double sstVal(int i, int j, double t) { return 10.0 + i + 0.1 * j + t; }
double uVal(int i, int j, int k, double t) {
  return 100.0 * k + i + 0.01 * j + t;
}

struct Setup {
  std::unique_ptr<DiagManager> mgr;
  int sst = -1, u = -1, geolat = -1, tke = -1;
};

Setup makeManager(const DiagConfig& cfg, IO::IoSystem& sys,
                  const Decomp2D& dom) {
  Setup s;
  s.mgr = std::make_unique<DiagManager>(cfg, Calendar::NoLeap, at(0.0), sys);
  CHECK(s.mgr->ok(), s.mgr->error().c_str());

  std::vector<double> xh(NX), yh(NY), xq(NX + 1), zl{10.0, 20.0};
  for (int i = 0; i < NX; ++i) xh[i] = i + 0.5;
  for (int j = 0; j < NY; ++j) yh[j] = j + 0.5;
  for (int i = 0; i <= NX; ++i) xq[i] = i;

  AxisSpec ax;
  ax.name = "xh"; ax.values = xh; ax.units = "degrees_east";
  ax.cartesian = "X"; ax.long_name = "h longitude";
  ax.domain_key = 7; ax.domain = dom;
  const int id_xh = s.mgr->defineAxis(ax);
  ax.name = "yh"; ax.values = yh; ax.units = "degrees_north";
  ax.cartesian = "Y"; ax.long_name = "h latitude";
  const int id_yh = s.mgr->defineAxis(ax);
  ax.name = "xq"; ax.values = xq; ax.cartesian = "X";
  ax.long_name = "q longitude"; ax.staggered = true;
  const int id_xq = s.mgr->defineAxis(ax);
  AxisSpec az;
  az.name = "zl"; az.values = zl; az.units = "kg m-3"; az.cartesian = "Z";
  az.long_name = "layer"; az.direction = -1;
  const int id_zl = s.mgr->defineAxis(az);

  FieldOptions fo;
  fo.long_name = "Sea Surface Temperature"; fo.units = "degC";
  fo.missing_value = -1e34;
  s.sst = s.mgr->registerField("ocn", "sst", {id_xh, id_yh}, fo);
  s.mgr->addAttribute(s.sst, "cell_methods", "area:mean");

  FieldOptions fu;
  fu.long_name = "Zonal velocity"; fu.units = "m s-1"; fu.interp_method = "none";
  s.u = s.mgr->registerField("ocn", "u", {id_xq, id_yh, id_zl}, fu);

  FieldOptions fg;
  fg.long_name = "Latitude"; fg.units = "degrees"; fg.is_static = true;
  s.geolat = s.mgr->registerField("ocn", "geolat", {id_xh, id_yh}, fg);

  FieldOptions ft;
  ft.long_name = "Total KE"; ft.units = "J";
  s.tke = s.mgr->registerField("ocn", "tke", {kNullAxis}, ft);

  CHECK(s.sst >= 0 && s.u >= 0 && s.geolat >= 0 && s.tke >= 0,
        "all fields registered");
  CHECK(s.mgr->registerField("ocn", "not_in_table", {id_xh, id_yh}, fo) ==
            kFieldNotFound,
        "unrequested field -> DIAG_FIELD_NOT_FOUND");
  CHECK(s.mgr->fieldId("OCN", "SST") == s.sst, "case-insensitive lookup");
  return s;
}

void postStep(Setup& s, double t) {
  std::vector<double> sst(NX * NY), u((NX + 1) * NY * NZ);
  for (int j = 0; j < NY; ++j)
    for (int i = 0; i < NX; ++i) sst[j * NX + i] = sstVal(i, j, t);
  for (int k = 0; k < NZ; ++k)
    for (int j = 0; j < NY; ++j)
      for (int i = 0; i <= NX; ++i)
        u[(k * NY + j) * (NX + 1) + i] = uVal(i, j, k, t);
  double tk = 1000.0 + t;
  std::string err;
  CHECK(s.mgr->post(s.sst, at(t), sst.data(), nullptr, nullptr, 1.0, &err),
        err.c_str());
  CHECK(s.mgr->post(s.u, at(t), u.data()), "post u");
  CHECK(s.mgr->post(s.tke, at(t), &tk), "post tke");
}

void postGeolat(Setup& s) {
  std::vector<double> g(NX * NY);
  for (int j = 0; j < NY; ++j)
    for (int i = 0; i < NX; ++i) g[j * NX + i] = -30.0 + j;
  CHECK(s.mgr->post(s.geolat, at(0.0), g.data()), "post geolat");
}

}  // namespace

int main(int argc, char** argv) {
  MPI_Init(&argc, &argv);

  {  // filename stamping (literal FMS get_time_string port)
    CHECK(fmsBaseName("avg_%4yr_%2mo") == "avg_", "base truncated at %digit");
    CHECK(fmsBaseName("snap") == "snap", "no token: base = name");
    // 0001-01-06, noleap: yr+dy tokens -> julian day-of-year
    CHECK(fmsTimeSuffix("p_%4yr_%3dy", Calendar::NoLeap, at(5.5)) ==
              ".0001-006",
          "yr+dy stamp");
    CHECK(fmsTimeSuffix("m_%4yr_%2mo", Calendar::NoLeap, at(45.5)) ==
              ".0001-02",
          "yr+mo stamp");
    // no yr: months count cumulatively from year 1
    CHECK(fmsTimeSuffix("m_%3mo", Calendar::NoLeap, at(400.0)) == ".014",
          "cumulative months without yr token");
  }

  {  // IoSystem must die before MPI_Finalize (the io_infra_end lesson)
  IO::IoSystem::Options io_opts;
  io_opts.niotasks = 1;
  IO::IoSystem sys(MPI_COMM_WORLD, io_opts);
  const Decomp2D dom(NX, NY, 1, NX, 1, NY, /*symmetric=*/true);

  writeDiagTable("diag_table_test");
  std::string perr;
  auto cfg = Diag::parseClassicDiagTable("diag_table_test", &perr);
  CHECK(cfg.has_value(), perr.c_str());
  if (!cfg) { MPI_Finalize(); return 1; }

  // ---- continuous run: 10 days, quarter-day steps ----
  mkdir("run_a", 0755);
  chdir("run_a");
  {
    Setup s = makeManager(*cfg, sys, dom);
    postGeolat(s);
    for (int k = 1; k <= 40; ++k) postStep(s, 0.25 * k);
    CHECK(s.mgr->end(at(10.0)) == 0, "end");
  }
  chdir("..");

  using S = IO::Backend::Serial;
  const char* avg = "run_a/avg_.0001-01.nc";
  const char* snap = "run_a/snap.nc";
  CHECK(S::fileExists(avg), "rollover filename avg_.0001-01.nc");
  CHECK(S::fileExists(snap), "plain filename snap.nc");

  {  // averaged file: 10 daily-mean records at window midpoints
    int nd, nv, nt;
    CHECK(S::fileInfo(avg, &nd, &nv, &nt) == 0 && nt == 10, "10 records");
    std::vector<double> tv(10);
    S::timeValues(avg, tv.data(), 10);
    bool mid_ok = true;
    for (int r = 0; r < 10; ++r) mid_ok = mid_ok && tv[r] == r + 0.5;
    CHECK(mid_ok, "record times = window midpoints");

    // day-d mean of sst: posts at d+.25,.5,.75,1.0, weight 1 each
    for (int r : {0, 4, 9}) {
      int start[4] = {1, 1, r + 1, 1}, count[4] = {NX, NY, 1, 1};
      std::vector<double> a(NX * NY);
      CHECK(S::readSlab(avg, "sst", start, count, a.data()) == 0, "read sst");
      bool ok = true;
      for (int j = 0; j < NY; ++j)
        for (int i = 0; i < NX; ++i) {
          double sum = 0;
          for (int q = 1; q <= 4; ++q) sum += sstVal(i, j, r + 0.25 * q);
          ok = ok && a[j * NX + i] == sum / 4.0;
        }
      CHECK(ok, "sst daily mean bit-exact");
    }
    {  // scalar stream rides the same records
      int start[4] = {5, 1, 1, 1}, count[4] = {1, 1, 1, 1};
      std::vector<double> v(1);
      CHECK(S::readSlab(avg, "tke", start, count, v.data()) == 0, "read tke");
      double sum = 0;
      for (int q = 1; q <= 4; ++q) sum += 1000.0 + 4.0 + 0.25 * q;
      CHECK(v[0] == sum / 4.0, "scalar mean record 5");
    }
    {  // averaging metadata
      int start[4] = {3, 1, 1, 1}, count[4] = {1, 1, 1, 1};
      double t1, t2, dt;
      S::readSlab(avg, "average_T1", start, count, &t1);
      S::readSlab(avg, "average_T2", start, count, &t2);
      S::readSlab(avg, "average_DT", start, count, &dt);
      CHECK(t1 == 2.0 && t2 == 3.0 && dt == 1.0, "average_T1/T2/DT record 3");
      int s2[4] = {1, 3, 1, 1}, c2[4] = {2, 1, 1, 1};
      double bnds[2];
      S::readSlab(avg, "time_bounds", s2, c2, bnds);
      CHECK(bnds[0] == 2.0 && bnds[1] == 3.0, "time_bounds record 3");
    }
    std::string a;
    CHECK(S::attText(avg, "sst", "cell_methods", &a) == 0 &&
              a == "area:mean time: mean",
          "cell_methods with FMS time suffix");
    CHECK(S::attText(avg, "sst", "time_avg_info", &a) == 0 &&
              a == "average_T1,average_T2,average_DT",
          "time_avg_info");
    CHECK(S::attText(avg, "time", "calendar", &a) == 0 && a == "noleap",
          "time calendar attribute");
    CHECK(S::attText(avg, "time", "bounds", &a) == 0 && a == "time_bounds",
          "time bounds attribute");
  }

  {  // snapshot file: strict > trigger stamps the boundary sample
    int nd, nv, nt;
    CHECK(S::fileInfo(snap, &nd, &nv, &nt) == 0 && nt == 5,
          "5 snapshot records (2,4,6,8 + >= flush at 10)");
    std::vector<double> tv(5);
    S::timeValues(snap, tv.data(), 5);
    CHECK(tv[0] == 2.0 && tv[3] == 8.0 && tv[4] == 10.0,
          "snapshot record times");
    // record 1 (t=2): value posted AT the boundary (2.0 accumulated into the
    // then-open window; 2.25 triggered the write)
    int start[4] = {1, 1, 1, 1}, count[4] = {NX + 1, NY, NZ, 1};
    std::vector<double> u((NX + 1) * NY * NZ);
    CHECK(S::readSlab(snap, "u", start, count, u.data()) == 0, "read u");
    bool ok = true;
    for (int k = 0; k < NZ; ++k)
      for (int j = 0; j < NY; ++j)
        for (int i = 0; i <= NX; ++i)
          ok = ok && u[(k * NY + j) * (NX + 1) + i] ==
                         (double)(float)uVal(i, j, k, 2.0);
    CHECK(ok, "snapshot = boundary sample, float packing");
    // statics written at close, no time dim
    int sizes[4];
    CHECK(S::varSizes(snap, "geolat", sizes) == 2 && sizes[0] == NX &&
              sizes[1] == NY,
          "static var 2-d");
    std::vector<double> g(NX * NY);
    int gs[4] = {1, 1, 1, 1}, gc[4] = {NX, NY, 1, 1};
    CHECK(S::readSlab(snap, "geolat", gs, gc, g.data()) == 0 &&
              g[2 * NX + 3] == -28.0,
          "static values");
    std::string a;
    CHECK(S::attText(snap, "zl", "positive", &a) == 0 && a == "down",
          "z positive attribute");
    CHECK(S::attText(snap, "u", "interp_method", &a) == 0 && a == "none",
          "interp_method attribute");
    CHECK(S::attText(snap, "Time", "calendar", &a) == 0 && a == "noleap",
          "snap Time axis calendar");
  }

  // ---- Q5: mid-window save/restore, bit-identical to continuous ----
  mkdir("run_b", 0755);
  chdir("run_b");
  {
    Setup s = makeManager(*cfg, sys, dom);
    postGeolat(s);
    for (int k = 1; k <= 6; ++k) postStep(s, 0.25 * k);  // through t=1.5
    CHECK(s.mgr->saveState("../diag_state.nc") == 0, "saveState");
    // segment 1 abandoned mid-window (its partial file is segment 1's)
  }
  chdir("..");
  mkdir("run_c", 0755);
  chdir("run_c");
  {
    // Fresh manager, restart at t=1.5 (mid window (1,2]).
    Setup s = makeManager(*cfg, sys, dom);
    postGeolat(s);
    CHECK(s.mgr->restoreState("../diag_state.nc") == 0, "restoreState");
    CHECK(s.mgr->restoreState("../no_such_state.nc") == 1,
          "missing state = cold start signal");
    for (int k = 7; k <= 12; ++k) postStep(s, 0.25 * k);  // t=1.75..3.0
    CHECK(s.mgr->end(at(3.0)) == 0, "end segment 2");
  }
  chdir("..");

  {
    // Continuous run over the same 3 days for the reference records.
    mkdir("run_d", 0755);
    chdir("run_d");
    Setup s = makeManager(*cfg, sys, dom);
    postGeolat(s);
    for (int k = 1; k <= 12; ++k) postStep(s, 0.25 * k);
    CHECK(s.mgr->end(at(3.0)) == 0, "end continuous");
    chdir("..");

    const char* cont = "run_d/avg_.0001-01.nc";
    const char* rest = "run_c/avg_.0001-01.nc";
    int nd, nv, ntc, ntr;
    S::fileInfo(cont, &nd, &nv, &ntc);
    S::fileInfo(rest, &nd, &nv, &ntr);
    CHECK(ntc == 3, "continuous: 3 daily records");
    CHECK(ntr == 2, "restarted segment: records for windows (1,2],(2,3]");
    // The restart-spanning window (1,2] — record 2 continuous, record 1
    // restarted — must be BIT-identical (Q5 gate).
    for (int pair = 0; pair < 2; ++pair) {
      const int rc = 2 + pair, rr = 1 + pair;
      int sc[4] = {1, 1, rc, 1}, sr[4] = {1, 1, rr, 1},
          count[4] = {NX, NY, 1, 1};
      std::vector<double> a(NX * NY), b(NX * NY);
      S::readSlab(cont, "sst", sc, count, a.data());
      S::readSlab(rest, "sst", sr, count, b.data());
      bool bit = true;
      for (size_t q = 0; q < a.size(); ++q) bit = bit && a[q] == b[q];
      CHECK(bit, pair == 0 ? "restart-spanning window bit-identical"
                           : "post-restart window bit-identical");
      double ta = -1, tb = -2;
      int c1[4] = {1, 1, 1, 1};
      int sa[4] = {rc, 1, 1, 1}, sb[4] = {rr, 1, 1, 1};
      S::readSlab(cont, "average_T1", sa, c1, &ta);
      S::readSlab(rest, "average_T1", sb, c1, &tb);
      CHECK(ta == tb, "average_T1 matches across restart");
    }
  }
  }  // IoSystem scope

  if (failures == 0) std::printf("ALL MANAGER CHECKS PASS\n");
  MPI_Finalize();
  return failures == 0 ? 0 : 1;
}
