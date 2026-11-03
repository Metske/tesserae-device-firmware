# Issue tracker: GitHub (`Metske/tesserae-device-firmware`)

Issues and specs for this repo live as GitHub issues on the fork **`Metske/tesserae-device-firmware`**. Use the `gh` CLI for all operations and pass `-R Metske/tesserae-device-firmware` (or verify `gh repo view --json nameWithOwner` resolves to `Metske/tesserae-device-firmware`) so issues are **never** filed against upstream `dmellok/tesserae-device-firmware`.

## Conventions

- **Create an issue**: `gh issue create -R Metske/tesserae-device-firmware --title "..." --body "..."`. Use a heredoc or `--body-file` for multi-line bodies.
- **Read an issue**: `gh issue view <number> -R Metske/tesserae-device-firmware --json number,title,body,labels,comments`.
- **List issues**: `gh issue list -R Metske/tesserae-device-firmware --state open --json number,title,body,labels,comments --jq '[.[] | {number, title, body, labels: [.labels[].name], comments: [.comments[].body]}]'` with appropriate `--label` and `--state` filters.
- **Make an issue a sub-issue of a parent**: `gh issue create -R Metske/tesserae-device-firmware --parent <parent> ...`, or `gh issue edit <parent> -R Metske/tesserae-device-firmware --add-sub-issue <child>` afterwards (`gh` 2.94+). Older `gh`: `gh api --method POST repos/Metske/tesserae-device-firmware/issues/<parent>/sub_issues -F sub_issue_id=<child-db-id>`. Without sub-issues, put `Part of #<parent>` at the top of the child body.
- **Blocking**: GitHub's **native issue dependencies**, the canonical, UI-visible representation. Add an edge with `gh issue create -R Metske/tesserae-device-firmware --blocked-by <n>,<n> ...` or `gh issue edit <child> -R Metske/tesserae-device-firmware --add-blocked-by <blocker>` (`gh` 2.94+). Older `gh`: `gh api --method POST repos/Metske/tesserae-device-firmware/issues/<child>/dependencies/blocked_by -F issue_id=<blocker-db-id>`, where `<blocker-db-id>` is the blocker's numeric **database id** (`gh api repos/Metske/tesserae-device-firmware/issues/<n> --jq .id`). Where dependencies aren't available, fall back to a `Blocked by: #<n>, #<n>` line at the top of the child body. A ticket is unblocked when every blocker is closed.
- **Count open blockers**: `gh api repos/Metske/tesserae-device-firmware/issues/<n> --jq .issue_dependencies_summary.blocked_by` (open blockers only; 0 means startable).
- **Comment on an issue**: `gh issue comment <number> -R Metske/tesserae-device-firmware --body "..."`
- **Apply / remove labels**: `gh issue edit <number> -R Metske/tesserae-device-firmware --add-label "..."` / `--remove-label "..."`
- **Close**: post the explanation with `gh issue comment <number> -R Metske/tesserae-device-firmware --body "..."`, then `gh issue close <number> -R Metske/tesserae-device-firmware`.

File issues **only** into `Metske/tesserae-device-firmware` — never into `dmellok/tesserae-device-firmware` or `mattpocock/skills`.

## Pull requests as a triage surface

**PRs as a request surface: no.** _(Set to `yes` if this repo treats external PRs as feature requests; `/triage` reads this flag.)_

## When a skill says "publish to the issue tracker"

Create a GitHub issue on `Metske/tesserae-device-firmware`.

## When a skill says "fetch the relevant ticket"

Read it as in **Read an issue** above.

## Wayfinding operations

Used by `/wayfinder`. The **map** is a single issue with **child** issues as tickets.

- **Map**: a single issue labelled `wayfinder:map`, holding the Notes / Decisions-so-far / Fog body. `gh issue create -R Metske/tesserae-device-firmware --label wayfinder:map`.
- **Child ticket**: an issue linked to the map as a GitHub sub-issue (see **Make an issue a sub-issue of a parent**). Labels: `wayfinder:<type>` (`research`/`prototype`/`grilling`/`task`). Once claimed, the ticket is assigned to the driving dev.
- **Blocking**: see **Blocking** under Conventions.
- **Frontier query**: list the map's open children (`gh issue list -R Metske/tesserae-device-firmware --state open`), drop any with an open blocker or an assignee; first in map order wins.
- **Claim**: `gh issue edit <n> -R Metske/tesserae-device-firmware --add-assignee @me`, the session's first write.
- **Resolve**: `gh issue comment <n> -R Metske/tesserae-device-firmware --body "<answer>"`, then `gh issue close <n> -R Metske/tesserae-device-firmware`, then append a context pointer (gist + link) to the map's Decisions-so-far.
