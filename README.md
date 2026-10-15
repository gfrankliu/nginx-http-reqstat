Name
====

* ngx_http_reqstat_module

This module collects the running status of nginx, grouped by an arbitrary key
(domain, url, etc), and shows it in plain text or in Prometheus exposition format.

It comes from Taobao Tengine @ https://github.com/alibaba/tengine, and is kept in
sync with Tengine's `modules/ngx_http_reqstat_module`, adapted to work with stock
nginx (no core patch needed).

Description
===========

* It can provide running status information of nginx.

* The information is divided into different zones, and each zone is independent.

* The status information is about connections, requests, response status codes
  (by class and for selected individual codes such as 502/503/504),
  input and output flows, rt, upstreams, and so on.

* It shows all the results by default, and can be set to show part of them by specifying zones.

* It supports user-defined status by using nginx variables. The maximum of all the status is 50.

* It recycles out-of-date running status information when a zone is full.

* It supports defining output fields, and a Prometheus output format.

* It follows the request processing flow, so internal redirects will not affect monitoring.

* Do not use response variables (e.g. `$status`) as a key, unless `req_status_lazy` is on.

* HTTP/1.x, HTTP/2 and HTTP/3 are supported. For HTTP/2 and HTTP/3, `conn_total`
  counts client connections, not streams.

Compilation
===========

Requires nginx 1.9.11 or later; tested with nginx 1.18, 1.24 and 1.31.

Static module:

    ./configure --add-module=/path/to/nginx-http-reqstat

Dynamic module:

    ./configure --with-compat --add-dynamic-module=/path/to/nginx-http-reqstat
    make modules

and load it in `nginx.conf`:

    load_module modules/ngx_http_reqstat_module.so;

Older versions of this module required patching nginx with `connection.patch`.
This is no longer needed and the patch has been removed.


Example
===========

    http {

        req_status_zone server "$host,$server_addr:$server_port" 10M;
        req_status_zone_add_indicator server $limit;

        server {
            location /us {
                req_status_show;
                req_status_show_field req_total $limit;
            }

            location /metrics {
                req_status_show;
                req_status_show_format prometheus;
                allow 10.0.0.0/8;
                deny all;
            }

            set $limit 0;

            if ($arg_limit = '1') {
                set $limit 1;
            }

            req_status server;
        }
    }

Plain text format
-----------------

* when you call '/us' without `req_status_show_field`, you will get results like this:

            www.example.com,127.0.0.1:80,162,6242,1,1,1,0,0,0,0,10,1,10,1....

    * Each line shows the status information of a "$host,$server_addr:$server_port".

    * Default line format:

            kv,bytes_in,bytes_out,conn_total,req_total,http_2xx,http_3xx,http_4xx,http_5xx,http_other_status,rt,ups_req,ups_rt,ups_tries,http_200,http_206,http_302,http_304,http_403,http_404,http_416,http_499,http_500,http_502,http_503,http_504,http_508,http_other_detail_status,http_ups_4xx,http_ups_5xx

        followed by the user defined indicators of the zone, if any.

        * **kv**                value of the variable defined by the directive 'req_status_zone'. The maximum key length is configurable, 152B by default, and longer keys are cut off
        * **bytes_in**          total number of bytes received from client (`$request_length`)
        * **bytes_out**         total number of bytes sent to client (`$bytes_sent`)
        * **conn_total**        total number of accepted connections
        * **req_total**         total number of processed requests
        * **http_2xx**          total number of 2xx requests
        * **http_3xx**          total number of 3xx requests
        * **http_4xx**          total number of 4xx requests
        * **http_5xx**          total number of 5xx requests
        * **http_other_status** total number of other requests
        * **rt**                accumulation of rt, in milliseconds
        * **ups_req**           total number of requests calling for upstream
        * **ups_rt**            accumulation of upstream rt, in milliseconds
        * **ups_tries**         total number of times calling for upstream
        * **http_200**          total number of 200 requests
        * **http_206**          total number of 206 requests
        * **http_302**          total number of 302 requests
        * **http_304**          total number of 304 requests
        * **http_403**          total number of 403 requests
        * **http_404**          total number of 404 requests
        * **http_416**          total number of 416 requests
        * **http_499**          total number of 499 requests
        * **http_500**          total number of 500 requests
        * **http_502**          total number of 502 requests
        * **http_503**          total number of 503 requests
        * **http_504**          total number of 504 requests
        * **http_508**          total number of 508 requests
        * **http_other_detail_status**      total number of requests of other status codes
        * **http_ups_4xx**      total number of requests whose last upstream response was 4xx
        * **http_ups_5xx**      total number of requests whose last upstream response was 5xx

    * You can use names in the left column to define output format, with directive 'req_status_show_field'

    * The first 14 columns are the same as in earlier versions of this module
      (Tengine 2.1.0), so existing parsers keep working; the new columns are appended.

* tsar can parse the result and monitor, see also https://github.com/alibaba/tsar

Prometheus format
-----------------

With `req_status_show_format prometheus;` the same counters are exposed in the
Prometheus text exposition format (version 0.0.4). Every sample carries the labels
`zone` (zone name) and `key` (the zone key value):

    # HELP nginx_reqstat_requests_total Total requests processed.
    # TYPE nginx_reqstat_requests_total counter
    nginx_reqstat_requests_total{zone="server",key="www.example.com,127.0.0.1:80"} 162
    # HELP nginx_reqstat_responses_total Total responses by status class.
    # TYPE nginx_reqstat_responses_total counter
    nginx_reqstat_responses_total{zone="server",key="www.example.com,127.0.0.1:80",status="2xx"} 150
    ...

| metric                                               | extra label             | text field(s)                    |
|------------------------------------------------------|-------------------------|----------------------------------|
| `nginx_reqstat_bytes_in_total`                       |                         | bytes_in                         |
| `nginx_reqstat_bytes_out_total`                      |                         | bytes_out                        |
| `nginx_reqstat_connections_total`                    |                         | conn_total                       |
| `nginx_reqstat_requests_total`                       |                         | req_total                        |
| `nginx_reqstat_responses_total`                      | `status` = 2xx..5xx, other | http_2xx .. http_other_status |
| `nginx_reqstat_responses_by_code_total`              | `code` = 200, 206, ..., 508, other | http_200 .. http_other_detail_status |
| `nginx_reqstat_request_time_seconds_total`           |                         | rt (converted to seconds)        |
| `nginx_reqstat_upstream_requests_total`              |                         | ups_req                          |
| `nginx_reqstat_upstream_tries_total`                 |                         | ups_tries                        |
| `nginx_reqstat_upstream_response_time_seconds_total` |                         | ups_rt (converted to seconds)    |
| `nginx_reqstat_upstream_responses_total`             | `status` = 4xx, 5xx     | http_ups_4xx, http_ups_5xx       |
| `nginx_reqstat_indicator_total`                      | `name` = variable name  | user defined indicators          |

Average latency can be computed in PromQL, e.g.

    rate(nginx_reqstat_request_time_seconds_total[5m]) / rate(nginx_reqstat_requests_total[5m])

`req_status_show_field` also applies to the Prometheus format: only the selected
fields are exposed. Keep in mind that every distinct key becomes a time series, so
avoid high-cardinality keys (such as `$uri` or `$remote_addr`) when scraping.

Directives
==========

req_status_zone
-------------------------

**Syntax**: *req_status_zone zone_name value size*

**Default**: *none*

**Context**: *http*

create shared memory for this module. 'zone_name' is the name of memory block.
'value' defines the key, in which variables can be used.
'size' defines the size of shared memory.

Example:

    req_status_zone server "$host,$server_addr:$server_port" 10M;

    the memory is 10MB, the key is "$host,$server_addr:$server_port", and the name is "server".

* Notice, if you want to use tsar to monitor, you should not use comma in the key.


req_status
-------------------------

**Syntax**: *req_status zone_name1 [zone_name2 [zone_name3 [...]]]*

**Default**: *none*

**Context**: *http, srv, loc*

Enable monitoring. You can specify multiple zones to monitor.


req_status_show
-------------------------

**Syntax**: *req_status_show [zone_name1 [zone_name2 [...]]]*

**Default**: *all the targets defined by 'req_status_zone'*

**Context**: *loc*

Display the status information. You can specify zones to display.


req_status_show_format
-------------------------------

**Syntax**: *req_status_show_format text|prometheus*

**Default**: *text*

**Context**: *loc*

Output format used by `req_status_show`. `prometheus` produces the Prometheus text
exposition format, described above.


req_status_show_field
-------------------------------

**Syntax**: *req_status_show_field field_name1 [field_name2 [field_name3 [...]]]*

**Default**: *all the fields, including user defined fields*

**Context**: *loc*

Define output format, used with the directive 'req_status_show'. You can use names
to define internal supported fields, see it above. And also you can use variables
to define user defined fields. 'kv' is always the first field in a line.


req_status_bypass
-------------------------------

**Syntax**: *req_status_bypass condition ...*

**Default**: *none*

**Context**: *http, srv, loc*

Do not count the request if any of the conditions is not empty and not "0",
like `proxy_cache_bypass`. Useful to exclude the status page itself:

    location /us {
        req_status_show;
        req_status_bypass 1;
    }


req_status_zone_add_indicator
--------------------------------

**Syntax**: *req_status_zone_add_indicator zone_name $var1 [$var2 [...]]*

**Default**: *none*

**Context**: *http*

Add user-defined status by using nginx variables. The trailing number of each
variable value is accumulated; if it has a fractional part, it is scaled by its
number of decimal digits (e.g. "0.25" adds 25). The status will be appended at the
end of each line on display.


req_status_zone_key_length
-------------------------------

**Syntax**: *req_status_zone_key_length zone_name length*

**Default**: *152*

**Context**: *http*

Define the maximum length of key for a zone. Changing it on reload takes effect
only after a restart, or when the zone size changes.


req_status_zone_recycle
-------------------------------

**Syntax**: *req_status_zone_recycle zone_name times seconds*

**Default**: *10 60*

**Context**: *http*

Define the recycle threshold for a zone. Recycle will be switched on when the shared memory is exhausted,
and will only take effect on information whose visit frequency is lower than the setting.
The setting frequency is defined by 'times' and 'seconds', and it is 10r/min by default.

     req_status_zone_recycle demo_zone 10 60;


req_status_lazy
-------------------------------

**Syntax**: *req_status_lazy on|off*

**Default**: *off*

**Context**: *http, srv, loc*

Re-evaluate the key of `req_status_zone` in the log phase, so that variables which
are only known late (such as `$upstream_*`) can be used as keys.


Notes
=====

This is a Tengine module made to work with stock nginx. It follows the original
Tengine license.
