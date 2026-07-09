// ExternalField checks: one rank, real PIO for the decomposed reads, plain
// netCDF to synthesize a monthly modulo climatology shaped exactly like the
// CESM salt-restore files (TIME modulo=" ", units "days since 0001-01-01",
// calendar noleap, float var with _FillValue=-1e34). Verifies FMS
// time_interp_list semantics bit-for-bit against hand-computed doubles.
#include "../../tim/cpp/io/tim_external_field.hpp"
#include "../../tim/cpp/io/tim_iosystem.hpp"

#include <mpi.h>
#include <netcdf.h>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace TIM;
using namespace TIM::IO;

static int failures = 0;
#define CHECK(cond, what)                                         \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s (%s:%d)\n", what, __FILE__, __LINE__); \
      ++failures;                                                 \
    }                                                             \
  } while (0)

namespace {
const int NX = 8, NY = 6, NREC = 12;
const double kTimes[NREC] = {15,   44,    73.5, 104,   134.5, 165,
                             195.5, 226.5, 257,  287.5, 318.5, 349};
const float kMiss = -1.0e34f;

// Record value at (i,j): base + rec (masked cell at i=2,j=1).
float val(int i, int j, int rec) {
  if (i == 2 && j == 1) return kMiss;
  return (float)(30.0 + i + 0.1 * j + rec);
}

void writeClim(const char* path) {
  int nc, dt, dy, dx, vt, vv;
  nc_create(path, NC_CLOBBER, &nc);
  nc_def_dim(nc, "TIME", NC_UNLIMITED, &dt);
  nc_def_dim(nc, "LAT", NY, &dy);
  nc_def_dim(nc, "LON", NX, &dx);
  int td[1] = {dt};
  nc_def_var(nc, "TIME", NC_DOUBLE, 1, td, &vt);
  nc_put_att_text(nc, vt, "units", 25, "days since 0001-01-01");
  nc_put_att_text(nc, vt, "calendar", 6, "noleap");
  nc_put_att_text(nc, vt, "modulo", 1, " ");
  int vd[3] = {dt, dy, dx};
  nc_def_var(nc, "salt", NC_FLOAT, 3, vd, &vv);
  nc_put_att_float(nc, vv, "_FillValue", NC_FLOAT, 1, &kMiss);
  nc_put_att_float(nc, vv, "missing_value", NC_FLOAT, 1, &kMiss);
  nc_enddef(nc);
  for (int r = 0; r < NREC; ++r) {
    size_t s[3] = {(size_t)r, 0, 0}, c[3] = {1, NY, NX};
    std::vector<float> buf(NX * NY);
    for (int j = 0; j < NY; ++j)
      for (int i = 0; i < NX; ++i) buf[j * NX + i] = val(i, j, r + 1);
    nc_put_vara_float(nc, vv, s, c, buf.data());
    size_t ts = r, tc = 1;
    nc_put_vara_double(nc, vt, &ts, &tc, &kTimes[r]);
  }
  nc_close(nc);
}

TimeStamp at(double days) {
  const long long s = (long long)std::llround(days * 86400.0);
  return TimeStamp{(int)(s / 86400), (int)(s % 86400), 0};
}

double dval(int i, int j, int rec) { return (double)val(i, j, rec); }
}  // namespace

int main(int argc, char** argv) {
  MPI_Init(&argc, &argv);
  writeClim("extfield_clim.nc");
  {
    IoSystem::Options o;
    o.niotasks = 1;
    IoSystem sys(MPI_COMM_WORLD, o);
    const Decomp2D dom(NX, NY, 1, NX, 1, NY, false);

    ExternalField f(sys, "extfield_clim.nc", "SALT" /*case-insens*/, 7, dom,
                    Calendar::NoLeap);
    CHECK(f.ok(), f.ok() ? "ok" : f.error().c_str());
    CHECK(f.varName() == "salt", "case-insensitive resolve");
    int siz[4];
    f.sizes(siz);
    CHECK(siz[0] == NX && siz[1] == NY && siz[2] == 1 && siz[3] == NREC,
          "sizes");
    CHECK(f.missingValue() == (double)kMiss, "missing = double(-1e34f)");
    CHECK(f.npts() == NX * NY, "window npts");

    std::vector<double> out(NX * NY);
    std::vector<unsigned char> m(NX * NY);
    auto idx = [&](int i, int j) { return j * NX + i; };

    // Exactly on record 1 (T == Ts): w2 = 0, pure record 1.
    CHECK(f.interp(at(15.0), out.data(), m.data()) == 0, "interp t=15");
    CHECK(out[idx(3, 2)] == dval(3, 2, 1), "exact record value");
    CHECK(out[idx(2, 1)] == (double)kMiss && m[idx(2, 1)] == 0,
          "masked cell -> missing");
    CHECK(m[idx(3, 2)] == 1, "valid mask");

    // Interior bracket: t=50 in (44, 73.5]: w2 = 6*86400 / (29.5*86400).
    {
      CHECK(f.interp(at(50.0), out.data(), nullptr) == 0, "interp t=50");
      const double w2 = (6.0 * 86400.0) / (2549 * 1000.0 + 86400.0 * 29.5 - 2549000.0);
      (void)w2;  // compute reference the same way the impl does:
      const double W2 = (6.0 * 86400.0) / (29.5 * 86400.0);
      const double ref = dval(3, 2, 2) * (1.0 - W2) + dval(3, 2, 3) * W2;
      CHECK(out[idx(3, 2)] == ref, "interior weight bit-exact");
    }

    // Year wrap, after last record: t=360: w2 = 11/(365-334) = 11/31.
    {
      CHECK(f.interp(at(360.0), out.data(), nullptr) == 0, "interp t=360");
      const double W2 = (11.0 * 86400.0) / (31.0 * 86400.0);
      const double ref = dval(3, 2, 12) * (1.0 - W2) + dval(3, 2, 1) * W2;
      CHECK(out[idx(3, 2)] == ref, "wrap after Te");
    }
    // Year wrap, before first record: t=5: w2 = 1 - 10/31.
    {
      CHECK(f.interp(at(5.0), out.data(), nullptr) == 0, "interp t=5");
      const double W2 = 1.0 - (10.0 * 86400.0) / (31.0 * 86400.0);
      const double ref = dval(3, 2, 12) * (1.0 - W2) + dval(3, 2, 1) * W2;
      CHECK(out[idx(3, 2)] == ref, "wrap before Ts");
    }
    // Modulo year mapping: year 4 day-of-year 50 == year 1 day 50.
    {
      std::vector<double> out2(NX * NY);
      CHECK(f.interp(at(3 * 365 + 50.0), out2.data(), nullptr) == 0,
            "interp yr4");
      f.interp(at(50.0), out.data(), nullptr);
      bool same = true;
      for (size_t q = 0; q < out.size(); ++q) same = same && out[q] == out2[q];
      CHECK(same, "modulo year mapping bit-identical");
    }

    // Replicated (no-domain) path agrees with the decomposed path.
    {
      ExternalField fr(sys, "extfield_clim.nc", "salt", -1, Decomp2D(),
                       Calendar::NoLeap);
      CHECK(fr.ok(), fr.ok() ? "ok" : fr.error().c_str());
      std::vector<double> outr(NX * NY);
      CHECK(fr.interp(at(50.0), outr.data(), nullptr) == 0, "replicated");
      f.interp(at(50.0), out.data(), nullptr);
      bool same = true;
      for (size_t q = 0; q < out.size(); ++q)
        same = same && out[q] == outr[q];
      CHECK(same, "replicated == decomposed");
    }
  }
  if (failures == 0) std::printf("ALL EXTFIELD CHECKS PASS\n");
  MPI_Finalize();
  return failures == 0 ? 0 : 1;
}
