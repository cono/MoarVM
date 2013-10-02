#include "moarvm.h"

static const MVMContainerSpec container_spec;

static void set_container_spec(MVMThreadContext *tc, MVMSTable *st) {
    if (st->REPR->ID != MVM_REPR_ID_CScalar)
        MVM_exception_throw_adhoc(tc,
                "can only make C scalar objects into CFPtr containers");

    st->container_spec = &container_spec;
    st->container_data = NULL;
}

static void configure_container_spec(MVMThreadContext *tc, MVMSTable *st,
        MVMObject *config) {
    /* noop */
}

const MVMContainerConfigurer MVM_CONTAINER_CONF_CFPtr = {
    set_container_spec,
    configure_container_spec
};