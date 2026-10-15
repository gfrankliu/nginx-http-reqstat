
/*
 * Copyright (C) 2010-2015 Alibaba Group Holding Limited
 */


#include "ngx_http_reqstat.h"


#define NGX_HTTP_REQSTAT_CHUNK_SIZE  16384

/* "200:12;404:1;...;other:3" */
#define NGX_HTTP_REQSTAT_CODES_TEXT_LEN                                 \
    (NGX_HTTP_REQSTAT_CODE_SLOTS                                        \
         * (NGX_HTTP_REQSTAT_CODE_LEN + NGX_ATOMIC_T_LEN + 2)           \
     + sizeof("other:") + NGX_ATOMIC_T_LEN)

/* {"200":12,"404":1,...,"other":3} */
#define NGX_HTTP_REQSTAT_CODES_JSON_LEN                                 \
    (NGX_HTTP_REQSTAT_CODE_SLOTS                                        \
         * (NGX_HTTP_REQSTAT_CODE_LEN + NGX_ATOMIC_T_LEN + 4)           \
     + sizeof("{\"other\":}") + NGX_ATOMIC_T_LEN)


typedef struct {
    ngx_http_reqstat_rbnode_t   *node;
    ngx_str_t                    key;
} ngx_http_reqstat_snapshot_t;


typedef struct {
    ngx_shm_zone_t              *zone;
    ngx_http_reqstat_ctx_t      *ctx;
    ngx_array_t                  nodes;    /* ngx_http_reqstat_snapshot_t */
} ngx_http_reqstat_zone_snapshot_t;


typedef struct {
    ngx_pool_t                  *pool;
    ngx_chain_t                 *out;
    ngx_chain_t                **last;
    ngx_buf_t                   *buf;
    off_t                        size;
} ngx_http_reqstat_writer_t;


/* prometheus metric families, in output order */

enum {
    NGX_HTTP_REQSTAT_PROM_BYTES_IN = 0,
    NGX_HTTP_REQSTAT_PROM_BYTES_OUT,
    NGX_HTTP_REQSTAT_PROM_CONNECTIONS,
    NGX_HTTP_REQSTAT_PROM_REQUESTS,
    NGX_HTTP_REQSTAT_PROM_RESPONSES,
    NGX_HTTP_REQSTAT_PROM_RESPONSES_BY_CODE,
    NGX_HTTP_REQSTAT_PROM_REQUEST_TIME,
    NGX_HTTP_REQSTAT_PROM_UPS_REQUESTS,
    NGX_HTTP_REQSTAT_PROM_UPS_TRIES,
    NGX_HTTP_REQSTAT_PROM_UPS_RESPONSE_TIME,
    NGX_HTTP_REQSTAT_PROM_UPS_RESPONSES,
    NGX_HTTP_REQSTAT_PROM_STATUS_CODES,
    NGX_HTTP_REQSTAT_PROM_INDICATOR,
    NGX_HTTP_REQSTAT_PROM_NFAMILIES
};


typedef struct {
    ngx_str_t                    name;
    ngx_str_t                    type;
    ngx_str_t                    help;
} ngx_http_reqstat_prom_family_t;


typedef struct {
    ngx_uint_t                   family;
    ngx_str_t                    label;
    ngx_uint_t                   msec;     /* value is in ms, print seconds */
} ngx_http_reqstat_prom_field_t;


static ngx_http_reqstat_prom_family_t
    ngx_http_reqstat_prom_families[NGX_HTTP_REQSTAT_PROM_NFAMILIES] =
{
    { ngx_string("nginx_reqstat_bytes_in_total"), ngx_string("counter"),
      ngx_string("Total bytes received from clients.") },
    { ngx_string("nginx_reqstat_bytes_out_total"), ngx_string("counter"),
      ngx_string("Total bytes sent to clients.") },
    { ngx_string("nginx_reqstat_connections_total"), ngx_string("counter"),
      ngx_string("Total client connections accepted.") },
    { ngx_string("nginx_reqstat_requests_total"), ngx_string("counter"),
      ngx_string("Total requests processed.") },
    { ngx_string("nginx_reqstat_responses_total"), ngx_string("counter"),
      ngx_string("Total responses by status class.") },
    { ngx_string("nginx_reqstat_responses_by_code_total"),
      ngx_string("counter"),
      ngx_string("Total responses by selected status code.") },
    { ngx_string("nginx_reqstat_request_time_seconds_total"),
      ngx_string("counter"),
      ngx_string("Accumulated request processing time.") },
    { ngx_string("nginx_reqstat_upstream_requests_total"),
      ngx_string("counter"),
      ngx_string("Total requests passed to upstreams.") },
    { ngx_string("nginx_reqstat_upstream_tries_total"), ngx_string("counter"),
      ngx_string("Total upstream peers tried.") },
    { ngx_string("nginx_reqstat_upstream_response_time_seconds_total"),
      ngx_string("counter"),
      ngx_string("Accumulated upstream response time.") },
    { ngx_string("nginx_reqstat_upstream_responses_total"),
      ngx_string("counter"),
      ngx_string("Total upstream responses by status class.") },
    { ngx_string("nginx_reqstat_status_codes_total"), ngx_string("counter"),
      ngx_string("Total responses by exact status code.") },
    { ngx_string("nginx_reqstat_indicator_total"), ngx_string("counter"),
      ngx_string("Accumulated user defined indicators.") }
};


static ngx_http_reqstat_prom_field_t
    ngx_http_reqstat_prom_fields[NGX_HTTP_REQSTAT_RSRV] =
{
    { NGX_HTTP_REQSTAT_PROM_BYTES_IN, ngx_null_string, 0 },
    { NGX_HTTP_REQSTAT_PROM_BYTES_OUT, ngx_null_string, 0 },
    { NGX_HTTP_REQSTAT_PROM_CONNECTIONS, ngx_null_string, 0 },
    { NGX_HTTP_REQSTAT_PROM_REQUESTS, ngx_null_string, 0 },
    { NGX_HTTP_REQSTAT_PROM_RESPONSES, ngx_string("status=\"2xx\""), 0 },
    { NGX_HTTP_REQSTAT_PROM_RESPONSES, ngx_string("status=\"3xx\""), 0 },
    { NGX_HTTP_REQSTAT_PROM_RESPONSES, ngx_string("status=\"4xx\""), 0 },
    { NGX_HTTP_REQSTAT_PROM_RESPONSES, ngx_string("status=\"5xx\""), 0 },
    { NGX_HTTP_REQSTAT_PROM_RESPONSES, ngx_string("status=\"other\""), 0 },
    { NGX_HTTP_REQSTAT_PROM_REQUEST_TIME, ngx_null_string, 1 },
    { NGX_HTTP_REQSTAT_PROM_UPS_REQUESTS, ngx_null_string, 0 },
    { NGX_HTTP_REQSTAT_PROM_UPS_RESPONSE_TIME, ngx_null_string, 1 },
    { NGX_HTTP_REQSTAT_PROM_UPS_TRIES, ngx_null_string, 0 },
    { NGX_HTTP_REQSTAT_PROM_RESPONSES_BY_CODE, ngx_string("code=\"200\""), 0 },
    { NGX_HTTP_REQSTAT_PROM_RESPONSES_BY_CODE, ngx_string("code=\"206\""), 0 },
    { NGX_HTTP_REQSTAT_PROM_RESPONSES_BY_CODE, ngx_string("code=\"302\""), 0 },
    { NGX_HTTP_REQSTAT_PROM_RESPONSES_BY_CODE, ngx_string("code=\"304\""), 0 },
    { NGX_HTTP_REQSTAT_PROM_RESPONSES_BY_CODE, ngx_string("code=\"403\""), 0 },
    { NGX_HTTP_REQSTAT_PROM_RESPONSES_BY_CODE, ngx_string("code=\"404\""), 0 },
    { NGX_HTTP_REQSTAT_PROM_RESPONSES_BY_CODE, ngx_string("code=\"416\""), 0 },
    { NGX_HTTP_REQSTAT_PROM_RESPONSES_BY_CODE, ngx_string("code=\"499\""), 0 },
    { NGX_HTTP_REQSTAT_PROM_RESPONSES_BY_CODE, ngx_string("code=\"500\""), 0 },
    { NGX_HTTP_REQSTAT_PROM_RESPONSES_BY_CODE, ngx_string("code=\"502\""), 0 },
    { NGX_HTTP_REQSTAT_PROM_RESPONSES_BY_CODE, ngx_string("code=\"503\""), 0 },
    { NGX_HTTP_REQSTAT_PROM_RESPONSES_BY_CODE, ngx_string("code=\"504\""), 0 },
    { NGX_HTTP_REQSTAT_PROM_RESPONSES_BY_CODE, ngx_string("code=\"508\""), 0 },
    { NGX_HTTP_REQSTAT_PROM_RESPONSES_BY_CODE, ngx_string("code=\"other\""),
      0 },
    { NGX_HTTP_REQSTAT_PROM_UPS_RESPONSES, ngx_string("status=\"4xx\""), 0 },
    { NGX_HTTP_REQSTAT_PROM_UPS_RESPONSES, ngx_string("status=\"5xx\""), 0 }
};


static ngx_http_output_body_filter_pt  ngx_http_next_body_filter;


static variable_index_t REQSTAT_RSRV_VARIABLES[NGX_HTTP_REQSTAT_RSRV] = {
    variable_index("bytes_in", 0),
    variable_index("bytes_out", 1),
    variable_index("conn_total", 2),
    variable_index("req_total", 3),
    variable_index("http_2xx", 4),
    variable_index("http_3xx", 5),
    variable_index("http_4xx", 6),
    variable_index("http_5xx", 7),
    variable_index("http_other_status", 8),
    variable_index("rt", 9),
    variable_index("ups_req", 10),
    variable_index("ups_rt", 11),
    variable_index("ups_tries", 12),
    variable_index("http_200", 13),
    variable_index("http_206", 14),
    variable_index("http_302", 15),
    variable_index("http_304", 16),
    variable_index("http_403", 17),
    variable_index("http_404", 18),
    variable_index("http_416", 19),
    variable_index("http_499", 20),
    variable_index("http_500", 21),
    variable_index("http_502", 22),
    variable_index("http_503", 23),
    variable_index("http_504", 24),
    variable_index("http_508", 25),
    variable_index("http_other_detail_status", 26),
    variable_index("http_ups_4xx", 27),
    variable_index("http_ups_5xx", 28),
};


static off_t ngx_http_reqstat_fields[NGX_HTTP_REQSTAT_RSRV] = {
    NGX_HTTP_REQSTAT_BYTES_IN,
    NGX_HTTP_REQSTAT_BYTES_OUT,
    NGX_HTTP_REQSTAT_CONN_TOTAL,
    NGX_HTTP_REQSTAT_REQ_TOTAL,
    NGX_HTTP_REQSTAT_2XX,
    NGX_HTTP_REQSTAT_3XX,
    NGX_HTTP_REQSTAT_4XX,
    NGX_HTTP_REQSTAT_5XX,
    NGX_HTTP_REQSTAT_OTHER_STATUS,
    NGX_HTTP_REQSTAT_RT,
    NGX_HTTP_REQSTAT_UPS_REQ,
    NGX_HTTP_REQSTAT_UPS_RT,
    NGX_HTTP_REQSTAT_UPS_TRIES,
    NGX_HTTP_REQSTAT_200,
    NGX_HTTP_REQSTAT_206,
    NGX_HTTP_REQSTAT_302,
    NGX_HTTP_REQSTAT_304,
    NGX_HTTP_REQSTAT_403,
    NGX_HTTP_REQSTAT_404,
    NGX_HTTP_REQSTAT_416,
    NGX_HTTP_REQSTAT_499,
    NGX_HTTP_REQSTAT_500,
    NGX_HTTP_REQSTAT_502,
    NGX_HTTP_REQSTAT_503,
    NGX_HTTP_REQSTAT_504,
    NGX_HTTP_REQSTAT_508,
    NGX_HTTP_REQSTAT_OTHER_DETAIL_STATUS,
    NGX_HTTP_REQSTAT_UPS_4XX,
    NGX_HTTP_REQSTAT_UPS_5XX
};


static ngx_conf_enum_t  ngx_http_reqstat_formats[] = {
    { ngx_string("text"), NGX_HTTP_REQSTAT_FORMAT_TEXT },
    { ngx_string("prometheus"), NGX_HTTP_REQSTAT_FORMAT_PROMETHEUS },
    { ngx_string("json"), NGX_HTTP_REQSTAT_FORMAT_JSON },
    { ngx_null_string, 0 }
};


static void *ngx_http_reqstat_create_main_conf(ngx_conf_t *cf);
static void *ngx_http_reqstat_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_reqstat_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);
static ngx_int_t ngx_http_reqstat_init(ngx_conf_t *cf);
static ngx_int_t ngx_http_reqstat_add_variable(ngx_conf_t *cf);

static ngx_int_t ngx_http_reqstat_variable_enable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);

static char *ngx_http_reqstat_show(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_reqstat_show_field(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_reqstat_zone(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_reqstat_zone_add_indicator(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static char *ngx_http_reqstat_zone_key_length(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static char *ngx_http_reqstat_zone_recycle(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static char *ngx_http_reqstat(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_shm_zone_t *ngx_http_reqstat_get_zone(ngx_conf_t *cf,
    ngx_str_t *name);
static void ngx_http_reqstat_count(void *data, off_t offset,
    ngx_int_t incr);
static void ngx_http_reqstat_count_code(ngx_http_reqstat_rbnode_t *node,
    ngx_atomic_uint_t code);
static ngx_int_t ngx_http_reqstat_init_zone(ngx_shm_zone_t *shm_zone,
    void *data);

static ngx_int_t ngx_http_reqstat_log_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_reqstat_init_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_reqstat_show_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_reqstat_body_filter(ngx_http_request_t *r,
    ngx_chain_t *in);

static ngx_uint_t ngx_http_reqstat_first_request(ngx_http_request_t *r);
static void ngx_http_reqstat_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel);
static ngx_http_reqstat_store_t *
    ngx_http_reqstat_create_store(ngx_http_request_t *r,
    ngx_http_reqstat_conf_t *rlcf);
static ngx_int_t ngx_http_reqstat_check_enable(ngx_http_request_t *r,
    ngx_http_reqstat_conf_t **rlcf, ngx_http_reqstat_store_t **store);

static ngx_int_t ngx_http_reqstat_snapshot(ngx_http_request_t *r,
    ngx_array_t *display, ngx_str_t *key, ngx_array_t *zones);
static u_char *ngx_http_reqstat_reserve(ngx_http_reqstat_writer_t *w,
    size_t size);
static ngx_int_t ngx_http_reqstat_write_text(ngx_http_reqstat_writer_t *w,
    ngx_http_reqstat_conf_t *rlcf, ngx_array_t *zones);
static ngx_int_t ngx_http_reqstat_write_prometheus(
    ngx_http_reqstat_writer_t *w, ngx_http_reqstat_conf_t *rlcf,
    ngx_array_t *zones);
static ngx_int_t ngx_http_reqstat_write_prom_samples(
    ngx_http_reqstat_writer_t *w, ngx_http_reqstat_zone_snapshot_t *zs,
    ngx_uint_t n, ngx_http_reqstat_prom_family_t *family, ngx_uint_t field,
    ngx_str_t *label, ngx_uint_t msec);
static u_char *ngx_http_reqstat_prom_escape(u_char *dst, u_char *src,
    size_t size);
static ngx_int_t ngx_http_reqstat_write_prom_codes(
    ngx_http_reqstat_writer_t *w, ngx_http_reqstat_zone_snapshot_t *zs,
    ngx_uint_t n, ngx_http_reqstat_prom_family_t *family);
static ngx_int_t ngx_http_reqstat_write_json(ngx_http_reqstat_writer_t *w,
    ngx_array_t *zones, ngx_uint_t single);
static ngx_int_t ngx_http_reqstat_write_json_not_found(
    ngx_http_reqstat_writer_t *w, ngx_str_t *key);
static u_char *ngx_http_reqstat_codes_text(u_char *p,
    ngx_http_reqstat_rbnode_t *node);
static u_char *ngx_http_reqstat_codes_json(u_char *p,
    ngx_http_reqstat_rbnode_t *node);
static u_char *ngx_http_reqstat_code_name(u_char *p, ngx_atomic_uint_t code);


static ngx_command_t   ngx_http_reqstat_commands[] = {

    { ngx_string("req_status_zone"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE3,
      ngx_http_reqstat_zone,
      0,
      0,
      NULL },

    { ngx_string("req_status"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_reqstat,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("req_status_lazy"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_reqstat_conf_t, lazy),
      NULL },

    { ngx_string("req_status_bypass"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_set_predicate_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_reqstat_conf_t, bypass),
      NULL },

    { ngx_string("req_status_show"),
      NGX_HTTP_LOC_CONF|NGX_CONF_ANY,
      ngx_http_reqstat_show,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("req_status_show_field"),
      NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_reqstat_show_field,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("req_status_show_format"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_enum_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_reqstat_conf_t, format),
      &ngx_http_reqstat_formats },

    { ngx_string("req_status_show_key"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_set_complex_value_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_reqstat_conf_t, show_key),
      NULL },

    { ngx_string("req_status_zone_add_indicator"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_2MORE,
      ngx_http_reqstat_zone_add_indicator,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("req_status_zone_key_length"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE2,
      ngx_http_reqstat_zone_key_length,
      0,
      0,
      NULL },

    { ngx_string("req_status_zone_recycle"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE3,
      ngx_http_reqstat_zone_recycle,
      0,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_reqstat_module_ctx = {
    ngx_http_reqstat_add_variable,         /* preconfiguration */
    ngx_http_reqstat_init,                 /* postconfiguration */

    ngx_http_reqstat_create_main_conf,     /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_reqstat_create_loc_conf,      /* create location configuration */
    ngx_http_reqstat_merge_loc_conf        /* merge location configuration */
};


ngx_module_t  ngx_http_reqstat_module = {
    NGX_MODULE_V1,
    &ngx_http_reqstat_module_ctx,          /* module context */
    ngx_http_reqstat_commands,             /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static void *
ngx_http_reqstat_create_main_conf(ngx_conf_t *cf)
{
    return ngx_pcalloc(cf->pool, sizeof(ngx_http_reqstat_conf_t));
}


static void *
ngx_http_reqstat_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_reqstat_conf_t      *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_reqstat_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->lazy = NGX_CONF_UNSET;
    conf->format = NGX_CONF_UNSET_UINT;
    conf->bypass = NGX_CONF_UNSET_PTR;
    conf->monitor = NGX_CONF_UNSET_PTR;
    conf->display = NGX_CONF_UNSET_PTR;
    conf->user_select = NGX_CONF_UNSET_PTR;
    conf->user_defined_str = NGX_CONF_UNSET_PTR;
    conf->show_key = NGX_CONF_UNSET_PTR;

    return conf;
}


static char *
ngx_http_reqstat_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child)
{
    ngx_http_reqstat_conf_t      *conf = child;
    ngx_http_reqstat_conf_t      *prev = parent;

    ngx_conf_merge_value(conf->lazy, prev->lazy, 0);
    ngx_conf_merge_uint_value(conf->format, prev->format,
                              NGX_HTTP_REQSTAT_FORMAT_TEXT);
    ngx_conf_merge_ptr_value(conf->bypass, prev->bypass, NULL);
    ngx_conf_merge_ptr_value(conf->monitor, prev->monitor, NULL);
    ngx_conf_merge_ptr_value(conf->display, prev->display, NULL);
    ngx_conf_merge_ptr_value(conf->user_select, prev->user_select, NULL);
    ngx_conf_merge_ptr_value(conf->user_defined_str, prev->user_defined_str,
                             NULL);
    ngx_conf_merge_ptr_value(conf->show_key, prev->show_key, NULL);

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_reqstat_add_variable(ngx_conf_t *cf)
{
    ngx_str_t                  name = ngx_string("reqstat_enable");
    ngx_http_variable_t       *var;
    ngx_http_reqstat_conf_t   *rmcf;

    rmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_reqstat_module);

    var = ngx_http_add_variable(cf, &name, 0);
    if (var == NULL) {
        return NGX_ERROR;
    }

    var->get_handler = ngx_http_reqstat_variable_enable;
    var->data = 0;

    rmcf->index = ngx_http_get_variable_index(cf, &name);

    if (rmcf->index == NGX_ERROR) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_reqstat_init(ngx_conf_t *cf)
{
    ngx_uint_t                    i;
    ngx_array_t                  *display;
    ngx_http_handler_pt          *h;
    ngx_http_reqstat_conf_t      *rmcf, **loc;
    ngx_http_core_main_conf_t    *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_LOG_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_reqstat_log_handler;

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_REWRITE_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_reqstat_init_handler;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_reqstat_body_filter;

    rmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_reqstat_module);

    if (rmcf->show_locations == NULL) {
        return NGX_OK;
    }

    loc = rmcf->show_locations->elts;

    for (i = 0; i < rmcf->show_locations->nelts; i++) {
        if (loc[i]->format != NGX_HTTP_REQSTAT_FORMAT_JSON) {
            continue;
        }

        display = loc[i]->display ? loc[i]->display : rmcf->monitor;

        if (display == NULL || display->nelts != 1) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "\"req_status_show_format json\" requires "
                               "exactly one zone, specify it in "
                               "\"req_status_show\"");
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


static char *
ngx_http_reqstat_show_field(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_int_t                  valid, *index;
    ngx_str_t                 *value, *user;
    ngx_uint_t                 i, j;
    ngx_http_reqstat_conf_t   *rmcf, *rlcf;

    rlcf = conf;
    rmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_reqstat_module);

    if (rlcf->user_select != NGX_CONF_UNSET_PTR) {
        return "is duplicate";
    }

    rlcf->user_select = ngx_array_create(cf->pool, cf->args->nelts - 1,
                                         sizeof(ngx_int_t));
    if (rlcf->user_select == NULL) {
        return NGX_CONF_ERROR;
    }

    index = ngx_array_push_n(rlcf->user_select, cf->args->nelts - 1);
    if (index == NULL) {
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;
    for (i = 1; i < cf->args->nelts; i++) {
        valid = 0;

        if (value[i].len > 1 && value[i].data[0] == '$') {
            value[i].data++;
            value[i].len--;

            if (rmcf->user_defined_str) {

                user = rmcf->user_defined_str->elts;

                for (j = 0; j < rmcf->user_defined_str->nelts; j++) {
                    if (value[i].len != user[j].len
                        || ngx_strncmp(value[i].data, user[j].data,
                                       value[i].len) != 0)
                    {
                        continue;
                    }

                    *index++ = NGX_HTTP_REQSTAT_RSRV + j;
                    valid = 1;
                    break;
                }
            }

        } else if (value[i].len == sizeof("status_codes") - 1
                   && ngx_strncmp(value[i].data, "status_codes",
                                  value[i].len) == 0)
        {
            *index++ = NGX_HTTP_REQSTAT_FIELD_CODES;
            valid = 1;

        } else {
            for (j = 0; j < NGX_HTTP_REQSTAT_RSRV; j++) {
                if (value[i].len != REQSTAT_RSRV_VARIABLES[j].name.len
                    || ngx_strncmp(REQSTAT_RSRV_VARIABLES[j].name.data,
                                   value[i].data, value[i].len) != 0)
                {
                    continue;
                }

                *index++ = REQSTAT_RSRV_VARIABLES[j].index;
                valid = 1;
                break;
            }
        }

        if (!valid) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "field \"%V\" does not exist", &value[i]);
            return NGX_CONF_ERROR;
        }
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_reqstat_show(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t                    *value;
    ngx_uint_t                    i;
    ngx_shm_zone_t               *shm_zone, **z;
    ngx_http_core_loc_conf_t     *clcf;
    ngx_http_reqstat_conf_t      *smcf, **loc;

    ngx_http_reqstat_conf_t      *rlcf = conf;

    value = cf->args->elts;

    if (rlcf->display != NGX_CONF_UNSET_PTR) {
        return "is duplicate";
    }

    smcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_reqstat_module);

    if (smcf->show_locations == NULL) {
        smcf->show_locations = ngx_array_create(cf->pool, 4,
                                          sizeof(ngx_http_reqstat_conf_t *));
        if (smcf->show_locations == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    loc = ngx_array_push(smcf->show_locations);
    if (loc == NULL) {
        return NGX_CONF_ERROR;
    }

    *loc = rlcf;

    if (cf->args->nelts == 1) {
        rlcf->display = NULL;
        goto reg_handler;
    }

    rlcf->display = ngx_array_create(cf->pool, cf->args->nelts - 1,
                                     sizeof(ngx_shm_zone_t *));
    if (rlcf->display == NULL) {
        return NGX_CONF_ERROR;
    }

    for (i = 1; i < cf->args->nelts; i++) {
        shm_zone = ngx_shared_memory_add(cf, &value[i], 0,
                                         &ngx_http_reqstat_module);
        if (shm_zone == NULL) {
            return NGX_CONF_ERROR;
        }

        z = ngx_array_push(rlcf->display);
        if (z == NULL) {
            return NGX_CONF_ERROR;
        }

        *z = shm_zone;
    }

reg_handler:

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_reqstat_show_handler;

    return NGX_CONF_OK;
}


static ngx_shm_zone_t *
ngx_http_reqstat_get_zone(ngx_conf_t *cf, ngx_str_t *name)
{
    ngx_shm_zone_t                    *shm_zone;

    shm_zone = ngx_shared_memory_add(cf, name, 0, &ngx_http_reqstat_module);
    if (shm_zone == NULL) {
        return NULL;
    }

    if (shm_zone->data == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "zone \"%V\" should be defined first", name);
        return NULL;
    }

    return shm_zone;
}


static char *
ngx_http_reqstat_zone_add_indicator(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_int_t                         *i;
    ngx_str_t                         *value, *name;
    ngx_uint_t                         j;
    ngx_shm_zone_t                    *shm_zone;
    ngx_http_reqstat_ctx_t            *ctx;
    ngx_http_reqstat_conf_t           *rmcf;

    rmcf = conf;
    value = cf->args->elts;

    shm_zone = ngx_http_reqstat_get_zone(cf, &value[1]);
    if (shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    ctx = shm_zone->data;

    if (ctx->user_defined != NULL) {
        return "is duplicate";
    }

    if (cf->args->nelts > NGX_HTTP_REQSTAT_USER + 2) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "too many user defined variables");
        return NGX_CONF_ERROR;
    }

    ctx->user_defined = ngx_array_create(cf->pool, cf->args->nelts - 2,
                                         sizeof(ngx_int_t));
    if (ctx->user_defined == NULL) {
        return NGX_CONF_ERROR;
    }

    ctx->user_defined_str = ngx_array_create(cf->pool, cf->args->nelts - 2,
                                             sizeof(ngx_str_t));
    if (ctx->user_defined_str == NULL) {
        return NGX_CONF_ERROR;
    }

    /* used by req_status_show_field to resolve "$name" */
    rmcf->user_defined_str = ctx->user_defined_str;

    for (j = 2; j < cf->args->nelts; j++) {
        if (value[j].len > 1 && value[j].data[0] == '$') {
            value[j].data++;
            value[j].len--;
        }

        name = ngx_array_push(ctx->user_defined_str);
        if (name == NULL) {
            return NGX_CONF_ERROR;
        }

        *name = value[j];

        i = ngx_array_push(ctx->user_defined);
        if (i == NULL) {
            return NGX_CONF_ERROR;
        }

        *i = ngx_http_get_variable_index(cf, &value[j]);
        if (*i == NGX_ERROR) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "failed to obtain variable \"%V\"",
                               &value[j]);
            return NGX_CONF_ERROR;
        }
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_reqstat_zone_key_length(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_str_t                         *value;
    ngx_shm_zone_t                    *shm_zone;
    ngx_http_reqstat_ctx_t            *ctx;

    value = cf->args->elts;

    shm_zone = ngx_http_reqstat_get_zone(cf, &value[1]);
    if (shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    ctx = shm_zone->data;

    ctx->key_len = ngx_atoi(value[2].data, value[2].len);
    if (ctx->key_len == NGX_ERROR || ctx->key_len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid key length \"%V\"", &value[2]);
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_reqstat_zone_recycle(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_int_t                          rate, scale;
    ngx_str_t                         *value;
    ngx_shm_zone_t                    *shm_zone;
    ngx_http_reqstat_ctx_t            *ctx;

    value = cf->args->elts;

    shm_zone = ngx_http_reqstat_get_zone(cf, &value[1]);
    if (shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    ctx = shm_zone->data;

    rate = ngx_atoi(value[2].data, value[2].len);
    if (rate == NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid threshold \"%V\"", &value[2]);
        return NGX_CONF_ERROR;
    }

    scale = ngx_atoi(value[3].data, value[3].len);
    if (scale == NGX_ERROR || scale == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid scale \"%V\"", &value[3]);
        return NGX_CONF_ERROR;
    }

    ctx->recycle_rate = rate * 1000 / scale;

    return NGX_CONF_OK;
}


static char *
ngx_http_reqstat_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ssize_t                            size;
    ngx_str_t                         *value;
    ngx_shm_zone_t                    *shm_zone;
    ngx_http_reqstat_ctx_t            *ctx;
    ngx_http_compile_complex_value_t   ccv;

    value = cf->args->elts;

    size = ngx_parse_size(&value[3]);
    if (size == NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid zone size \"%V\"", &value[3]);
        return NGX_CONF_ERROR;
    }

    if (size < (ssize_t) (8 * ngx_pagesize)) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "zone \"%V\" is too small", &value[1]);
        return NGX_CONF_ERROR;
    }

    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_http_reqstat_ctx_t));
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    if (ngx_http_script_variables_count(&value[2]) == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "the value \"%V\" is a constant",
                           &value[2]);
        return NGX_CONF_ERROR;
    }

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

    ccv.cf = cf;
    ccv.value = &value[2];
    ccv.complex_value = &ctx->value;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    ctx->val = ngx_palloc(cf->pool, sizeof(ngx_str_t));
    if (ctx->val == NULL) {
        return NGX_CONF_ERROR;
    }
    *ctx->val = value[2];

    ctx->key_len = 152;          /* now an item is 640B at length. */
    ctx->recycle_rate = 167;     /* rate threshold is 10r/min */
    ctx->alloc_already_fail = 0;

    shm_zone = ngx_shared_memory_add(cf, &value[1], size,
                                     &ngx_http_reqstat_module);
    if (shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    if (shm_zone->data) {
        ctx = shm_zone->data;

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "%V \"%V\" is already bound to value \"%V\"",
                           &cmd->name, &value[1], ctx->val);
        return NGX_CONF_ERROR;
    }

    shm_zone->init = ngx_http_reqstat_init_zone;
    shm_zone->data = ctx;

    return NGX_CONF_OK;
}


static char *
ngx_http_reqstat(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t                    *value;
    ngx_uint_t                    i, j;
    ngx_shm_zone_t               *shm_zone, **z;
    ngx_http_reqstat_conf_t      *smcf;

    ngx_http_reqstat_conf_t      *rlcf = conf;

    value = cf->args->elts;
    smcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_reqstat_module);

    if (rlcf->monitor != NGX_CONF_UNSET_PTR) {
        return "is duplicate";
    }

    if (smcf->monitor == NULL) {
        smcf->monitor = ngx_array_create(cf->pool, cf->args->nelts - 1,
                                         sizeof(ngx_shm_zone_t *));
        if (smcf->monitor == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    rlcf->monitor = ngx_array_create(cf->pool, cf->args->nelts - 1,
                                     sizeof(ngx_shm_zone_t *));
    if (rlcf->monitor == NULL) {
        return NGX_CONF_ERROR;
    }

    for (i = 1; i < cf->args->nelts; i++) {
        shm_zone = ngx_shared_memory_add(cf, &value[i], 0,
                                         &ngx_http_reqstat_module);
        if (shm_zone == NULL) {
            return NGX_CONF_ERROR;
        }

        z = ngx_array_push(rlcf->monitor);
        if (z == NULL) {
            return NGX_CONF_ERROR;
        }

        *z = shm_zone;

        z = smcf->monitor->elts;
        for (j = 0; j < smcf->monitor->nelts; j++) {
            if (z[j] == shm_zone) {
                break;
            }
        }

        if (j == smcf->monitor->nelts) {
            z = ngx_array_push(smcf->monitor);
            if (z == NULL) {
                return NGX_CONF_ERROR;
            }
            *z = shm_zone;
        }
    }

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_reqstat_variable_enable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    u_char                       *p;
    ngx_http_reqstat_store_t     *store;

    p = ngx_pnalloc(r->pool, 1 + sizeof(uintptr_t));
    if (p == NULL) {
        return NGX_ERROR;
    }

    v->len = 1;
    v->valid = 1;
    v->not_found = 0;
    v->no_cacheable = 0;
    v->data = p;

    /*
     * The store pointer is kept in the variable value: unlike module
     * contexts, variables survive internal redirects.
     */

    store = ngx_http_get_module_ctx(r, ngx_http_reqstat_module);
    if (store == NULL || store->bypass) {
        *p = '0';

    } else {
        *p++ = '1';
        ngx_memcpy(p, &store, sizeof(uintptr_t));
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_reqstat_init_handler(ngx_http_request_t *r)
{
    ngx_http_reqstat_conf_t      *rmcf, *rlcf;
    ngx_http_reqstat_store_t     *store;

    store = ngx_http_get_module_ctx(r, ngx_http_reqstat_module);
    rmcf = ngx_http_get_module_main_conf(r, ngx_http_reqstat_module);
    rlcf = ngx_http_get_module_loc_conf(r, ngx_http_reqstat_module);

    if (store == NULL) {
        if (r->variables[rmcf->index].valid) {
            return NGX_DECLINED;
        }

        store = ngx_http_reqstat_create_store(r, rlcf);
        if (store == NULL) {
            return NGX_ERROR;
        }

        ngx_http_set_ctx(r, store, ngx_http_reqstat_module);
    }

    if (ngx_http_get_indexed_variable(r, rmcf->index) == NULL) {
        return NGX_ERROR;
    }

    return NGX_DECLINED;
}


static ngx_uint_t
ngx_http_reqstat_first_request(ngx_http_request_t *r)
{
    /*
     * HTTP/2 and HTTP/3 requests run on per-stream fake connections,
     * so count a connection on its first client-initiated stream only.
     */

#if (NGX_HTTP_V3)
    if (r->http_version == NGX_HTTP_VERSION_30) {
        return r->connection->quic != NULL && r->connection->quic->id == 0;
    }
#endif

#if (NGX_HTTP_V2)
    if (r->stream) {
        return r->stream->node != NULL && r->stream->node->id == 1;
    }
#endif

    return r->connection->requests == 1;
}


static ngx_int_t
ngx_http_reqstat_log_handler(ngx_http_request_t *r)
{
    u_char                       *p;
    off_t                         sent;
    ngx_str_t                     val, *value;
    ngx_int_t                    *indicator, iv;
    ngx_uint_t                    i, j, k, status, utries, first, noups;
    ngx_time_t                   *tp;
    ngx_msec_int_t                ms, total_ms;
    ngx_shm_zone_t              **shm_zone, *z;
    ngx_http_reqstat_ctx_t       *ctx;
    ngx_http_reqstat_conf_t      *rcf;
    ngx_http_reqstat_store_t     *store;
    ngx_http_reqstat_rbnode_t    *fnode, **fnode_store;
    ngx_http_upstream_state_t    *state;
    ngx_http_variable_value_t    *v;

    switch (ngx_http_reqstat_check_enable(r, &rcf, &store)) {
        case NGX_ERROR:
            return NGX_ERROR;

        case NGX_DECLINED:
        case NGX_AGAIN:
            return NGX_OK;

        default:
            break;
    }

    first = ngx_http_reqstat_first_request(r);

    /*
     * The request was proxied, but the last upstream attempt gave no
     * status ("-" in $upstream_status): the response came from nginx,
     * e.g. the client closed the connection while waiting (499).
     */

    noups = 0;

    if (r->upstream_states != NULL && r->upstream_states->nelts > 0) {
        state = r->upstream_states->elts;

        if (state[r->upstream_states->nelts - 1].status == 0) {
            noups = NGX_HTTP_REQSTAT_CODE_NOUPS;
        }
    }

    shm_zone = rcf->monitor->elts;
    fnode_store = store->monitor_index.elts;
    value = store->value_index.elts;
    for (i = 0; i < store->monitor_index.nelts; i++) {
        if (fnode_store[i] == NULL) {
            continue;
        }

        z = shm_zone[i];
        ctx = z->data;

        fnode = fnode_store[i];

        /* bytes not yet accounted by the body filter */
        sent = r->connection->sent - store->sent;

        if (rcf->lazy) {
            if (ngx_http_complex_value(r, &ctx->value, &val) != NGX_OK) {
                ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                              "failed to reap the key \"%V\"", ctx->val);
                continue;
            }

            if (value[i].len != val.len
                || ngx_strncmp(value[i].data, val.data, val.len) != 0)
            {
                fnode = ngx_http_reqstat_rbtree_lookup(z, &val);

                if (fnode == NULL) {
                    ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                                  "failed to alloc node in zone \"%V\", "
                                  "enlarge it please",
                                  &z->shm.name);

                    fnode = fnode_store[i];

                } else {
                    sent = r->connection->sent;
                }
            }
        }

        if (first) {
            ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_CONN_TOTAL, 1);
        }

        ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_REQ_TOTAL, 1);
        ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_BYTES_IN,
                               r->request_length);
        ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_BYTES_OUT, sent);

        if (r->err_status) {
            status = r->err_status;

        } else if (r->headers_out.status) {
            status = r->headers_out.status;

        } else if (r->http_version == NGX_HTTP_VERSION_9) {
            status = 9;

        } else {
            status = 0;
        }

        ngx_http_reqstat_count_code(fnode, (status + 1) | noups);

        if (status >= 200 && status < 300) {
            ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_2XX, 1);

            switch (status) {
            case 200:
                ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_200, 1);
                break;

            case 206:
                ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_206, 1);
                break;

            default:
                ngx_http_reqstat_count(fnode,
                                       NGX_HTTP_REQSTAT_OTHER_DETAIL_STATUS, 1);
                break;
            }

        } else if (status >= 300 && status < 400) {
            ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_3XX, 1);

            switch (status) {
            case 302:
                ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_302, 1);
                break;

            case 304:
                ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_304, 1);
                break;

            default:
                ngx_http_reqstat_count(fnode,
                                       NGX_HTTP_REQSTAT_OTHER_DETAIL_STATUS, 1);
                break;
            }

        } else if (status >= 400 && status < 500) {
            ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_4XX, 1);

            switch (status) {
            case 403:
                ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_403, 1);
                break;

            case 404:
                ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_404, 1);
                break;

            case 416:
                ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_416, 1);
                break;

            case 499:
                ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_499, 1);
                break;

            default:
                ngx_http_reqstat_count(fnode,
                                       NGX_HTTP_REQSTAT_OTHER_DETAIL_STATUS, 1);
                break;
            }

        } else if (status >= 500 && status < 600) {
            ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_5XX, 1);

            switch (status) {
            case 500:
                ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_500, 1);
                break;

            case 502:
                ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_502, 1);
                break;

            case 503:
                ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_503, 1);
                break;

            case 504:
                ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_504, 1);
                break;

            case 508:
                ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_508, 1);
                break;

            default:
                ngx_http_reqstat_count(fnode,
                                       NGX_HTTP_REQSTAT_OTHER_DETAIL_STATUS, 1);
                break;
            }

        } else {
            ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_OTHER_STATUS, 1);

            ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_OTHER_DETAIL_STATUS,
                                   1);
        }

        tp = ngx_timeofday();

        ms = (ngx_msec_int_t)
             ((tp->sec - r->start_sec) * 1000 + (tp->msec - r->start_msec));
        ms = ngx_max(ms, 0);
        ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_RT, ms);

        if (r->upstream_states != NULL && r->upstream_states->nelts > 0) {
            state = r->upstream_states->elts;

            /* response status of last upstream peer */

            status = state[r->upstream_states->nelts - 1].status;

            if (status >= 400 && status < 500) {
                ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_UPS_4XX, 1);

            } else if (status >= 500 && status < 600) {
                ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_UPS_5XX, 1);
            }

            ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_UPS_REQ, 1);

            j = 0;
            total_ms = 0;
            utries = 0;

            for ( ;; ) {

                utries++;

#if (nginx_version >= 1009002)
                ms = (ngx_msec_int_t) state[j].response_time;
#else
                ms = (ngx_msec_int_t) (state[j].response_sec * 1000
                                               + state[j].response_msec);
#endif
                ms = ngx_max(ms, 0);
                total_ms += ms;

                if (++j == r->upstream_states->nelts) {
                    break;
                }

                /* skip the separator between upstream groups */

                if (state[j].peer == NULL) {
                    if (++j == r->upstream_states->nelts) {
                        break;
                    }
                }
            }

            ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_UPS_RT,
                                   total_ms);
            ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_UPS_TRIES,
                                   utries);
        }

        if (ctx->user_defined) {
            indicator = ctx->user_defined->elts;
            for (j = 0; j < ctx->user_defined->nelts; j++) {
                v = ngx_http_get_indexed_variable(r, indicator[j]);
                if (v == NULL || v->not_found || !v->valid || v->len == 0) {
                    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                                   "reqstat indicator is uninitialized");
                    continue;
                }

                /* take the trailing number, e.g. "0.012, 0.345" -> 345 */

                for (k = 0, p = v->data + v->len; p > v->data; p--) {
                    if (p[-1] == '.') {
                        k = v->data + v->len - p;
                        continue;
                    }

                    if (p[-1] < '0' || p[-1] > '9') {
                        break;
                    }
                }

                if (k) {
                    iv = ngx_atofp(p, v->data + v->len - p, k);

                } else {
                    iv = ngx_atoi(p, v->data + v->len - p);
                }

                if (iv == NGX_ERROR) {
                    continue;
                }

                ngx_http_reqstat_count(fnode, NGX_HTTP_REQSTAT_EXTRA(j), iv);
            }
        }
    }

    store->sent = r->connection->sent;

    return NGX_OK;
}


static ngx_int_t
ngx_http_reqstat_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    off_t                         diff;
    ngx_int_t                     rc;
    ngx_uint_t                    i;
    ngx_http_request_t           *mr;
    ngx_http_reqstat_conf_t      *rcf;
    ngx_http_reqstat_store_t     *store;
    ngx_http_reqstat_rbnode_t   **fnode_store;

    rc = ngx_http_next_body_filter(r, in);

    /* account bytes as they are written, so long responses show progress */

    mr = r->main;

    if (ngx_http_reqstat_check_enable(mr, &rcf, &store) != NGX_OK) {
        return rc;
    }

    diff = mr->connection->sent - store->sent;
    if (diff <= 0) {
        return rc;
    }

    store->sent = mr->connection->sent;

    fnode_store = store->monitor_index.elts;
    for (i = 0; i < store->monitor_index.nelts; i++) {
        if (fnode_store[i] == NULL) {
            continue;
        }

        ngx_http_reqstat_count(fnode_store[i], NGX_HTTP_REQSTAT_BYTES_OUT,
                               diff);
    }

    return rc;
}


static ngx_int_t
ngx_http_reqstat_show_handler(ngx_http_request_t *r)
{
    ngx_int_t                     rc;
    ngx_str_t                     key, *only;
    ngx_uint_t                    i, found, status;
    ngx_array_t                  *display, zones;
    ngx_chain_t                  *cl;
    ngx_http_reqstat_conf_t      *rlcf;
    ngx_http_reqstat_conf_t      *smcf;
    ngx_http_reqstat_writer_t     w;
    ngx_http_reqstat_zone_snapshot_t  *zs;

    if (!(r->method & (NGX_HTTP_GET|NGX_HTTP_HEAD))) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return rc;
    }

    rlcf = ngx_http_get_module_loc_conf(r, ngx_http_reqstat_module);
    smcf = ngx_http_get_module_main_conf(r, ngx_http_reqstat_module);

    display = rlcf->display == NULL ? smcf->monitor : rlcf->display;
    if (display == NULL) {
        r->headers_out.status = NGX_HTTP_NO_CONTENT;
        r->header_only = 1;
        return ngx_http_send_header(r);
    }

    only = NULL;

    if (rlcf->show_key) {
        if (ngx_http_complex_value(r, rlcf->show_key, &key) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        only = &key;
    }

    if (ngx_http_reqstat_snapshot(r, display, only, &zones) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    status = NGX_HTTP_OK;

    if (only) {
        found = 0;
        zs = zones.elts;

        for (i = 0; i < zones.nelts; i++) {
            found += zs[i].nodes.nelts;
        }

        if (!found) {
            if (rlcf->format != NGX_HTTP_REQSTAT_FORMAT_JSON) {
                return NGX_HTTP_NOT_FOUND;
            }

            status = NGX_HTTP_NOT_FOUND;
        }
    }

    ngx_memzero(&w, sizeof(ngx_http_reqstat_writer_t));
    w.pool = r->pool;
    w.last = &w.out;

    if (rlcf->format == NGX_HTTP_REQSTAT_FORMAT_PROMETHEUS) {
        ngx_str_set(&r->headers_out.content_type,
                    "text/plain; version=0.0.4; charset=utf-8");
        rc = ngx_http_reqstat_write_prometheus(&w, rlcf, &zones);

    } else if (rlcf->format == NGX_HTTP_REQSTAT_FORMAT_JSON) {
        ngx_str_set(&r->headers_out.content_type, "application/json");

        if (status == NGX_HTTP_NOT_FOUND) {
            rc = ngx_http_reqstat_write_json_not_found(&w, only);

        } else {
            rc = ngx_http_reqstat_write_json(&w, &zones, only != NULL);
        }

    } else {
        ngx_str_set(&r->headers_out.content_type, "text/plain");
        rc = ngx_http_reqstat_write_text(&w, rlcf, &zones);
    }

    if (rc != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    r->headers_out.content_type_len = r->headers_out.content_type.len;
    r->headers_out.content_type_lowcase = NULL;
    r->headers_out.status = status;
    r->headers_out.content_length_n = w.size;

    if (w.size == 0) {
        r->header_only = 1;
    }

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    for (cl = w.out; cl->next; cl = cl->next) { /* void */ }

    cl->buf->last_buf = (r == r->main) ? 1 : 0;
    cl->buf->last_in_chain = 1;

    return ngx_http_output_filter(r, w.out);
}


static ngx_int_t
ngx_http_reqstat_snapshot(ngx_http_request_t *r, ngx_array_t *display,
    ngx_str_t *key, ngx_array_t *zones)
{
    size_t                             len;
    ngx_uint_t                         i;
    ngx_queue_t                       *q;
    ngx_shm_zone_t                   **shm_zone;
    ngx_http_reqstat_ctx_t            *ctx;
    ngx_http_reqstat_rbnode_t         *node;
    ngx_http_reqstat_snapshot_t       *sn;
    ngx_http_reqstat_zone_snapshot_t  *zs;

    if (ngx_array_init(zones, r->pool, display->nelts,
                       sizeof(ngx_http_reqstat_zone_snapshot_t))
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    shm_zone = display->elts;

    for (i = 0; i < display->nelts; i++) {
        ctx = shm_zone[i]->data;

        zs = ngx_array_push(zones);
        if (zs == NULL) {
            return NGX_ERROR;
        }

        zs->zone = shm_zone[i];
        zs->ctx = ctx;

        if (ngx_array_init(&zs->nodes, r->pool, 16,
                           sizeof(ngx_http_reqstat_snapshot_t))
            != NGX_OK)
        {
            return NGX_ERROR;
        }

        /*
         * Copy keys under the lock: a node may be recycled and get
         * a new key at any time.  Counters are read later without
         * locking, as they are updated atomically.
         */

        ngx_shmtx_lock(&ctx->shpool->mutex);

        for (q = ngx_queue_head(&ctx->sh->queue);
             q != ngx_queue_sentinel(&ctx->sh->queue);
             q = ngx_queue_next(q))
        {
            node = ngx_queue_data(q, ngx_http_reqstat_rbnode_t, queue);

            if (node->req_total == 0) {
                continue;
            }

            if (key) {
                /* keys are stored truncated to the zone key length */
                len = ngx_min(key->len, (size_t) ctx->key_len);

                if (node->len != len
                    || ngx_memcmp(node->data, key->data, len) != 0)
                {
                    continue;
                }
            }

            sn = ngx_array_push(&zs->nodes);
            if (sn == NULL) {
                ngx_shmtx_unlock(&ctx->shpool->mutex);
                return NGX_ERROR;
            }

            sn->node = node;
            sn->key.len = node->len;
            sn->key.data = ngx_pnalloc(r->pool, node->len);
            if (sn->key.data == NULL) {
                ngx_shmtx_unlock(&ctx->shpool->mutex);
                return NGX_ERROR;
            }

            ngx_memcpy(sn->key.data, node->data, node->len);
        }

        ngx_shmtx_unlock(&ctx->shpool->mutex);
    }

    return NGX_OK;
}


static u_char *
ngx_http_reqstat_reserve(ngx_http_reqstat_writer_t *w, size_t size)
{
    ngx_buf_t                    *b;
    ngx_chain_t                  *cl;

    b = w->buf;

    if (b != NULL && (size_t) (b->end - b->last) >= size) {
        return b->last;
    }

    b = ngx_create_temp_buf(w->pool,
                            ngx_max(size, NGX_HTTP_REQSTAT_CHUNK_SIZE));
    if (b == NULL) {
        return NULL;
    }

    cl = ngx_alloc_chain_link(w->pool);
    if (cl == NULL) {
        return NULL;
    }

    cl->buf = b;
    cl->next = NULL;

    *w->last = cl;
    w->last = &cl->next;
    w->buf = b;

    return b->last;
}


#define ngx_http_reqstat_commit(w, p)                                   \
    (w)->size += (p) - (w)->buf->last;                                  \
    (w)->buf->last = (p)


static ngx_int_t
ngx_http_reqstat_write_text(ngx_http_reqstat_writer_t *w,
    ngx_http_reqstat_conf_t *rlcf, ngx_array_t *zones)
{
    u_char                            *p;
    size_t                             len;
    ngx_int_t                         *user, index;
    ngx_uint_t                         i, j, k, nuser;
    ngx_http_reqstat_rbnode_t         *node;
    ngx_http_reqstat_snapshot_t       *sn;
    ngx_http_reqstat_zone_snapshot_t  *zs;

    zs = zones->elts;

    for (i = 0; i < zones->nelts; i++) {

        nuser = zs[i].ctx->user_defined ? zs[i].ctx->user_defined->nelts : 0;
        sn = zs[i].nodes.elts;

        for (k = 0; k < zs[i].nodes.nelts; k++) {
            node = sn[k].node;

            len = sn[k].key.len + 1
                  + NGX_HTTP_REQSTAT_MAX * (NGX_ATOMIC_T_LEN + 1)
                  + NGX_HTTP_REQSTAT_CODES_TEXT_LEN;

            p = ngx_http_reqstat_reserve(w, len);
            if (p == NULL) {
                return NGX_ERROR;
            }

            p = ngx_cpymem(p, sn[k].key.data, sn[k].key.len);
            *p++ = ',';

            if (rlcf->user_select) {
                user = rlcf->user_select->elts;
                for (j = 0; j < rlcf->user_select->nelts; j++) {
                    if (user[j] == NGX_HTTP_REQSTAT_FIELD_CODES) {
                        p = ngx_http_reqstat_codes_text(p, node);
                        *p++ = ',';

                    } else if (user[j] < NGX_HTTP_REQSTAT_RSRV) {
                        index = user[j];
                        p = ngx_sprintf(p, "%uA,",
                                        *NGX_HTTP_REQSTAT_REQ_FIELD(node,
                                              ngx_http_reqstat_fields[index]));

                    } else {
                        index = user[j] - NGX_HTTP_REQSTAT_RSRV;
                        p = ngx_sprintf(p, "%uA,",
                                        *NGX_HTTP_REQSTAT_REQ_FIELD(node,
                                               NGX_HTTP_REQSTAT_EXTRA(index)));
                    }
                }

            } else {

                for (j = 0; j < NGX_HTTP_REQSTAT_RSRV; j++) {
                    p = ngx_sprintf(p, "%uA,",
                                    *NGX_HTTP_REQSTAT_REQ_FIELD(node,
                                                  ngx_http_reqstat_fields[j]));
                }

                for (j = 0; j < nuser; j++) {
                    p = ngx_sprintf(p, "%uA,",
                                    *NGX_HTTP_REQSTAT_REQ_FIELD(node,
                                                   NGX_HTTP_REQSTAT_EXTRA(j)));
                }
            }

            *(p - 1) = '\n';

            ngx_http_reqstat_commit(w, p);
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_reqstat_write_prometheus(ngx_http_reqstat_writer_t *w,
    ngx_http_reqstat_conf_t *rlcf, ngx_array_t *zones)
{
    u_char                            *p;
    size_t                             len;
    ngx_int_t                         *user;
    ngx_str_t                          label, *names;
    ngx_uint_t                         f, i, j, nuser;
    ngx_uint_t                         rsrv[NGX_HTTP_REQSTAT_RSRV];
    ngx_uint_t                         extra[NGX_HTTP_REQSTAT_USER];
    ngx_uint_t                         family[NGX_HTTP_REQSTAT_PROM_NFAMILIES];
    ngx_http_reqstat_prom_field_t     *field;
    ngx_http_reqstat_prom_family_t    *fam;
    ngx_http_reqstat_zone_snapshot_t  *zs;

    ngx_memzero(rsrv, sizeof(rsrv));
    ngx_memzero(extra, sizeof(extra));
    ngx_memzero(family, sizeof(family));

    if (rlcf->user_select) {
        user = rlcf->user_select->elts;
        for (j = 0; j < rlcf->user_select->nelts; j++) {
            if (user[j] == NGX_HTTP_REQSTAT_FIELD_CODES) {
                family[NGX_HTTP_REQSTAT_PROM_STATUS_CODES] = 1;

            } else if (user[j] < NGX_HTTP_REQSTAT_RSRV) {
                rsrv[user[j]] = 1;

            } else {
                extra[user[j] - NGX_HTTP_REQSTAT_RSRV] = 1;
            }
        }

    } else {
        family[NGX_HTTP_REQSTAT_PROM_STATUS_CODES] = 1;

        for (j = 0; j < NGX_HTTP_REQSTAT_RSRV; j++) {
            rsrv[j] = 1;
        }

        for (j = 0; j < NGX_HTTP_REQSTAT_USER; j++) {
            extra[j] = 1;
        }
    }

    for (j = 0; j < NGX_HTTP_REQSTAT_RSRV; j++) {
        if (rsrv[j]) {
            family[ngx_http_reqstat_prom_fields[j].family] = 1;
        }
    }

    zs = zones->elts;

    for (i = 0; i < zones->nelts; i++) {
        nuser = zs[i].ctx->user_defined ? zs[i].ctx->user_defined->nelts : 0;
        for (j = 0; j < nuser; j++) {
            if (extra[j]) {
                family[NGX_HTTP_REQSTAT_PROM_INDICATOR] = 1;
            }
        }
    }

    for (f = 0; f < NGX_HTTP_REQSTAT_PROM_NFAMILIES; f++) {
        if (!family[f]) {
            continue;
        }

        fam = &ngx_http_reqstat_prom_families[f];

        len = sizeof("# HELP  \n# TYPE  \n") - 1
              + 2 * fam->name.len + fam->help.len + fam->type.len;

        p = ngx_http_reqstat_reserve(w, len);
        if (p == NULL) {
            return NGX_ERROR;
        }

        p = ngx_sprintf(p, "# HELP %V %V\n# TYPE %V %V\n",
                        &fam->name, &fam->help, &fam->name, &fam->type);

        ngx_http_reqstat_commit(w, p);

        if (f == NGX_HTTP_REQSTAT_PROM_STATUS_CODES) {
            if (ngx_http_reqstat_write_prom_codes(w, zs, zones->nelts, fam)
                != NGX_OK)
            {
                return NGX_ERROR;
            }

            continue;
        }

        if (f != NGX_HTTP_REQSTAT_PROM_INDICATOR) {
            for (j = 0; j < NGX_HTTP_REQSTAT_RSRV; j++) {
                field = &ngx_http_reqstat_prom_fields[j];

                if (!rsrv[j] || field->family != f) {
                    continue;
                }

                if (ngx_http_reqstat_write_prom_samples(w, zs, zones->nelts,
                                            fam,
                                            ngx_http_reqstat_fields[j],
                                            &field->label, field->msec)
                    != NGX_OK)
                {
                    return NGX_ERROR;
                }
            }

            continue;
        }

        /* user defined indicators, named per zone */

        for (i = 0; i < zones->nelts; i++) {
            if (zs[i].ctx->user_defined == NULL) {
                continue;
            }

            names = zs[i].ctx->user_defined_str->elts;

            for (j = 0; j < zs[i].ctx->user_defined->nelts; j++) {
                if (!extra[j]) {
                    continue;
                }

                label.data = ngx_pnalloc(w->pool,
                                         sizeof("name=\"\"") - 1
                                         + 2 * names[j].len);
                if (label.data == NULL) {
                    return NGX_ERROR;
                }

                p = ngx_cpymem(label.data, "name=\"", sizeof("name=\"") - 1);
                p = ngx_http_reqstat_prom_escape(p, names[j].data,
                                                 names[j].len);
                *p++ = '"';

                label.len = p - label.data;

                if (ngx_http_reqstat_write_prom_samples(w, &zs[i], 1, fam,
                                            NGX_HTTP_REQSTAT_EXTRA(j),
                                            &label, 0)
                    != NGX_OK)
                {
                    return NGX_ERROR;
                }
            }
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_reqstat_write_prom_samples(ngx_http_reqstat_writer_t *w,
    ngx_http_reqstat_zone_snapshot_t *zs, ngx_uint_t n,
    ngx_http_reqstat_prom_family_t *family, ngx_uint_t field,
    ngx_str_t *label, ngx_uint_t msec)
{
    u_char                            *p;
    size_t                             len;
    ngx_str_t                         *name;
    ngx_uint_t                         i, k;
    ngx_atomic_uint_t                  val;
    ngx_array_t                       *nodes;
    ngx_http_reqstat_snapshot_t       *sn;

    for (i = 0; i < n; i++) {
        name = &zs[i].zone->shm.name;
        nodes = &zs[i].nodes;
        sn = nodes->elts;

        for (k = 0; k < nodes->nelts; k++) {

            len = family->name.len + sizeof("{zone=\"\",key=\"\",} .000\n") - 1
                  + 2 * name->len + 2 * sn[k].key.len + label->len
                  + NGX_ATOMIC_T_LEN;

            p = ngx_http_reqstat_reserve(w, len);
            if (p == NULL) {
                return NGX_ERROR;
            }

            p = ngx_cpymem(p, family->name.data, family->name.len);
            p = ngx_cpymem(p, "{zone=\"", sizeof("{zone=\"") - 1);
            p = ngx_http_reqstat_prom_escape(p, name->data, name->len);
            p = ngx_cpymem(p, "\",key=\"", sizeof("\",key=\"") - 1);
            p = ngx_http_reqstat_prom_escape(p, sn[k].key.data,
                                             sn[k].key.len);
            *p++ = '"';

            if (label->len) {
                *p++ = ',';
                p = ngx_cpymem(p, label->data, label->len);
            }

            val = *NGX_HTTP_REQSTAT_REQ_FIELD(sn[k].node, field);

            if (msec) {
                p = ngx_sprintf(p, "} %uA.%03uA\n", val / 1000, val % 1000);

            } else {
                p = ngx_sprintf(p, "} %uA\n", val);
            }

            ngx_http_reqstat_commit(w, p);
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_reqstat_write_prom_codes(ngx_http_reqstat_writer_t *w,
    ngx_http_reqstat_zone_snapshot_t *zs, ngx_uint_t n,
    ngx_http_reqstat_prom_family_t *family)
{
    u_char                            *p;
    size_t                             len;
    ngx_str_t                         *name;
    ngx_uint_t                         i, j, k;
    ngx_atomic_uint_t                  code, count;
    ngx_http_reqstat_rbnode_t         *node;
    ngx_http_reqstat_snapshot_t       *sn;

    for (i = 0; i < n; i++) {
        name = &zs[i].zone->shm.name;
        sn = zs[i].nodes.elts;

        for (k = 0; k < zs[i].nodes.nelts; k++) {
            node = sn[k].node;

            len = (family->name.len
                   + sizeof("{zone=\"\",key=\"\",code=\"\"} \n") - 1
                   + 2 * name->len + 2 * sn[k].key.len
                   + NGX_HTTP_REQSTAT_CODE_LEN + NGX_ATOMIC_T_LEN)
                  * (NGX_HTTP_REQSTAT_CODE_SLOTS + 1);

            p = ngx_http_reqstat_reserve(w, len);
            if (p == NULL) {
                return NGX_ERROR;
            }

            for (j = 0; j <= NGX_HTTP_REQSTAT_CODE_SLOTS; j++) {

                /* the extra iteration reports codes that found no slot */

                if (j < NGX_HTTP_REQSTAT_CODE_SLOTS) {
                    code = node->code[j];
                    count = node->code_count[j];

                } else {
                    code = 0;
                    count = node->code_overflow;
                }

                if (count == 0) {
                    continue;
                }

                p = ngx_cpymem(p, family->name.data, family->name.len);
                p = ngx_cpymem(p, "{zone=\"", sizeof("{zone=\"") - 1);
                p = ngx_http_reqstat_prom_escape(p, name->data, name->len);
                p = ngx_cpymem(p, "\",key=\"", sizeof("\",key=\"") - 1);
                p = ngx_http_reqstat_prom_escape(p, sn[k].key.data,
                                                 sn[k].key.len);
                p = ngx_cpymem(p, "\",code=\"", sizeof("\",code=\"") - 1);

                if (code) {
                    p = ngx_http_reqstat_code_name(p, code);

                } else {
                    p = ngx_cpymem(p, "other", sizeof("other") - 1);
                }

                p = ngx_sprintf(p, "\"} %uA\n", count);
            }

            ngx_http_reqstat_commit(w, p);
        }
    }

    return NGX_OK;
}


/*
 * Same shape as the Lua based collector this replaces:
 * {"<key>": {"200": 12, "499_upstream": 1}, ...}, or just the inner
 * object when a single key is shown.
 */

static ngx_int_t
ngx_http_reqstat_write_json(ngx_http_reqstat_writer_t *w,
    ngx_array_t *zones, ngx_uint_t single)
{
    u_char                            *p;
    size_t                             len;
    ngx_uint_t                         i, k, first;
    ngx_http_reqstat_snapshot_t       *sn;
    ngx_http_reqstat_zone_snapshot_t  *zs;

    zs = zones->elts;

    if (single) {
        for (i = 0; i < zones->nelts; i++) {
            if (zs[i].nodes.nelts == 0) {
                continue;
            }

            sn = zs[i].nodes.elts;

            p = ngx_http_reqstat_reserve(w,
                                         NGX_HTTP_REQSTAT_CODES_JSON_LEN + 1);
            if (p == NULL) {
                return NGX_ERROR;
            }

            p = ngx_http_reqstat_codes_json(p, sn[0].node);
            *p++ = '\n';

            ngx_http_reqstat_commit(w, p);

            return NGX_OK;
        }

        return NGX_OK;
    }

    p = ngx_http_reqstat_reserve(w, 1);
    if (p == NULL) {
        return NGX_ERROR;
    }

    *p++ = '{';
    ngx_http_reqstat_commit(w, p);

    first = 1;

    for (i = 0; i < zones->nelts; i++) {
        sn = zs[i].nodes.elts;

        for (k = 0; k < zs[i].nodes.nelts; k++) {

            len = sizeof(",\"\":") - 1
                  + sn[k].key.len
                  + ngx_escape_json(NULL, sn[k].key.data, sn[k].key.len)
                  + NGX_HTTP_REQSTAT_CODES_JSON_LEN;

            p = ngx_http_reqstat_reserve(w, len);
            if (p == NULL) {
                return NGX_ERROR;
            }

            if (!first) {
                *p++ = ',';
            }

            first = 0;

            *p++ = '"';
            p = (u_char *) ngx_escape_json(p, sn[k].key.data, sn[k].key.len);
            *p++ = '"';
            *p++ = ':';

            p = ngx_http_reqstat_codes_json(p, sn[k].node);

            ngx_http_reqstat_commit(w, p);
        }
    }

    p = ngx_http_reqstat_reserve(w, 2);
    if (p == NULL) {
        return NGX_ERROR;
    }

    *p++ = '}';
    *p++ = '\n';
    ngx_http_reqstat_commit(w, p);

    return NGX_OK;
}


static ngx_int_t
ngx_http_reqstat_write_json_not_found(ngx_http_reqstat_writer_t *w,
    ngx_str_t *key)
{
    u_char                            *p;
    size_t                             len;

    len = sizeof("{\"message\":\"Invalid key\",\"key\":\"\"}\n") - 1
          + key->len + ngx_escape_json(NULL, key->data, key->len);

    p = ngx_http_reqstat_reserve(w, len);
    if (p == NULL) {
        return NGX_ERROR;
    }

    p = ngx_cpymem(p, "{\"message\":\"Invalid key\",\"key\":\"",
                   sizeof("{\"message\":\"Invalid key\",\"key\":\"") - 1);
    p = (u_char *) ngx_escape_json(p, key->data, key->len);
    p = ngx_cpymem(p, "\"}\n", sizeof("\"}\n") - 1);

    ngx_http_reqstat_commit(w, p);

    return NGX_OK;
}


static u_char *
ngx_http_reqstat_codes_text(u_char *p, ngx_http_reqstat_rbnode_t *node)
{
    u_char                       *start;
    ngx_uint_t                    i;
    ngx_atomic_uint_t             code, count;

    start = p;

    for (i = 0; i < NGX_HTTP_REQSTAT_CODE_SLOTS; i++) {
        code = node->code[i];
        count = node->code_count[i];

        if (code == 0) {
            break;
        }

        if (count == 0) {
            continue;
        }

        if (p != start) {
            *p++ = ';';
        }

        p = ngx_http_reqstat_code_name(p, code);
        p = ngx_sprintf(p, ":%uA", count);
    }

    count = node->code_overflow;

    if (count) {
        if (p != start) {
            *p++ = ';';
        }

        p = ngx_sprintf(p, "other:%uA", count);
    }

    return p;
}


static u_char *
ngx_http_reqstat_codes_json(u_char *p, ngx_http_reqstat_rbnode_t *node)
{
    u_char                       *start;
    ngx_uint_t                    i;
    ngx_atomic_uint_t             code, count;

    *p++ = '{';
    start = p;

    for (i = 0; i < NGX_HTTP_REQSTAT_CODE_SLOTS; i++) {
        code = node->code[i];
        count = node->code_count[i];

        if (code == 0) {
            break;
        }

        if (count == 0) {
            continue;
        }

        if (p != start) {
            *p++ = ',';
        }

        *p++ = '"';
        p = ngx_http_reqstat_code_name(p, code);
        p = ngx_sprintf(p, "\":%uA", count);
    }

    count = node->code_overflow;

    if (count) {
        if (p != start) {
            *p++ = ',';
        }

        p = ngx_sprintf(p, "\"other\":%uA", count);
    }

    *p++ = '}';

    return p;
}


static u_char *
ngx_http_reqstat_prom_escape(u_char *dst, u_char *src, size_t size)
{
    while (size) {
        switch (*src) {
        case '\\':
            *dst++ = '\\';
            *dst++ = '\\';
            break;

        case '"':
            *dst++ = '\\';
            *dst++ = '"';
            break;

        case '\n':
            *dst++ = '\\';
            *dst++ = 'n';
            break;

        default:
            *dst++ = *src;
            break;
        }

        src++;
        size--;
    }

    return dst;
}


static void
ngx_http_reqstat_count(void *data, off_t offset, ngx_int_t incr)
{
    ngx_http_reqstat_rbnode_t    *node = data;

    (void) ngx_atomic_fetch_add(NGX_HTTP_REQSTAT_REQ_FIELD(node, offset), incr);
}


/*
 * Count an exact status code in a fixed set of slots.  Slots are claimed
 * lock-free on first use; once all of them are taken, further distinct
 * codes are only counted in code_overflow.
 */

static void
ngx_http_reqstat_count_code(ngx_http_reqstat_rbnode_t *node,
    ngx_atomic_uint_t code)
{
    ngx_uint_t                    i;

    for (i = 0; i < NGX_HTTP_REQSTAT_CODE_SLOTS; i++) {

        if (node->code[i] == 0) {
            (void) ngx_atomic_cmp_set(&node->code[i], 0, code);
        }

        if (node->code[i] == code) {
            (void) ngx_atomic_fetch_add(&node->code_count[i], 1);
            return;
        }
    }

    (void) ngx_atomic_fetch_add(&node->code_overflow, 1);
}


static u_char *
ngx_http_reqstat_code_name(u_char *p, ngx_atomic_uint_t code)
{
    p = ngx_sprintf(p, "%03ui", (ngx_uint_t) (code & 0xffff) - 1);

    if (code & NGX_HTTP_REQSTAT_CODE_NOUPS) {
        p = ngx_cpymem(p, "_upstream", sizeof("_upstream") - 1);
    }

    return p;
}


ngx_http_reqstat_rbnode_t *
ngx_http_reqstat_rbtree_lookup(ngx_shm_zone_t *shm_zone, ngx_str_t *val)
{
    size_t                        size, len;
    uint32_t                      hash;
    ngx_int_t                     rc, excess;
    ngx_time_t                   *tp;
    ngx_msec_t                    now;
    ngx_queue_t                  *q;
    ngx_msec_int_t                ms;
    ngx_rbtree_node_t            *node, *sentinel;
    ngx_http_reqstat_ctx_t       *ctx;
    ngx_http_reqstat_rbnode_t    *rs;

    ctx = shm_zone->data;

    /* keys are stored truncated, so hash and compare the truncated key */

    len = ngx_min((size_t) ctx->key_len, val->len);

    hash = ngx_murmur_hash2(val->data, len);

    tp = ngx_timeofday();
    now = (ngx_msec_t) (tp->sec * 1000 + tp->msec);

    ngx_shmtx_lock(&ctx->shpool->mutex);

    node = ctx->sh->rbtree.root;
    sentinel = ctx->sh->rbtree.sentinel;

    while (node != sentinel) {

        if (hash < node->key) {
            node = node->left;
            continue;
        }

        if (hash > node->key) {
            node = node->right;
            continue;
        }

        /* hash == node->key */

        rs = (ngx_http_reqstat_rbnode_t *) &node->color;

        rc = ngx_memn2cmp(val->data, rs->data, len, (size_t) rs->len);

        if (rc == 0) {

            ms = (ngx_msec_int_t) (now - rs->last_visit);

            rs->excess = rs->excess - ngx_abs(ms) * ctx->recycle_rate / 1000
                       + 1000;
            rs->last_visit = now;

            if (rs->excess > 0) {
                ngx_queue_remove(&rs->visit);
                ngx_queue_insert_head(&ctx->sh->visit, &rs->visit);
            }

            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, shm_zone->shm.log, 0,
                           "reqstat lookup exist: %*s", (size_t) rs->len,
                           rs->data);

            ngx_shmtx_unlock(&ctx->shpool->mutex);

            return rs;
        }

        node = (rc < 0) ? node->left : node->right;
    }

    rc = 0;
    node = NULL;

    size = offsetof(ngx_rbtree_node_t, color)
         + offsetof(ngx_http_reqstat_rbnode_t, data)
         + ctx->key_len;

    if (ctx->alloc_already_fail == 0) {
        node = ngx_slab_alloc_locked(ctx->shpool, size);
        if (node == NULL) {
            ctx->alloc_already_fail = 1;
        }
    }

    if (node == NULL) {

        if (ngx_queue_empty(&ctx->sh->visit)) {
            ngx_shmtx_unlock(&ctx->shpool->mutex);
            return NULL;
        }

        /* try to free a vacant node */
        q = ngx_queue_last(&ctx->sh->visit);
        rs = ngx_queue_data(q, ngx_http_reqstat_rbnode_t, visit);

        ms = (ngx_msec_int_t) (now - rs->last_visit);

        excess = rs->excess - ngx_abs(ms) * ctx->recycle_rate / 1000;

        ngx_log_debug3(NGX_LOG_DEBUG_HTTP, shm_zone->shm.log, 0,
                       "reqstat lookup try recycle: %*s, %i",
                       (size_t) rs->len, rs->data, excess);

        if (excess < 0) {

            rc = 1;

            node = (ngx_rbtree_node_t *)
                            ((char *) rs - offsetof(ngx_rbtree_node_t, color));
            ngx_rbtree_delete(&ctx->sh->rbtree, node);
            ngx_queue_remove(&rs->visit);

            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, shm_zone->shm.log, 0,
                           "reqstat lookup recycle: %*s",
                           (size_t) rs->len, rs->data);

            ngx_memzero((void *) &rs->bytes_in,
                        size - offsetof(ngx_rbtree_node_t, color)
                             - offsetof(ngx_http_reqstat_rbnode_t, bytes_in));

        } else {
            ngx_shmtx_unlock(&ctx->shpool->mutex);
            return NULL;
        }
    }

    node->key = hash;

    rs = (ngx_http_reqstat_rbnode_t *) &node->color;

    ngx_memcpy(rs->data, val->data, len);
    rs->len = len;

    ngx_rbtree_insert(&ctx->sh->rbtree, node);
    ngx_queue_insert_head(&ctx->sh->visit, &rs->visit);
    if (!rc) {
        ngx_queue_insert_head(&ctx->sh->queue, &rs->queue);
    }

    rs->last_visit = now;
    rs->excess = 1000;

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, shm_zone->shm.log, 0,
                   "reqstat lookup build: %*s", (size_t) rs->len, rs->data);

    ngx_shmtx_unlock(&ctx->shpool->mutex);

    return rs;
}


static ngx_int_t
ngx_http_reqstat_init_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    size_t                        n;
    ngx_http_reqstat_ctx_t       *ctx, *octx;

    octx = data;
    ctx = shm_zone->data;

    if (octx != NULL) {
        if (ctx->val->len != octx->val->len
            || ngx_strncmp(ctx->val->data, octx->val->data, ctx->val->len)
               != 0)
        {
            ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
                          "reqstat \"%V\" uses the value str \"%V\" "
                          "while previously it used \"%V\"",
                          &shm_zone->shm.name, ctx->val, octx->val);
            return NGX_ERROR;
        }

        ctx->shpool = octx->shpool;
        ctx->sh = octx->sh;

        /* nodes already allocated have room for the old key length only */

        if (ctx->key_len != ctx->sh->key_len) {
            ngx_log_error(NGX_LOG_WARN, shm_zone->shm.log, 0,
                          "reqstat \"%V\" key length change to %i "
                          "is ignored until restart, using %i",
                          &shm_zone->shm.name, ctx->key_len,
                          ctx->sh->key_len);

            ctx->key_len = ctx->sh->key_len;
        }

        return NGX_OK;
    }

    ctx->shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    /* running out of memory is handled by recycling nodes */
    ctx->shpool->log_nomem = 0;

    if (shm_zone->shm.exists) {
        ctx->sh = ctx->shpool->data;
        ctx->key_len = ctx->sh->key_len;
        return NGX_OK;
    }

    ctx->sh = ngx_slab_alloc(ctx->shpool, sizeof(ngx_http_reqstat_shctx_t));
    if (ctx->sh == NULL) {
        return NGX_ERROR;
    }

    ctx->shpool->data = ctx->sh;
    ctx->sh->key_len = ctx->key_len;

    n = sizeof(" in req_status zone \"\"") + shm_zone->shm.name.len;
    ctx->shpool->log_ctx = ngx_slab_alloc(ctx->shpool, n);
    if (ctx->shpool->log_ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_sprintf(ctx->shpool->log_ctx,
                " in req_status zone \"%V\"%Z",
                &shm_zone->shm.name);

    ngx_rbtree_init(&ctx->sh->rbtree, &ctx->sh->sentinel,
                    ngx_http_reqstat_rbtree_insert_value);

    ngx_queue_init(&ctx->sh->queue);
    ngx_queue_init(&ctx->sh->visit);

    return NGX_OK;
}


static void
ngx_http_reqstat_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    ngx_rbtree_node_t          **p;
    ngx_http_reqstat_rbnode_t   *rsn, *rsnt;

    for ( ;; ) {

        if (node->key < temp->key) {

            p = &temp->left;

        } else if (node->key > temp->key) {

            p = &temp->right;

        } else { /* node->key == temp->key */

            rsn = (ngx_http_reqstat_rbnode_t *) &node->color;
            rsnt = (ngx_http_reqstat_rbnode_t *) &temp->color;

            p = (ngx_memn2cmp(rsn->data, rsnt->data, rsn->len, rsnt->len) < 0)
                ? &temp->left : &temp->right;
        }

        if (*p == sentinel) {
            break;
        }

        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}


static ngx_http_reqstat_store_t *
ngx_http_reqstat_create_store(ngx_http_request_t *r,
    ngx_http_reqstat_conf_t *rlcf)
{
    ngx_str_t                     val, *value;
    ngx_uint_t                    i;
    ngx_shm_zone_t              **shm_zone, *z;
    ngx_http_reqstat_ctx_t       *ctx;
    ngx_http_reqstat_store_t     *store;
    ngx_http_reqstat_rbnode_t    *fnode, **fnode_store;

    store = ngx_pcalloc(r->pool, sizeof(ngx_http_reqstat_store_t));
    if (store == NULL) {
        return NULL;
    }

    if (rlcf->monitor == NULL) {
        store->bypass = 1;
        return store;
    }

    store->conf = rlcf;

    switch (ngx_http_test_predicates(r, rlcf->bypass)) {

    case NGX_ERROR:
        return NULL;

    case NGX_DECLINED:
        store->bypass = 1;
        return store;

    default: /* NGX_OK */
        break;
    }

    if (ngx_array_init(&store->monitor_index, r->pool, rlcf->monitor->nelts,
                       sizeof(ngx_http_reqstat_rbnode_t *)) == NGX_ERROR)
    {
        return NULL;
    }

    if (ngx_array_init(&store->value_index, r->pool, rlcf->monitor->nelts,
                       sizeof(ngx_str_t)) == NGX_ERROR)
    {
        return NULL;
    }

    /* both arrays are kept index-aligned with rlcf->monitor */

    shm_zone = rlcf->monitor->elts;
    for (i = 0; i < rlcf->monitor->nelts; i++) {
        z = shm_zone[i];
        ctx = z->data;

        fnode = NULL;

        if (ngx_http_complex_value(r, &ctx->value, &val) != NGX_OK) {
            ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                          "failed to reap the key \"%V\"", ctx->val);
            ngx_str_null(&val);

        } else {
            fnode = ngx_http_reqstat_rbtree_lookup(z, &val);
            if (fnode == NULL) {
                ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                              "failed to alloc node in zone \"%V\", "
                              "enlarge it please",
                              &z->shm.name);
            }
        }

        value = ngx_array_push(&store->value_index);
        if (value == NULL) {
            return NULL;
        }

        *value = val;

        fnode_store = ngx_array_push(&store->monitor_index);
        if (fnode_store == NULL) {
            return NULL;
        }

        *fnode_store = fnode;
    }

    return store;
}


static ngx_int_t
ngx_http_reqstat_check_enable(ngx_http_request_t *r,
    ngx_http_reqstat_conf_t **rlcf, ngx_http_reqstat_store_t **store)
{
    ngx_http_reqstat_conf_t      *rmcf;
    ngx_http_reqstat_store_t     *s;
    ngx_http_variable_value_t    *v;

    rmcf = ngx_http_get_module_main_conf(r, ngx_http_reqstat_module);

    if (r->variables[rmcf->index].valid) {
        v = ngx_http_get_flushed_variable(r, rmcf->index);

        if (v == NULL || v->len == 0 || v->data[0] == '0') {
            return NGX_DECLINED;
        }

        ngx_memcpy(&s, &v->data[1], sizeof(uintptr_t));

    } else {

        /* the request did not reach the rewrite phase */

        s = ngx_http_get_module_ctx(r, ngx_http_reqstat_module);

        if (s == NULL) {
            s = ngx_http_reqstat_create_store(r,
                      ngx_http_get_module_loc_conf(r, ngx_http_reqstat_module));
            if (s == NULL) {
                return NGX_ERROR;
            }

            ngx_http_set_ctx(r, s, ngx_http_reqstat_module);

            if (ngx_http_get_indexed_variable(r, rmcf->index) == NULL) {
                return NGX_ERROR;
            }
        }

        if (s->bypass) {
            return NGX_DECLINED;
        }
    }

    *rlcf = s->conf;
    *store = s;

    return NGX_OK;
}
