# The AI assistant

Slate can hand a request written in plain words to an AI model and apply what
it suggests. It is not tied to one vendor: you point it at whichever service or
local model you already use.

## What it actually does

The model is not given the drawing database and cannot reach into it. It is
given a digest: the units, how many of what, the extents, each layer with its
colour and linetype and what sits on it, the blocks and their attributes, the
styles, the sheets, and each selected object in a line. With a reference
folder set (below) it is also told the office's standards. And it gets the
list of commands this build has. It answers with commands, one per line:

```
RECTANGLE 0,0 400,300
CIRCLE 200,150 25
```

Those commands run through exactly the same path as commands you type. Three
things follow from that, and they are the reason it is built this way:

- **It can only do what you could have done by hand.** There is no second route
  into the drawing for it to take.
- **You see precisely what will happen before it happens.** The steps are shown
  and nothing is applied until you agree.
- **Everything it does undoes.** Each step is an ordinary command, so Ctrl+Z
  takes them back one at a time.

A command this build does not have is refused and reported rather than guessed
at. A model inventing a plausible-sounding command is the usual failure, and
running its arguments through whatever else happened to match would change the
drawing in a way nobody asked for while still reading as though it worked.

## What Slate reads before asking

A digest of counts cannot tell a model which room is the corridor. Before a
request goes out, Slate reads the drawing as the things a person would name,
and passes that along too:

- **Rooms.** Every label set in capitals that reads as a name ("OFFICE",
  "CONF. #1", "STAFF WORK AREA") with the room number beside it, and, where
  the walls around the label close, the room's area, size and centre. Doors
  and windows count as closing, since their leaf and swing sit in the gap. A
  label whose walls do not close is still reported, marked not enclosed.
- **Systems by layer.** Layers named for ducts, pipes, fittings, terminals or
  equipment, with what is on them: single-line runs drawn with a width, with
  their widths and total length, lines, circles, the blocks placed, and the
  labels used. The name also says whether the layer is supply, return or
  exhaust, and whether it is new, existing or demolition work.
- **The shell.** Which layers hold walls, how many doors and windows, and
  which blocks they are.
- **The title block.** The block with the most attributes on each sheet,
  and its fields.

Frozen and switched-off layers are read, and so is everything a resolved
external reference brings in, placed where the reference is placed: a
mechanical plan nearly always references the architecture, and the rooms are
in the reference. Nothing is read from a reference that could not be found.

**DESCRIBE** prints the same reading in the history, so what the assistant
will be told can be checked before anything is asked of it. A room it did
not find gets no duct; a room it found in the wrong place gets one there.
Tracing walls stops after two seconds on a very large plan and the rest of
the rooms are reported by their labels.

## Setting it up

**Tools > AI Assistant…**

| Field | What it is |
| --- | --- |
| API | The request shape. `Anthropic messages`, or the `chat completions` shape OpenAI defined and nearly everything else copied. |
| Endpoint | Where to send it. Change this to use a different service, an aggregator, or a model running on your own machine. |
| Model | The model name that service expects. |
| API key | Your key with that service. |
| Give up after | How long to wait before reporting that nothing came back. |
| Reference folder | A folder of your own drawings, which the assistant learns your conventions from. |

### The reference folder

One drawing cannot tell a model which layer a wall goes on, what your door
block is called, or what a dimension should look like. The drawings you have
already made can. Point the reference folder at them and Slate reads every DWG
and DXF in it, subfolders included, and boils them down to what they agree on:

- each layer, with its usual colour and linetype, how many drawings have it
  and how much sits on it
- each block, how often it is placed, and the attributes it asks for
- the text and dimension styles, the units, and the sheet sizes

That summary goes into every request as "house standards", and the model is
told to follow them: to put each kind of object on the layer you use for it,
to reuse your block names, and to make a standard layer if the open drawing
lacks it.

Any `.txt` or `.md` file in the folder is passed along as written. That is
the place to say what the drawings cannot: which layer is for what, what the
office does and does not do, what a client wants. Keep it short.

The folder is read once and the result kept in your settings folder. It is
read again only when a file in it changes. The **Show** button reads it and
shows exactly what the assistant will be told, so you can check what it has
understood before trusting it. The first hundred drawings are read; if the
folder is bigger than that, make a smaller one.

The reference folder is read, never written. Nothing the assistant does
touches the drawings in it.

Because the chat-completions shape is so widely copied, "some service Slate has
never heard of" and "a model running locally" are both handled by typing the
address in — there is nothing to add to Slate itself.

### Where the key is kept

Typed into the dialog, it is stored **in plain text** in your settings file
(`slate.ini`). That is worth knowing before you hand the folder to anybody.

To keep it off disk entirely, set the environment variable instead:

```
set SLATE_AGENT_KEY=your-key-here
```

When that variable is set it is used, the dialog's key box is disabled, and
nothing is written to the settings file. Keys are also stripped out of error
messages before they reach the command history, because some providers quote
the offending key back at you in the error body.

## From the command line

The same path, without the window:

```
Slate.exe drawing.dxf --agent "add a 900mm door opening in the north wall"
```

And for scripting or testing, answer from a file instead of a provider — no
key, no network:

```
Slate.exe --agent-reply reply.txt --agent "draw a room" --save-as out.dxf
```

`reply.txt` holds whatever a model would have said. This is how the automated
tests cover the whole path: `tools/interaction_tests.py` drives the real
application through it and checks the geometry that comes out.

To see what the assistant would be told without asking it anything:

```
Slate.exe drawing.dxf --agent-context
Slate.exe drawing.dxf --agent-reference "D:\Projects\Standards" --agent-context
```

The first prints the digest of the drawing. The second also reads the given
folder as the reference folder for that run, whatever the settings say.

## What is not covered by the tests

Everything either side of the network is tested: the prompt, the reply format,
what is allowed to run, what running it does, undo, and the failure paths.

**The HTTP transport itself is not.** No automated run here contacts a real
provider. Pointing it at a real endpoint with a real key is a manual check, and
the first thing to try if a provider that should work does not.
