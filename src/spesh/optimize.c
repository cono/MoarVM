#include "moar.h"

/* This is where the main optimization work on a spesh graph takes place,
 * using facts discovered during analysis. */

/* Obtains facts for an operand, just directly accessing them without
 * inferring any kind of usage. */
static MVMSpeshFacts * get_facts_direct(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshOperand o) {
    return &g->facts[o.reg.orig][o.reg.i];
}

/* Obtains facts for an operand, indicating they are being used. */
MVMSpeshFacts * MVM_spesh_get_and_use_facts(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshOperand o) {
    MVMSpeshFacts *facts = get_facts_direct(tc, g, o);
    if (facts->flags & MVM_SPESH_FACT_FROM_LOG_GUARD)
        g->log_guards[facts->log_guard].used = 1;
    return facts;
}

/* Obtains facts for an operand, but doesn't (yet) indicate usefulness */
MVMSpeshFacts * MVM_spesh_get_facts(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshOperand o) {
    return get_facts_direct(tc, g, o);
}

/* Mark facts for an operand as being relied upon */
void MVM_spesh_use_facts(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshFacts *f) {
    if (f->flags & MVM_SPESH_FACT_FROM_LOG_GUARD)
        g->log_guards[f->log_guard].used = 1;
}

/* Obtains a string constant. */
MVMString * MVM_spesh_get_string(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshOperand o) {
    return g->sf->body.cu->body.strings[o.lit_str_idx];
}

/* Copy facts between two register operands. */
static void copy_facts(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshOperand to,
                       MVMSpeshOperand from) {
    MVMSpeshFacts *tfacts = get_facts_direct(tc, g, to);
    MVMSpeshFacts *ffacts = get_facts_direct(tc, g, from);
    tfacts->flags         = ffacts->flags;
    tfacts->type          = ffacts->type;
    tfacts->decont_type   = ffacts->decont_type;
    tfacts->value         = ffacts->value;
    tfacts->log_guard     = ffacts->log_guard;
}

/* Adds a value into a spesh slot and returns its index. */
MVMint16 MVM_spesh_add_spesh_slot(MVMThreadContext *tc, MVMSpeshGraph *g, MVMCollectable *c) {
    if (g->num_spesh_slots >= g->alloc_spesh_slots) {
        g->alloc_spesh_slots += 8;
        if (g->spesh_slots)
            g->spesh_slots = realloc(g->spesh_slots,
                g->alloc_spesh_slots * sizeof(MVMCollectable *));
        else
            g->spesh_slots = malloc(g->alloc_spesh_slots * sizeof(MVMCollectable *));
    }
    g->spesh_slots[g->num_spesh_slots] = c;
    return g->num_spesh_slots++;
}

/* Performs optimization on a method lookup. If we know the type that we'll
 * be dispatching on, resolve it right off. If not, add a cache. */
static void optimize_method_lookup(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshIns *ins) {
    /* See if we can resolve the method right off due to knowing the type. */
    MVMSpeshFacts *obj_facts = MVM_spesh_get_facts(tc, g, ins->operands[1]);
    MVMint32 resolved = 0;
    if (obj_facts->flags & MVM_SPESH_FACT_KNOWN_TYPE) {
        /* Try to resolve. */
        MVMString *name = MVM_spesh_get_string(tc, g, ins->operands[2]);
        MVMObject *meth = MVM_6model_find_method_cache_only(tc, obj_facts->type, name);
        if (!MVM_is_null(tc, meth)) {
            /* Could compile-time resolve the method. Add it in a spesh slot. */
            MVMint16 ss = MVM_spesh_add_spesh_slot(tc, g, (MVMCollectable *)meth);

            /* Tweak facts for the target, given we know the method. */
            MVMSpeshFacts *meth_facts = MVM_spesh_get_and_use_facts(tc, g, ins->operands[0]);
            meth_facts->flags |= MVM_SPESH_FACT_KNOWN_VALUE;
            meth_facts->value.o = meth;

            /* Update the instruction to grab the spesh slot. */
            ins->info = MVM_op_get_op(MVM_OP_sp_getspeshslot);
            ins->operands[1].lit_i16 = ss;

            resolved = 1;

            MVM_spesh_use_facts(tc, g, obj_facts);
            obj_facts->usages--;
        }
    }

    /* If not, add space to cache a single type/method pair, to save hash
     * lookups in the (common) monomorphic case, and rewrite to caching
     * version of the instruction. */
    if (!resolved) {
        MVMSpeshOperand *orig_o = ins->operands;
        ins->info = MVM_op_get_op(MVM_OP_sp_findmeth);
        ins->operands = MVM_spesh_alloc(tc, g, 4 * sizeof(MVMSpeshOperand));
        memcpy(ins->operands, orig_o, 3 * sizeof(MVMSpeshOperand));
        ins->operands[3].lit_i16 = MVM_spesh_add_spesh_slot(tc, g, NULL);
        MVM_spesh_add_spesh_slot(tc, g, NULL);
    }
}

/* Sees if we can resolve an istype at compile time. */
static void optimize_istype(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshIns *ins) {
    MVMSpeshFacts *obj_facts  = MVM_spesh_get_facts(tc, g, ins->operands[1]);
    MVMSpeshFacts *type_facts = MVM_spesh_get_facts(tc, g, ins->operands[2]);
    MVMSpeshFacts *result_facts;

    if (type_facts->flags & MVM_SPESH_FACT_KNOWN_TYPE &&
         obj_facts->flags & MVM_SPESH_FACT_KNOWN_TYPE) {
        MVMint32 result;
        if (!MVM_6model_try_cache_type_check(tc, obj_facts->type, type_facts->type, &result))
            return;
        ins->info = MVM_op_get_op(MVM_OP_const_i64_16);
        result_facts = MVM_spesh_get_facts(tc, g, ins->operands[0]);
        result_facts->flags |= MVM_SPESH_FACT_KNOWN_VALUE;
        ins->operands[1].lit_i16 = result;
        result_facts->value.i16  = result;

        obj_facts->usages--;
        type_facts->usages--;
        MVM_spesh_use_facts(tc, g, obj_facts);
        MVM_spesh_use_facts(tc, g, type_facts);
    }
}

static void optimize_is_reprid(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshIns *ins) {
    MVMSpeshFacts *obj_facts = MVM_spesh_get_facts(tc, g, ins->operands[1]);
    MVMuint32 wanted_repr_id;
    MVMuint64 result_value;

    if (!(obj_facts->flags & MVM_SPESH_FACT_KNOWN_TYPE)) {
        return;
    }

    switch (ins->info->opcode) {
        case MVM_OP_islist: wanted_repr_id = MVM_REPR_ID_MVMArray; break;
        case MVM_OP_ishash: wanted_repr_id = MVM_REPR_ID_MVMHash; break;
        case MVM_OP_isint:  wanted_repr_id = MVM_REPR_ID_P6int; break;
        case MVM_OP_isnum:  wanted_repr_id = MVM_REPR_ID_P6num; break;
        case MVM_OP_isstr:  wanted_repr_id = MVM_REPR_ID_P6str; break;
        default:            return;
    }

    MVM_spesh_use_facts(tc, g, obj_facts);

    result_value = REPR(obj_facts->type)->ID == wanted_repr_id;

    if (result_value == 0) {
        MVMSpeshFacts *result_facts = MVM_spesh_get_facts(tc, g, ins->operands[0]);
        ins->info = MVM_op_get_op(MVM_OP_const_i64_16);
        ins->operands[1].lit_i16 = 0;
        result_facts->flags |= MVM_SPESH_FACT_KNOWN_VALUE;
        result_facts->value.i64 = 0;
    } else {
        ins->info = MVM_op_get_op(MVM_OP_isnonnull);
    }
}

/* Sees if we can resolve an isconcrete at compile time. */
static void optimize_isconcrete(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshIns *ins) {
    MVMSpeshFacts *obj_facts = MVM_spesh_get_facts(tc, g, ins->operands[1]);
    if (obj_facts->flags & (MVM_SPESH_FACT_CONCRETE | MVM_SPESH_FACT_TYPEOBJ)) {
        MVMSpeshFacts *result_facts = MVM_spesh_get_facts(tc, g, ins->operands[0]);
        ins->info                   = MVM_op_get_op(MVM_OP_const_i64_16);
        result_facts->flags        |= MVM_SPESH_FACT_KNOWN_VALUE;
        result_facts->value.i16     = obj_facts->flags & MVM_SPESH_FACT_CONCRETE ? 1 : 0;
        ins->operands[1].lit_i16    = result_facts->value.i16;

        MVM_spesh_use_facts(tc, g, obj_facts);
        obj_facts->usages--;
    }
}

/* iffy ops that operate on a known value register can turn into goto
 * or be dropped. */
static void optimize_iffy(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshIns *ins, MVMSpeshBB *bb) {
    MVMSpeshFacts *flag_facts = MVM_spesh_get_facts(tc, g, ins->operands[0]);
    MVMuint8 negated_op;
    MVMuint8 truthvalue;

    switch (ins->info->opcode) {
        case MVM_OP_if_i:
        case MVM_OP_if_s:
        case MVM_OP_if_n:
        case MVM_OP_if_o:
        case MVM_OP_ifnonnull:
            negated_op = 0;
            break;
        case MVM_OP_unless_i:
        case MVM_OP_unless_s:
        case MVM_OP_unless_n:
        case MVM_OP_unless_o:
            negated_op = 1;
            break;
        default:
            return;
    }

    if (flag_facts->flags & MVM_SPESH_FACT_KNOWN_VALUE) {
        switch (ins->info->opcode) {
            case MVM_OP_if_i:
            case MVM_OP_unless_i:
                truthvalue = flag_facts->value.i64;
                break;
            case MVM_OP_if_o:
            case MVM_OP_unless_o: {
                MVMObject *objval = flag_facts->value.o;
                MVMBoolificationSpec *bs = objval->st->boolification_spec;
                MVMRegister resultreg;
                switch (bs == NULL ? MVM_BOOL_MODE_NOT_TYPE_OBJECT : bs->mode) {
                    case MVM_BOOL_MODE_UNBOX_INT:
                    case MVM_BOOL_MODE_UNBOX_NUM:
                    case MVM_BOOL_MODE_UNBOX_STR_NOT_EMPTY:
                    case MVM_BOOL_MODE_UNBOX_STR_NOT_EMPTY_OR_ZERO:
                    case MVM_BOOL_MODE_BIGINT:
                    case MVM_BOOL_MODE_ITER:
                    case MVM_BOOL_MODE_HAS_ELEMS:
                    case MVM_BOOL_MODE_NOT_TYPE_OBJECT:
                        MVM_coerce_istrue(tc, objval, &resultreg, NULL, NULL, 0);
                        truthvalue = resultreg.i64;
                        break;
                    case MVM_BOOL_MODE_CALL_METHOD:
                    default:
                        return;
                }
                break;
            }
            case MVM_OP_if_n:
            case MVM_OP_unless_n:
                truthvalue = flag_facts->value.n64 != 0.0;
                break;
            default:
                return;
        }
    } else {
        return;
    }

    MVM_spesh_use_facts(tc, g, flag_facts);
    flag_facts->usages--;

    truthvalue = truthvalue ? 1 : 0;
    if (truthvalue != negated_op) {
        /* this conditional can be turned into an unconditional jump */
        ins->info = MVM_op_get_op(MVM_OP_goto);
        ins->operands[0] = ins->operands[1];

        /* since we have an unconditional jump now, we can remove the successor
         * that's in the linear_next */
        MVM_spesh_manipulate_remove_successor(tc, bb, bb->linear_next);
    } else {
        /* this conditional can be dropped completely */
        MVM_spesh_manipulate_remove_successor(tc, bb, ins->operands[1].ins_bb);
        MVM_spesh_manipulate_delete_ins(tc, g, bb, ins);
    }
}

/* objprimspec can be done at spesh-time if we know the type of something.
 * Another thing is, that if we rely on the type being known, we'll be assured
 * we'll have a guard that promises the object in question to be non-null. */
static void optimize_objprimspec(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshIns *ins) {
    MVMSpeshFacts *obj_facts = MVM_spesh_get_facts(tc, g, ins->operands[1]);

    if (obj_facts->flags & MVM_SPESH_FACT_KNOWN_TYPE && obj_facts->type) {
        MVMSpeshFacts *result_facts = MVM_spesh_get_facts(tc, g, ins->operands[0]);
        ins->info                   = MVM_op_get_op(MVM_OP_const_i64_16);
        result_facts->flags        |= MVM_SPESH_FACT_KNOWN_VALUE;
        result_facts->value.i16     = REPR(obj_facts->type)->get_storage_spec(tc, STABLE(obj_facts->type)).boxed_primitive;
        ins->operands[1].lit_i16    = result_facts->value.i16;

        MVM_spesh_use_facts(tc, g, obj_facts);
        obj_facts->usages--;
    }
}

/* Optimizes a hllize instruction away if the type is known and already in the
 * right HLL, by turning it into a set. */
static void optimize_hllize(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshIns *ins) {
    MVMSpeshFacts *obj_facts = MVM_spesh_get_facts(tc, g, ins->operands[1]);
    if (obj_facts->flags & MVM_SPESH_FACT_KNOWN_TYPE && obj_facts->type) {
        if (STABLE(obj_facts->type)->hll_owner == g->sf->body.cu->body.hll_config) {
            ins->info = MVM_op_get_op(MVM_OP_set);

            MVM_spesh_use_facts(tc, g, obj_facts);

            copy_facts(tc, g, ins->operands[0], ins->operands[1]);
        }
    }
}

/* Turns a decont into a set, if we know it's not needed. Also make sure we
 * propagate any needed information. */
static void optimize_decont(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshBB *bb, MVMSpeshIns *ins) {
    MVMSpeshFacts *obj_facts = MVM_spesh_get_facts(tc, g, ins->operands[1]);
    if (obj_facts->flags & (MVM_SPESH_FACT_DECONTED | MVM_SPESH_FACT_TYPEOBJ)) {
        ins->info = MVM_op_get_op(MVM_OP_set);

        MVM_spesh_use_facts(tc, g, obj_facts);

        copy_facts(tc, g, ins->operands[0], ins->operands[1]);
    }
    else {
        MVMSpeshFacts *res_facts;
        if (obj_facts->flags & MVM_SPESH_FACT_KNOWN_TYPE && obj_facts->type) {
            MVMSTable *stable = STABLE(obj_facts->type);
            MVMContainerSpec const *contspec = stable->container_spec;
            if (contspec && contspec->fetch_never_invokes && contspec->spesh) {
                contspec->spesh(tc, stable, g, bb, ins);
            }
        }

        MVM_spesh_use_facts(tc, g, obj_facts);

        res_facts = MVM_spesh_get_facts(tc, g, ins->operands[0]);
        if (obj_facts->flags & MVM_SPESH_FACT_KNOWN_DECONT_TYPE) {
            res_facts->type   = obj_facts->decont_type;
            res_facts->flags |= MVM_SPESH_FACT_KNOWN_TYPE;
        }
        if (obj_facts->flags & MVM_SPESH_FACT_DECONT_CONCRETE)
            res_facts->flags |= MVM_SPESH_FACT_CONCRETE;
        else if (obj_facts->flags & MVM_SPESH_FACT_DECONT_TYPEOBJ)
            res_facts->flags |= MVM_SPESH_FACT_TYPEOBJ;
    }
}

/* Optimize away assertparamcheck if we know it will pass. */
static void optimize_assertparamcheck(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshBB *bb, MVMSpeshIns *ins) {
    MVMSpeshFacts *facts = MVM_spesh_get_facts(tc, g, ins->operands[0]);
    if (facts->flags & MVM_SPESH_FACT_KNOWN_VALUE && facts->value.i64) {
        MVM_spesh_use_facts(tc, g, facts);
        facts->usages--;
        MVM_spesh_manipulate_delete_ins(tc, g, bb, ins);
    }
}

static void optimize_can_op(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshBB *bb, MVMSpeshIns *ins) {
    /* XXX This causes problems, Spesh: failed to fix up handlers (-1, 110, 110) */
    MVMSpeshFacts *obj_facts = MVM_spesh_get_facts(tc, g, ins->operands[1]);
    MVMString *method_name;
    MVMint64 can_result;

    if (!(obj_facts->flags & MVM_SPESH_FACT_KNOWN_TYPE) || !obj_facts->type)
        return;

    if (ins->info->opcode == MVM_OP_can_s) {
        MVMSpeshFacts *name_facts = MVM_spesh_get_facts(tc, g, ins->operands[2]);
        if (!(name_facts->flags & MVM_SPESH_FACT_KNOWN_VALUE)) {
            return;
        }
        method_name = name_facts->value.s;
    } else {
        method_name = MVM_spesh_get_string(tc, g, ins->operands[2]);
    }

    can_result = MVM_6model_can_method_cache_only(tc, obj_facts->type, method_name);

    if (can_result == -1) {
        return;
    } else {
        MVMSpeshFacts *result_facts;

        if (ins->info->opcode == MVM_OP_can_s)
            MVM_spesh_get_facts(tc, g, ins->operands[2])->usages--;

        result_facts                = MVM_spesh_get_facts(tc, g, ins->operands[0]);
        ins->info                   = MVM_op_get_op(MVM_OP_const_i64_16);
        result_facts->flags        |= MVM_SPESH_FACT_KNOWN_VALUE;
        ins->operands[1].lit_i16    = can_result;
        result_facts->value.i16     = can_result;

        obj_facts->usages--;
        MVM_spesh_use_facts(tc, g, obj_facts);
    }
}

/* If we have a const_i and a coerce_in, we can emit a const_n instead. */
static void optimize_coerce(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshBB *bb, MVMSpeshIns *ins) {
    MVMSpeshFacts *facts = MVM_spesh_get_facts(tc, g, ins->operands[1]);

    if (facts->flags & MVM_SPESH_FACT_KNOWN_VALUE) {
        MVMSpeshFacts *result_facts = MVM_spesh_get_facts(tc, g, ins->operands[0]);
        MVMnum64 result = facts->value.i64;

        MVM_spesh_use_facts(tc, g, facts);
        facts->usages--;

        ins->info = MVM_op_get_op(MVM_OP_const_n64);
        ins->operands[1].lit_n64 = result;

        result_facts->flags |= MVM_SPESH_FACT_KNOWN_VALUE;
        result_facts->value.n64 = result;
    }
}

/* If we know the type of a significant operand, we might try to specialize by
 * representation. */
static void optimize_repr_op(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshBB *bb,
                             MVMSpeshIns *ins, MVMint32 type_operand) {
    MVMSpeshFacts *facts = MVM_spesh_get_facts(tc, g, ins->operands[type_operand]);
    if (facts->flags & MVM_SPESH_FACT_KNOWN_TYPE && facts->type)
        if (REPR(facts->type)->spesh) {
            REPR(facts->type)->spesh(tc, STABLE(facts->type), g, bb, ins);
            MVM_spesh_use_facts(tc, g, facts);
        }
}

/* smrt_strify and smrt_numify can turn into unboxes, but at least
 * for smrt_numify it's "complicated". Also, later when we know how
 * to put new invocations into spesh'd code, we could make direct
 * invoke calls to the .Str and .Num methods.
 */
static void optimize_smart_coerce(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshBB *bb, MVMSpeshIns *ins) {
    MVMSpeshFacts *facts = MVM_spesh_get_facts(tc, g, ins->operands[1]);

    MVMuint16 is_strify = ins->info->opcode == MVM_OP_smrt_strify;

    if (facts->flags & (MVM_SPESH_FACT_KNOWN_TYPE | MVM_SPESH_FACT_CONCRETE)) {
        MVMStorageSpec ss;
        MVMint64 can_result;

        ss = REPR(facts->type)->get_storage_spec(tc, STABLE(facts->type));

        if (is_strify && ss.can_box & MVM_STORAGE_SPEC_CAN_BOX_STR) {
            MVM_spesh_use_facts(tc, g, facts);

            ins->info = MVM_op_get_op(MVM_OP_unbox_s);
            /* And now that we have a repr op, we can try to optimize
             * it even further. */
            optimize_repr_op(tc, g, bb, ins, 1);

            return;
        }
        can_result = MVM_6model_can_method_cache_only(tc, facts->type,
                is_strify ? tc->instance->str_consts.Str : tc->instance->str_consts.Num);

        if (can_result == -1) {
            /* Couldn't safely figure out if the type has a Str method or not. */
            return;
        } else if (can_result == 0) {
            MVM_spesh_use_facts(tc, g, facts);
            /* We can't .Str this object, so we'll duplicate the "guessing"
             * logic from smrt_strify here to remove indirection. */
            if (is_strify && REPR(facts->type)->ID == MVM_REPR_ID_MVMException) {
                MVMSpeshOperand *operands  = MVM_spesh_alloc(tc, g, sizeof( MVMSpeshOperand ) * 3);
                MVMSpeshOperand *old_opers = ins->operands;

                ins->info = MVM_op_get_op(MVM_OP_sp_get_s);

                ins->operands = operands;

                operands[0] = old_opers[0];
                operands[1] = old_opers[1];
                operands[2].lit_i16 = offsetof( MVMException, body.message );
            } else if(ss.can_box & (MVM_STORAGE_SPEC_CAN_BOX_NUM | MVM_STORAGE_SPEC_CAN_BOX_INT)) {
                MVMuint16 register_type =
                    ss.can_box & MVM_STORAGE_SPEC_CAN_BOX_INT ? MVM_reg_int64 : MVM_reg_num64;

                MVMSpeshIns     *new_ins   = MVM_spesh_alloc(tc, g, sizeof( MVMSpeshIns ));
                MVMSpeshOperand *operands  = MVM_spesh_alloc(tc, g, sizeof( MVMSpeshOperand ) * 2);
                MVMSpeshOperand  temp      = MVM_spesh_manipulate_get_temp_reg(tc, g, register_type);
                MVMSpeshOperand  orig_dst  = ins->operands[0];

                ins->info = MVM_op_get_op(register_type == MVM_reg_num64 ? MVM_OP_unbox_n : MVM_OP_unbox_i);
                ins->operands[0] = temp;

                if (is_strify)
                    new_ins->info = MVM_op_get_op(register_type == MVM_reg_num64 ? MVM_OP_coerce_ns : MVM_OP_coerce_is);
                else
                    new_ins->info = MVM_op_get_op(register_type == MVM_reg_num64 ? MVM_OP_set : MVM_OP_coerce_in);
                new_ins->operands = operands;
                operands[0] = orig_dst;
                operands[1] = temp;

                /* We can directly "eliminate" a set instruction here. */
                if (new_ins->info->opcode != MVM_OP_set) {
                    MVM_spesh_manipulate_insert_ins(tc, bb, ins, new_ins);

                    MVM_spesh_get_facts(tc, g, temp)->usages++;
                } else {
                    ins->operands[0] = orig_dst;
                }

                /* Finally, let's try to optimize the unboxing REPROp. */
                optimize_repr_op(tc, g, bb, ins, 1);

                /* And as a last clean-up step, we release the temporary register. */
                MVM_spesh_manipulate_release_temp_reg(tc, g, temp);

                return;
            } else if (!is_strify && (REPR(facts->type)->ID == MVM_REPR_ID_MVMArray ||
                                     (REPR(facts->type)->ID == MVM_REPR_ID_MVMHash))) {
                /* A smrt_numify on an array or hash can be replaced by an
                 * elems operation, that can then be optimized by our
                 * versatile and dilligent friend optimize_repr_op. */

                MVMSpeshIns     *new_ins   = MVM_spesh_alloc(tc, g, sizeof( MVMSpeshIns ));
                MVMSpeshOperand *operands  = MVM_spesh_alloc(tc, g, sizeof( MVMSpeshOperand ) * 2);
                MVMSpeshOperand  temp      = MVM_spesh_manipulate_get_temp_reg(tc, g, MVM_reg_int64);
                MVMSpeshOperand  orig_dst  = ins->operands[0];

                ins->info = MVM_op_get_op(MVM_OP_elems);
                ins->operands[0] = temp;

                new_ins->info = MVM_op_get_op(MVM_OP_coerce_in);
                new_ins->operands = operands;
                operands[0] = orig_dst;
                operands[1] = temp;

                MVM_spesh_manipulate_insert_ins(tc, bb, ins, new_ins);

                optimize_repr_op(tc, g, bb, ins, 1);

                MVM_spesh_get_facts(tc, g, temp)->usages++;
                MVM_spesh_manipulate_release_temp_reg(tc, g, temp);
                return;
            }
        } else if (can_result == 1) {
            /* When we know how to generate additional callsites, we could
             * make an invocation to .Str or .Num here and perhaps have it
             * in-lined. */
        }
    }
}

/* boolification has a major indirection, which we can spesh away.
 * Afterwards, we may be able to spesh even further, so we defer
 * to other optimization methods. */
static void optimize_istrue_isfalse(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshBB *bb, MVMSpeshIns *ins) {
    MVMuint8 negated_op;
    MVMSpeshFacts *facts = MVM_spesh_get_facts(tc, g, ins->operands[1]);
    if (ins->info->opcode == MVM_OP_istrue) {
        negated_op = 0;
    } else if (ins->info->opcode == MVM_OP_isfalse) {
        negated_op = 1;
    } else {
        return;
    }

    /* Let's try to figure out the boolification spec. */
    if (facts->flags & MVM_SPESH_FACT_KNOWN_TYPE) {
        MVMBoolificationSpec *bs = STABLE(facts->type)->boolification_spec;
        switch (bs == NULL ? MVM_BOOL_MODE_NOT_TYPE_OBJECT : bs->mode) {
            case MVM_BOOL_MODE_UNBOX_INT:
                /* We can just unbox the int and pretend it's a bool. */
                ins->info = MVM_op_get_op(MVM_OP_unbox_i);
                /* And then we might be able to optimize this even further. */
                optimize_repr_op(tc, g, bb, ins, 1);
                break;
            case MVM_BOOL_MODE_NOT_TYPE_OBJECT:
                /* This is the same as isconcrete. */
                ins->info = MVM_op_get_op(MVM_OP_isconcrete);
                /* And now defer another bit of optimization */
                optimize_isconcrete(tc, g, ins);
                break;
            /* TODO implement MODE_UNBOX_NUM and the string ones */
            default:
                return;
        }
        /* Now we can take care of the negation. */
        if (negated_op) {
            MVMSpeshIns     *new_ins   = MVM_spesh_alloc(tc, g, sizeof( MVMSpeshIns ));
            MVMSpeshOperand *operands  = MVM_spesh_alloc(tc, g, sizeof( MVMSpeshOperand ) * 2);
            MVMSpeshFacts   *res_facts = MVM_spesh_get_facts(tc, g, ins->operands[0]);

            /* This is a bit naughty with regards to the SSA form, but
             * we'll hopefully get away with it until we have a proper
             * way to get new registers crammed in the middle of things */
            new_ins->info = MVM_op_get_op(MVM_OP_not_i);
            new_ins->operands = operands;
            operands[0] = ins->operands[0];
            operands[1] = ins->operands[0];
            MVM_spesh_manipulate_insert_ins(tc, bb, ins, new_ins);

            /* If there's a known value, update the fact. */
            if (res_facts->flags & MVM_SPESH_FACT_KNOWN_VALUE)
                res_facts->value.i64 = !res_facts->value.i64;
        }

        MVM_spesh_use_facts(tc, g, facts);
    }
}

/* Checks if we have specialized on the invocant - useful to know for some
 * optimizations. */
static MVMint32 specialized_on_invocant(MVMThreadContext *tc, MVMSpeshGraph *g) {
    MVMint32 i;
    for (i = 0; i < g->num_arg_guards; i++)
        if (g->arg_guards[i].slot == 0)
            return 1;
    return 0;
}

/* Optimizes away a lexical lookup when we know the value won't change from
 * the logged one. */
static void optimize_getlex_known(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshBB *bb,
                                  MVMSpeshIns *ins) {
    /* Ensure we have a log instruction following this one. */
    if (ins->next && ins->next->info->opcode == MVM_OP_sp_log) {
        /* Locate logged object. */
        MVMuint16       log_slot = ins->next->operands[1].lit_i16 * MVM_SPESH_LOG_RUNS;
        MVMCollectable *log_obj  = g->log_slots[log_slot];
        if (log_obj) {
            MVMSpeshFacts *facts;

            /* Place in a spesh slot. */
            MVMuint16 ss = MVM_spesh_add_spesh_slot(tc, g, log_obj);

            /* Delete logging instruction. */
            MVM_spesh_manipulate_delete_ins(tc, g, bb, ins->next);

            /* Transform lookup instruction into spesh slot read. */
            MVM_spesh_get_facts(tc, g, ins->operands[1])->usages--;
            ins->info = MVM_op_get_op(MVM_OP_sp_getspeshslot);
            ins->operands[1].lit_i16 = ss;

            /* Set up facts. */
            facts = MVM_spesh_get_facts(tc, g, ins->operands[0]);
            facts->flags  |= MVM_SPESH_FACT_KNOWN_TYPE | MVM_SPESH_FACT_KNOWN_VALUE;
            facts->type    = STABLE(log_obj)->WHAT;
            facts->value.o = (MVMObject *)log_obj;
            if (IS_CONCRETE(log_obj)) {
                facts->flags |= MVM_SPESH_FACT_CONCRETE;
                if (!STABLE(log_obj)->container_spec)
                    facts->flags |= MVM_SPESH_FACT_DECONTED;
            }
            else {
                facts->flags |= MVM_SPESH_FACT_TYPEOBJ;
            }
        }
    }
}

/* Determines if there's a matching spesh candidate for a callee and a given
 * set of argument info. */
static MVMint32 try_find_spesh_candidate(MVMThreadContext *tc, MVMCode *code, MVMSpeshCallInfo *arg_info) {
    MVMStaticFrameBody *sfb = &(code->body.sf->body);
    MVMint32 num_spesh      = sfb->num_spesh_candidates;
    MVMint32 i, j;
    for (i = 0; i < num_spesh; i++) {
        MVMSpeshCandidate *cand = &sfb->spesh_candidates[i];
        if (cand->cs == arg_info->cs) {
            /* Matching callsite, now see if we have enough information to
             * test the guards. */
            MVMint32 guard_failed = 0;
            for (j = 0; j < cand->num_guards; j++) {
                MVMint32       slot    = cand->guards[j].slot;
                MVMSpeshFacts *facts   = slot < MAX_ARGS_FOR_OPT ? arg_info->arg_facts[slot] : NULL;
                MVMSTable     *want_st = (MVMSTable *)cand->guards[j].match;
                if (!facts) {
                    guard_failed = 1;
                    break;
                }
                switch (cand->guards[j].kind) {
                case MVM_SPESH_GUARD_CONC:
                    if (!(facts->flags & MVM_SPESH_FACT_CONCRETE) ||
                            !(facts->flags & MVM_SPESH_FACT_KNOWN_TYPE) ||
                            STABLE(facts->type) != want_st)
                        guard_failed = 1;
                    break;
                case MVM_SPESH_GUARD_TYPE:
                    if (!(facts->flags & MVM_SPESH_FACT_TYPEOBJ) ||
                            !(facts->flags & MVM_SPESH_FACT_KNOWN_TYPE) ||
                            STABLE(facts->type) != want_st)
                        guard_failed = 1;
                    break;
                case MVM_SPESH_GUARD_DC_CONC:
                    if (!(facts->flags & MVM_SPESH_FACT_DECONT_CONCRETE) ||
                            !(facts->flags & MVM_SPESH_FACT_KNOWN_DECONT_TYPE) ||
                            STABLE(facts->decont_type) != want_st)
                        guard_failed = 1;
                    break;
                case MVM_SPESH_GUARD_DC_TYPE:
                    if (!(facts->flags & MVM_SPESH_FACT_DECONT_TYPEOBJ) ||
                            !(facts->flags & MVM_SPESH_FACT_KNOWN_DECONT_TYPE) ||
                            STABLE(facts->decont_type) != want_st)
                        guard_failed = 1;
                    break;
                default:
                    guard_failed = 1;
                    break;
                }
                if (guard_failed)
                    break;
            }
            if (!guard_failed)
                return i;
        }
    }
    return -1;
}

/* Drives optimization of a call. */
static void optimize_call(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshBB *bb,
                          MVMSpeshIns *ins, MVMint32 callee_idx, MVMSpeshCallInfo *arg_info) {
    /* Ensure we know what we're going to be invoking. */
    MVMSpeshFacts *callee_facts = MVM_spesh_get_and_use_facts(tc, g, ins->operands[callee_idx]);
    if (callee_facts->flags & MVM_SPESH_FACT_KNOWN_VALUE) {
        MVMObject *code   = callee_facts->value.o;
        MVMObject *target = NULL;
        if (REPR(code)->ID == MVM_REPR_ID_MVMCode) {
            /* Already have a code object we know we'll call. */
            target = code;
        }
        else if (STABLE(code)->invocation_spec) {
            /* What kind of invocation will it be? */
            MVMInvocationSpec *is = STABLE(code)->invocation_spec;
            if (!MVM_is_null(tc, is->md_class_handle)) {
                /* Multi-dispatch. Check if this is a dispatch where we can
                 * use the cache directly. */
                MVMRegister dest;
                REPR(code)->attr_funcs.get_attribute(tc,
                    STABLE(code), code, OBJECT_BODY(code),
                    is->md_class_handle, is->md_valid_attr_name,
                    is->md_valid_hint, &dest, MVM_reg_int64);
                if (dest.i64) {
                    /* Yes. Try to obtain the cache. */
                    REPR(code)->attr_funcs.get_attribute(tc,
                        STABLE(code), code, OBJECT_BODY(code),
                        is->md_class_handle, is->md_cache_attr_name,
                        is->md_cache_hint, &dest, MVM_reg_obj);
                    if (!MVM_is_null(tc, dest.o)) {
                        MVMObject *found = MVM_multi_cache_find_spesh(tc, dest.o, arg_info);
                        if (found) {
                            /* Found it. Is it a code object already, or do we
                             * have futher unpacking to do? */
                            if (REPR(found)->ID == MVM_REPR_ID_MVMCode) {
                                target = found;
                            }
                            else if (STABLE(found)->invocation_spec) {
                                MVMInvocationSpec *m_is = STABLE(found)->invocation_spec;
                                if (!MVM_is_null(tc, m_is->class_handle)) {
                                    REPR(found)->attr_funcs.get_attribute(tc,
                                        STABLE(found), found, OBJECT_BODY(found),
                                        is->class_handle, is->attr_name,
                                        is->hint, &dest, MVM_reg_obj);
                                    if (REPR(dest.o)->ID == MVM_REPR_ID_MVMCode)
                                        target = dest.o;
                                }
                            }
                        }
                    }
                }
            }
            else if (!MVM_is_null(tc, is->class_handle)) {
                /* Single dispatch; retrieve the code object. */
                MVMRegister dest;
                REPR(code)->attr_funcs.get_attribute(tc,
                    STABLE(code), code, OBJECT_BODY(code),
                    is->class_handle, is->attr_name,
                    is->hint, &dest, MVM_reg_obj);
                if (REPR(dest.o)->ID == MVM_REPR_ID_MVMCode)
                    target = dest.o;
            }
        }

        /* If we resolved to something better than the code object, then add
         * the resolved item in a spesh slot and insert a lookup. */
        if (target && target != code && !((MVMCode *)target)->body.is_compiler_stub) {
            MVMSpeshIns *pa_ins = arg_info->prepargs_ins;
            MVMSpeshIns *ss_ins = MVM_spesh_alloc(tc, g, sizeof(MVMSpeshIns));
            ss_ins->info        = MVM_op_get_op(MVM_OP_sp_getspeshslot);
            ss_ins->operands    = MVM_spesh_alloc(tc, g, 2 * sizeof(MVMSpeshOperand));
            ss_ins->operands[0] = ins->operands[callee_idx];
            ss_ins->operands[1].lit_i16 = MVM_spesh_add_spesh_slot(tc, g,
                (MVMCollectable *)target);
            /* Basically, we're inserting between arg* and invoke_*.
             * Since invoke_* directly uses the code in the register,
             * the register must have held the code during the arg*
             * instructions as well, because none of {prepargs, arg*}
             * can manipulate the register that holds the code.
             *
             * To make a long story very short, I think it should be
             * safe to move the sp_getspeshslot to /before/ the
             * prepargs instruction. And this is very convenient for
             * me, as it allows me to treat set of prepargs, arg*,
             * invoke, as a /single node/, and this greatly simplifies
             * invoke JIT compilation */

            MVM_spesh_manipulate_insert_ins(tc, bb, pa_ins->prev, ss_ins);
            /* XXX TODO: Do this differently so we can eliminate the original
             * lookup of the enclosing code object also. */
        }

        /* See if we can point the call at a particular specialization. */
        if (target) {
            MVMCode *target_code  = (MVMCode *)target;
            MVMint32 spesh_cand = try_find_spesh_candidate(tc, target_code, arg_info);
            if (spesh_cand >= 0) {
                /* Yes. Will we be able to inline? */
                MVMSpeshGraph *inline_graph = MVM_spesh_inline_try_get_graph(tc, g,
                    target_code, &target_code->body.sf->body.spesh_candidates[spesh_cand]);
                if (inline_graph) {
                    /* Yes, have inline graph, so go ahead and do it. */
                    /*char *c_name_i = MVM_string_utf8_encode_C_string(tc, target_code->body.sf->body.name);
                    char *c_cuid_i = MVM_string_utf8_encode_C_string(tc, target_code->body.sf->body.cuuid);
                    char *c_name_t = MVM_string_utf8_encode_C_string(tc, g->sf->body.name);
                    char *c_cuid_t = MVM_string_utf8_encode_C_string(tc, g->sf->body.cuuid);
                    printf("Can inline %s (%s) into %s (%s)\n",
                        c_name_i, c_cuid_i, c_name_t, c_cuid_t);
                    free(c_name_i);
                    free(c_cuid_i);
                    free(c_name_t);
                    free(c_cuid_t);*/
                    MVM_spesh_inline(tc, g, arg_info, bb, ins, inline_graph, target_code);
                }
                else {
                    /* Can't inline, so just identify candidate. */
                    MVMSpeshOperand *new_operands = MVM_spesh_alloc(tc, g, 3 * sizeof(MVMSpeshOperand));
                    if (ins->info->opcode == MVM_OP_invoke_v) {
                        new_operands[0]         = ins->operands[0];
                        new_operands[1].lit_i16 = spesh_cand;
                        ins->operands           = new_operands;
                        ins->info               = MVM_op_get_op(MVM_OP_sp_fastinvoke_v);
                    }
                    else {
                        new_operands[0]         = ins->operands[0];
                        new_operands[1]         = ins->operands[1];
                        new_operands[2].lit_i16 = spesh_cand;
                        ins->operands           = new_operands;
                        switch (ins->info->opcode) {
                        case MVM_OP_invoke_i:
                            ins->info = MVM_op_get_op(MVM_OP_sp_fastinvoke_i);
                            break;
                        case MVM_OP_invoke_n:
                            ins->info = MVM_op_get_op(MVM_OP_sp_fastinvoke_n);
                            break;
                        case MVM_OP_invoke_s:
                            ins->info = MVM_op_get_op(MVM_OP_sp_fastinvoke_s);
                            break;
                        case MVM_OP_invoke_o:
                            ins->info = MVM_op_get_op(MVM_OP_sp_fastinvoke_o);
                            break;
                        default:
                            MVM_exception_throw_adhoc(tc, "Spesh: unhandled invoke instruction");
                        }
                    }
                }
            }
        }
    }
}

/* Optimizes an extension op. */
static void optimize_extop(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshBB *bb, MVMSpeshIns *ins) {
    MVMExtOpRecord *extops     = g->sf->body.cu->body.extops;
    MVMuint16       num_extops = g->sf->body.cu->body.num_extops;
    MVMuint16       i;
    for (i = 0; i < num_extops; i++) {
        if (extops[i].info == ins->info) {
            /* Found op; call its spesh function, if any. */
            if (extops[i].spesh)
                extops[i].spesh(tc, g, bb, ins);
            return;
        }
    }
}

/* Tries to optimize a throwcat instruction. Note that within a given frame
 * (we don't consider inlines here) the throwcat instructions all have the
 * same semantics. */
static void optimize_throwcat(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshBB *bb, MVMSpeshIns *ins) {
    /* First, see if we have any goto handlers for this category. */
    MVMint32 *handlers_found = malloc(g->sf->body.num_handlers * sizeof(MVMint32));
    MVMint32  num_found      = 0;
    MVMuint32 category       = (MVMuint32)ins->operands[1].lit_i64;
    MVMint32  i;
    for (i = 0; i < g->sf->body.num_handlers; i++)
        if (g->sf->body.handlers[i].action == MVM_EX_ACTION_GOTO)
            if (g->sf->body.handlers[i].category_mask & category)
                handlers_found[num_found++] = i;

    /* If we found any appropriate handlers, we'll now do a scan through the
     * graph to see if we're in the scope of any of them. Note we can't keep
     * track of this in optimize_bb as it walks the dominance children, but
     * we need a linear view. */
    if (num_found) {
        MVMint32    *in_handlers = calloc(g->sf->body.num_handlers, sizeof(MVMint32));
        MVMSpeshBB **goto_bbs    = calloc(g->sf->body.num_handlers, sizeof(MVMSpeshBB *));
        MVMSpeshBB  *search_bb   = g->entry;
        MVMint32     picked      = -1;
        while (search_bb) {
            MVMSpeshIns *search_ins = search_bb->first_ins;
            while (search_ins) {
                /* Track handlers. */
                MVMSpeshAnn *ann = search_ins->annotations;
                while (ann) {
                    switch (ann->type) {
                    case MVM_SPESH_ANN_FH_START:
                        in_handlers[ann->data.frame_handler_index] = 1;
                        break;
                    case MVM_SPESH_ANN_FH_END:
                        in_handlers[ann->data.frame_handler_index] = 0;
                        break;
                    case MVM_SPESH_ANN_FH_GOTO:
                        goto_bbs[ann->data.frame_handler_index] = search_bb;
                        if (picked >= 0 && ann->data.frame_handler_index == picked)
                            goto search_over;
                        break;
                    }
                    ann = ann->next;
                }

                /* Is this instruction the one we're trying to optimize? */
                if (search_ins == ins) {
                    /* See if we're in any acceptable handler (rely on the
                     * table being pre-sorted by nesting depth here, just like
                     * normal exception handler search does). */
                    for (i = 0; i < num_found; i++) {
                        if (in_handlers[handlers_found[i]]) {
                            /* Got it! If we already found its goto target, we
                             * can finish the search. */
                            picked = handlers_found[i];
                            if (goto_bbs[picked])
                                goto search_over;
                            break;
                        }
                    }
                }

                search_ins = search_ins->next;
            }
            search_bb = search_bb->linear_next;
        }
      search_over:

        /* If we picked a handler and know where it should goto, we can do the
         * rewrite into a goto. */
        if (picked >=0 && goto_bbs[picked]) {
            ins->info               = MVM_op_get_op(MVM_OP_goto);
            ins->operands[0].ins_bb = goto_bbs[picked];
            bb->succ[0]             = goto_bbs[picked];
        }

        free(in_handlers);
        free(goto_bbs);
    }

    free(handlers_found);
}

/* Visits the blocks in dominator tree order, recursively. */
static void optimize_bb(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshBB *bb) {
    MVMSpeshCallInfo arg_info;
    MVMint32 i;

    /* Look for instructions that are interesting to optimize. */
    MVMSpeshIns *ins = bb->first_ins;
    while (ins) {
        switch (ins->info->opcode) {
        case MVM_OP_set:
            copy_facts(tc, g, ins->operands[0], ins->operands[1]);
            break;
        case MVM_OP_istrue:
        case MVM_OP_isfalse:
            optimize_istrue_isfalse(tc, g, bb, ins);
            break;
        case MVM_OP_if_i:
        case MVM_OP_unless_i:
        case MVM_OP_if_n:
        case MVM_OP_unless_n:
        case MVM_OP_if_o:
        case MVM_OP_unless_o:
            optimize_iffy(tc, g, ins, bb);
            break;
        case MVM_OP_prepargs:
            arg_info.cs = g->sf->body.cu->body.callsites[ins->operands[0].callsite_idx];
            arg_info.prepargs_ins = ins;
            break;
        case MVM_OP_arg_i:
        case MVM_OP_arg_n:
        case MVM_OP_arg_s:
        case MVM_OP_arg_o: {
            MVMint16 idx = ins->operands[0].lit_i16;
            if (idx < MAX_ARGS_FOR_OPT) {
                arg_info.arg_is_const[idx] = 0;
                arg_info.arg_facts[idx]    = MVM_spesh_get_and_use_facts(tc, g, ins->operands[1]);
                arg_info.arg_ins[idx]      = ins;
            }
            break;
        }
        case MVM_OP_argconst_i:
        case MVM_OP_argconst_n:
        case MVM_OP_argconst_s: {
            MVMint16 idx = ins->operands[0].lit_i16;
            if (idx < MAX_ARGS_FOR_OPT) {
                arg_info.arg_is_const[idx] = 1;
                arg_info.arg_ins[idx]      = ins;
            }
            break;
        }
        case MVM_OP_coerce_in:
            optimize_coerce(tc, g, bb, ins);
            break;
        case MVM_OP_smrt_numify:
        case MVM_OP_smrt_strify:
            optimize_smart_coerce(tc, g, bb, ins);
            break;
        case MVM_OP_invoke_v:
            optimize_call(tc, g, bb, ins, 0, &arg_info);
            break;
        case MVM_OP_invoke_i:
        case MVM_OP_invoke_n:
        case MVM_OP_invoke_s:
        case MVM_OP_invoke_o:
            optimize_call(tc, g, bb, ins, 1, &arg_info);
            break;
        case MVM_OP_throwcatdyn:
        case MVM_OP_throwcatlex:
        case MVM_OP_throwcatlexotic:
            optimize_throwcat(tc, g, bb, ins);
            break;
        case MVM_OP_islist:
        case MVM_OP_ishash:
        case MVM_OP_isint:
        case MVM_OP_isnum:
        case MVM_OP_isstr:
            optimize_is_reprid(tc, g, ins);
            break;
        case MVM_OP_findmeth:
            optimize_method_lookup(tc, g, ins);
            break;
        case MVM_OP_can:
        case MVM_OP_can_s:
            break; /* XXX This causes problems, Spesh: failed to fix up handlers (-1, 110, 110) */
            /* optimize_can_op(tc, g, bb, ins);
            break; */
        case MVM_OP_create:
            optimize_repr_op(tc, g, bb, ins, 1);
            break;
        case MVM_OP_isconcrete:
            optimize_isconcrete(tc, g, ins);
            break;
        case MVM_OP_istype:
            optimize_istype(tc, g, ins);
            break;
        case MVM_OP_objprimspec:
            optimize_objprimspec(tc, g, ins);
            break;
        case MVM_OP_bindattr_i:
        case MVM_OP_bindattr_n:
        case MVM_OP_bindattr_s:
        case MVM_OP_bindattr_o:
        case MVM_OP_bindattrs_i:
        case MVM_OP_bindattrs_n:
        case MVM_OP_bindattrs_s:
        case MVM_OP_bindattrs_o:
            optimize_repr_op(tc, g, bb, ins, 0);
            break;
        case MVM_OP_getattr_i:
        case MVM_OP_getattr_n:
        case MVM_OP_getattr_s:
        case MVM_OP_getattr_o:
        case MVM_OP_getattrs_i:
        case MVM_OP_getattrs_n:
        case MVM_OP_getattrs_s:
        case MVM_OP_getattrs_o:
            optimize_repr_op(tc, g, bb, ins, 1);
            break;
        case MVM_OP_box_i:
        case MVM_OP_box_n:
        case MVM_OP_box_s:
            optimize_repr_op(tc, g, bb, ins, 2);
            break;
        case MVM_OP_unbox_i:
        case MVM_OP_unbox_n:
        case MVM_OP_unbox_s:
            optimize_repr_op(tc, g, bb, ins, 1);
            break;
        case MVM_OP_elems:
            optimize_repr_op(tc, g, bb, ins, 1);
            break;
        case MVM_OP_hllize:
            optimize_hllize(tc, g, ins);
            break;
        case MVM_OP_decont:
            optimize_decont(tc, g, bb, ins);
            break;
        case MVM_OP_assertparamcheck:
            optimize_assertparamcheck(tc, g, bb, ins);
            break;
        case MVM_OP_getlexstatic_o:
            optimize_getlex_known(tc, g, bb, ins);
            break;
        case MVM_OP_getlexperinvtype_o:
            if (specialized_on_invocant(tc, g))
                optimize_getlex_known(tc, g, bb, ins);
            break;
        case MVM_OP_sp_log:
        case MVM_OP_sp_osrfinalize:
            /* Left-over log instruction that didn't become a guard, or OSR
             * finalize instruction; just delete it. */
            MVM_spesh_manipulate_delete_ins(tc, g, bb, ins);
            break;
        default:
            if (ins->info->opcode == (MVMuint16)-1)
                optimize_extop(tc, g, bb, ins);
        }
        ins = ins->next;
    }

    /* Visit children. */
    for (i = 0; i < bb->num_children; i++)
        optimize_bb(tc, g, bb->children[i]);
}

/* Eliminates any unused instructions. */
static void eliminate_dead_ins(MVMThreadContext *tc, MVMSpeshGraph *g) {
    /* Keep eliminating to a fixed point. */
    MVMint8 death = 1;
    while (death) {
        MVMSpeshBB *bb = g->entry;
        death = 0;
        while (bb && !bb->inlined) {
            MVMSpeshIns *ins = bb->last_ins;
            while (ins) {
                MVMSpeshIns *prev = ins->prev;
                if (ins->info->opcode == MVM_SSA_PHI) {
                    MVMSpeshFacts *facts = get_facts_direct(tc, g, ins->operands[0]);
                    if (facts->usages == 0) {
                        /* Propagate non-usage. */
                        MVMint32 i;
                        for (i = 1; i < ins->info->num_operands; i++)
                            get_facts_direct(tc, g, ins->operands[i])->usages--;

                        /* Remove this phi. */
                        MVM_spesh_manipulate_delete_ins(tc, g, bb, ins);
                        death = 1;
                    }
                }
                else if (ins->info->pure) {
                    /* Sanity check to make sure it's a write reg as first operand. */
                    if ((ins->info->operands[0] & MVM_operand_rw_mask) == MVM_operand_write_reg) {
                        MVMSpeshFacts *facts = get_facts_direct(tc, g, ins->operands[0]);
                        if (facts->usages == 0) {
                            /* Propagate non-usage. */
                            MVMint32 i;
                            for (i = 1; i < ins->info->num_operands; i++)
                                if ((ins->info->operands[i] & MVM_operand_rw_mask) == MVM_operand_read_reg)
                                    get_facts_direct(tc, g, ins->operands[i])->usages--;

                            /* Remove this instruction. */
                            MVM_spesh_manipulate_delete_ins(tc, g, bb, ins);
                            death = 1;
                        }
                    }
                }
                ins = prev;
            }
            bb = bb->linear_next;
        }
    }
}

/* Eliminates any unreachable basic blocks (that is, dead code). Not having
 * to consider them any further simplifies all that follows. */
static void eliminate_dead_bbs(MVMThreadContext *tc, MVMSpeshGraph *g) {
    /* Iterate to fixed point. */
    MVMint8  *seen     = malloc(g->num_bbs);
    MVMint32  orig_bbs = g->num_bbs;
    MVMint8   death    = 1;
    while (death) {
        /* First pass: mark every basic block that is the entry point or the
         * successor of some other block. */
        MVMSpeshBB *cur_bb = g->entry;
        memset(seen, 0, g->num_bbs);
        seen[0] = 1;
        while (cur_bb) {
            MVMuint16 i;
            for (i = 0; i < cur_bb->num_succ; i++)
                seen[cur_bb->succ[i]->idx] = 1;
            cur_bb = cur_bb->linear_next;
        }

        /* Second pass: eliminate dead BBs from consideration. */
        death = 0;
        cur_bb = g->entry;
        while (cur_bb->linear_next) {
            if (!seen[cur_bb->linear_next->idx]) {
                if (!cur_bb->linear_next->inlined) {
                    cur_bb->linear_next = cur_bb->linear_next->linear_next;
                    g->num_bbs--;
                    death = 1;
                }
            }
            cur_bb = cur_bb->linear_next;
        }
    }
    free(seen);

    if (g->num_bbs != orig_bbs) {
        MVMint32    new_idx  = 0;
        MVMSpeshBB *cur_bb   = g->entry;
        while (cur_bb) {
            cur_bb->idx = new_idx;
            new_idx++;
            cur_bb = cur_bb->linear_next;
        }
    }
}

/* Goes through the various log-based guard instructions and removes any that
 * are not being made use of. */
void eliminate_unused_log_guards(MVMThreadContext *tc, MVMSpeshGraph *g) {
    MVMint32 i;
    for (i = 0; i < g->num_log_guards; i++)
        if (!g->log_guards[i].used)
            MVM_spesh_manipulate_delete_ins(tc, g, g->log_guards[i].bb,
                g->log_guards[i].ins);
}

/* Drives the overall optimization work taking place on a spesh graph. */
void MVM_spesh_optimize(MVMThreadContext *tc, MVMSpeshGraph *g) {
    optimize_bb(tc, g, g->entry);
    eliminate_dead_ins(tc, g);
    eliminate_dead_bbs(tc, g);
    eliminate_unused_log_guards(tc, g);
}
