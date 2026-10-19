/*
 * terminal_calc.c - the expression evaluator behind Terminal's `calc`.
 *
 * Recursive descent over doubles (x87 FPU):
 *
 *   expr    := term (('+' | '-') term)*
 *   term    := unary (('*' | '/' | '%') unary | implicit-multiplication)*
 *   unary   := ('-' | '+') unary | power
 *   power   := postfix ('^' unary)?            right associative: 2^3^2 = 512
 *   postfix := primary '!'*                    factorial
 *   primary := number | '(' expr ')' | name | name '(' args ')'
 *
 * Numbers: 12, 3.5, .5, 1e-3, 0x1F, 0b101. Names: pi, e, tau, phi, ans and
 * functions (sqrt, sin, cos, ..., min, max, pow, atan2, hypot). "2pi" and
 * "3(1+2)" multiply implicitly; -2^2 = -4 as in mathematics.
 */
#include "terminal_term.h"

typedef struct {
    const char *s;
    int pos;
    double ans;
    char *err;
    int errsz;
    bool failed;
    int depth;
} calc_t;

static double expr(calc_t *c);

static void fail(calc_t *c, const char *fmt, ...) {
    if (c->failed) return;
    c->failed = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, (size_t)c->errsz, fmt, ap);
    va_end(ap);
}

static void skip_ws(calc_t *c) {
    while (c->s[c->pos] == ' ' || c->s[c->pos] == '\t') c->pos++;
}

static char peek(calc_t *c) {
    skip_ws(c);
    return c->s[c->pos];
}

static bool accept(calc_t *c, char ch) {
    if (peek(c) == ch) { c->pos++; return true; }
    return false;
}

static double number(calc_t *c) {
    const char *p = c->s + c->pos;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        char *end;
        unsigned long v = strtoul(p + 2, &end, 16);
        if (end == p + 2) { fail(c, "bad hex number"); return 0; }
        c->pos += (int)(end - p);
        return (double)v;
    }
    if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B') && (p[2] == '0' || p[2] == '1')) {
        double v = 0;
        int k = 2;
        while (p[k] == '0' || p[k] == '1') v = v * 2 + (p[k++] - '0');
        c->pos += k;
        return v;
    }
    char *end;
    double v = strtod(p, &end);
    if (end == p) { fail(c, "expected a number at '%s'", p); return 0; }
    c->pos += (int)(end - p);
    return v;
}

static double factorial(calc_t *c, double v) {
    if (v < 0 || v != floor(v)) { fail(c, "factorial needs a whole number >= 0"); return 0; }
    if (v > 170) { fail(c, "factorial too large"); return 0; }
    double r = 1;
    for (int i = 2; i <= (int)v; i++) r *= i;
    return r;
}

typedef struct { const char *name; int nargs; } fdesc_t;
static const fdesc_t s_funcs[] = {
    { "sqrt", 1 }, { "cbrt", 1 }, { "sin", 1 }, { "cos", 1 }, { "tan", 1 },
    { "asin", 1 }, { "acos", 1 }, { "atan", 1 }, { "sinh", 1 }, { "cosh", 1 }, { "tanh", 1 },
    { "exp", 1 }, { "ln", 1 }, { "log", 1 }, { "log2", 1 }, { "log10", 1 },
    { "abs", 1 }, { "floor", 1 }, { "ceil", 1 }, { "round", 1 }, { "trunc", 1 },
    { "deg", 1 }, { "rad", 1 }, { "min", 2 }, { "max", 2 }, { "pow", 2 },
    { "atan2", 2 }, { "hypot", 2 }, { "mod", 2 },
};

static double call(calc_t *c, const char *f, const double *a) {
    if (!strcmp(f, "sqrt")) { if (a[0] < 0) fail(c, "sqrt of a negative number"); return sqrt(a[0]); }
    if (!strcmp(f, "cbrt")) return cbrt(a[0]);
    if (!strcmp(f, "sin")) return sin(a[0]);
    if (!strcmp(f, "cos")) return cos(a[0]);
    if (!strcmp(f, "tan")) return tan(a[0]);
    if (!strcmp(f, "asin")) { if (fabs(a[0]) > 1) fail(c, "asin: argument outside -1..1"); return asin(a[0]); }
    if (!strcmp(f, "acos")) { if (fabs(a[0]) > 1) fail(c, "acos: argument outside -1..1"); return acos(a[0]); }
    if (!strcmp(f, "atan")) return atan(a[0]);
    if (!strcmp(f, "sinh")) return sinh(a[0]);
    if (!strcmp(f, "cosh")) return cosh(a[0]);
    if (!strcmp(f, "tanh")) return tanh(a[0]);
    if (!strcmp(f, "exp")) return exp(a[0]);
    if (!strcmp(f, "ln") || !strcmp(f, "log") || !strcmp(f, "log2") || !strcmp(f, "log10")) {
        if (a[0] <= 0) { fail(c, "%s of a number <= 0", f); return 0; }
        if (!strcmp(f, "ln")) return log(a[0]);
        if (!strcmp(f, "log2")) return log2(a[0]);
        return log10(a[0]);
    }
    if (!strcmp(f, "abs")) return fabs(a[0]);
    if (!strcmp(f, "floor")) return floor(a[0]);
    if (!strcmp(f, "ceil")) return ceil(a[0]);
    if (!strcmp(f, "round")) return round(a[0]);
    if (!strcmp(f, "trunc")) return trunc(a[0]);
    if (!strcmp(f, "deg")) return a[0] * 180.0 / M_PI;
    if (!strcmp(f, "rad")) return a[0] * M_PI / 180.0;
    if (!strcmp(f, "min")) return fmin(a[0], a[1]);
    if (!strcmp(f, "max")) return fmax(a[0], a[1]);
    if (!strcmp(f, "pow")) return pow(a[0], a[1]);
    if (!strcmp(f, "atan2")) return atan2(a[0], a[1]);
    if (!strcmp(f, "hypot")) return hypot(a[0], a[1]);
    if (!strcmp(f, "mod")) { if (a[1] == 0) { fail(c, "modulo by zero"); return 0; } return fmod(a[0], a[1]); }
    fail(c, "unknown function '%s'", f);
    return 0;
}

static double primary(calc_t *c) {
    if (++c->depth > 200) { fail(c, "expression too deeply nested"); return 0; }
    char ch = peek(c);
    double v = 0;
    if (ch == '(') {
        c->pos++;
        v = expr(c);
        if (!accept(c, ')')) fail(c, "missing ')'");
    } else if (isdigit((unsigned char)ch) || ch == '.') {
        v = number(c);
    } else if (isalpha((unsigned char)ch) || ch == '_') {
        char name[16];
        int n = 0;
        while ((isalnum((unsigned char)c->s[c->pos]) || c->s[c->pos] == '_') && n < 15) name[n++] = c->s[c->pos++];
        name[n] = 0;
        for (int i = 0; name[i]; i++) name[i] = (char)tolower((unsigned char)name[i]);
        const fdesc_t *fd = NULL;
        for (int i = 0; i < ARRAY_SIZE(s_funcs); i++) if (!strcmp(s_funcs[i].name, name)) fd = &s_funcs[i];
        if (fd) {
            double a[2] = { 0, 0 };
            if (!accept(c, '(')) {
                /* allow "sqrt 16" / "sin pi" for one-argument functions */
                if (fd->nargs == 1) { a[0] = primary(c); }
                else { fail(c, "%s needs parentheses: %s(a, b)", name, name); }
            } else {
                for (int k = 0; k < fd->nargs; k++) {
                    if (k > 0 && !accept(c, ',')) { fail(c, "%s needs %d arguments", name, fd->nargs); break; }
                    a[k] = expr(c);
                }
                if (!accept(c, ')')) fail(c, "missing ')' after %s(...)", name);
            }
            v = c->failed ? 0 : call(c, name, a);
        } else if (!strcmp(name, "pi")) v = M_PI;
        else if (!strcmp(name, "e")) v = M_E;
        else if (!strcmp(name, "tau")) v = 2 * M_PI;
        else if (!strcmp(name, "phi")) v = (1 + sqrt(5.0)) / 2;
        else if (!strcmp(name, "ans")) v = c->ans;
        else fail(c, "unknown name '%s'", name);
    } else if (!ch) {
        fail(c, "unexpected end of expression");
    } else {
        fail(c, "unexpected '%c'", ch);
    }
    c->depth--;
    return v;
}

static double postfix(calc_t *c) {
    double v = primary(c);
    while (!c->failed && peek(c) == '!' && c->s[c->pos + 1] != '=') {
        c->pos++;
        v = factorial(c, v);
    }
    return v;
}

static double unary(calc_t *c);

static double power(calc_t *c) {
    double b = postfix(c);
    if (!c->failed && (accept(c, '^') || (peek(c) == '*' && c->s[c->pos + 1] == '*' && (c->pos += 2)))) {
        double e = unary(c);               /* right associative, allows 2^-1 */
        if (b == 0 && e < 0) { fail(c, "division by zero"); return 0; }
        if (b < 0 && e != floor(e)) { fail(c, "fractional power of a negative number"); return 0; }
        return pow(b, e);
    }
    return b;
}

static double unary(calc_t *c) {
    if (accept(c, '-')) return -unary(c);
    if (accept(c, '+')) return unary(c);
    return power(c);
}

static double term(calc_t *c) {
    double v = unary(c);
    while (!c->failed) {
        char ch = peek(c);
        if (ch == '*' && c->s[c->pos + 1] != '*') {
            c->pos++;
            v *= unary(c);
        } else if (ch == '/') {
            c->pos++;
            double d = unary(c);
            if (d == 0 && !c->failed) { fail(c, "division by zero"); return 0; }
            v /= d;
        } else if (ch == '%') {
            c->pos++;
            double d = unary(c);
            if (d == 0 && !c->failed) { fail(c, "modulo by zero"); return 0; }
            v = fmod(v, d);
        } else if (ch == '(' || isalpha((unsigned char)ch) || ch == '_' ||
                   ((isdigit((unsigned char)ch) || ch == '.') && c->pos > 0 && c->s[c->pos - 1] == ')')) {
            v *= unary(c);                 /* implicit multiplication: 2pi, 3(4+5), (1+2)(3+4) */
        } else {
            break;
        }
    }
    return v;
}

static double expr(calc_t *c) {
    double v = term(c);
    while (!c->failed) {
        if (accept(c, '+')) v += term(c);
        else if (accept(c, '-')) v -= term(c);
        else break;
    }
    return v;
}

int term_calc_eval(const char *s, double ans, double *result, char *err, int errsz) {
    calc_t c = { s, 0, ans, err, errsz, false, 0 };
    err[0] = 0;
    if (!*s) { snprintf(err, (size_t)errsz, "empty expression"); return -1; }
    double v = expr(&c);
    if (!c.failed && peek(&c)) fail(&c, peek(&c) == ')' ? "unbalanced ')'" : "unexpected '%c'", peek(&c));
    if (!c.failed && isnan(v)) fail(&c, "result is undefined");
    if (!c.failed && isinf(v)) fail(&c, "result is too large");
    if (c.failed) return -1;
    *result = v;
    return 0;
}

/* Integers print without decimals, others with up to 12 significant digits. */
void term_calc_format(double v, char *out, int size) {
    if (v == 0) v = 0;                                     /* no "-0" */
    if (fabs(v) < 1e15 && v == floor(v)) {
        snprintf(out, (size_t)size, "%.0f", v);
        return;
    }
    snprintf(out, (size_t)size, "%.12g", v);
}

int term_cmd_calc(tctx_t *c, int argc, char **argv) {
    if (argc < 2) {
        if (!tc_tty(c)) { tc_err(c, "usage: calc EXPRESSION"); return 2; }
        c->t->mode = TERM_MODE_CALC;
        tc_puts(c, "\x1b[1;96mcalc\x1b[0m interactive mode \x1b[90m-\x1b[0m type expressions, "
                   "\x1b[1mexit\x1b[0m or \x1b[1mCtrl+C\x1b[0m to leave \x1b[90m(ans = last result)\x1b[0m\n");
        return 0;
    }
    char buf[TERM_INPUT_MAX];
    buf[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1) strlcat(buf, " ", sizeof buf);
        strlcat(buf, argv[i], sizeof buf);
    }
    double r;
    char err[96];
    if (term_calc_eval(buf, c->t->calc_ans, &r, err, sizeof err)) {
        tc_err(c, "calc: %s", err);
        return 1;
    }
    c->t->calc_ans = r;
    char out[64];
    term_calc_format(r, out, sizeof out);
    if (tc_tty(c)) {
        tc_printf(c, "\x1b[90m%s =\x1b[0m \x1b[1;93m%s\x1b[0m", buf, out);
        if (r == floor(r) && fabs(r) < 4294967296.0 && r >= 0 && r >= 16)
            tc_printf(c, "  \x1b[90m(0x%X)\x1b[0m", (unsigned)r);
        tc_puts(c, "\n");
    } else {
        tc_printf(c, "%s\n", out);
    }
    return 0;
}
