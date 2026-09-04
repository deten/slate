/*
 * BOLTCIRCLE — an example Slate plugin.
 *
 * Draws a bolt hole circle: a ring of equally spaced holes about a centre,
 * with a construction circle through their centres on its own layer. A real
 * command a mechanical drafter would use, chosen over a toy so the example
 * exercises the parts of the ABI a real plugin needs: multi-step input,
 * numeric defaults, a keyword alternative, layer creation, entity creation,
 * and per-invocation state.
 *
 * Written in C, not C++, deliberately. The plugin header is meant to be
 * consumable by any language with a C FFI, and the only way to keep that true
 * is for something to actually compile it as C.
 *
 * Everything this file creates lands in the command's transaction, so the
 * whole ring undoes as one step — the same as a built-in command, because it
 * *is* the same mechanism.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cad/abi/cad_abi.h>

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

/* The host's function table, captured at load time. */
static const CadApi* g_api = NULL;

/* Layer the construction circle goes on. Created on demand. */
static const char* const kConstructionLayer = "BOLTCIRCLE";

/* ------------------------------------------------------------------- state */

enum BoltStage {
    STAGE_CENTER = 0,
    STAGE_RADIUS,
    STAGE_COUNT,
    STAGE_HOLE
};

/*
 * Per-invocation state.
 *
 * One allocation per run of the command, not one per registration: the same
 * command may be running in two drawings at once, and shared mutable state
 * would have them overwrite each other's half-finished input.
 */
typedef struct BoltState {
    int stage;
    CadPoint center;
    double radius;
    int32_t count;
    int32_t use_marks;
    /* Prompts are built here rather than in a local, so the pointer handed to
     * the host is still valid when the callback returns. */
    char prompt[160];
} BoltState;

static void* CAD_CALL bolt_create(void* user_data) {
    BoltState* state = (BoltState*)calloc(1, sizeof(BoltState));
    (void)user_data;
    if (state != NULL) {
        state->stage = STAGE_CENTER;
        state->count = 6;
    }
    return state;
}

static void CAD_CALL bolt_destroy(void* instance) {
    free(instance);
}

/* ------------------------------------------------------------ step helpers */

static CadStepResult step_ask(BoltState* state, int32_t kind, const char* prompt) {
    CadStepResult step;
    memset(&step, 0, sizeof(step));
    step.struct_size = (uint32_t)sizeof(step);
    step.kind = CAD_STEP_REQUEST;
    step.request.struct_size = (uint32_t)sizeof(step.request);
    step.request.kind = kind;

    snprintf(state->prompt, sizeof(state->prompt), "%s", prompt);
    step.request.prompt = state->prompt;
    return step;
}

static CadStepResult step_done(const char* message) {
    CadStepResult step;
    memset(&step, 0, sizeof(step));
    step.struct_size = (uint32_t)sizeof(step);
    step.kind = CAD_STEP_DONE;
    step.message = message;
    return step;
}

static CadStepResult step_failed(const char* message) {
    CadStepResult step;
    memset(&step, 0, sizeof(step));
    step.struct_size = (uint32_t)sizeof(step);
    step.kind = CAD_STEP_FAILED;
    step.message = message;
    return step;
}

static CadStepResult step_cancelled(void) {
    CadStepResult step;
    memset(&step, 0, sizeof(step));
    step.struct_size = (uint32_t)sizeof(step);
    step.kind = CAD_STEP_CANCELLED;
    return step;
}

/* ---------------------------------------------------------------- geometry */

/*
 * Build the ring.
 *
 * Note what is absent: no explicit transaction management. The transaction
 * belongs to the executor, which commits it when the command reports DONE and
 * rolls it back on CANCELLED or FAILED. A plugin that half-finishes cannot
 * leave the drawing half-edited.
 */
static CadStatus bolt_draw(CadCommandContext* context, const BoltState* state,
                           double hole_radius) {
    CadTransaction* transaction = g_api->context_transaction(context);
    CadEntityProperties properties;
    CadHandle layer = CAD_NULL_HANDLE;
    CadStatus status;
    int32_t i;

    if (transaction == NULL) {
        return CAD_ERROR_NO_TRANSACTION;
    }

    memset(&properties, 0, sizeof(properties));
    properties.struct_size = (uint32_t)sizeof(properties);

    /* The construction circle goes on its own layer so it can be turned off
     * without touching the holes. */
    status = g_api->ensure_layer(transaction, kConstructionLayer, &layer);
    if (status != CAD_OK) {
        return status;
    }
    properties.layer = layer;
    properties.color.method = CAD_COLOR_INDEX;
    properties.color.index = 4; /* cyan */

    status = g_api->add_circle(transaction, CAD_NULL_HANDLE, state->center, state->radius,
                               &properties, NULL);
    if (status != CAD_OK) {
        return status;
    }

    /* The holes go on whatever layer is current, which is what a drafter
     * expects: the construction geometry is the special case, not the parts. */
    for (i = 0; i < state->count; ++i) {
        const double angle = 2.0 * M_PI * (double)i / (double)state->count;
        CadPoint at;
        at.x = state->center.x + state->radius * cos(angle);
        at.y = state->center.y + state->radius * sin(angle);
        at.z = state->center.z;

        if (state->use_marks) {
            status = g_api->add_point(transaction, CAD_NULL_HANDLE, at, NULL, NULL);
        } else {
            status = g_api->add_circle(transaction, CAD_NULL_HANDLE, at, hole_radius, NULL, NULL);
        }
        if (status != CAD_OK) {
            return status;
        }
    }
    return CAD_OK;
}

/* --------------------------------------------------------------- callbacks */

static CadStepResult CAD_CALL bolt_start(CadCommandContext* context, void* user_data) {
    BoltState* state = (BoltState*)user_data;
    (void)context;
    if (state == NULL) {
        return step_failed("BOLTCIRCLE could not allocate its state");
    }
    state->stage = STAGE_CENTER;
    return step_ask(state, CAD_INPUT_POINT, "Specify centre of bolt circle");
}

static CadStepResult CAD_CALL bolt_supply(CadCommandContext* context, const CadInputValue* input,
                                          void* user_data) {
    BoltState* state = (BoltState*)user_data;
    CadStepResult step;

    if (state == NULL || input == NULL) {
        return step_failed("BOLTCIRCLE received no input");
    }
    if (input->kind == CAD_VALUE_CANCEL) {
        return step_cancelled();
    }

    switch (state->stage) {
        case STAGE_CENTER:
            if (input->kind != CAD_VALUE_POINT) {
                return step_ask(state, CAD_INPUT_POINT, "Specify centre of bolt circle");
            }
            state->center = input->point;
            state->stage = STAGE_RADIUS;
            step = step_ask(state, CAD_INPUT_DISTANCE, "Specify bolt circle radius");
            /* A base point turns the prompt into a rubber-band measurement and
             * lets the user answer by picking rather than typing. */
            step.request.has_base_point = 1;
            step.request.base_point = state->center;
            return step;

        case STAGE_RADIUS: {
            double radius = 0.0;
            if (input->kind == CAD_VALUE_NUMBER) {
                radius = input->number;
            } else if (input->kind == CAD_VALUE_POINT) {
                const double dx = input->point.x - state->center.x;
                const double dy = input->point.y - state->center.y;
                radius = sqrt(dx * dx + dy * dy);
            } else {
                step = step_ask(state, CAD_INPUT_DISTANCE, "Specify bolt circle radius");
                step.request.has_base_point = 1;
                step.request.base_point = state->center;
                return step;
            }
            if (!(radius > 0.0)) {
                return step_failed("The bolt circle radius must be greater than zero");
            }
            state->radius = radius;
            state->stage = STAGE_COUNT;

            step = step_ask(state, CAD_INPUT_NUMBER, "Number of holes");
            step.request.has_default_number = 1;
            step.request.default_number = (double)state->count;
            step.request.allow_empty = 1;
            return step;
        }

        case STAGE_COUNT: {
            static const CadKeyword kHoleKeywords[] = {
                {"Marks", "M", "Place point nodes instead of drilled circles"}
            };
            long count = state->count;
            if (input->kind == CAD_VALUE_NUMBER) {
                count = (long)(input->number + 0.5);
            } else if (input->kind != CAD_VALUE_EMPTY) {
                step = step_ask(state, CAD_INPUT_NUMBER, "Number of holes");
                step.request.has_default_number = 1;
                step.request.default_number = (double)state->count;
                step.request.allow_empty = 1;
                return step;
            }
            if (count < 2 || count > 4096) {
                return step_failed("A bolt circle needs between 2 and 4096 holes");
            }
            state->count = (int32_t)count;
            state->stage = STAGE_HOLE;

            step = step_ask(state, CAD_INPUT_DISTANCE, "Hole radius");
            step.request.has_default_number = 1;
            step.request.default_number = state->radius / 8.0;
            step.request.allow_empty = 1;
            step.request.keywords = kHoleKeywords;
            step.request.keyword_count = 1;
            return step;
        }

        case STAGE_HOLE: {
            double hole_radius = state->radius / 8.0;
            CadStatus status;
            char summary[96];

            if (input->kind == CAD_VALUE_KEYWORD) {
                state->use_marks = 1;
            } else if (input->kind == CAD_VALUE_NUMBER) {
                hole_radius = input->number;
                if (!(hole_radius > 0.0)) {
                    return step_failed("The hole radius must be greater than zero");
                }
            } else if (input->kind == CAD_VALUE_POINT) {
                const double dx = input->point.x - state->center.x;
                const double dy = input->point.y - state->center.y;
                hole_radius = fabs(sqrt(dx * dx + dy * dy) - state->radius);
                if (!(hole_radius > 0.0)) {
                    return step_failed("The hole radius must be greater than zero");
                }
            }

            status = bolt_draw(context, state, hole_radius);
            if (status != CAD_OK) {
                return step_failed(g_api->last_error());
            }

            snprintf(summary, sizeof(summary), "%d holes on a %.4g radius bolt circle",
                     (int)state->count, state->radius);
            g_api->log_message(context, summary);
            return step_done("BOLTCIRCLE complete");
        }

        default:
            return step_failed("BOLTCIRCLE reached an unknown state");
    }
}

/* -------------------------------------------------------------- entry point */

CAD_PLUGIN_EXPORT void CAD_CALL cad_plugin_version(uint32_t* out_major, uint32_t* out_minor) {
    if (out_major != NULL) {
        *out_major = CAD_ABI_VERSION_MAJOR;
    }
    if (out_minor != NULL) {
        *out_minor = CAD_ABI_VERSION_MINOR;
    }
}

CAD_PLUGIN_EXPORT int32_t CAD_CALL cad_plugin_init(const CadApi* api, CadPluginHost* host) {
    static const char* const kAliases[] = {"BC"};
    CadCommandDesc desc;

    if (api == NULL || host == NULL) {
        return CAD_ERROR_INVALID_ARGUMENT;
    }
    if (api->version_major != CAD_ABI_VERSION_MAJOR) {
        return CAD_ERROR_UNSUPPORTED_VERSION;
    }
    g_api = api;

    memset(&desc, 0, sizeof(desc));
    desc.struct_size = (uint32_t)sizeof(desc);
    desc.id = "example.draw.boltcircle";
    desc.name = "BOLTCIRCLE";
    desc.title = "Bolt Circle";
    desc.description = "Place equally spaced holes on a circle about a centre point";
    desc.category = "Draw";
    desc.aliases = kAliases;
    desc.alias_count = 1;
    desc.transaction_mode = CAD_TRANSACTION_SINGLE;
    desc.repeatable = 1;
    desc.on_create = bolt_create;
    desc.on_destroy = bolt_destroy;
    desc.on_start = bolt_start;
    desc.on_supply = bolt_supply;

    return (int32_t)api->register_command(host, &desc);
}

CAD_PLUGIN_EXPORT void CAD_CALL cad_plugin_shutdown(void) {
    g_api = NULL;
}
