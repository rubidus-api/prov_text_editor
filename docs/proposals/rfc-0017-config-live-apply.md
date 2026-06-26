# RFC-0017 — `zc` config live-apply

- **Status:** **IMPLEMENTED (2026-06-22).** Phase 6 of the `TODO.md` roadmap
  (backlog #7). Builds on the existing `zc` config editor and `prov_config_parse`.

## 1. Motivation

`zc` opens the config file the editor actually reads at startup
(`config_path`: an exe-side `provconf.ini` if present, else
`~/.prov/provconf.ini`). Until now editing it required a **restart** to see any
effect, and nothing in the UI told you the buffer you were editing was *the*
config. This RFC closes that loop: the config is **re-applied live on save**, and
the buffer is **tagged** so it's obvious which file drives the editor.

## 2. Behavior

- **Tag.** When a window shows the config file, its status line appends a
  ` [config]` marker after the name (`is_config_path` compares the buffer path to
  `config_path`).
- **Apply on save.** After any successful save of the config file (`zw`, `:w`,
  Ed-mode `^S`, or save-as to that path), the just-saved buffer text is re-parsed
  with `prov_config_parse` and applied:
  - On success → `config_apply` replaces `s->cfg` and runs the few settings that
    need a side effect on change; status shows **"config applied (live)"**.
  - On a parse error → the running config is left **untouched** and the status
    shows **"config saved — NOT applied (line N: …)"**.

## 3. What "live" covers

Almost every setting is read straight from `s->cfg` at its point of use
(`tabstop`, `expandtab`, `scrolloff`, `line_numbers`, `wrap`, `trigger`, the
`[search]` flags, `clipboard_sync`, `fallback_encoding`), so replacing `s->cfg`
makes them take effect on the next frame / next use. `config_apply` additionally
fires the side-effecting appliers:

| setting | applier |
|---------|---------|
| `mouse` | `prov_term_enable_mouse(on)` when it changed |
| `charset_backend` | `prov_charset_configure` |
| `charset_iconv_path` | `prov_charset_set_iconv_path` |
| `undo_limit` | `prov_editor_set_undo_limit` on every open buffer |

**No setting currently requires a restart.** The roadmap item mentioned
relaunching for restart-only settings; in practice the config has none today. If a
future setting genuinely cannot be applied live, gate it in `config_apply` and tell
the user to restart (or offer a relaunch) — the hook is the single place to do so.

## 4. Out of scope

- A "reload config from disk" command (today apply is tied to saving the buffer).
- Per-buffer / per-filetype config overrides.
