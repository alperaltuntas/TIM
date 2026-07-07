#pragma once
// TIM::IO::IoContext — one component instance's I/O world (prototype design
// pass). Owns the IoSystem (built on an explicit communicator), the domain
// registry, the held-open read-file cache, and the write-file handle table.
// Ensemble runs: one IoContext per member, each on its member communicator.
// No singletons anywhere in the C++ layer; the bind(C) adapter owns exactly
// one context per component, created by tim_io_init and destroyed by
// tim_io_finalize (which must run before MPI_Finalize).

#include "../core/tim_domain.hpp"
#include "tim_file.hpp"
#include "tim_iosystem.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace TIM {
namespace IO {

class IoContext {
 public:
  explicit IoContext(MPI_Comm comm)
      : sys_(std::make_unique<IoSystem>(comm)) {}
  IoContext(MPI_Comm comm, const IoSystem::Options& opts)
      : sys_(std::make_unique<IoSystem>(comm, opts)) {}
  ~IoContext() {
    // Files (borrowing the IoSystem) must die before it does.
    read_cache_.clear();
    write_files_.clear();
    sys_.reset();
  }
  IoContext(const IoContext&) = delete;
  IoContext& operator=(const IoContext&) = delete;

  IoSystem& sys() { return *sys_; }

  int registerDomain(const Decomp2D& d) {
    domains_.push_back(d);
    return (int)domains_.size() - 1;
  }
  const Decomp2D& domain(int handle) const { return domains_[(size_t)handle]; }

  // Held-open read files (opening dominates read cost at scale).
  File* readFile(const std::string& path) {
    auto it = read_cache_.find(path);
    if (it != read_cache_.end()) return &it->second;
    auto f = File::openForRead(*sys_, path);
    if (!f) return nullptr;
    return &read_cache_.emplace(path, std::move(*f)).first->second;
  }

  int addWriteFile(File&& f) {
    write_files_.emplace_back(std::move(f));
    return (int)write_files_.size() - 1;
  }
  File& writeFile(int fh) { return *write_files_[(size_t)fh]; }
  void closeWriteFile(int fh) {
    write_files_[(size_t)fh]->close();
    write_files_[(size_t)fh].reset();
  }

 private:
  std::unique_ptr<IoSystem> sys_;
  std::vector<Decomp2D> domains_;
  std::map<std::string, File> read_cache_;
  std::vector<std::optional<File>> write_files_;
};

}  // namespace IO
}  // namespace TIM
