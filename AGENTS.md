# AGENTS.md

## Safety & Repository Guardrails

- **Fork-only remotes**: `metske` points to `Metske/tesserae-device-firmware`; `origin` points to upstream `dmellok/tesserae-device-firmware`. Push branches, tags, PRs, and GitHub Issues **only** to `Metske/tesserae-device-firmware` (`git push metske ...`, `gh ... -R Metske/tesserae-device-firmware`).
- **Pi test isolation**: Never modify or push to production device `reterminal_e1003_750004` or rotation `monochrome_gallery_rotation` on `tesserae-pi`. Use test device `reterminal_e1003_qemu01` and deck `qemu_e1003_deck` for simulator/hardware testing.
- **E1003 QEMU simulator**: Run automated regression & latency checks via WSL `xteink-firmware-screenshots` (`capture.py --target tesserae-e1003 --suite tesserae`) against `-machine e1003` (`Metske/eink-qemu` branch `ext/e1003`).

## Agent skills

### Issue tracker

GitHub Issues on `Metske/tesserae-device-firmware` via `gh` CLI. See `docs/agents/issue-tracker.md`.

### Triage labels

Default five-role vocabulary (`needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`). See `docs/agents/triage-labels.md`.

### Domain docs

Single-context layout (`GLOSSARY.md` and `docs/adr/` at the repo root). See `docs/agents/domain.md`.
