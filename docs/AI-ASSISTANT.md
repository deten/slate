# The AI assistant

Slate can hand a request written in plain words to an AI model and apply what
it suggests. It is not tied to one vendor: you point it at whichever service or
local model you already use.

## What it actually does

The model is not given the drawing database and cannot reach into it. It is
given a short summary — how many of what, the extents, the layers, what is
selected — and the list of commands this build has. It answers with commands,
one per line:

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

## Setting it up

**Tools > AI Assistant…**

| Field | What it is |
| --- | --- |
| API | The request shape. `Anthropic messages`, or the `chat completions` shape OpenAI defined and nearly everything else copied. |
| Endpoint | Where to send it. Change this to use a different service, an aggregator, or a model running on your own machine. |
| Model | The model name that service expects. |
| API key | Your key with that service. |
| Give up after | How long to wait before reporting that nothing came back. |

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

## What is not covered by the tests

Everything either side of the network is tested: the prompt, the reply format,
what is allowed to run, what running it does, undo, and the failure paths.

**The HTTP transport itself is not.** No automated run here contacts a real
provider. Pointing it at a real endpoint with a real key is a manual check, and
the first thing to try if a provider that should work does not.
