// SPDX-License-Identifier: GPL-3.0-or-later

#include "stream.h"
#include "stream-thread.h"
#include "stream-receiver-internals.h"
#include "web/server/h2o/http_server.h"
#include "replication.h"

// --------------------------------------------------------------------------------------------------------------------

void stream_receiver_log_status(struct receiver_state *rpt, const char *msg, const char *status, ND_LOG_FIELD_PRIORITY priority) {
    // this function may be called BEFORE we spawn the receiver thread
    // so, we need to add the fields again (it does not harm)
    ND_LOG_STACK lgs[] = {
        ND_LOG_FIELD_TXT(NDF_SRC_IP, rpt->client_ip),
        ND_LOG_FIELD_TXT(NDF_SRC_PORT, rpt->client_port),
        ND_LOG_FIELD_TXT(NDF_NIDL_NODE, (rpt->hostname && *rpt->hostname) ? rpt->hostname : ""),
        ND_LOG_FIELD_TXT(NDF_RESPONSE_CODE, status),
        ND_LOG_FIELD_UUID(NDF_MESSAGE_ID, &streaming_from_child_msgid),
        ND_LOG_FIELD_END(),
    };
    ND_LOG_STACK_PUSH(lgs);

    nd_log(NDLS_ACCESS, priority, "api_key:'%s' machine_guid:'%s' msg:'%s'"
           , (rpt->key && *rpt->key)? rpt->key : ""
           , (rpt->machine_guid && *rpt->machine_guid) ? rpt->machine_guid : ""
           , msg);

    nd_log(NDLS_DAEMON, priority, "STREAM RECEIVE '%s' [from [%s]:%s]: %s %s%s%s"
           , (rpt->hostname && *rpt->hostname) ? rpt->hostname : ""
           , rpt->client_ip, rpt->client_port
           , msg
           , rpt->exit.reason != STREAM_HANDSHAKE_NEVER?" (":""
           , stream_handshake_error_to_string(rpt->exit.reason)
           , rpt->exit.reason != STREAM_HANDSHAKE_NEVER?")":""
    );
}

// --------------------------------------------------------------------------------------------------------------------

void stream_receiver_free(struct receiver_state *rpt) {
    nd_sock_close(&rpt->sock);
    stream_decompressor_destroy(&rpt->thread.compressed.decompressor);

    if(rpt->system_info)
        rrdhost_system_info_free(rpt->system_info);

    __atomic_sub_fetch(&netdata_buffers_statistics.rrdhost_receivers, sizeof(*rpt), __ATOMIC_RELAXED);

    freez(rpt->key);
    freez(rpt->hostname);
    freez(rpt->registry_hostname);
    freez(rpt->machine_guid);
    freez(rpt->os);
    freez(rpt->timezone);
    freez(rpt->abbrev_timezone);
    freez(rpt->client_ip);
    freez(rpt->client_port);
    freez(rpt->program_name);
    freez(rpt->program_version);

    freez(rpt->thread.compressed.buf);
    rpt->thread.compressed.buf = NULL;
    rpt->thread.compressed.size = 0;

    string_freez(rpt->config.send.api_key);
    string_freez(rpt->config.send.parents);
    string_freez(rpt->config.send.charts_matching);

    freez(rpt);
}

// --------------------------------------------------------------------------------------------------------------------

static int stream_receiver_response_permission_denied(struct web_client *w) {
    // we always respond with the same message and error code
    // to prevent an attacker from gaining info about the error
    buffer_flush(w->response.data);
    buffer_strcat(w->response.data, START_STREAMING_ERROR_NOT_PERMITTED);
    return HTTP_RESP_UNAUTHORIZED;
}

static int stream_receiver_response_too_busy_now(struct web_client *w) {
    // we always respond with the same message and error code
    // to prevent an attacker from gaining info about the error
    buffer_flush(w->response.data);
    buffer_strcat(w->response.data, START_STREAMING_ERROR_BUSY_TRY_LATER);
    return HTTP_RESP_SERVICE_UNAVAILABLE;
}

static void stream_receiver_takeover_web_connection(struct web_client *w, struct receiver_state *rpt) {
    rpt->sock.fd = w->ifd;
    rpt->sock.ssl = w->ssl;

    w->ssl = NETDATA_SSL_UNSET_CONNECTION;

    WEB_CLIENT_IS_DEAD(w);

    if(web_server_mode == WEB_SERVER_MODE_STATIC_THREADED) {
        web_client_flag_set(w, WEB_CLIENT_FLAG_DONT_CLOSE_SOCKET);
    }
    else {
        if(w->ifd == w->ofd)
            w->ifd = w->ofd = -1;
        else
            w->ifd = -1;
    }

    buffer_flush(w->response.data);
}

static void stream_send_error_on_taken_over_connection(struct receiver_state *rpt, const char *msg) {
    nd_sock_send_timeout(&rpt->sock, (char *)msg, strlen(msg), 0, 5);
}

static bool stream_receiver_send_first_response(struct receiver_state *rpt) {
    // find the host for this receiver
    {
        // this will also update the host with our system_info
        RRDHOST *host = rrdhost_find_or_create(
            rpt->hostname,
            rpt->registry_hostname,
            rpt->machine_guid,
            rpt->os,
            rpt->timezone,
            rpt->abbrev_timezone,
            rpt->utc_offset,
            rpt->program_name,
            rpt->program_version,
            rpt->config.update_every,
            rpt->config.history,
            rpt->config.mode,
            rpt->config.health.enabled != CONFIG_BOOLEAN_NO,
            rpt->config.send.enabled && rpt->config.send.parents && rpt->config.send.api_key,
            rpt->config.send.parents,
            rpt->config.send.api_key,
            rpt->config.send.charts_matching,
            rpt->config.replication.enabled,
            rpt->config.replication.period,
            rpt->config.replication.step,
            rpt->system_info,
            0);

        if(!host) {
            stream_receiver_log_status(
                rpt,
                "rejecting streaming connection; failed to find or create the required host structure",
                STREAM_STATUS_INTERNAL_SERVER_ERROR, NDLP_ERR);

            stream_send_error_on_taken_over_connection(rpt, START_STREAMING_ERROR_INTERNAL_ERROR);
            return false;
        }
        // IMPORTANT: KEEP THIS FIRST AFTER CHECKING host RESPONSE!
        // THIS IS HOW WE KNOW THE system_info IS GONE NOW...
        // system_info has been consumed by the host structure
        rpt->system_info = NULL;

        if (unlikely(rrdhost_flag_check(host, RRDHOST_FLAG_PENDING_CONTEXT_LOAD))) {
            stream_receiver_log_status(
                rpt,
                "rejecting streaming connection; host is initializing, retry later",
                STREAM_STATUS_INITIALIZATION_IN_PROGRESS, NDLP_NOTICE);

            stream_send_error_on_taken_over_connection(rpt, START_STREAMING_ERROR_INITIALIZATION);
            return false;
        }

        if (unlikely(!stream_control_children_should_be_accepted())) {
            stream_receiver_log_status(
                rpt,
                "rejecting streaming connection; the system is backfilling higher tiers with high-resolution data, retry later",
                STREAM_STATUS_INITIALIZATION_IN_PROGRESS, NDLP_NOTICE);

            stream_send_error_on_taken_over_connection(rpt, START_STREAMING_ERROR_INITIALIZATION);
            return false;
        }

        if(!rrdhost_set_receiver(host, rpt)) {
            stream_receiver_log_status(
                rpt,
                "rejecting streaming connection; host is already served by another receiver",
                STREAM_STATUS_DUPLICATE_RECEIVER, NDLP_INFO);

            stream_send_error_on_taken_over_connection(rpt, START_STREAMING_ERROR_ALREADY_STREAMING);
            return false;
        }
    }

#ifdef NETDATA_INTERNAL_CHECKS
    netdata_log_info("STREAM RECEIVE '%s' [from [%s]:%s]: "
                     "client willing to stream metrics for host '%s' with machine_guid '%s': "
                     "update every = %d, history = %d, memory mode = %s, health %s,%s"
                     , rpt->hostname
                     , rpt->client_ip
                     , rpt->client_port
                     , rrdhost_hostname(rpt->host)
                         , rpt->host->machine_guid
                     , rpt->host->rrd_update_every
                     , rpt->host->rrd_history_entries
                     , rrd_memory_mode_name(rpt->host->rrd_memory_mode)
                         , (rpt->config.health.enabled == CONFIG_BOOLEAN_NO)?"disabled":((rpt->config.health.enabled == CONFIG_BOOLEAN_YES)?"enabled":"auto")
                         , (rpt->sock.ssl.conn != NULL) ? " SSL," : ""
    );
#endif // NETDATA_INTERNAL_CHECKS

    stream_select_receiver_compression_algorithm(rpt);

    {
        // netdata_log_info("STREAM %s [receive from [%s]:%s]: initializing communication...", rrdhost_hostname(rpt->host), rpt->client_ip, rpt->client_port);
        char initial_response[HTTP_HEADER_SIZE];
        if (stream_has_capability(rpt, STREAM_CAP_VCAPS)) {
            log_receiver_capabilities(rpt);
            sprintf(initial_response, "%s%u", START_STREAMING_PROMPT_VN, rpt->capabilities);
        }
        else if (stream_has_capability(rpt, STREAM_CAP_VN)) {
            log_receiver_capabilities(rpt);
            sprintf(initial_response, "%s%d", START_STREAMING_PROMPT_VN, stream_capabilities_to_vn(rpt->capabilities));
        }
        else if (stream_has_capability(rpt, STREAM_CAP_V2)) {
            log_receiver_capabilities(rpt);
            sprintf(initial_response, "%s", START_STREAMING_PROMPT_V2);
        }
        else { // stream_has_capability(rpt, STREAM_CAP_V1)
            log_receiver_capabilities(rpt);
            sprintf(initial_response, "%s", START_STREAMING_PROMPT_V1);
        }

        // OUR FIRST RESPONSE IS READY!

        // web server sockets are non-blocking - set them to blocking mode
#ifdef ENABLE_H2O
        unless_h2o_rrdpush(rpt)
#endif
        {
            // remove the non-blocking flag from the socket
            if(sock_delnonblock(rpt->sock.fd) < 0)
                nd_log(NDLS_DAEMON, NDLP_ERR,
                       "STREAM RECEIVE '%s' [from [%s]:%s]: cannot remove the non-blocking flag from socket %d",
                       rrdhost_hostname(rpt->host), rpt->client_ip, rpt->client_port, rpt->sock.fd);

            struct timeval timeout;
            timeout.tv_sec = 600;
            timeout.tv_usec = 0;
            if (unlikely(setsockopt(rpt->sock.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) != 0))
                nd_log(NDLS_DAEMON, NDLP_ERR,
                       "STREAM RECEIVE '%s' [from [%s]:%s]: cannot set timeout for socket %d",
                       rrdhost_hostname(rpt->host), rpt->client_ip, rpt->client_port, rpt->sock.fd);
        }

        netdata_log_debug(D_STREAM, "Initial response to %s: %s", rpt->client_ip, initial_response);
#ifdef ENABLE_H2O
        if (is_h2o_rrdpush(rpt)) {
            h2o_stream_write(rpt->h2o_ctx, initial_response, strlen(initial_response));
        } else {
#endif
            ssize_t bytes_sent = nd_sock_send_timeout(&rpt->sock, initial_response, strlen(initial_response), 0, 60);

            if(bytes_sent != (ssize_t)strlen(initial_response)) {
                internal_error(true, "Cannot send response, got %zd bytes, expecting %zu bytes", bytes_sent, strlen(initial_response));
                stream_receiver_log_status(
                    rpt,
                    "cannot reply back, dropping connection",
                    STREAM_STATUS_CANT_REPLY, NDLP_ERR);
                rrdhost_clear_receiver(rpt);
                return false;
            }
#ifdef ENABLE_H2O
        }
#endif
    }

    return true;
}

int stream_receiver_accept_connection(struct web_client *w, char *decoded_query_string, void *h2o_ctx __maybe_unused) {

    if(!service_running(ABILITY_STREAMING_CONNECTIONS))
        return stream_receiver_response_too_busy_now(w);

    struct receiver_state *rpt = callocz(1, sizeof(*rpt));
    rpt->thread.compressed.size = COMPRESSION_MAX_CHUNK;
    rpt->thread.compressed.buf = mallocz(rpt->thread.compressed.size);
    rpt->connected_since_s = now_realtime_sec();
    rpt->last_msg_t = now_monotonic_sec();
    rpt->hops = 1;

    rpt->capabilities = STREAM_CAP_INVALID;

#ifdef ENABLE_H2O
    rpt->h2o_ctx = h2o_ctx;
#endif

    __atomic_add_fetch(&netdata_buffers_statistics.rrdhost_receivers, sizeof(*rpt), __ATOMIC_RELAXED);
    __atomic_add_fetch(&netdata_buffers_statistics.rrdhost_allocations_size, sizeof(struct rrdhost_system_info), __ATOMIC_RELAXED);

    rpt->system_info = callocz(1, sizeof(struct rrdhost_system_info));
    rpt->system_info->hops = rpt->hops;

    nd_sock_init(&rpt->sock, netdata_ssl_web_server_ctx, false);
    rpt->client_ip         = strdupz(w->client_ip);
    rpt->client_port       = strdupz(w->client_port);

    rpt->config.update_every = default_rrd_update_every;

    // parse the parameters and fill rpt and rpt->system_info

    while(decoded_query_string) {
        char *value = strsep_skip_consecutive_separators(&decoded_query_string, "&");
        if(!value || !*value) continue;

        char *name = strsep_skip_consecutive_separators(&value, "=");
        if(!name || !*name) continue;
        if(!value || !*value) continue;

        if(!strcmp(name, "key") && !rpt->key)
            rpt->key = strdupz(value);

        else if(!strcmp(name, "hostname") && !rpt->hostname)
            rpt->hostname = strdupz(value);

        else if(!strcmp(name, "registry_hostname") && !rpt->registry_hostname)
            rpt->registry_hostname = strdupz(value);

        else if(!strcmp(name, "machine_guid") && !rpt->machine_guid)
            rpt->machine_guid = strdupz(value);

        else if(!strcmp(name, "update_every"))
            rpt->config.update_every = (int)strtoul(value, NULL, 0);

        else if(!strcmp(name, "os") && !rpt->os)
            rpt->os = strdupz(value);

        else if(!strcmp(name, "timezone") && !rpt->timezone)
            rpt->timezone = strdupz(value);

        else if(!strcmp(name, "abbrev_timezone") && !rpt->abbrev_timezone)
            rpt->abbrev_timezone = strdupz(value);

        else if(!strcmp(name, "utc_offset"))
            rpt->utc_offset = (int32_t)strtol(value, NULL, 0);

        else if(!strcmp(name, "hops"))
            rpt->hops = rpt->system_info->hops = (int16_t)strtol(value, NULL, 0);

        else if(!strcmp(name, "ml_capable"))
            rpt->system_info->ml_capable = strtoul(value, NULL, 0);

        else if(!strcmp(name, "ml_enabled"))
            rpt->system_info->ml_enabled = strtoul(value, NULL, 0);

        else if(!strcmp(name, "mc_version"))
            rpt->system_info->mc_version = strtoul(value, NULL, 0);

        else if(!strcmp(name, "ver") && (rpt->capabilities & STREAM_CAP_INVALID))
            rpt->capabilities = convert_stream_version_to_capabilities(strtoul(value, NULL, 0), NULL, false);

        else {
            // An old Netdata child does not have a compatible streaming protocol, map to something sane.
            if (!strcmp(name, "NETDATA_SYSTEM_OS_NAME"))
                name = "NETDATA_HOST_OS_NAME";

            else if (!strcmp(name, "NETDATA_SYSTEM_OS_ID"))
                name = "NETDATA_HOST_OS_ID";

            else if (!strcmp(name, "NETDATA_SYSTEM_OS_ID_LIKE"))
                name = "NETDATA_HOST_OS_ID_LIKE";

            else if (!strcmp(name, "NETDATA_SYSTEM_OS_VERSION"))
                name = "NETDATA_HOST_OS_VERSION";

            else if (!strcmp(name, "NETDATA_SYSTEM_OS_VERSION_ID"))
                name = "NETDATA_HOST_OS_VERSION_ID";

            else if (!strcmp(name, "NETDATA_SYSTEM_OS_DETECTION"))
                name = "NETDATA_HOST_OS_DETECTION";

            else if(!strcmp(name, "NETDATA_PROTOCOL_VERSION") && (rpt->capabilities & STREAM_CAP_INVALID))
                rpt->capabilities = convert_stream_version_to_capabilities(1, NULL, false);

            if (unlikely(rrdhost_set_system_info_variable(rpt->system_info, name, value))) {
                nd_log_daemon(NDLP_NOTICE, "STREAM RECEIVE '%s' [from [%s]:%s]: "
                                           "request has parameter '%s' = '%s', which is not used."
                              , (rpt->hostname && *rpt->hostname) ? rpt->hostname : "-"
                              , rpt->client_ip, rpt->client_port
                              , name, value);
            }
        }
    }

    if (rpt->capabilities & STREAM_CAP_INVALID)
        // no version is supplied, assume version 0;
        rpt->capabilities = convert_stream_version_to_capabilities(0, NULL, false);

    // find the program name and version
    if(w->user_agent && w->user_agent[0]) {
        char *t = strchr(w->user_agent, '/');
        if(t && *t) {
            *t = '\0';
            t++;
        }

        rpt->program_name = strdupz(w->user_agent);
        if(t && *t) rpt->program_version = strdupz(t);
    }

    // check if we should accept this connection

    if(!rpt->key || !*rpt->key) {
        stream_receiver_log_status(
            rpt,
            "rejecting streaming connection; request without an API key",
            STREAM_STATUS_NO_API_KEY, NDLP_WARNING);

        stream_receiver_free(rpt);
        return stream_receiver_response_permission_denied(w);
    }

    if(!rpt->hostname || !*rpt->hostname) {
        stream_receiver_log_status(
            rpt,
            "rejecting streaming connection; request without a hostname",
            STREAM_STATUS_NO_HOSTNAME, NDLP_WARNING);

        stream_receiver_free(rpt);
        return stream_receiver_response_permission_denied(w);
    }

    if(!rpt->registry_hostname)
        rpt->registry_hostname = strdupz(rpt->hostname);

    if(!rpt->machine_guid || !*rpt->machine_guid) {
        stream_receiver_log_status(
            rpt,
            "rejecting streaming connection; request without a machine UUID",
            STREAM_STATUS_NO_MACHINE_GUID, NDLP_WARNING);

        stream_receiver_free(rpt);
        return stream_receiver_response_permission_denied(w);
    }

    {
        char buf[GUID_LEN + 1];

        if (regenerate_guid(rpt->key, buf) == -1) {
            stream_receiver_log_status(
                rpt,
                "rejecting streaming connection; API key is not a valid UUID (use the command uuidgen to generate one)",
                STREAM_STATUS_INVALID_API_KEY, NDLP_WARNING);

            stream_receiver_free(rpt);
            return stream_receiver_response_permission_denied(w);
        }

        if (regenerate_guid(rpt->machine_guid, buf) == -1) {
            stream_receiver_log_status(
                rpt,
                "rejecting streaming connection; machine UUID is not a valid UUID",
                STREAM_STATUS_INVALID_MACHINE_GUID, NDLP_WARNING);

            stream_receiver_free(rpt);
            return stream_receiver_response_permission_denied(w);
        }
    }

    if(!stream_conf_is_key_type(rpt->key, "api")) {
        stream_receiver_log_status(
            rpt,
            "rejecting streaming connection; API key provided is a machine UUID (did you mix them up?)",
            STREAM_STATUS_INVALID_API_KEY, NDLP_WARNING);

        stream_receiver_free(rpt);
        return stream_receiver_response_permission_denied(w);
    }

    // the default for api keys is false, so that users
    // have to enable them manually
    if(!stream_conf_api_key_is_enabled(rpt->key, false)) {
        stream_receiver_log_status(
            rpt,
            "rejecting streaming connection; API key is not enabled in stream.conf",
            STREAM_STATUS_API_KEY_DISABLED, NDLP_WARNING);

        stream_receiver_free(rpt);
        return stream_receiver_response_permission_denied(w);
    }

    if(!stream_conf_api_key_allows_client(rpt->key, w->client_ip)) {
        stream_receiver_log_status(
            rpt,
            "rejecting streaming connection; API key is not allowed from this IP",
            STREAM_STATUS_NOT_ALLOWED_IP, NDLP_WARNING);

        stream_receiver_free(rpt);
        return stream_receiver_response_permission_denied(w);
    }

    if (!stream_conf_is_key_type(rpt->machine_guid, "machine")) {
        stream_receiver_log_status(
            rpt,
            "rejecting streaming connection; machine UUID is an API key (did you mix them up?)",
            STREAM_STATUS_INVALID_MACHINE_GUID, NDLP_WARNING);

        stream_receiver_free(rpt);
        return stream_receiver_response_permission_denied(w);
    }

    // the default for machine guids is true, so that users do not
    // have to enable them manually
    if(!stream_conf_api_key_is_enabled(rpt->machine_guid, true)) {
        stream_receiver_log_status(
            rpt,
            "rejecting streaming connection; machine UUID is not enabled in stream.conf",
            STREAM_STATUS_MACHINE_GUID_DISABLED, NDLP_WARNING);

        stream_receiver_free(rpt);
        return stream_receiver_response_permission_denied(w);
    }

    if(!stream_conf_api_key_allows_client(rpt->machine_guid, w->client_ip)) {
        stream_receiver_log_status(
            rpt,
            "rejecting streaming connection; machine UUID is not allowed from this IP",
            STREAM_STATUS_NOT_ALLOWED_IP, NDLP_WARNING);

        stream_receiver_free(rpt);
        return stream_receiver_response_permission_denied(w);
    }

    if (strcmp(rpt->machine_guid, localhost->machine_guid) == 0) {
        stream_receiver_takeover_web_connection(w, rpt);

        stream_receiver_log_status(
            rpt,
            "rejecting streaming connection; machine UUID is my own",
            STREAM_STATUS_LOCALHOST, NDLP_DEBUG);

        char initial_response[HTTP_HEADER_SIZE + 1];
        snprintfz(initial_response, HTTP_HEADER_SIZE, "%s", START_STREAMING_ERROR_SAME_LOCALHOST);

        if(nd_sock_send_timeout(&rpt->sock, initial_response, strlen(initial_response), 0, 60) !=
            (ssize_t)strlen(initial_response)) {

            nd_log_daemon(NDLP_ERR, "STREAM RECEIVE '%s' [from [%s]:%s]: failed to reply.",
                          rpt->hostname, rpt->client_ip, rpt->client_port
            );
        }

        stream_receiver_free(rpt);
        return HTTP_RESP_OK;
    }

    if(unlikely(web_client_streaming_rate_t > 0)) {
        static SPINLOCK spinlock = SPINLOCK_INITIALIZER;
        static time_t last_stream_accepted_t = 0;

        time_t now = now_realtime_sec();
        spinlock_lock(&spinlock);

        if(unlikely(last_stream_accepted_t == 0))
            last_stream_accepted_t = now;

        if(now - last_stream_accepted_t < web_client_streaming_rate_t) {
            spinlock_unlock(&spinlock);

            char msg[100 + 1];
            snprintfz(msg, sizeof(msg) - 1,
                      "rejecting streaming connection; rate limit, will accept new connection in %ld secs",
                      (long)(web_client_streaming_rate_t - (now - last_stream_accepted_t)));

            stream_receiver_log_status(rpt, msg, STREAM_STATUS_RATE_LIMIT, NDLP_NOTICE);

            stream_receiver_free(rpt);
            return stream_receiver_response_too_busy_now(w);
        }

        last_stream_accepted_t = now;
        spinlock_unlock(&spinlock);
    }

    /*
     * Quick path for rejecting multiple connections. The lock taken is fine-grained - it only protects the receiver
     * pointer within the host (if a host exists). This protects against multiple concurrent web requests hitting
     * separate threads within the web-server and landing here. The lock guards the thread-shutdown sequence that
     * detaches the receiver from the host. If the host is being created (first time-access) then we also use the
     * lock to prevent race-hazard (two threads try to create the host concurrently, one wins and the other does a
     * lookup to the now-attached structure).
     */

    {
        time_t age = 0;
        bool receiver_stale = false;
        bool receiver_working = false;

        rrd_rdlock();
        RRDHOST *host = rrdhost_find_by_guid(rpt->machine_guid);
        if (unlikely(host && rrdhost_flag_check(host, RRDHOST_FLAG_ARCHIVED))) /* Ignore archived hosts. */
            host = NULL;

        if (host) {
            rrdhost_receiver_lock(host);
            if (host->receiver) {
                age = now_monotonic_sec() - host->receiver->last_msg_t;

                if (age < 30)
                    receiver_working = true;
                else
                    receiver_stale = true;
            }
            rrdhost_receiver_unlock(host);
        }
        rrd_rdunlock();

        if (receiver_stale &&
            stream_receiver_signal_to_stop_and_wait(host, STREAM_HANDSHAKE_DISCONNECT_STALE_RECEIVER)) {
            // we stopped the receiver
            // we can proceed with this connection
            receiver_stale = false;

            nd_log_daemon(NDLP_NOTICE, "STREAM '%s' [receive from [%s]:%s]: "
                                       "stopped previous stale receiver to accept this one."
                          , rpt->hostname
                          , rpt->client_ip, rpt->client_port
            );
        }

        if (receiver_working || receiver_stale) {
            // another receiver is already connected
            // try again later

            char msg[200 + 1];
            snprintfz(msg, sizeof(msg) - 1,
                      "rejecting streaming connection; multiple connections for same host, "
                      "old connection was last used %ld secs ago%s",
                      age, receiver_stale ? " (signaled old receiver to stop)" : " (new connection not accepted)");

            stream_receiver_log_status(rpt, msg, STREAM_STATUS_ALREADY_CONNECTED, NDLP_DEBUG);

            // Have not set WEB_CLIENT_FLAG_DONT_CLOSE_SOCKET - caller should clean up
            buffer_flush(w->response.data);
            buffer_strcat(w->response.data, START_STREAMING_ERROR_ALREADY_STREAMING);
            stream_receiver_free(rpt);
            return HTTP_RESP_CONFLICT;
        }
    }

    stream_receiver_takeover_web_connection(w, rpt);

    // after this point, our response code is irrelevant
    // the socket is now ours...

    // read the configuration for this receiver
    stream_conf_receiver_config(rpt, &rpt->config, rpt->key, rpt->machine_guid);

    if(stream_receiver_send_first_response(rpt)) {
        // we are the receiver of the node

        stream_receiver_log_status(
            rpt,
            "connected and ready to receive data",
            STREAM_STATUS_CONNECTED, NDLP_INFO);

        // in case we have cloud connection we inform cloud a new child connected
        schedule_node_state_update(rpt->host, 300);
        rrdhost_set_is_parent_label();

        if (rpt->config.ephemeral)
            rrdhost_option_set(rpt->host, RRDHOST_OPTION_EPHEMERAL_HOST);

        // let it reconnect to parents asap
        rrdhost_stream_parents_reset(rpt->host, STREAM_HANDSHAKE_PREPARING);

        // add it to a stream thread queue
        stream_receiver_add_to_queue(rpt);
    }
    else {
        // we are not the receiver of the node
        // the child has been notified (or we couldn't send a message to it)
        stream_receiver_free(rpt);
    }

    return HTTP_RESP_OK;
}
