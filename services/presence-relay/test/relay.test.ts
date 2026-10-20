import assert from "node:assert/strict";
import test from "node:test";

import { CoverSession, createSession, makeFallbackPng, signCoverPath, validatePng } from "../src/index.ts";

test("private admission rejects invalid credentials before allocating resources", async () => {
  let calls = 0;
  const namespace = {
    idFromName: () => { calls++; throw new Error("unauthorized allocation"); },
    get: () => { calls++; throw new Error("unauthorized allocation"); }
  };
  const verifier = Buffer.from(await crypto.subtle.digest("SHA-256",
    new TextEncoder().encode("a".repeat(32)))).toString("hex");
  for (const [config, authorization, status] of [
    [undefined, "", 503], ["broken", "", 503], ["[]", "", 503],
    [JSON.stringify([verifier]), "", 401],
    [JSON.stringify([verifier]), "Bearer short", 401],
    [JSON.stringify([verifier]), `Bearer ${"b".repeat(32)}`, 401]
  ] as const) {
    const response = await createSession(new Request("https://relay.invalid/v1/sessions", {
      method: "POST", headers: { authorization }
    }), { AUTHORIZED_CLIENT_SHA256: config,
      COVER_SESSIONS: namespace, RATE_LIMITERS: namespace } as never);
    assert.equal(response.status, status);
    assert.equal(calls, 0);
  }
});

test("rotated private credentials retain the existing rate limit", async () => {
  const token = "c".repeat(32);
  const verifier = Buffer.from(await crypto.subtle.digest("SHA-256",
    new TextEncoder().encode(token))).toString("hex");
  let calls = 0;
  const limiter = { idFromName: () => "fixture", get: () => ({
    fetch: async () => { calls++; return new Response("", {status:429}); }
  }) };
  const response = await createSession(new Request("https://relay.invalid/v1/sessions", {
    method:"POST", headers:{authorization:`Bearer ${token}`}
  }), { AUTHORIZED_CLIENT_SHA256: JSON.stringify(["0".repeat(64), verifier]),
    RATE_LIMITERS:limiter, COVER_SESSIONS:{} } as never);
  assert.equal(response.status, 429);
  assert.equal(calls, 1);
});

test("fallback is a complete bounded PNG", () => {
  const png = makeFallbackPng();
  return validatePng(png).then(valid => {
    assert.equal(valid, true);
    assert.ok(png.length > 1024);
    assert.ok(png.length < 1024 * 1024);
  });
});

test("PNG validation rejects truncation and excessive dimensions", async () => {
  const png = makeFallbackPng(64);
  assert.equal(await validatePng(png.subarray(0, png.length - 12)), false);
  const altered = png.slice();
  altered[16] = 0;
  altered[17] = 0;
  altered[18] = 4;
  altered[19] = 0;
  assert.equal(await validatePng(altered), false);
  const corrupt = png.slice();
  corrupt[corrupt.length - 16] ^= 0x40;
  assert.equal(await validatePng(corrupt), false);
});

test("malformed first chunk returns false and settles a pending relay request", async () => {
  const image = makeFallbackPng(32);
  image[12] = 255;
  assert.equal(await validatePng(image), false);
  // Use the actual handler with a fake Durable Object state: no sockets/network.
  const previous = Reflect.get(globalThis, "WebSocketRequestResponsePair");
  Reflect.set(globalThis, "WebSocketRequestResponsePair", class {});
  try {
    const session = new CoverSession({setWebSocketAutoResponse() {}, blockConcurrencyWhile() {}} as never);
    const requestId = "a".repeat(32);
    let outcome: {exact:boolean} | undefined;
    const timer = setTimeout(() => { throw new Error("pending request was not settled"); }, 1000);
    const pending = Reflect.get(session, "pending") as Map<string, unknown>;
    pending.set(requestId, {timer, resolve(value: {exact:boolean}) {outcome=value;}});
    const payload = new Uint8Array(32 + image.length);
    payload.set(new TextEncoder().encode(requestId)); payload.set(image, 32);
    await session.webSocketMessage({} as WebSocket, payload.buffer);
    assert.deepEqual(outcome && {exact:outcome.exact}, {exact:false});
    assert.equal(pending.size, 0);
  } finally {
    if (previous === undefined) Reflect.deleteProperty(globalThis, "WebSocketRequestResponsePair");
    else Reflect.set(globalThis, "WebSocketRequestResponsePair", previous);
  }
});

test("PNG validation rejects metadata, invalid filters and decompression bombs", async () => {
  const png = makeFallbackPng(32);
  const metadata = new TextEncoder().encode("tEXt");
  const ancillary = new Uint8Array([0, 0, 0, 0, ...metadata, 53, 93, 28, 11]);
  const withMetadata = new Uint8Array(png.length + ancillary.length);
  withMetadata.set(png.subarray(0, 33));
  withMetadata.set(ancillary, 33);
  withMetadata.set(png.subarray(33), 33 + ancillary.length);
  assert.equal(await validatePng(withMetadata), false);

  const wrongProfile = png.slice();
  wrongProfile[24] = 16;
  assert.equal(await validatePng(wrongProfile), false);

  const bomb = png.slice();
  bomb[19] = 1;
  assert.equal(await validatePng(bomb), false);
});

test("cover signatures bind session, asset and expiry", async () => {
  const secret = "AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA";
  const first = await signCoverPath(secret, "a".repeat(32), "b".repeat(32), 1234567890123);
  const second = await signCoverPath(secret, "a".repeat(32), "c".repeat(32), 1234567890123);
  assert.match(first, /^[0-9a-f]{32}$/u);
  assert.notEqual(first, second);
});
