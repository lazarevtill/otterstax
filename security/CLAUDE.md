# Client security

`access` validates Keycloak access tokens using bounded HTTPS introspection.
The provider verifies signing keys and sessions; this server also requires the
configured issuer/audience, Bearer token type, subject, expiration and optional
not-before claim. No positive results are cached. Secrets come from an environment
variable named in configuration. Never log tokens, secrets, form bodies or provider
responses.

`transport` builds TLS and gRPC credentials. Policy construction rejects a
missing deployment mode. Keycloak mode requires TLS; trusted-private
mode explicitly admits all network clients. Each gRPC call is checked by the
credentials' AuthMetadataProcessor. Its AuthContext is connection-shared and must
not store per-call identity or tokens.
