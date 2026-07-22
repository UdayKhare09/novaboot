# Static resources

Configure a directory with `Server::Builder::static_resources`. NovaBoot serves
regular files beneath that directory only when no application route matches.

```cpp
auto app = novaboot::Server::create()
    .static_resources("src/resources/static")
    .build();
```

`GET /` resolves to `index.html`. `GET` and `HEAD` responses include a content
type, `Content-Length`, `ETag`, `Cache-Control: public, max-age=3600`, and
`Accept-Ranges: bytes`.

The server supports exact `If-None-Match`/`*` conditional requests (`304`) and
one RFC 7233 byte range (`206`). Invalid, multiple, or unsatisfiable ranges
return `416` with `Content-Range: bytes */<size>`. File resolution rejects
paths outside the canonical resource root, backslashes, and embedded NULs.

This is intentionally a regular-file convenience layer, not an asset pipeline.
Use a CDN or reverse proxy when an application needs immutable fingerprinted
assets, compression variants, or global caching policy.
