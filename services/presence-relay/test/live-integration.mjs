import assert from "node:assert/strict";
import WebSocket from "ws";

import { makeFallbackPng, signCoverPath, validatePng } from "../src/index.ts";

const origin = process.env.GODEN_RELAY_TEST_ORIGIN ?? "http://127.0.0.1:8792";
const admission = process.env.GODEN_RELAY_TEST_CREDENTIAL;
if (!admission) throw new Error("Set GODEN_RELAY_TEST_CREDENTIAL for the local fixture.");
const created = await fetch(`${origin}/v1/sessions`, {
  method: "POST", headers: { authorization: `Bearer ${admission}` }
});
assert.equal(created.status, 201);
const session = await created.json();

const socket = new WebSocket(session.socketUrl, {
  headers: { authorization: `Bearer ${session.secret}` }
});
await new Promise((resolve, reject) => {
  const timeout = setTimeout(() => reject(new Error("socket open timeout")), 3000);
  socket.once("open", () => {
    clearTimeout(timeout);
    resolve();
  });
  socket.once("error", reject);
});

const artwork = new Map();
const ignored = new Set();
const fetchCounts = new Map();
const deliveries = [];
socket.on("message", (data, isBinary) => {
  if (isBinary) return;
  const message = JSON.parse(data.toString("utf8"));
  if (message.op === "fetch") {
    fetchCounts.set(message.asset, (fetchCounts.get(message.asset) ?? 0) + 1);
    if (ignored.has(message.asset)) return;
    const png = artwork.get(message.asset);
    if (!png) return;
    const prefix = new TextEncoder().encode(message.request);
    const response = new Uint8Array(prefix.length + png.length);
    response.set(prefix);
    response.set(png, prefix.length);
    socket.send(response);
  } else if (message.op === "delivered") {
    deliveries.push(message);
  }
});

async function signedUrl(asset, expiresAt = Math.min(session.expiresAt, Date.now() + 60_000)) {
  const signature = await signCoverPath(
    session.secret, session.sessionId, asset, expiresAt
  );
  return `${session.coverBaseUrl}/${asset}/${expiresAt}/${signature}.png`;
}

async function expectFallback(url) {
  const response = await fetch(url);
  assert.equal(response.status, 200);
  assert.equal(response.headers.get("x-goden-cover"), "fallback");
  const bytes = new Uint8Array(await response.arrayBuffer());
  assert.equal(await validatePng(bytes), true);
  return bytes;
}

// Concurrent requests for the same exact cover are coalesced into one socket
// request and both receive byte-identical artwork.
const exactAsset = "ab".repeat(16);
const exactPng = makeFallbackPng(64);
artwork.set(exactAsset, exactPng);
const exactUrl = await signedUrl(exactAsset);
const [first, second] = await Promise.all([fetch(exactUrl), fetch(exactUrl)]);
for (const response of [first, second]) {
  assert.equal(response.status, 200);
  assert.equal(response.headers.get("x-goden-cover"), "exact");
  assert.deepEqual(new Uint8Array(await response.arrayBuffer()), exactPng);
}
assert.equal(fetchCounts.get(exactAsset), 1);

for (let attempt = 0; attempt < 20 && deliveries.length === 0; ++attempt) {
  await new Promise(resolve => setTimeout(resolve, 25));
}
assert.equal(deliveries.at(-1)?.asset, exactAsset);
assert.equal(deliveries.at(-1)?.kind, "exact");

// Invalid and expired tokens never expose an error body to Discord. Both
// resolve to a complete static PNG with HTTP 200.
const invalidUrl = exactUrl.replace(/[0-9a-f]{32}\.png$/u, `${"00".repeat(16)}.png`);
await expectFallback(invalidUrl);
const expiredAt = Date.now() - 1;
await expectFallback(await signedUrl("bc".repeat(16), expiredAt));
const overlongAt = Date.now() + 61_000;
await expectFallback(await signedUrl("bd".repeat(16), overlongAt));

// A connected client that cannot supply an asset exercises the 1.5 s timeout
// path; no HTML/5xx response escapes from the relay.
const timeoutAsset = "cd".repeat(16);
ignored.add(timeoutAsset);
const timeoutStarted = Date.now();
await expectFallback(await signedUrl(timeoutAsset));
assert.ok(Date.now() - timeoutStarted >= 1400);
assert.equal(fetchCounts.get(timeoutAsset), 1);

// Rapid distinct tracks remain generation-independent and cannot exchange
// each other's bytes.
const rapidA = "de".repeat(16);
const rapidB = "ef".repeat(16);
const rapidPngA = makeFallbackPng(32);
const rapidPngB = makeFallbackPng(48);
artwork.set(rapidA, rapidPngA);
artwork.set(rapidB, rapidPngB);
const [rapidResponseA, rapidResponseB] = await Promise.all([
  fetch(await signedUrl(rapidA)), fetch(await signedUrl(rapidB))
]);
assert.deepEqual(new Uint8Array(await rapidResponseA.arrayBuffer()), rapidPngA);
assert.deepEqual(new Uint8Array(await rapidResponseB.arrayBuffer()), rapidPngB);

// Once disconnected, a new valid token still returns the static fallback.
await new Promise(resolve => {
  socket.once("close", resolve);
  socket.close();
});
await expectFallback(await signedUrl("fa".repeat(16)));

console.log("relay live integration: PASS");
