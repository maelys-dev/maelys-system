---
name: maelys-release
description: Release a Maelys product through the shared maelys-release socle, or change its release, packaging, dependency pins or Homebrew formula files, without breaking the signed-tag, attestation and tap contracts.
---

# Releasing a Maelys product

The product is `maelys-system`; its release mechanics come from maelys-release
v0.5.0 (`docs/conventions.md` there is normative).

## Cut a release

1. On `main`, set `VERSION`, date the `CHANGELOG.md` entry (`check`
   reports a `VERSION` without one), regenerate any generated
   documentation, and run `make check`; it must pass on the exact commit
   that will be tagged.
2. Merge through a pull request with green CI.
3. Run `bin/maelys-release preflight .` from a maelys-release checkout at
   v0.5.0; it exits 2 on anything the workflow would refuse (signing
   configuration, previous tag, existing `vX.Y.Z`, `release` environment
   not limited to tags `v*`).
4. Tag the merge commit: `git tag -s vX.Y.Z -m "maelys-system X.Y.Z"`, then
   `git push origin vX.Y.Z`. The tag must be annotated and signed with a key
   registered on GitHub; the workflow refuses anything else.
5. Watch the `release` workflow; the `publish` job runs in the `release`
   environment. Verify with `gh release view vX.Y.Z` and
   `gh attestation verify <asset> --repo <owner>/<repo>`.

## Before the first tag, or after changing packaging

Run `bin/maelys-release rehearse . linux-arm64` (and `linux-x86_64`,
emulated) from a maelys-release checkout: it replays the Linux build job of the
release in Docker on a copy of the working tree. The release must never be
the first Linux build of the product.

## Change release or packaging files

- `.github/workflows/release.yml` and `scripts/checkout-dependency.sh`:
  never by hand. They are generated from `adapter/*_PIN`, `adapter/PACKAGES`
  and `packaging/homebrew/*.rb.in`; change those, then run
  `bin/maelys-release adopt DIR --apply` from a maelys-release checkout at
  the target tag; check drift with `bin/maelys-release check DIR`. Every
  command answers `--format json` with an agent-cli/v2 envelope and
  `describe --format json` returns the catalog.
- `adapter/<NAME>_PIN`: nearest tag on line 1, pinned commit on line 2.
  `scripts/checkout-dependency.sh NAME` clones the dependency at that
  commit; write no other checkout script.
- `adapter/PACKAGES`: the apt (`[linux]`) and brew (`[macos]`) packages the
  build needs, one per line. Nothing else installs packages in a release
  or in CI: `.github/workflows/ci.yml` calls the socle's
  `check-product.yml` with the same declarations; keep that job, add yours
  next to it.
- `scripts/package-release.sh TARGET`: must leave every artifact and its
  `.sha256` in `dist/`; keep it runnable locally.
- packaging/homebrew/libmaelys-sys.rb.in: placeholders `@URL@`, `@VERSION@`,
  `@SHA256@`, plus any pin placeholder your renderer fills. Validate with
  `brew style`, `brew audit --strict` through a temporary tap, and
  `brew install --build-from-source` followed by `brew test`.
- Bottles: the tap workflow builds them on the macOS runners listed in
  `bottles`; the formula keeps its source URL so `--build-from-source`
  always works for an open product.

## Never

- push a tag whose commit did not pass `make check`;
- force-push or delete a published tag;
- edit a generated file by hand: `release.yml`, `checkout-dependency.sh`,
  anything under `docs/generated/`;
- install packages from a checkout script or from `package-release.sh`;
  declare them in `adapter/PACKAGES`;
- put credentials in the repository; the tap secrets are
  `HOMEBREW_TAP_TOKEN` and `HOMEBREW_TAP_SIGNING_KEY`;
- run release jobs on a self-hosted runner from a public repository.
