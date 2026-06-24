#ifndef PROV_REGEX_H
#define PROV_REGEX_H

/* Self-contained regex engine (RFC-0009): a Pike VM (Thompson NFA + submatch
 * tracking) — linear time, no backtracking/ReDoS, capture groups. This header
 * grows by stage; S1 is the parser → AST. Pure: libc-free, allocator-injected,
 * byte-range oriented. Semantics are leftmost-greedy (Perl/RE2/ripgrep). */

#include "proven/types.h"
#include "proven/allocator.h"
#include "proven/u8str.h"
#include "proven/error.h"

/* ---- AST (S1) ---------------------------------------------------------- */

typedef enum {
    RX_EMPTY,    /* epsilon */
    RX_LIT,      /* one literal byte (`byte`) */
    RX_ANY,      /* `.` — one codepoint (excludes '\n' unless DOTALL) */
    RX_CLASS,    /* [ ... ] — `set` bitmap over bytes, `negated` */
    RX_ANCHOR,   /* ^ $ \b \B — `anchor` */
    RX_MARK,     /* \zs \ze — `mark` */
    RX_CONCAT,   /* sequence kids[0..nkids) */
    RX_ALT,      /* alternation kids[0..nkids) */
    RX_REPEAT,   /* kids[0] repeated [rmin,rmax]; `lazy` */
    RX_GROUP     /* ( ... ) kids[0]; `group` = capture index (0 = non-capturing) */
} prov_rx_kind_t;

enum { RX_A_BOL, RX_A_EOL, RX_A_WORDB, RX_A_NWORDB };   /* anchor kinds */
enum { RX_M_ZS, RX_M_ZE };                              /* mark kinds */
#define RX_INF ((proven_u32)0xFFFFFFFFu)

typedef struct prov_rx_node prov_rx_node_t;
struct prov_rx_node {
    prov_rx_kind_t   kind;
    prov_rx_node_t **kids;
    proven_size_t    nkids;
    proven_u8        byte;       /* RX_LIT */
    proven_u8        set[32];    /* RX_CLASS: 256-bit membership bitmap */
    bool             negated;    /* RX_CLASS */
    int              anchor;     /* RX_ANCHOR */
    int              mark;       /* RX_MARK */
    int              group;      /* RX_GROUP capture index (>=1; 0 = non-capturing) */
    proven_u32       rmin, rmax; /* RX_REPEAT (rmax == RX_INF = unbounded) */
    bool             lazy;       /* RX_REPEAT */
};

typedef struct {
    proven_err_t    err;        /* OK, or PROVEN_ERR_INVALID_ARG / OOM on failure */
    proven_size_t   err_off;    /* byte offset of the error within the pattern */
    const char     *err_msg;    /* static description */
    prov_rx_node_t *root;       /* AST root (lives in `backing`); NULL on error */
    int             ngroups;    /* number of capturing groups */
    void           *backing;    /* arena backing — release with prov_rx_parse_free */
} prov_rx_parse_t;

/* Parse `pattern` into an AST. Always pair with prov_rx_parse_free (it owns the
 * backing arena that holds every node). On a malformed pattern, `err` != OK and
 * err_off / err_msg locate it. */
prov_rx_parse_t prov_rx_parse(proven_allocator_t a, proven_u8str_view_t pattern);
void prov_rx_parse_free(proven_allocator_t a, prov_rx_parse_t *p);

/* Serialize the AST as an s-expression into `buf` (NUL-terminated, bounded).
 * Returns the length that would be written (may exceed cap). Test/debug aid. */
proven_size_t prov_rx_ast_dump(const prov_rx_node_t *n, char *buf, proven_size_t cap);

/* ---- compiled program / bytecode (S2) ----------------------------------- */

#define PROV_RX_MAX_GROUPS 32
#define PROV_RX_MAX_INSTS  8192u    /* total program-size cap (decision 4); also
                                     * bounds addthread recursion depth */
#define PROV_RX_NONE ((proven_size_t)-1)   /* unset capture slot */

enum { PROV_RX_ICASE = 1u << 0, PROV_RX_MULTILINE = 1u << 1, PROV_RX_DOTALL = 1u << 2 };

typedef enum {
    RXO_BYTE,    /* x = exact byte */
    RXO_RANGE,   /* x..y inclusive byte range */
    RXO_CLASS,   /* x = index into `sets` (256-bit membership) */
    RXO_SPLIT,   /* fork: try x (higher priority) then y */
    RXO_JMP,     /* x = target */
    RXO_SAVE,    /* x = capture slot (2*group .. 2*group+1; 0/1 = whole match) */
    RXO_ASSERT,  /* x = anchor kind (RX_A_*) */
    RXO_MATCH
} prov_rx_op_t;

typedef struct { proven_u32 op, x, y; } prov_rx_inst_t;

typedef struct prov_regex {
    prov_rx_inst_t *insts;
    proven_size_t   ninsts, instcap;
    proven_u8     (*sets)[32];
    proven_size_t   nsets, setcap;
    int             ngroups;
    unsigned        flags;
    bool            has_ze;     /* a \ze marker set group-0 end explicitly */
    /* exec scratch (allocated at compile, reused across runs) */
    proven_size_t   nsave;      /* 2*(ngroups+1) capture slots */
    proven_u32      gen;        /* dedup generation counter */
    proven_u32     *vis;        /* ninsts: per-pc visit stamp */
    proven_u32     *cl_pc, *nl_pc;   /* ninsts each: thread program counters */
    proven_size_t  *cl_sv, *nl_sv;   /* ninsts*nsave each: thread capture slots */
} prov_regex_t;

typedef struct {
    proven_size_t start, end;                 /* group 0 (honors \zs/\ze) */
    int           ngroups;
    struct { proven_size_t start, end; bool set; } groups[PROV_RX_MAX_GROUPS];
} prov_regex_match_t;

typedef struct {
    proven_err_t   err;
    proven_size_t  err_off;
    const char    *err_msg;
    prov_regex_t  *re;          /* owned; free with prov_regex_destroy */
} prov_result_regex_t;

/* Compile `pattern` to a Pike VM program. On a bad pattern, err != OK + offset. */
prov_result_regex_t prov_regex_compile(proven_allocator_t a, proven_u8str_view_t pattern,
                                       unsigned flags);
void prov_regex_destroy(proven_allocator_t a, prov_regex_t *re);

/* Disassemble the program (one instruction per line). Test/debug aid. */
proven_size_t prov_rx_prog_dump(const prov_regex_t *re, char *buf, proven_size_t cap);

/* ---- execution (S3) ----------------------------------------------------- */

/* Anchored: succeed iff a match starts exactly at `at`. Fills `out` (leftmost-
 * greedy captures) on success. Mutates `re`'s exec scratch (not thread-safe;
 * one regex object per matching site). */
bool prov_regex_match_at(prov_regex_t *re, const proven_u8 *hay, proven_size_t len,
                         proven_size_t at, prov_regex_match_t *out);

/* Unanchored: the leftmost match whose start is >= `from`, in a single linear
 * pass (implicit lazy prefix; not O(n^2)). Fills `out` on success. */
bool prov_regex_search(prov_regex_t *re, const proven_u8 *hay, proven_size_t len,
                       proven_size_t from, prov_regex_match_t *out);

#endif /* PROV_REGEX_H */
