## Incremental partial-save algorithm

This document describes the partial stack-saving algorithm used by libtealet.


## Core state and terms

- `g_prev` is the list of partially saved stacks.
- `g_prev` is newest-first (head is most recently linked partial stack).
- Each saved stack carries:
	- `stack_near`: saved edge nearest the active stack pointer.
	- `stack_far`: far boundary for that tealet.
	- `saved`: bytes currently copied to heap.

Descending-stack terminology used here:
- lower address = nearer.
- higher address = farther.


## What happens on a switch

When switching from current stack `C` to target tealet `T`:

1. Set `target_stop = T.stack_far`.
2. Grow existing partial stacks in `g_prev` with `target_stop`:
	 - iterate newest to older,
	 - for each stack, attempt to increase saved bytes up to `target_stop`,
	 - unlink any stack that becomes fully saved,
	 - if the iteration reaches target stack, unlink it and stop.
3. Save current stack `C` up to `target_stop`:
	 - if fully saved, do not link to `g_prev`,
	 - if partially saved, link as new head of `g_prev`.

Important property:
- grow is monotone. A stack can only increase `saved`; it never shrinks.


## Save boundary convention

The copied interval endpoint uses `saveto`:
- descending stacks: copied bytes are `[stack_near, saveto)`
- ascending stacks: copied bytes are `[saveto, stack_near)`

For descending stacks, lowering `saveto` requests fewer bytes, so it cannot
force additional growth of an already partially saved older stack.


## List order model (descending stack)

Most recent partial stack is at front:

```
g_prev
	|
	v
[S_newest] -> [S_older] -> [S_oldest] -> NULL
	 far=F3       far=F2      far=F1

Typical descending staircase:
	F3 < F2 < F1
```


## Address view for disjoint unsaved bands

For descending stacks with staircase boundaries `F3 < F2 < F1`, unsaved bands
for partial stacks partition memory into non-overlapping windows.

```
low addr                                                     high addr
|---------------------------------------------------------------|

S_newest: [near3 --------------------------- far3]
S_older : [near2 --------------------------- far3 ===== far2]
S_oldest: [near1 --------------------------- far3 ----- far2 ===== far1]

Legend:
	- `-` = region already saved for that stack
	- `=` = that stack's unsaved band
```

Partition bands in this example:
- `[F3, F2)`
- `[F2, F1)`


## Worked transition: staircase down, then middle switch

Assume descending stack and `F3 < F2 < F1`.

Step 0: run at `F1`.

```
g_prev: NULL
```

Step 1: switch to target with `far=F2`.

```
g_prev: [S(F1)]
S(F1) unsaved:               [F2 ===== F1)
```

Step 2: switch to target with `far=F3`.

```
g_prev before save current:  [S(F1)]
g_prev after save current :  [S(F2)] -> [S(F1)]

S(F2) unsaved:               [F3 ===== F2)
S(F1) unsaved:                       [F2 ===== F1)
```

Why `S(F1)` stays `[F2, F1)`:
- lowering `saveto` from `F2` to `F3` does not require growth,
- and growth is the only operation that changes saved extent.

Step 3: switch back to middle target `far=F2`.

```
before: g_prev = [S(F2)] -> [S(F1)]
after : g_prev = [S(F1)]
```

Effect summary:
- descending to lower far extends the staircase with a new head band,
- switching up can prune list entries that are no longer partial for that
	switch,
- remaining list order stays newest-first.


## Implication for sharing saved stack data between Tealets

Under this algorithm, unsaved memory for partial stacks is naturally expressed
as disjoint staircase bands in the descending case.  This means that it is not
possible to share saved chunks between different stacks.  Each additionally
saved chunk is unique to a particular suspended Tealet.


