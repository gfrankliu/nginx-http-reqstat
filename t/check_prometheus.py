"""Parse the Prometheus output of req_status_show and check a few samples.

usage: check_prometheus.py metrics.txt key
"""

import sys

from prometheus_client.parser import text_string_to_metric_families


def main(path, key):
    with open(path) as f:
        families = list(text_string_to_metric_families(f.read()))

    samples = {}
    for family in families:
        if family.type != "counter":
            sys.exit("%s: unexpected type %s" % (family.name, family.type))

        for s in family.samples:
            if s.labels.get("key") != key:
                continue
            extra = tuple(sorted((k, v) for k, v in s.labels.items()
                                 if k not in ("zone", "key")))
            samples[(s.name, extra)] = s.value

    expected = {
        ("nginx_reqstat_connections_total", ()): 2,
        ("nginx_reqstat_requests_total", ()): 4,
        ("nginx_reqstat_responses_total", (("status", "2xx"),)): 2,
        ("nginx_reqstat_responses_total", (("status", "5xx"),)): 1,
        ("nginx_reqstat_responses_by_code_total", (("code", "200"),)): 2,
        ("nginx_reqstat_responses_by_code_total", (("code", "404"),)): 1,
        ("nginx_reqstat_responses_by_code_total", (("code", "502"),)): 1,
        ("nginx_reqstat_upstream_requests_total", ()): 1,
        ("nginx_reqstat_upstream_responses_total", (("status", "5xx"),)): 1,
    }

    for k, v in expected.items():
        if samples.get(k) != v:
            sys.exit("%s%s: expected %s, got %s" % (k[0], k[1], v,
                                                     samples.get(k)))

    print("ok: prometheus, %d families, %d samples for %s"
          % (len(families), len(samples), key))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
