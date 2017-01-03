uwsgi-datadog
=============

uWSGI plugin for datadog integration

It currently supports only metrics, alarm support (via events) is on work

INSTALL
=======

The plugin is 2.x friendly:

```sh
uwsgi --build-plugin https://github.com/unbit/uwsgi-datadog
```

USAGE
=====

Just pass the url of your datadog api:

```ini
[uwsgi]
master = true
processes = 8
threads = 4

http-socket = :9090
enable-metrics = true

plugin = datadog
stats-push = datadog:https://app.datadoghq.com/api/v1/series?api_key=API_KEY
# optional prefix for uwsgi internal metric name, up to 14 chars
datadog-prefix = uwsgi
# use canonical name for the hostname
datadog-canonical-hostname = true
