# ADR 0007 — Copying between drawings matches symbol records by name, and carries a DXF fragment on the clipboard

**Status:** Accepted
**Date:** 2026-08-31

## Context

An entity is not self-contained. A line names its layer by *handle*, and that
handle means nothing outside the database it was allocated in. The same is true
of its linetype, of a text entity's style, of a dimension's dimension style, and
of a block reference's definition. Copying geometry alone between two drawings
produces objects on the wrong layer, in the wrong colour, at the wrong text
height, or referring to a block that does not exist — a paste that looks like it
worked and is wrong in every way that matters on a drawing set.

So cut, copy and paste needed three decisions that are not obvious, and that
would be expensive to change once drawings had been edited under them.

Two constraints frame all three. `cad_core` links nothing but the C++ standard
library (ADR 0002), so it cannot touch the system clipboard. And the clipboard
is shared with every other process on the machine, so whatever crosses it has to
survive leaving this application's address space entirely.

## Decision

**1. The clipboard carries a DXF fragment.**

A copy is extracted into a complete, standalone `Database` — the geometry plus
every symbol record it depends on — and that database is written as ASCII DXF
onto the clipboard under a private MIME type, `application/x-slate-dxf`.

The alternative, a private binary struct dump, is faster to produce and cheaper
to parse. It is also a second serialisation format to keep in step with the
first: every new entity field would have to be added to the DXF writer *and* to
the clipboard format, and the day they disagreed the symptom would be geometry
that changes when it is pasted. Reusing DXF means the clipboard is exercised by
the same round-trip tests as saving, is inspectable when something goes wrong,
and works between two separately running copies of the application without any
shared memory layout.

A private MIME type rather than `text/plain`: a DXF fragment pasted into a text
editor is noise, and a text editor's contents pasted into a drawing are not
geometry.

**2. Symbol records are matched by name, and the destination always wins.**

Pasting onto a drawing that already has a layer called "Walls" puts the geometry
on *that* layer and does not touch its colour, linetype or state. A layer, style
or block definition is created only when the destination has nothing by that
name.

This is what a drafter means by paste. The alternative — importing a second
"Walls" under a generated name — is how a drawing acquires forty layers that
differ only by suffix, and it defeats the reason drawing offices agree on layer
names at all. The cost is real and accepted: a block named "TB" that draws
something different in the two drawings will paste as the *host's* TB. That is
still the right answer, because redefining the host's block would silently
change every other reference to it on the sheet, which is far worse than one
reference drawing the wrong thing. The command says so when it happens.

**3. A reference to something that did not come along is cleared, not followed.**

An associative hatch names the geometry it was drawn against. A field names the
object whose property it reports. Following those references would silently drag
objects the user never selected into the copy — select one hatch, paste half the
floor plan. So the transfer copies the closure of what an entity *owns* (a block
reference's attributes, a block definition's contents) and drops references to
anything outside the selection.

The hatch keeps its shape and loses its association; the field keeps its cached
value, which is already the defined behaviour for a field that cannot be
evaluated. other CAD software drops the association here too. The count of cleared
references is reported rather than hidden.

Two references are always cleared: a dimension's and a table's anonymous drawn
block. Both are kept but never drawn (ADR 0005), both belong to the source
file's handle space, and both are regenerated wherever the object lands.

## Consequences

The engine gained `cad::extractEntities` and `cad::pasteContent`
(`cad/db/Transfer.hpp`), which know nothing about clipboards. They are the same
operation that block insertion, WBLOCK and binding an xref need, and
`XrefLoader` already contains a narrower hand-rolled version of it that should
eventually be replaced by this one.

The shell gained `COPYCLIP`, `CUTCLIP`, `PASTECLIP` and `PASTEORIG`
(`src/app/ClipboardCommands.cpp`), registered through the same extension point a
plugin uses, for the reason ADR 0002 gives for `ZOOM` living there.

Because the fragment is a whole database, it records its own base point in
`$INSBASE` and its own units in `$INSUNITS`. A paste therefore knows where to
measure from without being told, and content copied from a drawing in
millimetres arrives correctly scaled in a drawing measured in metres.

`PASTECLIP` is offered only when the clipboard actually holds one of these
fragments, so the menu item is greyed out rather than failing when triggered.
