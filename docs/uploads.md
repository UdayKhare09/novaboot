# Multipart uploads

Use `http::parse_multipart` for a completed `multipart/form-data` request.
The parser is deliberately explicit: controller code decides when and where a
temporary upload becomes durable application storage.

```cpp
auto parsed = novaboot::http::parse_multipart(request, {
    .max_total_bytes = 10U * 1024U * 1024U,
    .max_in_memory_file_bytes = 256U * 1024U,
});
if (!parsed) {
    response.status(400).json(R"({"error":"Invalid multipart upload"})");
    return;
}

const auto title = parsed->field("title").value_or("");
for (const auto* file : parsed->files("attachment")) {
    if (file->in_memory()) {
        persist_bytes(file->filename(), file->data());
    } else {
        persist_file(file->filename(), file->temporary_path());
    }
}
```

The parser requires a valid `multipart/form-data` boundary and enforces total
body, part-count, part-header, and form-field limits. Small files remain owned
in the returned form. Larger files are written through `mkstemp` as private
mode-0600 files and are deleted when the last `UploadedFile` owner is destroyed.
Copy or move a file into durable storage before the form leaves scope; never use
the client filename as a filesystem path.

Request transport buffering still occurs before parsing. Configure
`BodySizeLimitMiddleware` with a compatible maximum as the first admission
limit for routes that accept uploads.
