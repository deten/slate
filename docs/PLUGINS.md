# Writing a plugin

A plugin adds commands to Slate. A command it adds is indistinguishable
from a built-in one: same command line, same menus, same undo. Nothing in the
engine changes to accommodate it.

The whole interface is one header, `cad/abi/cad_abi.h`. It is pure C, so a
plugin can be written in C, C++, Rust, Zig, D, or anything else with a C FFI.
See `examples/plugins/boltcircle` for a complete, commented example.

## The shape of it

A plugin is a shared library exporting one required entry point:

```c
CAD_PLUGIN_EXPORT int32_t CAD_CALL cad_plugin_init(const CadApi* api,
                                                   CadPluginHost* host);
```

`api` is the host's function table, valid for the plugin's lifetime. `host` is
the registration target. Return `CAD_OK` to stay loaded.

Two optional entry points:

```c
CAD_PLUGIN_EXPORT void CAD_CALL cad_plugin_version(uint32_t* major, uint32_t* minor);
CAD_PLUGIN_EXPORT void CAD_CALL cad_plugin_shutdown(void);
```

Export `cad_plugin_version`. It is how the host can refuse an incompatible
plugin *before* running any of its code, which is the only moment at which
refusing is still safe.

## A command is a state machine

A command alternates between asking for input and receiving it. `on_start`
returns the first request; `on_supply` receives each answer and returns what to
do next.

```c
static CadStepResult CAD_CALL on_start(CadCommandContext* context, void* instance) {
    CadStepResult step;
    memset(&step, 0, sizeof(step));
    step.struct_size = (uint32_t)sizeof(step);
    step.kind = CAD_STEP_REQUEST;
    step.request.struct_size = (uint32_t)sizeof(step.request);
    step.request.kind = CAD_INPUT_POINT;
    step.request.prompt = "Specify first point";
    return step;
}
```

Return `CAD_STEP_DONE` to finish (changes are committed), `CAD_STEP_CANCELLED`
or `CAD_STEP_FAILED` to give up (changes are rolled back). A command that does
its work immediately returns `CAD_STEP_DONE` from `on_start` and never sees
`on_supply`.

Always handle `CAD_VALUE_CANCEL` — the user pressed Escape. Returning another
prompt does not prevent the command from ending; Escape is always an exit.

## Per-invocation state

Set `on_create` and `on_destroy` if the command accumulates anything across
steps. The host calls `on_create` once per *invocation*, so two drawings can run
the same command at once without sharing half-finished input.

```c
static void* CAD_CALL create(void* user_data) { return calloc(1, sizeof(MyState)); }
static void  CAD_CALL destroy(void* instance) { free(instance); }
```

Without them, the descriptor's `user_data` is passed through unchanged, which is
right for a stateless command and wrong for anything else.

## Registration

```c
CadCommandDesc desc;
memset(&desc, 0, sizeof(desc));
desc.struct_size = (uint32_t)sizeof(desc);
desc.id       = "acme.draw.bolt";   /* stable, unique, never shown */
desc.name     = "BOLT";             /* typed at the command line */
desc.title    = "Bolt";             /* shown in menus */
desc.category = "Draw";             /* which menu; "Plugins" if omitted */
desc.on_start = on_start;
desc.on_supply = on_supply;
return api->register_command(host, &desc);
```

Every string is copied, so the descriptor can be a stack local. The function
pointers are retained.

A colliding name or id is refused with `CAD_ERROR_ALREADY_EXISTS` rather than
overwriting. One plugin silently shadowing another's command would be close to
undiagnosable from the drafting side.

## Making geometry

Everything that modifies the drawing needs a transaction, and inside a command
that transaction is the executor's:

```c
CadTransaction* tx = api->context_transaction(context);
api->add_circle(tx, CAD_NULL_HANDLE, center, radius, NULL, &handle);
```

Do not commit or abort it — the call is refused. The executor commits when the
command reports `DONE` and rolls back on `CANCELLED` or `FAILED`, so a command
that gives up halfway cannot leave the drawing half-edited. Everything the
command created undoes as one step.

Outside a command, open your own:

```c
CadTransaction* tx = NULL;
api->begin_transaction(document, "Renumber", &tx);
/* ... */
api->commit_transaction(tx);
```

Pass `CAD_NULL_HANDLE` as the block argument to mean model space, and `NULL`
properties to accept layer 0 and ByLayer.

## Rules that are not optional

**Set `struct_size` on every struct you fill in.** Zero is rejected. It is how
the host knows which fields you actually wrote, and it is what lets version 1.1
append a field without breaking you.

**Never let an exception or panic cross the boundary.** The host guards its own
side, but unwinding out of a C callback is undefined behaviour.

**Strings you return need only survive until the call returns** — the host
copies them. A `char[]` in your state struct is fine; a pointer to a freed
buffer is not.

**Strings the host returns use your buffer.** Call once with a null buffer to
learn the size, then again with one large enough. Nothing is ever truncated:
too small a buffer is `CAD_ERROR_BUFFER_TOO_SMALL`, not a shortened answer.

**Angles are radians, counter-clockwise. Matrices are column-major.** The same
conventions the core and DWG use. Degrees exist only in files and in the UI.

## Building

Link the header and nothing else:

```cmake
add_library(my_plugin MODULE my_plugin.c)
target_link_libraries(my_plugin PRIVATE cad::plugin_sdk)
set_target_properties(my_plugin PROPERTIES PREFIX "")
```

`cad::plugin_sdk` is header-only by design. A plugin that could link `cad_core`
would come to depend on the engine's C++ types, and the boundary would be
decorative.

## Installing

Drop the shared library into `plugins/` beside `Slate.exe`. Plugins are
loaded when a drawing is opened or created, in path order, and each result —
success or failure — is reported in the command history. A plugin that fails to
load says so; silence would look identical to one that loaded and did nothing.

## Versioning

`CAD_ABI_VERSION_MAJOR` changes only for a breaking change, and the host refuses
a plugin whose major version differs. `CAD_ABI_VERSION_MINOR` increments when
functions or fields are appended; a plugin may be older than the host but not
newer.

Within a major version: existing functions keep their signature and meaning, new
functions are appended to `CadApi` and nowhere else, new struct fields are
appended and guarded by `struct_size`, and enumerators are only added.
