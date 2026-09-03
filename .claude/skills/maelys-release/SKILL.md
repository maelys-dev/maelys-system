---
name: maelys-release
description: Release a Maelys product through the shared maelys-release socle, or change its release, packaging or Homebrew formula files, without breaking the signed-tag, attestation and tap contracts.
---

# Releasing a Maelys product

The product is `maelys-system`; its release mechanics come from maelys-release
v0.2.1 (`docs/conventions.md` there is normative).

## Cut a release

1. On `main`, set `VERSION`, date the `CHANGELOG.md` entry, regenerate any
   generated documentation, and run `make check`; it must pass on the exact
   commit that will be tagged.
2. Merge through a pull request with green CI.
3. Tag the merge commit: `git tag -s vX.Y.Z -m "maelys-system X.Y.Z"`, then
   `git push origin vX.Y.Z`. The tag must be annotated and signed with a key
   registered on GitHub; the workflow refuses anything else.
4. Watch the `release` workflow; the `publish` job runs in the `release`
   environment. Verify with `gh release view vX.Y.Z` and
   `gh attestation verify <asset> --repo <owner>/<repo>`.

## Change release or packaging files

- `.github/workflows/release.yml`: never by hand. Upgrade with
  `scripts/adopt.sh DIR --apply` from a maelys-release checkout at the target
  tag; check drift with `--check`.
- `scripts/package-release.sh TARGET`: must leave every artifact and its
  `.sha256` in `dist/`; keep it runnable locally.
- `packaging/homebrew/maelys-system.rb.in`: placeholders `@URL@`, `@VERSION@`,
  `@SHA256@`, plus any pin placeholder your renderer fills. Validate with
  `brew style`, `brew audit --strict` through a temporary tap, and
  `brew install --build-from-source` followed by `brew test`.
- Bottles: the tap workflow builds them on the macOS runners listed in
  `bottles`; the formula keeps its source URL so `--build-from-source`
  always works for an open product.

## Never

- push a tag whose commit did not pass `make check`;
- force-push or delete a published tag;
- edit files under `docs/generated/` by hand;
- put credentials in the repository; the tap secrets are
  `HOMEBREW_TAP_TOKEN` and `HOMEBREW_TAP_SIGNING_KEY`;
- run release jobs on a self-hosted runner from a public repository.
