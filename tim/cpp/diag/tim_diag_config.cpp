#include "tim_diag_config.hpp"

#include <cctype>
#include <fstream>
#include <sstream>

namespace TIM {
namespace Diag {

namespace {

// Splits one diag_table line into comma-separated tokens, honoring quotes and
// stripping surrounding whitespace; a '#' outside quotes starts a comment.
std::vector<std::string> tokenize(const std::string& line) {
  std::vector<std::string> toks;
  std::string cur;
  bool in_quote = false, any = false;
  for (char ch : line) {
    if (ch == '"') { in_quote = !in_quote; any = true; continue; }
    if (!in_quote && ch == '#') break;
    if (!in_quote && ch == ',') {
      toks.push_back(cur); cur.clear(); any = true; continue;
    }
    if (!in_quote && std::isspace((unsigned char)ch)) {
      if (!cur.empty()) cur += ch;  // keep interior spaces, trim later
      continue;
    }
    cur += ch;
  }
  if (!cur.empty() || any) toks.push_back(cur);
  // trim trailing whitespace kept above
  for (auto& t : toks) {
    while (!t.empty() && std::isspace((unsigned char)t.back())) t.pop_back();
  }
  while (!toks.empty() && toks.back().empty()) toks.pop_back();
  return toks;
}

std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

bool parseInt(const std::string& s, int* v) {
  try {
    size_t pos = 0;
    *v = std::stoi(s, &pos);
    return pos == s.size();
  } catch (...) { return false; }
}

// "mean"/"average"/"avg"/.true. -> Mean; "none"/"point"/.false. -> None;
// min/max/rms; "pow##"; "diurnal##".
bool parseReduction(const std::string& raw, Reduction* r, int* pow_exp,
                    int* diurnal) {
  const std::string s = lower(raw);
  *pow_exp = 1; *diurnal = 0;
  if (s == "mean" || s == "average" || s == "avg" || s == ".true.") *r = Reduction::Mean;
  else if (s == "none" || s == "point" || s == ".false.") *r = Reduction::None;
  else if (s == "min") *r = Reduction::Min;
  else if (s == "max") *r = Reduction::Max;
  else if (s == "rms") *r = Reduction::RMS;
  else if (s.rfind("pow", 0) == 0 && parseInt(s.substr(3), pow_exp)) *r = Reduction::Pow;
  else if (s.rfind("diurnal", 0) == 0 && parseInt(s.substr(7), diurnal)) *r = Reduction::Diurnal;
  else return false;
  return true;
}

}  // namespace

const FileSpec* DiagConfig::findFile(const std::string& name) const {
  for (const auto& f : files)
    if (f.name == name) return &f;
  return nullptr;
}

std::vector<const FieldSpec*> DiagConfig::fieldsForFile(
    const std::string& name) const {
  std::vector<const FieldSpec*> out;
  for (const auto& f : fields)
    if (f.file_name == name) out.push_back(&f);
  return out;
}

std::optional<DiagConfig> parseClassicDiagTable(const std::string& path,
                                                std::string* error) {
  auto fail = [&](int lineno, const std::string& what) {
    if (error) {
      std::ostringstream os;
      os << path << " line " << lineno << ": " << what;
      *error = os.str();
    }
    return std::nullopt;
  };

  std::ifstream in(path);
  if (!in) return fail(0, "cannot open");

  DiagConfig cfg;
  std::string line;
  int lineno = 0;
  int header = 0;  // 0: need title, 1: need base date, 2: body

  while (std::getline(in, line)) {
    ++lineno;
    auto toks = tokenize(line);
    if (toks.empty()) continue;

    if (header == 0) {  // title
      cfg.title = toks[0];
      header = 1;
      continue;
    }
    if (header == 1) {  // base date: 6 ints (whitespace separated, one token)
      std::istringstream ds(toks.size() == 1 ? toks[0] : line);
      DateFields& d = cfg.base_date;
      if (!(ds >> d.year >> d.month >> d.day >> d.hour >> d.minute >> d.second))
        return fail(lineno, "malformed base date");
      header = 2;
      continue;
    }

    // Body: a field line has 8 tokens with tok[1] non-numeric; a file line
    // has >= 6 with tok[1] an integer (output_freq). Distinguish on that.
    int dummy;
    const bool is_file_line = toks.size() >= 6 && parseInt(toks[1], &dummy);
    if (is_file_line) {
      FileSpec f;
      f.name = toks[0];
      if (!parseInt(toks[1], &f.output_freq))
        return fail(lineno, "bad output_freq");
      if (!parseTimeUnit(lower(toks[2]), &f.output_freq_units))
        return fail(lineno, "bad output_freq units '" + toks[2] + "'");
      if (!parseInt(toks[3], &f.file_format) || f.file_format != 1)
        return fail(lineno, "only file_format 1 (netCDF) is supported");
      if (!parseTimeUnit(lower(toks[4]), &f.time_axis_units))
        return fail(lineno, "bad time axis units '" + toks[4] + "'");
      f.time_axis_name = toks[5];
      if (toks.size() >= 8) {
        if (!parseInt(toks[6], &f.new_file_freq))
          return fail(lineno, "bad new_file_freq");
        if (!parseTimeUnit(lower(toks[7]), &f.new_file_freq_units))
          return fail(lineno, "bad new_file_freq units");
      }
      if (toks.size() >= 9) f.start_time = toks[8];
      if (toks.size() >= 11) {
        if (!parseInt(toks[9], &f.file_duration))
          return fail(lineno, "bad file_duration");
        if (!parseTimeUnit(lower(toks[10]), &f.file_duration_units))
          return fail(lineno, "bad file_duration units");
      }
      cfg.files.push_back(std::move(f));
      continue;
    }

    if (toks.size() < 8)
      return fail(lineno, "expected a file or field line (got " +
                              std::to_string(toks.size()) + " tokens)");
    FieldSpec fd;
    fd.module_name = toks[0];
    fd.field_name = toks[1];
    fd.output_name = toks[2];
    fd.file_name = toks[3];
    fd.time_sampling = toks[4];
    if (!parseReduction(toks[5], &fd.reduction, &fd.pow_exponent,
                        &fd.diurnal_samples))
      return fail(lineno, "unrecognized reduction '" + toks[5] + "'");
    fd.region = toks[6];
    if (lower(fd.region) != "none")
      return fail(lineno, "regional output is not supported (region '" +
                              fd.region + "')");
    if (!parseInt(toks[7], &fd.packing) || fd.packing < 1 || fd.packing > 2)
      return fail(lineno, "unsupported packing '" + toks[7] + "'");
    cfg.fields.push_back(std::move(fd));
  }

  if (header < 2) return fail(lineno, "missing title/base-date header");

  // Validate field->file references (FMS FATALs on unknown files).
  for (const auto& fd : cfg.fields) {
    if (fd.file_name == "null") continue;
    if (!cfg.findFile(fd.file_name))
      return fail(0, "field '" + fd.field_name + "' names unknown file '" +
                         fd.file_name + "'");
  }
  return cfg;
}

}  // namespace Diag
}  // namespace TIM
