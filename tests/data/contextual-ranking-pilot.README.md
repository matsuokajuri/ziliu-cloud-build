# Synthetic contextual-ranking fixture

The 60 examples in `contextual-ranking-pilot.json` are repository-authored
synthetic development diagnostics, licensed with the project. They do not contain
personal typing history or copied user documents.

There are 51 contextual cases, six empty-context controls and three misleading-
context controls. Related full/prefix/initials cases are correlated examples,
not independent observations. `expected` labels are development annotations,
not exhaustive ground truth; empty labels are neutral controls.

Rime receives only the ASCII `input` field. `context` is reserved for offline
ranking experiments. The fixture is not a final benchmark or product acceptance
set. `contextual-ranking-pilot.annotations.json` records two label exclusions
without changing the frozen input. Tests verify the input hash and ensure labels
are not supplied to the ranking implementation.
