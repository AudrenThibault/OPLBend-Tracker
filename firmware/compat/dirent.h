// Les dossiers du systeme de fichiers en flash du FM-1 (fm1_fs.c). Un dossier
// n'est qu'un prefixe de nom ; il « existe » des qu'un fichier y est range ou
// qu'on l'a cree avec mkdir.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8
struct dirent { unsigned char d_type; char d_name[64]; };
typedef struct fm1_dossier DIR;
DIR *opendir(const char *chemin);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);
#ifdef __cplusplus
}
#endif
