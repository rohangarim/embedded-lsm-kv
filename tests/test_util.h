#pragma once

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace lsm {
namespace testing_support {

// Recursively removes a directory tree. Tests create real files -- an LSM tree
// with the filesystem stubbed out would not be testing the interesting part.
inline void RemoveTree(const std::string& path) {
  if (DIR* dir = ::opendir(path.c_str())) {
    while (struct dirent* entry = ::readdir(dir)) {
      const std::string name = entry->d_name;
      if (name == "." || name == "..") continue;
      const std::string child = path + "/" + name;
      struct stat st;
      if (::stat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        RemoveTree(child);
      } else {
        ::unlink(child.c_str());
      }
    }
    ::closedir(dir);
  }
  ::rmdir(path.c_str());
}

// A fresh directory under the system temp dir, deleted on scope exit.
class ScopedTempDir {
 public:
  explicit ScopedTempDir(const std::string& label) {
    const char* base = std::getenv("TMPDIR");
    std::string prefix = base != nullptr ? std::string(base) : std::string("/tmp/");
    if (prefix.back() != '/') prefix.push_back('/');
    char buf[64];
    std::snprintf(buf, sizeof(buf), "lsmtest_%s_%d_%p", label.c_str(), ::getpid(),
                  static_cast<void*>(this));
    path_ = prefix + buf;
    RemoveTree(path_);
    ::mkdir(path_.c_str(), 0755);
  }
  ~ScopedTempDir() { RemoveTree(path_); }

  ScopedTempDir(const ScopedTempDir&) = delete;
  ScopedTempDir& operator=(const ScopedTempDir&) = delete;

  const std::string& path() const { return path_; }
  std::string File(const std::string& name) const { return path_ + "/" + name; }

 private:
  std::string path_;
};

// Zero-padded so lexicographic order matches numeric order -- the tests lean on
// that when checking scan results.
inline std::string Key(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%08d", i);
  return buf;
}

inline std::string Value(int i, size_t length = 64) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "value%08d:", i);
  std::string out(buf);
  out.resize(length, '.');
  return out;
}

// Deterministic shuffle so a failure reproduces from the seed alone.
inline std::vector<int> ShuffledRange(int n, uint32_t seed) {
  std::vector<int> v(n);
  for (int i = 0; i < n; ++i) v[i] = i;
  std::mt19937 rng(seed);
  std::shuffle(v.begin(), v.end(), rng);
  return v;
}

}  // namespace testing_support
}  // namespace lsm
