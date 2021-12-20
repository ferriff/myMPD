/*
 SPDX-License-Identifier: GPL-3.0-or-later
 myMPD (c) 2018-2021 Juergen Mang <mail@jcgames.de>
 https://github.com/jcorporation/mympd
*/

#include "mympd_config_defs.h"
#include "web_server_radiobrowser.h"

#include "../lib/jsonrpc.h"
#include "../lib/log.h"
#include "../lib/sds_extras.h"
#include "web_server_proxy.h"
#include "web_server_utility.h"

void radiobrowser_api(struct mg_connection *nc, struct mg_connection *backend_nc,
    enum mympd_cmd_ids cmd_id, sds body, int id)
{
    unsigned offset;
    unsigned limit;
    sds filter = NULL;
    sds searchstr = NULL;
    sds error = sdsempty();
    sds uri = sdsempty();
    const char *cmd = get_cmd_id_method_name(cmd_id);

    switch(cmd_id) {
        case MYMPD_API_CLOUD_RADIOBROWSER_SEARCH:
            if (json_get_uint(body, "$.params.offset", 0, MPD_PLAYLIST_LENGTH_MAX, &offset, &error) == true &&
                json_get_uint(body, "$.params.limit", MPD_RESULTS_MIN, MPD_RESULTS_MAX, &limit, &error) == true &&
                json_get_string(body, "$.params.filter", 1, NAME_LEN_MAX, &filter, vcb_isprint, &error) == true &&
                json_get_string(body, "$.params.searchstr", 0, NAME_LEN_MAX, &searchstr, vcb_isname, &error) == true)
            {
                sds searchstr_encoded = sds_urlencode(sdsempty(), searchstr, sdslen(searchstr));
                uri = sdscatprintf(uri, "/json/stations/search?offset=%u&limit=%u&%s=%s",
                    offset, limit, filter, searchstr_encoded);
                FREE_SDS(searchstr_encoded);
            }
            break;
        default:
            error = sdscat(error, "Invalid API method for radiobrowser");
    }

    if (sdslen(error) > 0) {
        sds response = jsonrpc_respond_message(sdsempty(), cmd, id, true,
            "general", "error", error);
        MYMPD_LOG_ERROR("Error processing method \"%s\"", cmd);
        webserver_send_data(nc, response, sdslen(response), "Content-Type: application/json\r\n");
        FREE_SDS(response);
    }
    else {
        bool rc = radiobrowser_send(nc, backend_nc, cmd_id, uri);
        if (rc == false) {
            sds response = jsonrpc_respond_message(sdsempty(), cmd, id, true,
                "general", "error", "Error connection to radio-browser.info");
            webserver_send_data(nc, response, sdslen(response), "Content-Type: application/json\r\n");
            FREE_SDS(response);
        }
    }
    FREE_SDS(filter);
    FREE_SDS(searchstr);
    FREE_SDS(error);
    FREE_SDS(uri);
}

bool radiobrowser_send(struct mg_connection *nc, struct mg_connection *backend_nc,
        enum mympd_cmd_ids cmd_id, const char *request)
{
    const char *host = "de1.api.radio-browser.info";
    sds uri = sdscatprintf(sdsempty(), "https://%s%s", host, request);
    backend_nc = create_http_backend_connection(nc, backend_nc, uri, radiobrowser_handler);
    sdsfree(uri);
    if (backend_nc != NULL) {
        struct backend_nc_data_t *backend_nc_data = (struct backend_nc_data_t *)backend_nc->fn_data;
        backend_nc_data->cmd_id = cmd_id;
        return true;
    }
    return false;
}

void radiobrowser_handler(struct mg_connection *nc, int ev, void *ev_data, void *fn_data) {
    struct t_mg_user_data *mg_user_data = (struct t_mg_user_data *) nc->mgr->userdata;
    struct backend_nc_data_t *backend_nc_data = (struct backend_nc_data_t *)fn_data;
    switch(ev) {
        case MG_EV_CONNECT: {
            MYMPD_LOG_INFO("Backend HTTP connection \"%lu\" connected", nc->id);
            struct mg_str host = mg_url_host(backend_nc_data->uri);
            struct mg_tls_opts tls_opts = {
                .srvname = host
            };
            mg_tls_init(nc, &tls_opts);
            mg_printf(nc, "GET %s HTTP/1.1\r\n"
                "Host: %.*s\r\n"
                "User-Agent: "MYMPD_VERSION"/v9.1.0\r\n"
                "Connection: keep-alive\r\n"
                "\r\n",
                mg_url_uri(backend_nc_data->uri),
                host.len, host.ptr);
            mg_user_data->connection_count++;
            break;
        }
        case MG_EV_ERROR:
            MYMPD_LOG_ERROR("HTTP connection \"%lu\" failed", nc->id);
            break;
        case MG_EV_HTTP_MSG: {
            struct mg_http_message *hm = (struct mg_http_message *) ev_data;
            MYMPD_LOG_DEBUG("Got response from connection \"%lu\": %d bytes", nc->id, hm->body.len);
            const char *cmd = get_cmd_id_method_name(backend_nc_data->cmd_id);
            sds result = jsonrpc_result_start(sdsempty(), cmd, 0);
            result = sdscat(result, "\"data\":");
            result = sdscatlen(result, hm->body.ptr, hm->body.len);
            result = jsonrpc_result_end(result);
            webserver_send_data(backend_nc_data->frontend_nc, result, sdslen(result), "Content-Type: application/json\r\n");
            sdsfree(result);
            break;
        }
        case MG_EV_CLOSE: {
            //print end of jsonrpc message
            MYMPD_LOG_INFO("Backend HTTP connection \"%lu\" closed", nc->id);
            mg_user_data->connection_count--;
            if (backend_nc_data->frontend_nc != NULL) {
                //remove backend connection pointer from frontend connection
                backend_nc_data->frontend_nc->fn_data = NULL;
                //close frontend connection
                backend_nc_data->frontend_nc->is_closing = 1;
                //free backend_nc_data
                free_backend_nc_data(backend_nc_data);
                free(fn_data);
            }
            break;
        }
    }
}
