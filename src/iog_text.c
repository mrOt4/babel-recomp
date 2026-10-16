/* Illusion of Gaia text pipeline (see iog_text.h). */

#include "iog_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- text engine command set (identical in every release) ------------- */

enum {
    CMD_END = 0xC0, CMD_ADR = 0xC5, CMD_RET = 0xCA, CMD_N = 0xCB,
    CMD_PRT = 0xCD, CMD_FIN = 0xCF, CMD_JMP = 0xD1, CMD_LU1 = 0xD6,
    CMD_LU2 = 0xD7, CMD_ESC = 0xD8, CMD_LAST = 0xDA,
};
/* Argument sizes in bytes per command $C0-$DA; ESC is delimited by $00. */
static const uint8_t kArgs[CMD_LAST - 0xC0 + 1][2] = {
    [0x00] = {0, 0}, [0x01] = {1, 1}, [0x02] = {1, 0}, [0x03] = {1, 0},
    [0x04] = {0, 0}, [0x05] = {2, 2}, [0x06] = {2, 2}, [0x07] = {1, 1},
    [0x08] = {0, 0}, [0x09] = {1, 0}, [0x0A] = {0, 0}, [0x0B] = {0, 0},
    [0x0C] = {1, 0}, [0x0D] = {3, 0}, [0x0E] = {0, 0}, [0x0F] = {0, 0},
    [0x10] = {0, 0}, [0x11] = {2, 0}, [0x12] = {1, 0}, [0x13] = {0, 0},
    [0x14] = {1, 2}, [0x15] = {1, 0}, [0x16] = {1, 0}, [0x17] = {1, 0},
    [0x18] = {0, 0}, [0x19] = {1, 0}, [0x1A] = {1, 0},
};
/* Commands whose presence and order make up a string's "skeleton". */
static int IsSkeleton(uint8_t c)
{
    switch (c) {
    case 0xC1: case 0xC7: case 0xC2: case 0xC3: case 0xD2: case 0xC5: case 0xC6:
    case 0xD5: case 0xD4: case 0xDA: case 0xCC: case 0xD9: case 0xC9: case 0xD3:
    case 0xC8:
        return 1;
    }
    return 0;
}
static int IsHalt(uint8_t c) { return c == CMD_END || c == CMD_RET || c == CMD_JMP; }
static int IsRef(uint8_t c) { return c == CMD_ADR || c == CMD_PRT || c == CMD_JMP; }

/* ---- tokens ------------------------------------------------------------ */

enum { T_GLYPH, T_CMD, T_MARK };

typedef struct {
    uint8_t type, code;
    uint32_t a[3];             /* raw argument values */
    int ref;                   /* resolved label/table index, -1: numeric */
    uint8_t ref_m;             /* reference is to the target's [::] marker */
    uint8_t blen;
    uint8_t bin[24];           /* ESC payload */
    uint32_t src;              /* source byte offset of the top-level token */
} Tok;

typedef struct {
    Tok *t;
    int n, cap;
} Toks;

static void Push(Toks *v, const Tok *t)
{
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 64;
        v->t = realloc(v->t, (size_t)v->cap * sizeof(Tok));
    }
    v->t[v->n++] = *t;
}

typedef struct {
    const uint8_t *rom;
    size_t size;
    uint32_t engine;
    uint32_t dict[2];          /* [LU1], [LU2] tables (ROM offsets) */
} Rom;

static uint32_t Rd16(const Rom *r, uint32_t off)
{
    return off + 1 < r->size ? (uint32_t)(r->rom[off] | r->rom[off + 1] << 8) : 0;
}

static const uint8_t kEngineSig[] = {
    0x08, 0x0b, 0xda, 0xa9, 0x00, 0x00, 0x5b, 0xae, 0x98, 0x09, 0xe2, 0x20, 0xb9, 0x00, 0x00, 0xc9,
};

static int RomInit(Rom *r, const uint8_t *rom, size_t size)
{
    r->rom = rom;
    r->size = size;
    r->engine = 0;
    for (size_t i = 0; i + sizeof(kEngineSig) <= size; i++)
        if (!memcmp(rom + i, kEngineSig, sizeof(kEngineSig))) {
            r->engine = (uint32_t)i;
            break;
        }
    if (!r->engine)
        return 0;
    /* The command table is $6E past the engine entry; entries $16/$17 are
     * the dictionary handlers: SEP #$20 / LDA #bank / PHA / PLB / REP #$20 /
     * LDA table,Y. */
    for (int k = 0; k < 2; k++) {
        uint32_t h = (r->engine & 0xFF0000) | Rd16(r, r->engine + 0x6E + 2 * (0x16 + k));
        r->dict[k] = 0;
        for (uint32_t i = h; i < h + 40 && i + 11 < size; i++) {
            const uint8_t *p = rom + i;
            if (p[0] == 0xE2 && p[1] == 0x20 && p[2] == 0xA9 && p[4] == 0x48 && p[5] == 0xAB &&
                p[6] == 0xC2 && p[7] == 0x20 && p[8] == 0xB9) {
                r->dict[k] = ((uint32_t)(p[3] & 0x3F) << 16) | p[9] | p[10] << 8;
                break;
            }
        }
        if (!r->dict[k])
            return 0;
    }
    return 1;
}

/* Decode one string at `off` into `out`. Dictionary references are
 * expanded; a word's own [RET] is dropped. Returns 0 on data that is not
 * text (bad command, glyph code of a font bottom half, runaway). */
static int Decode(const Rom *r, uint32_t off, Toks *out, int depth, uint32_t src_base)
{
    uint32_t i = off;
    while (i < r->size && i - off < 4096) {
        Tok t = {0};
        t.ref = -1;
        t.src = depth ? src_base : i;
        uint8_t b = r->rom[i++];
        if (b < 0xC0) {
            if (b & 0x10)
                return 0;               /* bottom-half tile: not a glyph */
            t.type = T_GLYPH;
            t.code = b;
            Push(out, &t);
            continue;
        }
        if (b > CMD_LAST)
            return 0;
        t.type = T_CMD;
        t.code = b;
        if (b == CMD_ESC) {
            while (i < r->size && r->rom[i] != 0 && t.blen < sizeof(t.bin))
                t.bin[t.blen++] = r->rom[i++];
            if (i >= r->size || r->rom[i] != 0)
                return 0;
            i++;
            Push(out, &t);
            continue;
        }
        for (int k = 0; k < 2; k++) {
            int n = kArgs[b - 0xC0][k];
            uint32_t v = 0;
            for (int j = 0; j < n; j++)
                v |= (uint32_t)r->rom[i + j] << (8 * j);
            i += (uint32_t)n;
            t.a[k] = v;
        }
        if ((b == CMD_LU1 || b == CMD_LU2) && depth < 4) {
            uint32_t table = r->dict[b - CMD_LU1];
            uint32_t word = (table & 0xFF0000) | Rd16(r, table + 2 * t.a[0]);
            int before = out->n;
            if (!Decode(r, word, out, depth + 1, t.src))
                return 0;
            if (out->n > before && out->t[out->n - 1].type == T_CMD &&
                out->t[out->n - 1].code == CMD_RET)
                out->n--;
            continue;
        }
        Push(out, &t);
        if (IsHalt(b))
            return 1;
    }
    return 0;
}

/* Bytes the string at `off` occupies in this ROM, up to and including the
 * command that ends it (dictionary words are references, not inline).
 * 0 when it does not decode. A redirect written over a string must fit in
 * this: what follows a string can be code (e.g. $08:F37B "[CLD]", two bytes
 * right before the Freedan->Will animation, whose COP 8E a 5-byte redirect
 * overwrote: Will came back from Freedan with scrambled sprites). */
static uint32_t StringSpan(const Rom *r, uint32_t off)
{
    uint32_t i = off;
    while (i < r->size && i - off < 4096) {
        const uint8_t b = r->rom[i++];
        if (b < 0xC0) {
            if (b & 0x10)
                return 0;
            continue;
        }
        if (b > CMD_LAST)
            return 0;
        if (b == CMD_ESC) {
            while (i < r->size && r->rom[i] != 0)
                i++;
            if (i >= r->size)
                return 0;
            i++;
            continue;
        }
        i += (uint32_t)(kArgs[b - 0xC0][0] + kArgs[b - 0xC0][1]);
        if (IsHalt(b))
            return i - off;
    }
    return 0;
}

/* ---- structure (i18n/structure.txt) ------------------------------------ */

typedef struct {
    char name[24];
    int unk16;
    uint32_t off;
    uint32_t marker;           /* byte offset in the USA encoding, 0: none */
    int has_marker;
    int scene;                 /* scene title (see SceneTitle) */
} Label;

typedef struct {
    char name[32];
    uint32_t off;
    int n;
    int entry[256];
    uint8_t entry_m[256];
} Table;

typedef struct {
    Label *l;
    int nl;
    Table *t;
    int nt;
    uint32_t font_ptr[8];      /* USA offsets of scene_meta font pointers ([0]: BG3) */
    int nfont;
    uint32_t map_table, map_start, map_end;   /* overworld names (sprite text) */
    uint32_t pro_str[16], pro_site[16];       /* prologue strings, LDA #imm operand */
    int npro;
    uint32_t free_start, free_end;            /* $FF padding in the prologue's bank */
    uint32_t scene_table;                     /* scene_actors: word pointer per map */
    int scene_n;
    uint32_t *ascii;           /* menu/item strings (ASCII engine) */
    int nascii;
    uint32_t ptab[32];         /* tables reached by ASCII [PTR] */
    int ptab_n[32];
    int nptab;
} Structure;

static int FindLabel(const Structure *s, const char *name)
{
    for (int i = 0; i < s->nl; i++)
        if (!strcmp(s->l[i].name, name))
            return i;
    return -1;
}

static int LoadStructure(Structure *s, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    char line[8192];
    memset(s, 0, sizeof(*s));
    s->l = calloc(4096, sizeof(Label));
    s->t = calloc(32, sizeof(Table));
    /* Tables name labels, so resolve them after every label is known. */
    char *tables[32];
    int ntab = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == 'S' && s->nl < 4096) {
            Label *l = &s->l[s->nl];
            char kind[16];
            unsigned off, mk;
            int used = 0;
            if (sscanf(line, "S %23s %15s %x%n", l->name, kind, &off, &used) < 3)
                continue;
            l->off = off;
            l->unk16 = !strcmp(kind, "unk16");
            if (sscanf(line + used, " %x", &mk) == 1) {
                l->marker = mk;
                l->has_marker = 1;
            }
            s->nl++;
        } else if (line[0] == 'A' && s->nascii < 1024) {
            char nm[64];
            unsigned off;
            if (!s->ascii)
                s->ascii = calloc(1024, sizeof(uint32_t));
            if (sscanf(line, "A %63s %x", nm, &off) == 2)
                s->ascii[s->nascii++] = off;
        } else if (line[0] == 'P' && s->nptab < 32) {
            char nm[64];
            unsigned off;
            int n;
            if (sscanf(line, "P %63s %x %d", nm, &off, &n) == 3) {
                s->ptab[s->nptab] = off;
                s->ptab_n[s->nptab++] = n;
            }
        } else if (line[0] == 'F') {
            unsigned off;
            if (sscanf(line, "F %x", &off) == 1 && s->nfont < 8)
                s->font_ptr[s->nfont++] = off;
        } else if (line[0] == 'N') {
            unsigned a, b, c;
            if (sscanf(line, "N %x %x %x", &a, &b, &c) == 3)
                s->map_table = a, s->map_start = b, s->map_end = c;
        } else if (line[0] == 'Q' && s->npro < 16) {
            unsigned a, b;
            if (sscanf(line, "Q %x %x", &a, &b) == 2)
                s->pro_str[s->npro] = a, s->pro_site[s->npro++] = b;
        } else if (line[0] == 'E') {
            unsigned a;
            int n;
            if (sscanf(line, "E %x %d", &a, &n) == 2)
                s->scene_table = a, s->scene_n = n;
        } else if (line[0] == 'Z') {
            unsigned a, b;
            if (sscanf(line, "Z %x %x", &a, &b) == 2)
                s->free_start = a, s->free_end = b;
        } else if (line[0] == 'T' && ntab < 32) {
            tables[ntab++] = strdup(line);
        }
    }
    fclose(f);
    for (int k = 0; k < ntab; k++) {
        Table *t = &s->t[s->nt];
        unsigned off;
        int used = 0;
        if (sscanf(tables[k], "T %31s %x%n", t->name, &off, &used) >= 2) {
            t->off = off;
            char *p = tables[k] + used, *tok;
            while ((tok = strtok(p, " \n")) && t->n < 256) {
                p = NULL;
                char *plus = strstr(tok, "+M");
                t->entry_m[t->n] = plus != NULL;
                if (plus)
                    *plus = 0;
                t->entry[t->n++] = FindLabel(s, tok);
            }
            s->nt++;
        }
        free(tables[k]);
    }
    return s->nl > 0;
}

/* ---- USA references ----------------------------------------------------- */

static int LabelAt(const Structure *s, uint32_t off, uint8_t *m)
{
    for (int i = 0; i < s->nl; i++) {
        if (s->l[i].off == off) {
            *m = 0;
            return i;
        }
        if (s->l[i].has_marker && s->l[i].off + s->l[i].marker == off) {
            *m = 1;
            return i;
        }
    }
    return -1;
}

/* Turn a decoded USA string's numeric references into label/table refs and
 * put its [::] marker back. Returns 0 if a bank-relative reference names
 * something the structure does not know (the string then stays in ROM). */
static int ResolveUsa(const Structure *s, int li, Toks *v)
{
    uint32_t bank = s->l[li].off & 0xFF0000;
    int ok = 1;
    for (int i = 0; i < v->n; i++) {
        Tok *t = &v->t[i];
        if (t->type != T_CMD || !IsRef(t->code))
            continue;
        if (t->code == CMD_ADR) {
            t->ref = -1;
            for (int k = 0; k < s->nt; k++)
                if (s->t[k].off == (bank | t->a[0]))
                    t->ref = k;
            ok &= t->ref >= 0;
        } else if (t->code == CMD_JMP) {
            t->ref = LabelAt(s, bank | t->a[0], &t->ref_m);
            ok &= t->ref >= 0;
        } else {            /* PRT: 24-bit, may stay a ROM address */
            t->ref = LabelAt(s, ((t->a[0] & 0x3F0000) | (t->a[0] & 0xFFFF)), &t->ref_m);
        }
    }
    if (s->l[li].has_marker) {
        Tok mk = {0};
        mk.type = T_MARK;
        mk.ref = -1;
        int at = v->n;
        for (int i = 0; i < v->n; i++)
            if (v->t[i].src >= s->l[li].off + s->l[li].marker) {
                at = i;
                break;
            }
        Push(v, &mk);
        memmove(&v->t[at + 1], &v->t[at], (size_t)(v->n - 1 - at) * sizeof(Tok));
        v->t[at] = mk;
    }
    return ok;
}

/* ---- pairing ------------------------------------------------------------- */

typedef struct {
    uint32_t off, target;
    uint8_t op;
    int li;                    /* USA: label; EU: index into eu strings */
} Site;

/* Text engine call sites:
 *   COP BF / COP 6B <ptr16>       string in the script's bank
 *   COP 19 <music> <addr24>       music + string
 *   LDY #<ptr16> / JSL <routine>  string in a bank the routine selects; the
 *                                 banks to try come from the USA pairs */
static int Sites(const uint8_t *rom, size_t size, Site *out, int max, const uint8_t *ldy_banks,
                 int nldy)
{
    int n = 0;
    for (size_t i = 16; i + 16 < size && n < max; i++) {
        if (rom[i] == 0x02 && (rom[i + 1] == 0xBF || rom[i + 1] == 0x6B)) {
            out[n++] = (Site){(uint32_t)i, ((uint32_t)i & 0xFF0000) | rom[i + 2] | rom[i + 3] << 8,
                              rom[i + 1], -1};
        } else if (rom[i] == 0x02 && rom[i + 1] == 0x19) {
            uint32_t a = rom[i + 3] | rom[i + 4] << 8 | (uint32_t)rom[i + 5] << 16;
            out[n++] = (Site){(uint32_t)i, ((a >> 16) & 0x3F) << 16 | (a & 0xFFFF), 0x19, -1};
        } else if (rom[i] == 0xA0 && rom[i + 3] == 0x22) {
            for (int b = 0; b < nldy && n < max; b++)
                out[n++] = (Site){(uint32_t)i, (uint32_t)ldy_banks[b] << 16 | rom[i + 1] | rom[i + 2] << 8,
                                  0xA0, -1};
        }
    }
    return n;
}

static int Skeleton(const Toks *v, uint8_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < v->n && n < max; i++)
        if (v->t[i].type == T_CMD && IsSkeleton(v->t[i].code))
            out[n++] = v->t[i].code;
    return n;
}

typedef struct {
    Toks toks;
    uint8_t skel[64];
    int nskel;
} Text;

static int Score(const Rom *us, const Site *a, const Text *at, const Rom *eu, const Site *b,
                 const Text *bt)
{
    if (a->op != b->op)
        return -1;
    int s = 0;
    /* Script bytes after the operand (for LDY, after the JSL too). */
    int post = a->op == 0x19 ? 6 : a->op == 0xA0 ? 7 : 4;
    for (int k = 1; k <= 8; k++)
        s += us->rom[a->off - k] == eu->rom[b->off - k];
    for (int k = post; k < post + 6; k++)
        s += us->rom[a->off + k] == eu->rom[b->off + k];
    if (at->nskel == bt->nskel && !memcmp(at->skel, bt->skel, (size_t)at->nskel))
        s += 6;
    return s >= 10 ? s : -1;
}

/* ---- encoding and placement --------------------------------------------- */

typedef struct {
    int li;
    Toks toks;
    int bank_index, pos, len;
    int placed, relocatable;
    uint8_t tail[4];
} Item;

static uint32_t HiromAddr(uint32_t off)
{
    return (off & 0x8000) ? (0x800000 | off) : (0xC00000 | off);
}

enum { kVBankFirst = 0xA0, kVBase = 0x8000 };

typedef struct {
    Item *items;
    int nitems;
    int *item_of_label;
    int *table_bank, *table_pos;
    const Structure *s;
} Layout;

static int MarkerPos(const Item *it)
{
    int pos = 0;
    for (int i = 0; i < it->toks.n; i++) {
        const Tok *t = &it->toks.t[i];
        if (t->type == T_MARK)
            return pos;
        pos += t->type == T_GLYPH ? 1 : 1 + kArgs[t->code - 0xC0][0] + kArgs[t->code - 0xC0][1] +
               (t->code == CMD_ESC ? t->blen + 1 : 0);
    }
    return 0;
}

/* Encode one item. With lay == NULL references encode as 0 (sizing).
 * Returns the length, or -1 when a reference cannot be expressed. */
static int Encode(const Layout *lay, const Item *it, uint8_t *out)
{
    int n = 0, last_halt = 0;
    for (int i = 0; i < it->toks.n; i++) {
        const Tok *t = &it->toks.t[i];
        last_halt = 0;
        if (t->type == T_MARK)
            continue;
        if (t->type == T_GLYPH) {
            if (out)
                out[n] = t->code;
            n++;
            continue;
        }
        if (out)
            out[n] = t->code;
        n++;
        if (t->code == CMD_ESC) {
            if (out)
                memcpy(out + n, t->bin, t->blen), out[n + t->blen] = 0;
            n += t->blen + 1;
            continue;
        }
        uint32_t a0 = t->a[0];
        if (lay && IsRef(t->code) && t->ref >= 0) {
            if (t->code == CMD_ADR) {
                if (lay->table_bank[t->ref] != it->bank_index)
                    return -1;
                /* Every entry must live in this virtual bank too. */
                const Table *tb = &lay->s->t[t->ref];
                for (int e = 0; e < tb->n; e++) {
                    int ei = tb->entry[e] >= 0 ? lay->item_of_label[tb->entry[e]] : -1;
                    if (ei < 0 || !lay->items[ei].placed ||
                        lay->items[ei].bank_index != it->bank_index)
                        return -1;
                }
                a0 = kVBase + (uint32_t)lay->table_pos[t->ref];
            } else {
                int ti = lay->item_of_label[t->ref];
                const Item *tg = ti >= 0 ? &lay->items[ti] : NULL;
                if (tg && tg->placed) {
                    uint32_t p = kVBase + (uint32_t)tg->pos + (t->ref_m ? (uint32_t)MarkerPos(tg) : 0);
                    if (t->code == CMD_JMP) {
                        if (tg->bank_index != it->bank_index)
                            return -1;
                        a0 = p;
                    } else {
                        a0 = ((uint32_t)(kVBankFirst + tg->bank_index) << 16) | p;
                    }
                } else if (t->code == CMD_PRT) {
                    const Label *l = &lay->s->l[t->ref];
                    a0 = HiromAddr(l->off + (t->ref_m ? l->marker : 0));
                } else {
                    return -1;
                }
            }
        } else if (lay && IsRef(t->code) && t->code != CMD_PRT) {
            return -1;          /* bank-relative reference nobody resolved */
        }
        uint32_t args[2] = {a0, t->a[1]};
        for (int k = 0; k < 2; k++)
            for (int j = 0; j < kArgs[t->code - 0xC0][k]; j++) {
                if (out)
                    out[n] = (uint8_t)(args[k] >> (8 * j));
                n++;
            }
        last_halt = IsHalt(t->code);
    }
    if (!last_halt) {
        if (out)
            out[n] = CMD_RET;
        n++;
    }
    /* unk16 blocks carry a raw 4-byte tail after their [RET]; the sizing
     * pass (lay == NULL) leaves it to the caller. */
    if (lay && lay->s->l[it->li].unk16) {
        if (out)
            memcpy(out + n, it->tail, 4);
        n += 4;
    }
    return n;
}

const char *IogTextRomLanguage(const uint8_t *rom, size_t size)
{
    if (size < 0x10000 || memcmp(rom + 0xFFC0, "ILLUSION OF TIME ", 17))
        return NULL;
    const char *r = (const char *)rom + 0xFFC0 + 17;
    if (!memcmp(r, "SPN", 3)) return "es";
    if (!memcmp(r, "FRA", 3)) return "fr";
    if (!memcmp(r, "GRM", 3)) return "de";
    if (!memcmp(r, "ITA", 3)) return "it";
    return NULL;
}

static const char *LanguageName(const char *code)
{
    if (!strcmp(code, "es")) return "Español";
    if (!strcmp(code, "fr")) return "Français";
    if (!strcmp(code, "de")) return "Deutsch";
    if (!strcmp(code, "it")) return "Italiano";
    return code;
}

/* Append a patch of `len` bytes at USA offset `off`. */
static void AddPatch(IogPack *pack, uint32_t off, const uint8_t *bytes, uint32_t len)
{
    if (pack->npatches == pack->cap_patches) {
        pack->cap_patches = pack->cap_patches ? pack->cap_patches * 2 : 1024;
        pack->patches = realloc(pack->patches, (size_t)pack->cap_patches * sizeof(IogPatch));
    }
    IogPatch *p = &pack->patches[pack->npatches++];
    p->offset = off;
    p->len = len;
    p->data = malloc(len);
    p->original = calloc(1, len);
    memcpy(p->data, bytes, len);
}

static void AddRedirect(IogPack *pack, uint32_t off, uint8_t cmd, uint32_t addr, uint8_t end)
{
    const uint8_t b[5] = {cmd, (uint8_t)addr, (uint8_t)(addr >> 8), (uint8_t)(addr >> 16), end};
    AddPatch(pack, off, b, 5);
}

/* ---- QuintetLZ (the game's graphics compression) ------------------------ */

/* Decompress at `off` into out (cap bytes). Returns the decompressed size,
 * 0 on malformed data; *consumed gets the compressed size. */
static size_t QuintetExpand(const Rom *r, uint32_t off, uint8_t *out, size_t cap, size_t *consumed)
{
    if (off + 2 > r->size)
        return 0;
    size_t len = r->rom[off] | r->rom[off + 1] << 8;
    if (len > cap)
        return 0;
    uint64_t bit = (uint64_t)(off + 2) * 8;
    uint8_t dict[256];
    memset(dict, 0x20, sizeof(dict));
    unsigned dp = 0xEF;
    size_t n = 0;
#define BITS(k, v) do { v = 0; for (int _b = 0; _b < (k); _b++, bit++) { \
        if ((bit >> 3) >= r->size) return 0; \
        v = v << 1 | (r->rom[bit >> 3] >> (7 - (bit & 7)) & 1); } } while (0)
    while (n < len) {
        unsigned flag, v;
        BITS(1, flag);
        if (flag) {
            BITS(8, v);
            out[n++] = (uint8_t)v;
            dict[dp] = (uint8_t)v;
            dp = (dp + 1) & 0xFF;
        } else {
            unsigned idx, ln;
            BITS(8, idx);
            BITS(4, ln);
            for (ln += 2; ln--; ) {
                uint8_t b = dict[idx];
                idx = (idx + 1) & 0xFF;
                if (n < len)
                    out[n++] = b;
                dict[dp] = b;
                dp = (dp + 1) & 0xFF;
            }
        }
    }
#undef BITS
    *consumed = (size_t)((bit + 7) / 8 - off);
    return n;
}

static uint32_t Ptr24Offset(const Rom *r, uint32_t at)
{
    uint32_t a = r->rom[at] | r->rom[at + 1] << 8 | (uint32_t)r->rom[at + 2] << 16;
    return ((a >> 16) & 0x3F) << 16 | (a & 0xFFFF);
}

/* Find the European counterpart of a USA pointer by the bytes around it. */
static uint32_t FindByContext(const Rom *ru, const Rom *re, uint32_t at, int pre, int width, int post)
{
    if (at < (uint32_t)pre || at + (uint32_t)(width + post) > ru->size)
        return 0;
    const uint8_t *a = ru->rom + at - pre, *b = ru->rom + at + width;
    /* Same place first (most layout data did not move), with a shorter
     * context so an edited neighbouring byte does not hide it. */
    int sp = pre < 4 ? pre : 4;
    if (at + (uint32_t)(width + post) <= re->size &&
        !memcmp(re->rom + at - sp, ru->rom + at - sp, (size_t)sp) &&
        !memcmp(re->rom + at + width, b, (size_t)post))
        return at;
    for (size_t i = 0; i + (size_t)(pre + width + post) < re->size; i++)
        if (!memcmp(re->rom + i, a, (size_t)pre) && !memcmp(re->rom + i + pre + width, b, (size_t)post))
            return (uint32_t)i + (uint32_t)pre;
    return 0;
}

/* Use the European release's fonts: copy their compressed data into a
 * virtual bank and point scene_meta's entries there. The European entries
 * are found by the bytes around the USA pointers. The first one (the BG3
 * text font) is also kept decompressed for swapping VRAM on a live switch. */
static void AddFont(const Structure *st, const Rom *ru, const Rom *re, IogPack *pack)
{
    int vb = -1, fill = kIogVirtualBankSize;
    for (int k = 0; k < st->nfont; k++) {
        uint32_t f = st->font_ptr[k];
        uint32_t at = FindByContext(ru, re, f, 6, 3, 5);
        static uint8_t tmp[0x10000];
        size_t used = 0, usa_used = 0;
        size_t n = at ? QuintetExpand(re, Ptr24Offset(re, at), tmp, sizeof(tmp), &used) : 0;
        if (!n || used > kIogVirtualBankSize) {
            fprintf(stderr, "[i18n] European font %d not found; keeping the USA one\n", k);
            continue;
        }
        if (k == 0) {
            if (n != sizeof(pack->font) ||
                QuintetExpand(ru, Ptr24Offset(ru, f), pack->usa_font, sizeof(pack->usa_font),
                              &usa_used) != sizeof(pack->usa_font))
                continue;
            memcpy(pack->font, tmp, sizeof(pack->font));
            pack->has_font = 1;
        }
        if (fill + (int)used > kIogVirtualBankSize) {
            if (pack->nbanks >= kIogVirtualBanks)
                return;
            vb = pack->nbanks++;
            pack->banks[vb] = calloc(1, kIogVirtualBankSize);
            fill = 0;
        }
        memcpy(pack->banks[vb] + fill, re->rom + Ptr24Offset(re, at), used);
        uint32_t addr = ((uint32_t)(kVBankFirst + vb) << 16) | (kVBase + (uint32_t)fill);
        const uint8_t ptr[3] = {(uint8_t)addr, (uint8_t)(addr >> 8), (uint8_t)(addr >> 16)};
        AddPatch(pack, f, ptr, 3);
        fill += (int)used;
    }
}

/* ---- sprite text (overworld names, prologue) ------------------------------- */

/* ---- scene titles -----------------------------------------------------------
 *
 * scene_actors ($0C:8000) holds a word pointer per map to that map's actor
 * list: 7-byte entries (9 when the seventh byte, an enemy's, is nonzero)
 * up to $FF, then the scene's title as a dialogue string ("Lance's House",
 * "TEST MAP"). The European releases translate them at the same map index.
 * Returns the ROM offset of map m's title, 0 if the list does not parse. */
static uint32_t SceneTitle(const Rom *r, const Structure *st, int m)
{
    uint32_t t = st->scene_table;
    if (!t || m >= st->scene_n || t + 2 * (uint32_t)m + 2 > r->size)
        return 0;
    uint32_t o = (t & 0xFF0000) | Rd16(r, t + 2 * (uint32_t)m);
    for (int n = 0; n < 256; n++) {
        if (o + 9 > r->size)
            return 0;
        if (r->rom[o] == 0xFF)
            return o + 1;
        o += r->rom[o + 6] ? 9 : 7;
    }
    return 0;
}

/* Title length up to its [RET]; 0 if none nearby. */
static uint32_t SceneTitleLen(const Rom *r, uint32_t off)
{
    for (uint32_t i = off; i < r->size && i < off + 64; i++)
        if (r->rom[i] == CMD_RET)
            return i - off;
    return 0;
}

/* Append a label per distinct USA scene title long enough to take a
 * redirect; scene_map[li] keeps the first map it belongs to. */
static void AddSceneLabels(Structure *st, const Rom *ru, int *scene_map)
{
    for (int m = 0; m < st->scene_n && st->nl < 4096; m++) {
        uint32_t off = SceneTitle(ru, st, m);
        if (!off || SceneTitleLen(ru, off) < 4)
            continue;
        int dup = 0;
        for (int i = 0; i < st->nl && !dup; i++)
            dup = st->l[i].off == off;
        if (dup)
            continue;
        Label *l = &st->l[st->nl];
        memset(l, 0, sizeof(*l));
        snprintf(l->name, sizeof(l->name), "scene_title_%02X", m);
        l->off = off;
        l->scene = 1;
        scene_map[st->nl++] = m;
    }
}

static uint32_t SpriteLen(const Rom *r, uint32_t off)
{
    uint32_t i = off;
    while (i < r->size && r->rom[i] != 0xCA && i - off < 512)
        i++;
    return i < r->size && r->rom[i] == 0xCA ? i - off + 1 : 0;
}

/* The sprite text engine has no redirect command, so its strings move:
 *   - overworld names: the European names are repacked into the USA names'
 *     region and the name table (data) re-pointed, matching entries by map;
 *   - prologue: its pointers are LDA #imm operands in code the interpreter
 *     runs (bank $0B has no compiled functions there), so the European text
 *     goes into the bank's free $FF space and the operands are patched. */
static void AddSprites(const Structure *st, const Rom *ru, const Rom *re, IogPack *pack)
{
    int maps = 0, pro = 0;
    if (st->map_table) {
        /* European table: the lookup routine loads it as LDA #bank / PHA /
         * PLB / LDA table,X; find the routine by the USA code around it. */
        static const uint8_t sig[] = {0x08, 0xDA, 0xA2, 0x00, 0x00, 0xE2, 0x20, 0x8B, 0xA9};
        uint32_t eu_tab = 0;
        for (size_t i = 0; i + 16 < re->size && !eu_tab; i++)
            if (!memcmp(re->rom + i, sig, sizeof(sig)) && re->rom[i + 10] == 0x48 &&
                re->rom[i + 11] == 0xAB && re->rom[i + 12] == 0xBD)
                eu_tab = ((uint32_t)(re->rom[i + 9] & 0x3F) << 16) | re->rom[i + 13] | re->rom[i + 14] << 8;
        uint32_t cap = st->map_end - st->map_start;
        uint8_t *region = eu_tab && cap < 0x4000 ? malloc(cap) : NULL;
        uint32_t fill = 0, placed_eu[64], placed_at[64];
        int nplaced = 0, ok = region != NULL;
        uint32_t ut = st->map_table;
        /* entries: id, ptr16; id 0 ends */
        for (int k = 0; ok && ru->rom[ut + 3 * k] && k < 64; k++) {
            uint8_t id = ru->rom[ut + 3 * k];
            uint32_t eo = 0;
            for (int e = 0; re->rom[eu_tab + 3 * e] && e < 64; e++)
                if (re->rom[eu_tab + 3 * e] == id)
                    eo = (eu_tab & 0xFF0000) | Rd16(re, eu_tab + 3 * e + 1);
            if (!eo)
                continue;
            int j = 0;
            while (j < nplaced && placed_eu[j] != eo)
                j++;
            if (j == nplaced) {
                uint32_t n = SpriteLen(re, eo);
                if (!n || fill + n > cap || nplaced >= 64) {
                    ok = 0;
                    break;
                }
                memcpy(region + fill, re->rom + eo, n);
                placed_eu[nplaced] = eo;
                placed_at[nplaced++] = st->map_start + fill;
                fill += n;
            }
            uint32_t at = placed_at[j];
            const uint8_t ptr[2] = {(uint8_t)at, (uint8_t)(at >> 8)};
            AddPatch(pack, ut + 3 * (uint32_t)k + 1, ptr, 2);
            maps++;
        }
        if (ok && fill)
            AddPatch(pack, st->map_start, region, fill);
        else
            maps = 0;
        free(region);
    }
    if (st->npro && st->free_end > st->free_start) {
        uint32_t cap = st->free_end - st->free_start, fill = 0;
        int clean = 1;
        for (uint32_t i = st->free_start; i < st->free_end; i++)
            clean &= ru->rom[i] == 0xFF;
        uint8_t *space = clean ? malloc(cap) : NULL;
        /* European prologue sites in order: LDA #text / STA $0026,Y. The
         * k-th USA site maps to the k-th one when the script bytes before it
         * differ (longer text moved its position arguments). */
        uint32_t eu_sites[16];
        int neu = 0;
        uint32_t lo = st->pro_site[0] & 0xFF0000, first = st->pro_site[0], last = first;
        for (int k = 1; k < st->npro; k++) {
            if (st->pro_site[k] < first) first = st->pro_site[k];
            if (st->pro_site[k] > last) last = st->pro_site[k];
        }
        for (uint32_t i = first - 0x200; i + 6 < re->size && i < last + 0x200 && neu < 16; i++)
            if (re->rom[i] == 0xA9 && re->rom[i + 3] == 0x99 && re->rom[i + 4] == 0x26 &&
                re->rom[i + 5] == 0x00 && (i & 0xFF0000) == lo) {
                uint32_t eo = lo | re->rom[i + 1] | re->rom[i + 2] << 8;
                if (SpriteLen(re, eo))
                    eu_sites[neu++] = i + 1;
            }
        uint32_t usa_sorted[16];
        for (int k = 0; k < st->npro; k++)
            usa_sorted[k] = st->pro_site[k];
        for (int k = 0; k < st->npro; k++)
            for (int j = k + 1; j < st->npro; j++)
                if (usa_sorted[j] < usa_sorted[k]) {
                    uint32_t t = usa_sorted[k];
                    usa_sorted[k] = usa_sorted[j];
                    usa_sorted[j] = t;
                }
        for (int k = 0; space && k < st->npro; k++) {
            /* European LDA #imm: same script bytes before it, else by order. */
            uint32_t site = FindByContext(ru, re, st->pro_site[k], 9, 2, 0);
            if (!site && neu == st->npro)
                for (int q = 0; q < st->npro; q++)
                    if (usa_sorted[q] == st->pro_site[k])
                        site = eu_sites[q];
            if (!site)
                continue;
            uint32_t eo = (site & 0xFF0000) | Rd16(re, site);
            uint32_t n = SpriteLen(re, eo);
            if (!n || fill + n > cap)
                continue;
            memcpy(space + fill, re->rom + eo, n);
            uint32_t at = st->free_start + fill;
            const uint8_t imm[2] = {(uint8_t)at, (uint8_t)(at >> 8)};
            AddPatch(pack, st->pro_site[k], imm, 2);
            fill += n;
            pro++;
        }
        if (space && fill)
            AddPatch(pack, st->free_start, space, fill);
        free(space);
    }
    pack->sprite_translated = maps + pro;
}

/* ---- menu/item text (ASCII engine) ---------------------------------------- */

/* Argument sizes of ASCII commands $01-$11 ($0C ESC is $FF-delimited). */
static const uint8_t kAsciiArgs[18][3] = {
    [1] = {1, 1}, [2] = {3}, [3] = {1}, [4] = {3, 2}, [5] = {1, 2}, [6] = {1, 1, 2},
    [7] = {2}, [8] = {1, 2}, [14] = {2}, [15] = {1, 1}, [16] = {1},
};
enum { A_PRT = 2, A_PTR = 4, A_ESC = 12, A_LU = 16 };

typedef struct {
    Rom rom;
    uint32_t engine, lu;
} AsciiRom;

static const uint8_t kAsciiEngineSig[] = {0x08, 0x8b, 0xe2, 0x20, 0xb9, 0x00, 0x00, 0xc8, 0xc9, 0x12, 0xb0};

static int AsciiInit(AsciiRom *a, const Rom *r)
{
    a->rom = *r;
    a->engine = a->lu = 0;
    for (size_t i = 0; i + sizeof(kAsciiEngineSig) < r->size; i++)
        if (!memcmp(r->rom + i, kAsciiEngineSig, sizeof(kAsciiEngineSig))) {
            a->engine = (uint32_t)i;
            break;
        }
    if (!a->engine)
        return 0;
    /* JSR (table,X) operand at +$15; entry $10 is [LU]: SEP #$20 / LDA #bank /
     * PHA / PLB / REP #$20 / (PLY) / LDA table,Y. */
    uint32_t table = (a->engine & 0xFF0000) | Rd16(r, a->engine + 0x15);
    uint32_t h = (a->engine & 0xFF0000) | Rd16(r, table + 2 * A_LU);
    for (uint32_t i = h; i < h + 40 && i + 12 < r->size; i++) {
        const uint8_t *p = r->rom + i;
        if (p[0] == 0xE2 && p[1] == 0x20 && p[2] == 0xA9 && p[4] == 0x48 && p[5] == 0xAB &&
            p[6] == 0xC2 && p[7] == 0x20) {
            int k = p[8] == 0x7A ? 9 : 8;
            if (p[k] == 0xB9) {
                a->lu = ((uint32_t)(p[3] & 0x3F) << 16) | p[k + 1] | p[k + 2] << 8;
                break;
            }
        }
    }
    return a->lu != 0;
}

static uint32_t Off24(uint32_t a) { return ((a >> 16) & 0x3F) << 16 | (a & 0xFFFF); }

/* Decode one menu string ([LU] expanded). 0 on anything that is not one. */
static int AsciiDecode(const AsciiRom *a, uint32_t off, Toks *out, int depth)
{
    const Rom *r = &a->rom;
    for (uint32_t i = off; i < r->size && i - off < 512;) {
        Tok t = {0};
        t.ref = -1;
        uint8_t b = r->rom[i++];
        if (b == 0)
            return 1;
        if (b >= 0x12) {
            t.type = T_GLYPH;
            t.code = b;
            Push(out, &t);
            continue;
        }
        t.type = T_CMD;
        t.code = b;
        if (b == A_ESC) {
            while (i < r->size && r->rom[i] != 0xFF && t.blen < sizeof(t.bin))
                t.bin[t.blen++] = r->rom[i++];
            if (i >= r->size || r->rom[i] != 0xFF)
                return 0;
            i++;
            Push(out, &t);
            continue;
        }
        if (b == 9 || b == 10 || b == 11 || b == 13 || b == 17 || kAsciiArgs[b][0]) {
            for (int k = 0; k < 3; k++) {
                uint32_t v = 0;
                for (int j = 0; j < kAsciiArgs[b][k]; j++)
                    v |= (uint32_t)r->rom[i + j] << (8 * j);
                i += kAsciiArgs[b][k];
                t.a[k] = v;
            }
        } else {
            return 0;
        }
        if (b == A_LU && depth < 3) {
            uint32_t w = (a->lu & 0xFF0000) | Rd16(r, a->lu + 2 * t.a[0]);
            if (!AsciiDecode(a, w, out, depth + 1))
                return 0;
            continue;
        }
        Push(out, &t);
    }
    return 0;
}

static int AsciiEncode(const Toks *v, uint8_t *out)
{
    int n = 0;
    for (int i = 0; i < v->n; i++) {
        const Tok *t = &v->t[i];
        if (out)
            out[n] = t->code;
        n++;
        if (t->type == T_GLYPH)
            continue;
        if (t->code == A_ESC) {
            if (out)
                memcpy(out + n, t->bin, t->blen), out[n + t->blen] = 0xFF;
            n += t->blen + 1;
            continue;
        }
        for (int k = 0; k < 3; k++)
            for (int j = 0; j < kAsciiArgs[t->code][k]; j++) {
                if (out)
                    out[n] = (uint8_t)(t->a[k] >> (8 * j));
                n++;
            }
    }
    if (out)
        out[n] = 0;
    return n + 1;
}

static int IsAsciiRef(const Tok *t) { return t->type == T_CMD && (t->code == A_PRT || t->code == A_PTR); }

static int AsciiSkelEq(const Toks *a, const Toks *b)
{
    int i = 0, j = 0;
    for (;;) {
        while (i < a->n && a->t[i].type != T_CMD) i++;
        while (j < b->n && b->t[j].type != T_CMD) j++;
        if (i >= a->n || j >= b->n)
            return i >= a->n && j >= b->n;
        if (a->t[i++].code != b->t[j++].code)
            return 0;
    }
}

static int AsciiIndex(const Structure *st, uint32_t off)
{
    for (int i = 0; i < st->nascii; i++)
        if (st->ascii[i] == off)
            return i;
    return -1;
}

/* Pair the menu strings, relocate the European ones into new virtual banks
 * and redirect each USA string with [PRT:@virtual][$00]. Tables stay in ROM:
 * they point at string starts, which are redirected themselves. */
static void AddAscii(const Structure *st, const Rom *ru, const Rom *re, IogPack *pack)
{
    AsciiRom au, ae;
    if (!st->nascii || !AsciiInit(&au, ru) || !AsciiInit(&ae, re)) {
        fprintf(stderr, "[i18n] menu text engine not found; menus stay in English\n");
        return;
    }
    const int n = st->nascii;
    uint32_t *eu_of = calloc((size_t)n, sizeof(uint32_t));
    int *has = calloc((size_t)n, sizeof(int));

    /* COP BD <string24> call sites. */
    Site *us = calloc(4096, sizeof(Site)), *es = calloc(4096, sizeof(Site));
    int nus = 0, nes = 0;
    for (size_t i = 16; i + 16 < ru->size && nus < 4096; i++)
        if (ru->rom[i] == 0x02 && ru->rom[i + 1] == 0xBD) {
            uint32_t t = Off24(ru->rom[i + 2] | ru->rom[i + 3] << 8 | (uint32_t)ru->rom[i + 4] << 16);
            int ai = AsciiIndex(st, t);
            if (ai >= 0)
                us[nus++] = (Site){(uint32_t)i, t, 0xBD, ai};
        }
    Text *ut = calloc((size_t)nus + 1, sizeof(Text));
    for (int i = 0; i < nus; i++)
        AsciiDecode(&au, us[i].target, &ut[i].toks, 0);
    Text *et = calloc(4096, sizeof(Text));
    for (size_t i = 16; i + 16 < re->size && nes < 4096; i++)
        if (re->rom[i] == 0x02 && re->rom[i + 1] == 0xBD) {
            uint32_t t = Off24(re->rom[i + 2] | re->rom[i + 3] << 8 | (uint32_t)re->rom[i + 4] << 16);
            Toks v = {0};
            if (AsciiDecode(&ae, t, &v, 0) && v.n) {
                et[nes].toks = v;
                es[nes++] = (Site){(uint32_t)i, t, 0xBD, nes};
            } else {
                free(v.t);
            }
        }
    int *best = calloc((size_t)(nus + 1) * (size_t)(nes + 1), sizeof(int));
    uint8_t *move = calloc((size_t)(nus + 1) * (size_t)(nes + 1), 1);
#define B(i, j) best[(size_t)(i) * (size_t)(nes + 1) + (size_t)(j)]
#define M(i, j) move[(size_t)(i) * (size_t)(nes + 1) + (size_t)(j)]
    for (int i = 1; i <= nus; i++)
        for (int j = 1; j <= nes; j++) {
            int v = B(i - 1, j), mv = 1;
            if (B(i, j - 1) > v)
                v = B(i, j - 1), mv = 2;
            int sc = 0;
            for (int k = 1; k <= 8; k++)
                sc += ru->rom[us[i - 1].off - k] == re->rom[es[j - 1].off - k];
            for (int k = 5; k < 11; k++)
                sc += ru->rom[us[i - 1].off + k] == re->rom[es[j - 1].off + k];
            if (AsciiSkelEq(&ut[i - 1].toks, &et[j - 1].toks))
                sc += 6;
            if (sc >= 10 && B(i - 1, j - 1) + sc > v)
                v = B(i - 1, j - 1) + sc, mv = 3;
            B(i, j) = v;
            M(i, j) = (uint8_t)mv;
        }
    for (int i = nus, j = nes; i && j;) {
        if (M(i, j) == 3) {
            int ai = us[i - 1].li;
            if (!has[ai])
                eu_of[ai] = es[j - 1].target, has[ai] = 1;
            i--, j--;
        } else if (M(i, j) == 1) {
            i--;
        } else {
            j--;
        }
    }
#undef B
#undef M

    /* Follow [PRT] strings and [PTR] tables in step. */
    for (int changed = 1; changed;) {
        changed = 0;
        for (int ai = 0; ai < n; ai++) {
            if (!has[ai])
                continue;
            Toks u = {0}, e = {0};
            if (AsciiDecode(&au, st->ascii[ai], &u, 0) && AsciiDecode(&ae, eu_of[ai], &e, 0)) {
                for (int x = 0, y = 0;; x++, y++) {
                    while (x < u.n && !IsAsciiRef(&u.t[x])) x++;
                    while (y < e.n && !IsAsciiRef(&e.t[y])) y++;
                    if (x >= u.n || y >= e.n || u.t[x].code != e.t[y].code)
                        break;
                    uint32_t uo = Off24(u.t[x].a[0]), eo = Off24(e.t[y].a[0]);
                    if (u.t[x].code == A_PRT) {
                        int k = AsciiIndex(st, uo);
                        if (k >= 0 && !has[k])
                            eu_of[k] = eo, has[k] = 1, changed = 1;
                        continue;
                    }
                    for (int q = 0; q < st->nptab; q++) {
                        if (st->ptab[q] != uo)
                            continue;
                        for (int k = 0; k < st->ptab_n[q]; k++) {
                            uint32_t ue = (uo & 0xFF0000) | Rd16(ru, uo + 2 * (uint32_t)k);
                            uint32_t ee = (eo & 0xFF0000) | Rd16(re, eo + 2 * (uint32_t)k);
                            int ki = AsciiIndex(st, ue);
                            if (ki >= 0 && !has[ki])
                                eu_of[ki] = ee, has[ki] = 1, changed = 1;
                        }
                    }
                }
            }
            free(u.t);
            free(e.t);
        }
    }

    /* Relocate and redirect. */
    uint32_t ends[4096];
    int nends = 0;
    for (int i = 0; i < st->nl && nends < 4096; i++) ends[nends++] = st->l[i].off;
    for (int i = 0; i < n && nends < 4096; i++) ends[nends++] = st->ascii[i];
    for (int i = 0; i < st->nt && nends < 4096; i++) ends[nends++] = st->t[i].off;
    for (int i = 0; i < st->nptab && nends < 4096; i++) ends[nends++] = st->ptab[i];
    int vb = -1, fill = kIogVirtualBankSize, done = 0;
    for (int ai = 0; ai < n; ai++) {
        if (!has[ai]) {
            if (getenv("IOG_I18N_REPORT"))
                fprintf(stderr, "[i18n] menu untranslated %06X (no pair)\n", st->ascii[ai]);
            continue;
        }
        uint32_t off = st->ascii[ai], next = off + 0x10000;
        for (int k = 0; k < nends; k++)
            if (ends[k] > off && ends[k] < next)
                next = ends[k];
        if (next - off < 5) {
            if (getenv("IOG_I18N_REPORT"))
                fprintf(stderr, "[i18n] menu untranslated %06X (only %u bytes)\n", off, next - off);
            continue;
        }
        Toks u = {0}, e = {0};
        int ok = AsciiDecode(&au, off, &u, 0) && AsciiDecode(&ae, eu_of[ai], &e, 0);
        /* Drawn tile runs ([ESC], e.g. the HUD's PLAYER label) must match:
         * where the European release draws them elsewhere, the USA string
         * with the European font already shows the European art. */
        int ue = 0, ee = 0;
        for (int k = 0; k < u.n; k++) ue += u.t[k].type == T_CMD && u.t[k].code == A_ESC;
        for (int k = 0; k < e.n; k++) ee += e.t[k].type == T_CMD && e.t[k].code == A_ESC;
        if (ok && ue != ee && pack->has_font) {
            /* Keep the USA string (with the European font it shows the
             * European art), completing drawn runs from the European HUD. */
            free(e.t);
            e = (Toks){0};
            for (int k = 0; k < u.n; k++) {
                Tok t = u.t[k];
                if (t.type == T_CMD && t.code == A_ESC && t.blen) {
                    int run = 1;
                    for (int q = 1; q < t.blen; q++)
                        run &= t.bin[q] == t.bin[q - 1] + 1;
                    /* The European HUD is a plain tilemap in ROM (tile | $3400
                     * words); where it holds the same run, the tiles that
                     * follow it complete the label (JUGADO + R). */
                    uint8_t pat[2 * sizeof(t.bin)];
                    int np = 0;
                    for (int q = 0; q < t.blen; q++)
                        pat[np++] = t.bin[q], pat[np++] = 0x34;
                    for (size_t i = 0; run && np && i + (size_t)np + 2 < re->size; i++) {
                        if (memcmp(re->rom + i, pat, (size_t)np))
                            continue;
                        for (size_t w = i + (size_t)np; w + 1 < re->size && re->rom[w] &&
                             re->rom[w + 1] == 0x34 && t.blen < sizeof(t.bin); w += 2)
                            t.bin[t.blen++] = re->rom[w];
                        break;
                    }
                }
                Push(&e, &t);
            }
        } else {
            ok &= ue == ee;
        }
        /* The European references become the USA ones, in order. */
        for (int x = 0, y = 0; ok; x++, y++) {
            while (x < u.n && !IsAsciiRef(&u.t[x])) x++;
            while (y < e.n && !IsAsciiRef(&e.t[y])) y++;
            if (x >= u.n && y >= e.n)
                break;
            if (x >= u.n || y >= e.n || u.t[x].code != e.t[y].code) {
                ok = 0;
                break;
            }
            e.t[y].a[0] = u.t[x].a[0];
        }
        int len = ok ? AsciiEncode(&e, NULL) : 0;
        if (ok && len <= kIogVirtualBankSize) {
            if (fill + len > kIogVirtualBankSize) {
                if (pack->nbanks >= kIogVirtualBanks)
                    ok = 0;
                else {
                    vb = pack->nbanks++;
                    pack->banks[vb] = calloc(1, kIogVirtualBankSize);
                    fill = 0;
                }
            }
            if (ok) {
                AsciiEncode(&e, pack->banks[vb] + fill);
                uint32_t addr = ((uint32_t)(kVBankFirst + vb) << 16) | (kVBase + (uint32_t)fill);
                AddRedirect(pack, off, A_PRT, addr, 0);
                fill += len;
                done++;
            }
        }
        if (!ok && getenv("IOG_I18N_REPORT"))
            fprintf(stderr, "[i18n] menu untranslated %06X (differs, %d/%d drawn runs)\n", off, ue, ee);
        free(u.t);
        free(e.t);
    }
    pack->menu_translated = done;
    pack->menu_total = n;
    for (int i = 0; i < nus; i++) free(ut[i].toks.t);
    for (int i = 0; i < nes; i++) free(et[i].toks.t);
    free(ut); free(et); free(us); free(es); free(best); free(move); free(eu_of); free(has);
}

/* ---- build ---------------------------------------------------------------------- */

void IogTextFreePack(IogPack *p)
{
    if (!p)
        return;
    for (int i = 0; i < p->nbanks; i++)
        free(p->banks[i]);
    for (int i = 0; i < p->npatches; i++) {
        free(p->patches[i].data);
        free(p->patches[i].original);
    }
    free(p->patches);
    free(p);
}

static int CountRefs(const Toks *v, uint8_t *codes, int max)
{
    int n = 0;
    for (int i = 0; i < v->n; i++)
        if (v->t[i].type == T_CMD && IsRef(v->t[i].code)) {
            if (n < max)
                codes[n] = v->t[i].code;
            n++;
        }
    return n;
}

/* Replace a European string's references by the USA string's, in order, and
 * put the [::] marker after the same number of line/page breaks. */
static int AdoptUsaStructure(const Toks *usa, Toks *eu)
{
    uint8_t uc[32], ec[32];
    int nu = CountRefs(usa, uc, 32), ne = CountRefs(eu, ec, 32);
    if (nu != ne || nu > 32 || memcmp(uc, ec, (size_t)nu))
        return 0;
    int k = 0;
    for (int i = 0; i < eu->n; i++)
        if (eu->t[i].type == T_CMD && IsRef(eu->t[i].code)) {
            int seen = 0;
            for (int j = 0; j < usa->n; j++)
                if (usa->t[j].type == T_CMD && IsRef(usa->t[j].code) && seen++ == k) {
                    eu->t[i] = usa->t[j];
                    break;
                }
            k++;
        }
    int breaks = -1;
    for (int j = 0, b = 0; j < usa->n; j++) {
        if (usa->t[j].type == T_MARK) {
            breaks = b;
            break;
        }
        b += usa->t[j].type == T_CMD && (usa->t[j].code == CMD_N || usa->t[j].code == CMD_FIN);
    }
    if (breaks >= 0) {
        int at = eu->n, b = 0;
        for (int i = 0; i < eu->n; i++) {
            if (b == breaks) {
                at = i;
                break;
            }
            b += eu->t[i].type == T_CMD && (eu->t[i].code == CMD_N || eu->t[i].code == CMD_FIN);
        }
        Tok mk = {0};
        mk.type = T_MARK;
        mk.ref = -1;
        Push(eu, &mk);
        memmove(&eu->t[at + 1], &eu->t[at], (size_t)(eu->n - 1 - at) * sizeof(Tok));
        eu->t[at] = mk;
    }
    return 1;
}

/* Lay out every label's final text into virtual banks and redirects.
 * tr/has: translated tokens per label (European ROM text when `adopt`,
 * which then takes the USA string's references and marker; a community
 * translation already names them). Consumes the token lists it uses. */
/* A scene title is also measured, by $02:A172, which sizes and centres its
 * box: it counts glyphs, [SKP:n] as n and dictionary words, steps over any
 * other command byte alone and does not follow [PRT]. The stub
 * [PRT:@virtual][SKP:k][RET] makes it measure the new text: the address
 * bytes count as the routine sees them and k makes up the rest (the skip
 * lands after the last letter, so nothing moves on screen). */
static void AddSceneRedirect(IogPack *pack, const Rom *ru, uint32_t off, uint32_t addr,
                             const Toks *v)
{
    int width = 0;
    for (int k = 0; k < v->n; k++)
        if (v->t[k].type == T_GLYPH)
            width++;
        else if (v->t[k].type == T_CMD && v->t[k].code == 0xCC)
            width += (int)v->t[k].a[0];
    uint8_t b[7] = {CMD_PRT, (uint8_t)addr, (uint8_t)(addr >> 8), (uint8_t)(addr >> 16),
                    0xCC, 0, CMD_RET};
    int seen = 0;
    for (int k = 1; k < 4; k++) {
        if (b[k] == CMD_RET || b[k] == 0xCC || b[k] == CMD_LU1 || b[k] == CMD_LU2)
            return;                    /* would derail the measure: keep USA text */
        seen += b[k] < 0xC0;
    }
    if (width < seen || width - seen > 0xFF || SceneTitleLen(ru, off) + 1 < sizeof(b))
        return;
    b[5] = (uint8_t)(width - seen);
    AddPatch(pack, off, b, sizeof(b));
}


/* ---- hand translations ---------------------------------------------------
 * The few dialogue strings with no counterpart in the European ROM (found by
 * IOG_I18N_REPORT=1) are translated here. The USA string's commands are
 * kept (boxes, colours, pauses, line breaks); only its runs of text are
 * replaced, in order, separated by '|'. Glyph codes: the dialogue font
 * (GaiaLabs' WideString map, shiftType h2); the European fonts put their
 * accents on codes $01-$09 (read off the Spanish ROM). */
typedef struct { const char *ch; uint8_t code; } GlyphMap;
static const GlyphMap kGlyphs[] = { {"0",0x20},{"1",0x21},{"2",0x22},{"3",0x23},{"4",0x24},{"5",0x25},{"6",0x26},{"7",0x27},{"8",0x28},{"9",0x29},{".",0x2A},{",",0x2B},{"\"",0x2D},{":",0x2F},{"A",0x40},{"B",0x41},{"C",0x42},{"D",0x43},{"E",0x44},{"F",0x45},{"G",0x46},{"H",0x47},{"I",0x48},{"J",0x49},{"K",0x4A},{"L",0x4B},{"M",0x4C},{"N",0x4D},{"O",0x4E},{"!",0x4F},{"P",0x60},{"Q",0x61},{"R",0x62},{"S",0x63},{"T",0x64},{"U",0x65},{"V",0x66},{"W",0x67},{"X",0x68},{"Y",0x69},{"Z",0x6A},{"/",0x6B},{"*",0x6C},{"-",0x6D},{"(",0x6E},{")",0x6F},{"a",0x80},{"b",0x81},{"c",0x82},{"d",0x83},{"e",0x84},{"f",0x85},{"g",0x86},{"h",0x87},{"i",0x88},{"j",0x89},{"k",0x8A},{"l",0x8B},{"m",0x8C},{"n",0x8D},{"o",0x8E},{";",0x8F},{"p",0xA0},{"q",0xA1},{"r",0xA2},{"s",0xA3},{"t",0xA4},{"u",0xA5},{"v",0xA6},{"w",0xA7},{"x",0xA8},{"y",0xA9},{"z",0xAA},{" ",0xAC},{"?",0x0D},{"'",0x0E} };
static const GlyphMap kEuGlyphs[] = {
    {"é",0x01},{"á",0x02},{"í",0x03},{"ó",0x04},{"ú",0x05},{"ñ",0x06},{"ü",0x07},{"¿",0x08},{"¡",0x09},
    {"É",0x40|4},{"Á",0x40},{"Í",0x48},{"Ó",0x4E},{"Ú",0x65},
};

static int GlyphFor(const char **ps, uint8_t *out)
{
    for (size_t i = 0; i < sizeof(kEuGlyphs) / sizeof(kEuGlyphs[0]); i++) {
        const size_t n = strlen(kEuGlyphs[i].ch);
        if (!strncmp(*ps, kEuGlyphs[i].ch, n)) { *out = kEuGlyphs[i].code; *ps += n; return 1; }
    }
    for (size_t i = 0; i < sizeof(kGlyphs) / sizeof(kGlyphs[0]); i++)
        if (**ps == kGlyphs[i].ch[0]) { *out = kGlyphs[i].code; (*ps)++; return 1; }
    (*ps)++;
    return 0;
}

static const char *GlyphText(uint8_t code)
{
    for (size_t i = 0; i < sizeof(kGlyphs) / sizeof(kGlyphs[0]); i++)
        if (kGlyphs[i].code == code) return kGlyphs[i].ch;
    return "#";
}

/* The runs of text of a string, '|'-separated (for the report). */
static void RunsText(const Toks *v, char *out, size_t cap)
{
    size_t n = 0;
    int in = 0;
    out[0] = 0;
    for (int k = 0; k < v->n && n + 8 < cap; k++) {
        if (v->t[k].type == T_GLYPH) {
            if (!in && n) out[n++] = '|';
            in = 1;
            const char *g = GlyphText(v->t[k].code);
            while (*g && n + 1 < cap) out[n++] = *g++;
        } else if (v->t[k].type == T_CMD) {
            in = 0;
        }
    }
    out[n] = 0;
}

typedef struct { const char *label, *lang, *text; } HandText;
static const HandText kHand[] = {
#include "iog_hand_text.inc"
};

/* Replace the runs of v with the hand translation of `label` in `lang`. */
static int ApplyHand(const char *label, const char *lang, Toks *v)
{
    const char *text = NULL;
    for (size_t i = 0; i < sizeof(kHand) / sizeof(kHand[0]); i++)
        if (!strcmp(kHand[i].label, label) && !strcmp(kHand[i].lang, lang))
            text = kHand[i].text;
    if (!text)
        return 0;
    int runs = 0, parts = 1;
    for (int k = 0, in = 0; k < v->n; k++) {
        if (v->t[k].type == T_GLYPH) { if (!in) runs++; in = 1; }
        else if (v->t[k].type == T_CMD) in = 0;
    }
    for (const char *p = text; *p; p++) parts += *p == '|';
    if (runs != parts) {
        fprintf(stderr, "[i18n] hand text %s/%s: %d parts for %d runs\n", label, lang, parts, runs);
        return 0;
    }
    Toks nv = {0};
    const char *p = text;
    for (int k = 0; k < v->n;) {
        if (v->t[k].type != T_GLYPH) { Push(&nv, &v->t[k]); k++; continue; }
        Tok tpl = v->t[k];
        while (k < v->n && v->t[k].type == T_GLYPH) k++;
        while (*p && *p != '|') {
            uint8_t c;
            if (GlyphFor(&p, &c)) { tpl.code = c; Push(&nv, &tpl); }
        }
        if (*p == '|') p++;
    }
    free(v->t);
    *v = nv;
    return 1;
}

static IogPack *Assemble(const Structure *stp, const Rom *rup, Text *usa_text, const int *usa_ok,
                         Toks *tr, const int *has, int adopt, const char *code, const char *name)
{
    Structure st = *stp;
    Rom ru = *rup;
    const uint8_t *usa = ru.rom;
    size_t usa_size = ru.size;
    /* Final token list of every label: the translation, else the USA text. */
    Item *items = calloc((size_t)st.nl, sizeof(Item));
    int *item_of = malloc((size_t)st.nl * sizeof(int));
    int nitems = 0, translated = 0;
    for (int li = 0; li < st.nl; li++) {
        item_of[li] = -1;
        if (!usa_ok[li])
            continue;
        Item *it = &items[nitems];
        it->li = li;
        it->relocatable = 1;
        if (has[li] && (!adopt || AdoptUsaStructure(&usa_text[li].toks, &tr[li]))) {
            it->toks = tr[li];
            tr[li].t = NULL;
            translated++;
        } else {
            it->toks = usa_text[li].toks;
            usa_text[li].toks.t = NULL;
            if (ApplyHand(st.l[li].name, code, &it->toks)) {
                translated++;
            } else if (getenv("IOG_I18N_REPORT")) {
                int glyphs = 0;
                for (int k = 0; k < it->toks.n; k++)
                    glyphs += it->toks.t[k].type == T_GLYPH;
                if (glyphs > 1) {
                    char runs[1024];
                    RunsText(&it->toks, runs, sizeof(runs));
                    fprintf(stderr, "[i18n] untranslated %s (%s, %d glyphs): %s\n", st.l[li].name,
                            has[li] ? "structure differs" : "no pair", glyphs, runs);
                }
            }
        }
        if (st.l[li].unk16) {
            /* Strip the text's own [RET]; the raw tail follows it in ROM. */
            Toks *v = &it->toks;
            if (v->n && v->t[v->n - 1].type == T_CMD && v->t[v->n - 1].code == CMD_RET)
                v->n--;
            uint32_t p = st.l[li].off;
            Toks tmp = {0};
            Decode(&ru, p, &tmp, 0, 0);
            /* tail: 4 bytes after the USA string's [RET] */
            uint32_t end = p;
            while (end < usa_size && usa[end] != CMD_RET)
                end++;
            memcpy(it->tail, usa + end + 1, 4);
            free(tmp.t);
        }
        item_of[li] = nitems++;
    }

    /* Placement: strings of one USA bank (and its tables) share a virtual
     * bank; iterate until every bank-relative reference resolves. */
    IogPack *pack = calloc(1, sizeof(IogPack));
    snprintf(pack->code, sizeof(pack->code), "%s", code);
    snprintf(pack->name, sizeof(pack->name), "%s", name);
    int *table_bank = malloc((size_t)(st.nt ? st.nt : 1) * sizeof(int));
    int *table_pos = malloc((size_t)(st.nt ? st.nt : 1) * sizeof(int));
    Layout lay = {items, nitems, item_of, table_bank, table_pos, &st};
    uint8_t *scratch = malloc(0x8000);
    for (int pass = 0; pass < 8; pass++) {
        int fill[kIogVirtualBanks] = {0};
        for (int k = 0; k < st.nt; k++)
            table_bank[k] = -1;
        for (int i = 0; i < nitems; i++)
            items[i].placed = 0;
        for (uint32_t bank = 0; bank < 0x40; bank++) {
            int need = 0;
            for (int i = 0; i < nitems; i++)
                if (items[i].relocatable && (st.l[items[i].li].off >> 16) == bank) {
                    int n = Encode(NULL, &items[i], NULL);
                    items[i].len = n + (st.l[items[i].li].unk16 ? 4 : 0);
                    need += items[i].len;
                }
            for (int k = 0; k < st.nt; k++)
                if ((st.t[k].off >> 16) == bank)
                    need += 2 * st.t[k].n;
            if (!need)
                continue;
            int vb = 0;
            while (vb < kIogVirtualBanks && fill[vb] + need > kIogVirtualBankSize)
                vb++;
            if (vb == kIogVirtualBanks)
                break;
            for (int i = 0; i < nitems; i++)
                if (items[i].relocatable && (st.l[items[i].li].off >> 16) == bank) {
                    items[i].bank_index = vb;
                    items[i].pos = fill[vb];
                    items[i].placed = 1;
                    fill[vb] += items[i].len;
                }
            for (int k = 0; k < st.nt; k++)
                if ((st.t[k].off >> 16) == bank) {
                    table_bank[k] = vb;
                    table_pos[k] = fill[vb];
                    fill[vb] += 2 * st.t[k].n;
                }
        }
        int dropped = 0;
        for (int i = 0; i < nitems; i++)
            if (items[i].placed && Encode(&lay, &items[i], scratch) < 0) {
                items[i].relocatable = 0;
                dropped++;
            }
        if (!dropped) {
            for (int vb = 0; vb < kIogVirtualBanks; vb++)
                if (fill[vb])
                    pack->nbanks = vb + 1;
            break;
        }
    }
    for (int vb = 0; vb < pack->nbanks; vb++)
        pack->banks[vb] = calloc(1, kIogVirtualBankSize);
    for (int i = 0; i < nitems; i++)
        if (items[i].placed)
            Encode(&lay, &items[i], pack->banks[items[i].bank_index] + items[i].pos);
    for (int k = 0; k < st.nt; k++) {
        if (table_bank[k] < 0)
            continue;
        uint8_t *t = pack->banks[table_bank[k]] + table_pos[k];
        for (int e = 0; e < st.t[k].n; e++) {
            int li = st.t[k].entry[e];
            int ii = li >= 0 ? item_of[li] : -1;
            uint32_t v = 0;
            if (ii >= 0 && items[ii].placed && items[ii].bank_index == table_bank[k])
                v = kVBase + (uint32_t)items[ii].pos +
                    (st.t[k].entry_m[e] ? (uint32_t)MarkerPos(&items[ii]) : 0);
            else if (li >= 0)
                v = (st.l[li].off + (st.t[k].entry_m[e] ? st.l[li].marker : 0)) & 0xFFFF;
            t[2 * e] = (uint8_t)v;
            t[2 * e + 1] = (uint8_t)(v >> 8);
        }
    }

    /* Redirects: [PRT:@virtual][RET] over each relocated dialogue string
     * with room for it before the next string. */
    for (int i = 0; i < nitems; i++) {
        const Label *l = &st.l[items[i].li];
        if (!items[i].placed || l->unk16)
            continue;
        uint32_t next = l->off + 0x10000;
        for (int j = 0; j < st.nl; j++)
            if (st.l[j].off > l->off && st.l[j].off < next)
                next = st.l[j].off;
        if (next - l->off < 5)
            continue;
        /* ...and within the string itself (code may follow it). */
        if (StringSpan(&ru, l->off) < 5)
            continue;
        uint32_t addr = ((uint32_t)(kVBankFirst + items[i].bank_index) << 16) | (kVBase + (uint32_t)items[i].pos);
        if (l->scene) {
            AddSceneRedirect(pack, &ru, l->off, addr, &items[i].toks);
            continue;
        }
        AddRedirect(pack, l->off, CMD_PRT, addr, CMD_RET);
    }
    pack->strings_translated = translated;
    pack->strings_total = nitems;

    for (int i = 0; i < nitems; i++)
        free(items[i].toks.t);
    free(items); free(item_of); free(table_bank); free(table_pos); free(scratch);
    return pack;
}

IogPack *IogTextBuildPack(const uint8_t *usa, size_t usa_size, const uint8_t *eu,
                          size_t eu_size, const char *structure_path)
{
    const char *lang = IogTextRomLanguage(eu, eu_size);
    Structure st;
    Rom ru, re;
    if (!lang || !LoadStructure(&st, structure_path) || !RomInit(&ru, usa, usa_size) ||
        !RomInit(&re, eu, eu_size)) {
        fprintf(stderr, "[i18n] cannot build a pack (%s)\n",
                !lang ? "not an Illusion of Time ROM" : "structure or text engine not found");
        return NULL;
    }

    int *scene_map = malloc(4096 * sizeof(int));
    for (int i = 0; i < 4096; i++)
        scene_map[i] = -1;
    AddSceneLabels(&st, &ru, scene_map);

    /* USA text of every label, references resolved. */
    Text *usa_text = calloc((size_t)st.nl, sizeof(Text));
    int *usa_ok = calloc((size_t)st.nl, sizeof(int));
    for (int i = 0; i < st.nl; i++) {
        if (Decode(&ru, st.l[i].off, &usa_text[i].toks, 0, 0))
            usa_ok[i] = ResolveUsa(&st, i, &usa_text[i].toks);
        usa_text[i].nskel = Skeleton(&usa_text[i].toks, usa_text[i].skel, 64);
    }

    /* Call sites. */
    enum { kMaxSites = 16384 };
    Site *us = calloc(kMaxSites, sizeof(Site)), *es = calloc(kMaxSites, sizeof(Site));
    /* USA: LDY sites are tried against every bank; only a pointer that names
     * exactly one known string is kept. */
    uint8_t all_banks[0x40], ldy_banks[0x40];
    int nldy = 0;
    for (int b = 0; b < 0x40; b++)
        all_banks[b] = (uint8_t)b;
    int nus_all = Sites(usa, usa_size, us, kMaxSites, all_banks, 0x40), nus = 0;
    for (int i = 0; i < nus_all; i++) {
        int m = -1, count = 0;
        for (int j = 0; j < st.nl; j++)
            if (st.l[j].off == us[i].target && !st.l[j].unk16)
                m = j, count++;
        if (us[i].op == 0xA0) {
            /* the same LDY appears once per bank tried: keep it only if
             * its pointer is unambiguous across banks */
            int total = 0;
            for (int j = 0; j < st.nl; j++)
                total += !st.l[j].unk16 && (st.l[j].off & 0xFFFF) == (us[i].target & 0xFFFF);
            if (total != 1)
                m = -1;
        }
        if (m >= 0 && count == 1) {
            us[nus] = us[i];
            us[nus++].li = m;
            if (us[i].op == 0xA0) {
                uint8_t b = (uint8_t)(us[i].target >> 16);
                int seen = 0;
                for (int k = 0; k < nldy; k++)
                    seen |= ldy_banks[k] == b;
                if (!seen)
                    ldy_banks[nldy++] = b;
            }
        }
    }
    int nes_all = Sites(eu, eu_size, es, kMaxSites, ldy_banks, nldy), nes = 0;
    Text *eu_text = calloc((size_t)nes_all, sizeof(Text));
    for (int i = 0; i < nes_all; i++) {
        Text t = {0};
        if (!Decode(&re, es[i].target, &t.toks, 0, 0)) {
            free(t.toks.t);
            continue;
        }
        t.nskel = Skeleton(&t.toks, t.skel, 64);
        eu_text[nes] = t;
        es[nes] = es[i];
        es[nes].li = nes;
        nes++;
    }

    /* Banded monotonic alignment. */
    const int band = 250;
    int *best = calloc((size_t)(nus + 1) * (size_t)(nes + 1), sizeof(int));
    uint8_t *move = calloc((size_t)(nus + 1) * (size_t)(nes + 1), 1);
#define B(i, j) best[(size_t)(i) * (size_t)(nes + 1) + (size_t)(j)]
#define M(i, j) move[(size_t)(i) * (size_t)(nes + 1) + (size_t)(j)]
    for (int i = 1; i <= nus; i++) {
        int centre = nes ? (int)((long)i * nes / nus) : 0;
        for (int j = 1; j <= nes; j++) {
            int v = B(i - 1, j), mv = 1;
            if (B(i, j - 1) > v)
                v = B(i, j - 1), mv = 2;
            if (j >= centre - band && j <= centre + band) {
                int sc = Score(&ru, &us[i - 1], &usa_text[us[i - 1].li], &re, &es[j - 1],
                               &eu_text[j - 1]);
                if (sc >= 0 && B(i - 1, j - 1) + sc > v)
                    v = B(i - 1, j - 1) + sc, mv = 3;
            }
            B(i, j) = v;
            M(i, j) = (uint8_t)mv;
        }
    }
    Toks *tr = calloc((size_t)st.nl, sizeof(Toks));   /* translated tokens */
    int *has = calloc((size_t)st.nl, sizeof(int));
    for (int i = nus, j = nes; i && j;) {
        int mv = M(i, j);
        if (mv == 3) {
            int li = us[i - 1].li;
            if (!has[li]) {
                Toks c = {0};
                for (int k = 0; k < eu_text[j - 1].toks.n; k++)
                    Push(&c, &eu_text[j - 1].toks.t[k]);
                tr[li] = c;
                has[li] = 1;
            }
            i--, j--;
        } else if (mv == 1) {
            i--;
        } else {
            j--;
        }
    }
#undef B
#undef M
    free(best);
    free(move);

    /* Follow references in step: a USA reference and the European one in the
     * same position name the same string (or table). */
    for (int changed = 1; changed;) {
        changed = 0;
        for (int li = 0; li < st.nl; li++) {
            if (!has[li] || !usa_ok[li])
                continue;
            uint32_t bank = st.l[li].off & 0xFF0000;
            const Toks *u = &usa_text[li].toks;
            const Toks *e = &tr[li];
            int ku = 0;
            for (int i = 0; i < e->n; i++) {
                const Tok *et = &e->t[i];
                if (et->type != T_CMD || !IsRef(et->code) || et->ref >= 0)
                    continue;
                const Tok *ut = NULL;
                for (int j = 0, seen = 0; j < u->n; j++)
                    if (u->t[j].type == T_CMD && IsRef(u->t[j].code) && seen++ == ku) {
                        ut = &u->t[j];
                        break;
                    }
                ku++;
                if (!ut || ut->code != et->code || ut->ref < 0 || ut->ref_m)
                    continue;
                int targets[256], nt = 0;
                uint32_t eoffs[256];
                if (et->code == CMD_ADR) {
                    const Table *t = &st.t[ut->ref];
                    uint32_t etab = bank | et->a[0];
                    for (int k = 0; k < t->n && nt < 256; k++)
                        if (t->entry[k] >= 0 && !t->entry_m[k]) {
                            targets[nt] = t->entry[k];
                            eoffs[nt++] = bank | Rd16(&re, etab + 2 * (uint32_t)k);
                        }
                } else {
                    targets[0] = ut->ref;
                    eoffs[0] = et->code == CMD_JMP ? (bank | et->a[0])
                                                   : ((et->a[0] & 0x3F0000) | (et->a[0] & 0xFFFF));
                    nt = 1;
                }
                for (int k = 0; k < nt; k++) {
                    int tl = targets[k];
                    if (tl < 0 || has[tl])
                        continue;
                    Toks c = {0};
                    if (!Decode(&re, eoffs[k], &c, 0, 0) || (st.l[tl].unk16 && c.n == 0)) {
                        free(c.t);
                        continue;
                    }
                    if (st.l[tl].unk16 && c.n && c.t[c.n - 1].type == T_CMD &&
                        c.t[c.n - 1].code == CMD_RET)
                        c.n--;
                    tr[tl] = c;
                    has[tl] = 1;
                    changed = 1;
                }
            }
        }
    }

    /* Scene titles: paired by map index. */
    for (int li = 0; li < st.nl; li++) {
        if (scene_map[li] < 0 || has[li])
            continue;
        uint32_t off = SceneTitle(&re, &st, scene_map[li]);
        Toks c = {0};
        if (off && SceneTitleLen(&re, off) && Decode(&re, off, &c, 0, 0)) {
            tr[li] = c;
            has[li] = 1;
        } else {
            free(c.t);
        }
    }
    free(scene_map);

    IogPack *pack = Assemble(&st, &ru, usa_text, usa_ok, tr, has, 1, lang, LanguageName(lang));
    AddFont(&st, &ru, &re, pack);
    AddAscii(&st, &ru, &re, pack);
    AddSprites(&st, &ru, &re, pack);
    for (int i = 0; i < st.nl; i++) {
        free(usa_text[i].toks.t);
        free(tr[i].t);
    }
    for (int i = 0; i < nes; i++)
        free(eu_text[i].toks.t);
    free(usa_text); free(usa_ok); free(us); free(es); free(eu_text); free(tr); free(has);
    free(st.l); free(st.t); free(st.ascii);
    return pack;
}

