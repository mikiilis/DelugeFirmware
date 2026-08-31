# Agent instructions

## Living architecture doc

`ARCHITECTURE.md` at the repo root is the permanent dev-context file for the
Loop Station work. Before doing Loop Station work, read it first instead of
re-deriving context.

When any of the following change, update `ARCHITECTURE.md` in the same commit:
- a design decision changes (append to the Decision log)
- a component moves or a new one is added to the feature
- the status of an open item changes (check it off, update Status)
- next steps change

Deep invariants belong in `src/deluge/loop_station/README.md`, not the root doc.
Keep the root doc short and operational; it must stay readable in one screen.
