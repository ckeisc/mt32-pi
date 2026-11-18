// POSIX directory scan behind a non-colliding interface, so the
// FatFs stub header never includes <dirent.h> (whose DIR typedef
// would collide with FatFs's DIR struct).
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>

namespace {
struct Handle {
  ::DIR *dir;
  char path[1024];
};
} // namespace

extern "C" void *ffdir_open(const char *dirpath) {
  ::DIR *d = ::opendir(dirpath);
  if (!d) return nullptr;
  Handle *h = new Handle;
  h->dir = d;
  std::strncpy(h->path, dirpath, sizeof h->path - 1);
  h->path[sizeof h->path - 1] = 0;
  return h;
}

extern "C" int ffdir_next(void *handle, char *name_out, size_t name_cap,
                           unsigned *size_out) {
  Handle *h = static_cast<Handle *>(handle);
  struct dirent *e;
  while ((e = ::readdir(h->dir)) != nullptr) {
    if (e->d_name[0] == '.') continue;
    char full[1536];
    std::snprintf(full, sizeof full, "%s/%s", h->path, e->d_name);
    struct stat st;
    if (::stat(full, &st) != 0) continue;
    if (!S_ISREG(st.st_mode)) continue;
    std::strncpy(name_out, e->d_name, name_cap - 1);
    name_out[name_cap - 1] = 0;
    *size_out = static_cast<unsigned>(st.st_size);
    return 1;
  }
  return 0;
}

extern "C" void ffdir_close(void *handle) {
  Handle *h = static_cast<Handle *>(handle);
  ::closedir(h->dir);
  delete h;
}
