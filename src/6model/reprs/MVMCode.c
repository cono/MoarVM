#include "moar.h"

/* This representation's function pointer table. */
static const MVMREPROps this_repr;

/* Invocation protocol handler. */
static void invoke_handler(MVMThreadContext *tc, MVMObject *invokee, MVMCallsite *callsite, MVMRegister *args) {
    if (IS_CONCRETE(invokee)) {
        MVMCode *code = (MVMCode *)invokee;
        MVM_frame_invoke(tc, code->body.sf, callsite, args, code->body.outer, invokee, -1);
    }
    else {
        MVM_exception_throw_adhoc(tc, "Cannot invoke code type object");
    }
}

/* Creates a new type object of this representation, and associates it with
 * the given HOW. Also sets the invocation protocol handler in the STable. */
static MVMObject * type_object_for(MVMThreadContext *tc, MVMObject *HOW) {
    MVMSTable *st = MVM_gc_allocate_stable(tc, &this_repr, HOW);

    MVMROOT(tc, st, {
        MVMObject *obj = MVM_gc_allocate_type_object(tc, st);
        MVM_ASSIGN_REF(tc, &(st->header), st->WHAT, obj);
        st->invoke = invoke_handler;
        st->size = sizeof(MVMCode);
    });

    return st->WHAT;
}

/* Copies the body of one object to another. */
static void copy_to(MVMThreadContext *tc, MVMSTable *st, void *src, MVMObject *dest_root, void *dest) {
    MVMCodeBody *src_body  = (MVMCodeBody *)src;
    MVMCodeBody *dest_body = (MVMCodeBody *)dest;
    MVM_ASSIGN_REF(tc, &(dest_root->header), dest_body->sf, src_body->sf);
    if (src_body->outer)
        dest_body->outer = MVM_frame_inc_ref(tc, src_body->outer);
    MVM_ASSIGN_REF(tc, &(dest_root->header), dest_body->name, src_body->name);
    /* Explicitly do *not* copy state vars in a (presumably closure) clone. */
}

/* Adds held objects to the GC worklist. */
static void gc_mark(MVMThreadContext *tc, MVMSTable *st, void *data, MVMGCWorklist *worklist) {
    MVMCodeBody *body = (MVMCodeBody *)data;
    MVM_gc_worklist_add_frame(tc, worklist, body->outer);
    MVM_gc_worklist_add(tc, worklist, &body->code_object);
    MVM_gc_worklist_add(tc, worklist, &body->sf);
    MVM_gc_worklist_add(tc, worklist, &body->name);
    if (body->state_vars) {
        MVMuint8 *flags  = body->sf->body.static_env_flags;
        MVMuint16 *types = body->sf->body.lexical_types;
        MVMint64 numlex  = body->sf->body.num_lexicals;
        MVMint64 i;
        for (i = 0; i < numlex; i++) {
            if (flags[i] == 2) {
                if (types[i] == MVM_reg_obj)
                    MVM_gc_worklist_add(tc, worklist, &body->state_vars[i].o);
                else if (types[i] == MVM_reg_str)
                    MVM_gc_worklist_add(tc, worklist, &body->state_vars[i].s);
            }
        }
    }
}

/* Called by the VM in order to free memory associated with this object. */
static void gc_free(MVMThreadContext *tc, MVMObject *obj) {
    MVMCode *code_obj = (MVMCode *)obj;
    if (code_obj->body.outer)
        code_obj->body.outer = MVM_frame_dec_ref(tc, code_obj->body.outer);
    MVM_checked_free_null(code_obj->body.state_vars);
}

/* Gets the storage specification for this representation. */
static MVMStorageSpec get_storage_spec(MVMThreadContext *tc, MVMSTable *st) {
    MVMStorageSpec spec;
    spec.inlineable      = MVM_STORAGE_SPEC_REFERENCE;
    spec.boxed_primitive = MVM_STORAGE_SPEC_BP_NONE;
    spec.can_box         = 0;
    return spec;
}

/* Compose the representation. */
static void compose(MVMThreadContext *tc, MVMSTable *st, MVMObject *info) {
    /* Nothing to do for this REPR. */
}

/* Initializes the representation. */
const MVMREPROps * MVMCode_initialize(MVMThreadContext *tc) {
    return &this_repr;
}

static const MVMREPROps this_repr = {
    type_object_for,
    MVM_gc_allocate_object,
    NULL, /* initialize */
    copy_to,
    MVM_REPR_DEFAULT_ATTR_FUNCS,
    MVM_REPR_DEFAULT_BOX_FUNCS,
    MVM_REPR_DEFAULT_POS_FUNCS,
    MVM_REPR_DEFAULT_ASS_FUNCS,
    MVM_REPR_DEFAULT_ELEMS,
    get_storage_spec,
    NULL, /* change_type */
    NULL, /* serialize */
    NULL, /* deserialize */
    NULL, /* serialize_repr_data */
    NULL, /* deserialize_repr_data */
    NULL, /* deserialize_stable_size */
    gc_mark,
    gc_free,
    NULL, /* gc_cleanup */
    NULL, /* gc_mark_repr_data */
    NULL, /* gc_free_repr_data */
    compose,
    NULL, /* spesh */
    "MVMCode", /* name */
    MVM_REPR_ID_MVMCode,
    1, /* refs_frames */
};
