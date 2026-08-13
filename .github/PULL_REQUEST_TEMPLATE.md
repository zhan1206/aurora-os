# FIXED (v4.4.3): P0-0.2 — PR template with selftest + CHANGELOG enforcement

## Description
<!-- Briefly describe the changes -->

## Checklist

### Build
- [ ] `make clean && make` produces 0 errors, 0 warnings
- [ ] `make iso` produces bootable os.iso

### Selftest
- [ ] `kernel_selftest()` runs and all tests pass
- [ ] Selftest PASS/FAIL counts match expectations:
  - [ ] If new tests added: updated test count in README.md badge
  - [ ] If tests fixed: confirmed the fix with a regression test

### CHANGELOG
- [ ] CHANGELOG.md updated with this version's changes
- [ ] Changes are categorized (Fix/Feature/Docs/Build/Test)
- [ ] No false claims (e.g., "已修复" when actually not fixed)
- [ ] `scripts/audit_changelog.sh` passes

### QA Scripts
- [ ] `bash scripts/check_nested_comments.sh` passes
- [ ] `bash scripts/check_header_includes.sh` passes
- [ ] `bash scripts/check_static_vs_header.sh` passes
- [ ] `bash scripts/audit_changelog.sh` passes

### Documentation
- [ ] LIMITATIONS.md updated if any limitation was fixed or discovered
- [ ] README.md badges updated if version/test count changed

## Related Issues
<!-- Link to issues/PRs -->

## Test Results
```
<!-- Paste selftest output here -->
```