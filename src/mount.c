/* This is a managed file. Do not delete this comment. */

#include <driver/mnt/fieldbus/fieldbus.h>

static
void add_to_register_map(
    fieldbus_mount this,
    uint64_t key,
    corto_object instance,
    corto_type type,
    void *ptr)
{
    corto_trace("add register '%u' for '%s' of type '%s'",
        key, corto_fullpath(NULL, instance), corto_fullpath(NULL, type));

    register_helper *el = corto_ptr_new(register_helper_o);
    corto_set_ref(&el->instance, instance);
    corto_set_ref(&el->type, type);
    el->ptr = (uintptr_t)ptr;
    corto_rb_set(this->register_map, (void*)(uintptr_t)key, el);
}

static
void remove_from_register_map(
    fieldbus_mount this,
    uint64_t key)
{
    register_helper *el = corto_rb_remove(this->register_map, (void*)(uintptr_t)key);
    if (el) {
        corto_trace("remove register '%u' for '%s' of type '%s'", key);
        corto_ptr_free(el, register_helper_o);
    }
}

static
void add_instance(
    fieldbus_mount this,
    fieldbus_instance config)
{
    /* Create object of instance type in mount scope with same id as instance */
    corto_object data_object = corto_create(
        root_o,
        strarg(
            "%s/%s",
            corto_subscriber(this)->query.from,
            corto_idof(config)
        ),
        config
    );

    if (!data_object) {
        corto_error("failed to create data object for instance '%s'",
            corto_fullpath(NULL, config));
        return;
    }

    corto_trace("created data object '%s' of type '%s'",
        corto_fullpath(NULL, data_object),
        corto_fullpath(NULL, config));

    /* Walk over members of instance and add them to the register map of the
     * mount. Since registers can only be of primitive types, there is no need
     * for a more complex API, like corto_metawalk. */
    int i;
    for (i = 0; i < corto_interface(config)->members.length; i ++) {
        corto_member m = corto_interface(config)->members.buffer[i];

        /* If instance contains const, readonly or private members, do not add
         * those to the register map. */
        if (!(m->modifiers & (CORTO_CONST | CORTO_READONLY | CORTO_PRIVATE))) {
            /* Make sure it is a register member */
            if (corto_instanceof(fieldbus_register_o, m)) {
                fieldbus_register r = fieldbus_register(m);
                /* Add entry to register map */
                add_to_register_map(
                    this,
                    config->index + r->offset,
                    data_object,
                    m->type,
                    CORTO_OFFSET(data_object, m->offset));
            }
        }
    }
}

static
void remove_instance(
    fieldbus_mount this,
    fieldbus_instance config)
{
    /* Create object of instance type in mount scope with same id as instance */
    corto_object data_object = corto_lookup(
        root_o,
        strarg(
            "%s/%s",
            corto_subscriber(this)->query.from,
            corto_idof(config)
        )
    );

    if (!data_object) {
        /* Nothing to remove */
        return;
    }

    /* Delete object */
    corto_delete(data_object);

    /* Walk over members of instance and remove them from the register map of the
     * mount. Since registers can only be of primitive types, there is no need
     * for a more complex API, like corto_metawalk. */
    int i;
    for (i = 0; i < corto_interface(config)->members.length; i ++) {
        corto_member m = corto_interface(config)->members.buffer[i];
        /* If instance contains const, readonly or private members, do not add
         * those to the register map. */
        if (!(m->modifiers & (CORTO_CONST | CORTO_READONLY | CORTO_PRIVATE))) {
            /* Make sure it is a register member */
            if (corto_instanceof(fieldbus_register_o, m)) {
                fieldbus_register r = fieldbus_register(m);
                /* Add entry to register map */
                remove_from_register_map(this, config->index + r->offset);
            }
        }
    }
}

int16_t fieldbus_mount_construct(
    fieldbus_mount this)
{
    /* Listen to new, modified and deleted configuration in mount scope */
    if (corto_observer_observe(fieldbus_mount_config_observer_o, this, this)) {
        goto error;
    }

    return 0;
error:
    return -1;
}

void fieldbus_mount_config_observer(
    corto_observerEvent *e)
{
    /* React to configuration changes */
    switch (e->event) {
    case CORTO_DEFINE:
        add_instance(e->instance, e->data);
        break;
    case CORTO_UPDATE:
        remove_instance(e->instance, e->data);
        add_instance(e->instance, e->data);
        break;
    case CORTO_DELETE:
        remove_instance(e->instance, e->data);
        break;
    default:
        /* Ignore */
        break;
    }
}

void fieldbus_mount_simulate_event(
    fieldbus_mount this,
    uint32_t _register,
    uintptr_t binary_data)
{
    register_helper *el = corto_rb_find(this->register_map, &_register);
    if (!el) {
        /* Received data for register that is not in config, ignore */
        return;
    }

    uint32_t size = 0;

    /* Copy data into object */
    switch(el->type->width) {
    case CORTO_WIDTH_8: size = sizeof(int8_t); break;
    case CORTO_WIDTH_16: size = sizeof(int16_t); break;
    case CORTO_WIDTH_32: size = sizeof(int32_t); break;
    case CORTO_WIDTH_64: size = sizeof(int64_t); break;
    case CORTO_WIDTH_WORD: size = sizeof(uintptr_t); break;
        break;
    }

    /* If endianness is different between data provided by fieldbus and node on
     * which the mount is run, flip endianness here */

    /* Copy value into member & update object */
    if (corto_update_begin(el->instance)) {
        corto_throw(
            "failed to begin updating '%s'",
            corto_fullpath(NULL, el->instance));
        corto_raise();
        return;
    }

    memcpy((void*)el->ptr, (void*)binary_data, size);

    if (corto_update_end(el->instance)) {
        corto_throw(
            "failed to update '%s'", corto_fullpath(NULL, el->instance));
        corto_raise();
        return;
    }
}
