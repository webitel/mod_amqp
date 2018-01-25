#include "mod_amqp.h"
#include "switch.h"


static switch_state_handler_table_t state_handlers = {
        /*.on_init */ NULL,
        /*.on_routing */ NULL,
        /*.on_execute */ NULL,
        /*.on_hangup */ NULL,
        /*.on_exchange_media */ NULL,
        /*.on_soft_execute */ NULL,
        /*.on_consume_media */ NULL,
        /*.on_hibernate */ NULL,
        /*.on_reset */ NULL,
        /*.on_park */ NULL,
        /*.on_reporting */ mod_amqp_cdr_reporting
};


static switch_status_t mod_amqp_cdr_routing_key(int is_b, char routingKey[MAX_AMQP_ROUTING_KEY_LENGTH],
                                         switch_channel_t* channel, mod_amqp_keypart_t routingKeyEventHeaderNames[]) {

    int i = 0, idx = 0, x = 0;
    char keybuffer[MAX_AMQP_ROUTING_KEY_LENGTH];

    for (i = 0; i < MAX_ROUTING_KEY_FORMAT_FIELDS && idx < MAX_AMQP_ROUTING_KEY_LENGTH; i++) {
        if (routingKeyEventHeaderNames[i].size) {
            if (idx) {
                routingKey[idx++] = '.';
            }
            for( x = 0; x < routingKeyEventHeaderNames[i].size; x++) {
                if (routingKeyEventHeaderNames[i].name[x][0] == '#') {
                    strncpy(routingKey + idx, routingKeyEventHeaderNames[i].name[x] + 1, MAX_AMQP_ROUTING_KEY_LENGTH - idx);
                    break;
                } else if (!strncmp(routingKeyEventHeaderNames[i].name[x], "LEG", 3)) {
                    strncpy(routingKey + idx, is_b ? "b" : "a", MAX_AMQP_ROUTING_KEY_LENGTH - idx);
                    break;
                } else {
                    char *value = (char *)switch_channel_get_variable(channel, routingKeyEventHeaderNames[i].name[x]);
                    if (value) {
                        amqp_util_encode(value, keybuffer);
                        strncpy(routingKey + idx, keybuffer, MAX_AMQP_ROUTING_KEY_LENGTH - idx);
                        break;
                    }
                }
            }
            idx += strlen(routingKey + idx);
        }
    }

    return SWITCH_STATUS_SUCCESS;
}

/* This should only be called in a single threaded context from the cdr profile send thread */
switch_status_t mod_amqp_cdr_send(mod_amqp_cdr_profile_t *profile, mod_amqp_message_t *msg)
{
    amqp_table_entry_t messageTableEntries[1];
    amqp_basic_properties_t props;
    int status;

    if (!profile->conn_active) {
        /* No connection, so we can not send the message. */
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Profile[%s] not active\n", profile->name);
        return SWITCH_STATUS_NOT_INITALIZED;
    }
    memset(&props, 0, sizeof(amqp_basic_properties_t));

    props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG;
    props.content_type = amqp_cstring_bytes("application/json");

    if(profile->delivery_mode > 0) {
        props._flags |= AMQP_BASIC_DELIVERY_MODE_FLAG;
        props.delivery_mode = profile->delivery_mode;
    }

    if (profile->enable_fallback_format_fields) {
        amqp_array_t inner_array;
        amqp_field_value_t inner_values[1];

        props._flags |= AMQP_BASIC_HEADERS_FLAG;
        props.headers.num_entries = 1;

        inner_values[0].kind = AMQP_FIELD_KIND_UTF8;
        inner_values[0].value.bytes = amqp_cstring_bytes(msg->cc_routing_key);
        inner_array.num_entries = 1;
        inner_array.entries = inner_values;
        props.headers.num_entries = 1;
        messageTableEntries[0].key = amqp_cstring_bytes("BCC");
        messageTableEntries[0].value.kind = AMQP_FIELD_KIND_ARRAY;
        messageTableEntries[0].value.value.array = inner_array;
        props.headers.entries = messageTableEntries;
    }

    status = amqp_basic_publish(
            profile->conn_active->state,
            1,
            amqp_cstring_bytes(profile->exchange),
            amqp_cstring_bytes(msg->routing_key),
            0,
            0,
            &props,
            amqp_cstring_bytes(msg->pjson));

    if (status < 0) {
        const char *errstr = amqp_error_string2(-status);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Profile[%s] failed to send event on connection[%s]: %s\n",
                          profile->name, profile->conn_active->name, errstr);

        /* This is bad, we couldn't send the message. Clear up any connection */
        mod_amqp_connection_close(profile->conn_active);
        profile->conn_active = NULL;
        return SWITCH_STATUS_SOCKERR;
    }

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t mod_amqp_cdr_reporting(switch_core_session_t *session)
{
    switch_hash_index_t *hi = NULL;
    switch_channel_t *channel = switch_core_session_get_channel(session);
    int is_b;
    int snd = 0;
    mod_amqp_cdr_profile_t *cdr = NULL;
    mod_amqp_message_t *msg = NULL;
    cJSON *json_cdr = NULL;
    char *json_text = NULL;

    is_b = channel && switch_channel_get_originator_caller_profile(channel);

    if (switch_ivr_generate_json_cdr(session, &json_cdr, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error Generating Data!\n");
        return SWITCH_STATUS_FALSE;
    }

    json_text = cJSON_PrintUnformatted(json_cdr);

    /*
      1. Loop through cdr hash of profiles. Check for a profile that accepts this logging level, and file regex.
      2. If event not already parsed/created, then create it now
      3. Queue copy of event into cdr profile send queue
      4. Destroy local event copy
    */
    for (hi = switch_core_hash_first(mod_amqp_globals.cdr_hash); hi; hi = switch_core_hash_next(&hi)) {
        switch_core_hash_this(hi, NULL, NULL, (void **) &cdr);

        if (cdr) {
            /* Create message */
            switch_malloc(msg, sizeof(mod_amqp_message_t));
            msg->pjson = strdup(json_text);

            if (cdr->enable_fallback_format_fields) {
                mod_amqp_cdr_routing_key(is_b, msg->cc_routing_key, channel, cdr->format_fields);
            }

            snprintf(msg->routing_key, sizeof(msg->routing_key), "%s", is_b ? cdr->binding_key_b : cdr->binding_key_a);

            if (switch_queue_trypush(cdr->send_queue, msg) != SWITCH_STATUS_SUCCESS) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT,
                                  "AMQP cdr message queue full. Messages will be dropped!\n");
                switch_safe_free(hi);
                goto done;
            }
            snd++;
            break;
        }
    }

done:
    switch_safe_free(json_text);
    if (json_cdr) {
        cJSON_Delete(json_cdr);
    }
    if (snd == 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Error sending data!\n");
        return SWITCH_STATUS_FALSE;
    } else {
        return SWITCH_STATUS_SUCCESS;
    }
}

switch_status_t mod_amqp_cdr_destroy(mod_amqp_cdr_profile_t **prof)
{
    mod_amqp_message_t *msg = NULL;
    switch_status_t status = SWITCH_STATUS_SUCCESS;
    mod_amqp_connection_t *conn = NULL, *conn_next = NULL;
    switch_memory_pool_t *pool;
    mod_amqp_cdr_profile_t *profile;

    if (!prof || !*prof) {
        return SWITCH_STATUS_SUCCESS;
    }

    profile = *prof;
    pool = profile->pool;

    if (profile->name) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Profile[%s] shutting down...\n", profile->name);
        switch_core_hash_delete(mod_amqp_globals.cdr_hash, profile->name);
    }

    profile->running = 0;

    if (profile->cdr_thread) {
        switch_thread_join(&status, profile->cdr_thread);
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Profile[%s] closing AMQP socket...\n", profile->name);

    for (conn = profile->conn_root; conn; conn = conn_next) {
        conn_next = conn->next;
        mod_amqp_connection_destroy(&conn);
    }

    profile->conn_active = NULL;
    profile->conn_root = NULL;

    while (profile->send_queue && switch_queue_trypop(profile->send_queue, (void**)&msg) == SWITCH_STATUS_SUCCESS) {
        mod_amqp_util_msg_destroy(&msg);
    }

    if (pool) {
        switch_core_destroy_memory_pool(&pool);
    }

    *prof = NULL;

    switch_core_remove_state_handler(&state_handlers);

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t mod_amqp_cdr_create(char *name, switch_xml_t cfg)
{
    mod_amqp_cdr_profile_t *profile = NULL;

    switch_xml_t params, param, connections, connection;
    switch_threadattr_t *thd_attr = NULL;
    char *exchange = NULL, *exchange_type = NULL, *queue_leg_a = NULL, *queue_leg_b = NULL;
    char *format_fields[MAX_ROUTING_KEY_FORMAT_FIELDS+1];
    int format_fields_size = 0;

    int exchange_durable = 1; /* durable */
    int delivery_mode = -1;
    int arg = 0, i = 0;
    switch_memory_pool_t *pool;

    if (switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
        goto err;
    }

    profile = switch_core_alloc(pool, sizeof(mod_amqp_cdr_profile_t));
    profile->pool = pool;
    profile->name = switch_core_strdup(profile->pool, name);
    profile->running = 1;

    profile->conn_root   = NULL;
    profile->conn_active = NULL;
    /* Set reasonable defaults which may change if more reasonable defaults are found */
    /* Handle defaults of non string types */
    profile->circuit_breaker_ms = 10000;
    profile->reconnect_interval_ms = 1000;
    profile->send_queue_size = 5000;

    memset(profile->format_fields, 0, (MAX_ROUTING_KEY_FORMAT_FIELDS + 1) * sizeof(mod_amqp_keypart_t));
    memset(format_fields, 0, sizeof(format_fields));

    if ((params = switch_xml_child(cfg, "params")) != NULL) {
        for (param = switch_xml_child(params, "param"); param; param = param->next) {
            char *var = (char *) switch_xml_attr_soft(param, "name");
            char *val = (char *) switch_xml_attr_soft(param, "value");

            if (!var) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Profile[%s] param missing 'name' attribute\n", profile->name);
                continue;
            }

            if (!val) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Profile[%s] param[%s] missing 'value' attribute\n", profile->name, var);
                continue;
            }

            if (!strncmp(var, "reconnect_interval_ms", 21)) {
                int interval = atoi(val);
                if ( interval && interval > 0 ) {
                    profile->reconnect_interval_ms = interval;
                }
            } else if (!strncmp(var, "circuit_breaker_ms", 18)) {
                int interval = atoi(val);
                if ( interval && interval > 0 ) {
                    profile->circuit_breaker_ms = interval;
                }
            } else if (!strncmp(var, "send_queue_size", 15)) {
                int interval = atoi(val);
                if ( interval && interval > 0 ) {
                    profile->send_queue_size = (unsigned int)interval;
                }
            } else if (!strncmp(var, "exchange-type", 13)) {
                exchange_type = switch_core_strdup(profile->pool, val);
            } else if (!strncmp(var, "exchange-name", 13)) {
                exchange = switch_core_strdup(profile->pool, val);
            } else if (!strncmp(var, "exchange-durable", 16)) {
                exchange_durable = switch_true(val);
            } else if (!strncmp(var, "delivery-mode", 13)) {
                delivery_mode = atoi(val);
            } else if (!strncmp(var, "exchange_type", 13)) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Found exchange_type parameter. please change to exchange-type\n");
            } else if (!strncmp(var, "queue-name-leg-a", 16)) {
                queue_leg_a = switch_core_strdup(profile->pool, val);
            } else if (!strncmp(var, "queue-name-leg-b", 16)) {
                queue_leg_b = switch_core_strdup(profile->pool, val);
            } else if (!strncmp(var, "exchange", 8)) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Found exchange parameter. please change to exchange-name\n");
            } else if (!strncmp(var, "enable_fallback_format_fields", 29)) {
                int interval = atoi(val);
                if ( interval && interval > 0 ) {
                    profile->enable_fallback_format_fields = 1;
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "amqp fallback format fields enabled\n");
                }
            } else if (!strncmp(var, "format_fields", 13)) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "amqp format fields : %s\n", val);
                if ((format_fields_size = mod_amqp_count_chars(val, ',')) >= MAX_ROUTING_KEY_FORMAT_FIELDS) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "You can have only %d routing fields in the routing key.\n",
                                      MAX_ROUTING_KEY_FORMAT_FIELDS);
                    goto err;
                }

                /* increment size because the count returned the number of separators, not number of fields */
                format_fields_size++;
                switch_separate_string(val, ',', format_fields, MAX_ROUTING_KEY_FORMAT_FIELDS);
                format_fields[format_fields_size] = NULL;
            }
        } /* params for loop */
    }


    /* Handle defaults of string types */
    profile->exchange = exchange ? exchange : switch_core_strdup(profile->pool, "TAP.Events");
    profile->exchange_type = exchange_type ? exchange_type : switch_core_strdup(profile->pool, "topic");
    profile->exchange_durable = exchange_durable;
    profile->delivery_mode = delivery_mode;

    profile->queue_name_a = queue_leg_a ? queue_leg_a : "cdr-leg-a";
    profile->queue_name_b = queue_leg_b ? queue_leg_b : "cdr-leg-b";

    for(i = 0; i < format_fields_size; i++) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "amqp routing key %d : %s\n", i, format_fields[i]);
        if(profile->enable_fallback_format_fields) {
            profile->format_fields[i].size = switch_separate_string(format_fields[i], '|', profile->format_fields[i].name, MAX_ROUTING_KEY_FORMAT_FALLBACK_FIELDS);
            if(profile->format_fields[i].size > 1) {
                for(arg = 0; arg < profile->format_fields[i].size; arg++) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "amqp routing key %d : sub key %d : %s\n", i, arg, profile->format_fields[i].name[arg]);
                }
            }
        }
    }

    if ((connections = switch_xml_child(cfg, "connections")) != NULL) {
        for (connection = switch_xml_child(connections, "connection"); connection; connection = connection->next) {
            if ( ! profile->conn_root ) { /* Handle first root node */
                if (mod_amqp_connection_create(&(profile->conn_root), connection, profile->pool) != SWITCH_STATUS_SUCCESS) {
                    /* Handle connection create failure */
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Profile[%s] failed to create connection\n", profile->name);
                    continue;
                }
                profile->conn_active = profile->conn_root;
            } else {
                if (mod_amqp_connection_create(&(profile->conn_active->next), connection, profile->pool) != SWITCH_STATUS_SUCCESS) {
                    /* Handle connection create failure */
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Profile[%s] failed to create connection\n", profile->name);
                    continue;
                }
                profile->conn_active = profile->conn_active->next;
            }
        }
    }

    profile->conn_active = NULL;
    /* Create a bounded FIFO queue for sending messages */
    if (switch_queue_create(&(profile->send_queue), profile->send_queue_size, profile->pool) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Cannot create send queue of size %d!\n",
                          profile->send_queue_size);
        goto err;
    }

    switch_threadattr_create(&thd_attr, profile->pool);
    switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);

    if (switch_thread_create(&profile->cdr_thread, thd_attr, mod_amqp_cdr_thread, profile, profile->pool)) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Cannot create 'amqp event sender' thread!\n");
        goto err;
    }

    if ( switch_core_hash_insert(mod_amqp_globals.cdr_hash, name, (void *) profile) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to insert new profile [%s] into mod_amqp profile hash\n", name);
        goto err;
    }
    switch_core_add_state_handler(&state_handlers);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Profile[%s] Successfully started\n", profile->name);
    return SWITCH_STATUS_SUCCESS;

err:
    /* Cleanup */
    mod_amqp_cdr_destroy(&profile);
    return SWITCH_STATUS_GENERR;
}

switch_status_t mod_amqp_cdr_connect(mod_amqp_cdr_profile_t *profile)
{
    int status;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Amqp no connection- reconnecting...\n");

    if ( mod_amqp_connection_open(profile->conn_root, &(profile->conn_active), profile->name, profile->custom_attr)	== SWITCH_STATUS_SUCCESS ) {
        // Ensure that the exchange exists, and is of the correct type
#if AMQP_VERSION_MAJOR == 0 && AMQP_VERSION_MINOR >= 6
        amqp_exchange_declare(profile->conn_active->state, 1,
                              amqp_cstring_bytes(profile->exchange),
                              amqp_cstring_bytes(profile->exchange_type),
                              0, /* passive */
                              profile->exchange_durable,
                              profile->exchange_auto_delete,
                              0,
                              amqp_empty_table);
#else
        amqp_exchange_declare(profile->conn_active->state, 1,
						  amqp_cstring_bytes(profile->exchange),
						  amqp_cstring_bytes(profile->exchange_type),
						  0, /* passive */
						  profile->exchange_durable,
						  amqp_empty_table);
#endif

        if ((status = mod_amqp_log_if_amqp_error(amqp_get_rpc_reply(profile->conn_active->state),
                                            "Declaring exchange")) < 0) {
            goto err;
        };


        {
            amqp_queue_declare_ok_t *recv_queue;
            //TODO move config ???
            profile->binding_key_a = switch_core_strdup(profile->pool, "leg.a");
            profile->binding_key_b = switch_core_strdup(profile->pool, "leg.b");

            //leg a
            recv_queue = amqp_queue_declare(profile->conn_active->state, // state
                                            1,                           // channel
                                            profile->queue_name_a ? amqp_cstring_bytes(profile->queue_name_a)
                                                                  : amqp_cstring_bytes("cdr-leg-a"), // queue name
                                            0, 1,                        // passive, durable
                                            0, 0,                        // exclusive, auto-delete
                                            amqp_empty_table);           // args

            if (mod_amqp_log_if_amqp_error(amqp_get_rpc_reply(profile->conn_active->state), "Declaring queue")) {
                goto err;
            }

            /* Bind the queue to the exchange */
            amqp_queue_bind(profile->conn_active->state,                   // state
                            1,                                             // channel
                            recv_queue->queue,                                     // queue
                            amqp_cstring_bytes(profile->exchange),         // exchange
                            amqp_cstring_bytes(profile->binding_key_a),      // routing key
                            amqp_empty_table);                             // args

            if (mod_amqp_log_if_amqp_error(amqp_get_rpc_reply(profile->conn_active->state), "Binding queue")) {
                goto err;
            }

            //leg b
            recv_queue = amqp_queue_declare(profile->conn_active->state, // state
                                            1,                           // channel
                                            profile->queue_name_b ? amqp_cstring_bytes(profile->queue_name_b)
                                                                  : amqp_cstring_bytes("cdr-leg-b"), // queue name
                                            0, 1,                        // passive, durable
                                            0, 0,                        // exclusive, auto-delete
                                            amqp_empty_table);           // args

            if (mod_amqp_log_if_amqp_error(amqp_get_rpc_reply(profile->conn_active->state), "Declaring queue")) {
                goto err;
            }

            /* Bind the queue to the exchange */
            amqp_queue_bind(profile->conn_active->state,                   // state
                            1,                                             // channel
                            recv_queue->queue,                             // queue
                            amqp_cstring_bytes(profile->exchange),         // exchange
                            amqp_cstring_bytes(profile->binding_key_b),      // routing key
                            amqp_empty_table);                             // args

            if (mod_amqp_log_if_amqp_error(amqp_get_rpc_reply(profile->conn_active->state), "Binding queue")) {
                goto err;
            }
        }

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Amqp reconnect successful- connected\n");
        return SWITCH_STATUS_SUCCESS;
    } else {
        return SWITCH_STATUS_FALSE;
    }

err:
    {
        const char *errstr = amqp_error_string2(-status);

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Profile[%s] failed to connect: %s\n",
                          profile->name, errstr);

        /* This is bad, we couldn't send the message. Clear up any connection */
        mod_amqp_connection_close(profile->conn_active);
        profile->conn_active = NULL;

        return SWITCH_STATUS_FALSE;
    }
}

void * SWITCH_THREAD_FUNC mod_amqp_cdr_thread(switch_thread_t *thread, void *data)
{
    mod_amqp_message_t *msg = NULL;
    switch_status_t status;
    mod_amqp_cdr_profile_t *profile = (mod_amqp_cdr_profile_t *)data;

    while (profile->running) {

        if (!profile->conn_active) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Amqp no connection - reconnecting...\n");

            status = mod_amqp_cdr_connect(profile);
            if (status == SWITCH_STATUS_SUCCESS) {
                continue;
            }

            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Profile[%s] failed to connect with code(%d), sleeping for %dms\n",
                              profile->name, status, profile->reconnect_interval_ms);
            switch_sleep(profile->reconnect_interval_ms * 1000);
            continue;
        }

        while (profile->running && profile->conn_active) {
            //TODO check connection & push if no send

           // printf("CDR run: %d act: %d q: %x\n", profile->running ? 1 : 0, profile->conn_active ? 1 : 0, switch_queue_size(profile->send_queue));

            if (!msg && switch_queue_pop_timeout(profile->send_queue, (void**)&msg, 1000000) != SWITCH_STATUS_SUCCESS) {
                continue;
            }

            switch (mod_amqp_cdr_send(profile, msg)) {
                case SWITCH_STATUS_SUCCESS:
                    /* Success: prepare for next message */
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Send [%s] success\n", msg->routing_key);
                    mod_amqp_util_msg_destroy(&msg);
                    break;

                case SWITCH_STATUS_NOT_INITALIZED:
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Send failed with 'not initialised'\n");
                    break;

                case SWITCH_STATUS_SOCKERR:
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Send failed with 'socket error'\n");
                    break;

                default:
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Send failed with a generic error\n");

                    /* Send failed and closed the connection; reconnect will happen at the beginning of the loop
                     * NB: do we need a delay here to prevent a fast reconnect-send-fail loop? */
                    break;
            }
        }

    }
    return NULL;
}

