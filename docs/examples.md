# Reference applications

NovaBoot keeps runnable applications in `examples/`. Configure the repository,
build the desired target, then launch its executable from the repository root.

```bash
cmake -S . -B build -DNOVABOOT_BUILD_EXAMPLES=ON
cmake --build build --target websocket_chat_app
./build/examples/websocket_chat/websocket_chat_app
```

| Name | Focus |
| --- | --- |
| `knowledge_hub` | PostgreSQL CRUD service with explicit migrations, schema validation, relationships, paging, optimistic locking, and a static UI. |
| `todo_notes` | Secured PostgreSQL API with JWT issuance/verification, route authorization, transactions, and validation. |
| `websocket_chat` | Raw WebSocket and STOMP chat UI, including `/app` message mappings and `/topic` delivery. |
| `server` | Small DI, controller, middleware, and static-resource starter application. |

The PostgreSQL applications read their connection and TLS settings from the
example-local `src/resources/config.toml`. Run migrations/schema validation at
startup as shown by `knowledge_hub`; do not point them at production databases.

Reference apps are maintained as framework integration examples, not generic
deployment manifests. Add production secret management, reverse-proxy policy,
and database backup/restore procedures in the consuming application.
