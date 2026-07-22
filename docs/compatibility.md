# Toolchain and compatibility guide

NovaBoot currently requires a compiler implementation of C++26 static
reflection. The supported development baseline is GCC 16.1 or newer with
`-freflection`, CMake 3.25 or newer, and Linux development libraries for
OpenSSL, io_uring, nghttp2, nghttp3, ngtcp2, SQLite, PostgreSQL, spdlog,
simdjson, and toml++.

CMake checks `-freflection` during configuration and stops with a direct
diagnostic when the selected compiler cannot support NovaBoot's public headers.
The `novaboot` target also exports that option to consuming targets, so an
application added with `add_subdirectory` compiles its controller, annotation,
DTO, and entity headers with the same reflection mode.

## Common build failures

| Symptom | Cause and action |
| --- | --- |
| `unrecognized command-line option '-freflection'` | Use the supported GCC reflection toolchain; upstream Clang and older GCC releases do not provide this implementation. |
| `std::meta` or `^^Type` parse errors | The consuming target is not linked through `novaboot::novaboot`, or it bypasses its public compile requirements. Link the target normally rather than copying header include paths. |
| Missing nghttp/ngtcp/io_uring CMake package | Install the Linux development package named in the configuration output, then configure again. |
| `io_uring_queue_init ... Operation not permitted` in a container or restricted CI | Allow io_uring for that test/runtime environment. This is an execution-policy restriction, not an application protocol failure. |

## Compatibility policy

NovaBoot's public API is source-oriented and uses C++ templates and reflection;
it does not promise ABI compatibility across compiler versions. Rebuild the
framework and application together when changing compiler version, standard
library, or compile flags. Keep reflection annotations and persistence mappings
in the application source so compiler diagnostics can point at the real type.

For supported data/JPA semantics and intentional differences from Hibernate,
see [JPA compatibility](jpa-compatibility.md). For framework-level examples,
see [reference applications](examples.md).
