# OpenAPI

`novaboot::openapi::Document` produces a deterministic OpenAPI 3.1 JSON
snapshot from the routes already registered with a NovaBoot router. Reflected
controllers and fluent routes share that router metadata, so both appear in the
same document.

```cpp
auto server = Server::create()
    .di_container(root)
    .tls("cert.pem", "key.pem")
    .build();

novaboot::openapi::serve(
    server->router(), "/openapi.json",
    {.title = "Knowledge Hub API", .version = "1.0.0",
     .servers = {"https://api.example.test"}});
```

The endpoint uses `application/vnd.oai.openapi+json;version=3.1`. Route
patterns such as `/articles/:article_id` become `/articles/{article_id}` with
required string path parameters. The document is a snapshot: install it after
all routes are registered.

The current reflected validation API is runtime `Schema<T>` code rather than
field annotations, so validator constraints and request/response DTO schemas
cannot yet be inferred safely. Add explicit API/validation metadata before
claiming schema-level OpenAPI generation.
