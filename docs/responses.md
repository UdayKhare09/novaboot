# HTTP representations and content negotiation

Handlers should set an explicit representation instead of relying on a browser
default:

```cpp
response.json(R"({"status":"ok"})");
response.text("ready");
response.body(pdf_bytes).download("report.pdf", "application/pdf");
```

`json` sets `application/json`; `text` sets UTF-8 `text/plain`; `download`
sets `Content-Disposition: attachment` and a supplied content type. Download
filenames are normalized to avoid CR/LF injection and path-like names. Raw
`body()` remains available for protocol-level code and deliberately does not
guess a content type.

To enforce the request's `Accept` header, install the opt-in middleware:

```cpp
app->middleware(std::make_shared<novaboot::middleware::ContentNegotiationMiddleware>());
```

It supports comma-separated media ranges, quality values, `type/*`, and
`*/*`. A response with an explicit `Content-Type` that is not acceptable to the
client is replaced with `406 Not Acceptable` and `Vary: Accept`. The endpoint
still chooses which representation it can produce; NovaBoot does not silently
serialize an arbitrary C++ type into a different format.
