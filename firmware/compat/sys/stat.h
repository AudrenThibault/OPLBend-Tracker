// stat et mkdir sur le systeme de fichiers en flash du FM-1 (fm1_fs.c).
#pragma once
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
#ifndef S_IFMT
#define S_IFMT  0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif
struct stat { unsigned st_mode; long st_size; long st_mtime; };
int stat(const char *chemin, struct stat *st);
int mkdir(const char *chemin, unsigned mode);
#ifdef __cplusplus
}
#endif
