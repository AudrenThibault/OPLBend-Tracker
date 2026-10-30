// ============================================================================
//  stdio pour le FM-1 : la libc de JieLi n'en a pas. Les fichiers vivent dans
//  la flash (fm1_fs.c) ; printf et compagnie sont dans fm1_libc.c.
//  Seuls les formats dont le tracker se sert sont geres : %d %i %u %x %X %c %s
//  %%, avec drapeaux - + espace 0, largeur, precision (.N sur %s) et l / ll / z.
// ============================================================================
#pragma once
#include <stddef.h>
#include <stdarg.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct fm1_fichier FILE;
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ 256
#define FILENAME_MAX 192

extern FILE *const stdin;
extern FILE *const stdout;
extern FILE *const stderr;

FILE *fopen(const char *chemin, const char *mode);
int fclose(FILE *f);
size_t fread(void *p, size_t taille, size_t n, FILE *f);
size_t fwrite(const void *p, size_t taille, size_t n, FILE *f);
int fseek(FILE *f, long position, int origine);
long ftell(FILE *f);
void rewind(FILE *f);
int feof(FILE *f);
int ferror(FILE *f);
int fflush(FILE *f);
int fgetc(FILE *f);
char *fgets(char *s, int n, FILE *f);
int fputc(int c, FILE *f);
int fputs(const char *s, FILE *f);
int puts(const char *s);
int putchar(int c);
int remove(const char *chemin);
int rename(const char *ancien, const char *nouveau);

int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int sprintf(char *s, const char *fmt, ...);
int snprintf(char *s, size_t n, const char *fmt, ...);
int vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
int vsprintf(char *s, const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int sscanf(const char *s, const char *fmt, ...);

#ifdef __cplusplus
}
#endif
