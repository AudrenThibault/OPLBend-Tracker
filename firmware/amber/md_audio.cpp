// ============================================================================
//  Lecture d'un fichier son, pour les voies de la DS.
//
//  Un echantillon garde sa FORME D'ORIGINE, comme dans un module XM : 8 bits
//  s'il est en 8 bits, 16 bits sinon, et sa cadence d'enregistrement. Les
//  voies de la DS lisent les deux resolutions a n'importe quelle cadence : il
//  n'y a aucune raison de degrader le son a l'import. Seules deux choses sont
//  ramenees a ce que la console sait jouer :
//    - la stereo devient mono (une voie de la DS est mono) ;
//    - au-dela de 48 kHz on decime par un facteur entier (96 kHz -> 48 kHz) :
//      la sortie de la console tourne a 32 kHz, les aigus en plus ne
//      s'entendraient pas et doubleraient la place prise.
//
//  Le fichier est lu PAR BLOCS et decode directement dans la banque du
//  morceau : la DS Lite n'a que 4 Mo, on ne tient jamais deux copies du son.
//
//  Copyright (C) 2026 Audren Thibault. Tous droits reserves.
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Le melangeur de la DS tourne a 32 768 Hz : au-dela, il n'y a plus rien a
// restituer, et le materiel replierait les aigus faute de filtre.
#define CADENCE_MAX 32000

static uint32_t lit32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static uint16_t lit16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

// Un echantillon du fichier, ramene sur 16 bits signes.
static int32_t echantillon16(const uint8_t *p, int bits, int format) {
  if (format == 3) {                       // IEEE 754 sur 32 bits
    uint32_t u = lit32(p);
    float f; memcpy(&f, &u, 4);
    if (f > 1.0f) f = 1.0f;
    if (f < -1.0f) f = -1.0f;
    return (int32_t)(f * 32767.0f);
  }
  switch (bits) {
  case 8:  return ((int32_t)p[0] - 128) << 8;      // le WAV 8 bits est NON signe
  case 16: return (int16_t)lit16(p);
  case 24: { int32_t v = ((int32_t)p[0] << 8) | ((int32_t)p[1] << 16) |
                         ((int32_t)p[2] << 24);
             return v >> 16; }
  case 32: return (int32_t)lit32(p) >> 16;
  default: return 0;
  }
}

// Decode `chemin` dans `dest` (au plus `capacite` octets).
//   images    nombre d'images ecrites
//   cadence   cadence de l'echantillon, en Hz
//   bits      8 ou 16
//   boucle    debut de boucle en images, -1 si le fichier n'en porte pas
//   note_base note du tracker a laquelle il joue a sa cadence (49 = C-4)
// Rend 0 si tout va bien, 1 si le fichier est illisible, 2 s'il ne tient pas.
extern "C" int md_audio_charge_wav(const char *chemin, uint8_t *dest,
                                   uint32_t capacite, uint32_t *images,
                                   uint32_t *cadence, int *bits,
                                   int32_t *boucle, int *note_base) {
  *images = 0; *cadence = 0; *bits = 8; *boucle = -1; *note_base = 49;
  if (!dest) return 2;
  FILE *f = fopen(chemin, "rb");
  if (!f) return 1;
  fseek(f, 0, SEEK_END);
  const long taille = ftell(f);
  fseek(f, 0, SEEK_SET);

  uint8_t h[12];
  if (taille < 44 || fread(h, 1, 12, f) != 12 ||
      memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
    fclose(f); return 1;
  }

  // ── Les morceaux du fichier ──────────────────────────────────────────────
  int format = 0, voies = 0, bitsSrc = 0; uint32_t hz = 0;
  long posDonnees = -1; uint32_t lgDonnees = 0;
  int32_t boucleDebut = -1; uint32_t boucleFin = 0;
  uint8_t e[8];
  while (fread(e, 1, 8, f) == 8) {
    const uint32_t lg = lit32(e + 4);
    const long debut = ftell(f);
    if (!memcmp(e, "fmt ", 4) && lg >= 16) {
      uint8_t fmt[40];
      const uint32_t n = lg < 40 ? lg : 40;
      if (fread(fmt, 1, n, f) != n) break;
      format  = lit16(fmt);
      voies   = lit16(fmt + 2);
      hz      = lit32(fmt + 4);
      bitsSrc = lit16(fmt + 14);
      // WAVE_FORMAT_EXTENSIBLE : le vrai format est au debut du sous-type.
      if (format == 0xFFFE && n >= 26) format = lit16(fmt + 24);
    } else if (!memcmp(e, "data", 4)) {
      posDonnees = debut;
      lgDonnees = lg;
    } else if (!memcmp(e, "smpl", 4) && lg >= 60) {
      // Le bloc des echantillonneurs : la note de reference, et la boucle.
      uint8_t s[60];
      if (fread(s, 1, 60, f) == 60) {
        const int unite = (int)lit32(s + 12);          // note MIDI
        if (unite >= 12 && unite <= 119) *note_base = unite - 11;
        if (lit32(s + 28) > 0) {                       // au moins une boucle
          boucleDebut = (int32_t)lit32(s + 44);
          boucleFin = lit32(s + 48);
        }
      }
    }
    if (fseek(f, debut + (long)lg + (long)(lg & 1), SEEK_SET) != 0) break;
  }
  if (posDonnees < 0 || voies < 1 || hz == 0 ||
      (format != 1 && format != 3) || (format == 3 && bitsSrc != 32) ||
      (bitsSrc != 8 && bitsSrc != 16 && bitsSrc != 24 && bitsSrc != 32)) {
    fclose(f); return 1;
  }
  if (posDonnees + (long)lgDonnees > taille)
    lgDonnees = (uint32_t)(taille - posDonnees);

  // ── Ce qu'on va produire ────────────────────────────────────────────────
  // ⚠️ POURQUOI ON RAMENE TOUT A 32 kHz AU PLUS.
  // Le melangeur de la DS tourne a 32 768 Hz et ne filtre RIEN : un sample a
  // 44,1 kHz est ramene a cette cadence en sautant des echantillons, ce qui
  // replie ses aigus en parasites metalliques. En faisant la conversion ICI,
  // avec une moyenne sur la fenetre — un vrai filtre passe-bas — on garde le
  // meme son sans les parasites, et il prend moins de place. En dessous de
  // 32 kHz, on ne touche a rien : la cadence d'origine est conservee.
  const int bitsSortie = (format == 1 && bitsSrc == 8) ? 8 : 16;
  const uint32_t octetsSortie = (uint32_t)(bitsSortie / 8);
  const uint32_t cible = (hz > CADENCE_MAX) ? CADENCE_MAX : hz;
  const uint32_t cadre = (uint32_t)(bitsSrc / 8 * voies);
  uint32_t cadres = lgDonnees / cadre;
  // Une boucle qui s'arrete avant la fin : la DS boucle toujours jusqu'au
  // bout des donnees, on coupe donc ce qui suit la fin de boucle.
  if (boucleDebut >= 0 && boucleFin > (uint32_t)boucleDebut && boucleFin < cadres)
    cadres = boucleFin + 1;
  if (cadres == 0) { fclose(f); return 1; }
  const uint32_t nb = (uint32_t)(((uint64_t)cadres * cible) / hz);
  if (nb == 0) { fclose(f); return 1; }
  if (nb > capacite / octetsSortie) { fclose(f); return 2; }

  // Combien d'images d'entree pour une image de sortie, en virgule fixe 16.16.
  // Vaut 65 536 quand on ne reechantillonne pas.
  const uint32_t pas = (uint32_t)(((uint64_t)hz << 16) / cible);
  // La moyenne des voies, sans division par echantillon : l'ARM9 n'a pas
  // d'instruction de division.
  const int32_t recipVoies = (int32_t)(65536 / (uint32_t)voies);

#ifdef FM1
  static uint8_t tampon[2048];   // (FM-1 : lu par plus petits morceaux ; la memoire vive manquait aux gros .A2M)
#else
  static uint8_t tampon[8192];
#endif
  const uint32_t parBloc = sizeof(tampon) / cadre;   // images entieres par bloc
  if (parBloc == 0) { fclose(f); return 1; }
  fseek(f, posDonnees, SEEK_SET);

  uint32_t fait = 0;        // images ecrites
  uint32_t lues = 0;        // images lues
  int32_t acc = 0;          // somme de la fenetre courante
  uint32_t nAcc = 0;        // images dans la fenetre
  uint32_t phase = 0;
  while (lues < cadres && fait < nb) {
    uint32_t n = cadres - lues;
    if (n > parBloc) n = parBloc;
    if (fread(tampon, 1, n * cadre, f) != n * cadre) break;
    const uint8_t *src = tampon;
    for (uint32_t i = 0; i < n && fait < nb; i++) {
      // Les voies sont moyennees : une voie de la DS est mono.
      int32_t somme = 0;
      for (int c = 0; c < voies; c++) {
        somme += echantillon16(src, bitsSrc, format);
        src += bitsSrc / 8;
      }
      const int32_t mono = (voies == 1)
                         ? somme
                         : (int32_t)(((int64_t)somme * recipVoies) >> 16);
      acc += mono;
      nAcc++;
      phase += 65536;
      while (phase >= pas && fait < nb) {
        phase -= pas;
        int32_t v = (nAcc > 1) ? acc / (int32_t)nAcc : acc;
        acc = 0;
        nAcc = 0;
        if (bitsSortie == 8) {
          dest[fait] = (uint8_t)(int8_t)(v >> 8);
        } else {
          if (v > 32767) v = 32767;
          if (v < -32768) v = -32768;
          const uint32_t o = fait * 2;
          dest[o]     = (uint8_t)(v & 0xFF);
          dest[o + 1] = (uint8_t)((v >> 8) & 0xFF);
        }
        fait++;
      }
    }
    lues += n;
  }
  fclose(f);
  if (fait == 0) return 1;

  *images = fait;                 // un fichier tronque donne ce qu'il contient
  *cadence = cible;
  *bits = bitsSortie;
  if (boucleDebut >= 0) {
    const uint32_t bcl = (uint32_t)(((uint64_t)boucleDebut * cible) / hz);
    if (bcl < fait) *boucle = (int32_t)bcl;
  }
  return 0;
}
