# Prompt for a gpuic session: an empty cached view and UBSan

Status: **draft for your review**, not sent. Written to be pasted into a Claude Code session whose working directory is the gpuic
repository, against gpuic `da89032`. It is self-contained: that session has not seen PulseRack. Everything inside the fence is the
prompt; the notes after it are for you.

---

```text
I'm building a virtual modular synthesizer ("PulseRack") with gpuic's C++ layer, against your commit da89032. Running my window
tests built with clang -fsanitize=address,undefined (UBSAN_OPTIONS=halt_on_error=1), every run stops on the same report:

    src/paint.c:983:12: runtime error: null pointer passed as argument 1, which is declared to never be null
    /usr/include/string.h:48:28: note: nonnull attribute specified here

Nothing else is reported: with halt_on_error=0 all my checks pass and ASan finds nothing. It is harmless in practice, but it
stops every UBSan run of a program that uses cached views this way, so please fix it.

## What I think happens (confirm before changing anything)

At the end of the function in src/paint.c that paints a cached view and keeps its paint for reuse (around lines 970-990):

    size_t count = pc->scene->len - start;
    if (count > vv->cap_chunk) { ... realloc vv->chunk ... }
    memcpy(vv->chunk, pc->scene->items + start, count * sizeof *vv->chunk);

When a cached view paints nothing, count is 0. If that view has never painted anything, vv->cap_chunk is 0, the realloc is
skipped, and vv->chunk is still NULL, so this is memcpy(NULL, ..., 0). C makes that undefined even for a length of 0 (memcpy's
arguments are declared nonnull), and UBSan reports it. The source can be NULL too: if nothing at all has been painted into the
scene yet, pc->scene->items + start is NULL + 0, which C also leaves undefined.

How a program gets there: a cached child view whose render returns an element that draws nothing. Mine are an overlay that shows
the cable being dragged and returns an empty div() when nothing is dragged, and a cable's view while its cable is lifted off.
Both are ordinary uses of gp_child_view_cached.

## What I'd like

1. Skip the copy when there is nothing to copy (for example, `if (count) memcpy(...)`), keeping nchunk, chunk_sig, chunk_x/y,
   chunk_gen and chunk_valid exactly as now, so an empty view is still cached and reused as empty.
2. A regression test that fails today under -fsanitize=undefined: a window with a cached child view whose render draws nothing
   (an empty div), drawn a few times, plus a view that changes between drawing something and drawing nothing. It should also
   check that the empty view is reused (paint_hits grows: gp_app_stats has it) and that switching back to drawing something
   paints it.
3. Since you are there, a quick look for the same pattern elsewhere: a copy or memset whose length can be 0 while a pointer may
   not have been allocated yet. I looked at paint.c, views.c and app.c and found only this one (app.c:26 already guards
   `if (init && size)`), but I did not read the rest.

If your sanitizer CI does not build with clang and -fsanitize=undefined, it would catch this class of problem; say whether you
want to add that. Keep all your tests passing, and say in the commit message what changed.
```

---

## Notes for you

* **Why it matters to PulseRack.** It is not a crash: `memcpy` of 0 bytes does nothing on every real platform. But it stops the
  clang ASan/UBSan run of `ui_smoke` and `ui_screenshot` at the first frame, and that run is part of how each milestone is checked
  (`gui.md`, *As built (milestone 3)*). Until it is fixed the window tests can only be run under UBSan with `halt_on_error=0`.
* **What I checked.** The report is the only one in a full `ui_smoke` run (105 checks pass) and in opening
  `examples/sequence.rack`; it appeared with milestone 3, which brought the first cached views that can be empty. In gpuic I read
  the function around `paint.c:983` and grepped `paint.c`, `views.c` and `app.c` for `memcpy`/`memmove`: the other copies have
  fixed sizes or a guard.
* **Not worked around in PulseRack.** Making the empty views draw something invisible would hide the report without fixing
  anything, and would leave the bug for the next program.
* **When it lands**, the clang ASan/UBSan run goes back to `halt_on_error=1` for all 7 tests, and `gui.md` drops the caveat.
