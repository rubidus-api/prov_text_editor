#include "highlight.h"
#include "display.h"     /* PROV_CELL_FG */

#include <string.h>

/* ---- helpers ------------------------------------------------------------ */

static char lo(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static bool ends_with_ci(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    if (lf > ls) return false;
    for (size_t i = 0; i < lf; i++) if (lo(s[ls - lf + i]) != lo(suf[i])) return false;
    return true;
}

static const char *basename_of(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '/' || *q == '\\') b = q + 1;
    return b;
}

/* biased cell fg for a token class, via the theme */
static proven_u8 cls_fg(const prov_theme_t *th, prov_tok_class_t c) {
    if (!th || c >= PROV_TOK_COUNT) return 0;
    return PROV_CELL_FG(th->cls[c].fg);
}

/* fill out_fg[a..b) with the biased fg of class `c` (clamped to len) */
static void paint(proven_u8 *out, proven_size_t len, proven_size_t a, proven_size_t b,
                  proven_u8 fg) {
    if (b > len) b = len;
    for (proven_size_t i = a; i < b; i++) out[i] = fg;
}

static bool is_ident(proven_u8 c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
static bool is_digit(proven_u8 c) { return c >= '0' && c <= '9'; }

/* ---- detection ---------------------------------------------------------- */

static bool name_ci(const char *a, const char *b) {  /* exact basename, case-insensitive */
    for (; *a && *b; a++, b++) if (lo(*a) != lo(*b)) return false;
    return *a == *b;
}

prov_hl_lang_t prov_hl_detect(const char *path) {
    if (!path || !*path) return PROV_HL_NONE;
    const char *bn = basename_of(path);
    if (ends_with_ci(path, ".md") || ends_with_ci(path, ".markdown")) return PROV_HL_MARKDOWN;
    if (ends_with_ci(path, ".c")  || ends_with_ci(path, ".h")) return PROV_HL_C;
    if (ends_with_ci(path, ".py")) return PROV_HL_PYTHON;
    if (ends_with_ci(path, ".js") || ends_with_ci(path, ".jsx") ||
        ends_with_ci(path, ".mjs") || ends_with_ci(path, ".cjs")) return PROV_HL_JAVASCRIPT;
    if (ends_with_ci(path, ".ts") || ends_with_ci(path, ".tsx")) return PROV_HL_TYPESCRIPT;
    if (ends_with_ci(path, ".sh") || ends_with_ci(path, ".bash") ||
        ends_with_ci(path, ".zsh")) return PROV_HL_SHELL;
    if (ends_with_ci(path, ".json")) return PROV_HL_JSON;
    if (ends_with_ci(path, ".toml") || ends_with_ci(path, ".ini") ||
        ends_with_ci(path, ".cfg") || ends_with_ci(path, ".conf") ||
        ends_with_ci(path, ".env") || name_ci(bn, ".env")) return PROV_HL_TOML;
    if (ends_with_ci(path, ".yml") || ends_with_ci(path, ".yaml")) return PROV_HL_YAML;
    if (ends_with_ci(path, ".css")) return PROV_HL_CSS;
    if (ends_with_ci(path, ".rs")) return PROV_HL_RUST;
    if (ends_with_ci(path, ".go")) return PROV_HL_GO;
    if (ends_with_ci(path, ".java")) return PROV_HL_JAVA;
    if (ends_with_ci(path, ".kt") || ends_with_ci(path, ".kts")) return PROV_HL_KOTLIN;
    if (ends_with_ci(path, ".swift")) return PROV_HL_SWIFT;
    if (ends_with_ci(path, ".lua")) return PROV_HL_LUA;
    if (ends_with_ci(path, ".sql")) return PROV_HL_SQL;
    if (ends_with_ci(path, ".html") || ends_with_ci(path, ".htm") ||
        ends_with_ci(path, ".xml") || ends_with_ci(path, ".xhtml") ||
        ends_with_ci(path, ".svg")) return PROV_HL_MARKUP;
    return PROV_HL_NONE;
}

/* ===================================================================== */
/*  Markdown                                                              */
/* ===================================================================== */
/* state.kind: 0 = NORMAL, 1 = inside a fenced code block.
 * state.param: the fence marker length (>=3) when kind==1. */
enum { MD_NORMAL = 0, MD_FENCE = 1 };

static prov_hl_state_t hl_markdown(prov_hl_state_t st, const proven_u8 *s,
                                   proven_size_t n, const prov_theme_t *th,
                                   proven_u8 *out) {
    proven_u8 fg_head = cls_fg(th, PROV_TOK_FUNCTION);   /* headings: function color */
    proven_u8 fg_str  = cls_fg(th, PROV_TOK_STRING);     /* code spans / fences */
    proven_u8 fg_cmt  = cls_fg(th, PROV_TOK_COMMENT);    /* blockquotes */
    proven_u8 fg_kw   = cls_fg(th, PROV_TOK_KEYWORD);    /* emphasis markers */
    proven_u8 fg_num  = cls_fg(th, PROV_TOK_NUMBER);     /* list markers */
    proven_u8 fg_pre  = cls_fg(th, PROV_TOK_PREPROC);    /* link [..](..) */

    for (proven_size_t i = 0; i < n; i++) out[i] = 0;    /* default */

    /* leading whitespace count */
    proven_size_t ind = 0; while (ind < n && (s[ind] == ' ' || s[ind] == '\t')) ind++;

    /* fenced code block toggle: ``` or ~~~ (>=3) at line start (after indent) */
    if (ind + 3 <= n && (s[ind] == '`' || s[ind] == '~')) {
        proven_u8 fc = s[ind]; proven_size_t k = ind; while (k < n && s[k] == fc) k++;
        if (k - ind >= 3) {
            paint(out, n, 0, n, fg_str);
            if (st.kind == MD_FENCE) return PROV_HL_STATE0;          /* closing fence */
            return (prov_hl_state_t){ MD_FENCE, 0, (proven_u32)(k - ind) };
        }
    }
    if (st.kind == MD_FENCE) { paint(out, n, 0, n, fg_str); return st; }  /* inside code */

    /* ATX heading: leading # ... */
    if (ind < n && s[ind] == '#') {
        paint(out, n, 0, n, fg_head);
        return PROV_HL_STATE0;
    }
    /* blockquote: leading > */
    if (ind < n && s[ind] == '>') { paint(out, n, 0, n, fg_cmt); return PROV_HL_STATE0; }
    /* list marker: -, *, + then space, or "N." */
    if (ind + 1 < n && (s[ind] == '-' || s[ind] == '*' || s[ind] == '+') && s[ind+1] == ' ')
        paint(out, n, ind, ind + 1, fg_num);
    else if (ind < n && is_digit(s[ind])) {
        proven_size_t k = ind; while (k < n && is_digit(s[k])) k++;
        if (k < n && (s[k] == '.' || s[k] == ')')) paint(out, n, ind, k + 1, fg_num);
    }

    /* inline spans over the rest of the line */
    for (proven_size_t i = ind; i < n; i++) {
        /* `code span` */
        if (s[i] == '`') {
            proven_size_t j = i + 1; while (j < n && s[j] != '`') j++;
            paint(out, n, i, (j < n ? j + 1 : n), fg_str);
            i = (j < n ? j : n - 1);
            continue;
        }
        /* emphasis run *...* or _..._ or **...** (mark the delimiters) */
        if (s[i] == '*' || s[i] == '_') {
            out[i] = fg_kw; continue;
        }
        /* link / image: [text](url) — color the bracketed parts */
        if (s[i] == '[') {
            proven_size_t j = i + 1; while (j < n && s[j] != ']') j++;
            if (j < n && j + 1 < n && s[j+1] == '(') {
                proven_size_t k = j + 2; while (k < n && s[k] != ')') k++;
                paint(out, n, i, (k < n ? k + 1 : n), fg_pre);
                i = (k < n ? k : n - 1);
                continue;
            }
        }
    }
    return PROV_HL_STATE0;
}

/* ===================================================================== */
/*  C                                                                    */
/* ===================================================================== */
/* state.kind: 0 = NORMAL, 1 = inside a slash-star block comment. */
enum { C_NORMAL = 0, C_BLOCKCMT = 1 };

static const char *const C_KEYWORDS[] = {
    "auto","break","case","const","continue","default","do","else","enum",
    "extern","for","goto","if","inline","register","restrict","return","sizeof",
    "static","struct","switch","typedef","union","volatile","while","_Alignas",
    "_Alignof","_Atomic","_Bool","_Generic","_Noreturn","_Static_assert",
    "_Thread_local","alignas","alignof","static_assert","thread_local","typeof",
    "true","false","nullptr","constexpr",
};
static const char *const C_TYPES[] = {
    "void","char","short","int","long","float","double","signed","unsigned",
    "bool","size_t","ssize_t","ptrdiff_t","intptr_t","uintptr_t","wchar_t",
    "int8_t","int16_t","int32_t","int64_t","uint8_t","uint16_t","uint32_t","uint64_t",
};

static bool in_set(const char *const *set, size_t n, const proven_u8 *s, proven_size_t len) {
    for (size_t i = 0; i < n; i++) {
        const char *k = set[i]; size_t kl = strlen(k);
        if (kl == len && memcmp(k, s, len) == 0) return true;
    }
    return false;
}

static prov_hl_state_t hl_c(prov_hl_state_t st, const proven_u8 *s, proven_size_t n,
                            const prov_theme_t *th, proven_u8 *out) {
    proven_u8 fg_kw  = cls_fg(th, PROV_TOK_KEYWORD);
    proven_u8 fg_ty  = cls_fg(th, PROV_TOK_TYPE);
    proven_u8 fg_str = cls_fg(th, PROV_TOK_STRING);
    proven_u8 fg_cmt = cls_fg(th, PROV_TOK_COMMENT);
    proven_u8 fg_num = cls_fg(th, PROV_TOK_NUMBER);
    proven_u8 fg_fn  = cls_fg(th, PROV_TOK_FUNCTION);
    proven_u8 fg_pre = cls_fg(th, PROV_TOK_PREPROC);

    for (proven_size_t i = 0; i < n; i++) out[i] = 0;

    proven_size_t i = 0;

    /* continuing a block comment from a previous line */
    if (st.kind == C_BLOCKCMT) {
        while (i < n) {
            if (i + 1 < n && s[i] == '*' && s[i+1] == '/') { paint(out, n, 0, i + 2, fg_cmt); i += 2; goto normal; }
            i++;
        }
        paint(out, n, 0, n, fg_cmt);
        return st;   /* still open */
    }

    /* preprocessor: first non-space char is # → color the whole directive line
     * (kept simple; inline trailing comments on a directive are rare) */
    {
        proven_size_t k = 0; while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
        if (k < n && s[k] == '#') { paint(out, n, 0, n, fg_pre); return PROV_HL_STATE0; }
    }

normal:
    for (; i < n; i++) {
        proven_u8 c = s[i];
        /* line comment */
        if (c == '/' && i + 1 < n && s[i+1] == '/') { paint(out, n, i, n, fg_cmt); break; }
        /* block comment open */
        if (c == '/' && i + 1 < n && s[i+1] == '*') {
            proven_size_t j = i + 2;
            while (j + 1 < n && !(s[j] == '*' && s[j+1] == '/')) j++;
            if (j + 1 < n && s[j] == '*' && s[j+1] == '/') { paint(out, n, i, j + 2, fg_cmt); i = j + 1; continue; }
            paint(out, n, i, n, fg_cmt);
            return (prov_hl_state_t){ C_BLOCKCMT, 0, 0 };   /* runs to next line */
        }
        /* string / char literal (with \ escapes) */
        if (c == '"' || c == '\'') {
            proven_u8 q = c; proven_size_t j = i + 1;
            while (j < n) { if (s[j] == '\\') { j += 2; continue; } if (s[j] == q) { j++; break; } j++; }
            paint(out, n, i, j, fg_str);
            i = j - 1;
            continue;
        }
        /* number */
        if (is_digit(c) || (c == '.' && i + 1 < n && is_digit(s[i+1]))) {
            proven_size_t j = i;
            while (j < n && (is_ident(s[j]) || s[j] == '.' ||
                   ((s[j] == '+' || s[j] == '-') && j > i && (s[j-1]=='e'||s[j-1]=='E'||s[j-1]=='p'||s[j-1]=='P'))))
                j++;
            paint(out, n, i, j, fg_num);
            i = j - 1;
            continue;
        }
        /* identifier / keyword / type / function call */
        if (is_ident(c) && !is_digit(c)) {
            proven_size_t j = i; while (j < n && is_ident(s[j])) j++;
            proven_size_t len = j - i;
            if (in_set(C_KEYWORDS, sizeof C_KEYWORDS/sizeof C_KEYWORDS[0], s + i, len))
                paint(out, n, i, j, fg_kw);
            else if (in_set(C_TYPES, sizeof C_TYPES/sizeof C_TYPES[0], s + i, len) ||
                     (len > 2 && s[j-1] == 't' && s[j-2] == '_'))   /* foo_t convention */
                paint(out, n, i, j, fg_ty);
            else {
                proven_size_t k = j; while (k < n && (s[k]==' '||s[k]=='\t')) k++;
                if (k < n && s[k] == '(') paint(out, n, i, j, fg_fn);   /* call/def: name( */
            }
            i = j - 1;
            continue;
        }
    }
    return PROV_HL_STATE0;
}

/* ===================================================================== */
/*  Generic C-like lexer (js / ts / shell share this)                    */
/* ===================================================================== */
enum { CLK_NORMAL = 0, CLK_BLOCKCMT = 1 };   /* shared block-comment carry */
enum {                                       /* hl_clike option flags */
    CLK_BLOCK   = 1u << 0,   /* slash-star block comments */
    CLK_SLASHLC = 1u << 1,   /* // line comments */
    CLK_HASHLC  = 1u << 2,   /* # line comments */
    CLK_DOLLAR  = 1u << 3,   /* $var / ${var} (shell) */
    CLK_BACKTICK= 1u << 4,   /* `...` strings (js template, kept single-line) */
    CLK_DASHLC  = 1u << 5,   /* -- line comments (lua, sql) */
};

static prov_hl_state_t hl_clike(prov_hl_state_t st, const proven_u8 *s, proven_size_t n,
                                const prov_theme_t *th, proven_u8 *out,
                                const char *const *kw, size_t nkw,
                                const char *const *ty, size_t nty,
                                unsigned flags) {
    proven_u8 fg_kw = cls_fg(th, PROV_TOK_KEYWORD), fg_ty = cls_fg(th, PROV_TOK_TYPE);
    proven_u8 fg_str= cls_fg(th, PROV_TOK_STRING),  fg_cmt= cls_fg(th, PROV_TOK_COMMENT);
    proven_u8 fg_num= cls_fg(th, PROV_TOK_NUMBER),  fg_fn = cls_fg(th, PROV_TOK_FUNCTION);
    proven_u8 fg_var= cls_fg(th, PROV_TOK_CONSTANT);

    for (proven_size_t i = 0; i < n; i++) out[i] = 0;
    proven_size_t i = 0;

    if (st.kind == CLK_BLOCKCMT) {
        while (i < n) { if (i+1 < n && s[i]=='*' && s[i+1]=='/') { paint(out,n,0,i+2,fg_cmt); i+=2; goto body; } i++; }
        paint(out, n, 0, n, fg_cmt); return st;
    }
body:
    for (; i < n; i++) {
        proven_u8 c = s[i];
        if ((flags & CLK_SLASHLC) && c=='/' && i+1<n && s[i+1]=='/') { paint(out,n,i,n,fg_cmt); break; }
        if ((flags & CLK_DASHLC)  && c=='-' && i+1<n && s[i+1]=='-') { paint(out,n,i,n,fg_cmt); break; }
        if ((flags & CLK_HASHLC)  && c=='#') { paint(out,n,i,n,fg_cmt); break; }
        if ((flags & CLK_BLOCK)   && c=='/' && i+1<n && s[i+1]=='*') {
            proven_size_t j=i+2; while (j+1<n && !(s[j]=='*'&&s[j+1]=='/')) j++;
            if (j+1<n && s[j]=='*'&&s[j+1]=='/') { paint(out,n,i,j+2,fg_cmt); i=j+1; continue; }
            paint(out,n,i,n,fg_cmt); return (prov_hl_state_t){ CLK_BLOCKCMT,0,0 };
        }
        if (c=='"' || c=='\'' || ((flags & CLK_BACKTICK) && c=='`')) {
            proven_u8 q=c; proven_size_t j=i+1;
            while (j<n) { if (s[j]=='\\') { j+=2; continue; } if (s[j]==q) { j++; break; } j++; }
            paint(out,n,i,j,fg_str); i=j-1; continue;
        }
        if ((flags & CLK_DOLLAR) && c=='$') {
            proven_size_t j=i+1;
            if (j<n && s[j]=='{') { while (j<n && s[j]!='}') j++; if (j<n) j++; }
            else while (j<n && is_ident(s[j])) j++;
            paint(out,n,i,j,fg_var); i=j-1; continue;
        }
        if (is_digit(c) || (c=='.' && i+1<n && is_digit(s[i+1]))) {
            proven_size_t j=i; while (j<n && (is_ident(s[j])||s[j]=='.')) j++;
            paint(out,n,i,j,fg_num); i=j-1; continue;
        }
        if (is_ident(c) && !is_digit(c)) {
            proven_size_t j=i; while (j<n && is_ident(s[j])) j++; proven_size_t len=j-i;
            if (in_set(kw,nkw,s+i,len)) paint(out,n,i,j,fg_kw);
            else if (ty && in_set(ty,nty,s+i,len)) paint(out,n,i,j,fg_ty);
            else { proven_size_t k=j; while (k<n && (s[k]==' '||s[k]=='\t')) k++;
                   if (k<n && s[k]=='(') paint(out,n,i,j,fg_fn); }
            i=j-1; continue;
        }
    }
    return PROV_HL_STATE0;
}

/* ---- keyword sets ---- */
static const char *const JS_KW[] = {
    "break","case","catch","class","const","continue","debugger","default","delete",
    "do","else","export","extends","finally","for","function","if","import","in",
    "instanceof","let","new","return","super","switch","this","throw","try","typeof",
    "var","void","while","with","yield","async","await","static","get","set","of",
    "true","false","null","undefined",
};
static const char *const TS_TY[] = {
    "any","unknown","never","number","string","boolean","object","symbol","bigint",
    "void","readonly","type","interface","enum","namespace","declare","abstract",
    "implements","private","public","protected","keyof","infer",
};
static const char *const SH_KW[] = {
    "if","then","elif","else","fi","case","esac","for","select","while","until","do",
    "done","function","in","return","break","continue","local","export","readonly",
    "declare","typeset","unset","shift","exit","source","echo","cd","test",
};
static const char *const RUST_KW[] = {
    "as","async","await","break","const","continue","crate","dyn","else","enum",
    "extern","false","fn","for","if","impl","in","let","loop","match","mod","move",
    "mut","pub","ref","return","self","Self","static","struct","super","trait","true",
    "type","unsafe","use","where","while","union","macro_rules",
};
static const char *const RUST_TY[] = {
    "i8","i16","i32","i64","i128","isize","u8","u16","u32","u64","u128","usize",
    "f32","f64","bool","char","str","String","Vec","Option","Result","Box","Rc","Arc",
};
static const char *const GO_KW[] = {
    "break","case","chan","const","continue","default","defer","else","fallthrough",
    "for","func","go","goto","if","import","interface","map","package","range",
    "return","select","struct","switch","type","var","true","false","nil","iota",
};
static const char *const GO_TY[] = {
    "bool","byte","rune","string","error","int","int8","int16","int32","int64",
    "uint","uint8","uint16","uint32","uint64","uintptr","float32","float64",
    "complex64","complex128","any",
};
static const char *const JAVA_KW[] = {
    "abstract","assert","break","case","catch","class","const","continue","default",
    "do","else","enum","extends","final","finally","for","goto","if","implements",
    "import","instanceof","interface","native","new","package","private","protected",
    "public","return","static","strictfp","super","switch","synchronized","this",
    "throw","throws","transient","try","volatile","while","var","record","sealed",
    "true","false","null",
};
static const char *const JAVA_TY[] = {
    "boolean","byte","char","double","float","int","long","short","void",
    "String","Object","Integer","Long","Double","Boolean","List","Map","Set",
};
static const char *const KT_KW[] = {
    "as","break","class","continue","do","else","false","for","fun","if","in","is",
    "null","object","package","return","super","this","throw","true","try","typealias",
    "val","var","when","while","by","catch","constructor","delegate","import","init",
    "interface","override","private","protected","public","internal","sealed","data",
    "companion","enum","abstract","final","open","suspend","lateinit","vararg",
};
static const char *const SWIFT_KW[] = {
    "associatedtype","class","deinit","enum","extension","fileprivate","func","import",
    "init","inout","internal","let","open","operator","private","protocol","public",
    "static","struct","subscript","typealias","var","break","case","continue","default",
    "defer","do","else","fallthrough","for","guard","if","in","repeat","return",
    "switch","where","while","as","catch","false","is","nil","rethrows","self","Self",
    "super","throw","throws","true","try","async","await","actor","some","any",
};
static const char *const LUA_KW[] = {
    "and","break","do","else","elseif","end","false","for","function","goto","if",
    "in","local","nil","not","or","repeat","return","then","true","until","while","self",
};
static const char *const SQL_KW[] = {
    "select","from","where","insert","into","values","update","set","delete","create",
    "table","drop","alter","add","index","view","join","inner","left","right","outer",
    "on","group","by","order","having","limit","offset","union","all","distinct","as",
    "and","or","not","null","is","in","like","between","exists","case","when","then",
    "else","end","primary","key","foreign","references","default","unique","check",
    "constraint","int","integer","varchar","text","char","date","timestamp","boolean",
    "begin","commit","rollback","transaction","if","SELECT","FROM","WHERE","INSERT",
};

/* ===================================================================== */
/*  Python                                                               */
/* ===================================================================== */
/* state.kind: 0 NORMAL, 2 inside ''' , 3 inside """  (triple-string carry) */
enum { PY_TS_SINGLE = 2, PY_TS_DOUBLE = 3 };
static const char *const PY_KW[] = {
    "and","as","assert","async","await","break","class","continue","def","del",
    "elif","else","except","finally","for","from","global","if","import","in","is",
    "lambda","nonlocal","not","or","pass","raise","return","try","while","with",
    "yield","match","case","True","False","None",
};
static const char *const PY_BUILTIN[] = {
    "print","len","range","int","str","float","bool","list","dict","set","tuple",
    "self","cls","super","isinstance","type","open","enumerate","zip","map","filter",
};

static bool triple_at(const proven_u8 *s, proven_size_t n, proven_size_t i, proven_u8 q) {
    return i + 2 < n && s[i]==q && s[i+1]==q && s[i+2]==q;
}

static prov_hl_state_t hl_python(prov_hl_state_t st, const proven_u8 *s, proven_size_t n,
                                 const prov_theme_t *th, proven_u8 *out) {
    proven_u8 fg_kw=cls_fg(th,PROV_TOK_KEYWORD), fg_str=cls_fg(th,PROV_TOK_STRING);
    proven_u8 fg_cmt=cls_fg(th,PROV_TOK_COMMENT), fg_num=cls_fg(th,PROV_TOK_NUMBER);
    proven_u8 fg_fn=cls_fg(th,PROV_TOK_FUNCTION), fg_bi=cls_fg(th,PROV_TOK_BUILTIN);
    proven_u8 fg_ty=cls_fg(th,PROV_TOK_TYPE);
    for (proven_size_t i=0;i<n;i++) out[i]=0;
    proven_size_t i=0;

    if (st.kind == PY_TS_SINGLE || st.kind == PY_TS_DOUBLE) {
        proven_u8 q = (st.kind==PY_TS_SINGLE) ? '\'' : '"';
        while (i < n) { if (triple_at(s,n,i,q)) { paint(out,n,0,i+3,fg_str); i+=3; goto body; } i++; }
        paint(out,n,0,n,fg_str); return st;
    }
body:
    for (; i<n; i++) {
        proven_u8 c=s[i];
        if (c=='#') { paint(out,n,i,n,fg_cmt); break; }
        if (c=='"' || c=='\'') {
            if (triple_at(s,n,i,c)) {
                proven_size_t j=i+3; while (j+2<n && !triple_at(s,n,j,c)) j++;
                if (j+2<n && triple_at(s,n,j,c)) { paint(out,n,i,j+3,fg_str); i=j+2; continue; }
                paint(out,n,i,n,fg_str);
                return (prov_hl_state_t){ c=='\''?PY_TS_SINGLE:PY_TS_DOUBLE, 0, 0 };
            }
            proven_u8 q=c; proven_size_t j=i+1;
            while (j<n){ if(s[j]=='\\'){j+=2;continue;} if(s[j]==q){j++;break;} j++; }
            paint(out,n,i,j,fg_str); i=j-1; continue;
        }
        if (c=='@') {  /* decorator */
            proven_size_t j=i+1; while (j<n && is_ident(s[j])) j++;
            paint(out,n,i,j,cls_fg(th,PROV_TOK_PREPROC)); i=j-1; continue;
        }
        if (is_digit(c) || (c=='.' && i+1<n && is_digit(s[i+1]))) {
            proven_size_t j=i; while (j<n && (is_ident(s[j])||s[j]=='.')) j++;
            paint(out,n,i,j,fg_num); i=j-1; continue;
        }
        if (is_ident(c) && !is_digit(c)) {
            proven_size_t j=i; while (j<n && is_ident(s[j])) j++; proven_size_t len=j-i;
            if (in_set(PY_KW,sizeof PY_KW/sizeof PY_KW[0],s+i,len)) paint(out,n,i,j,fg_kw);
            else if (in_set(PY_BUILTIN,sizeof PY_BUILTIN/sizeof PY_BUILTIN[0],s+i,len)) paint(out,n,i,j,fg_bi);
            else if (s[i]>='A'&&s[i]<='Z') paint(out,n,i,j,fg_ty);   /* ClassName convention */
            else { proven_size_t k=j; while (k<n && (s[k]==' '||s[k]=='\t')) k++;
                   if (k<n && s[k]=='(') paint(out,n,i,j,fg_fn); }
            i=j-1; continue;
        }
    }
    return PROV_HL_STATE0;
}

/* ===================================================================== */
/*  JSON                                                                 */
/* ===================================================================== */
static prov_hl_state_t hl_json(prov_hl_state_t st, const proven_u8 *s, proven_size_t n,
                               const prov_theme_t *th, proven_u8 *out) {
    (void)st;
    proven_u8 fg_str=cls_fg(th,PROV_TOK_STRING), fg_num=cls_fg(th,PROV_TOK_NUMBER);
    proven_u8 fg_kw=cls_fg(th,PROV_TOK_KEYWORD), fg_key=cls_fg(th,PROV_TOK_KEY);
    for (proven_size_t i=0;i<n;i++) out[i]=0;
    for (proven_size_t i=0;i<n;i++) {
        proven_u8 c=s[i];
        if (c=='"') {
            proven_u8 q=c; proven_size_t j=i+1;
            while (j<n){ if(s[j]=='\\'){j+=2;continue;} if(s[j]==q){j++;break;} j++; }
            proven_size_t k=j; while (k<n && (s[k]==' '||s[k]=='\t')) k++;   /* a key if "..." : */
            paint(out,n,i,j, (k<n && s[k]==':') ? fg_key : fg_str);
            i=j-1; continue;
        }
        if (is_digit(c) || (c=='-' && i+1<n && is_digit(s[i+1]))) {
            proven_size_t j=i+1; while (j<n && (is_digit(s[j])||s[j]=='.'||s[j]=='e'||s[j]=='E'||s[j]=='+'||s[j]=='-')) j++;
            paint(out,n,i,j,fg_num); i=j-1; continue;
        }
        if (is_ident(c)) {
            proven_size_t j=i; while (j<n && is_ident(s[j])) j++; proven_size_t len=j-i;
            const char *lits[]={"true","false","null"};
            if (in_set(lits,3,s+i,len)) paint(out,n,i,j,fg_kw);
            i=j-1; continue;
        }
    }
    return PROV_HL_STATE0;
}

/* ===================================================================== */
/*  TOML / INI / .env  (key = value, [section], # and ; comments)        */
/* ===================================================================== */
static prov_hl_state_t hl_toml(prov_hl_state_t st, const proven_u8 *s, proven_size_t n,
                               const prov_theme_t *th, proven_u8 *out) {
    (void)st;
    proven_u8 fg_str=cls_fg(th,PROV_TOK_STRING), fg_num=cls_fg(th,PROV_TOK_NUMBER);
    proven_u8 fg_cmt=cls_fg(th,PROV_TOK_COMMENT), fg_sec=cls_fg(th,PROV_TOK_FUNCTION);
    proven_u8 fg_key=cls_fg(th,PROV_TOK_KEY), fg_kw=cls_fg(th,PROV_TOK_KEYWORD);
    for (proven_size_t i=0;i<n;i++) out[i]=0;
    proven_size_t a=0; while (a<n && (s[a]==' '||s[a]=='\t')) a++;
    if (a<n && (s[a]=='#'||s[a]==';')) { paint(out,n,a,n,fg_cmt); return PROV_HL_STATE0; }
    if (a<n && s[a]=='[') { paint(out,n,a,n,fg_sec); return PROV_HL_STATE0; }   /* [section] */
    /* key = value : color the key up to = */
    proven_size_t eq=a; while (eq<n && s[eq]!='=' && s[eq]!='#' && s[eq]!=';') eq++;
    if (eq<n && s[eq]=='=') paint(out,n,a,eq,fg_key);
    /* value side: strings / numbers / true|false + trailing comment */
    for (proven_size_t i=(eq<n&&s[eq]=='=')?eq+1:a; i<n; i++) {
        proven_u8 c=s[i];
        if (c=='#'||c==';') { paint(out,n,i,n,fg_cmt); break; }
        if (c=='"'||c=='\'') { proven_u8 q=c; proven_size_t j=i+1;
            while (j<n){ if(s[j]=='\\'){j+=2;continue;} if(s[j]==q){j++;break;} j++; }
            paint(out,n,i,j,fg_str); i=j-1; continue; }
        if (is_digit(c)||(c=='-'&&i+1<n&&is_digit(s[i+1]))) { proven_size_t j=i+1;
            while (j<n && (is_digit(s[j])||s[j]=='.'||s[j]=='_'||s[j]==':')) j++;
            paint(out,n,i,j,fg_num); i=j-1; continue; }
        if (is_ident(c)&&!is_digit(c)) { proven_size_t j=i; while (j<n && is_ident(s[j])) j++;
            const char *lits[]={"true","false","on","off","yes","no"};
            if (in_set(lits,6,s+i,j-i)) paint(out,n,i,j,fg_kw);
            i=j-1; continue; }
    }
    return PROV_HL_STATE0;
}

/* ===================================================================== */
/*  YAML                                                                 */
/* ===================================================================== */
static prov_hl_state_t hl_yaml(prov_hl_state_t st, const proven_u8 *s, proven_size_t n,
                               const prov_theme_t *th, proven_u8 *out) {
    (void)st;
    proven_u8 fg_str=cls_fg(th,PROV_TOK_STRING), fg_num=cls_fg(th,PROV_TOK_NUMBER);
    proven_u8 fg_cmt=cls_fg(th,PROV_TOK_COMMENT), fg_key=cls_fg(th,PROV_TOK_KEY);
    proven_u8 fg_kw=cls_fg(th,PROV_TOK_KEYWORD), fg_pun=cls_fg(th,PROV_TOK_NUMBER);
    for (proven_size_t i=0;i<n;i++) out[i]=0;
    proven_size_t a=0; while (a<n && (s[a]==' '||s[a]=='\t')) a++;
    if (a<n && s[a]=='#') { paint(out,n,a,n,fg_cmt); return PROV_HL_STATE0; }
    proven_size_t start=a;
    if (a<n && s[a]=='-' && (a+1>=n || s[a+1]==' ')) { paint(out,n,a,a+1,fg_pun); start=a+1;
        while (start<n && s[start]==' ') start++; }
    /* key: value */
    proven_size_t col=start; while (col<n && s[col]!=':' && s[col]!='#') col++;
    if (col<n && s[col]==':') paint(out,n,start,col,fg_key);
    for (proven_size_t i=(col<n&&s[col]==':')?col+1:start; i<n; i++) {
        proven_u8 c=s[i];
        if (c=='#'&&i>0&&s[i-1]==' ') { paint(out,n,i,n,fg_cmt); break; }
        if (c=='"'||c=='\'') { proven_u8 q=c; proven_size_t j=i+1;
            while (j<n){ if(s[j]==q){j++;break;} j++; } paint(out,n,i,j,fg_str); i=j-1; continue; }
        if (is_digit(c)) { proven_size_t j=i; while (j<n && (is_digit(s[j])||s[j]=='.')) j++;
            paint(out,n,i,j,fg_num); i=j-1; continue; }
        if (is_ident(c)&&!is_digit(c)) { proven_size_t j=i; while (j<n && is_ident(s[j])) j++;
            const char *lits[]={"true","false","null","yes","no"};
            if (in_set(lits,5,s+i,j-i)) paint(out,n,i,j,fg_kw);
            i=j-1; continue; }
    }
    return PROV_HL_STATE0;
}

/* ===================================================================== */
/*  CSS                                                                  */
/* ===================================================================== */
/* state.kind: 0 NORMAL, 1 inside a slash-star block comment */
static prov_hl_state_t hl_css(prov_hl_state_t st, const proven_u8 *s, proven_size_t n,
                              const prov_theme_t *th, proven_u8 *out) {
    proven_u8 fg_str=cls_fg(th,PROV_TOK_STRING), fg_num=cls_fg(th,PROV_TOK_NUMBER);
    proven_u8 fg_cmt=cls_fg(th,PROV_TOK_COMMENT), fg_prop=cls_fg(th,PROV_TOK_TYPE);
    proven_u8 fg_sel=cls_fg(th,PROV_TOK_FUNCTION), fg_at=cls_fg(th,PROV_TOK_PREPROC);
    for (proven_size_t i=0;i<n;i++) out[i]=0;
    proven_size_t i=0;
    if (st.kind == CLK_BLOCKCMT) {
        while (i<n){ if(i+1<n&&s[i]=='*'&&s[i+1]=='/'){paint(out,n,0,i+2,fg_cmt);i+=2;goto body;} i++; }
        paint(out,n,0,n,fg_cmt); return st;
    }
body:
    /* a property line "name:" inside a block, vs a selector / at-rule */
    {
        proven_size_t a=i; while (a<n && (s[a]==' '||s[a]=='\t')) a++;
        if (a<n && s[a]=='@') { paint(out,n,a,n,fg_at); }
    }
    for (; i<n; i++) {
        proven_u8 c=s[i];
        if (c=='/' && i+1<n && s[i+1]=='*') {
            proven_size_t j=i+2; while (j+1<n && !(s[j]=='*'&&s[j+1]=='/')) j++;
            if (j+1<n) { paint(out,n,i,j+2,fg_cmt); i=j+1; continue; }
            paint(out,n,i,n,fg_cmt); return (prov_hl_state_t){ CLK_BLOCKCMT,0,0 };
        }
        if (c=='"'||c=='\'') { proven_u8 q=c; proven_size_t j=i+1;
            while (j<n){ if(s[j]=='\\'){j+=2;continue;} if(s[j]==q){j++;break;} j++; }
            paint(out,n,i,j,fg_str); i=j-1; continue; }
        if (c=='#') { proven_size_t j=i+1; while (j<n && is_ident(s[j])) j++;   /* #id / #hex */
            paint(out,n,i,j,fg_num); i=j-1; continue; }
        if (is_digit(c)||(c=='.'&&i+1<n&&is_digit(s[i+1]))) { proven_size_t j=i;
            while (j<n && (is_ident(s[j])||s[j]=='.'||s[j]=='%')) j++;
            paint(out,n,i,j,fg_num); i=j-1; continue; }
        if (is_ident(c)&&!is_digit(c)) { proven_size_t j=i; while (j<n && (is_ident(s[j])||s[j]=='-')) j++;
            proven_size_t k=j; while (k<n && (s[k]==' '||s[k]=='\t')) k++;
            if (k<n && s[k]==':') paint(out,n,i,j,fg_prop);   /* property: */
            else paint(out,n,i,j,fg_sel);                     /* selector token */
            i=j-1; continue; }
    }
    return PROV_HL_STATE0;
}

/* ===================================================================== */
/*  Markup (HTML / XML)                                                   */
/* ===================================================================== */
/* state.kind: 0 NORMAL, 1 inside <!-- ... --> comment */
enum { MK_NORMAL = 0, MK_COMMENT = 1 };
static prov_hl_state_t hl_markup(prov_hl_state_t st, const proven_u8 *s, proven_size_t n,
                                 const prov_theme_t *th, proven_u8 *out) {
    proven_u8 fg_tag=cls_fg(th,PROV_TOK_KEYWORD), fg_attr=cls_fg(th,PROV_TOK_KEY);
    proven_u8 fg_str=cls_fg(th,PROV_TOK_STRING), fg_cmt=cls_fg(th,PROV_TOK_COMMENT);
    proven_u8 fg_ent=cls_fg(th,PROV_TOK_NUMBER);
    for (proven_size_t i=0;i<n;i++) out[i]=0;
    proven_size_t i=0;

    if (st.kind == MK_COMMENT) {
        while (i+2 < n) { if (s[i]=='-'&&s[i+1]=='-'&&s[i+2]=='>') { paint(out,n,0,i+3,fg_cmt); i+=3; goto body; } i++; }
        paint(out,n,0,n,fg_cmt); return st;
    }
body:
    for (; i<n; i++) {
        proven_u8 c=s[i];
        if (c=='<' && i+3<n && s[i+1]=='!'&&s[i+2]=='-'&&s[i+3]=='-') {   /* comment */
            proven_size_t j=i+4; while (j+2<n && !(s[j]=='-'&&s[j+1]=='-'&&s[j+2]=='>')) j++;
            if (j+2<n) { paint(out,n,i,j+3,fg_cmt); i=j+2; continue; }
            paint(out,n,i,n,fg_cmt); return (prov_hl_state_t){ MK_COMMENT,0,0 };
        }
        if (c=='<') {   /* a tag: color < / tagname, attrs, strings, > */
            proven_size_t j=i+1;
            if (j<n && (s[j]=='/'||s[j]=='!'||s[j]=='?')) j++;
            proven_size_t ts=j; while (j<n && (is_ident(s[j])||s[j]==':'||s[j]=='-')) j++;
            paint(out,n,i,j,fg_tag);                 /* '<' + tag name */
            (void)ts;
            while (j<n && s[j]!='>') {                /* attributes + values */
                if (s[j]=='"'||s[j]=='\'') { proven_u8 q=s[j]; proven_size_t k=j+1;
                    while (k<n && s[k]!=q) k++;
                    if (k<n) k++;
                    paint(out,n,j,k,fg_str); j=k; continue; }
                if (is_ident(s[j])) { proven_size_t k=j; while (k<n && (is_ident(s[k])||s[k]=='-'||s[k]==':')) k++;
                    paint(out,n,j,k,fg_attr); j=k; continue; }
                j++;
            }
            if (j<n && s[j]=='>') { out[j]=fg_tag; }
            i=j; continue;
        }
        if (c=='&') {   /* entity &amp; */
            proven_size_t j=i+1; while (j<n && j<i+10 && s[j]!=';' && is_ident(s[j])) j++;
            if (j<n && s[j]==';') { paint(out,n,i,j+1,fg_ent); i=j; continue; }
        }
    }
    return PROV_HL_STATE0;
}

/* ===================================================================== */

prov_hl_state_t prov_hl_line(prov_hl_lang_t lang, prov_hl_state_t st,
                             const proven_u8 *line, proven_size_t len,
                             const prov_theme_t *theme, proven_u8 *out_fg) {
    if (len) for (proven_size_t i = 0; i < len; i++) out_fg[i] = 0;
    switch (lang) {
        case PROV_HL_MARKDOWN: return hl_markdown(st, line, len, theme, out_fg);
        case PROV_HL_C:        return hl_c(st, line, len, theme, out_fg);
        case PROV_HL_PYTHON:   return hl_python(st, line, len, theme, out_fg);
        case PROV_HL_JAVASCRIPT:
            return hl_clike(st, line, len, theme, out_fg,
                            JS_KW, sizeof JS_KW/sizeof JS_KW[0], NULL, 0,
                            CLK_BLOCK|CLK_SLASHLC|CLK_BACKTICK);
        case PROV_HL_TYPESCRIPT:
            return hl_clike(st, line, len, theme, out_fg,
                            JS_KW, sizeof JS_KW/sizeof JS_KW[0],
                            TS_TY, sizeof TS_TY/sizeof TS_TY[0],
                            CLK_BLOCK|CLK_SLASHLC|CLK_BACKTICK);
        case PROV_HL_SHELL:
            return hl_clike(st, line, len, theme, out_fg,
                            SH_KW, sizeof SH_KW/sizeof SH_KW[0], NULL, 0,
                            CLK_HASHLC|CLK_DOLLAR);
        case PROV_HL_JSON:     return hl_json(st, line, len, theme, out_fg);
        case PROV_HL_TOML:     return hl_toml(st, line, len, theme, out_fg);
        case PROV_HL_YAML:     return hl_yaml(st, line, len, theme, out_fg);
        case PROV_HL_CSS:      return hl_css(st, line, len, theme, out_fg);
        case PROV_HL_MARKUP:   return hl_markup(st, line, len, theme, out_fg);
        case PROV_HL_RUST:
            return hl_clike(st, line, len, theme, out_fg,
                            RUST_KW, sizeof RUST_KW/sizeof RUST_KW[0],
                            RUST_TY, sizeof RUST_TY/sizeof RUST_TY[0],
                            CLK_BLOCK|CLK_SLASHLC);
        case PROV_HL_GO:
            return hl_clike(st, line, len, theme, out_fg,
                            GO_KW, sizeof GO_KW/sizeof GO_KW[0],
                            GO_TY, sizeof GO_TY/sizeof GO_TY[0],
                            CLK_BLOCK|CLK_SLASHLC|CLK_BACKTICK);
        case PROV_HL_JAVA:
            return hl_clike(st, line, len, theme, out_fg,
                            JAVA_KW, sizeof JAVA_KW/sizeof JAVA_KW[0],
                            JAVA_TY, sizeof JAVA_TY/sizeof JAVA_TY[0],
                            CLK_BLOCK|CLK_SLASHLC);
        case PROV_HL_KOTLIN:
            return hl_clike(st, line, len, theme, out_fg,
                            KT_KW, sizeof KT_KW/sizeof KT_KW[0], NULL, 0,
                            CLK_BLOCK|CLK_SLASHLC|CLK_DOLLAR);
        case PROV_HL_SWIFT:
            return hl_clike(st, line, len, theme, out_fg,
                            SWIFT_KW, sizeof SWIFT_KW/sizeof SWIFT_KW[0], NULL, 0,
                            CLK_BLOCK|CLK_SLASHLC);
        case PROV_HL_LUA:
            return hl_clike(st, line, len, theme, out_fg,
                            LUA_KW, sizeof LUA_KW/sizeof LUA_KW[0], NULL, 0,
                            CLK_DASHLC);
        case PROV_HL_SQL:
            return hl_clike(st, line, len, theme, out_fg,
                            SQL_KW, sizeof SQL_KW/sizeof SQL_KW[0], NULL, 0,
                            CLK_BLOCK|CLK_DASHLC);
        default:               return PROV_HL_STATE0;
    }
}
