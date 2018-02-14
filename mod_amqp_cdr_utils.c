//
// Created by igor on 2/14/18.
//

#include "switch.h"
#include "mod_amqp_cdr_utils.h"
#include <switch_ivr.h>

//TODO change times to int


#define add_jstat(_j, _i, _s)											\
	switch_snprintf(var_val, sizeof(var_val), "%" SWITCH_SIZE_T_FMT, _i); \
	cJSON_AddItemToObject(_j, _s, cJSON_CreateNumber(_i))

static void switch_ivr_set_json_call_stats(cJSON *json, switch_core_session_t *session, switch_media_type_t type)
{
    const char *name = (type == SWITCH_MEDIA_TYPE_VIDEO) ? "video" : "audio";
    cJSON *j_stat, *j_in, *j_out;
    switch_rtp_stats_t *stats = switch_core_media_get_stats(session, type, NULL);
    char var_val[35] = "";

    if (!stats) return;

    j_stat = cJSON_CreateObject();
    j_in = cJSON_CreateObject();
    j_out = cJSON_CreateObject();

    cJSON_AddItemToObject(json, name, j_stat);
    cJSON_AddItemToObject(j_stat, "inbound", j_in);
    cJSON_AddItemToObject(j_stat, "outbound", j_out);

    stats->inbound.std_deviation = sqrt(stats->inbound.variance);

    add_jstat(j_in, stats->inbound.raw_bytes, "raw_bytes");
    add_jstat(j_in, stats->inbound.media_bytes, "media_bytes");
    add_jstat(j_in, stats->inbound.packet_count, "packet_count");
    add_jstat(j_in, stats->inbound.media_packet_count, "media_packet_count");
    add_jstat(j_in, stats->inbound.skip_packet_count, "skip_packet_count");
    add_jstat(j_in, stats->inbound.jb_packet_count, "jitter_packet_count");
    add_jstat(j_in, stats->inbound.dtmf_packet_count, "dtmf_packet_count");
    add_jstat(j_in, stats->inbound.cng_packet_count, "cng_packet_count");
    add_jstat(j_in, stats->inbound.flush_packet_count, "flush_packet_count");
    add_jstat(j_in, stats->inbound.largest_jb_size, "largest_jb_size");
    add_jstat(j_in, stats->inbound.min_variance, "jitter_min_variance");
    add_jstat(j_in, stats->inbound.max_variance, "jitter_max_variance");
    add_jstat(j_in, stats->inbound.lossrate, "jitter_loss_rate");
    add_jstat(j_in, stats->inbound.burstrate, "jitter_burst_rate");
    add_jstat(j_in, stats->inbound.mean_interval, "mean_interval");
    add_jstat(j_in, stats->inbound.flaws, "flaw_total");
    add_jstat(j_in, stats->inbound.R, "quality_percentage");
    add_jstat(j_in, stats->inbound.mos, "mos");


    if (stats->inbound.error_log) {
        cJSON *j_err_log, *j_err;
        switch_error_period_t *ep;

        j_err_log = cJSON_CreateArray();
        cJSON_AddItemToObject(j_in, "errorLog", j_err_log);

        for(ep = stats->inbound.error_log; ep; ep = ep->next) {

            if (!(ep->start && ep->stop)) continue;

            j_err = cJSON_CreateObject();

            cJSON_AddItemToObject(j_err, "start", cJSON_CreateNumber(ep->start));
            cJSON_AddItemToObject(j_err, "stop", cJSON_CreateNumber(ep->stop));
            cJSON_AddItemToObject(j_err, "flaws", cJSON_CreateNumber(ep->flaws));
            cJSON_AddItemToObject(j_err, "consecutiveFlaws", cJSON_CreateNumber(ep->consecutive_flaws));
            cJSON_AddItemToObject(j_err, "durationMS", cJSON_CreateNumber((ep->stop - ep->start) / 1000));
            cJSON_AddItemToArray(j_err_log, j_err);
        }
    }

    add_jstat(j_out, stats->outbound.raw_bytes, "raw_bytes");
    add_jstat(j_out, stats->outbound.media_bytes, "media_bytes");
    add_jstat(j_out, stats->outbound.packet_count, "packet_count");
    add_jstat(j_out, stats->outbound.media_packet_count, "media_packet_count");
    add_jstat(j_out, stats->outbound.skip_packet_count, "skip_packet_count");
    add_jstat(j_out, stats->outbound.dtmf_packet_count, "dtmf_packet_count");
    add_jstat(j_out, stats->outbound.cng_packet_count, "cng_packet_count");
    add_jstat(j_out, stats->rtcp.packet_count, "rtcp_packet_count");
    add_jstat(j_out, stats->rtcp.octet_count, "rtcp_octet_count");
}

static void switch_ivr_set_json_profile_data(cJSON *json, switch_caller_profile_t *caller_profile)
{
    cJSON_AddItemToObject(json, "username", cJSON_CreateString((char *)caller_profile->username));
    cJSON_AddItemToObject(json, "dialplan", cJSON_CreateString((char *)caller_profile->dialplan));
    cJSON_AddItemToObject(json, "caller_id_name", cJSON_CreateString((char *)caller_profile->caller_id_name));
    cJSON_AddItemToObject(json, "ani", cJSON_CreateString((char *)caller_profile->ani));
    cJSON_AddItemToObject(json, "aniii", cJSON_CreateString((char *)caller_profile->aniii));
    cJSON_AddItemToObject(json, "caller_id_number", cJSON_CreateString((char *)caller_profile->caller_id_number));
    cJSON_AddItemToObject(json, "network_addr", cJSON_CreateString((char *)caller_profile->network_addr));
    cJSON_AddItemToObject(json, "rdnis", cJSON_CreateString((char *)caller_profile->rdnis));
    cJSON_AddItemToObject(json, "destination_number", cJSON_CreateString(caller_profile->destination_number));
    cJSON_AddItemToObject(json, "uuid", cJSON_CreateString(caller_profile->uuid));
    cJSON_AddItemToObject(json, "source", cJSON_CreateString((char *)caller_profile->source));
    cJSON_AddItemToObject(json, "context", cJSON_CreateString((char *)caller_profile->context));
    cJSON_AddItemToObject(json, "chan_name", cJSON_CreateString(caller_profile->chan_name));
}

static void switch_ivr_set_json_chan_vars(cJSON *json, switch_channel_t *channel, switch_bool_t urlencode)
{
    switch_event_header_t *hi = switch_channel_variable_first(channel);

    if (!hi)
        return;

    for (; hi; hi = hi->next) {
        if (!zstr(hi->name) && !zstr(hi->value)) {
            char *data = hi->value;
            if (urlencode) {
                switch_size_t dlen = strlen(hi->value) * 3;

                if ((data = malloc(dlen))) {
                    memset(data, 0, dlen);
                    switch_url_encode(hi->value, data, dlen);
                }
            }

            cJSON_AddItemToObject(json, hi->name, cJSON_CreateString(data));

            if (data != hi->value) {
                switch_safe_free(data);
            }
        }
    }
    switch_channel_variable_last(channel);
}

switch_status_t generate_json_cdr(switch_core_session_t *session, cJSON **json_cdr, switch_bool_t urlencode)
{
    cJSON *cdr = cJSON_CreateObject();
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_caller_profile_t *caller_profile;
    cJSON *variables, *j_main_cp, *j_caller_profile, *j_caller_extension, *j_caller_extension_apps, *j_times, *j_application,
            *j_callflow, *j_profile, *j_inner_extension, *j_app_log, *j_apps, *j_o, *j_o_profiles, *j_channel_data, *callStats;
    switch_app_log_t *app_log;
    char tmp[512], *f;

    cJSON_AddItemToObject(cdr, "core-uuid", cJSON_CreateString(switch_core_get_uuid()));
    cJSON_AddItemToObject(cdr, "switchname", cJSON_CreateString(switch_core_get_switchname()));
    j_channel_data = cJSON_CreateObject();

    cJSON_AddItemToObject(cdr, "channel_data", j_channel_data);

    cJSON_AddItemToObject(j_channel_data, "state", cJSON_CreateString((char *) switch_channel_state_name(switch_channel_get_state(channel))));
    cJSON_AddItemToObject(j_channel_data, "direction", cJSON_CreateString(switch_channel_direction(channel) == SWITCH_CALL_DIRECTION_OUTBOUND ? "outbound" : "inbound"));

    switch_snprintf(tmp, sizeof(tmp), "%d", switch_channel_get_state(channel));
    cJSON_AddItemToObject(j_channel_data, "state_number", cJSON_CreateString((char *) tmp));

    if ((f = switch_channel_get_flag_string(channel))) {
        cJSON_AddItemToObject(j_channel_data, "flags", cJSON_CreateString((char *) f));
        free(f);
    }

    if ((f = switch_channel_get_cap_string(channel))) {
        cJSON_AddItemToObject(j_channel_data, "caps", cJSON_CreateString((char *) f));
        free(f);
    }

    callStats = cJSON_CreateObject();
    cJSON_AddItemToObject(cdr, "callStats", callStats);
    switch_ivr_set_json_call_stats(callStats, session, SWITCH_MEDIA_TYPE_AUDIO);
    switch_ivr_set_json_call_stats(callStats, session, SWITCH_MEDIA_TYPE_VIDEO);

    variables = cJSON_CreateObject();
    cJSON_AddItemToObject(cdr, "variables", variables);
    switch_ivr_set_json_chan_vars(variables, channel, urlencode);


    if ((app_log = switch_core_session_get_app_log(session))) {
        switch_app_log_t *ap;

        j_app_log = cJSON_CreateObject();
        j_apps = cJSON_CreateArray();

        cJSON_AddItemToObject(cdr, "app_log", j_app_log);
        cJSON_AddItemToObject(j_app_log, "applications", j_apps);

        for (ap = app_log; ap; ap = ap->next) {
            j_application = cJSON_CreateObject();

            cJSON_AddItemToObject(j_application, "app_name", cJSON_CreateString(ap->app));
            cJSON_AddItemToObject(j_application, "app_data", cJSON_CreateString(ap->arg));
            cJSON_AddItemToObject(j_application, "app_stamp", cJSON_CreateNumber(ap->stamp));

            cJSON_AddItemToArray(j_apps, j_application);
        }
    }


    caller_profile = switch_channel_get_caller_profile(channel);

    j_callflow = cJSON_CreateArray();
    cJSON_AddItemToObject(cdr, "callflow", j_callflow);

    while (caller_profile) {

        j_profile = cJSON_CreateObject();

        if (!zstr(caller_profile->dialplan)) {
            cJSON_AddItemToObject(j_profile, "dialplan", cJSON_CreateString((char *)caller_profile->dialplan));
        }

        if (!zstr(caller_profile->profile_index)) {
            cJSON_AddItemToObject(j_profile, "profile_index", cJSON_CreateString((char *)caller_profile->profile_index));
        }

        if (caller_profile->caller_extension) {
            switch_caller_application_t *ap;

            j_caller_extension = cJSON_CreateObject();
            j_caller_extension_apps = cJSON_CreateArray();

            cJSON_AddItemToObject(j_profile, "extension", j_caller_extension);

            cJSON_AddItemToObject(j_caller_extension, "name", cJSON_CreateString(caller_profile->caller_extension->extension_name));
            cJSON_AddItemToObject(j_caller_extension, "number", cJSON_CreateString(caller_profile->caller_extension->extension_number));
            cJSON_AddItemToObject(j_caller_extension, "applications", j_caller_extension_apps);

            if (caller_profile->caller_extension->current_application) {
                cJSON_AddItemToObject(j_caller_extension, "current_app", cJSON_CreateString(caller_profile->caller_extension->current_application->application_name));
            }

            for (ap = caller_profile->caller_extension->applications; ap; ap = ap->next) {
                j_application = cJSON_CreateObject();

                cJSON_AddItemToArray(j_caller_extension_apps, j_application);

                if (ap == caller_profile->caller_extension->current_application) {
                    cJSON_AddItemToObject(j_application, "last_executed", cJSON_CreateString("true"));
                }
                cJSON_AddItemToObject(j_application, "app_name", cJSON_CreateString(ap->application_name));
                cJSON_AddItemToObject(j_application, "app_data", cJSON_CreateString(switch_str_nil(ap->application_data)));
            }

            if (caller_profile->caller_extension->children) {
                switch_caller_profile_t *cp = NULL;
                j_inner_extension = cJSON_CreateArray();
                cJSON_AddItemToObject(j_caller_extension, "sub_extensions", j_inner_extension);
                for (cp = caller_profile->caller_extension->children; cp; cp = cp->next) {

                    if (!cp->caller_extension) {
                        continue;
                    }

                    j_caller_extension = cJSON_CreateObject();
                    cJSON_AddItemToArray(j_inner_extension, j_caller_extension);

                    cJSON_AddItemToObject(j_caller_extension, "name", cJSON_CreateString(cp->caller_extension->extension_name));
                    cJSON_AddItemToObject(j_caller_extension, "number", cJSON_CreateString(cp->caller_extension->extension_number));

                    cJSON_AddItemToObject(j_caller_extension, "dialplan", cJSON_CreateString((char *)cp->dialplan));

                    if (cp->caller_extension->current_application) {
                        cJSON_AddItemToObject(j_caller_extension, "current_app", cJSON_CreateString(cp->caller_extension->current_application->application_name));
                    }

                    j_caller_extension_apps = cJSON_CreateArray();
                    cJSON_AddItemToObject(j_caller_extension, "applications", j_caller_extension_apps);
                    for (ap = cp->caller_extension->applications; ap; ap = ap->next) {
                        j_application = cJSON_CreateObject();
                        cJSON_AddItemToArray(j_caller_extension_apps, j_application);

                        if (ap == cp->caller_extension->current_application) {
                            cJSON_AddItemToObject(j_application, "last_executed", cJSON_CreateString("true"));
                        }
                        cJSON_AddItemToObject(j_application, "app_name", cJSON_CreateString(ap->application_name));
                        cJSON_AddItemToObject(j_application, "app_data", cJSON_CreateString(switch_str_nil(ap->application_data)));
                    }
                }
            }
        }

        j_main_cp = cJSON_CreateObject();
        cJSON_AddItemToObject(j_profile, "caller_profile", j_main_cp);

        switch_ivr_set_json_profile_data(j_main_cp, caller_profile);

        if (caller_profile->originator_caller_profile) {
            switch_caller_profile_t *cp = NULL;

            j_o = cJSON_CreateObject();
            cJSON_AddItemToObject(j_main_cp, "originator", j_o);

            j_o_profiles = cJSON_CreateArray();
            cJSON_AddItemToObject(j_o, "originator_caller_profiles", j_o_profiles);

            for (cp = caller_profile->originator_caller_profile; cp; cp = cp->next) {
                j_caller_profile = cJSON_CreateObject();
                cJSON_AddItemToArray(j_o_profiles, j_caller_profile);

                switch_ivr_set_json_profile_data(j_caller_profile, cp);
            }
        }

        if (caller_profile->originatee_caller_profile) {
            switch_caller_profile_t *cp = NULL;

            j_o = cJSON_CreateObject();
            cJSON_AddItemToObject(j_main_cp, "originatee", j_o);

            j_o_profiles = cJSON_CreateArray();
            cJSON_AddItemToObject(j_o, "originatee_caller_profiles", j_o_profiles);

            for (cp = caller_profile->originatee_caller_profile; cp; cp = cp->next) {
                j_caller_profile = cJSON_CreateObject();
                cJSON_AddItemToArray(j_o_profiles, j_caller_profile);

                switch_ivr_set_json_profile_data(j_caller_profile, cp);
            }
        }

        if (caller_profile->times) {

            j_times = cJSON_CreateObject();
            cJSON_AddItemToObject(j_profile, "times", j_times);
            cJSON_AddItemToObject(j_times, "created_time", cJSON_CreateNumber(caller_profile->times->created));
            cJSON_AddItemToObject(j_times, "profile_created_time", cJSON_CreateNumber(caller_profile->times->profile_created));
            cJSON_AddItemToObject(j_times, "progress_time", cJSON_CreateNumber(caller_profile->times->progress));
            cJSON_AddItemToObject(j_times, "progress_media_time", cJSON_CreateNumber(caller_profile->times->progress_media));
            cJSON_AddItemToObject(j_times, "answered_time", cJSON_CreateNumber(caller_profile->times->answered));
            cJSON_AddItemToObject(j_times, "bridged_time", cJSON_CreateNumber(caller_profile->times->bridged));
            cJSON_AddItemToObject(j_times, "last_hold_time", cJSON_CreateNumber(caller_profile->times->last_hold));
            cJSON_AddItemToObject(j_times, "hold_accum_time", cJSON_CreateNumber(caller_profile->times->hold_accum));
            cJSON_AddItemToObject(j_times, "hangup_time", cJSON_CreateNumber(caller_profile->times->hungup));
            cJSON_AddItemToObject(j_times, "resurrect_time", cJSON_CreateNumber(caller_profile->times->resurrected));
            cJSON_AddItemToObject(j_times, "transfer_time", cJSON_CreateNumber(caller_profile->times->transferred));

        }
        cJSON_AddItemToArray(j_callflow, j_profile);
        caller_profile = caller_profile->next;
    }

    *json_cdr = cdr;

    return SWITCH_STATUS_SUCCESS;

}
