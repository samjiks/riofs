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
#include "global.h"
#include "s3http_connection.h"
#include "dir_tree.h"
#include "s3fuse.h"
#include "s3client_pool.h"
#include "s3http_client.h"

#define APP_LOG "main"

struct _Application {
    struct event_base *evbase;
    struct evdns_base *dns_base;
    
    S3Fuse *s3fuse;
    DirTree *dir_tree;

    S3HttpConnection *service_con;
    gint service_con_redirects;

    S3ClientPool *write_client_pool;
    S3ClientPool *read_client_pool;
    S3ClientPool *ops_client_pool;

    gchar *aws_access_key_id;
    gchar *aws_secret_access_key;

    gchar *bucket_name;
    struct evhttp_uri *uri;

    gchar *uri_str; // full path to S3 bucket

    gchar *tmp_dir;

    gboolean foreground;
    gchar *mountpoint;

    struct event *sigint_ev;
    struct event *sigpipe_ev;
    struct event *sigusr1_ev;
};

/*{{{ getters */
struct event_base *application_get_evbase (Application *app)
{
    return app->evbase;
}

struct evdns_base *application_get_dnsbase (Application *app)
{
    return app->dns_base;
}

DirTree *application_get_dir_tree (Application *app)
{
    return app->dir_tree;
}

const gchar *application_get_access_key_id (Application *app)
{
    return (const gchar *) app->aws_access_key_id;
}

const gchar *application_get_secret_access_key (Application *app)
{
    return (const gchar *) app->aws_secret_access_key;
}

S3ClientPool *application_get_write_client_pool (Application *app)
{
    return app->write_client_pool;
}

S3ClientPool *application_get_read_client_pool (Application *app)
{
    return app->read_client_pool;
}

S3ClientPool *application_get_ops_client_pool (Application *app)
{
    return app->ops_client_pool;
}

const gchar *application_get_bucket_uri_str (Application *app)
{
    return app->uri_str;
}

const gchar *application_get_bucket_name (Application *app)
{
    return app->bucket_name;
}

struct evhttp_uri *application_get_bucket_uri (Application *app)
{
    return app->uri;
}

const gchar *application_get_tmp_dir (Application *app)
{
    return app->tmp_dir;
}
/*}}}*/

/*{{{ signal handlers */
/* This structure mirrors the one found in /usr/include/asm/ucontext.h */
typedef struct _sig_ucontext {
    unsigned long     uc_flags;
    struct ucontext   *uc_link;
    stack_t           uc_stack;
    struct sigcontext uc_mcontext;
    sigset_t          uc_sigmask;
} sig_ucontext_t;

static void sigsegv_cb (int sig_num, siginfo_t *info, void * ucontext)
{
    void *array[50];
    void *caller_address;
    char **messages;
    int size, i;
    sig_ucontext_t *uc;
    FILE *f;
    
    g_fprintf (stderr, "Got Sigfault !\n");

	uc = (sig_ucontext_t *)ucontext;

    /* Get the address at the time the signal was raised from the EIP (x86) */
#ifdef __i386__
    caller_address = (void *) uc->uc_mcontext.eip;   
#else
    caller_address = (void *) uc->uc_mcontext.rip;   
#endif

	f = stderr;

	fprintf (f, "signal %d (%s), address is %p from %p\n", sig_num, strsignal (sig_num), info->si_addr, (void *)caller_address);

	size = backtrace (array, 50);

	/* overwrite sigaction with caller's address */
	array[1] = caller_address;

	messages = backtrace_symbols (array, size);

	/* skip first stack frame (points here) */
	for (i = 1; i < size && messages != NULL; ++i) {
		fprintf (f, "[bt]: (%d) %s\n", i, messages[i]);
	}

    fflush (f);

	free (messages);

	LOG_err (APP_LOG, "signal %d (%s), address is %p from %p\n", sig_num, strsignal (sig_num), info->si_addr, (void *)caller_address);
//	continue execution, default handler will create core file
//	exit (EXIT_FAILURE);
}

static void sigpipe_cb (G_GNUC_UNUSED evutil_socket_t sig, G_GNUC_UNUSED short events, G_GNUC_UNUSED void *user_data)
{
	LOG_msg (APP_LOG, "Got SIGPIPE");
}

// terminate application without calling destruction functions
static void sigusr1_cb (G_GNUC_UNUSED evutil_socket_t sig, G_GNUC_UNUSED short events, G_GNUC_UNUSED void *user_data)
{
	LOG_err (APP_LOG, "Got SIGUSR1");
    
    exit (1);
}

// terminate application, freeing all used memory
static void sigint_cb (G_GNUC_UNUSED evutil_socket_t sig, G_GNUC_UNUSED short events, void *user_data)
{
	Application *app = (Application *) user_data;

	LOG_err (APP_LOG, "Got SIGINT");

    // terminate after running all active events 
    event_base_loopexit (app->evbase, NULL);
}
/*}}}*/

static gint application_finish_initialization_and_run (Application *app)
{
    struct sigaction sigact;

    // create S3ClientPool for reading operations
    app->read_client_pool = s3client_pool_create (app, 1,
        s3http_client_create,
        s3http_client_destroy,
        s3http_client_set_on_released_cb,
        s3http_client_check_rediness
        );
    if (!app->read_client_pool) {
        LOG_err (APP_LOG, "Failed to create S3ClientPool !");
        return -1;
    }

    // create S3ClientPool for writing operations
    app->write_client_pool = s3client_pool_create (app, 2,
        s3http_connection_create,
        s3http_connection_destroy,
        s3http_connection_set_on_released_cb,
        s3http_connection_check_rediness
        );
    if (!app->write_client_pool) {
        LOG_err (APP_LOG, "Failed to create S3ClientPool !");
        return -1;
    }

    // create S3ClientPool for various operations
    app->ops_client_pool = s3client_pool_create (app, 4,
        s3http_connection_create,
        s3http_connection_destroy,
        s3http_connection_set_on_released_cb,
        s3http_connection_check_rediness
        );
    if (!app->ops_client_pool) {
        LOG_err (APP_LOG, "Failed to create S3ClientPool !");
        return -1;
    }

/*{{{ DirTree*/
    app->dir_tree = dir_tree_create (app);
    if (!app->dir_tree) {
        LOG_err (APP_LOG, "Failed to create DirTree !");
        return -1;
    }
/*}}}*/

/*{{{ FUSE*/
    app->s3fuse = s3fuse_new (app, app->mountpoint);
    if (!app->s3fuse) {
        LOG_err (APP_LOG, "Failed to create FUSE fs !");
        return -1;
    }
/*}}}*/

/*{{{ signal handlers*/
	// SIGINT
	app->sigint_ev = evsignal_new (app->evbase, SIGINT, sigint_cb, app);
	event_add (app->sigint_ev, NULL);
	// SIGSEGV
    sigact.sa_sigaction = sigsegv_cb;
    sigact.sa_flags = (int)SA_RESETHAND | SA_SIGINFO;
	sigemptyset (&sigact.sa_mask);
    if (sigaction (SIGSEGV, &sigact, (struct sigaction *) NULL) != 0) {
        fprintf (stderr, "error setting signal handler for %d (%s)\n", SIGSEGV, strsignal(SIGSEGV));
		return 1;
    }
	// SIGABRT
    sigact.sa_sigaction = sigsegv_cb;
    sigact.sa_flags = (int)SA_RESETHAND | SA_SIGINFO;
	sigemptyset (&sigact.sa_mask);
    if (sigaction (SIGABRT, &sigact, (struct sigaction *) NULL) != 0) {
        fprintf (stderr, "error setting signal handler for %d (%s)\n", SIGABRT, strsignal(SIGABRT));
		return 1;
    }
	// SIGPIPE
	app->sigpipe_ev = evsignal_new (app->evbase, SIGPIPE, sigpipe_cb, app);
	event_add (app->sigpipe_ev, NULL);
    // SIGUSR1
	app->sigusr1_ev = evsignal_new (app->evbase, SIGUSR1, sigusr1_cb, app);
	event_add (app->sigusr1_ev, NULL);
/*}}}*/
    
    if (!app->foreground)
        fuse_daemonize (0);

    return 0;
}

// S3 replies with error on initial HEAD request
static void application_get_service_on_error (S3HttpConnection *con, void *ctx)
{
    Application *app = (Application *)ctx;

    LOG_err (APP_LOG, "Failed to access S3 bucket URL !");

    s3http_connection_destroy (con);
    
    event_base_loopexit (app->evbase, NULL);
}

// S3 replies on initial HEAD request
static void application_get_service_on_done (S3HttpConnection *con, void *ctx, 
        G_GNUC_UNUSED const gchar *buf, G_GNUC_UNUSED size_t buf_len, struct evkeyvalq *headers)
{
    Application *app = (Application *)ctx;
    const char *loc;
    
    // make sure it breaks infinite redirect loop
    if (app->service_con_redirects > 20) {
        LOG_err (APP_LOG, "Too many redirects !");
        event_base_loopexit (app->evbase, NULL);
        return;
    }
    app->service_con_redirects ++;

    loc = evhttp_find_header (headers, "Location");
    // redirect detected, use new location
    if (loc) {
        s3http_connection_destroy (con);
        
        g_free (app->uri_str);
        evhttp_uri_free (app->uri);

        app->uri = evhttp_uri_parse (loc);
        if (!app->uri) {
            LOG_err (APP_LOG, "Invalid S3 service URL !");
            event_base_loopexit (app->evbase, NULL);
            return;
        }
        app->uri_str = g_strdup (evhttp_uri_get_host (app->uri));
        LOG_debug (APP_LOG, "New service URL: %s", app->uri_str);
        
        // perform a new request
        app->service_con = s3http_connection_create (app);
        if (!app->service_con) {
            LOG_err (APP_LOG, "Failed to execute a request !");
            event_base_loopexit (app->evbase, NULL);
            return;
        }

        if (!s3http_connection_make_request (app->service_con, "/", "/", "HEAD", NULL,
            application_get_service_on_done, application_get_service_on_error, app)) {

            LOG_err (APP_LOG, "Failed to execute a request !");
            event_base_loopexit (app->evbase, NULL);
            return;
        }
    } else {
        application_finish_initialization_and_run (app);
    }
}

static void application_destroy (Application *app)
{
    // destroy S3Fuse
    if (app->s3fuse)
        s3fuse_destroy (app->s3fuse);

    if (app->read_client_pool)
        s3client_pool_destroy (app->read_client_pool);
    if (app->write_client_pool)
        s3client_pool_destroy (app->write_client_pool);
    if (app->ops_client_pool)
        s3client_pool_destroy (app->ops_client_pool);

    if (app->dir_tree)
        dir_tree_destroy (app->dir_tree);

    if (app->sigint_ev)
        event_free (app->sigint_ev);
    if (app->sigpipe_ev)
        event_free (app->sigpipe_ev);
    if (app->sigusr1_ev)
        event_free (app->sigusr1_ev);
    
    if (app->service_con)
        s3http_connection_destroy (app->service_con);

    evdns_base_free (app->dns_base, 0);
    event_base_free (app->evbase);

    g_free (app->mountpoint);
    g_free (app->tmp_dir);
    g_free (app->bucket_name);
    g_free (app->uri_str);
    evhttp_uri_free (app->uri);

    g_free (app);
    
    ENGINE_cleanup ();
    CRYPTO_cleanup_all_ex_data ();
	ERR_free_strings ();
	ERR_remove_thread_state (NULL);
	CRYPTO_mem_leaks_fp (stderr);
}

static void print_usage (const char *progname)
{
    g_fprintf (stderr, "Usage: %s [http://s3.amazonaws.com] [bucketname] [-v] [mountpoint]\n", progname);
    g_fprintf (stderr, "Please set both AWSACCESSKEYID and AWSSECRETACCESSKEY environment variables !\n");
}

int main (int argc, char *argv[])
{
    Application *app;
    gboolean verbose = FALSE;
    GError *error = NULL;
    GOptionContext *context;
    gchar **s_mountpoint = NULL;
    gchar *progname;
    gboolean foreground = FALSE;

    GOptionEntry entries[] = {
	    { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &s_mountpoint, "Mountpoint", NULL },
        { "foreground", 'f', 0, G_OPTION_ARG_NONE, &foreground, "Do not daemonize process", NULL },
        { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Verbose output", NULL },
        { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
    };

    // init libraries
    ENGINE_load_builtin_engines ();
    ENGINE_register_all_complete ();

    progname = argv[0];

    // init main app structure
    app = g_new0 (Application, 1);
    app->service_con_redirects = 0;
    app->evbase = event_base_new ();

    //XXX: fix it
    app->tmp_dir = g_strdup ("/tmp");

    if (!app->evbase) {
        LOG_err (APP_LOG, "Failed to create event base !");
        return -1;
    }

    app->dns_base = evdns_base_new (app->evbase, 1);
    if (!app->dns_base) {
        LOG_err (APP_LOG, "Failed to create DNS base !");
        return -1;
    }

/*{{{ cmd line args */
    // get access parameters from the enviorment
    app->aws_access_key_id = getenv("AWSACCESSKEYID");
    app->aws_secret_access_key = getenv("AWSSECRETACCESSKEY");

    if (!app->aws_access_key_id || !app->aws_secret_access_key) {
        print_usage (progname);
        return -1;
    }

    if (argc < 3) {
        print_usage (progname);
        return -1;
    }

    app->uri = evhttp_uri_parse (argv[1]);
    if (!app->uri) {
        print_usage (progname);
        return -1;
    }
    app->bucket_name = g_strdup (argv[2]);
    app->uri_str = g_strdup_printf ("%s.%s", app->bucket_name, evhttp_uri_get_host (app->uri));

    argv += 2;
    argc -= 2;

    // parse command line options
    context = g_option_context_new (NULL);
    g_option_context_add_main_entries (context, entries, NULL);
    if (!g_option_context_parse (context, &argc, &argv, &error)) {
        g_fprintf (stderr, "Failed to parse command line options: %s\n", error->message);
        return FALSE;
    }
    
    if (!s_mountpoint || g_strv_length (s_mountpoint) < 1) {
        print_usage (progname);
        return -1;
    }
    app->mountpoint = g_strdup (s_mountpoint[0]);
    g_strfreev (s_mountpoint);

    app->foreground = foreground;

    if (verbose)
        log_level = LOG_debug;
    else
        log_level = LOG_msg;
    
    g_option_context_free (context);
    /*}}}*/
    
    // perform the initial request to get S3 service URL (in case of redirect)
    app->service_con = s3http_connection_create (app);
    if (!app->service_con) 
        return -1;

    if (!s3http_connection_make_request (app->service_con, "/", "/", "HEAD", NULL,
        application_get_service_on_done, application_get_service_on_error, app))
        return -1;

    // start the loop
    event_base_dispatch (app->evbase);

    application_destroy (app);

    return 0;
}
