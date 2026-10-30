// ============================================================================
//  Enregistrement d'un morceau : un .A2M (AdLib Tracker 2, version 14) et, s'il
//  y a quelque chose qu'AT2 ne connait pas, un .OPS a cote.
//
//  Le .A2M : chaque ligne du SONG devient un pattern ; pour chaque piste OPL3
//  (01-18), son chain est deroule ligne par ligne (la transposition du chain
//  est appliquee aux notes, AT2 n'en a pas). Les patterns identiques ne sont
//  ecrits qu'une fois. Les effets FX1/FX2 partent tels quels. Ne partent PAS :
//  la colonne CMD (LSDJ), VEL, FX3/FX4, les tables, les pistes 19 et plus, et
//  toute case jouee par un instrument SAMPLE (ecrite vide : AT2 n'y voit
//  rien). La longueur des patterns (PATTERN LEN) est celle du morceau.
//  Un morceau venu d'AT2 repart du bloc « songdata » d'origine : tout ce que le
//  tracker ne represente pas (tables d'arpege, de vibrato...) est conserve.
//
//  Le .OPS : le morceau COMPLET tel que le tracker le connait (sa sauvegarde
//  interne, samples compris), plus ce qu'elle n'enregistre pas (macros FM,
//  disto, longueur des patterns), et l'empreinte (CRC32) du .A2M ecrit avec
//  lui. A l'ouverture, si le .A2M n'a pas change, c'est le .OPS qui fait foi ;
//  s'il a ete modifie dans AT2 entre-temps, le .A2M seul est importe.
//
//  La compression LZH et le CRC32 sont ceux d'Amber OPL3 Tracker.
// ============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

extern "C" {
#include "CustomReplayer/md_replayer.h"
#include "lzh_compress.h"
#include "a2m_import.h"
}

static const uint32_t kTailleSongdata = 1138338;
static const uint32_t kNoms = 86, kInstruments = 11051, kMacros = 14621,
                      kColonnesOff = 1130042, kOrdre = 1124381, kTempo = 1124509,
                      kSpeed = 1124510, kLongueur = 1124512, kPistes = 1124514,
                      kMacroSpeedup = 1124515, kLignesParTemps = 1138335,
                      kFinetuneTempo = 1138336;
static const int kPistesA2M = 20, kLignesA2M = 256;
static const uint32_t kTaillePattern = kPistesA2M * kLignesA2M * 6;

static void ecrit16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void ecrit32(uint8_t *p, uint32_t v) {
  p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = v >> 24;
}
static uint32_t lit32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void chainePascal(uint8_t *dest, const char *src) {
  memset(dest, 0, 43);
  int n = (int)strlen(src);
  if (n > 42) n = 42;
  dest[0] = (uint8_t)n;
  memcpy(dest + 1, src, n);
}
static void message(char *msg, int cap, const char *t) { if (msg && cap > 0) snprintf(msg, cap, "%s", t); }

static bool estOpl(const md_module_t *m, int ins) {
  return ins >= 1 && ins <= MD_MAX_INSTRUMENTS && m->instruments[ins - 1].kind == MD_INSTR_KIND_OPL;
}

// Le H de fin de pattern pose par l'import (ligne `longueur` du pattern) n'est
// pas une donnee en plus : l'import le refait a partir de la longueur.
static bool estHopDeFin(const md_phrase_row_t &pr, int ligneAbsolue, int longueur) {
  return pr.cmd != MD_EMPTY && md_table_cmd_kind(pr.cmd) == MD_CMD_HOP &&
         pr.cmdval == 0 && ligneAbsolue == longueur;
}

// ── Les lignes de SONG jouees : jusqu'a la premiere ligne vide partout ──────
static int nbLignesSong(const md_module_t *m, int pistes) {
  int n = 0;
  while (n < MD_SONG_ROWS && n < 128) {
    bool quelque = false;
    for (int t = 0; t < pistes && !quelque; t++) quelque = m->song[t][n] != MD_EMPTY;
    if (!quelque) break;
    n++;
  }
  return n;
}

// Longueur des patterns : celle du morceau, sinon le plus long chain.
static int longueurPatterns(const md_module_t *m, int lignes, int pistes) {
  if (m->patt_len > 0) return m->patt_len;
  int lg = MD_ROWS_PER_PHRASE;
  for (int k = 0; k < lignes; k++)
    for (int t = 0; t < pistes; t++) {
      const uint8_t ch = m->song[t][k];
      if (ch == MD_EMPTY) continue;
      for (int r = MD_ROWS_PER_CHAIN - 1; r >= 0; r--)
        if (m->chains[ch % MD_MAX_CHAINS].rows[r].phrase != MD_EMPTY) {
          if ((r + 1) * MD_ROWS_PER_PHRASE > lg) lg = (r + 1) * MD_ROWS_PER_PHRASE;
          break;
        }
    }
  return lg > kLignesA2M ? kLignesA2M : lg;
}

// La case (piste t, ligne r) du pattern de la ligne de SONG k ; faux si vide.
static bool caseDe(const md_module_t *m, int t, int k, int r, md_phrase_row_t *pr, int *tsp) {
  const uint8_t ch = m->song[t][k];
  if (ch == MD_EMPTY) return false;
  const md_chain_row_t &cr = m->chains[ch % MD_MAX_CHAINS].rows[r / MD_ROWS_PER_PHRASE];
  if (cr.phrase == MD_EMPTY) return false;
  *pr = m->phrases[cr.phrase % MD_MAX_PHRASES].rows[r % MD_ROWS_PER_PHRASE];
  *tsp = cr.transpose;
  return true;
}

// ── Faut-il un .OPS ? ───────────────────────────────────────────────────────
static bool besoinOps(const md_module_t *m) {
  if (m->num_channels > MD_PISTES_OPL) return true;
  if (m->pcm_used > 0) return true;     // des samples charges, meme pas encore places
  const int lignes = nbLignesSong(m, m->num_channels);
  const int lg = longueurPatterns(m, lignes, MD_PISTES_OPL);
  for (int k = 0; k < lignes; k++)
    for (int t = 0; t < m->num_channels; t++)
      for (int r = 0; r < MD_ROWS_PER_CHAIN * MD_ROWS_PER_PHRASE; r++) {
        md_phrase_row_t pr; int tsp;
        if (!caseDe(m, t, k, r, &pr, &tsp)) continue;
        const bool horsPattern = r >= lg;
        if (pr.instr && !estOpl(m, pr.instr)) return true;               // un sample
        if (pr.cmd != MD_EMPTY && !estHopDeFin(pr, r, lg)) return true;  // LSDJ
        if (pr.vel != MD_EMPTY) return true;
        if (pr.cc[1] != MD_EMPTY || pr.cc[2] != MD_EMPTY) return true;  // FX3, FX4
        if (horsPattern && (pr.note || pr.instr || pr.mdcmd != MD_EMPTY || pr.cc[0] != MD_EMPTY))
          return true;                                                   // apres la fin
        if (pr.note && pr.note != MD_EMPTY && (int)pr.note + tsp > 96) return true;
        if (pr.instr && estOpl(m, pr.instr) && m->instruments[pr.instr - 1].table != MD_EMPTY)
          return true;                                                   // table LSDJ
      }
  return false;
}

// ── Le .A2M ─────────────────────────────────────────────────────────────────
static bool ecritA2M(const char *chemin, uint32_t *crcFichier, char *msg, int cap) {
  md_module_t *m = md_replayer_get_module();
  const int pistes = m->num_channels < MD_PISTES_OPL ? m->num_channels : MD_PISTES_OPL;
  const int lignes = nbLignesSong(m, pistes);
  if (lignes == 0) { message(msg, cap, "NOTHING TO SAVE"); return false; }
  const int lg = longueurPatterns(m, lignes, pistes);

  // Les patterns, chacun une seule fois.
  std::vector<std::vector<uint8_t>> patterns;
  std::map<std::vector<uint8_t>, int> vus;
  uint8_t ordre[128];
  memset(ordre, 0, sizeof ordre);
  for (int k = 0; k < lignes; k++) {
    std::vector<uint8_t> p(kTaillePattern, 0);
    for (int t = 0; t < pistes; t++)
      for (int r = 0; r < lg; r++) {
        md_phrase_row_t pr; int tsp;
        if (!caseDe(m, t, k, r, &pr, &tsp)) continue;
        if (pr.instr && !estOpl(m, pr.instr)) continue;   // un sample : case vide
        uint8_t *e = &p[(t * kLignesA2M + r) * 6];
        if (pr.note == MD_EMPTY) e[0] = 0xFF;             // key off, comme AT2
        else if (pr.note) {
          int n = pr.note + tsp;
          e[0] = (uint8_t)(n < 1 ? 1 : (n > 96 ? 96 : n));
        }
        e[1] = pr.instr;
        if (pr.mdcmd != MD_EMPTY) { e[2] = pr.mdcmd; e[3] = pr.mdval; }
        if (pr.cc[0] != MD_EMPTY) { e[4] = pr.cc[0]; e[5] = pr.ccval[0]; }
      }
    auto vu = vus.find(p);
    int idx;
    if (vu != vus.end()) idx = vu->second;
    else {
      if (patterns.size() >= 128) { message(msg, cap, "TOO MANY PATTERNS"); return false; }
      idx = (int)patterns.size();
      vus[p] = idx;
      patterns.push_back(p);
    }
    ordre[k] = (uint8_t)idx;
  }
  if (lignes < 128) ordre[lignes] = 0x80;   // reboucle sur la premiere position

  // Le bloc « songdata » : celui d'origine si le morceau vient d'AT2.
  std::vector<uint8_t> sd(kTailleSongdata, 0);
  const uint8_t *origine = a2m_songdata_origine();
  if (origine) memcpy(sd.data(), origine, kTailleSongdata);
  chainePascal(&sd[0], m->songname);
  chainePascal(&sd[43], m->composer);
  for (int i = 0; i < MD_MAX_INSTRUMENTS; i++) {
    const md_instr_t *ins = &m->instruments[i];
    uint8_t *s = &sd[kInstruments + i * 14];
    char defaut[16];
    snprintf(defaut, sizeof defaut, "Instrument %02d", i + 1);
    if (strcmp(m->instr_names[i], defaut) == 0 && !origine) memset(&sd[kNoms + i * 43], 0, 43);
    else chainePascal(&sd[kNoms + i * 43], m->instr_names[i]);
    if (ins->kind == MD_INSTR_KIND_OPL) {
      memcpy(s, MD_OPL_FM(ins), 11);
      s[11] = ins->panning < 85 ? 1 : (ins->panning > 170 ? 2 : 0);
      s[12] = (uint8_t)ins->fine_tune;
    } else {
      memset(s, 0, 13);   // un sample n'a pas d'instrument OPL3
    }
    md_macro_fm_t mac; uint32_t off = 0;
    if (ins->kind == MD_INSTR_KIND_OPL && md_replayer_get_instr_macro(i + 1, &mac, &off)) {
      memcpy(&sd[kMacros + i * sizeof(md_macro_fm_t)], &mac, sizeof mac);
      for (int b = 0; b < 28; b++) sd[kColonnesOff + i * 28 + b] = (off >> b) & 1;
    } else {
      memset(&sd[kMacros + i * sizeof(md_macro_fm_t)], 0, sizeof(md_macro_fm_t));
      memset(&sd[kColonnesOff + i * 28], 0, 28);
    }
  }
  memcpy(&sd[kOrdre], ordre, 128);
  sd[kTempo] = m->initial_tempo;
  sd[kSpeed] = m->initial_speed;
  ecrit16(&sd[kLongueur], (uint16_t)lg);
  if (!origine || sd[kPistes] < MD_PISTES_OPL) sd[kPistes] = MD_PISTES_OPL;
  ecrit16(&sd[kMacroSpeedup], m->macro_speedup ? m->macro_speedup : 1);
  sd[kLignesParTemps] = m->bpm_rows_per_beat;
  ecrit16(&sd[kFinetuneTempo], (uint16_t)m->bpm_tempo_finetune);

  // Compression, comme Amber : songdata, puis les patterns par huit.
  a2m_crc32_init();
  std::vector<uint8_t> sdC(kTailleSongdata + 65536);
  const uint32_t lgSd = a2m_lzh_compress(sd.data(), sdC.data(), kTailleSongdata, false);
  const int nbPatterns = (int)patterns.size();
  const int nbBlocs = (nbPatterns + 7) / 8;
  std::vector<std::vector<uint8_t>> blocs(nbBlocs);
  uint32_t lgBlocs[16];
  memset(lgBlocs, 0, sizeof lgBlocs);
  std::vector<uint8_t> brut(8 * kTaillePattern);
  for (int b = 0; b < nbBlocs; b++) {
    std::fill(brut.begin(), brut.end(), 0);
    for (int p = 0; p < 8 && b * 8 + p < nbPatterns; p++)
      memcpy(&brut[p * kTaillePattern], patterns[b * 8 + p].data(), kTaillePattern);
    blocs[b].resize(8 * kTaillePattern + 65536);
    lgBlocs[b] = a2m_lzh_compress(brut.data(), blocs[b].data(), 8 * kTaillePattern, false);
  }
  uint32_t crc = 0xFFFFFFFFu;
  crc = a2m_crc32(sdC.data(), lgSd, crc);
  for (int b = 0; b < nbBlocs; b++) crc = a2m_crc32(blocs[b].data(), lgBlocs[b], crc);
  { uint8_t l2[2] = { (uint8_t)(lgSd & 0xFF), (uint8_t)((lgSd >> 8) & 0xFF) }; crc = a2m_crc32(l2, 2, crc); }
  for (int b = 0; b < 16; b++) {
    uint8_t l2[2] = { (uint8_t)(lgBlocs[b] & 0xFF), (uint8_t)((lgBlocs[b] >> 8) & 0xFF) };
    crc = a2m_crc32(l2, 2, crc);
  }

  std::vector<uint8_t> f(84, 0);
  memcpy(f.data(), "_A2module_", 10);
  ecrit32(&f[10], crc);
  f[14] = 14;
  f[15] = (uint8_t)nbPatterns;
  ecrit32(&f[16], lgSd);
  for (int b = 0; b < 16; b++) ecrit32(&f[20 + b * 4], lgBlocs[b]);
  f.insert(f.end(), sdC.begin(), sdC.begin() + lgSd);
  for (int b = 0; b < nbBlocs; b++) f.insert(f.end(), blocs[b].begin(), blocs[b].begin() + lgBlocs[b]);

  FILE *o = fopen(chemin, "wb");
  if (!o) { message(msg, cap, "CANNOT WRITE A2M"); return false; }
  const bool ok = fwrite(f.data(), 1, f.size(), o) == f.size();
  fclose(o);
  if (!ok) { message(msg, cap, "CANNOT WRITE A2M"); return false; }
  *crcFichier = a2m_crc32(f.data(), (uint32_t)f.size(), 0xFFFFFFFFu);
  return true;
}

// ── Le .OPS ─────────────────────────────────────────────────────────────────
static bool ecritOps(const char *chemin, uint32_t crcA2M) {
  md_module_t *m = md_replayer_get_module();
  uint32_t lgBloc = 0;
  uint8_t *bloc = md_replayer_save_mem(&lgBloc);
  if (!bloc) return false;
  std::vector<uint8_t> f;
  auto u8 = [&](uint8_t v) { f.push_back(v); };
  auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) f.push_back((v >> (8 * i)) & 0xFF); };
  f.insert(f.end(), {'O', 'P', 'S', '1'});
  u32(crcA2M);
  u32(m->patt_len);
  // Le bloc du moteur, suivi des donnees des samples : md_replayer_load_mem
  // relit les deux d'un coup.
  uint32_t lgPcm = 0;
  const uint8_t *pcm = md_replayer_banque(&lgPcm);
  u32(lgBloc + lgPcm);
  f.insert(f.end(), bloc, bloc + lgBloc);
  free(bloc);
  if (lgPcm) f.insert(f.end(), pcm, pcm + lgPcm);
  for (int i = 0; i < MD_MAX_INSTRUMENTS; i++) u8(MD_DISTO(&m->instruments[i]));
  std::vector<int> avecMacro;
  for (int i = 1; i <= MD_MAX_INSTRUMENTS; i++) if (md_replayer_instr_a_macro(i)) avecMacro.push_back(i);
  u8((uint8_t)avecMacro.size());
  for (int i : avecMacro) {
    md_macro_fm_t mac; uint32_t off = 0;
    md_replayer_get_instr_macro(i, &mac, &off);
    u8((uint8_t)i); u32(off);
    f.insert(f.end(), (uint8_t *)&mac, (uint8_t *)&mac + sizeof mac);
  }
  FILE *o = fopen(chemin, "wb");
  if (!o) return false;
  const bool ok = fwrite(f.data(), 1, f.size(), o) == f.size();
  fclose(o);
  return ok;
}

static void cheminOps(const char *cheminA2M, char *dest, int cap) {
  snprintf(dest, cap, "%s", cheminA2M);
  char *point = strrchr(dest, '.');
  if (point && (int)(point - dest) + 4 < cap) strcpy(point, ".OPS");
}

extern "C" bool md_enregistre_a2m(const char *cheminA2M, char *msg, int cap) {
  if (!md_replayer_get_module()) { message(msg, cap, "NO SONG"); return false; }
  uint32_t crc = 0;
  if (!ecritA2M(cheminA2M, &crc, msg, cap)) return false;
  char ops[256];
  cheminOps(cheminA2M, ops, sizeof ops);
  if (besoinOps(md_replayer_get_module())) {
    if (!ecritOps(ops, crc)) { message(msg, cap, "A2M SAVED, OPS FAILED"); return false; }
    message(msg, cap, "SAVED .A2M + .OPS");
  } else {
    remove(ops);   // plus rien en dehors d'AT2 : l'ancien .OPS mentirait
    message(msg, cap, "SAVED .A2M");
  }
  return true;
}

// ── A l'ouverture : le .OPS, s'il correspond toujours au .A2M ──────────────
extern "C" bool md_charge_ops(const char *cheminA2M, char *msg, int cap) {
  char ops[256];
  cheminOps(cheminA2M, ops, sizeof ops);
  FILE *f = fopen(ops, "rb");
  if (!f) {   // .ops en minuscules ?
    char *point = strrchr(ops, '.');
    if (point) strcpy(point, ".ops");
    f = fopen(ops, "rb");
  }
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> d(n > 0 ? n : 0);
  const bool lu = n > 16 && fread(d.data(), 1, n, f) == (size_t)n;
  fclose(f);
  if (!lu || memcmp(d.data(), "OPS1", 4) != 0) return false;

  // L'empreinte du .A2M : a-t-il change depuis (dans AT2) ?
  FILE *a = fopen(cheminA2M, "rb");
  if (!a) return false;
  fseek(a, 0, SEEK_END);
  const long na = ftell(a);
  fseek(a, 0, SEEK_SET);
  std::vector<uint8_t> da(na > 0 ? na : 0);
  const bool luA = na > 0 && fread(da.data(), 1, na, a) == (size_t)na;
  fclose(a);
  if (!luA) return false;
  a2m_crc32_init();
  if (a2m_crc32(da.data(), (uint32_t)na, 0xFFFFFFFFu) != lit32(&d[4])) {
    message(msg, cap, "A2M CHANGED IN AT2: OPS IGNORED");
    return false;
  }

  size_t o = 8;
  const uint32_t pattLen = lit32(&d[o]); o += 4;
  const uint32_t lgBloc = lit32(&d[o]); o += 4;
  if (o + lgBloc + MD_MAX_INSTRUMENTS + 1 > d.size()) return false;
  if (!md_replayer_load_mem(&d[o], lgBloc)) return false;
  o += lgBloc;
  md_replayer_set_patt_len((int)pattLen);
  md_module_t *m = md_replayer_get_module();
  for (int i = 0; i < MD_MAX_INSTRUMENTS; i++) MD_DISTO(&m->instruments[i]) = d[o + i];
  o += MD_MAX_INSTRUMENTS;
  const int nbMacros = d[o++];
  for (int k = 0; k < nbMacros && o + 5 + sizeof(md_macro_fm_t) <= d.size(); k++) {
    const int i = d[o]; const uint32_t off = lit32(&d[o + 1]); o += 5;
    md_macro_fm_t mac;
    memcpy(&mac, &d[o], sizeof mac); o += sizeof mac;
    md_replayer_set_instr_macro(i, &mac, off);
  }
  message(msg, cap, "A2M + OPS LOADED");
  return true;
}
