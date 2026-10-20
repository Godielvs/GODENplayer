# Optional private cover relay
Public GODENplayer uses Catbox after disclosure; this service is not a public dependency. No personal endpoint or production credential is distributed.

Use Node 24, `npm ci` and `npm run check`. `npm run dev` starts the local fixture. Deployment is manual and is not performed by CI or by the public preparation process.

## Coordinated admission migration
1. Build/qualify clients with configurable HTTPS origin and per-client credential support first. Do not deploy a server admission requirement while existing private clients cannot supply credentials.
2. Generate separate random base64url credentials (at least 32 characters) for each approved client. Distribute privately. Configure endpoint and credential in Options; Windows DPAPI protects local persistence. Do not place endpoint/credentials in this source tree, public installers, logs or screenshots.
3. Provision `AUTHORIZED_CLIENT_SHA256` as a Worker **secret** containing a JSON array of lowercase SHA-256 credential verifiers (1–32 entries). Keep raw tokens outside the Worker and public files. Use interactive secret provisioning, not command-line arguments exposing tokens. No valid verifier means session creation returns 503 before allocating Durable Objects.
4. Confirm authenticated clients against staging, then switch the private deployment. Retain deployment/client rollback until both clients are verified. Rotate by temporarily admitting old/new verifiers, migrate clients, then remove old verifiers.

The admission credential is separate from the generated per-session WebSocket secret. Unauthorized requests allocate neither namespace. Authorized clients retain per-IP throttling. Image URLs bind session, asset and expiry. Native clients use only their configured origin. Private-provider failure never falls back to Catbox.

Local integration requires `GODEN_RELAY_TEST_CREDENTIAL` plus its verifier in an ignored `.dev.vars`; `npm run check:live` defaults to 127.0.0.1. QA fixture tokens are compiled out of production. Keep `.dev.vars`, `.env` and `.wrangler` ignored. No live endpoint is tested without its operator's authorization.

On-demand delivery keeps no permanent server image repository, but expiry does not guarantee deletion from recipient caches. See ../../docs/PRIVACY.md.
