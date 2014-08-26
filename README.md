uwsgi-datadog
=============

uWSGI plugin for datadog integration

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
