/*
 * Copyright (C) 2012 Paul Ionkin <paul.ionkin@gmail.com>
 * Copyright (C) 2012 Skoobe GmbH. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include "s3http_connection.h"

/*{{{ struct*/

#define CON_LOG "con"

static void s3http_connection_on_close (struct evhttp_connection *evcon, void *ctx);
static gboolean s3http_connection_init (S3HttpConnection *con);

/*}}}*/

/*{{{ create / destroy */
// create S3HttpConnection object
// establish HTTP connections to S3
gpointer s3http_connection_create (Application *app)
{
    S3HttpConnection *con;

    con = g_new0 (S3HttpConnection, 1);
    if (!con) {
        LOG_err (CON_LOG, "Failed to create S3HttpConnection !");
        return NULL;
    }
    
    con->app = app;
    con->conf = application_get_conf (app);

    con->is_acquired = FALSE;
    
    if (!s3http_connection_init (con))
        return NULL;

    return (gpointer)con;
}

static gboolean s3http_connection_init (S3HttpConnection *con)
{
    LOG_debug (CON_LOG, "Connecting to %s:%d", 
        conf_get_string (con->conf, "s3.host"),
        conf_get_int (con->conf, "s3.port")
    );

    if (con->evcon)
        evhttp_connection_free (con->evcon);

    // XXX: implement SSL
    con->evcon = evhttp_connection_base_new (
        application_get_evbase (con->app),
        application_get_dnsbase (con->app),
        conf_get_string (con->conf, "s3.host"),
        conf_get_int (con->conf, "s3.port")
    );

    if (!con->evcon) {
        LOG_err (CON_LOG, "Failed to create evhttp_connection !");
        return FALSE;
    }
    
    evhttp_connection_set_timeout (con->evcon, conf_get_int (con->conf, "connection.timeout"));
    evhttp_connection_set_retries (con->evcon, conf_get_int (con->conf, "connection.retries"));

    evhttp_connection_set_closecb (con->evcon, s3http_connection_on_close, con);
    
    return TRUE;
}

// destory S3HttpConnection)
void s3http_connection_destroy (gpointer data)
{
    S3HttpConnection *con = (S3HttpConnection *) data;
    
    if (con->evcon)
        evhttp_connection_free (con->evcon);
    g_free (con);
}
/*}}}*/

void s3http_connection_set_on_released_cb (gpointer client, S3ClientPool_on_released_cb client_on_released_cb, gpointer ctx)
{
    S3HttpConnection *con = (S3HttpConnection *) client;

    con->client_on_released_cb = client_on_released_cb;
    con->pool_ctx = ctx;
}

gboolean s3http_connection_check_rediness (gpointer client)
{
    S3HttpConnection *con = (S3HttpConnection *) client;

    return !con->is_acquired;
}

gboolean s3http_connection_acquire (S3HttpConnection *con)
{
    con->is_acquired = TRUE;

    return TRUE;
}

gboolean s3http_connection_release (S3HttpConnection *con)
{
    con->is_acquired = FALSE;

    if (con->client_on_released_cb)
        con->client_on_released_cb (con, con->pool_ctx);
    
    return TRUE;
}

// callback connection is closed
static void s3http_connection_on_close (struct evhttp_connection *evcon, void *ctx)
{
    S3HttpConnection *con = (S3HttpConnection *) ctx;

    LOG_debug (CON_LOG, "Connection closed !");
}

/*{{{ getters */
Application *s3http_connection_get_app (S3HttpConnection *con)
{
    return con->app;
}

struct evhttp_connection *s3http_connection_get_evcon (S3HttpConnection *con)
{
    return con->evcon;
}

/*}}}*/

/*{{{ get_auth_string */
// create S3 auth string
// http://docs.amazonwebservices.com/AmazonS3/2006-03-01/dev/RESTAuthentication.html
gchar *s3http_connection_get_auth_string (Application *app, 
        const gchar *method, const gchar *content_type, const gchar *resource, const gchar *time_str)
{
    gchar *string_to_sign;
    unsigned int md_len;
    unsigned char md[EVP_MAX_MD_SIZE];
    gchar *res;
    BIO *bmem, *b64;
    BUF_MEM *bptr;
    int ret;
    gchar *tmp;
    ConfData *conf;

    conf = application_get_conf (app);

    //tmp = g_strdup_printf ("/%s%s", conf_get_string (conf, "s3.bucket_name"), resource);
    tmp = g_strdup_printf ("%s", resource);

    string_to_sign = g_strdup_printf (
        "%s\n"  // HTTP-Verb + "\n"
        "%s\n"  // Content-MD5 + "\n"
        "%s\n"  // Content-Type + "\n"
        "%s\n"  // Date + "\n" 
        "%s"    // CanonicalizedAmzHeaders
        "%s",    // CanonicalizedResource

        method, "", content_type, time_str, "", tmp
    );

    g_free (tmp);

   LOG_debug (CON_LOG, "%s", string_to_sign);

    HMAC (EVP_sha1(),
        conf_get_string (conf, "s3.secret_access_key"),
        strlen (conf_get_string (conf, "s3.secret_access_key")),
        (unsigned char *)string_to_sign, strlen (string_to_sign),
        md, &md_len
    );
    g_free (string_to_sign);
    
    b64 = BIO_new (BIO_f_base64 ());
    bmem = BIO_new (BIO_s_mem ());
    b64 = BIO_push (b64, bmem);
    BIO_write (b64, md, md_len);
    ret = BIO_flush (b64);
    if (ret != 1) {
        LOG_err (CON_LOG, "Failed to create base64 of auth string !");
        return NULL;
    }
    BIO_get_mem_ptr (b64, &bptr);

    res = g_malloc (bptr->length);
    memcpy (res, bptr->data, bptr->length);
    res[bptr->length - 1] = '\0';

    BIO_free_all (b64);

    return res;
}
/*}}}*/

// create S3 and setup HTTP connection request
struct evhttp_request *s3http_connection_create_request (S3HttpConnection *con,
    void (*cb)(struct evhttp_request *, void *), void *arg,
    const gchar *auth_str)
{    
    struct evhttp_request *req;
    gchar auth_key[300];
    struct tm *cur_p;
	time_t t = time(NULL);
    struct tm cur;
    char date[50];
    //char hostname[1024];

	gmtime_r(&t, &cur);
	cur_p = &cur;

    snprintf (auth_key, sizeof (auth_key), "AWS %s:%s", conf_get_string (con->conf, "s3.access_key_id"), auth_str);

    req = evhttp_request_new (cb, arg);
    evhttp_add_header (req->output_headers, "Authorization", auth_key);
    evhttp_add_header (req->output_headers, "Host", conf_get_string (con->conf, "s3.host_header"));
		
    if (strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", cur_p) != 0) {
			evhttp_add_header (req->output_headers, "Date", date);
		}
    return req;
}


static const gchar *get_endpoint (const char *xml, size_t xml_len) {
    xmlDocPtr doc;
    xmlXPathContextPtr ctx;
    xmlXPathObjectPtr endpoint_xp;
    xmlNodeSetPtr nodes;
    gchar *endpoint = NULL;

    doc = xmlReadMemory (xml, xml_len, "", NULL, 0);
    ctx = xmlXPathNewContext (doc);
    endpoint_xp = xmlXPathEvalExpression ((xmlChar *) "/Error/Endpoint", ctx);
    nodes = endpoint_xp->nodesetval;

    if (!nodes || nodes->nodeNr < 1) {
        endpoint = NULL;
    } else {
        endpoint = (char *) xmlNodeListGetString (doc, nodes->nodeTab[0]->xmlChildrenNode, 1);
    }

    xmlXPathFreeObject (endpoint_xp);
    xmlXPathFreeContext (ctx);
    xmlFreeDoc (doc);

    return endpoint;
}


typedef struct {
    S3HttpConnection *con;
    S3HttpConnection_responce_cb responce_cb;
    gpointer ctx;

    // number of redirects so far
    gint redirects;

    // original values
    gchar *resource_path;
    gchar *http_cmd;
    struct evbuffer *out_buffer;
} RequestData;

static void request_data_free (RequestData *data)
{
    g_free (data->resource_path);
    g_free (data->http_cmd);
    g_free (data);
}

static void s3http_connection_on_responce_cb (struct evhttp_request *req, void *ctx)
{
    RequestData *data = (RequestData *) ctx;
    struct evbuffer *inbuf;
    const char *buf = NULL;
    size_t buf_len;

    LOG_debug (CON_LOG, "Got HTTP response from server !");

    if (!req) {
        LOG_err (CON_LOG, "Request failed !");
        if (data->responce_cb)
            data->responce_cb (data->con, data->ctx, FALSE, NULL, 0, NULL);
        goto done;
    }
    
    // check if we reached maximum redirect count
    if (data->redirects > conf_get_int (data->con->conf, "connection.max_redirects")) {
        LOG_err (CON_LOG, "Too many redirects !");
        if (data->responce_cb)
            data->responce_cb (data->con, data->ctx, FALSE, NULL, 0, NULL);
        goto done;
    }

    // handle redirect
    if (evhttp_request_get_response_code (req) == 301) {
        gchar *loc;
        struct evkeyvalq *headers;

        data->redirects++;
        headers = evhttp_request_get_input_headers (req);

        loc = evhttp_find_header (headers, "Location");
        if (!loc) {
            inbuf = evhttp_request_get_input_buffer (req);
            buf_len = evbuffer_get_length (inbuf);
            buf = (const char *) evbuffer_pullup (inbuf, buf_len);

            // let's parse XML
            loc = get_endpoint (buf, buf_len);
            
            if (!loc) {
                LOG_err (CON_LOG, "Redirect URL not found !");
                if (data->responce_cb)
                    data->responce_cb (data->con, data->ctx, FALSE, NULL, 0, NULL);
                goto done;
            }
        }

        LOG_debug (CON_LOG, "New URL: %s", loc);

        if (!application_set_url (data->con->app, loc)) {
            if (data->responce_cb)
                data->responce_cb (data->con, data->ctx, FALSE, NULL, 0, NULL);
            goto done;
        }

        if (!s3http_connection_init (data->con)) {
            if (data->responce_cb)
                data->responce_cb (data->con, data->ctx, FALSE, NULL, 0, NULL);
            goto done;
        }

        // re-send request
        s3http_connection_make_request (data->con, data->resource_path, data->http_cmd, data->out_buffer,
            data->responce_cb, data->ctx);
        goto done;
    }

    // 200 and 204 (No Content) are ok
    if (evhttp_request_get_response_code (req) != 200) {
        LOG_err (CON_LOG, "Server returned HTTP error: %d !", evhttp_request_get_response_code (req));
        LOG_debug (CON_LOG, "Error str: %s", req->response_code_line);
        if (data->responce_cb)
            data->responce_cb (data->con, data->ctx, FALSE, NULL, 0, NULL);
        goto done;
    }

    inbuf = evhttp_request_get_input_buffer (req);
    buf_len = evbuffer_get_length (inbuf);
    buf = (const char *) evbuffer_pullup (inbuf, buf_len);
    
    if (data->responce_cb)
        data->responce_cb (data->con, data->ctx, TRUE, buf, buf_len, evhttp_request_get_input_headers (req));
    else
        LOG_debug (CON_LOG, ">>> NO callback function !");

done:
    request_data_free (data);
}

gboolean s3http_connection_make_request (S3HttpConnection *con, 
    const gchar *resource_path,
    const gchar *http_cmd,
    struct evbuffer *out_buffer,
    S3HttpConnection_responce_cb responce_cb,
    gpointer ctx)
{
    gchar *auth_str;
    struct evhttp_request *req;
    gchar auth_key[300];
	time_t t;
    char time_str[50];
    RequestData *data;
    int res;
    enum evhttp_cmd_type cmd_type;
    gchar *request_str;

    data = g_new0 (RequestData, 1);
    data->responce_cb = responce_cb;
    data->ctx = ctx;
    data->con = con;
    data->redirects = 0;
    data->resource_path = g_strdup (resource_path);
    data->http_cmd = g_strdup (http_cmd);
    data->out_buffer = out_buffer;
    
    if (!strcasecmp (http_cmd, "GET")) {
        cmd_type = EVHTTP_REQ_GET;
    } else if (!strcasecmp (http_cmd, "PUT")) {
        cmd_type = EVHTTP_REQ_PUT;
    } else if (!strcasecmp (http_cmd, "DELETE")) {
        cmd_type = EVHTTP_REQ_DELETE;
    } else if (!strcasecmp (http_cmd, "HEAD")) {
        cmd_type = EVHTTP_REQ_HEAD;
    } else {
        LOG_err (CON_LOG, "Unsupported HTTP method: %s", http_cmd);
        request_data_free (data);
        return FALSE;
    }
    
    t = time (NULL);
    strftime (time_str, sizeof (time_str), "%a, %d %b %Y %H:%M:%S GMT", gmtime(&t));
    auth_str = s3http_connection_get_auth_string (con->app, http_cmd, "", resource_path, time_str);
    snprintf (auth_key, sizeof (auth_key), "AWS %s:%s", conf_get_string (con->conf, "s3.access_key_id"), auth_str);
    g_free (auth_str);

    req = evhttp_request_new (s3http_connection_on_responce_cb, data);
    if (!req) {
        LOG_err (CON_LOG, "Failed to create HTTP request object !");
        request_data_free (data);
        return FALSE;
    }

    evhttp_add_header (req->output_headers, "Authorization", auth_key);
    evhttp_add_header (req->output_headers, "Host", conf_get_string (con->conf, "s3.host"));
	evhttp_add_header (req->output_headers, "Date", time_str);

    if (out_buffer) {
        evbuffer_add_buffer (req->output_buffer, out_buffer);
    }

    //if (conf_get_boolean (con->conf, "s3.path_style")) {
    //request_str = g_strdup_printf("/%s%s", conf_get_string (con->conf, "s3.bucket_name"), resource_path);
    request_str = g_strdup_printf("%s", resource_path);
    //}

    LOG_debug (CON_LOG, "[%p] bucket: %s path: %s", con, conf_get_string (con->conf, "s3.bucket_name"), request_str);

    res = evhttp_make_request (s3http_connection_get_evcon (con), req, cmd_type, request_str);
    g_free (request_str);

    if (res < 0) {
        request_data_free (data);
        return FALSE;
    } else
        return TRUE;
}    