// Parser check against the two production diag_tables.
#include "../../tim/cpp/diag/tim_diag_config.hpp"
#include <cstdio>

int main(int argc, char** argv) {
  using namespace TIM::Diag;
  for (int a = 1; a < argc; ++a) {
    std::string err;
    auto cfg = parseClassicDiagTable(argv[a], &err);
    if (!cfg) { std::printf("FAIL %s: %s\n", argv[a], err.c_str()); return 1; }
    std::printf("OK   %s: title='%s' base=%d-%02d-%02d files=%zu fields=%zu\n",
                argv[a], cfg->title.c_str(), cfg->base_date.year,
                cfg->base_date.month, cfg->base_date.day, cfg->files.size(),
                cfg->fields.size());
    for (const auto& f : cfg->files)
      std::printf("  file %-22s freq=%d u=%d newf=%d\n", f.name.c_str(),
                  f.output_freq, (int)f.output_freq_units, f.new_file_freq);
    int mean = 0, none = 0;
    for (const auto& fd : cfg->fields)
      (fd.reduction == Reduction::Mean ? mean : none)++;
    std::printf("  reductions: mean=%d other/none=%d\n", mean, none);
  }
  return 0;
}
