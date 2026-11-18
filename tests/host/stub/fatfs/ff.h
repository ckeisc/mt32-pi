// Host stub for <fatfs/ff.h>: maps the engine's "SD:/roms/yamaha"
// tree onto ~/workspace/libmu50/roms/yamaha via POSIX calls.
// Only the API subset mu50synth.cpp uses is implemented.
#ifndef ENGSTUB_FATFS_FF_H
#define ENGSTUB_FATFS_FF_H
#include <cstdio>
#include <cstdlib>
#include <cstring>
typedef unsigned char BYTE;
typedef unsigned int UINT;
typedef std::uint32_t FSIZE_t;
typedef int FRESULT;
#define FR_OK 0
#define FR_NO_FILE 4
#define FR_DISK_ERR 1
#define FA_READ 0x01
#define AM_DIR 0x10
#define AM_HID 0x02
#define AM_SYS 0x04
struct FILINFO {
  char fname[256];
  BYTE fattrib;
  FSIZE_t fsize;
};
struct DIR {
  void *handle;
};
struct FIL {
  FILE *fp;
};
extern "C" void *ffdir_open(const char *dirpath);
extern "C" int ffdir_next(void *handle, char *name_out, size_t name_cap,
                           unsigned *size_out);
extern "C" void ffdir_close(void *handle);
namespace ffstub {
inline bool translate(const char *pPath, char *pOut, size_t nOut) {
  // Only the SD disk is backed by the local ROM tree.
  if (std::strncmp(pPath, "SD:/", 4) != 0) return false;
  const char *home = std::getenv("HOME");
  if (!home) return false;
  const char *base = std::strrchr(pPath, '/');
  std::snprintf(pOut, nOut, "%s/workspace/libmu50/roms/yamaha/%s",
                home, base ? base + 1 : pPath);
  return true;
}
inline bool fill_next(DIR *pDir, FILINFO *pInfo) {
  if (!pDir->handle) { pInfo->fname[0] = 0; return false; }
  unsigned size = 0;
  if (ffdir_next(pDir->handle, pInfo->fname, sizeof pInfo->fname, &size)) {
    pInfo->fattrib = 0;
    pInfo->fsize = static_cast<FSIZE_t>(size);
    return true;
  }
  pInfo->fname[0] = 0;
  return false;
}
} // namespace ffstub
inline FRESULT f_findfirst(DIR *pDir, FILINFO *pInfo, const char *pPath, const char *) {
  // pPath is "SD:/roms/yamaha" (directory, no trailing file).
  pDir->handle = nullptr;
  if (std::strncmp(pPath, "SD:/", 4) != 0) { pInfo->fname[0] = 0; return FR_OK; }
  const char *home = std::getenv("HOME");
  if (!home) { pInfo->fname[0] = 0; return FR_OK; }
  char local[1024];
  std::snprintf(local, sizeof local, "%s/workspace/libmu50/roms/yamaha", home);
  pDir->handle = ffdir_open(local);
  if (!pDir->handle) { pInfo->fname[0] = 0; return FR_OK; }
  ffstub::fill_next(pDir, pInfo);
  return FR_OK;
}
inline FRESULT f_findnext(DIR *pDir, FILINFO *pInfo) {
  ffstub::fill_next(pDir, pInfo);
  return FR_OK;
}
inline FRESULT f_closedir(DIR *pDir) {
  if (pDir->handle) { ffdir_close(pDir->handle); pDir->handle = nullptr; }
  return FR_OK;
}
inline FRESULT f_open(FIL *pFile, const char *pPath, BYTE) {
  char local[1024];
  if (!ffstub::translate(pPath, local, sizeof local)) return FR_NO_FILE;
  pFile->fp = std::fopen(local, "rb");
  return pFile->fp ? FR_OK : FR_NO_FILE;
}
inline FRESULT f_read(FIL *pFile, void *pBuffer, UINT nBytes, UINT *pRead) {
  size_t n = std::fread(pBuffer, 1, nBytes, pFile->fp);
  if (pRead) *pRead = static_cast<UINT>(n);
  return FR_OK;
}
inline FRESULT f_close(FIL *pFile) {
  if (pFile->fp) std::fclose(pFile->fp);
  pFile->fp = nullptr;
  return FR_OK;
}
#endif
