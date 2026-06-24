# Proposal Documents

This directory holds proposal-only notes for possible future features, policy
changes, and UX conventions.

These files are intentionally not the source of truth for shipped behavior.
Adopted behavior belongs in committed public documentation such as
`README.md`, `TEST.md`, `CHANGELOG.md`, and the `SPEC.md` file.

## Naming convention

Use this pattern for a proposal document:

```text
docs/proposals/rfc-0001-short-title.md
```

Rules:

- `rfc-` means "request for comments" in the generic sense.
- The number is zero-padded for stable ordering.
- The title is lowercase kebab-case.
- The document header should mark the item as `Draft`, `Proposal`, or another
  explicit status if needed.
- A proposal name does not imply adoption.

## Suggested alternatives

If a different label fits better than `rfc`, the usual options are:

- `proposal`
- `design-note`
- `spec-draft`

In ISO C / WG14 work, proposals are often circulated as `Nxxxx` papers, and
defect reports use `DR`-style labels. For this repository, `rfc-####-title.md`
is the preferred default because it reads as a reviewable proposal without
claiming final design status.
