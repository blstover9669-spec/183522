/*
 * fun_chess.c - Chess (Games).
 *
 * Full legal-move chess: castling (with all the "not through check" rules),
 * en passant, promotion (choose the piece), check, checkmate, stalemate, and
 * draws by the fifty-move rule, threefold repetition and insufficient
 * material. Play against the computer (Easy / Medium / Hard, as White or
 * Black) or two players on one machine.
 *
 *   - click a piece to see its legal moves, then click a target square; or
 *     drag and drop. Last move, check and legal targets are highlighted, the
 *     move list is written in standard algebraic notation (Nf3, exd5, O-O,
 *     e8=Q+, Qh7#);
 *   - Undo takes back your last move (and the computer's reply), Flip turns
 *     the board, captured pieces and the material balance are shown;
 *   - keyboard: arrows + Space/Enter move a cursor and pick squares, U or
 *     Backspace undo, F flip, N/F2 new game, Esc cancels a selection;
 *   - pieces are vector silhouettes rasterised at 4x into coverage masks,
 *     outlined with a distance transform and shaded, then downsampled - no
 *     image files.
 *
 * The engine: 0x88 board, Zobrist hashing, legal move generation by
 * make/unmake, a material + piece-square-table evaluation (tapered king
 * tables, passed pawns, bishop pair, mop-up for lone kings) and a negamax
 * alpha-beta search with quiescence, killer moves, MVV-LVA ordering and
 * iterative deepening. The search keeps its own explicit stack instead of
 * recursing, so it can stop after a few milliseconds and resume on the next
 * window timer tick: the computer thinks in the background and the desktop
 * never freezes. Easy searches 1 ply with noise, Medium 3 plies, Hard deepens
 * up to 6 plies within a time limit (4 under QEMU's emulated CPU).
 */
#include "fun_common.h"

/* ======================================================================================
 * Rules
 * ==================================================================================== */
#define WHITE 0
#define BLACK 1
enum { EMPTY, PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING };

#define MKP(c, t)   ((int8_t)((t) | ((c) << 3)))
#define PTYPE(p)    ((p) & 7)
#define PCOL(p)     (((p) >> 3) & 1)
#define SQ(r, f)    ((r) * 16 + (f))
#define RANK(s)     ((s) >> 4)
#define FILEOF(s)   ((s) & 7)
#define ONBOARD(s)  (!((s) & 0x88))

#define MF_CAP    1
#define MF_EP     2
#define MF_CASTLE 4
#define MF_DOUBLE 8
#define MF_PROMO  16

typedef struct { uint8_t from, to, promo, flags; } cmove_t;

typedef struct {
    int8_t b[128];
    int side, castle, ep, halfmove, fullmove;   /* castle: 1 = K, 2 = Q, 4 = k, 8 = q */
    int ksq[2];
    uint64_t hash;
} pos_t;

typedef struct {
    int8_t captured;
    uint8_t castle;
    int8_t ep;
    int16_t halfmove;
    uint64_t hash;
} undo_t;

static const int knight_off[8] = { 33, 31, 18, 14, -14, -18, -31, -33 };
static const int king_off[8] = { 1, -1, 16, -16, 15, 17, -15, -17 };
static const int diag_dir[4] = { 15, 17, -15, -17 };
static const int orth_dir[4] = { 1, -1, 16, -16 };

static uint64_t Zp[16][128], Zc[16], Zep[8], Zside;
static bool z_ready;

static uint64_t splitmix(uint64_t *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static void z_init(void) {
    if (z_ready) return;
    uint64_t s = 0x6E6F76615F636873ull;
    for (int p = 0; p < 16; p++)
        for (int q = 0; q < 128; q++) Zp[p][q] = splitmix(&s);
    for (int i = 0; i < 16; i++) Zc[i] = splitmix(&s);
    for (int i = 0; i < 8; i++) Zep[i] = splitmix(&s);
    Zside = splitmix(&s);
    z_ready = true;
}

static uint64_t pos_hash(const pos_t *p) {
    uint64_t h = 0;
    for (int sq = 0; sq < 128; sq++)
        if (ONBOARD(sq) && p->b[sq]) h ^= Zp[(int)p->b[sq]][sq];
    h ^= Zc[p->castle];
    if (p->ep >= 0) h ^= Zep[FILEOF(p->ep)];
    if (p->side == BLACK) h ^= Zside;
    return h;
}

static void pos_start(pos_t *p) {
    static const int back[8] = { ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK };
    z_init();
    memset(p, 0, sizeof *p);
    for (int f = 0; f < 8; f++) {
        p->b[SQ(0, f)] = MKP(WHITE, back[f]);
        p->b[SQ(1, f)] = MKP(WHITE, PAWN);
        p->b[SQ(6, f)] = MKP(BLACK, PAWN);
        p->b[SQ(7, f)] = MKP(BLACK, back[f]);
    }
    p->side = WHITE;
    p->castle = 15;
    p->ep = -1;
    p->fullmove = 1;
    p->ksq[WHITE] = SQ(0, 4);
    p->ksq[BLACK] = SQ(7, 4);
    p->hash = pos_hash(p);
}

/* Is square sq attacked by side `by`? */
static bool attacked(const pos_t *p, int sq, int by) {
    const int8_t *b = p->b;
    int s;
    if (by == WHITE) {
        s = sq - 15; if (ONBOARD(s) && b[s] == MKP(WHITE, PAWN)) return true;
        s = sq - 17; if (ONBOARD(s) && b[s] == MKP(WHITE, PAWN)) return true;
    } else {
        s = sq + 15; if (ONBOARD(s) && b[s] == MKP(BLACK, PAWN)) return true;
        s = sq + 17; if (ONBOARD(s) && b[s] == MKP(BLACK, PAWN)) return true;
    }
    int8_t kn = MKP(by, KNIGHT), kg = MKP(by, KING);
    for (int i = 0; i < 8; i++) {
        s = sq + knight_off[i];
        if (ONBOARD(s) && b[s] == kn) return true;
        s = sq + king_off[i];
        if (ONBOARD(s) && b[s] == kg) return true;
    }
    int8_t bi = MKP(by, BISHOP), rk = MKP(by, ROOK), qn = MKP(by, QUEEN);
    for (int i = 0; i < 4; i++) {
        for (s = sq + diag_dir[i]; ONBOARD(s); s += diag_dir[i]) {
            if (b[s]) { if (b[s] == bi || b[s] == qn) return true; break; }
        }
        for (s = sq + orth_dir[i]; ONBOARD(s); s += orth_dir[i]) {
            if (b[s]) { if (b[s] == rk || b[s] == qn) return true; break; }
        }
    }
    return false;
}

static bool in_check(const pos_t *p) { return attacked(p, p->ksq[p->side], p->side ^ 1); }

static inline void add_move(cmove_t *mv, int *n, int from, int to, int promo, int flags) {
    mv[*n].from = (uint8_t)from;
    mv[*n].to = (uint8_t)to;
    mv[*n].promo = (uint8_t)promo;
    mv[*n].flags = (uint8_t)flags;
    (*n)++;
}

static void add_promos(cmove_t *mv, int *n, int from, int to, int flags, bool noisy) {
    add_move(mv, n, from, to, QUEEN, flags | MF_PROMO);
    if (noisy) return;
    add_move(mv, n, from, to, KNIGHT, flags | MF_PROMO);
    add_move(mv, n, from, to, ROOK, flags | MF_PROMO);
    add_move(mv, n, from, to, BISHOP, flags | MF_PROMO);
}

/* Pseudo-legal moves (may leave the king in check). noisy = captures and queen
   promotions only (quiescence search). */
static int gen_moves(const pos_t *p, cmove_t *mv, bool noisy) {
    int n = 0, us = p->side, them = us ^ 1;
    const int8_t *b = p->b;
    for (int sq = 0; sq < 128; sq++) {
        if (sq & 0x88) { sq += 7; continue; }
        int pc = b[sq];
        if (!pc || PCOL(pc) != us) continue;
        int t = PTYPE(pc);
        if (t == PAWN) {
            int fwd = us == WHITE ? 16 : -16, start = us == WHITE ? 1 : 6, last = us == WHITE ? 7 : 0;
            int to = sq + fwd;
            if (ONBOARD(to) && !b[to]) {
                if (RANK(to) == last) add_promos(mv, &n, sq, to, 0, noisy);
                else if (!noisy) {
                    add_move(mv, &n, sq, to, 0, 0);
                    if (RANK(sq) == start && !b[to + fwd]) add_move(mv, &n, sq, to + fwd, 0, MF_DOUBLE);
                }
            }
            for (int k = -1; k <= 1; k += 2) {
                int c = to + k;
                if (!ONBOARD(c)) continue;
                if (b[c] && PCOL(b[c]) == them) {
                    if (RANK(c) == last) add_promos(mv, &n, sq, c, MF_CAP, noisy);
                    else add_move(mv, &n, sq, c, 0, MF_CAP);
                } else if (c == p->ep) {
                    add_move(mv, &n, sq, c, 0, MF_CAP | MF_EP);
                }
            }
        } else if (t == KNIGHT || t == KING) {
            const int *off = t == KNIGHT ? knight_off : king_off;
            for (int i = 0; i < 8; i++) {
                int to = sq + off[i];
                if (!ONBOARD(to)) continue;
                if (!b[to]) { if (!noisy) add_move(mv, &n, sq, to, 0, 0); }
                else if (PCOL(b[to]) == them) add_move(mv, &n, sq, to, 0, MF_CAP);
            }
        } else {
            for (int pass = 0; pass < 2; pass++) {
                const int *dir = pass ? orth_dir : diag_dir;
                if (pass == 0 && t == ROOK) continue;
                if (pass == 1 && t == BISHOP) continue;
                for (int i = 0; i < 4; i++) {
                    for (int to = sq + dir[i]; ONBOARD(to); to += dir[i]) {
                        if (!b[to]) { if (!noisy) add_move(mv, &n, sq, to, 0, 0); continue; }
                        if (PCOL(b[to]) == them) add_move(mv, &n, sq, to, 0, MF_CAP);
                        break;
                    }
                }
            }
        }
    }
    /* castling: the king may not be in check, pass through or land on an attacked square */
    if (!noisy) {
        int r = us == WHITE ? 0 : 7, ks = SQ(r, 4);
        int kbit = us == WHITE ? 1 : 4, qbit = us == WHITE ? 2 : 8;
        if ((p->castle & (kbit | qbit)) && p->ksq[us] == ks && !attacked(p, ks, them)) {
            if ((p->castle & kbit) && !b[ks + 1] && !b[ks + 2] && b[ks + 3] == MKP(us, ROOK) &&
                !attacked(p, ks + 1, them) && !attacked(p, ks + 2, them))
                add_move(mv, &n, ks, ks + 2, 0, MF_CASTLE);
            if ((p->castle & qbit) && !b[ks - 1] && !b[ks - 2] && !b[ks - 3] && b[ks - 4] == MKP(us, ROOK) &&
                !attacked(p, ks - 1, them) && !attacked(p, ks - 2, them))
                add_move(mv, &n, ks, ks - 2, 0, MF_CASTLE);
        }
    }
    return n;
}

static int castle_mask(int sq) {
    switch (sq) {
    case 0x00: return 15 & ~2;
    case 0x04: return 15 & ~3;
    case 0x07: return 15 & ~1;
    case 0x70: return 15 & ~8;
    case 0x74: return 15 & ~12;
    case 0x77: return 15 & ~4;
    }
    return 15;
}

static void make_move(pos_t *p, cmove_t m, undo_t *u) {
    int us = p->side;
    int8_t pc = p->b[m.from];
    u->castle = (uint8_t)p->castle;
    u->ep = (int8_t)p->ep;
    u->halfmove = (int16_t)p->halfmove;
    u->hash = p->hash;
    uint64_t h = p->hash;
    int capsq = m.to;
    if (m.flags & MF_EP) capsq = m.to + (us == WHITE ? -16 : 16);
    u->captured = (m.flags & MF_CAP) ? p->b[capsq] : 0;
    if (u->captured) {
        h ^= Zp[(int)u->captured][capsq];
        p->b[capsq] = 0;
    }
    h ^= Zp[(int)pc][m.from];
    int8_t placed = (m.flags & MF_PROMO) ? MKP(us, m.promo) : pc;
    p->b[m.from] = 0;
    p->b[m.to] = placed;
    h ^= Zp[(int)placed][m.to];
    if (m.flags & MF_CASTLE) {
        int rf = m.to > m.from ? m.from + 3 : m.from - 4, rt = m.to > m.from ? m.from + 1 : m.from - 1;
        int8_t rook = p->b[rf];
        p->b[rf] = 0;
        p->b[rt] = rook;
        h ^= Zp[(int)rook][rf] ^ Zp[(int)rook][rt];
    }
    if (PTYPE(pc) == KING) p->ksq[us] = m.to;
    int nc = p->castle & castle_mask(m.from) & castle_mask(m.to);
    h ^= Zc[p->castle] ^ Zc[nc];
    p->castle = nc;
    if (p->ep >= 0) h ^= Zep[FILEOF(p->ep)];
    p->ep = (m.flags & MF_DOUBLE) ? (m.from + m.to) / 2 : -1;
    if (p->ep >= 0) h ^= Zep[FILEOF(p->ep)];
    p->halfmove = (PTYPE(pc) == PAWN || u->captured) ? 0 : p->halfmove + 1;
    if (us == BLACK) p->fullmove++;
    p->side ^= 1;
    h ^= Zside;
    p->hash = h;
}

static void unmake_move(pos_t *p, cmove_t m, const undo_t *u) {
    p->side ^= 1;
    int us = p->side;
    if (us == BLACK) p->fullmove--;
    int8_t pc = p->b[m.to];
    if (m.flags & MF_PROMO) pc = MKP(us, PAWN);
    p->b[m.from] = pc;
    p->b[m.to] = 0;
    if (u->captured) {
        int capsq = (m.flags & MF_EP) ? m.to + (us == WHITE ? -16 : 16) : m.to;
        p->b[capsq] = u->captured;
    }
    if (m.flags & MF_CASTLE) {
        int rf = m.to > m.from ? m.from + 3 : m.from - 4, rt = m.to > m.from ? m.from + 1 : m.from - 1;
        p->b[rf] = p->b[rt];
        p->b[rt] = 0;
    }
    if (PTYPE(pc) == KING) p->ksq[us] = m.from;
    p->castle = u->castle;
    p->ep = u->ep;
    p->halfmove = u->halfmove;
    p->hash = u->hash;
}

static int gen_legal(pos_t *p, cmove_t *out) {
    cmove_t tmp[256];
    int n = gen_moves(p, tmp, false), k = 0;
    for (int i = 0; i < n; i++) {
        undo_t u;
        make_move(p, tmp[i], &u);
        if (!attacked(p, p->ksq[p->side ^ 1], p->side)) out[k++] = tmp[i];
        unmake_move(p, tmp[i], &u);
    }
    return k;
}

static bool same_move(cmove_t a, cmove_t b) {
    return a.from == b.from && a.to == b.to && a.promo == b.promo;
}

/* Dead positions: K v K, K+minor v K, K+B v K+B with bishops on the same colour. */
static bool insufficient_material(const pos_t *p) {
    int minors = 0, knights = 0, bishops[2] = { 0, 0 }, bcol[2] = { -1, -1 };
    for (int sq = 0; sq < 128; sq++) {
        if (!ONBOARD(sq) || !p->b[sq]) continue;
        int t = PTYPE(p->b[sq]), c = PCOL(p->b[sq]);
        if (t == PAWN || t == ROOK || t == QUEEN) return false;
        if (t == KNIGHT) { minors++; knights++; }
        if (t == BISHOP) { minors++; bishops[c]++; bcol[c] = (RANK(sq) + FILEOF(sq)) & 1; }
    }
    if (minors <= 1) return true;
    return minors == 2 && !knights && bishops[0] == 1 && bishops[1] == 1 && bcol[0] == bcol[1];
}

/* ---- FEN (Forsyth-Edwards Notation) ------------------------------------------------- */
static int fen_piece(char ch) {
    switch (ch | 32) {
    case 'p': return PAWN;
    case 'n': return KNIGHT;
    case 'b': return BISHOP;
    case 'r': return ROOK;
    case 'q': return QUEEN;
    case 'k': return KING;
    }
    return 0;
}

static bool fen_sep(char c) { return c == ' ' || c == '_' || c == '\t'; }

/* Parse a FEN position (fields separated by spaces or '_'; the move counters are
   optional). Returns false and leaves *out untouched when the text is not a legal
   position: one king each, no pawns on the back ranks, the side that just moved not
   in check. Castling rights and the en-passant square the board contradicts are
   dropped. */
static bool pos_from_fen(pos_t *out, const char *fen) {
    pos_t p;
    z_init();
    memset(&p, 0, sizeof p);
    const char *c = fen;
    while (*c && (fen_sep(*c) || *c == '\n' || *c == '\r')) c++;
    int r = 7, f = 0, kings[2] = { 0, 0 };
    for (; *c && !fen_sep(*c) && *c != '\n' && *c != '\r'; c++) {
        if (*c == '/') {
            if (f != 8 || r == 0) return false;
            r--;
            f = 0;
            continue;
        }
        if (*c >= '1' && *c <= '8') {
            f += *c - '0';
            if (f > 8) return false;
            continue;
        }
        int t = fen_piece(*c);
        if (!t || f > 7) return false;
        int col = (*c >= 'a') ? BLACK : WHITE;
        if (t == PAWN && (r == 0 || r == 7)) return false;
        p.b[SQ(r, f)] = MKP(col, t);
        if (t == KING) { kings[col]++; p.ksq[col] = SQ(r, f); }
        f++;
    }
    if (r != 0 || f != 8 || kings[WHITE] != 1 || kings[BLACK] != 1) return false;
    while (fen_sep(*c)) c++;
    p.side = *c == 'b' ? BLACK : WHITE;
    if (*c == 'w' || *c == 'b') c++;
    while (fen_sep(*c)) c++;
    for (; *c && !fen_sep(*c) && *c != '\n'; c++) {
        if (*c == 'K') p.castle |= 1;
        else if (*c == 'Q') p.castle |= 2;
        else if (*c == 'k') p.castle |= 4;
        else if (*c == 'q') p.castle |= 8;
    }
    if (p.b[0x04] != MKP(WHITE, KING)) p.castle &= ~3;
    if (p.b[0x07] != MKP(WHITE, ROOK)) p.castle &= ~1;
    if (p.b[0x00] != MKP(WHITE, ROOK)) p.castle &= ~2;
    if (p.b[0x74] != MKP(BLACK, KING)) p.castle &= ~12;
    if (p.b[0x77] != MKP(BLACK, ROOK)) p.castle &= ~4;
    if (p.b[0x70] != MKP(BLACK, ROOK)) p.castle &= ~8;
    while (fen_sep(*c)) c++;
    p.ep = -1;
    if (*c >= 'a' && *c <= 'h' && (c[1] == '3' || c[1] == '6')) {
        int ep = SQ(c[1] - '1', *c - 'a');
        int pawn = p.side == WHITE ? ep - 16 : ep + 16;   /* the pawn that just made the double step */
        if (RANK(ep) == (p.side == WHITE ? 5 : 2) && !p.b[ep] && p.b[pawn] == MKP(p.side ^ 1, PAWN)) p.ep = ep;
        c += 2;
    } else if (*c == '-') {
        c++;
    }
    while (fen_sep(*c)) c++;
    p.halfmove = 0;
    p.fullmove = 1;
    if (isdigit((unsigned char)*c)) {
        p.halfmove = CLAMP(atoi(c), 0, 100);
        while (isdigit((unsigned char)*c)) c++;
        while (fen_sep(*c)) c++;
        if (isdigit((unsigned char)*c)) p.fullmove = CLAMP(atoi(c), 1, 9999);
    }
    if (attacked(&p, p.ksq[p.side ^ 1], p.side)) return false;
    p.hash = pos_hash(&p);
    *out = p;
    return true;
}

static void pos_to_fen(const pos_t *p, char *out, int size) {
    char board[80], cs[5] = "-", ep[3] = "-";
    int k = 0;
    for (int r = 7; r >= 0; r--) {
        int empty = 0;
        for (int f = 0; f < 8; f++) {
            int pc = p->b[SQ(r, f)];
            if (!pc) { empty++; continue; }
            if (empty) { board[k++] = (char)('0' + empty); empty = 0; }
            char ch = " pnbrqk"[PTYPE(pc)];
            board[k++] = PCOL(pc) == WHITE ? (char)toupper(ch) : ch;
        }
        if (empty) board[k++] = (char)('0' + empty);
        if (r) board[k++] = '/';
    }
    board[k] = 0;
    int ci = 0;
    if (p->castle & 1) cs[ci++] = 'K';
    if (p->castle & 2) cs[ci++] = 'Q';
    if (p->castle & 4) cs[ci++] = 'k';
    if (p->castle & 8) cs[ci++] = 'q';
    if (ci) cs[ci] = 0;
    if (p->ep >= 0) { ep[0] = (char)('a' + FILEOF(p->ep)); ep[1] = (char)('1' + RANK(p->ep)); ep[2] = 0; }
    snprintf(out, (size_t)size, "%s %c %s %s %d %d", board, p->side == WHITE ? 'w' : 'b', cs, ep, p->halfmove,
             p->fullmove);
}

/* Standard algebraic notation of m in position p (p is restored afterwards). */
static void move_san(pos_t *p, cmove_t m, char *out) {
    char *o = out;
    int t = PTYPE(p->b[m.from]);
    if (m.flags & MF_CASTLE) {
        strcpy(o, FILEOF(m.to) == 6 ? "O-O" : "O-O-O");
        o += strlen(o);
    } else {
        if (t == PAWN) {
            if (m.flags & MF_CAP) { *o++ = (char)('a' + FILEOF(m.from)); *o++ = 'x'; }
        } else {
            *o++ = "  NBRQK"[t];
            cmove_t lg[256];
            int n = gen_legal(p, lg);
            bool amb = false, same_file = false, same_rank = false;
            for (int i = 0; i < n; i++) {
                if (lg[i].to != m.to || lg[i].from == m.from || PTYPE(p->b[lg[i].from]) != t) continue;
                amb = true;
                if (FILEOF(lg[i].from) == FILEOF(m.from)) same_file = true;
                if (RANK(lg[i].from) == RANK(m.from)) same_rank = true;
            }
            if (amb) {
                if (!same_file) *o++ = (char)('a' + FILEOF(m.from));
                else if (!same_rank) *o++ = (char)('1' + RANK(m.from));
                else { *o++ = (char)('a' + FILEOF(m.from)); *o++ = (char)('1' + RANK(m.from)); }
            }
            if (m.flags & MF_CAP) *o++ = 'x';
        }
        *o++ = (char)('a' + FILEOF(m.to));
        *o++ = (char)('1' + RANK(m.to));
        if (m.flags & MF_PROMO) { *o++ = '='; *o++ = "  NBRQ"[m.promo]; }
    }
    undo_t u;
    make_move(p, m, &u);
    if (in_check(p)) {
        cmove_t lg[256];
        *o++ = gen_legal(p, lg) ? '+' : '#';
    }
    unmake_move(p, m, &u);
    *o = 0;
}

/* ======================================================================================
 * Evaluation (centipawns, from the side to move's point of view)
 * Piece-square tables from White's side, a8 first (Tomasz Michniewski's
 * "simplified evaluation function").
 * ==================================================================================== */
static const int piece_val[7] = { 0, 100, 320, 330, 500, 900, 0 };

static const int8_t pst[7][64] = {
    { 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 50, 50, 50, 50, 50, 50, 50, 50, 10, 10, 20, 30, 30, 20, 10, 10,
      5, 5, 10, 25, 25, 10, 5, 5, 0, 0, 0, 20, 20, 0, 0, 0, 5, -5, -10, 0, 0, -10, -5, 5,
      5, 10, 10, -20, -20, 10, 10, 5, 0, 0, 0, 0, 0, 0, 0, 0 },
    { -50, -40, -30, -30, -30, -30, -40, -50, -40, -20, 0, 0, 0, 0, -20, -40, -30, 0, 10, 15, 15, 10, 0, -30,
      -30, 5, 15, 20, 20, 15, 5, -30, -30, 0, 15, 20, 20, 15, 0, -30, -30, 5, 10, 15, 15, 10, 5, -30,
      -40, -20, 0, 5, 5, 0, -20, -40, -50, -40, -30, -30, -30, -30, -40, -50 },
    { -20, -10, -10, -10, -10, -10, -10, -20, -10, 0, 0, 0, 0, 0, 0, -10, -10, 0, 5, 10, 10, 5, 0, -10,
      -10, 5, 5, 10, 10, 5, 5, -10, -10, 0, 10, 10, 10, 10, 0, -10, -10, 10, 10, 10, 10, 10, 10, -10,
      -10, 5, 0, 0, 0, 0, 5, -10, -20, -10, -10, -10, -10, -10, -10, -20 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 5, 10, 10, 10, 10, 10, 10, 5, -5, 0, 0, 0, 0, 0, 0, -5,
      -5, 0, 0, 0, 0, 0, 0, -5, -5, 0, 0, 0, 0, 0, 0, -5, -5, 0, 0, 0, 0, 0, 0, -5,
      -5, 0, 0, 0, 0, 0, 0, -5, 0, 0, 0, 5, 5, 0, 0, 0 },
    { -20, -10, -10, -5, -5, -10, -10, -20, -10, 0, 0, 0, 0, 0, 0, -10, -10, 0, 5, 5, 5, 5, 0, -10,
      -5, 0, 5, 5, 5, 5, 0, -5, 0, 0, 5, 5, 5, 5, 0, -5, -10, 5, 5, 5, 5, 5, 0, -10,
      -10, 0, 5, 0, 0, 0, 0, -10, -20, -10, -10, -5, -5, -10, -10, -20 },
    { -30, -40, -40, -50, -50, -40, -40, -30, -30, -40, -40, -50, -50, -40, -40, -30,
      -30, -40, -40, -50, -50, -40, -40, -30, -30, -40, -40, -50, -50, -40, -40, -30,
      -20, -30, -30, -40, -40, -30, -30, -20, -10, -20, -20, -20, -20, -20, -20, -10,
      20, 20, 0, 0, 0, 0, 20, 20, 20, 30, 10, 0, 0, 10, 30, 20 },
};

static const int8_t king_end[64] = {
    -50, -40, -30, -20, -20, -30, -40, -50, -30, -20, -10, 0, 0, -10, -20, -30,
    -30, -10, 20, 30, 30, 20, -10, -30, -30, -10, 30, 40, 40, 30, -10, -30,
    -30, -10, 30, 40, 40, 30, -10, -30, -30, -10, 20, 30, 30, 20, -10, -30,
    -30, -30, 0, 0, 0, 0, -30, -30, -50, -30, -30, -30, -30, -30, -30, -50,
};

static const int passed_bonus[8] = { 0, 5, 10, 20, 35, 60, 100, 0 };

static int evaluate(const pos_t *p) {
    int score[2] = { 0, 0 }, mat[2] = { 0, 0 }, bishops[2] = { 0, 0 }, phase = 0;
    int pmax[2][8], pmin[2][8];   /* highest / lowest pawn rank per file */
    for (int f = 0; f < 8; f++) { pmax[0][f] = pmax[1][f] = -1; pmin[0][f] = pmin[1][f] = 8; }
    for (int sq = 0; sq < 128; sq++) {
        if (sq & 0x88) { sq += 7; continue; }
        int pc = p->b[sq];
        if (!pc) continue;
        int t = PTYPE(pc), c = PCOL(pc), r = RANK(sq), f = FILEOF(sq);
        int idx = c == WHITE ? (7 - r) * 8 + f : r * 8 + f;
        if (t == KING) continue;
        score[c] += piece_val[t] + pst[t][idx];
        mat[c] += piece_val[t];
        if (t == PAWN) {
            if (r > pmax[c][f]) pmax[c][f] = r;
            if (r < pmin[c][f]) pmin[c][f] = r;
        } else {
            phase += t == QUEEN ? 4 : (t == ROOK ? 2 : 1);
        }
        if (t == BISHOP) bishops[c]++;
    }
    if (phase > 24) phase = 24;
    /* passed pawns, worth more as the board empties */
    for (int sq = 0; sq < 128; sq++) {
        if (sq & 0x88) { sq += 7; continue; }
        int pc = p->b[sq];
        if (PTYPE(pc) != PAWN) continue;
        int c = PCOL(pc), r = RANK(sq), f = FILEOF(sq);
        bool passed = true;
        for (int ff = MAX(0, f - 1); ff <= MIN(7, f + 1) && passed; ff++) {
            if (c == WHITE && pmax[BLACK][ff] > r) passed = false;
            if (c == BLACK && pmin[WHITE][ff] < r) passed = false;
        }
        if (passed) {
            int adv = c == WHITE ? r : 7 - r;
            score[c] += passed_bonus[adv] * (32 - phase) / 24;
        }
    }
    for (int c = 0; c < 2; c++) {
        int k = p->ksq[c], r = RANK(k), f = FILEOF(k);
        int idx = c == WHITE ? (7 - r) * 8 + f : r * 8 + f;
        score[c] += (pst[KING][idx] * phase + king_end[idx] * (24 - phase)) / 24;
        if (bishops[c] >= 2) score[c] += 30;
    }
    /* mop-up: with a big material lead and few pieces left, drive the enemy king to the
       edge and bring our own king closer (lets the engine finish K+Q v K and the like) */
    int lead = mat[WHITE] - mat[BLACK];
    if (phase <= 8 && (lead >= 400 || lead <= -400)) {
        int w = lead > 0 ? WHITE : BLACK, l = w ^ 1;
        int lk = p->ksq[l], wk = p->ksq[w];
        int cd = abs(2 * FILEOF(lk) - 7) + abs(2 * RANK(lk) - 7);   /* 2..14 */
        int kd = abs(FILEOF(lk) - FILEOF(wk)) + abs(RANK(lk) - RANK(wk));
        score[w] += cd * 5 + (14 - kd) * 6;
    }
    int s = score[WHITE] - score[BLACK];
    return (p->side == WHITE ? s : -s) + 10;   /* small bonus for having the move */
}

/* ======================================================================================
 * Search: negamax alpha-beta with quiescence, run from an explicit stack so it can be
 * paused at any node and resumed on the next timer tick.
 * ==================================================================================== */
#define MAXPLY  32
#define INF     32000
#define MATE    30000

typedef struct {
    cmove_t mv[256];
    int16_t sc[256];
    int n, i, legal;
    int alpha, beta, best, depth;
    bool qs, check, entered;
    cmove_t cur;
    undo_t undo;
} frame_t;

typedef struct {
    pos_t pos;
    frame_t st[MAXPLY];
    int sp;                       /* top frame, -1 between iterations   */
    int depth, max_depth;         /* current iteration / limit          */
    cmove_t best;                 /* result of the last finished iteration */
    int best_score;
    bool have_best;
    cmove_t iter_best;            /* best root move of the running iteration */
    int iter_score;
    bool iter_have;
    cmove_t killers[MAXPLY][2];
    uint64_t rep[1200];           /* position hashes: game since the last capture/pawn move + search path */
    int nrep;
    uint32_t t_start, t_limit;
    int noise;
    fn_rng_t rng;
    uint32_t nodes;
    bool done;
} search_t;

static void s_push(search_t *S, int depth, int alpha, int beta, bool qs) {
    frame_t *f = &S->st[++S->sp];
    f->depth = depth;
    f->alpha = alpha;
    f->beta = beta;
    f->qs = qs;
    f->entered = false;
    f->n = f->i = f->legal = 0;
    f->best = -INF;
}

static bool s_is_rep(search_t *S) {
    uint64_t h = S->pos.hash;
    int lim = S->nrep - 1 - S->pos.halfmove;
    for (int i = S->nrep - 3; i >= 0 && i >= lim; i -= 2)
        if (S->rep[i] == h) return true;
    return false;
}

/* Prepare a node. Returns true if it is resolved immediately (value in *ret). */
static bool s_enter(search_t *S, frame_t *f, int *ret) {
    pos_t *p = &S->pos;
    int ply = S->sp;
    f->entered = true;
    if (ply > 0 && (p->halfmove >= 100 || s_is_rep(S))) { *ret = 0; return true; }
    if (ply >= MAXPLY - 1) { *ret = evaluate(p); return true; }
    f->check = in_check(p);
    if (f->qs && !f->check) {
        int stand = evaluate(p);
        if (stand >= f->beta) { *ret = stand; return true; }
        if (stand > f->alpha) f->alpha = stand;
        f->best = stand;
        f->n = gen_moves(p, f->mv, true);
    } else {
        f->n = gen_moves(p, f->mv, false);   /* full width (also quiescence evasions in check) */
    }
    for (int k = 0; k < f->n; k++) {
        cmove_t m = f->mv[k];
        int s = 0;
        if (m.flags & MF_CAP) {
            int victim = (m.flags & MF_EP) ? PAWN : PTYPE(p->b[m.to]);
            s = 2000 + victim * 100 - PTYPE(p->b[m.from]);
        } else if (same_move(m, S->killers[ply][0])) {
            s = 1500;
        } else if (same_move(m, S->killers[ply][1])) {
            s = 1400;
        }
        if (m.flags & MF_PROMO) s += 1000 + m.promo * 10;
        if (ply == 0 && S->have_best && same_move(m, S->best)) s = 30000;
        f->sc[k] = (int16_t)s;
    }
    return false;
}

/* A child returned value v (from its own point of view): back it up into the parent. */
static void s_return(search_t *S, int v) {
    S->sp--;
    if (S->sp < 0) return;   /* the root finished: the iteration is complete */
    frame_t *f = &S->st[S->sp];
    unmake_move(&S->pos, f->cur, &f->undo);
    S->nrep--;
    v = -v;
    if (S->sp == 0 && S->noise && v > -MATE + 100 && v < MATE - 100)
        v += fn_rand_n(&S->rng, 2 * S->noise + 1) - S->noise;
    if (v > f->best) {
        f->best = v;
        if (S->sp == 0) {
            S->iter_best = f->cur;
            S->iter_score = v;
            S->iter_have = true;
        }
    }
    if (v > f->alpha) f->alpha = v;
    if (f->alpha >= f->beta) {
        if (!(f->cur.flags & (MF_CAP | MF_PROMO)) && !same_move(f->cur, S->killers[S->sp][0])) {
            S->killers[S->sp][1] = S->killers[S->sp][0];
            S->killers[S->sp][0] = f->cur;
        }
        f->i = f->n;   /* cut-off */
    }
}

static void search_begin(search_t *S, const pos_t *p, const uint64_t *hashes, int nhashes, int level, fn_rng_t *rng) {
    memset(S->killers, 0, sizeof S->killers);
    S->pos = *p;
    S->sp = -1;
    S->depth = 0;
    S->have_best = S->iter_have = false;
    S->done = false;
    S->nodes = 0;
    S->rng.s = fn_rand(rng) | 1;
    /* Hard deepens until about a third of its time is used (4 plies under emulation,
       more on a fast machine); an unfinished iteration still improves the move */
    static const int depth_for[3] = { 1, 3, 6 };
    static const int noise_for[3] = { 70, 10, 0 };
    static const uint32_t time_for[3] = { 1500, 3000, 5000 };
    S->max_depth = depth_for[CLAMP(level, 0, 2)];
    S->noise = noise_for[CLAMP(level, 0, 2)];
    S->t_limit = time_for[CLAMP(level, 0, 2)];
    S->t_start = sys_uptime_ms();
    int from = MAX(0, nhashes - 1 - p->halfmove);
    S->nrep = 0;
    for (int i = from; i < nhashes && S->nrep < 1000; i++) S->rep[S->nrep++] = hashes[i];
}

/* Run the search until `until` (uptime ms). Returns true when it has finished; the move
   to play is then S->best. */
static bool search_step(search_t *S, uint32_t until) {
    int checks = 0;
    for (;;) {
        if (++checks >= 32) {
            checks = 0;
            uint32_t now = sys_uptime_ms();
            if (S->depth > 1 && now - S->t_start >= S->t_limit) {
                /* out of time: the partial iteration searched the previous best move first,
                   so its best so far is at least as good */
                if (S->iter_have) { S->best = S->iter_best; S->best_score = S->iter_score; }
                S->done = true;
                return true;
            }
            if ((int32_t)(now - until) >= 0) return false;
        }
        if (S->sp < 0) {
            if (S->depth > 0) {
                S->best = S->iter_best;
                S->best_score = S->iter_score;
                S->have_best = S->iter_have;
                uint32_t used = sys_uptime_ms() - S->t_start;
                if (S->depth >= S->max_depth || !S->have_best || S->best_score > MATE - 100 ||
                    S->best_score < -MATE + 100 || used * 3 > S->t_limit) {
                    S->done = true;
                    return true;
                }
            }
            S->depth++;
            S->iter_have = false;
            s_push(S, S->depth, -INF, INF, false);
            continue;
        }
        frame_t *f = &S->st[S->sp];
        if (!f->entered) {
            int v;
            if (s_enter(S, f, &v)) s_return(S, v);
            continue;
        }
        if (f->i >= f->n) {
            int v = f->best;
            if (!f->legal) v = f->check ? -MATE + S->sp : (f->qs ? f->best : 0);
            s_return(S, v);
            continue;
        }
        /* next move in order (selection sort step) */
        int bi = f->i;
        for (int k = f->i + 1; k < f->n; k++)
            if (f->sc[k] > f->sc[bi]) bi = k;
        if (bi != f->i) {
            cmove_t tm = f->mv[bi]; f->mv[bi] = f->mv[f->i]; f->mv[f->i] = tm;
            int16_t ts = f->sc[bi]; f->sc[bi] = f->sc[f->i]; f->sc[f->i] = ts;
        }
        cmove_t m = f->mv[f->i++];
        make_move(&S->pos, m, &f->undo);
        if (attacked(&S->pos, S->pos.ksq[S->pos.side ^ 1], S->pos.side)) {
            unmake_move(&S->pos, m, &f->undo);
            continue;
        }
        f->legal++;
        f->cur = m;
        S->nodes++;
        S->rep[S->nrep++] = S->pos.hash;
        int cd = f->depth - 1;
        s_push(S, cd, -f->beta, -f->alpha, f->qs || cd <= 0);
    }
}

/* ======================================================================================
 * Piece images: vector shapes on a 100x100 grid rasterised at 4x into coverage masks,
 * outlined with a chamfer distance transform, shaded and downsampled.
 * ==================================================================================== */
typedef struct { int n; uint8_t *m; double sc; } mask_t;

static void mk_span(mask_t *k, int y, double xa, double xb, uint8_t val) {
    if (y < 0 || y >= k->n) return;
    int x0 = (int)ceil(xa - 0.5), x1 = (int)ceil(xb - 0.5) - 1;
    x0 = MAX(x0, 0);
    x1 = MIN(x1, k->n - 1);
    if (x1 >= x0) memset(k->m + (size_t)y * k->n + x0, val, (size_t)(x1 - x0 + 1));
}

static void mk_poly(mask_t *k, const double *pts, int np, uint8_t val) {
    double miny = 1e9, maxy = -1e9;
    for (int i = 0; i < np; i++) { miny = fmin(miny, pts[2 * i + 1]); maxy = fmax(maxy, pts[2 * i + 1]); }
    int y0 = MAX(0, (int)floor(miny * k->sc)), y1 = MIN(k->n - 1, (int)ceil(maxy * k->sc));
    double xs[32];
    for (int y = y0; y <= y1; y++) {
        double yc = (y + 0.5) / k->sc;
        int cnt = 0;
        for (int i = 0, j = np - 1; i < np && cnt < 32; j = i++) {
            double yi = pts[2 * i + 1], yj = pts[2 * j + 1];
            if ((yi <= yc && yj > yc) || (yj <= yc && yi > yc)) {
                double xi = pts[2 * i], xj = pts[2 * j];
                xs[cnt++] = (xi + (yc - yi) / (yj - yi) * (xj - xi)) * k->sc;
            }
        }
        for (int a = 1; a < cnt; a++)
            for (int b = a; b > 0 && xs[b - 1] > xs[b]; b--) { double t = xs[b]; xs[b] = xs[b - 1]; xs[b - 1] = t; }
        for (int a = 0; a + 1 < cnt; a += 2) mk_span(k, y, xs[a], xs[a + 1], val);
    }
}

static void mk_ellipse(mask_t *k, double cx, double cy, double rx, double ry, uint8_t val) {
    cx *= k->sc; cy *= k->sc; rx *= k->sc; ry *= k->sc;
    int y0 = MAX(0, (int)floor(cy - ry)), y1 = MIN(k->n - 1, (int)ceil(cy + ry));
    for (int y = y0; y <= y1; y++) {
        double dy = (y + 0.5 - cy) / ry;
        if (dy <= -1 || dy >= 1) continue;
        double hw = rx * sqrt(1 - dy * dy);
        mk_span(k, y, cx - hw, cx + hw, val);
    }
}

static void mk_circle(mask_t *k, double cx, double cy, double r, uint8_t val) { mk_ellipse(k, cx, cy, r, r, val); }

static void mk_rrect(mask_t *k, double x0, double y0, double x1, double y1, double r, uint8_t val) {
    double s = k->sc;
    x0 *= s; y0 *= s; x1 *= s; y1 *= s; r *= s;
    int ya = MAX(0, (int)floor(y0)), yb = MIN(k->n - 1, (int)ceil(y1));
    for (int y = ya; y <= yb; y++) {
        double yc = y + 0.5;
        if (yc < y0 || yc > y1) continue;
        double in = 0;
        if (yc < y0 + r) { double d = y0 + r - yc; in = r - sqrt(fmax(0, r * r - d * d)); }
        else if (yc > y1 - r) { double d = yc - (y1 - r); in = r - sqrt(fmax(0, r * r - d * d)); }
        mk_span(k, y, x0 + in, x1 - in, val);
    }
}

static void mk_line(mask_t *k, double x0, double y0, double x1, double y1, double w, uint8_t val) {
    double dx = x1 - x0, dy = y1 - y0, len = sqrt(dx * dx + dy * dy);
    if (len > 0) {
        double nx = -dy / len * w / 2, ny = dx / len * w / 2;
        double q[8] = { x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny };
        mk_poly(k, q, 4, val);
    }
    mk_circle(k, x0, y0, w / 2, val);
    mk_circle(k, x1, y1, w / 2, val);
}

#define P(...) (const double[]){ __VA_ARGS__ }
#define NP(a) ((int)(sizeof(a) / sizeof(double) / 2))

/* Silhouette (fill) and inner detail lines (det) of one piece type. */
static void piece_shape(int t, mask_t *fill, mask_t *det) {
    switch (t) {
    case PAWN: {
        mk_circle(fill, 50, 25, 12.5, 1);
        mk_ellipse(fill, 50, 41.5, 14.5, 4.6, 1);
        static const double body[] = { 42, 43, 58, 43, 61, 54, 66, 66, 71, 76, 29, 76, 34, 66, 39, 54 };
        mk_poly(fill, body, NP(body), 1);
        mk_rrect(fill, 24, 74, 76, 87, 4, 1);
        mk_line(det, 29, 75, 71, 75, 1.8, 1);
        mk_line(det, 38, 45.6, 62, 45.6, 1.6, 1);
        break;
    }
    case ROOK: {
        mk_rrect(fill, 22, 76, 78, 88, 3, 1);
        mk_rrect(fill, 27, 67, 73, 77, 1, 1);
        static const double body[] = { 33, 40, 67, 40, 65, 68, 35, 68 };
        mk_poly(fill, body, NP(body), 1);
        mk_rrect(fill, 29, 31, 71, 41, 1, 1);
        mk_rrect(fill, 27, 13, 73, 32, 1.5, 1);
        mk_rrect(fill, 37.5, 10, 44, 22, 0, 0);
        mk_rrect(fill, 56, 10, 62.5, 22, 0, 0);
        mk_line(det, 31, 40.5, 69, 40.5, 1.8, 1);
        mk_line(det, 33, 67.5, 67, 67.5, 1.8, 1);
        mk_line(det, 27, 76.5, 73, 76.5, 1.8, 1);
        mk_line(det, 29, 31.5, 71, 31.5, 1.8, 1);
        break;
    }
    case KNIGHT: {
        mk_rrect(fill, 22, 77, 78, 88, 3, 1);
        static const double body[] = {
            72, 78, 75, 64, 74, 49, 69, 35, 61, 24, 55, 20, 53, 10, 47, 17.5, 40, 19, 31, 25,
            24, 35, 18, 47, 16.5, 55, 20, 60.5, 28, 61.5, 35, 57.5, 42, 54, 45, 58.5, 39, 66,
            33, 72, 30, 78 };
        mk_poly(fill, body, NP(body), 1);
        mk_circle(det, 36.5, 32, 3.2, 1);
        mk_circle(det, 22, 52, 1.7, 1);
        mk_line(det, 19.5, 58, 26, 58.5, 1.5, 1);
        mk_line(det, 62, 29, 67.5, 40, 1.8, 1);
        mk_line(det, 67.5, 40, 70, 54, 1.8, 1);
        mk_line(det, 70, 54, 70, 68, 1.8, 1);
        mk_line(det, 27, 77.5, 73, 77.5, 1.8, 1);
        break;
    }
    case BISHOP: {
        mk_rrect(fill, 24, 78, 76, 88, 3, 1);
        static const double foot[] = { 36, 66, 64, 66, 70, 79, 30, 79 };
        mk_poly(fill, foot, NP(foot), 1);
        mk_ellipse(fill, 50, 65, 17, 5, 1);
        mk_ellipse(fill, 50, 46, 16.5, 17.5, 1);
        static const double tip[] = { 35.5, 41, 64.5, 41, 50, 18 };
        mk_poly(fill, tip, NP(tip), 1);
        mk_circle(fill, 50, 13.5, 5.5, 1);
        mk_line(fill, 58, 28.5, 47, 41.5, 4.2, 0);
        mk_line(det, 35, 68.5, 65, 68.5, 1.8, 1);
        mk_line(det, 28, 78.5, 72, 78.5, 1.8, 1);
        mk_line(det, 36, 61.5, 64, 61.5, 1.6, 1);
        break;
    }
    case QUEEN: {
        mk_rrect(fill, 22, 78, 78, 88, 3, 1);
        static const double foot[] = { 30, 69, 70, 69, 75, 79, 25, 79 };
        mk_poly(fill, foot, NP(foot), 1);
        static const double body[] = { 31, 70, 18.5, 32, 35, 52, 35, 23, 45.5, 49, 50, 19, 54.5, 49,
                                       65, 23, 65, 52, 81.5, 32, 69, 70 };
        mk_poly(fill, body, NP(body), 1);
        mk_circle(fill, 18.5, 29, 5, 1);
        mk_circle(fill, 35, 19.5, 5, 1);
        mk_circle(fill, 50, 15.5, 5, 1);
        mk_circle(fill, 65, 19.5, 5, 1);
        mk_circle(fill, 81.5, 29, 5, 1);
        mk_ellipse(fill, 50, 69, 20.5, 5, 1);
        mk_line(det, 31, 63.5, 69, 63.5, 1.8, 1);
        mk_line(det, 27, 78.5, 73, 78.5, 1.8, 1);
        break;
    }
    case KING: {
        mk_rrect(fill, 22, 78, 78, 88, 3, 1);
        static const double foot[] = { 30, 69, 70, 69, 75, 79, 25, 79 };
        mk_poly(fill, foot, NP(foot), 1);
        static const double body[] = { 31, 70, 22, 46, 78, 46, 69, 70 };
        mk_poly(fill, body, NP(body), 1);
        mk_ellipse(fill, 36, 44, 15, 10.5, 1);
        mk_ellipse(fill, 64, 44, 15, 10.5, 1);
        mk_ellipse(fill, 50, 40, 8, 10, 1);
        mk_rrect(fill, 46.5, 5, 53.5, 32, 1, 1);
        mk_rrect(fill, 40, 11, 60, 18, 1, 1);
        mk_ellipse(fill, 50, 69, 20.5, 5, 1);
        mk_line(det, 31, 63.5, 69, 63.5, 1.8, 1);
        mk_line(det, 27, 78.5, 73, 78.5, 1.8, 1);
        mk_line(det, 50, 32, 50, 58, 1.6, 1);
        break;
    }
    }
}

/* Render a piece of colour c into a new size x size SURF_ALPHA surface. */
static surface_t *render_piece(int t, int c, int size) {
    int n = size * 4;
    surface_t *out = surface_create_alpha(size, size);
    uint8_t *fm = calloc((size_t)n * n, 1), *dm = calloc((size_t)n * n, 1);
    uint16_t *dist = malloc((size_t)n * n * sizeof(uint16_t));
    if (!out || !fm || !dm || !dist) {
        free(fm); free(dm); free(dist);
        if (out) surface_destroy(out);
        return NULL;
    }
    mask_t fill = { n, fm, n / 100.0 }, det = { n, dm, n / 100.0 };
    piece_shape(t, &fill, &det);
    /* chamfer (3-4) distance to the silhouette */
    for (int i = 0; i < n * n; i++) dist[i] = fm[i] ? 0 : 60000;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int i = y * n + x;
            unsigned d = dist[i];
            if (x > 0 && dist[i - 1] + 3u < d) d = dist[i - 1] + 3u;
            if (y > 0) {
                if (dist[i - n] + 3u < d) d = dist[i - n] + 3u;
                if (x > 0 && dist[i - n - 1] + 4u < d) d = dist[i - n - 1] + 4u;
                if (x < n - 1 && dist[i - n + 1] + 4u < d) d = dist[i - n + 1] + 4u;
            }
            dist[i] = (uint16_t)d;
        }
    for (int y = n - 1; y >= 0; y--)
        for (int x = n - 1; x >= 0; x--) {
            int i = y * n + x;
            unsigned d = dist[i];
            if (x < n - 1 && dist[i + 1] + 3u < d) d = dist[i + 1] + 3u;
            if (y < n - 1) {
                if (dist[i + n] + 3u < d) d = dist[i + n] + 3u;
                if (x < n - 1 && dist[i + n + 1] + 4u < d) d = dist[i + n + 1] + 4u;
                if (x > 0 && dist[i + n - 1] + 4u < d) d = dist[i + n - 1] + 4u;
            }
            dist[i] = (uint16_t)d;
        }
    double ow = MAX(1.0, size / 40.0);          /* outline width in output pixels */
    unsigned olim = (unsigned)(ow * 4 * 3);     /* in chamfer units at 4x          */
    int so = MAX(2, (int)lround(size / 30.0 * 4));   /* shadow offset (4x pixels) */
    uint32_t top = c == WHITE ? RGB(255, 253, 246) : RGB(96, 96, 106);
    uint32_t bot = c == WHITE ? RGB(222, 212, 194) : RGB(36, 36, 42);
    uint32_t outline = c == WHITE ? RGB(46, 42, 38) : RGB(12, 12, 14);
    uint32_t detail = c == WHITE ? RGB(84, 76, 68) : RGB(168, 168, 178);
    for (int oy = 0; oy < size; oy++) {
        for (int ox = 0; ox < size; ox++) {
            unsigned rs = 0, gs = 0, bs = 0, a = 0;
            for (int sy = 0; sy < 4; sy++) {
                int y = oy * 4 + sy;
                uint32_t rowc = color_mix(top, bot, y * 255 / n);
                for (int sx = 0; sx < 4; sx++) {
                    int x = ox * 4 + sx, i = y * n + x;
                    uint32_t col;
                    if (fm[i]) {
                        if (dm[i]) col = detail;
                        else col = color_shade(rowc, (n / 2 - x) * 26 / n);   /* lit from the left */
                    } else if (dist[i] <= olim) {
                        col = outline;
                    } else {
                        int xs = x - so, ys = y - so;
                        if (xs >= 0 && ys >= 0 && dist[ys * n + xs] <= olim) a += 60;   /* soft shadow */
                        continue;
                    }
                    rs += COLOR_R(col) * 255;
                    gs += COLOR_G(col) * 255;
                    bs += COLOR_B(col) * 255;
                    a += 255;
                }
            }
            if (!a) continue;
            surface_row(out, oy)[ox] = RGBA(rs / a, gs / a, bs / a, a / 16);
        }
    }
    free(fm);
    free(dm);
    free(dist);
    return out;
}

/* Shared, immutable sprite caches (like the system icon cache, never freed). */
#define CH_SQ   60
#define MINI_SZ 22
static surface_t *spr[2][7], *mini[2][7];

static void ch_sprites_init(void) {
    for (int c = 0; c < 2; c++)
        for (int t = PAWN; t <= KING; t++) {
            if (!spr[c][t]) spr[c][t] = render_piece(t, c, CH_SQ);
            if (!mini[c][t]) mini[c][t] = render_piece(t, c, MINI_SZ);
        }
}

/* ======================================================================================
 * The app
 * ==================================================================================== */
#define CH_W      780
#define CH_H      520
#define BX        20
#define BY        20
#define BOARD     (8 * CH_SQ)
#define PX        (BX + BOARD + 22)
#define PW        (CH_W - PX - 18)
#define MAXHIST   1024
#define MOVE_MS   170
#define MIN_THINK 350

enum { MODE_AI, MODE_2P };
enum { ST_PLAY, ST_MATE, ST_STALE, ST_50, ST_REP, ST_MATERIAL };

static const char *const mode_names[2] = { "vs Computer", "2 Players" };
static const char *const level_names[3] = { "Easy", "Medium", "Hard" };
static const char *const side_names[2] = { "Play White", "Play Black" };

typedef struct {
    cmove_t m;
    undo_t u;
    char san[12];
    int8_t piece, captured;
} hist_t;

typedef struct {
    window_t *win;
    fn_rng_t rng;
    pos_t pos;
    hist_t *hist;
    uint64_t *hashes;           /* hashes[i] = position before move i; hashes[nhist] = now */
    int nhist;
    int start_side, start_move;  /* side to move / move number of the starting position */
    int mode, level, human;
    bool flip;                  /* user flipped the board */
    int status;
    cmove_t legal[256];
    int nlegal;
    int sel, hover;
    bool press, dragging;
    int press_x, press_y, drag_x, drag_y;
    bool promo_open;
    int promo_from, promo_to, promo_hover;
    bool anim;
    uint32_t anim_t0;
    int anim_from, anim_to, anim_rf, anim_rt;
    search_t *search;
    bool thinking;
    uint32_t think_t0;
    bool over_shown, recorded;
    uint32_t over_t0;
    int timer;
    int kb;
    bool kb_on;
    int list_scroll;
    fn_btn_t btn[5];            /* New Game, Undo, Flip, overlay New Game, overlay View Board */
    int seg_hover[3];
    rect_t seg_r[3], status_r, cap_r, list_r;
} chess_t;

/* ---- helpers --------------------------------------------------------------------------- */
static bool ch_white_bottom(chess_t *g) {
    bool wb = g->mode == MODE_2P ? true : g->human == WHITE;
    return g->flip ? !wb : wb;
}

static rect_t sq_rect(chess_t *g, int sq) {
    int f = FILEOF(sq), r = RANK(sq);
    if (!ch_white_bottom(g)) { f = 7 - f; r = 7 - r; }
    return rect_make(BX + f * CH_SQ, BY + (7 - r) * CH_SQ, CH_SQ, CH_SQ);
}

static int sq_at(chess_t *g, int x, int y) {
    if (x < BX || y < BY || x >= BX + BOARD || y >= BY + BOARD) return -1;
    int f = (x - BX) / CH_SQ, r = 7 - (y - BY) / CH_SQ;
    if (!ch_white_bottom(g)) { f = 7 - f; r = 7 - r; }
    return SQ(r, f);
}

static bool ch_ai_turn(chess_t *g) {
    return g->mode == MODE_AI && g->status == ST_PLAY && g->pos.side != g->human;
}

static bool ch_human_can_move(chess_t *g) {
    return g->status == ST_PLAY && !g->thinking && !ch_ai_turn(g) && !g->promo_open;
}

static bool ch_is_target(chess_t *g, int from, int to) {
    for (int i = 0; i < g->nlegal; i++)
        if (g->legal[i].from == from && g->legal[i].to == to) return true;
    return false;
}

static bool ch_has_moves(chess_t *g, int from) {
    for (int i = 0; i < g->nlegal; i++)
        if (g->legal[i].from == from) return true;
    return false;
}

static void ch_inval_board(chess_t *g) { win_invalidate_rect(g->win, BX - 8, BY - 8, BOARD + 16, BOARD + 16); }
static void ch_inval_panel(chess_t *g) { win_invalidate_rect(g->win, PX - 4, 0, CH_W - PX + 4, CH_H); }

static void ch_inval_sq(chess_t *g, int sq) {
    if (sq < 0) return;
    rect_t r = sq_rect(g, sq);
    win_invalidate_rect(g->win, r.x, r.y, r.w, r.h);
}

static void ch_update_timer(chess_t *g) {
    bool need = g->thinking || g->anim || (g->over_shown && sys_uptime_ms() - g->over_t0 < 400);
    if (need && g->timer <= 0) g->timer = win_set_timer(g->win, 16);
    else if (!need && g->timer > 0) { win_kill_timer(g->win, g->timer); g->timer = 0; }
}

static int ch_count_rep(chess_t *g) {
    int cnt = 0;
    uint64_t h = g->pos.hash;
    for (int i = g->nhist; i >= 0 && i >= g->nhist - g->pos.halfmove; i -= 2)
        if (g->hashes[i] == h) cnt++;
    return cnt;
}

static void ch_refresh(chess_t *g) {
    g->nlegal = gen_legal(&g->pos, g->legal);
    if (!g->nlegal) g->status = in_check(&g->pos) ? ST_MATE : ST_STALE;
    else if (g->pos.halfmove >= 100) g->status = ST_50;
    else if (insufficient_material(&g->pos)) g->status = ST_MATERIAL;
    else if (ch_count_rep(g) >= 3) g->status = ST_REP;
    else g->status = ST_PLAY;
}

/* Rows of the move list: a game that starts with Black to move begins with "1. ..". */
static int ch_list_rows(chess_t *g) {
    return (g->nhist + (g->start_side == BLACK) + 1) / 2;
}

static void ch_scroll_to_end(chess_t *g) {
    int rows = ch_list_rows(g), vis = (g->list_r.h - 34) / 20;
    g->list_scroll = MAX(0, rows - vis);
}

static void ch_save_settings(chess_t *g) {
    fn_score_set("chess_mode", g->mode);
    fn_score_set("chess_level", g->level);
    fn_score_set("chess_side", g->human);
}

/* ---- thinking ------------------------------------------------------------------------- */
static void ch_stop_thinking(chess_t *g) {
    g->thinking = false;
}

static void ch_maybe_think(chess_t *g) {
    if (!ch_ai_turn(g) || g->thinking) return;
    if (!g->search) g->search = malloc(sizeof(search_t));
    if (!g->search) return;
    search_begin(g->search, &g->pos, g->hashes, g->nhist + 1, g->level, &g->rng);
    g->thinking = true;
    g->think_t0 = sys_uptime_ms();
    ch_update_timer(g);
    ch_inval_panel(g);
}

/* ---- playing moves ---------------------------------------------------------------------- */
static void ch_game_over(chess_t *g) {
    g->over_shown = true;
    g->over_t0 = sys_uptime_ms() + (g->anim ? MOVE_MS : 0);
    int res = 0;   /* +1 human win, -1 loss, 0 draw (vs computer) */
    bool human_won = false;
    if (g->status == ST_MATE) {
        int winner = g->pos.side ^ 1;
        human_won = g->mode == MODE_2P || winner == g->human;
        res = winner == g->human ? 1 : -1;
    }
    if (g->mode == MODE_AI && !g->recorded && g->nhist > 0) {
        char key[32];
        snprintf(key, sizeof key, "chess_%s_%d", res > 0 ? "won" : (res < 0 ? "lost" : "drawn"), g->level);
        fn_score_set(key, fn_score_get(key, 0) + 1);
        g->recorded = true;
    }
    if (g->status == ST_MATE && human_won) {
        static const note_t tune[] = { { 523, 100 }, { 659, 100 }, { 784, 100 }, { 1047, 260 } };
        sys_play_notes(tune, ARRAY_SIZE(tune));
    } else if (g->status == ST_MATE) {
        static const note_t tune[] = { { 392, 140 }, { 330, 140 }, { 262, 300 } };
        sys_play_notes(tune, ARRAY_SIZE(tune));
    } else {
        static const note_t tune[] = { { 440, 120 }, { 0, 40 }, { 440, 160 } };
        sys_play_notes(tune, ARRAY_SIZE(tune));
    }
}

static void ch_apply(chess_t *g, cmove_t m, bool animate) {
    if (g->nhist >= MAXHIST - 1) return;
    hist_t *h = &g->hist[g->nhist];
    move_san(&g->pos, m, h->san);
    h->m = m;
    h->piece = g->pos.b[m.from];
    make_move(&g->pos, m, &h->u);
    h->captured = h->u.captured;
    g->nhist++;
    g->hashes[g->nhist] = g->pos.hash;
    g->sel = -1;
    g->dragging = g->press = false;
    g->promo_open = false;
    g->anim = animate;
    g->anim_t0 = sys_uptime_ms();
    g->anim_from = m.from;
    g->anim_to = m.to;
    g->anim_rf = g->anim_rt = -1;
    if (m.flags & MF_CASTLE) {
        g->anim_rf = m.to > m.from ? m.from + 3 : m.from - 4;
        g->anim_rt = m.to > m.from ? m.from + 1 : m.from - 1;
    }
    ch_refresh(g);
    bool check = in_check(&g->pos);
    if (g->status == ST_PLAY) {
        if (check) sys_beep(988, 70);
        else if (h->captured) sys_beep(330, 45);
        else sys_beep(560, 22);
    }
    ch_scroll_to_end(g);
    if (g->status != ST_PLAY) ch_game_over(g);
    else ch_maybe_think(g);
    ch_update_timer(g);
    win_invalidate(g->win);
}

/* Human move from -> to: opens the promotion picker when needed. */
static void ch_try_move(chess_t *g, int from, int to, bool animate) {
    cmove_t found[4];
    int nf = 0;
    for (int i = 0; i < g->nlegal && nf < 4; i++)
        if (g->legal[i].from == from && g->legal[i].to == to) found[nf++] = g->legal[i];
    if (!nf) return;
    if (found[0].flags & MF_PROMO) {
        g->promo_open = true;
        g->promo_from = from;
        g->promo_to = to;
        g->dragging = g->press = false;
        ch_inval_board(g);
        return;
    }
    ch_apply(g, found[0], animate);
}

/* Start a game from the standard position (from == NULL) or a set-up position. */
static void ch_start(chess_t *g, const pos_t *from) {
    ch_stop_thinking(g);
    if (from) g->pos = *from;
    else pos_start(&g->pos);
    g->start_side = g->pos.side;
    g->start_move = g->pos.fullmove;
    g->nhist = 0;
    g->hashes[0] = g->pos.hash;
    g->sel = g->hover = -1;
    g->press = g->dragging = g->promo_open = g->anim = false;
    g->over_shown = g->recorded = false;
    g->list_scroll = 0;
    g->kb = g->human == WHITE ? SQ(1, 4) : SQ(6, 4);
    ch_refresh(g);
    if (g->status != ST_PLAY) ch_game_over(g);
    else ch_maybe_think(g);
    ch_update_timer(g);
    win_invalidate(g->win);
}

static void ch_new_game(chess_t *g) { ch_start(g, NULL); }

/* Set up a position from FEN text; false if it is not a legal position. */
static bool ch_load_fen(chess_t *g, const char *fen) {
    pos_t p;
    if (!fen || !pos_from_fen(&p, fen)) return false;
    ch_start(g, &p);
    return true;
}

static void ch_undo_one(chess_t *g) {
    if (!g->nhist) return;
    hist_t *h = &g->hist[--g->nhist];
    unmake_move(&g->pos, h->m, &h->u);
}

static void ch_undo(chess_t *g) {
    if (!g->nhist) return;
    g->promo_open = false;
    if (g->thinking) {
        ch_stop_thinking(g);
        ch_undo_one(g);
    } else if (g->mode == MODE_AI) {
        ch_undo_one(g);
        while (g->nhist > 0 && g->pos.side != g->human) ch_undo_one(g);
    } else {
        ch_undo_one(g);
    }
    g->sel = -1;
    g->anim = g->press = g->dragging = false;
    g->over_shown = false;
    ch_refresh(g);
    ch_scroll_to_end(g);
    ch_maybe_think(g);
    ch_update_timer(g);
    sys_beep(440, 20);
    win_invalidate(g->win);
}

static void ch_set_mode(chess_t *g, int mode) {
    if (mode == g->mode) return;
    g->mode = mode;
    ch_stop_thinking(g);
    g->sel = -1;
    g->flip = false;
    ch_save_settings(g);
    ch_maybe_think(g);
    ch_update_timer(g);
    win_invalidate(g->win);
}

static void ch_set_level(chess_t *g, int level) {
    if (level == g->level) return;
    g->level = level;
    ch_save_settings(g);
    if (g->thinking) { ch_stop_thinking(g); ch_maybe_think(g); }
    ch_inval_panel(g);
}

static void ch_set_side(chess_t *g, int side) {
    if (side == g->human) return;
    g->human = side;
    g->flip = false;
    ch_save_settings(g);
    if (g->status != ST_PLAY || g->nhist == 0) {
        ch_new_game(g);
        return;
    }
    /* swap sides mid-game: the computer takes over the other colour */
    ch_stop_thinking(g);
    g->sel = -1;
    ch_maybe_think(g);
    ch_update_timer(g);
    win_invalidate(g->win);
}

/* ======================================================================================
 * Painting
 * ==================================================================================== */
static const uint32_t SQ_LIGHT = RGB(240, 217, 181), SQ_DARK = RGB(181, 136, 99);

static void ch_draw_piece(surface_t *s, int pc, int x, int y, int opacity) {
    if (!pc) return;
    surface_t *img = spr[PCOL(pc)][PTYPE(pc)];
    if (img) gfx_blit_alpha(s, x, y, img, opacity);
}

static void ch_anim_pos(chess_t *g, int from, int to, double t, int *x, int *y) {
    rect_t a = sq_rect(g, from), b = sq_rect(g, to);
    double e = fn_ease_out(t);
    *x = (int)lround(a.x + (b.x - a.x) * e);
    *y = (int)lround(a.y + (b.y - a.y) * e);
}

static void ch_paint_board(chess_t *g, surface_t *s, uint32_t now) {
    const theme_t *t = theme_get();
    gfx_shadow(s, BX - 6, BY - 6, BOARD + 12, BOARD + 12, 8, 10, t->dark ? 120 : 60);
    gfx_fill_rounded_rect(s, BX - 6, BY - 6, BOARD + 12, BOARD + 12, 8, RGB(112, 80, 58));
    gfx_rect(s, BX - 1, BY - 1, BOARD + 2, BOARD + 2, RGB(92, 64, 46));
    hist_t *last = g->nhist ? &g->hist[g->nhist - 1] : NULL;
    bool check = g->status != ST_STALE && in_check(&g->pos);
    for (int r = 0; r < 8; r++)
        for (int f = 0; f < 8; f++) {
            int sq = SQ(r, f);
            rect_t q = sq_rect(g, sq);
            bool light = (r + f) & 1;
            uint32_t base = light ? SQ_LIGHT : SQ_DARK;
            gfx_fill_rect(s, q.x, q.y, q.w, q.h, base);
            if (last && (sq == last->m.from || sq == last->m.to))
                gfx_fill_rect(s, q.x, q.y, q.w, q.h, RGBA(232, 220, 40, light ? 92 : 120));
            if (sq == g->sel) gfx_fill_rect(s, q.x, q.y, q.w, q.h, RGBA(20, 110, 60, 120));
            else if (g->sel >= 0 && sq == g->hover && ch_is_target(g, g->sel, sq))
                gfx_fill_rect(s, q.x, q.y, q.w, q.h, RGBA(20, 110, 60, 70));
            /* coordinates along the left and bottom edges */
            bool wb = ch_white_bottom(g);
            uint32_t lc = light ? SQ_DARK : SQ_LIGHT;
            if (f == (wb ? 0 : 7)) {
                char lab[2] = { (char)('1' + r), 0 };
                gfx_text(s, q.x + 3, q.y + 1, lab, lc, FONT_UI_BOLD);
            }
            if (r == (wb ? 0 : 7)) {
                char lab[2] = { (char)('a' + f), 0 };
                gfx_text(s, q.x + q.w - 4 - font_text_width(FONT_UI_BOLD, lab), q.y + q.h - 17, lab, lc, FONT_UI_BOLD);
            }
        }
    /* king in check: red glow */
    if (check) {
        rect_t q = sq_rect(g, g->pos.ksq[g->pos.side]);
        double cx = q.x + q.w / 2.0, cy = q.y + q.h / 2.0;
        surface_t sub = surface_sub(s, q);
        for (int i = 10; i >= 1; i--)
            gfx_fill_circle_aa(&sub, cx - q.x, cy - q.y, q.w * 0.07 * i, RGBA(230, 30, 20, 22));
    }
    /* pieces */
    double at = g->anim ? (double)(now - g->anim_t0) / MOVE_MS : 1.0;
    for (int r = 0; r < 8; r++)
        for (int f = 0; f < 8; f++) {
            int sq = SQ(r, f);
            int pc = g->pos.b[sq];
            if (!pc) continue;
            if (g->anim && at < 1 && (sq == g->anim_to || sq == g->anim_rt)) continue;
            rect_t q = sq_rect(g, sq);
            if (g->dragging && sq == g->sel) { ch_draw_piece(s, pc, q.x, q.y, 90); continue; }
            ch_draw_piece(s, pc, q.x, q.y, 255);
        }
    /* legal targets of the selected piece */
    if (g->sel >= 0) {
        for (int i = 0; i < g->nlegal; i++) {
            if (g->legal[i].from != g->sel) continue;
            if (g->legal[i].flags & MF_PROMO && g->legal[i].promo != QUEEN) continue;
            rect_t q = sq_rect(g, g->legal[i].to);
            double cx = q.x + q.w / 2.0, cy = q.y + q.h / 2.0;
            if (g->pos.b[g->legal[i].to] || (g->legal[i].flags & MF_EP)) {
                surface_t sub = surface_sub(s, q);
                gfx_circle_aa(&sub, cx - q.x, cy - q.y, q.w * 0.44, q.w * 0.09, RGBA(20, 90, 45, 110));
            } else {
                gfx_fill_circle_aa(s, cx, cy, q.w * 0.15, RGBA(20, 90, 45, g->legal[i].to == g->hover ? 150 : 100));
            }
        }
    }
    /* moving piece(s) */
    if (g->anim && at < 1) {
        int x, y;
        if (g->anim_rt >= 0) {
            ch_anim_pos(g, g->anim_rf, g->anim_rt, at, &x, &y);
            ch_draw_piece(s, g->pos.b[g->anim_rt], x, y, 255);
        }
        ch_anim_pos(g, g->anim_from, g->anim_to, at, &x, &y);
        ch_draw_piece(s, g->pos.b[g->anim_to], x, y, 255);
    }
    /* keyboard cursor */
    if (g->kb_on && g->status == ST_PLAY) {
        rect_t q = sq_rect(g, g->kb);
        gfx_rect(s, q.x, q.y, q.w, q.h, t->accent);
        gfx_rect(s, q.x + 1, q.y + 1, q.w - 2, q.h - 2, t->accent);
        gfx_rect(s, q.x + 2, q.y + 2, q.w - 4, q.h - 4, color_alpha(t->accent, 120));
    }
    /* dragged piece follows the pointer */
    if (g->dragging && g->sel >= 0) {
        int pc = g->pos.b[g->sel];
        ch_draw_piece(s, pc, g->drag_x - CH_SQ / 2, g->drag_y - CH_SQ / 2, 255);
    }
    /* promotion picker: a column of four pieces over the target file */
    if (g->promo_open) {
        gfx_fill_rect(s, BX, BY, BOARD, BOARD, RGBA(0, 0, 0, 90));
        static const int opts[4] = { QUEEN, KNIGHT, ROOK, BISHOP };
        rect_t q = sq_rect(g, g->promo_to);
        int dir = q.y == BY ? 1 : -1;
        int us = g->pos.side;
        gfx_shadow(s, q.x, dir > 0 ? q.y : q.y - 3 * CH_SQ, CH_SQ, 4 * CH_SQ, 6, 10, 120);
        for (int i = 0; i < 4; i++) {
            int y = q.y + dir * i * CH_SQ;
            uint32_t bg = i == g->promo_hover ? color_mix(COLOR_WHITE, t->accent, 70) : (i % 2 ? RGB(236, 236, 240) : COLOR_WHITE);
            gfx_fill_rect(s, q.x, y, CH_SQ, CH_SQ, bg);
            ch_draw_piece(s, MKP(us, opts[i]), q.x, y, 255);
        }
    }
}

/* Index (0..3) of the promotion choice under (x, y), or -1. */
static int ch_promo_hit(chess_t *g, int x, int y) {
    rect_t q = sq_rect(g, g->promo_to);
    int dir = q.y == BY ? 1 : -1;
    for (int i = 0; i < 4; i++)
        if (rect_contains(rect_make(q.x, q.y + dir * i * CH_SQ, CH_SQ, CH_SQ), x, y)) return i;
    return -1;
}

static int ch_material(const pos_t *p, int c) {
    static const int v[7] = { 0, 1, 3, 3, 5, 9, 0 };
    int m = 0;
    for (int sq = 0; sq < 128; sq++)
        if (ONBOARD(sq) && p->b[sq] && PCOL(p->b[sq]) == c) m += v[PTYPE(p->b[sq])];
    return m;
}

static void ch_disc(surface_t *s, double cx, double cy, double r, int c) {
    gfx_fill_circle_aa(s, cx, cy, r + 1, c == WHITE ? RGB(120, 120, 128) : RGB(10, 10, 12));
    gfx_fill_circle_aa(s, cx, cy, r, c == WHITE ? RGB(250, 249, 245) : RGB(44, 44, 50));
}

static void ch_card(surface_t *s, rect_t r) {
    const theme_t *t = theme_get();
    gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 10, t->window_border);
    gfx_fill_rounded_rect(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 9, fn_card_bg());
}

static void ch_status_text(chess_t *g, char *title, char *sub, int n, uint32_t *subcol) {
    const theme_t *t = theme_get();
    *subcol = fn_muted();
    int winner = g->pos.side ^ 1;
    switch (g->status) {
    case ST_MATE:
        snprintf(title, (size_t)n, "Checkmate");
        if (g->mode == MODE_AI) snprintf(sub, (size_t)n, winner == g->human ? "You win!" : "The computer wins");
        else snprintf(sub, (size_t)n, "%s wins", winner == WHITE ? "White" : "Black");
        *subcol = t->window_text;
        return;
    case ST_STALE: snprintf(title, (size_t)n, "Stalemate"); snprintf(sub, (size_t)n, "The game is a draw"); return;
    case ST_50: snprintf(title, (size_t)n, "Draw"); snprintf(sub, (size_t)n, "Fifty-move rule"); return;
    case ST_REP: snprintf(title, (size_t)n, "Draw"); snprintf(sub, (size_t)n, "Threefold repetition"); return;
    case ST_MATERIAL: snprintf(title, (size_t)n, "Draw"); snprintf(sub, (size_t)n, "Insufficient material"); return;
    }
    if (g->thinking) {
        snprintf(title, (size_t)n, "Thinking...");
        snprintf(sub, (size_t)n, "Computer (%s) plays %s", level_names[g->level], g->pos.side == WHITE ? "White" : "Black");
        return;
    }
    if (g->mode == MODE_AI) snprintf(title, (size_t)n, "Your move");
    else snprintf(title, (size_t)n, "%s to move", g->pos.side == WHITE ? "White" : "Black");
    if (in_check(&g->pos)) {
        snprintf(sub, (size_t)n, "Check!");
        *subcol = RGB(220, 50, 40);
    } else if (g->promo_open) {
        snprintf(sub, (size_t)n, "Choose a piece for the pawn");
    } else if (g->nhist) {
        snprintf(sub, (size_t)n, "Last move: %s", g->hist[g->nhist - 1].san);
    } else {
        snprintf(sub, (size_t)n, "Click a piece, then a square");
    }
}

static void ch_paint_panel(chess_t *g, surface_t *s, uint32_t now) {
    const theme_t *t = theme_get();
    fn_seg_draw(s, g->seg_r[0], mode_names, 2, g->mode, g->seg_hover[0]);
    fn_seg_draw(s, g->seg_r[1], level_names, 3, g->level, g->seg_hover[1]);
    fn_seg_draw(s, g->seg_r[2], side_names, 2, g->human, g->seg_hover[2]);
    if (g->mode == MODE_2P) {   /* computer settings do not apply */
        for (int i = 1; i <= 2; i++) {
            rect_t r = g->seg_r[i];
            gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 8, color_alpha(t->window_bg, 150));
        }
    }

    /* status */
    rect_t r = g->status_r;
    ch_card(s, r);
    int show_side = g->status == ST_MATE ? (g->pos.side ^ 1) : g->pos.side;
    ch_disc(s, r.x + 26, r.y + r.h / 2.0, 11, show_side);
    char title[64], sub[64];
    uint32_t subcol;
    ch_status_text(g, title, sub, sizeof title, &subcol);
    gfx_text(s, r.x + 48, r.y + 9, title, t->window_text, FONT_UI_LARGE);
    gfx_text_ellipsis(s, r.x + 48, r.y + 35, r.w - 56, sub, subcol, FONT_UI);
    if (g->thinking) {   /* spinner */
        double a = (now % 1000) / 1000.0 * 2 * M_PI;
        double cx = r.x + r.w - 22, cy = r.y + 21;
        gfx_circle_aa(s, cx, cy, 8, 2.4, color_alpha(t->accent, 50));
        fn_arc(s, cx, cy, 8, a, a + 1.9, 2.4, t->accent);
    }

    /* captured pieces, one row per side (top row = side at the top of the board) */
    int top = ch_white_bottom(g) ? BLACK : WHITE;
    int mat[2] = { ch_material(&g->pos, WHITE), ch_material(&g->pos, BLACK) };
    for (int row = 0; row < 2; row++) {
        int c = row == 0 ? top : top ^ 1;
        int y = g->cap_r.y + row * 24;
        ch_disc(s, g->cap_r.x + 8, y + 11, 6, c);
        int x = g->cap_r.x + 22, cnt = 0;
        static const int order[5] = { QUEEN, ROOK, BISHOP, KNIGHT, PAWN };
        for (int k = 0; k < 5; k++)
            for (int i = 0; i < g->nhist; i++) {
                hist_t *h = &g->hist[i];
                if (!h->captured || PCOL(h->piece) != c || PTYPE(h->captured) != order[k]) continue;
                if (x < g->cap_r.x + g->cap_r.w - 40 && mini[c ^ 1][order[k]])
                    gfx_blit_alpha(s, x, y, mini[c ^ 1][order[k]], 255);
                x += 12;
                cnt++;
            }
        if (!cnt) gfx_text(s, x, y + 3, "no captures", fn_muted(), FONT_UI);
        int adv = mat[c] - mat[c ^ 1];
        if (adv > 0) {
            char buf[16];
            snprintf(buf, sizeof buf, "+%d", adv);
            gfx_text(s, (cnt ? x + 14 : g->cap_r.x + g->cap_r.w - 30), y + 3, buf, t->window_text, FONT_UI_BOLD);
        }
    }

    /* move list */
    r = g->list_r;
    ch_card(s, r);
    gfx_text(s, r.x + 12, r.y + 8, "Moves", t->window_text, FONT_UI_BOLD);
    if (g->nhist) {
        char buf[32];
        snprintf(buf, sizeof buf, "%d", ch_list_rows(g));
        gfx_text(s, r.x + r.w - 12 - font_text_width(FONT_UI, buf), r.y + 8, buf, fn_muted(), FONT_UI);
    }
    ui_draw_separator(s, r.x + 8, r.y + 29, r.w - 16);
    rect_t inner = rect_make(r.x + 1, r.y + 31, r.w - 2, r.h - 33);
    rect_t old = s->clip;
    surface_set_clip(s, rect_intersect(old, inner));
    int rows = ch_list_rows(g), vis = (r.h - 34) / 20, off = g->start_side == BLACK;
    if (!rows) fn_text_c(s, r.x, r.y + 50, r.w, "No moves yet", fn_muted(), FONT_UI);
    for (int i = 0; i < vis + 1; i++) {
        int row = g->list_scroll + i;
        if (row >= rows) break;
        int y = r.y + 34 + i * 20;
        if (row & 1) gfx_fill_rect(s, r.x + 4, y - 1, r.w - 8, 20, color_alpha(t->window_text, 8));
        char num[16];
        snprintf(num, sizeof num, "%d.", g->start_move + row);
        gfx_text(s, r.x + 12, y + 1, num, fn_muted(), FONT_UI);
        for (int k = 0; k < 2; k++) {
            int idx = row * 2 + k - off;
            int x = r.x + 50 + k * 90;
            if (idx < 0) { gfx_text(s, x, y + 1, "...", fn_muted(), FONT_UI); continue; }
            if (idx >= g->nhist) break;
            bool last = idx == g->nhist - 1;
            if (last) {
                int w = font_text_width(FONT_UI_BOLD, g->hist[idx].san);
                gfx_fill_rounded_rect(s, x - 5, y - 1, w + 10, 19, 5, color_alpha(t->accent, t->dark ? 90 : 50));
            }
            gfx_text(s, x, y + 1, g->hist[idx].san, t->window_text, last ? FONT_UI_BOLD : FONT_UI);
        }
    }
    if (rows > vis) {   /* scroll position */
        int th = MAX(16, inner.h * vis / rows);
        int ty = inner.y + (inner.h - th) * g->list_scroll / MAX(1, rows - vis);
        gfx_fill_rounded_rect(s, r.x + r.w - 7, ty + 2, 4, th - 4, 2, color_alpha(t->window_text, 70));
    }
    s->clip = old;

    for (int i = 0; i < 3; i++) fn_btn_draw(s, &g->btn[i]);
}

static void ch_paint_over(chess_t *g, surface_t *s, uint32_t now) {
    const theme_t *t = theme_get();
    if ((int32_t)(now - g->over_t0) < 0) { g->btn[3].hidden = g->btn[4].hidden = true; return; }
    double p = fn_ease_out((now - g->over_t0) / 300.0);
    rect_t area = rect_make(BX, BY, BOARD, BOARD);
    rect_t c = fn_overlay_card(s, area, 300, 196, (int)(80 * p));
    char title[64], sub[64];
    uint32_t subcol;
    ch_status_text(g, title, sub, sizeof title, &subcol);
    fn_text_c(s, c.x, c.y + 18, c.w, title, t->window_text, FONT_UI_TITLE);
    fn_text_c(s, c.x, c.y + 60, c.w, sub, g->status == ST_MATE ? t->accent : t->window_text, FONT_UI_LARGE);
    char buf[96];
    if (g->mode == MODE_AI) {
        char k1[32], k2[32], k3[32];
        snprintf(k1, sizeof k1, "chess_won_%d", g->level);
        snprintf(k2, sizeof k2, "chess_lost_%d", g->level);
        snprintf(k3, sizeof k3, "chess_drawn_%d", g->level);
        snprintf(buf, sizeof buf, "Your record vs %s: %d won, %d lost, %d drawn", level_names[g->level],
                 fn_score_get(k1, 0), fn_score_get(k2, 0), fn_score_get(k3, 0));
    } else {
        int mv = ch_list_rows(g);
        snprintf(buf, sizeof buf, "%d move%s played", mv, mv == 1 ? "" : "s");
    }
    fn_text_c(s, c.x, c.y + 92, c.w, buf, fn_muted(), FONT_UI);
    g->btn[3].r = rect_make(c.x + c.w / 2 - 128, c.y + c.h - 56, 122, 34);
    g->btn[4].r = rect_make(c.x + c.w / 2 + 6, c.y + c.h - 56, 122, 34);
    g->btn[3].hidden = g->btn[4].hidden = false;
    fn_btn_draw(s, &g->btn[3]);
    fn_btn_draw(s, &g->btn[4]);
}

static void ch_paint(chess_t *g, surface_t *s) {
    uint32_t now = sys_uptime_ms();
    g->btn[1].disabled = g->nhist == 0;
    ch_paint_board(g, s, now);
    ch_paint_panel(g, s, now);
    if (g->over_shown) ch_paint_over(g, s, now);
    else g->btn[3].hidden = g->btn[4].hidden = true;
}

/* ======================================================================================
 * Input
 * ==================================================================================== */
static void ch_set_hover(chess_t *g, int sq) {
    if (sq == g->hover) return;
    ch_inval_sq(g, g->hover);
    g->hover = sq;
    ch_inval_sq(g, sq);
}

static void ch_select(chess_t *g, int sq) {
    if (g->sel == sq) return;
    g->sel = sq;
    ch_inval_board(g);
}

/* Click (or Space) on a square. */
static void ch_click_square(chess_t *g, int sq) {
    if (!ch_human_can_move(g) || sq < 0) return;
    if (g->sel >= 0 && ch_is_target(g, g->sel, sq)) {
        ch_try_move(g, g->sel, sq, true);
        return;
    }
    int pc = g->pos.b[sq];
    if (pc && PCOL(pc) == g->pos.side && ch_has_moves(g, sq)) {
        ch_select(g, g->sel == sq ? -1 : sq);
        sys_beep(1200, 6);
    } else {
        ch_select(g, -1);
    }
}

static void ch_choose_promo(chess_t *g, int idx) {
    static const int opts[4] = { QUEEN, KNIGHT, ROOK, BISHOP };
    if (!g->promo_open || idx < 0 || idx > 3) return;
    for (int i = 0; i < g->nlegal; i++) {
        cmove_t m = g->legal[i];
        if (m.from == g->promo_from && m.to == g->promo_to && m.promo == opts[idx]) {
            g->promo_open = false;
            ch_apply(g, m, false);
            return;
        }
    }
}

static void ch_cancel_promo(chess_t *g) {
    g->promo_open = false;
    g->sel = -1;
    ch_inval_board(g);
    ch_inval_panel(g);
}

static void ch_button(chess_t *g, int which) {
    switch (which) {
    case 0: case 3: ch_new_game(g); break;
    case 1: ch_undo(g); break;
    case 2: g->flip = !g->flip; win_invalidate(g->win); break;
    case 4: g->over_shown = false; win_invalidate(g->win); break;
    }
}

static void ch_mouse(chess_t *g, const event_t *ev) {
    bool used = false;
    int fired = fn_btns_event(g->win, g->btn, 5, ev, &used);
    if (fired >= 0) { ch_button(g, fired); return; }
    if (used) return;
    switch (ev->type) {
    case EV_MOUSE_MOVE: {
        for (int i = 0; i < 3; i++) {
            int n = i == 1 ? 3 : 2;
            int h = (i > 0 && g->mode == MODE_2P) ? -1 : fn_seg_hit(g->seg_r[i], n, ev->x, ev->y);
            if (h != g->seg_hover[i]) {
                g->seg_hover[i] = h;
                win_invalidate_rect(g->win, g->seg_r[i].x, g->seg_r[i].y, g->seg_r[i].w, g->seg_r[i].h);
            }
        }
        if (g->promo_open) {
            int h = ch_promo_hit(g, ev->x, ev->y);
            if (h != g->promo_hover) { g->promo_hover = h; ch_inval_board(g); }
            break;
        }
        if (g->over_shown) break;
        int sq = sq_at(g, ev->x, ev->y);
        if (g->press && g->sel >= 0) {
            if (!g->dragging && (abs(ev->x - g->press_x) > 4 || abs(ev->y - g->press_y) > 4)) g->dragging = true;
            if (g->dragging) {
                win_invalidate_rect(g->win, g->drag_x - CH_SQ, g->drag_y - CH_SQ, 2 * CH_SQ, 2 * CH_SQ);
                g->drag_x = ev->x;
                g->drag_y = ev->y;
                win_invalidate_rect(g->win, g->drag_x - CH_SQ, g->drag_y - CH_SQ, 2 * CH_SQ, 2 * CH_SQ);
                ch_inval_sq(g, g->sel);
            }
        }
        ch_set_hover(g, sq);
        bool can = ch_human_can_move(g) && sq >= 0 &&
                   ((g->pos.b[sq] && PCOL(g->pos.b[sq]) == g->pos.side && ch_has_moves(g, sq)) ||
                    (g->sel >= 0 && ch_is_target(g, g->sel, sq)));
        win_set_cursor(g->win, can || g->dragging ? CURSOR_HAND : CURSOR_ARROW);
        if (sq >= 0 && g->kb_on) { g->kb_on = false; ch_inval_board(g); }
        break;
    }
    case EV_MOUSE_LEAVE:
        ch_set_hover(g, -1);
        for (int i = 0; i < 3; i++)
            if (g->seg_hover[i] >= 0) { g->seg_hover[i] = -1; ch_inval_panel(g); }
        break;
    case EV_MOUSE_DOWN: {
        if (ev->button == MOUSE_RIGHT) {
            if (g->promo_open) ch_cancel_promo(g);
            else ch_select(g, -1);
            break;
        }
        if (ev->button != MOUSE_LEFT) break;
        for (int i = 0; i < 3; i++) {
            if (i > 0 && g->mode == MODE_2P) continue;
            int h = fn_seg_hit(g->seg_r[i], i == 1 ? 3 : 2, ev->x, ev->y);
            if (h < 0) continue;
            if (i == 0) ch_set_mode(g, h);
            else if (i == 1) ch_set_level(g, h);
            else ch_set_side(g, h);
            return;
        }
        if (g->promo_open) {
            int h = ch_promo_hit(g, ev->x, ev->y);
            if (h >= 0) ch_choose_promo(g, h);
            else ch_cancel_promo(g);
            break;
        }
        if (g->over_shown) break;
        int sq = sq_at(g, ev->x, ev->y);
        if (sq < 0) { ch_select(g, -1); break; }
        if (!ch_human_can_move(g)) break;
        if (g->sel >= 0 && ch_is_target(g, g->sel, sq)) { ch_try_move(g, g->sel, sq, true); break; }
        int pc = g->pos.b[sq];
        if (pc && PCOL(pc) == g->pos.side && ch_has_moves(g, sq)) {
            bool was = g->sel == sq;
            ch_select(g, sq);
            g->press = true;
            g->dragging = false;
            g->press_x = g->drag_x = ev->x;
            g->press_y = g->drag_y = ev->y;
            g->btn[0].pressed = false;
            if (!was) sys_beep(1200, 6);
            /* a second click on the selected piece (without dragging) deselects it */
            g->kb = sq;
            if (was) g->press_x = -10000;
        } else {
            ch_select(g, -1);
        }
        break;
    }
    case EV_MOUSE_UP: {
        if (ev->button != MOUSE_LEFT || !g->press) break;
        g->press = false;
        if (g->dragging) {
            g->dragging = false;
            int sq = sq_at(g, ev->x, ev->y);
            if (sq >= 0 && sq != g->sel && ch_is_target(g, g->sel, sq)) ch_try_move(g, g->sel, sq, false);
            ch_inval_board(g);
        } else if (g->press_x == -10000) {
            ch_select(g, -1);
        }
        break;
    }
    case EV_MOUSE_WHEEL:
        if (rect_contains(g->list_r, ev->x, ev->y)) {
            int rows = ch_list_rows(g), vis = (g->list_r.h - 34) / 20;
            g->list_scroll = CLAMP(g->list_scroll + ev->wheel * 2, 0, MAX(0, rows - vis));
            win_invalidate_rect(g->win, g->list_r.x, g->list_r.y, g->list_r.w, g->list_r.h);
        }
        break;
    default:
        break;
    }
}

static void ch_key(chess_t *g, const event_t *ev) {
    int k = ev->key;
    if (g->promo_open) {
        if (k == 'q' || k == KEY_ENTER || k == KEY_SPACE) ch_choose_promo(g, 0);
        else if (k == 'n') ch_choose_promo(g, 1);
        else if (k == 'r') ch_choose_promo(g, 2);
        else if (k == 'b') ch_choose_promo(g, 3);
        else if (k == KEY_ESCAPE) ch_cancel_promo(g);
        return;
    }
    if (ev->mods & MOD_CTRL) {
        if (k == 'c') {   /* copy the position as FEN */
            char fen[100];
            pos_to_fen(&g->pos, fen, sizeof fen);
            clipboard_set(fen);
            ui_notify("Chess", "Position copied to the clipboard (FEN)");
        } else if (k == 'v') {   /* set up a position from FEN on the clipboard */
            if (!ch_load_fen(g, clipboard_get()))
                ui_message_box(g->win, "Chess", "The clipboard does not hold a legal chess position (FEN).",
                               MB_OK | MB_ICON_WARNING, NULL, NULL);
        } else if (k == 'z') {
            ch_undo(g);
        } else if (k == 'n') {
            ch_new_game(g);
        }
        return;
    }
    if (k == KEY_F2 || k == 'n') { ch_new_game(g); return; }
    if (k == 'u' || k == KEY_BACKSPACE) { ch_undo(g); return; }
    if (k == 'f') { ch_button(g, 2); return; }
    if (k == KEY_ESCAPE) {
        if (g->over_shown) ch_button(g, 4);
        else ch_select(g, -1);
        return;
    }
    if (g->over_shown) {
        if (k == KEY_ENTER) ch_new_game(g);
        return;
    }
    int dx = 0, dy = 0;
    if (k == KEY_LEFT) dx = -1;
    else if (k == KEY_RIGHT) dx = 1;
    else if (k == KEY_UP) dy = -1;
    else if (k == KEY_DOWN) dy = 1;
    if (dx || dy) {
        if (!g->kb_on) {
            g->kb_on = true;
        } else {
            rect_t q = sq_rect(g, g->kb);
            int cx = CLAMP((q.x - BX) / CH_SQ + dx, 0, 7), cy = CLAMP((q.y - BY) / CH_SQ + dy, 0, 7);
            g->kb = sq_at(g, BX + cx * CH_SQ + 1, BY + cy * CH_SQ + 1);
        }
        ch_inval_board(g);
        return;
    }
    if (k == KEY_SPACE || k == KEY_ENTER) {
        if (!g->kb_on) { g->kb_on = true; ch_inval_board(g); return; }
        ch_click_square(g, g->kb);
    }
}

static void ch_timer(chess_t *g) {
    uint32_t now = sys_uptime_ms();
    if (g->thinking && g->search) {
        search_t *S = g->search;
        if (!S->done) search_step(S, now + 9);
        if (S->done && now - g->think_t0 >= MIN_THINK) {
            cmove_t m = S->best;
            bool ok = false;
            for (int i = 0; i < g->nlegal; i++)
                if (S->have_best && same_move(g->legal[i], m)) { m = g->legal[i]; ok = true; break; }
            if (!ok && g->nlegal) m = g->legal[0];
            klog("chess: %s depth %d, %u nodes, score %d, %u ms", level_names[g->level], S->depth, S->nodes,
                 S->best_score, sys_uptime_ms() - g->think_t0);
            g->thinking = false;
            if (g->nlegal) ch_apply(g, m, true);
        }
        win_invalidate_rect(g->win, g->status_r.x, g->status_r.y, g->status_r.w, g->status_r.h);
    }
    if (g->anim) {
        if (now - g->anim_t0 >= MOVE_MS) g->anim = false;
        ch_inval_board(g);
    }
    if (g->over_shown && now - g->over_t0 < 400) ch_inval_board(g);
    ch_update_timer(g);
}

static void ch_event(window_t *win, const event_t *ev) {
    chess_t *g = win_get_userdata(win);
    if (!g) return;
    switch (ev->type) {
    case EV_PAINT:
        ch_paint(g, win_surface(win));
        break;
    case EV_TIMER:
        ch_timer(g);
        break;
    case EV_MOUSE_MOVE: case EV_MOUSE_DOWN: case EV_MOUSE_UP: case EV_MOUSE_LEAVE:
    case EV_DOUBLE_CLICK: case EV_MOUSE_WHEEL:
        ch_mouse(g, ev);
        break;
    case EV_KEY_DOWN:
        ch_key(g, ev);
        break;
    case EV_BLUR:
        fn_btns_reset(win, g->btn, 5);
        if (g->dragging) { g->dragging = g->press = false; ch_inval_board(g); }
        break;
    case EV_CLOSE:
        free(g->search);
        free(g->hist);
        free(g->hashes);
        free(g);
        win_set_userdata(win, NULL);
        break;
    default:
        break;
    }
}

static void ch_launch(const char *arg) {
    chess_t *g = calloc(1, sizeof *g);
    if (!g) return;
    g->hist = calloc(MAXHIST, sizeof(hist_t));
    g->hashes = calloc(MAXHIST + 1, sizeof(uint64_t));
    if (!g->hist || !g->hashes) { free(g->hist); free(g->hashes); free(g); return; }
    fn_rng_seed(&g->rng, (uint32_t)(uintptr_t)g);
    ch_sprites_init();
    g->hover = g->sel = g->promo_hover = -1;
    for (int i = 0; i < 3; i++) g->seg_hover[i] = -1;
    g->seg_r[0] = rect_make(PX, 20, PW, 30);
    g->seg_r[1] = rect_make(PX, 58, PW, 28);
    g->seg_r[2] = rect_make(PX, 92, PW, 28);
    g->status_r = rect_make(PX, 130, PW, 62);
    g->cap_r = rect_make(PX, 202, PW, 46);
    g->list_r = rect_make(PX, 256, PW, 196);
    int bw = (PW - 16 - 96) / 2;
    g->btn[0] = (fn_btn_t){ .r = rect_make(PX, 464, 96, 34), .label = "New Game" };
    g->btn[1] = (fn_btn_t){ .r = rect_make(PX + 96 + 8, 464, bw, 34), .label = "Undo" };
    g->btn[2] = (fn_btn_t){ .r = rect_make(PX + 96 + bw + 16, 464, PW - 96 - bw - 16, 34), .label = "Flip" };
    g->btn[3] = (fn_btn_t){ .label = "New Game", .primary = true, .hidden = true };
    g->btn[4] = (fn_btn_t){ .label = "View Board", .hidden = true };
    g->mode = CLAMP(fn_score_get("chess_mode", MODE_AI), 0, 1);
    g->level = CLAMP(fn_score_get("chess_level", 1), 0, 2);
    g->human = CLAMP(fn_score_get("chess_side", WHITE), 0, 1);
    /* arg: a .fen file to open, or options "2p ai white black easy medium hard" optionally
       followed by "fen:<FEN>" (autorun=Chess:2p+fen:8/8/...+w+-+-) */
    char *fen = NULL;
    if (arg && *arg == '/') {
        uint32_t size = 0;
        fen = vfs_read_file(arg, &size);
        if (!fen) notify("Chess", "Could not read the position file", ICON_ERROR);
    } else if (arg && *arg) {
        const char *fp = strstr(arg, "fen:");
        int n = fp ? (int)(fp - arg) : (int)strlen(arg);
        char opt[64];
        snprintf(opt, sizeof opt, "%.*s", MIN(n, 63), arg);
        if (strstr(opt, "2p")) g->mode = MODE_2P;
        if (strstr(opt, "ai")) g->mode = MODE_AI;
        if (strstr(opt, "black")) g->human = BLACK;
        if (strstr(opt, "white")) g->human = WHITE;
        if (strstr(opt, "easy")) g->level = 0;
        if (strstr(opt, "medium")) g->level = 1;
        if (strstr(opt, "hard")) g->level = 2;
        if (fp) fen = strdup(fp + 4);
    }
    g->win = win_create("Chess", CH_W, CH_H, WIN_CENTER);
    if (!g->win) { free(fen); free(g->hist); free(g->hashes); free(g); return; }
    win_set_userdata(g->win, g);
    win_set_event_handler(g->win, ch_event);
    if (fen) {
        if (!ch_load_fen(g, fen)) {
            ui_message_box(g->win, "Chess", "That is not a legal chess position (FEN).\nStarting a new game instead.",
                           MB_OK | MB_ICON_WARNING, NULL, NULL);
            ch_new_game(g);
        }
        free(fen);
    } else {
        ch_new_game(g);
    }
}

/* Icon: a white knight on a framed 4x4 board. */
static void ch_icon(surface_t *s, int x, int y, int size) {
    double u = size / 48.0;
    gfx_fill_rounded_gradient_v(s, x + (int)(2 * u), y + (int)(2 * u), (int)(44 * u), (int)(44 * u), (int)(9 * u),
                                RGB(132, 94, 66), RGB(86, 58, 40));
    int bx = x + (int)lround(6 * u), by = y + (int)lround(6 * u);
    double q = 36 * u / 4;
    for (int r = 0; r < 4; r++)
        for (int f = 0; f < 4; f++) {
            int x0 = bx + (int)lround(f * q), y0 = by + (int)lround(r * q);
            int x1 = bx + (int)lround((f + 1) * q), y1 = by + (int)lround((r + 1) * q);
            gfx_fill_rect(s, x0, y0, x1 - x0, y1 - y0, (r + f) & 1 ? SQ_DARK : SQ_LIGHT);
        }
    int ps = (int)lround(40 * u);
    surface_t *kn = render_piece(KNIGHT, WHITE, ps);
    if (kn) {
        gfx_blit_alpha(s, x + (size - ps) / 2, y + (int)lround(3 * u), kn, 255);
        surface_destroy(kn);
    }
}

REGISTER_APP(.name = "Chess", .category = CAT_GAMES,
             .description = "Play chess against the computer or a friend",
             .launch = ch_launch, .draw_icon = ch_icon, .file_types = "fen");
