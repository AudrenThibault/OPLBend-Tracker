// ============================================================================
//  Import d'un module AdLib Tracker 2 (.A2M, version 14) dans la structure
//  SONG / CHAIN / PHRASE du tracker.
//
//  La lecture du fichier est celle d'Amber OPL3 Tracker (a2m_replayer.c) :
//  en-tete, bloc « songdata » compresse en LZH, puis les patterns par paquets
//  de huit, compresses eux aussi.
//
//  La conversion :
//    - chaque position de l'ordre devient une ligne du SONG ;
//    - pour chaque piste, le pattern devient un CHAIN de phrases de 16 lignes
//      (64 lignes -> 4 phrases). Phrases et chains identiques sont partages,
//      comme on le ferait a la main dans LSDJ ;
//    - les deux effets d'une case vont dans FX1 et FX2, TELS QUELS, Dxx et
//      Bxx compris : le moteur les joue comme AT2. La colonne CMD (LSDJ)
//      reste vide : un morceau venu d'AT2 doit pouvoir y retourner.
//  Les macros FM des instruments sont reprises. Ce que le tracker ne sait pas
//  representer est perdu : tables d'arpege et de vibrato, voies 4
//  operateurs, pistes au-dela de 18.
// ============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

extern "C" {
#include "CustomReplayer/md_replayer.h"
#include "CustomReplayer/md_lock.h"
#include "unlzh.h"
#include "a2m_import.h"
extern md_lock_t replayer_lock;
}

static uint8_t *s_origine = nullptr;   // le songdata du dernier A2M importe
extern "C" const uint8_t *a2m_songdata_origine(void) { return s_origine; }
extern "C" void a2m_oublie_origine(void) { free(s_origine); s_origine = nullptr; }

static uint16_t lit16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t lit32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Emplacements dans le bloc « songdata » d'un A2M v9-14 (Amber, parse_songdata).
static const uint32_t kTailleSongdata = 1138338;
static const uint32_t kNoms = 86, kInstruments = 11051, kOrdre = 1124381,
                      kTempo = 1124509, kSpeed = 1124510, kLongueur = 1124512,
                      kPistes = 1124514, kMacroSpeedup = 1124515,
                      kLignesParTemps = 1138335, kFinetuneTempo = 1138336;
static const uint32_t kMacros = 14621, kColonnesOff = 1130042;
static const int kPistesA2M = 20, kLignesA2M = 256;

struct Evenement { uint8_t note, instr, fx[2][2]; };

// Une note A2M : 1-96 comme le tracker (C-0 = 1). Les notes « fixes » (0x91 et
// plus) sont jouees comme des notes ordinaires ; tout le reste du haut de
// l'echelle est un key off.
static uint8_t convertitNote(uint8_t n) {
  if (n == 0) return 0;
  if (n <= 96) return n;
  if (n >= 0x91 && n <= 0x90 + 96) return (uint8_t)(n - 0x90);
  return MD_EMPTY;
}

// Les textes d'AT2 sont des chaines Pascal : un octet de longueur, puis les
// caracteres (String[42] : 43 octets en tout).
static void chainePascal(char *dest, const uint8_t *src) {
  int n = src[0] > 42 ? 42 : src[0];
  memcpy(dest, src + 1, n);
  dest[n] = 0;
}

static void message(char *msg, int cap, const char *texte) {
  if (msg && cap > 0) snprintf(msg, cap, "%s", texte);
}

extern "C" bool md_importe_a2m(const char *chemin, char *msg, int cap) {
  // ── Le fichier ─────────────────────────────────────────────────────────
  FILE *f = fopen(chemin, "rb");
  if (!f) { message(msg, cap, "FICHIER ILLISIBLE"); return false; }
  fseek(f, 0, SEEK_END);
  const long taille = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> donnees(taille > 0 ? taille : 0);
  const bool lu = taille > 84 && fread(donnees.data(), 1, taille, f) == (size_t)taille;
  fclose(f);
  if (!lu || memcmp(donnees.data(), "_A2module_", 10) != 0) {
    message(msg, cap, "PAS UN FICHIER A2M"); return false;
  }
  if (donnees[14] != 14) { message(msg, cap, "A2M: VERSION 14 SEULEMENT"); return false; }
  const int nbPatterns = donnees[15];
  const uint32_t longSongdata = lit32(&donnees[16]);
  uint32_t longBlocs[16];
  for (int i = 0; i < 16; i++) longBlocs[i] = lit32(&donnees[20 + i * 4]);

  // ── Le bloc songdata ───────────────────────────────────────────────────
  std::vector<uint8_t> sd(kTailleSongdata);
  uint32_t pos = 84;
  if (pos + longSongdata > (uint32_t)taille) { message(msg, cap, "A2M TRONQUE"); return false; }
  LZH_decompress((char *)&donnees[pos], (char *)sd.data(), longSongdata, kTailleSongdata);
  pos += longSongdata;

  // Garde pour l'enregistrement : tout ce qu'on ne represente pas y reste.
  a2m_oublie_origine();
  s_origine = (uint8_t *)malloc(kTailleSongdata);
  if (s_origine) memcpy(s_origine, sd.data(), kTailleSongdata);

  const int tempo = sd[kTempo], speed = sd[kSpeed];
  int longueur = lit16(&sd[kLongueur]);
  if (longueur < 1 || longueur > kLignesA2M) longueur = 64;
  int pistes = sd[kPistes];
  if (pistes < 1 || pistes > kPistesA2M) pistes = 18;
  const int macroSpeedup = lit16(&sd[kMacroSpeedup]);

  // ── Les patterns : huit par bloc compresse ─────────────────────────────
  std::vector<Evenement> pats((size_t)nbPatterns * kPistesA2M * kLignesA2M);
  {
    const uint32_t tailleBloc = 8 * kPistesA2M * kLignesA2M * 6;
    std::vector<uint8_t> brut(tailleBloc);
    for (int b = 0; b < (nbPatterns + 7) / 8; b++) {
      if (longBlocs[b] == 0) continue;
      if (pos + longBlocs[b] > (uint32_t)taille) break;
      std::fill(brut.begin(), brut.end(), 0);
      LZH_decompress((char *)&donnees[pos], (char *)brut.data(), longBlocs[b], tailleBloc);
      pos += longBlocs[b];
      for (int p = 0; p < 8 && b * 8 + p < nbPatterns; p++)
        for (int c = 0; c < kPistesA2M; c++)
          for (int r = 0; r < kLignesA2M; r++) {
            const uint8_t *s = &brut[((p * kPistesA2M + c) * kLignesA2M + r) * 6];
            Evenement &e = pats[((size_t)(b * 8 + p) * kPistesA2M + c) * kLignesA2M + r];
            e.note = s[0]; e.instr = s[1];
            e.fx[0][0] = s[2]; e.fx[0][1] = s[3];
            e.fx[1][0] = s[4]; e.fx[1][1] = s[5];
          }
    }
  }
  auto evenement = [&](int p, int c, int r) -> const Evenement & {
    return pats[((size_t)p * kPistesA2M + c) * kLignesA2M + r];
  };

  // ── L'ordre ────────────────────────────────────────────────────────────
  // Il s'arrete au premier saut (0x80 et plus) : le SONG du tracker reboucle
  // de lui-meme au debut.
  std::vector<int> ordre;
  for (int k = 0; k < 128 && (int)ordre.size() < MD_SONG_ROWS; k++) {
    const uint8_t o = sd[kOrdre + k];
    if (o >= 0x80) break;
    if (o >= nbPatterns) continue;
    ordre.push_back(o);
  }
  if (ordre.empty()) { message(msg, cap, "A2M: ORDRE VIDE"); return false; }

  // Longueur reelle de chaque pattern : un Dxx (pattern break, effet 13)
  // l'arrete a sa ligne.

  // ── Le module ──────────────────────────────────────────────────────────
  md_replayer_stop();
  md_replayer_new_empty();
  md_lock(&replayer_lock);
  md_module_t *m = md_replayer_get_module();
  if (!m) { md_unlock(&replayer_lock); message(msg, cap, "MEMOIRE PLEINE"); return false; }

  chainePascal(m->songname, &sd[0]);
  chainePascal(m->composer, &sd[43]);
  m->initial_tempo = (uint8_t)(tempo ? tempo : 50);
  m->initial_speed = (uint8_t)(speed ? speed : 6);
  m->macro_speedup = (uint16_t)(macroSpeedup ? macroSpeedup : 1);
  m->bpm_rows_per_beat = sd[kLignesParTemps] ? sd[kLignesParTemps] : 4;
  m->bpm_tempo_finetune = (int16_t)lit16(&sd[kFinetuneTempo]);
  m->num_channels = MD_PISTES_OPL;
  m->patt_len = (uint16_t)longueur;   // le moteur s'arrete la, comme AT2

  // Les instruments : onze octets FM, la stereo et le desaccord.
  for (int i = 0; i < MD_MAX_INSTRUMENTS; i++) {
    md_instr_t *ins = &m->instruments[i];
    const uint8_t *s = &sd[kInstruments + i * 14];
    ins->kind = MD_INSTR_KIND_OPL;
    memcpy(MD_OPL_FM(ins), s, MD_OPL_OCTETS);
    ins->panning = s[11] == 1 ? 0 : (s[11] == 2 ? 255 : 128);
    // Le F-TUNE d'AT2, tel quel : il s'ajoute au F-Num, comme dans AT2.
    ins->fine_tune = (int8_t)s[12];
    chainePascal(m->instr_names[i], &sd[kNoms + i * 43]);
  }
  // Les macros FM, et pour chacune ses colonnes desactivees (28 octets, un
  // par colonne, dans l'ordre d'AT2).
  for (int i = 0; i < MD_MAX_INSTRUMENTS; i++) {
    md_macro_fm_t mac;
    memcpy(&mac, &sd[kMacros + i * sizeof(md_macro_fm_t)], sizeof mac);
    uint32_t off = 0;
    for (int b = 0; b < 28; b++)
      if (sd[kColonnesOff + i * 28 + b]) off |= 1u << b;
    md_replayer_set_instr_macro(i + 1, mac.length ? &mac : NULL, off);
  }

  // Phrases et chains, partages quand ils sont identiques.
  std::map<std::vector<uint8_t>, int> phrasesVues, chainsVus;
  int nbPhrases = 0, nbChains = 0;
  bool plein = false;
  const int indiceHop = [] {
    for (int i = 0; i < md_table_cmd_count(); i++)
      if (md_table_cmd_letter(i) == 'H') return i;
    return -1;
  }();
  const int pistesGardees = pistes < MD_PISTES_OPL ? pistes : MD_PISTES_OPL;

  for (size_t k = 0; k < ordre.size() && !plein; k++) {
    const int p = ordre[k];
    // Toutes les lignes du pattern, meme celles apres un Dxx : c'est le
    // moteur qui joue le Dxx, comme AT2, et le morceau doit pouvoir repartir
    // tel quel vers AT2.
    const int lg = longueur;
    const int nbPh = (lg + MD_ROWS_PER_PHRASE - 1) / MD_ROWS_PER_PHRASE;
    for (int c = 0; c < pistesGardees && !plein; c++) {
      std::vector<uint8_t> cleChain;
      for (int ph = 0; ph < nbPh; ph++) {
        md_phrase_t phrase;
        std::vector<uint8_t> cle;
        for (int r = 0; r < MD_ROWS_PER_PHRASE; r++) {
          md_phrase_row_t *pr = &phrase.rows[r];
          memset(pr, 0, sizeof *pr);
          pr->vel = MD_EMPTY; pr->cmd = MD_EMPTY;
          for (int x = 0; x < MD_PHRASE_FX; x++) { *md_fx_type(pr, x) = MD_EMPTY; *md_fx_val(pr, x) = 0; }
          const int ligne = ph * MD_ROWS_PER_PHRASE + r;
          if (ligne < lg) {
            const Evenement &e = evenement(p, c, ligne);
            pr->note = convertitNote(e.note);
            pr->instr = e.instr;
            for (int x = 0; x < 2; x++) {
              const uint8_t t = e.fx[x][0], v = e.fx[x][1];
              if (t == 0 && v == 0) continue;               // case vide
              if (t > MD_EFF_GLOBAL_FSLIDE_DOWN) continue;  // inconnu du moteur
              *md_fx_type(pr, x) = t;
              *md_fx_val(pr, x) = v;
            }
          } else if (ligne == lg && indiceHop >= 0) {
            // Pattern plus court qu'un multiple de 16 lignes (84 dans
            // SEGAGYPT) : un H00 (LSDJ) sur la ligne QUI SUIT la derniere,
            // comme on le ferait a la main. La colonne CMD n'est jamais
            // ecrite dans le .A2M, et la longueur du pattern y est remise :
            // AT2 retrouve son morceau tel quel.
            pr->cmd = (uint8_t)indiceHop;
            pr->cmdval = 0;
          }
          cle.insert(cle.end(), (uint8_t *)pr, (uint8_t *)pr + sizeof *pr);
        }
        int id;
        auto vu = phrasesVues.find(cle);
        if (vu != phrasesVues.end()) id = vu->second;
        else {
          if (nbPhrases >= MD_MAX_PHRASES) { plein = true; break; }
          id = nbPhrases++;
          m->phrases[id] = phrase;
          phrasesVues[cle] = id;
        }
        cleChain.push_back((uint8_t)id);
      }
      if (plein) break;
      int idChain;
      auto vu = chainsVus.find(cleChain);
      if (vu != chainsVus.end()) idChain = vu->second;
      else {
        if (nbChains >= MD_MAX_CHAINS) { plein = true; break; }
        idChain = nbChains++;
        for (size_t r = 0; r < cleChain.size() && r < MD_ROWS_PER_CHAIN; r++) {
          m->chains[idChain].rows[r].phrase = cleChain[r];
          m->chains[idChain].rows[r].transpose = 0;
        }
        chainsVus[cleChain] = idChain;
      }
      m->song[c][k] = (uint8_t)idChain;
    }
  }
  md_unlock(&replayer_lock);

  md_replayer_set_tempo(m->initial_tempo);
  md_replayer_set_speed(m->initial_speed);

  if (plein) {
    snprintf(msg, cap, "A2M TROP GROS: %d CHAINS %d PHRASES", nbChains, nbPhrases);
    return true;   // charge quand meme, jusqu'ou ca tenait
  }
  snprintf(msg, cap, "A2M: %d PATTERNS, %d CHAINS, %d PHRASES",
           (int)ordre.size(), nbChains, nbPhrases);
  return true;
}
