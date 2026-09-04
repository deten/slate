/*
 * Slate plugin ABI.
 *
 * A stable, versioned, C-compatible boundary around the native C++ core.
 *
 * ============================ Why it looks like this =======================
 *
 * The core is modern C++, but nothing about that may cross this line. A C++
 * interface would tie every plugin to one compiler, one standard library and
 * one build configuration, and break on any of them changing. So:
 *
 *   - Only C types cross. No classes, no templates, no std::, no exceptions.
 *   - Objects are opaque handles. A plugin never sees a layout it could
 *     depend on, so the core is free to change internally.
 *   - Errors are return codes. Exceptions cannot cross a C boundary safely,
 *     and a plugin written in Rust or C# has no way to catch one.
 *   - Functions arrive in a versioned table rather than as linked exports.
 *     Adding a function extends the table; a host can serve an older plugin
 *     by honouring the size it was compiled against.
 *   - Fixed-width integers everywhere, and int32_t rather than bool, because
 *     _Bool's size is not fixed across the languages that will bind to this.
 *   - Strings are UTF-8, and no allocation ever crosses. Strings the host
 *     returns go into a caller-supplied buffer; strings a plugin returns are
 *     copied by the host before the call returns. Neither side ever frees the
 *     other's memory, which is the only arrangement that survives the two
 *     sides having been built with different allocators.
 *
 * A command registered through this header is indistinguishable from a
 * built-in one: same registry, same executor, same undo behaviour. That is the
 * requirement, not a convenience — "no modification of the core engine should
 * be required simply to add a new command".
 *
 * ================================ Writing one ==============================
 *
 *   #include <cad/abi/cad_abi.h>
 *
 *   static CadStepResult CAD_CALL on_start(CadCommandContext* ctx, void* user) {
 *       CadStepResult step = {0};
 *       step.struct_size = (uint32_t)sizeof(step);
 *       step.kind = CAD_STEP_REQUEST;
 *       step.request.struct_size = (uint32_t)sizeof(step.request);
 *       step.request.kind = CAD_INPUT_POINT;
 *       step.request.prompt = "Specify centre point";
 *       return step;
 *   }
 *
 *   CAD_PLUGIN_EXPORT int32_t CAD_CALL cad_plugin_init(const CadApi* api,
 *                                                      CadPluginHost* host) {
 *       CadCommandDesc desc = {0};
 *       desc.struct_size = (uint32_t)sizeof(desc);
 *       desc.id    = "acme.draw.bolt";
 *       desc.name  = "BOLT";
 *       desc.title = "Bolt";
 *       desc.category = "Draw";
 *       desc.on_start = on_start;
 *       desc.on_supply = on_supply;
 *       return api->register_command(host, &desc);
 *   }
 *
 * A complete, commented example lives in `examples/plugins/boltcircle`.
 *
 * ================================ Stability ================================
 *
 * Within a major version:
 *   - existing functions keep their signature and meaning;
 *   - new functions are appended to CadApi and nowhere else;
 *   - new struct fields are appended, guarded by struct_size;
 *   - enumerators are only added, never renumbered.
 *
 * A plugin must set struct_size on every struct it fills in, and should check
 * api->struct_size before calling a function added after its own version.
 *
 * struct_size is mandatory, and zero is rejected rather than assumed. Guessing
 * a layout from a missing size is exactly the mistake the field exists to
 * prevent: it would work today and read past the end of the struct the first
 * time a field is appended.
 */

#ifndef CAD_ABI_H
#define CAD_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ export */

#if defined(_WIN32)
#  define CAD_PLUGIN_EXPORT __declspec(dllexport)
#  define CAD_CALL __cdecl
#else
#  define CAD_PLUGIN_EXPORT __attribute__((visibility("default")))
#  define CAD_CALL
#endif

/* ----------------------------------------------------------------- version */

/* Incremented only for a breaking change. A host refuses a plugin built
 * against a different major version, because nothing else can be guaranteed. */
#define CAD_ABI_VERSION_MAJOR 1

/* Incremented when functions or fields are appended. A host accepts a plugin
 * with a lower minor version; a plugin must not call past what it was
 * compiled against. */
#define CAD_ABI_VERSION_MINOR 0

/* ------------------------------------------------------------------ status */

typedef enum CadStatus {
    CAD_OK = 0,
    CAD_ERROR_INVALID_ARGUMENT = 1,
    CAD_ERROR_NOT_FOUND = 2,
    /* A name or id is already taken. Registration never silently overwrites:
     * one plugin shadowing another's command invisibly is worse than a clear
     * failure. */
    CAD_ERROR_ALREADY_EXISTS = 3,
    /* The call needs an open transaction and there is none, or vice versa. */
    CAD_ERROR_NO_TRANSACTION = 4,
    /* The operation is understood but not permitted right now — editing a
     * locked layer, for instance. */
    CAD_ERROR_NOT_PERMITTED = 5,
    /* The host's ABI is too old to satisfy the request. */
    CAD_ERROR_UNSUPPORTED_VERSION = 6,
    /* A caller-supplied buffer was too small; the required size is reported. */
    CAD_ERROR_BUFFER_TOO_SMALL = 7,
    /* The core threw. The exception never crosses the boundary; this does. */
    CAD_ERROR_INTERNAL = 8
} CadStatus;

/* ----------------------------------------------------------------- handles */

/* Opaque. A plugin holds these but can never inspect them, so the core's
 * layout stays free to change. */
typedef struct CadPluginHost CadPluginHost;
typedef struct CadDocument CadDocument;
typedef struct CadTransaction CadTransaction;
typedef struct CadCommandContext CadCommandContext;

/* An object's permanent identity, matching the core's handle exactly. Passed
 * by value because it is just an integer, and 0 is always "nothing". */
typedef uint64_t CadHandle;
#define CAD_NULL_HANDLE ((CadHandle)0)

/* --------------------------------------------------------------- geometry */

typedef struct CadPoint {
    double x;
    double y;
    double z;
} CadPoint;

typedef struct CadBox {
    CadPoint min;
    CadPoint max;
} CadBox;

/* ------------------------------------------------------------------ colour */

typedef enum CadColorMethod {
    CAD_COLOR_BY_LAYER = 0,
    CAD_COLOR_BY_BLOCK = 1,
    CAD_COLOR_INDEX = 2,
    CAD_COLOR_TRUE = 3
} CadColorMethod;

typedef struct CadColor {
    int32_t method; /* CadColorMethod */
    int32_t index;  /* ACI, when method is CAD_COLOR_INDEX */
    uint32_t rgb;   /* 0x00RRGGBB, when method is CAD_COLOR_TRUE */
} CadColor;

/* ------------------------------------------------------------- entity data */

typedef enum CadEntityKind {
    CAD_ENTITY_UNKNOWN = 0,
    CAD_ENTITY_LINE = 1,
    CAD_ENTITY_CIRCLE = 2,
    CAD_ENTITY_ARC = 3,
    CAD_ENTITY_ELLIPSE = 4,
    CAD_ENTITY_POLYLINE = 5,
    CAD_ENTITY_POINT = 6,
    CAD_ENTITY_TEXT = 7,
    CAD_ENTITY_MTEXT = 8,
    CAD_ENTITY_INSERT = 9
} CadEntityKind;

/* One polyline vertex. `bulge` is tan(included angle / 4), the DWG
 * representation: 0 is a straight span, positive sweeps counter-clockwise. */
typedef struct CadVertex {
    CadPoint position;
    double bulge;
    double start_width;
    double end_width;
} CadVertex;

/* Properties shared by every entity. Zeroing the struct and setting only
 * struct_size gives sensible defaults: layer 0, ByLayer colour. */
typedef struct CadEntityProperties {
    uint32_t struct_size;
    CadHandle layer;      /* 0 means layer "0" */
    CadColor color;
    CadHandle linetype;   /* 0 means ByLayer */
    double linetype_scale;
    int32_t lineweight;   /* hundredths of a mm; negative values are sentinels */
    int32_t visible;      /* int32_t, not bool: _Bool's size is not portable */
} CadEntityProperties;

/* ------------------------------------------------------------ command input */

typedef enum CadInputKind {
    CAD_INPUT_POINT = 0,
    CAD_INPUT_DISTANCE = 1,
    CAD_INPUT_ANGLE = 2,
    CAD_INPUT_NUMBER = 3,
    CAD_INPUT_TEXT = 4,
    CAD_INPUT_SELECTION = 5,
    CAD_INPUT_ENTITY = 6,
    CAD_INPUT_KEYWORD = 7
} CadInputKind;

typedef enum CadInputValueKind {
    CAD_VALUE_POINT = 0,
    CAD_VALUE_NUMBER = 1,
    CAD_VALUE_TEXT = 2,
    CAD_VALUE_SELECTION = 3,
    CAD_VALUE_ENTITY = 4,
    CAD_VALUE_KEYWORD = 5,
    /* The user pressed Enter with nothing typed. */
    CAD_VALUE_EMPTY = 6,
    /* The user pressed Escape. A command may still finish normally in
     * response, keeping whatever it has already committed. */
    CAD_VALUE_CANCEL = 7
} CadInputValueKind;

/* An alternative the user may type instead of picking, e.g. "Close" in LINE.
 * `shortcut` is the capitalized prefix, which is all the user needs to type. */
typedef struct CadKeyword {
    const char* name;
    const char* shortcut;
    const char* description; /* may be NULL */
} CadKeyword;

/* What a command is asking for. Zero-initialize, then set what applies.
 *
 * Every string and array referenced here need only stay valid until the
 * callback returns; the host copies what it keeps. A prompt built into a local
 * buffer is therefore fine, as long as the buffer outlives the `return`. */
typedef struct CadInputRequest {
    uint32_t struct_size;
    int32_t kind;          /* CadInputKind */
    const char* prompt;    /* UTF-8 */

    /* Rubber-band origin. Also what PERPENDICULAR and TANGENT snap against,
     * so setting it makes those snaps work. */
    int32_t has_base_point;
    CadPoint base_point;

    const CadKeyword* keywords; /* may be NULL */
    uint32_t keyword_count;

    int32_t has_default_number;
    double default_number;

    /* Allow Enter to finish here rather than requiring input. */
    int32_t allow_empty;
} CadInputRequest;

/* A value supplied in response. */
typedef struct CadInputValue {
    uint32_t struct_size;
    int32_t kind;            /* CadInputValueKind */
    CadPoint point;
    double number;
    const char* text;        /* UTF-8; valid only during the call */
    const CadHandle* selection;
    uint32_t selection_count;
    CadHandle entity;
} CadInputValue;

typedef enum CadStepKind {
    /* Ask for more input. */
    CAD_STEP_REQUEST = 0,
    /* Finished successfully; changes are committed. */
    CAD_STEP_DONE = 1,
    /* Cancelled; every change is rolled back. */
    CAD_STEP_CANCELLED = 2,
    /* Failed; every change is rolled back and the message is shown. */
    CAD_STEP_FAILED = 3
} CadStepKind;

/* What a command step returns. */
typedef struct CadStepResult {
    uint32_t struct_size;
    int32_t kind;               /* CadStepKind */
    CadInputRequest request;    /* when kind is CAD_STEP_REQUEST */
    const char* message;        /* may be NULL; copied by the host */
} CadStepResult;

/* --------------------------------------------------------------- callbacks */

/* Begin. Return the first prompt, or complete immediately. */
typedef CadStepResult(CAD_CALL* CadCommandStartFn)(CadCommandContext* context,
                                                   void* user_data);

/* Handle one input and return what to do next. */
typedef CadStepResult(CAD_CALL* CadCommandSupplyFn)(CadCommandContext* context,
                                                    const CadInputValue* input,
                                                    void* user_data);

/* Called when the command ends for any reason. Cleanup only; the transaction
 * is already handled. May be NULL. */
typedef void(CAD_CALL* CadCommandCleanupFn)(CadCommandContext* context, void* user_data);

/* Called once per command *invocation* to make per-run state, and once to
 * destroy it. Both may be NULL for a stateless command; when they are, the
 * user_data from the descriptor is passed through unchanged.
 *
 * A command that keeps state across steps — LINE accumulating points — needs
 * these, because one descriptor may be running in several documents at once. */
typedef void*(CAD_CALL* CadCommandCreateFn)(void* user_data);
typedef void(CAD_CALL* CadCommandDestroyFn)(void* instance);

typedef enum CadTransactionMode {
    /* Wrapped in one transaction; the normal case, and one undo step. */
    CAD_TRANSACTION_SINGLE = 0,
    /* Makes no changes; creates no undo entry. */
    CAD_TRANSACTION_READ_ONLY = 1
} CadTransactionMode;

/* Everything needed to register a command. Zero-initialize, then fill.
 *
 * The host copies every string and the alias array during register_command, so
 * the descriptor itself may be a stack local. The function pointers and
 * user_data are retained, and must stay valid until the command is
 * unregistered or the plugin is unloaded.
 *
 * There is deliberately no preview callback yet. Rubber-band preview needs
 * entities to cross the boundary as owned objects, and getting that ownership
 * model wrong is expensive to unpick later; it will be appended as a minor
 * version once the shape is settled. A plugin command works fully without it,
 * it simply does not draw under the cursor. */
typedef struct CadCommandDesc {
    uint32_t struct_size;

    const char* id;       /* stable, unique, never shown: "acme.draw.bolt" */
    const char* name;     /* typed at the command line: "BOLT" */
    const char* title;    /* shown in menus: "Bolt" */
    const char* description; /* may be NULL */
    const char* category; /* groups it in menus: "Draw", "Modify" */

    const char* const* aliases; /* may be NULL */
    uint32_t alias_count;

    int32_t transaction_mode; /* CadTransactionMode */
    int32_t supports_pickfirst;
    int32_t repeatable;

    CadCommandCreateFn on_create;   /* may be NULL */
    CadCommandDestroyFn on_destroy; /* may be NULL */
    CadCommandStartFn on_start;     /* required */
    CadCommandSupplyFn on_supply;   /* required */
    CadCommandCleanupFn on_cleanup; /* may be NULL */

    void* user_data;
} CadCommandDesc;

/* ---------------------------------------------------------------- the table */

/*
 * The host's functions. Passed to cad_plugin_init and valid for the plugin's
 * lifetime.
 *
 * Check struct_size before calling anything added after the version you built
 * against; the host may be older than you are.
 */
typedef struct CadApi {
    uint32_t struct_size;
    uint32_t version_major;
    uint32_t version_minor;

    /* --- diagnostics ------------------------------------------------------
     * Message for the last failure on this thread. Valid until the next call.
     * Never NULL; returns "" when there is nothing to report. */
    const char*(CAD_CALL* last_error)(void);

    /* --- documents -------------------------------------------------------- */
    CadDocument*(CAD_CALL* host_document)(CadPluginHost* host);
    CadStatus(CAD_CALL* document_extents)(CadDocument* document, CadBox* out_extents);

    /* --- layers ----------------------------------------------------------- */
    /* Find by name, or 0 when absent. Names are case-insensitive. */
    CadHandle(CAD_CALL* find_layer)(CadDocument* document, const char* name);
    /* Find or create. Creating requires an open transaction. */
    CadStatus(CAD_CALL* ensure_layer)(CadTransaction* transaction, const char* name,
                                      CadHandle* out_layer);
    CadStatus(CAD_CALL* layer_name)(CadDocument* document, CadHandle layer, char* buffer,
                                    size_t capacity, size_t* out_required);
    CadStatus(CAD_CALL* layer_color)(CadDocument* document, CadHandle layer,
                                     CadColor* out_color);
    CadStatus(CAD_CALL* set_layer_color)(CadTransaction* transaction, CadHandle layer,
                                         CadColor color);
    uint32_t(CAD_CALL* layer_count)(CadDocument* document);
    CadStatus(CAD_CALL* layer_at)(CadDocument* document, uint32_t index, CadHandle* out_layer);

    /* --- blocks ----------------------------------------------------------- */
    CadHandle(CAD_CALL* find_block)(CadDocument* document, const char* name);
    CadHandle(CAD_CALL* model_space)(CadDocument* document);

    /* --- entity queries --------------------------------------------------- */
    int32_t(CAD_CALL* entity_kind)(CadDocument* document, CadHandle entity);
    CadStatus(CAD_CALL* entity_bounds)(CadDocument* document, CadHandle entity,
                                       CadBox* out_bounds);
    CadStatus(CAD_CALL* entity_properties)(CadDocument* document, CadHandle entity,
                                           CadEntityProperties* out_properties);
    CadStatus(CAD_CALL* set_entity_properties)(CadTransaction* transaction, CadHandle entity,
                                               const CadEntityProperties* properties);

    /* Entities in a block record, in draw order. Pass NULL for out_entities to
     * learn the count first — the standard two-call pattern, so no allocation
     * crosses the boundary. */
    CadStatus(CAD_CALL* block_entities)(CadDocument* document, CadHandle block,
                                        CadHandle* out_entities, uint32_t capacity,
                                        uint32_t* out_count);

    /* --- entity creation --------------------------------------------------
     * All require an open transaction, so everything a plugin creates is
     * undoable as part of the command that created it. Pass 0 for `block` to
     * mean model space, and NULL for `properties` to accept the defaults. */
    CadStatus(CAD_CALL* add_line)(CadTransaction* transaction, CadHandle block,
                                  CadPoint start, CadPoint end,
                                  const CadEntityProperties* properties,
                                  CadHandle* out_entity);
    CadStatus(CAD_CALL* add_circle)(CadTransaction* transaction, CadHandle block,
                                    CadPoint center, double radius,
                                    const CadEntityProperties* properties,
                                    CadHandle* out_entity);
    /* Angles are radians and sweep counter-clockwise, matching the core and
     * DWG. Degrees exist only in files and in the UI. */
    CadStatus(CAD_CALL* add_arc)(CadTransaction* transaction, CadHandle block,
                                 CadPoint center, double radius, double start_angle,
                                 double end_angle, const CadEntityProperties* properties,
                                 CadHandle* out_entity);
    CadStatus(CAD_CALL* add_polyline)(CadTransaction* transaction, CadHandle block,
                                      const CadVertex* vertices, uint32_t vertex_count,
                                      int32_t closed,
                                      const CadEntityProperties* properties,
                                      CadHandle* out_entity);
    CadStatus(CAD_CALL* add_point)(CadTransaction* transaction, CadHandle block,
                                   CadPoint position,
                                   const CadEntityProperties* properties,
                                   CadHandle* out_entity);
    CadStatus(CAD_CALL* add_text)(CadTransaction* transaction, CadHandle block,
                                  CadPoint position, const char* contents, double height,
                                  double rotation, const CadEntityProperties* properties,
                                  CadHandle* out_entity);

    /* --- entity modification ---------------------------------------------- */
    CadStatus(CAD_CALL* erase_entity)(CadTransaction* transaction, CadHandle entity);
    /* Column-major 4x4, matching the core and every graphics API. */
    CadStatus(CAD_CALL* transform_entity)(CadTransaction* transaction, CadHandle entity,
                                          const double matrix[16]);

    /* --- geometry queries -------------------------------------------------
     * Answered from the entity's curves, so they work for any curve-backed
     * type including ones added later. */
    CadStatus(CAD_CALL* closest_point)(CadDocument* document, CadHandle entity,
                                       CadPoint from, CadPoint* out_point,
                                       double* out_distance);
    CadStatus(CAD_CALL* entity_length)(CadDocument* document, CadHandle entity,
                                       double* out_length);
    /* Pass NULL for out_points to learn the count first. */
    CadStatus(CAD_CALL* intersect_entities)(CadDocument* document, CadHandle a, CadHandle b,
                                            CadPoint* out_points, uint32_t capacity,
                                            uint32_t* out_count);

    /* --- selection --------------------------------------------------------- */
    CadStatus(CAD_CALL* selection)(CadCommandContext* context, CadHandle* out_entities,
                                   uint32_t capacity, uint32_t* out_count);

    /* --- transactions -----------------------------------------------------
     * A command's transaction is supplied by the host and must not be
     * committed or aborted by the plugin: the executor owns its lifetime and
     * guarantees exactly one commit or rollback however the command ends. */
    CadTransaction*(CAD_CALL* context_transaction)(CadCommandContext* context);
    CadDocument*(CAD_CALL* context_document)(CadCommandContext* context);

    /* Run something outside a command, as its own undo step. */
    CadStatus(CAD_CALL* begin_transaction)(CadDocument* document, const char* name,
                                           CadTransaction** out_transaction);
    CadStatus(CAD_CALL* commit_transaction)(CadTransaction* transaction);
    CadStatus(CAD_CALL* abort_transaction)(CadTransaction* transaction);

    CadStatus(CAD_CALL* undo)(CadDocument* document);
    CadStatus(CAD_CALL* redo)(CadDocument* document);

    /* --- user interaction --------------------------------------------------- */
    void(CAD_CALL* log_message)(CadCommandContext* context, const char* message);
    void(CAD_CALL* log_warning)(CadCommandContext* context, const char* message);

    /* --- commands ---------------------------------------------------------
     * A command registered here goes into the same registry as the built-ins,
     * appears in the same menus, and undoes the same way. */
    CadStatus(CAD_CALL* register_command)(CadPluginHost* host, const CadCommandDesc* desc);
    CadStatus(CAD_CALL* unregister_command)(CadPluginHost* host, const char* id);
    /* Invoke any command by name or alias, built-in or not. */
    CadStatus(CAD_CALL* execute_command)(CadDocument* document, const char* name);
} CadApi;

/* ------------------------------------------------------------- entry points */

/*
 * Called once when the plugin is loaded. Register commands here.
 *
 * Return CAD_OK to stay loaded; anything else unloads the plugin and reports
 * the failure. `api` and `host` remain valid until cad_plugin_shutdown.
 */
typedef int32_t(CAD_CALL* CadPluginInitFn)(const CadApi* api, CadPluginHost* host);

/* Optional. Called before unloading. Commands are unregistered automatically. */
typedef void(CAD_CALL* CadPluginShutdownFn)(void);

/*
 * Optional. Lets the host reject an incompatible plugin *before* running any
 * of its code, which is the only safe moment to find out.
 */
typedef void(CAD_CALL* CadPluginVersionFn)(uint32_t* out_major, uint32_t* out_minor);

#define CAD_PLUGIN_INIT_SYMBOL "cad_plugin_init"
#define CAD_PLUGIN_SHUTDOWN_SYMBOL "cad_plugin_shutdown"
#define CAD_PLUGIN_VERSION_SYMBOL "cad_plugin_version"

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* CAD_ABI_H */
