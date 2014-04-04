uwsgi-riemann
=============

uWSGI plugin for riemann integration

It implements:

* stats pusher
* alarm
* hook

Building it
-----------

The plugin is 2.0-friendly so you can build it directly from your uWSGI binary:

```sh
uwsgi --build-plugin uwsgi-riemann
```

since uWSGI 2.0.3 you can git-clone and build in one shot

```
uwsgi --build-plugin https://github.com/unbit/uwsgi-riemann
```
