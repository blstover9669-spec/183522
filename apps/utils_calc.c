/*
 * utils_calc.c - Calculator (Accessories).
 *
 * Standard and scientific modes, a custom-drawn key pad (hover / pressed
 * states, keyboard presses flash the matching key), chained operations with
 * Windows-style immediate execution ("2 + 3 * 4 =" -> 20), repeated "=",
 * memory keys, percent, unary functions, degree/radian trig, clipboard copy
 * and paste, and readable error messages ("Cannot divide by zero").
 *
 * Everything is per-window state, so any number of calculators can be open.
 */
#include <nova.h>

/* ---- keys ------------------------------------------------------------------------ */
enum {
    K_0 = 0, K_1, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9,
    K_DOT, K_ADD, K_SUB, K_MUL, K_DIV, K_EQ, K_PCT, K_NEG,
    K_CE, K_C, K_BS, K_SQRT, K_SQR, K_RECIP,
    K_MC, K_MR, K_MADD, K_MSUB,
    K_2ND, K_DEG, K_SIN, K_COS, K_TAN, K_LN, K_LOG, K_EXP, K_TENX,
    K_POW, K_PI, K_E, K_FACT, K_ABS,
};

enum { KIND_DIGIT, KIND_FUNC, KIND_OP, KIND_EQ, KIND_FLAT };

typedef struct { const char *label; int key; int kind; } keydef_t;

#define ROWS 7
static const keydef_t std_keys[ROWS][4] = {
    { { "MC", K_MC, KIND_FLAT }, { "MR", K_MR, KIND_FLAT }, { "M+", K_MADD, KIND_FLAT }, { "M-", K_MSUB, KIND_FLAT } },
    { { "%", K_PCT, KIND_FUNC }, { "CE", K_CE, KIND_FUNC }, { "C", K_C, KIND_FUNC }, { "#bs", K_BS, KIND_FUNC } },
    { { "1/x", K_RECIP, KIND_FUNC }, { "x^2", K_SQR, KIND_FUNC }, { "#sqrt", K_SQRT, KIND_FUNC }, { "#div", K_DIV, KIND_OP } },
    { { "7", K_7, KIND_DIGIT }, { "8", K_8, KIND_DIGIT }, { "9", K_9, KIND_DIGIT }, { "#mul", K_MUL, KIND_OP } },
    { { "4", K_4, KIND_DIGIT }, { "5", K_5, KIND_DIGIT }, { "6", K_6, KIND_DIGIT }, { "#sub", K_SUB, KIND_OP } },
    { { "1", K_1, KIND_DIGIT }, { "2", K_2, KIND_DIGIT }, { "3", K_3, KIND_DIGIT }, { "#add", K_ADD, KIND_OP } },
    { { "+/-", K_NEG, KIND_DIGIT }, { "0", K_0, KIND_DIGIT }, { ".", K_DOT, KIND_DIGIT }, { "#eq", K_EQ, KIND_EQ } },
};
/* Extra columns on the left in scientific mode. Labels for "2nd" variants are chosen at draw time. */
static const keydef_t sci_keys[ROWS][2] = {
    { { "2nd", K_2ND, KIND_FLAT }, { "DEG", K_DEG, KIND_FLAT } },
    { { "sin", K_SIN, KIND_FUNC }, { "x^y", K_POW, KIND_FUNC } },
    { { "cos", K_COS, KIND_FUNC }, { "#pi", K_PI, KIND_FUNC } },
    { { "tan", K_TAN, KIND_FUNC }, { "e", K_E, KIND_FUNC } },
    { { "ln", K_LN, KIND_FUNC }, { "n!", K_FACT, KIND_FUNC } },
    { { "log", K_LOG, KIND_FUNC }, { "|x|", K_ABS, KIND_FUNC } },
    { { "e^x", K_EXP, KIND_FUNC }, { "10^x", K_TENX, KIND_FUNC } },
};

typedef struct { rect_t r; const keydef_t *def; } btn_t;

#define STD_W 320
#define SCI_W 468
#define DEF_H 500
#define PAD   8
#define GAP   4
#define MAX_ENTRY 16      /* significant digits the user may type */

typedef struct {
    window_t *win;
    /* calculator state */
    bool   sci, deg, second;
    char   entry[40];     /* number being typed (valid while entering) */
    bool   entering;
    bool   fresh;         /* a new operand was supplied since the last operator */
    bool   evaluated;     /* the display shows the result of "=" */
    double cur;           /* displayed value when not entering */
    double acc;           /* left operand of the pending operator */
    int    op;            /* pending binary operator ('+', '-', '*', '/', '^', 'r') or 0 */
    int    last_op;       /* for repeated "=" */
    double last_rhs;
    char   expr[160];     /* expression line above the number */
    char   uexpr[96];     /* description of the current operand ("sqrt(9)") or "" */
    const char *err;      /* error message or NULL */
    double mem;
    bool   has_mem;
    /* layout + interaction */
    btn_t  btn[ROWS * 6];
    int    nbtn;
    rect_t mode_r;        /* "Standard / Scientific" toggle */
    int    disp_h;
    int    hover, pressed, flash;
    bool   mode_hover;
    uint32_t flash_until;
    int    timer;
} calc_t;

/* ---- number formatting ------------------------------------------------------------- */
static void strip_zeros(char *s) {
    char *dot = strchr(s, '.');
    if (!dot) return;
    char *e = strchr(s, 'e');
    char tail[16] = "";
    if (e) { strlcpy(tail, e, sizeof tail); *e = 0; }
    int n = (int)strlen(s);
    while (n > 0 && s[n - 1] == '0') s[--n] = 0;
    if (n > 0 && s[n - 1] == '.') s[--n] = 0;
    strlcat(s, tail, 64);
}

/* Up to 13 significant digits (hides binary floating point noise), exponent form for
   very large or very small magnitudes. */
static void fmt_plain(double v, char *out, int n) {
    if (v == 0 || fabs(v) < 1e-300) { strlcpy(out, "0", n); return; }
    double a = fabs(v);
    int e = (int)floor(log10(a));
    char buf[64];
    if (e >= 16 || e <= -8) {
        snprintf(buf, sizeof buf, "%.12e", v);
    } else {
        int dec = CLAMP(12 - e, 0, 15);
        snprintf(buf, sizeof buf, "%.*f", dec, v);
    }
    strip_zeros(buf);
    if (!strcmp(buf, "-0")) strcpy(buf, "0");
    strlcpy(out, buf, n);
}

/* Insert thousands separators into the integer part of a plain number string. */
static void group(const char *in, char *out, int n) {
    if (strchr(in, 'e')) { strlcpy(out, in, n); return; }
    int o = 0;
    const char *p = in;
    if (*p == '-') { if (o < n - 1) out[o++] = *p; p++; }
    int digits = 0;
    while (p[digits] >= '0' && p[digits] <= '9') digits++;
    for (int i = 0; i < digits && o < n - 1; i++) {
        out[o++] = p[i];
        int left = digits - i - 1;
        if (left > 0 && left % 3 == 0 && o < n - 1) out[o++] = ',';
    }
    p += digits;
    while (*p && o < n - 1) out[o++] = *p++;
    out[o] = 0;
}

/* ---- calculator logic --------------------------------------------------------------- */
static double cur_value(calc_t *c) { return c->entering ? strtod(c->entry, NULL) : c->cur; }

static void operand_text(calc_t *c, char *out, int n) {
    if (c->uexpr[0]) { strlcpy(out, c->uexpr, n); return; }
    if (c->entering) {
        strlcpy(out, c->entry, n);
        int l = (int)strlen(out);
        if (l > 1 && out[l - 1] == '.') out[l - 1] = 0;
        return;
    }
    fmt_plain(c->cur, out, n);
}

static const char *op_str(int op) {
    switch (op) {
    case '+': return "+";
    case '-': return "-";
    case '*': return "\xD7";
    case '/': return "\xF7";
    case '^': return "^";
    case 'r': return "yroot";
    }
    return "?";
}

static void set_error(calc_t *c, const char *msg) {
    c->err = msg;
    c->op = 0;
    c->last_op = 0;
    c->entering = false;
    c->fresh = false;
    c->cur = 0;
    c->uexpr[0] = 0;
    sys_beep(220, 60);
}

static void clear_all(calc_t *c) {
    c->err = NULL;
    c->op = 0;
    c->last_op = 0;
    c->entering = false;
    c->fresh = false;
    c->evaluated = false;
    c->cur = 0;
    c->acc = 0;
    c->expr[0] = 0;
    c->uexpr[0] = 0;
}

/* Result checks: NaN -> invalid input, inf -> overflow. */
static bool check_result(calc_t *c, double v) {
    if (isnan(v)) { set_error(c, "Invalid input"); return false; }
    if (isinf(v)) { set_error(c, "Overflow"); return false; }
    return true;
}

static bool apply(calc_t *c, double a, int op, double b, double *res) {
    double r = 0;
    switch (op) {
    case '+': r = a + b; break;
    case '-': r = a - b; break;
    case '*': r = a * b; break;
    case '/':
        if (b == 0) { set_error(c, a == 0 ? "Result is undefined" : "Cannot divide by zero"); return false; }
        r = a / b;
        break;
    case '^':
        if (a == 0 && b < 0) { set_error(c, "Cannot divide by zero"); return false; }
        r = pow(a, b);
        break;
    case 'r':   /* b-th root of a */
        if (b == 0) { set_error(c, "Invalid input"); return false; }
        if (a < 0) {
            double bi = round(b);
            if (bi != b || fmod(fabs(bi), 2.0) != 1.0) { set_error(c, "Invalid input"); return false; }
            r = -pow(-a, 1.0 / b);
        } else {
            r = pow(a, 1.0 / b);
        }
        break;
    default: r = b; break;
    }
    if (!check_result(c, r)) return false;
    if (fabs(r) < 1e-300) r = 0;
    *res = r;
    return true;
}

static void begin_new_if_evaluated(calc_t *c) {
    if (c->evaluated) {
        c->expr[0] = 0;
        c->evaluated = false;
    }
}

static void input_digit(calc_t *c, int d) {
    if (c->err) clear_all(c);
    begin_new_if_evaluated(c);
    if (!c->entering) {
        strcpy(c->entry, "0");
        c->entering = true;
        c->uexpr[0] = 0;
    }
    int digits = 0;
    for (const char *p = c->entry; *p; p++) if (isdigit((unsigned char)*p)) digits++;
    if (!strcmp(c->entry, "0")) c->entry[0] = 0;
    else if (!strcmp(c->entry, "-0")) c->entry[1] = 0;
    else if (digits >= MAX_ENTRY) return;
    int l = (int)strlen(c->entry);
    c->entry[l] = (char)('0' + d);
    c->entry[l + 1] = 0;
    c->fresh = true;
}

static void input_dot(calc_t *c) {
    if (c->err) clear_all(c);
    begin_new_if_evaluated(c);
    if (!c->entering) {
        strcpy(c->entry, "0");
        c->entering = true;
        c->uexpr[0] = 0;
    }
    if (!strchr(c->entry, '.')) strlcat(c->entry, ".", sizeof c->entry);
    c->fresh = true;
}

static void do_backspace(calc_t *c) {
    if (c->err) { clear_all(c); return; }
    if (c->evaluated) { c->expr[0] = 0; return; }
    if (!c->entering) return;
    int l = (int)strlen(c->entry);
    if (l > 0) c->entry[--l] = 0;
    if (l == 0 || !strcmp(c->entry, "-")) strcpy(c->entry, "0");
}

static void do_binary(calc_t *c, int op) {
    if (c->err) return;
    char lhs[96];
    if (c->op && c->fresh) {
        /* chained operation: evaluate the pending one first */
        double v = cur_value(c), r;
        int prev = c->op;
        char a[64], b[96];
        fmt_plain(c->acc, a, sizeof a);
        operand_text(c, b, sizeof b);
        if (!apply(c, c->acc, prev, v, &r)) {
            snprintf(c->expr, sizeof c->expr, "%s %s %s", a, op_str(prev), b);
            return;
        }
        c->acc = r;
        fmt_plain(r, lhs, sizeof lhs);
    } else if (!c->op) {
        c->acc = cur_value(c);
        operand_text(c, lhs, sizeof lhs);   /* keeps "sqrt(9)" style descriptions */
    } else {
        fmt_plain(c->acc, lhs, sizeof lhs);  /* just changing the operator */
    }
    c->evaluated = false;
    c->op = op;
    c->entering = false;
    c->fresh = false;
    c->cur = c->acc;
    snprintf(c->expr, sizeof c->expr, "%s %s", lhs, op_str(op));
    c->uexpr[0] = 0;
}

static void do_equals(calc_t *c) {
    if (c->err) { clear_all(c); return; }
    char lhs[64], rhs[96];
    double r;
    if (c->op) {
        double b = c->fresh ? cur_value(c) : c->acc;
        if (c->fresh) operand_text(c, rhs, sizeof rhs); else fmt_plain(b, rhs, sizeof rhs);
        fmt_plain(c->acc, lhs, sizeof lhs);
        int op = c->op;
        double a = c->acc;
        if (!apply(c, a, op, b, &r)) { snprintf(c->expr, sizeof c->expr, "%s %s %s =", lhs, op_str(op), rhs); return; }
        snprintf(c->expr, sizeof c->expr, "%s %s %s =", lhs, op_str(op), rhs);
        c->last_op = op;
        c->last_rhs = b;
    } else if (c->last_op) {
        double a = cur_value(c);
        fmt_plain(a, lhs, sizeof lhs);
        fmt_plain(c->last_rhs, rhs, sizeof rhs);
        int op = c->last_op;
        if (!apply(c, a, op, c->last_rhs, &r)) { snprintf(c->expr, sizeof c->expr, "%s %s %s =", lhs, op_str(op), rhs); return; }
        snprintf(c->expr, sizeof c->expr, "%s %s %s =", lhs, op_str(op), rhs);
    } else {
        operand_text(c, rhs, sizeof rhs);
        r = cur_value(c);
        snprintf(c->expr, sizeof c->expr, "%s =", rhs);
    }
    c->op = 0;
    c->cur = r;
    c->acc = r;
    c->entering = false;
    c->fresh = false;
    c->evaluated = true;
    c->uexpr[0] = 0;
}

/* Replace the current operand by f(operand); name is used for the expression line. */
static void set_operand(calc_t *c, double v, const char *desc) {
    if (!check_result(c, v)) return;
    if (fabs(v) < 1e-300) v = 0;
    if (c->evaluated) { c->expr[0] = 0; c->evaluated = false; }
    c->cur = v;
    c->entering = false;
    c->fresh = true;
    if (desc) strlcpy(c->uexpr, desc, sizeof c->uexpr);
    else c->uexpr[0] = 0;
    /* show the pending part of the expression plus the operand description */
    if (c->op) {
        char a[64];
        fmt_plain(c->acc, a, sizeof a);
        if (c->uexpr[0]) snprintf(c->expr, sizeof c->expr, "%s %s %s", a, op_str(c->op), c->uexpr);
        else snprintf(c->expr, sizeof c->expr, "%s %s", a, op_str(c->op));
    } else {
        strlcpy(c->expr, c->uexpr, sizeof c->expr);
    }
}

static void do_unary(calc_t *c, int key) {
    if (c->err) return;
    double v = cur_value(c), r = 0;
    char opnd[96], desc[128];
    operand_text(c, opnd, sizeof opnd);
    double rad = c->deg ? v * M_PI / 180.0 : v;
    const char *name = NULL;
    switch (key) {
    case K_SQRT:
        if (v < 0) { set_error(c, "Invalid input"); return; }
        r = sqrt(v); name = "sqrt"; break;
    case K_SQR: r = v * v; name = "sqr"; break;
    case K_RECIP:
        if (v == 0) { set_error(c, "Cannot divide by zero"); return; }
        r = 1.0 / v; name = "1/"; break;
    case K_SIN:
        if (c->second) {
            if (v < -1 || v > 1) { set_error(c, "Invalid input"); return; }
            r = asin(v); if (c->deg) r = r * 180.0 / M_PI; name = "asin";
        } else { r = sin(rad); name = "sin"; }
        break;
    case K_COS:
        if (c->second) {
            if (v < -1 || v > 1) { set_error(c, "Invalid input"); return; }
            r = acos(v); if (c->deg) r = r * 180.0 / M_PI; name = "acos";
        } else { r = cos(rad); name = "cos"; }
        break;
    case K_TAN:
        if (c->second) { r = atan(v); if (c->deg) r = r * 180.0 / M_PI; name = "atan"; }
        else {
            if (c->deg && fmod(fabs(v), 180.0) == 90.0) { set_error(c, "Invalid input"); return; }
            r = tan(rad); name = "tan";
            if (fabs(r) > 1e15) { set_error(c, "Invalid input"); return; }
        }
        break;
    case K_LN:
        if (v <= 0) { set_error(c, "Invalid input"); return; }
        r = log(v); name = "ln"; break;
    case K_LOG:
        if (v <= 0) { set_error(c, "Invalid input"); return; }
        r = log10(v); name = "log"; break;
    case K_EXP: r = exp(v); name = "e^"; break;
    case K_TENX: r = pow(10.0, v); name = "10^"; break;
    case K_ABS: r = fabs(v); name = "abs"; break;
    case K_FACT: {
        if (v < 0 || v != floor(v)) { set_error(c, "Invalid input"); return; }
        if (v > 170) { set_error(c, "Overflow"); return; }
        r = 1;
        for (int i = 2; i <= (int)v; i++) r *= i;
        name = "fact";
        break;
    }
    default: return;
    }
    /* trig results within rounding noise of an integer are snapped (sin 180 = 0) */
    if (key == K_SIN || key == K_COS || key == K_TAN) {
        if (fabs(r) < 1e-12) r = 0;
        else if (fabs(r - round(r)) < 1e-12) r = round(r);
    }
    snprintf(desc, sizeof desc, "%s(%s)", name, opnd);
    set_operand(c, r, desc);
}

static void do_percent(calc_t *c) {
    if (c->err) return;
    double v = cur_value(c), r;
    if (c->op == '+' || c->op == '-') r = c->acc * v / 100.0;
    else r = v / 100.0;
    char d[64];
    fmt_plain(r, d, sizeof d);
    set_operand(c, r, d);
}

static void do_negate(calc_t *c) {
    if (c->err) return;
    if (c->entering) {
        if (c->entry[0] == '-') memmove(c->entry, c->entry + 1, strlen(c->entry));
        else if (strcmp(c->entry, "0") != 0) {
            memmove(c->entry + 1, c->entry, strlen(c->entry) + 1);
            c->entry[0] = '-';
        }
        return;
    }
    char opnd[96], desc[128];
    operand_text(c, opnd, sizeof opnd);
    snprintf(desc, sizeof desc, "negate(%s)", opnd);
    set_operand(c, -cur_value(c), desc);
}

static void press_key(calc_t *c, int key) {
    switch (key) {
    case K_0: case K_1: case K_2: case K_3: case K_4:
    case K_5: case K_6: case K_7: case K_8: case K_9:
        input_digit(c, key - K_0); break;
    case K_DOT: input_dot(c); break;
    case K_ADD: do_binary(c, '+'); break;
    case K_SUB: do_binary(c, '-'); break;
    case K_MUL: do_binary(c, '*'); break;
    case K_DIV: do_binary(c, '/'); break;
    case K_POW: do_binary(c, c->second ? 'r' : '^'); break;
    case K_EQ: do_equals(c); break;
    case K_PCT: do_percent(c); break;
    case K_NEG: do_negate(c); break;
    case K_CE:
        if (c->err) { clear_all(c); break; }
        if (c->evaluated) { clear_all(c); break; }
        c->entering = false; c->cur = 0; c->uexpr[0] = 0; c->fresh = true;
        break;
    case K_C: clear_all(c); break;
    case K_BS: do_backspace(c); break;
    case K_SQRT: case K_SQR: case K_RECIP: case K_SIN: case K_COS: case K_TAN:
    case K_LN: case K_LOG: case K_EXP: case K_TENX: case K_ABS: case K_FACT:
        do_unary(c, key); break;
    case K_PI: if (c->err) clear_all(c); set_operand(c, M_PI, NULL); break;
    case K_E: if (c->err) clear_all(c); set_operand(c, M_E, NULL); break;
    case K_MC: c->mem = 0; c->has_mem = false; break;
    case K_MR:
        if (!c->has_mem) break;
        if (c->err) clear_all(c);
        set_operand(c, c->mem, NULL);
        break;
    case K_MADD: case K_MSUB:
        if (c->err) break;
        c->mem += key == K_MADD ? cur_value(c) : -cur_value(c);
        c->has_mem = true;
        if (c->entering) { c->cur = cur_value(c); c->entering = false; }
        break;
    case K_2ND: c->second = !c->second; break;
    case K_DEG: c->deg = !c->deg; break;
    default: break;
    }
}

/* ---- layout ----------------------------------------------------------------------- */
static void layout(calc_t *c) {
    int W = win_width(c->win), H = win_height(c->win);
    int cols = c->sci ? 6 : 4;
    c->disp_h = CLAMP(H * 27 / 100, 116, 190);
    int mem_h = 32;
    int avail_h = H - c->disp_h - mem_h - PAD - GAP * 6;
    int row_h = MAX(24, avail_h / 6);
    int avail_w = W - 2 * PAD - GAP * (cols - 1);
    c->nbtn = 0;
    int y = c->disp_h;
    for (int r = 0; r < ROWS; r++) {
        int h = r == 0 ? mem_h : row_h;
        int x = PAD;
        for (int col = 0; col < cols; col++) {
            int w = avail_w / cols + (col < avail_w % cols ? 1 : 0);
            const keydef_t *d;
            if (c->sci) d = col < 2 ? &sci_keys[r][col] : &std_keys[r][col - 2];
            else d = &std_keys[r][col];
            c->btn[c->nbtn].r = rect_make(x, y, w, h);
            c->btn[c->nbtn].def = d;
            c->nbtn++;
            x += w + GAP;
        }
        y += h + (r == 0 ? GAP : GAP);
    }
    c->mode_r = rect_make(6, 6, 124, 28);
}

static int hit_test(calc_t *c, int x, int y) {
    for (int i = 0; i < c->nbtn; i++)
        if (rect_contains(c->btn[i].r, x, y)) return i;
    return -1;
}

static int find_key(calc_t *c, int key) {
    for (int i = 0; i < c->nbtn; i++)
        if (c->btn[i].def->key == key) return i;
    return -1;
}

static void invalidate_btn(calc_t *c, int i) {
    if (i < 0 || i >= c->nbtn) return;
    rect_t r = c->btn[i].r;
    win_invalidate_rect(c->win, r.x - 2, r.y - 2, r.w + 4, r.h + 4);
}

static void set_mode(calc_t *c, bool sci) {
    if (c->sci == sci) return;
    c->sci = sci;
    c->hover = c->pressed = c->flash = -1;
    int W = win_width(c->win), H = win_height(c->win);
    win_set_min_size(c->win, sci ? 400 : 260, 420);
    if (!win_is_maximized(c->win)) {
        /* keep the key width: add or remove the two scientific columns */
        int nw = sci ? W + (SCI_W - STD_W) : W - (SCI_W - STD_W);
        nw = MAX(nw, sci ? 400 : 260);
        win_resize(c->win, nw, H);   /* EV_RESIZE re-lays out */
    }
    layout(c);
    win_invalidate(c->win);
}

/* ---- drawing ---------------------------------------------------------------------- */
/* Thick anti-aliased line: filled quad plus Wu lines along both long edges. */
static void aa_stroke(surface_t *s, double x0, double y0, double x1, double y1, double wdt, uint32_t col) {
    double dx = x1 - x0, dy = y1 - y0, len = sqrt(dx * dx + dy * dy);
    if (len < 0.01) return;
    double nx = -dy / len * wdt / 2, ny = dx / len * wdt / 2;
    point_t p[4] = {
        { (int)lround(x0 + nx), (int)lround(y0 + ny) }, { (int)lround(x1 + nx), (int)lround(y1 + ny) },
        { (int)lround(x1 - nx), (int)lround(y1 - ny) }, { (int)lround(x0 - nx), (int)lround(y0 - ny) },
    };
    gfx_fill_polygon(s, p, 4, col);
    gfx_line_aa(s, x0 + nx, y0 + ny, x1 + nx, y1 + ny, col);
    gfx_line_aa(s, x0 - nx, y0 - ny, x1 - nx, y1 - ny, col);
}

static void draw_glyph(surface_t *s, const char *g, rect_t r, uint32_t col) {
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    if (!strcmp(g, "add")) {
        gfx_fill_rect(s, cx - 7, cy - 1, 14, 2, col);
        gfx_fill_rect(s, cx - 1, cy - 7, 2, 14, col);
    } else if (!strcmp(g, "sub")) {
        gfx_fill_rect(s, cx - 7, cy - 1, 14, 2, col);
    } else if (!strcmp(g, "mul")) {
        aa_stroke(s, cx - 5, cy - 5, cx + 5, cy + 5, 1.8, col);
        aa_stroke(s, cx - 5, cy + 5, cx + 5, cy - 5, 1.8, col);
    } else if (!strcmp(g, "div")) {
        gfx_fill_rect(s, cx - 7, cy - 1, 14, 2, col);
        gfx_fill_circle_aa(s, cx, cy - 5, 1.7, col);
        gfx_fill_circle_aa(s, cx, cy + 5, 1.7, col);
    } else if (!strcmp(g, "eq")) {
        gfx_fill_rect(s, cx - 7, cy - 4, 14, 2, col);
        gfx_fill_rect(s, cx - 7, cy + 2, 14, 2, col);
    } else if (!strcmp(g, "bs")) {
        /* backspace key: a tag pointing left with an x inside */
        double l = cx - 10, rr = cx + 9, t = cy - 6, b = cy + 6;
        aa_stroke(s, l, cy, l + 6, t, 1.4, col);
        aa_stroke(s, l, cy, l + 6, b, 1.4, col);
        aa_stroke(s, l + 6, t, rr, t, 1.4, col);
        aa_stroke(s, l + 6, b, rr, b, 1.4, col);
        aa_stroke(s, rr, t, rr, b, 1.4, col);
        aa_stroke(s, cx - 1, cy - 3, cx + 5, cy + 3, 1.4, col);
        aa_stroke(s, cx - 1, cy + 3, cx + 5, cy - 3, 1.4, col);
    } else if (!strcmp(g, "sqrt")) {
        /* radical sign over an "x" */
        int tw = font_text_width(FONT_UI, "x");
        double x0 = cx - (tw + 12) / 2.0;
        aa_stroke(s, x0, cy + 1, x0 + 3, cy, 1.3, col);
        aa_stroke(s, x0 + 3, cy, x0 + 6, cy + 6, 1.3, col);
        aa_stroke(s, x0 + 6, cy + 6, x0 + 9, cy - 7, 1.3, col);
        aa_stroke(s, x0 + 9, cy - 7, x0 + 13 + tw, cy - 7, 1.3, col);
        gfx_text(s, (int)x0 + 11, cy - font_height(FONT_UI) / 2 + 1, "x", col, FONT_UI);
    } else if (!strcmp(g, "pi")) {
        aa_stroke(s, cx - 6.5, cy - 4.5, cx + 6.5, cy - 4.5, 1.5, col);
        aa_stroke(s, cx - 2.5, cy - 4.5, cx - 3.5, cy + 6, 1.5, col);
        aa_stroke(s, cx + 2.5, cy - 4.5, cx + 2.5, cy + 4, 1.5, col);
        aa_stroke(s, cx + 2.5, cy + 4, cx + 5, cy + 6, 1.3, col);
    }
}

/* Labels: "#glyph", "base^sup" (superscript) or plain text. */
static void draw_label(surface_t *s, const char *label, rect_t r, uint32_t col, bool big) {
    if (label[0] == '#') { draw_glyph(s, label + 1, r, col); return; }
    const char *caret = strchr(label, '^');
    if (!caret) {
        gfx_text_aligned(s, r, label, col, big ? FONT_UI_LARGE : FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);
        return;
    }
    char base[16];
    int bl = MIN((int)(caret - label), 15);
    memcpy(base, label, (size_t)bl);
    base[bl] = 0;
    const char *sup = caret + 1;
    const font_t *bf = bl == 1 ? FONT_UI_LARGE : FONT_UI;
    int bw = font_text_width(bf, base), sw = font_text_width(FONT_UI, sup) - 1;
    int x = r.x + (r.w - bw - sw) / 2;
    int by = r.y + (r.h - font_height(bf)) / 2 + 1;
    gfx_text(s, x, by, base, col, bf);
    int sy = r.y + (r.h - font_height(FONT_UI)) / 2 - (bl == 1 ? 6 : 5);
    gfx_text(s, x + bw, sy, sup, col, FONT_UI);
}

static const char *key_label(calc_t *c, const keydef_t *d) {
    if (d->key == K_DEG) return c->deg ? "DEG" : "RAD";
    if (!c->second) return d->label;
    switch (d->key) {
    case K_SIN: return "sin^-1";
    case K_COS: return "cos^-1";
    case K_TAN: return "tan^-1";
    case K_POW: return "x^1/y";
    default: return d->label;
    }
}

static void draw_key(calc_t *c, surface_t *s, int i) {
    const theme_t *t = theme_get();
    const keydef_t *d = c->btn[i].def;
    rect_t r = c->btn[i].r;
    bool hover = i == c->hover && (c->pressed < 0 || c->pressed == i);
    bool down = (i == c->pressed && i == c->hover) || i == c->flash;
    bool disabled = (d->key == K_MC || d->key == K_MR) && !c->has_mem;
    bool active = (d->key == K_2ND && c->second);
    uint32_t bg, border, fg = t->window_text;
    switch (d->kind) {
    case KIND_DIGIT:
        bg = t->dark ? RGB(59, 61, 69) : RGB(255, 255, 255);
        border = t->dark ? RGB(70, 73, 82) : RGB(221, 224, 231);
        break;
    case KIND_EQ:
        bg = t->accent;
        border = color_shade(t->accent, -20);
        fg = t->accent_text;
        break;
    case KIND_FLAT:
        bg = active ? color_mix(t->window_bg, t->accent, 60) : t->window_bg;
        border = bg;
        if (active) fg = t->dark ? color_shade(t->accent, 60) : color_shade(t->accent, -40);
        break;
    default:
        bg = t->dark ? RGB(45, 47, 54) : RGB(238, 240, 245);
        border = t->dark ? RGB(56, 58, 66) : RGB(224, 227, 234);
        break;
    }
    if (disabled) {
        fg = t->disabled_text;
    } else if (down) {
        bg = d->kind == KIND_EQ ? color_shade(t->accent, -30) : color_mix(bg, t->window_text, 30);
        border = bg;
    } else if (hover) {
        bg = d->kind == KIND_EQ ? t->accent_hover : color_mix(bg, t->window_text, d->kind == KIND_FLAT ? 18 : 12);
        if (d->kind == KIND_FLAT) border = bg;
    }
    int rad = 6;
    if (border != bg) {
        gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, rad, border);
        gfx_fill_rounded_rect(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, rad - 1, bg);
    } else if (bg != t->window_bg) {
        gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, rad, bg);
    }
    rect_t tr = r;
    if (down) tr.y += 1;
    draw_label(s, key_label(c, d), tr, fg, d->kind == KIND_DIGIT);
}

static void display_text(calc_t *c, char *out, int n) {
    char plain[64];
    if (c->entering) strlcpy(plain, c->entry, sizeof plain);
    else fmt_plain(c->cur, plain, sizeof plain);
    group(plain, out, n);
}

static void paint(calc_t *c, surface_t *s) {
    const theme_t *t = theme_get();
    int W = s->w;
    uint32_t muted = color_mix(t->window_text, t->window_bg, 110);

    /* mode toggle: hamburger + mode name */
    rect_t m = c->mode_r;
    if (c->mode_hover) gfx_fill_rounded_rect(s, m.x, m.y, m.w, m.h, 6, color_mix(t->window_bg, t->window_text, 18));
    for (int i = 0; i < 3; i++) gfx_fill_rect(s, m.x + 9, m.y + 9 + i * 5, 14, 2, t->window_text);
    gfx_text(s, m.x + 32, m.y + (m.h - font_height(FONT_UI_BOLD)) / 2, c->sci ? "Scientific" : "Standard",
             t->window_text, FONT_UI_BOLD);

    /* indicators */
    int ix = W - 14;
    if (c->has_mem) {
        int w = font_text_width(FONT_UI_BOLD, "M");
        ix -= w;
        gfx_text(s, ix, 12, "M", t->accent, FONT_UI_BOLD);
        ix -= 10;
    }
    if (c->sci) {
        const char *u = c->deg ? "DEG" : "RAD";
        ix -= font_text_width(FONT_UI, u);
        gfx_text(s, ix, 12, u, muted, FONT_UI);
    }

    /* expression line */
    int ey = c->disp_h - 94;
    if (c->expr[0]) {
        int tw = font_text_width(FONT_UI, c->expr);
        int maxw = W - 2 * PAD - 8;
        if (tw <= maxw) gfx_text(s, W - PAD - 6 - tw, ey, c->expr, muted, FONT_UI);
        else {
            /* keep the end (most recent part) visible */
            const char *p = c->expr;
            while (*p && font_text_width(FONT_UI, p) > maxw - 14) p++;
            gfx_text(s, PAD + 4, ey, "\x9E", muted, FONT_UI);
            gfx_text(s, W - PAD - 6 - font_text_width(FONT_UI, p), ey, p, muted, FONT_UI);
        }
    }

    /* main number (or error message) */
    rect_t nr = rect_make(PAD, c->disp_h - 74, W - 2 * PAD - 6, 66);
    if (c->err) {
        const font_t *f = font_text_width(FONT_UI_TITLE, c->err) <= nr.w ? FONT_UI_TITLE : FONT_UI_LARGE;
        gfx_text_aligned(s, nr, c->err, t->window_text, f, ALIGN_RIGHT | ALIGN_MIDDLE);
    } else {
        char txt[96];
        display_text(c, txt, sizeof txt);
        const font_t *f = FONT_UI_HUGE;
        if (font_text_width(f, txt) > nr.w) f = FONT_UI_TITLE;
        if (font_text_width(f, txt) > nr.w) f = FONT_UI_LARGE;
        gfx_text_aligned(s, nr, txt, t->window_text, f, ALIGN_RIGHT | ALIGN_MIDDLE);
    }

    for (int i = 0; i < c->nbtn; i++) draw_key(c, s, i);
}

/* ---- events ----------------------------------------------------------------------- */
static void flash_key(calc_t *c, int key) {
    int i = find_key(c, key);
    if (i < 0) return;
    if (c->flash >= 0 && c->flash != i) invalidate_btn(c, c->flash);
    c->flash = i;
    c->flash_until = sys_uptime_ms() + 110;
    if (!c->timer) c->timer = win_set_timer(c->win, 30);
}

static void key_action(calc_t *c, int key) {
    flash_key(c, key);
    press_key(c, key);
    win_invalidate(c->win);
}

static void copy_display(calc_t *c) {
    char plain[64];
    if (c->err) return;
    if (c->entering) strlcpy(plain, c->entry, sizeof plain);
    else fmt_plain(c->cur, plain, sizeof plain);
    clipboard_set(plain);
}

static void paste_number(calc_t *c) {
    const char *clip = clipboard_get();
    char buf[64];
    int n = 0;
    for (const char *p = clip; *p && n < 63; p++) {
        if (*p == ',' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') continue;
        buf[n++] = *p;
    }
    buf[n] = 0;
    char *end;
    double v = strtod(buf, &end);
    if (end == buf || *end) { sys_beep(220, 60); return; }
    if (c->err) clear_all(c);
    set_operand(c, v, NULL);
    win_invalidate(c->win);
}

static int char_to_key(calc_t *c, int ch) {
    if (ch >= '0' && ch <= '9') return K_0 + (ch - '0');
    switch (ch) {
    case '.': case ',': return K_DOT;
    case '+': return K_ADD;
    case '-': return K_SUB;
    case '*': case 'x': case 'X': return K_MUL;
    case '/': return K_DIV;
    case '%': return K_PCT;
    case '=': return K_EQ;
    case '@': return K_SQRT;
    case 'r': case 'R': return K_RECIP;
    case 'q': case 'Q': return K_SQR;
    }
    if (!c->sci) return -1;
    switch (ch) {
    case '^': case 'y': return K_POW;
    case 'p': case 'P': return K_PI;
    case 'e': return K_E;
    case 's': return K_SIN;
    case 'o': return K_COS;
    case 't': return K_TAN;
    case 'n': return K_LN;
    case 'l': return K_LOG;
    case '!': return K_FACT;
    case '|': return K_ABS;
    case 'd': return K_DEG;
    }
    return -1;
}

static void calc_event(window_t *win, const event_t *ev) {
    calc_t *c = win_get_userdata(win);
    if (!c) return;
    switch (ev->type) {
    case EV_PAINT:
        paint(c, win_surface(win));
        break;
    case EV_RESIZE:
        layout(c);
        break;
    case EV_MOUSE_MOVE: {
        int h = hit_test(c, ev->x, ev->y);
        if (h != c->hover) {
            invalidate_btn(c, c->hover);
            invalidate_btn(c, h);
            c->hover = h;
        }
        bool mh = rect_contains(c->mode_r, ev->x, ev->y);
        if (mh != c->mode_hover) {
            c->mode_hover = mh;
            win_invalidate_rect(win, c->mode_r.x, c->mode_r.y, c->mode_r.w, c->mode_r.h);
        }
        break;
    }
    case EV_MOUSE_LEAVE:
        invalidate_btn(c, c->hover);
        c->hover = -1;
        if (c->mode_hover) { c->mode_hover = false; win_invalidate(win); }
        break;
    case EV_MOUSE_DOWN:
        if (ev->button != MOUSE_LEFT) break;
        if (rect_contains(c->mode_r, ev->x, ev->y)) { set_mode(c, !c->sci); break; }
        c->pressed = hit_test(c, ev->x, ev->y);
        c->hover = c->pressed;
        invalidate_btn(c, c->pressed);
        break;
    case EV_MOUSE_UP:
        if (ev->button != MOUSE_LEFT) break;
        if (c->pressed >= 0) {
            int p = c->pressed;
            c->pressed = -1;
            invalidate_btn(c, p);
            if (hit_test(c, ev->x, ev->y) == p) {
                press_key(c, c->btn[p].def->key);
                win_invalidate(win);
            }
        }
        break;
    case EV_TIMER:
        if (c->flash >= 0 && (int32_t)(sys_uptime_ms() - c->flash_until) >= 0) {
            invalidate_btn(c, c->flash);
            c->flash = -1;
        }
        if (c->flash < 0 && c->timer) { win_kill_timer(win, c->timer); c->timer = 0; }
        break;
    case EV_CHAR: {
        int k = char_to_key(c, ev->ch);
        if (k >= 0) key_action(c, k);
        break;
    }
    case EV_KEY_DOWN: {
        int k = ev->key;
        if (ev->mods & MOD_CTRL) {
            if (k == 'c') copy_display(c);
            else if (k == 'v') paste_number(c);
            else if (k == 'l') key_action(c, K_MC);
            else if (k == 'r') key_action(c, K_MR);
            else if (k == 'p') key_action(c, K_MADD);
            else if (k == 'q') key_action(c, K_MSUB);
            break;
        }
        if (ev->mods & MOD_ALT) {
            if (k == '1') set_mode(c, false);
            else if (k == '2') set_mode(c, true);
            break;
        }
        if (k == KEY_ENTER) key_action(c, K_EQ);
        else if (k == KEY_BACKSPACE) key_action(c, K_BS);
        else if (k == KEY_ESCAPE) key_action(c, K_C);
        else if (k == KEY_DELETE) key_action(c, K_CE);
        else if (k == KEY_F9) key_action(c, K_NEG);
        break;
    }
    case EV_CLOSE:
        win_set_userdata(win, NULL);
        free(c);
        break;
    default:
        break;
    }
}

static void calc_launch(const char *arg) {
    (void)arg;
    calc_t *c = calloc(1, sizeof *c);
    if (!c) return;
    window_t *win = win_create("Calculator", STD_W, DEF_H, WIN_RESIZABLE);
    if (!win) { free(c); return; }
    c->win = win;
    c->deg = true;
    c->hover = c->pressed = c->flash = -1;
    win_set_userdata(win, c);
    win_set_event_handler(win, calc_event);
    win_set_min_size(win, 260, 420);
    clear_all(c);
    layout(c);
}

/* Icon: a dark calculator body with a green display, a key grid and an orange "=" key. */
static void calc_icon(surface_t *s, int x, int y, int size) {
    double u = size / 48.0;
    int bx = x + (int)(8 * u), by = y + (int)(3 * u), bw = (int)(32 * u), bh = (int)(42 * u);
    gfx_fill_rounded_gradient_v(s, bx, by, bw, bh, (int)(6 * u), RGB(78, 86, 104), RGB(42, 47, 60));
    gfx_fill_rounded_rect(s, bx + (int)(4 * u), by + (int)(4 * u), bw - (int)(8 * u), (int)(10 * u), (int)(2 * u),
                          RGB(170, 226, 196));
    /* digits on the display */
    gfx_fill_rect(s, bx + bw - (int)(14 * u), by + (int)(7 * u), (int)(2 * u), (int)(4 * u), RGB(40, 90, 70));
    gfx_fill_rect(s, bx + bw - (int)(10 * u), by + (int)(7 * u), (int)(2 * u), (int)(4 * u), RGB(40, 90, 70));
    int kw = (int)(5 * u), kh = (int)(5 * u), gx = (int)(2.3 * u), gy = (int)(2.2 * u);
    int kx0 = bx + (int)(4 * u), ky0 = by + (int)(18 * u);
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++) {
            uint32_t kc = col == 3 ? RGB(255, 152, 50) : RGB(214, 220, 232);
            if (col == 3 && r == 3) kc = RGB(255, 196, 70);
            gfx_fill_rounded_rect(s, kx0 + col * (kw + gx), ky0 + r * (kh + gy), kw, kh, (int)(1.2 * u) + 1, kc);
        }
}

REGISTER_APP(.name = "Calculator", .category = CAT_ACCESSORIES,
             .description = "Standard and scientific calculator with memory",
             .launch = calc_launch, .draw_icon = calc_icon);
