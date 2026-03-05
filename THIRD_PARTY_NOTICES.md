# Third-Party Notices

## Yash POSIX test corpus and harness

Files in `tests/posix/` are vendored from `yash/tests` with local adaptation
for build wiring. Only POSIX-oriented test cases (`*-p.tst`) are included.
Additionally, `tests/siglist.h` is vendored from `yash/siglist.h` for the
`resetsig.c` helper build.

Upstream project:
- Name: yash (yet another shell)
- URL: https://github.com/magicant/yash
- Commit pinned for this import baseline: `77e38846848c7a5b975aa49d14ed142e4c7823fc`

Licensing:
- Most imported harness/helper files are GNU GPL v2 or later, as indicated in
  their file headers.
- `ptwrap.c` is MIT-licensed, as indicated in its file header.

License texts:
- GPL text is included as `tests/posix/COPYING`.
- MIT notice is preserved in `tests/posix/ptwrap.c`.

Notes:
- `posish` implementation under `src/` and `include/` is licensed under 0BSD.
- Imported files retain original notices and should not have notices removed.
- Sync path:
  - `scripts/sync_yash_posix_tests.sh` supports syncing from GitHub archive/API
    (`github:magicant/yash@<ref>`) to refresh imported POSIX tests.
