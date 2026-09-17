# cacert.pem

Mozilla's CA certificate set, as packaged by
[certifi](https://github.com/certifi/python-certifi) (MPL-2.0). It is what
the bundled fetcher verifies HTTPS certificates against, installed to
`/usr/local/crosskobo/cacert.pem`.

Refresh it with:

```sh
curl -o third_party/cacert.pem \
  https://raw.githubusercontent.com/certifi/python-certifi/master/certifi/cacert.pem
```

A reader who needs a private CA - a home server with its own certificate -
can append it to that file on the device, or add the certificate to
`/mnt/onboard/.crosskobo/extra-ca.pem`, which the fetcher is pointed at as
well when it exists.
