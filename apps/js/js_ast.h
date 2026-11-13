/*
 * js_ast.h - NovaJS syntax tree (js_parse.c), scopes (js_comp.c) and the
 * bytecode instruction set (js_comp.c emits it, js_vm.c runs it).
 */
#ifndef NOVA_JS_AST_H
#define NOVA_JS_AST_H

#include "jsi.h"

/* ==== tokens ============================================================================== */
enum {
    TK_EOF = 0, TK_IDENT = 256, TK_NUM, TK_BIGINT, TK_STR, TK_TEMPLATE, TK_REGEXP, TK_PRIVATE,
    TK_EQ, TK_NE, TK_SEQ, TK_SNE, TK_LE, TK_GE, TK_SHL, TK_SAR, TK_SHR, TK_AND, TK_OR, TK_NULLISH,
    TK_INC, TK_DEC, TK_POW, TK_ARROW, TK_ELLIPSIS, TK_OPTCHAIN,
    TK_ADD_A, TK_SUB_A, TK_MUL_A, TK_DIV_A, TK_MOD_A, TK_POW_A, TK_SHL_A, TK_SAR_A, TK_SHR_A,
    TK_AND_A, TK_OR_A, TK_XOR_A, TK_LAND_A, TK_LOR_A, TK_NULLISH_A
};

/* ==== syntax tree ============================================================================= */
enum {
    N_NUM = 1, N_STR, N_TEMPLATE, N_TQUASI, N_TAGGED, N_REGEXP, N_IDENT, N_THIS, N_SUPER_MEMBER, N_SUPER_CALL,
    N_NEW_TARGET, N_IMPORT_META, N_ARRAY, N_OBJECT, N_PROP, N_FUNC, N_CLASS, N_CLASS_ELEM, N_UNARY, N_UPDATE,
    N_BINARY, N_LOGICAL, N_ASSIGN, N_COND, N_CALL, N_NEW, N_MEMBER, N_INDEX, N_PRIVMEMBER, N_OPTCHAIN, N_SEQ,
    N_SPREAD, N_YIELD, N_AWAIT, N_PRIVIN, N_IMPORT_CALL, N_HOLE, N_BIGINT,
    N_ARRAY_PAT, N_OBJECT_PAT, N_ASSIGN_PAT, N_REST,
    N_VAR, N_DECL, N_FUNC_DECL, N_CLASS_DECL, N_EXPR, N_BLOCK, N_EMPTY, N_IF, N_FOR, N_FORIN, N_FOROF,
    N_WHILE, N_DOWHILE, N_CONTINUE, N_BREAK, N_RETURN, N_WITH, N_SWITCH, N_CASE, N_THROW, N_TRY,
    N_LABELED, N_DEBUGGER, N_IMPORT, N_EXPORT, N_FIELD_DEF, N_STATIC_BLOCK
};

/* node flags */
#define NF_OPTIONAL  0x0001   /* ?. member / call                          */
#define NF_COMPUTED  0x0002   /* [key]                                     */
#define NF_PREFIX    0x0004   /* ++x                                       */
#define NF_DELEGATE  0x0008   /* yield*                                    */
#define NF_STATIC    0x0010   /* class element                             */
#define NF_PRIVATE   0x0020   /* #name class element                       */
#define NF_AWAIT     0x0040   /* for await                                 */
#define NF_PAREN     0x0080   /* parenthesized expression                  */
#define NF_COVERINIT 0x0100   /* object literal {a = 1} (pattern only)     */
#define NF_TRAILCOMMA 0x0200  /* (a, b,) cover list                        */
#define NF_SHORTHAND 0x0400
#define NF_ASYNCARROW 0x0800
#define NF_ANNEXB    0x1000   /* block function also sets a var             */
#define NF_DIRECTEVAL 0x2000

enum { PK_INIT = 0, PK_GET, PK_SET, PK_METHOD, PK_SPREAD, PK_PROTO, PK_REST, PK_FIELD, PK_STATIC_BLOCK };
enum { VK_VAR = 0, VK_LET, VK_CONST };

typedef struct Func Func;
typedef struct Scope Scope;
typedef struct Binding Binding;
typedef struct Node Node;

struct Node {
    uint16_t k, op;
    uint32_t flags;
    int line;
    uint32_t pos;
    Node *a, *b, *c, *d, *next;
    JSValue v, v2;
    double num;
    Func *fn;
    void *res;          /* resolver: Binding* (identifiers) or Scope* (scoped statements) */
    int ival;
};

/* function kinds */
enum { FK_NORMAL = 0, FK_ARROW, FK_METHOD, FK_GETTER, FK_SETTER, FK_CTOR, FK_DERIVED, FK_FIELDS, FK_SCRIPT, FK_STATIC_INIT, FK_MODULE };

typedef struct CVar { Binding *b; uint16_t src; } CVar;   /* src: 0x8000 | parent local slot, or parent cvar index */

struct Func {
    Func *parent;
    Node *params, *body;
    JSValue name;
    uint32_t flags;        /* F_* */
    int kind;
    uint32_t start, end;
    int line;
    int length;            /* .length */
    bool simple_params, expr_body, use_strict, is_decl, is_expr_named, has_eval;
    Scope *scope;          /* parameters + var scope */
    Scope *lex;            /* the body's top-level lexical scope (== scope here) */
    Binding *self;         /* named function expression's own name */
    Binding *this_b, *nt_b, *func_b, *args_b;   /* hidden bindings (captured by arrows) */
    bool uses_this, uses_nt, uses_func, uses_args;
    int nlocals;
    CVar *cv; int ncv, cvcap;
    Node *cls;             /* class constructors / field inits: the class node */
    Func *fields;          /* class constructor: instance field initialiser */
    int rest_slot;
    JSBytecode *bc;        /* after compilation */
};

/* binding kinds */
enum { BK_VAR = 0, BK_LET, BK_CONST, BK_CLASS, BK_FUNC, BK_PARAM, BK_CATCH, BK_HIDDEN, BK_PRIVATE, BK_SELF, BK_IMPORT };

struct Binding {
    JSValue name;
    uint8_t kind;
    bool captured, tdz, global;    /* global: script top-level (accessed by name) */
    int slot;
    Func *fn;
    Scope *scope;
    Binding *next;
};

enum { SK_FUNC = 0, SK_BLOCK, SK_CATCH, SK_CLASS, SK_SCRIPT, SK_WITH };
struct Scope {
    Scope *parent;
    Func *fn;
    Binding *b;
    uint8_t kind;
    bool has_captured;
    Node *fdecls;          /* function declarations instantiated at scope entry (list via res chain) */
};

typedef struct Parser Parser;
/* parse a whole script into a Func (the script function); NULL + SyntaxError thrown */
Func *js_parse_program(JSContext *ctx, Parser **pp, const uint16_t *src, uint32_t len, int line, uint32_t fflags);
void  js_parser_free(Parser *p);
/* AST node allocation (arena) */
void *js_parser_alloc(Parser *p, size_t n);

/* ==== bytecode ====================================================================================== */
/* operand formats */
enum { OF_NONE = 0, OF_U8, OF_I8, OF_U16, OF_U32, OF_I32, OF_U16U32, OF_U32U8, OF_U16I32 };
#define JS_OPCODES(X) \
    X(NOP, NONE) X(PUSH_UNDEF, NONE) X(PUSH_NULL, NONE) X(PUSH_TRUE, NONE) X(PUSH_FALSE, NONE) \
    X(PUSH_I8, I8) X(PUSH_I32, I32) X(PUSH_CONST, U32) X(PUSH_THIS, NONE) X(PUSH_NEW_TARGET, NONE) \
    X(PUSH_FUNC, NONE) X(PUSH_UNINIT, NONE) X(PUSH_EMPTY, NONE) X(PUSH_HOME, NONE) \
    X(OBJECT_NEW, NONE) X(ARRAY_NEW, U16) X(ARRAY_PUSH, NONE) X(ARRAY_HOLE, NONE) X(ARRAY_SPREAD, NONE) \
    X(CLOSURE, U32) X(REGEXP, U32) X(TEMPLATE, U32) \
    X(POP, NONE) X(DUP, NONE) X(DUP2, NONE) X(SWAP, NONE) X(ROT3L, NONE) X(ROT3R, NONE) X(OVER, NONE) \
    X(ROT4L, NONE) X(SWAP2, NONE) X(NIP, NONE) \
    X(GET_LOC, U16) X(PUT_LOC, U16) X(SET_LOC, U16) X(GET_LOC_CHK, U16) X(PUT_LOC_CHK, U16) \
    X(GET_BOX, U16) X(PUT_BOX, U16) X(SET_BOX, U16) X(GET_BOX_CHK, U16) X(PUT_BOX_CHK, U16) \
    X(NEW_BOX, U16) X(NEW_BOX_UNDEF, U16) X(COPY_BOX, U16) X(INIT_BOX, U16) \
    X(GET_VREF, U16) X(PUT_VREF, U16) X(SET_VREF, U16) X(GET_VREF_CHK, U16) X(PUT_VREF_CHK, U16) \
    X(GET_GLOBAL, U32) X(GET_GLOBAL_TYPEOF, U32) X(PUT_GLOBAL, U32) X(SET_GLOBAL, U32) \
    X(DEFINE_GLOBAL_VAR, U32) X(DEFINE_GLOBAL_FUNC, U32) X(DEFINE_GLOBAL_LEX, U32U8) X(INIT_GLOBAL_LEX, U32) \
    X(DELETE_GLOBAL, U32) X(CHECK_GLOBAL_DECL, U32U8) \
    X(GET_FIELD, U32) X(GET_FIELD_KEEP, U32) X(PUT_FIELD, U32) X(GET_ELEM, NONE) X(GET_ELEM_KEEP, NONE) \
    X(PUT_ELEM, NONE) X(DELETE_FIELD, U32) X(DELETE_ELEM, NONE) X(GET_SUPER, NONE) X(PUT_SUPER, NONE) \
    X(GET_PRIVATE, NONE) X(PUT_PRIVATE, NONE) X(DEFINE_PRIVATE, NONE) X(PRIVATE_IN, NONE) X(NEW_PRIVATE, U32) \
    X(DEFINE_FIELD, U32) X(DEFINE_ELEM, NONE) X(DEFINE_METHOD, U32U8) X(DEFINE_METHOD_ELEM, U8) X(SET_PROTO, NONE) \
    X(COPY_DATA_PROPS, NONE) X(COPY_DATA_EXCL, NONE) X(SET_NAME, U32) X(SET_NAME_ELEM, NONE) X(SET_HOME, NONE) \
    X(CALL, U16U32) X(CALL_SPREAD, U32) X(NEW, U16) X(NEW_SPREAD, NONE) X(SUPER_CALL, U16) X(SUPER_CALL_SPREAD, U8) \
    X(EVAL, U16) X(RETURN, NONE) X(RETURN_UNDEF, NONE) \
    X(JMP, I32) X(JMP_FALSE, I32) X(JMP_TRUE, I32) X(JMP_FALSE_KEEP, I32) X(JMP_TRUE_KEEP, I32) \
    X(JMP_NOTNULLISH_KEEP, I32) X(JMP_NULLISH_UNDEF, I32) X(JMP_NULLISH_UNDEF2, I32) X(JMP_UNDEF_KEEP, I32) \
    X(THROW, NONE) X(TRY, I32) X(END_TRY, NONE) X(GOSUB, I32) X(RET, NONE) X(THROW_REF, U32) X(THROW_CONST, U32) \
    X(THROW_TYPE, U32) X(THROW_SYNTAX, U32) \
    X(ADD, NONE) X(SUB, NONE) X(MUL, NONE) X(DIV, NONE) X(MOD, NONE) X(POW, NONE) X(NEG, NONE) X(PLUS, NONE) \
    X(NOT, NONE) X(BNOT, NONE) X(BAND, NONE) X(BOR, NONE) X(BXOR, NONE) X(SHL, NONE) X(SAR, NONE) X(SHR, NONE) \
    X(LT, NONE) X(LE, NONE) X(GT, NONE) X(GE, NONE) X(EQ, NONE) X(NE, NONE) X(SEQ, NONE) X(SNE, NONE) \
    X(TYPEOF, NONE) X(IN, NONE) X(INSTANCEOF, NONE) X(INC, NONE) X(DEC, NONE) X(TO_NUMERIC, NONE) \
    X(TO_STRING, NONE) X(TO_KEY, NONE) X(IS_UNDEF, NONE) X(IS_NULLISH, NONE) \
    X(FOR_IN_START, NONE) X(FOR_IN_NEXT, I32) X(FOR_OF_START, NONE) X(FOR_OF_NEXT, I32) X(ITER_CLOSE, NONE) X(ITER_CLOSE_EXC, NONE) \
    X(FOR_AWAIT_START, NONE) X(ITER_NEXT_RAW, NONE) X(ITER_RESULT, I32) \
    X(ITER_STEP, NONE) X(ITER_REST, NONE) X(ITER_END, NONE) X(REQUIRE_COERCIBLE, NONE) \
    X(INITIAL_YIELD, NONE) X(YIELD, I32) X(YIELD_STAR, I32) X(AWAIT, NONE) X(ASYNC_YIELD_STAR, I32) \
    X(CLASS, U8) X(SET_FIELDS, NONE) X(RUN_FIELDS, NONE) X(CHECK_THIS_INIT, NONE) X(CHECK_DERIVED_RETURN, NONE) \
    X(ARGUMENTS, NONE) X(REST, U16) X(DEBUGGER, NONE) X(IMPORT_CALL, NONE) X(GET_ITER_METHOD, NONE) \
    X(SPREAD_CALL_ARGS, NONE) X(CHECK_OBJ, NONE) X(LOOP_HINT, NONE)
#define JS_OP_ENUM(n, f) OP_##n,
enum { JS_OPCODES(JS_OP_ENUM) OP_COUNT };
#undef JS_OP_ENUM

/* catch markers / return addresses on the operand stack */
#define MARK_CATCH 0x80000000u

#endif
