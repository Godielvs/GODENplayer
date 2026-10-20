interface Env {
  // JSON array of SHA-256 token verifiers, provisioned as a Worker secret.
  AUTHORIZED_CLIENT_SHA256?: string;
  COVER_SESSIONS: DurableObjectNamespace;
  RATE_LIMITERS: DurableObjectNamespace;
}

interface SessionRecord {
  sessionId: string;
  secret: string;
  expiresAt: number;
}

interface ImageResult {
  bytes: Uint8Array;
  exact: boolean;
}

interface PendingImage {
  resolve: (result: ImageResult) => void;
  timer: ReturnType<typeof setTimeout>;
}

const SESSION_LIFETIME_MS = 12 * 60 * 60 * 1000;
const COVER_LIFETIME_MS = 60 * 1000;
const COVER_REQUEST_TIMEOUT_MS = 1500;
const MAX_COVER_BYTES = 1024 * 1024;
const MAX_COVER_EDGE = 512;
const MAX_COVER_REQUESTS_PER_MINUTE = 120;
const PNG_SIGNATURE = new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10]);

const utf8 = new TextEncoder();
const ascii = new TextDecoder("ascii", { fatal: true, ignoreBOM: false });

function json(data: unknown, status = 200): Response {
  return new Response(JSON.stringify(data), {
    status,
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": "no-store",
      "x-content-type-options": "nosniff"
    }
  });
}

function randomBytes(count: number): Uint8Array {
  const result = new Uint8Array(count);
  crypto.getRandomValues(result);
  return result;
}

function hexadecimal(bytes: Uint8Array): string {
  return Array.from(bytes, value => value.toString(16).padStart(2, "0")).join("");
}

function base64Url(bytes: Uint8Array): string {
  let binary = "";
  for (const value of bytes) binary += String.fromCharCode(value);
  return btoa(binary).replaceAll("+", "-").replaceAll("/", "_").replace(/=+$/u, "");
}

function decodeBase64Url(value: string): Uint8Array {
  const padded = value.replaceAll("-", "+").replaceAll("_", "/") +
    "=".repeat((4 - (value.length % 4)) % 4);
  const binary = atob(padded);
  return Uint8Array.from(binary, character => character.charCodeAt(0));
}

async function sha256Hex(value: string): Promise<string> {
  return hexadecimal(new Uint8Array(await crypto.subtle.digest("SHA-256", utf8.encode(value))));
}

export async function signCoverPath(
  secret: string,
  sessionId: string,
  assetId: string,
  expiresAt: number
): Promise<string> {
  const keyBytes = decodeBase64Url(secret);
  const keyMaterial = keyBytes.buffer.slice(
    keyBytes.byteOffset, keyBytes.byteOffset + keyBytes.byteLength
  ) as ArrayBuffer;
  const key = await crypto.subtle.importKey(
    "raw", keyMaterial, { name: "HMAC", hash: "SHA-256" }, false, ["sign"]
  );
  const message = utf8.encode(`${sessionId}:${assetId}:${expiresAt}`);
  const signature = new Uint8Array(await crypto.subtle.sign("HMAC", key, message));
  return hexadecimal(signature.subarray(0, 16));
}

function constantTimeEqual(left: string, right: string): boolean {
  if (left.length !== right.length) return false;
  let different = 0;
  for (let index = 0; index < left.length; ++index) {
    different |= left.charCodeAt(index) ^ right.charCodeAt(index);
  }
  return different === 0;
}

function readU32(bytes: Uint8Array, offset: number): number {
  return ((bytes[offset] << 24) | (bytes[offset + 1] << 16) |
    (bytes[offset + 2] << 8) | bytes[offset + 3]) >>> 0;
}

async function inflateBounded(
  parts: Uint8Array[], expectedBytes: number
): Promise<Uint8Array | undefined> {
  try {
    const stream = new Blob(parts).stream().pipeThrough(new DecompressionStream("deflate"));
    const reader = stream.getReader();
    const chunks: Uint8Array[] = [];
    let total = 0;
    for (;;) {
      const result = await reader.read();
      if (result.done) break;
      const chunk = result.value;
      total += chunk.byteLength;
      if (total > expectedBytes) {
        await reader.cancel("PNG raster exceeds declared dimensions");
        return undefined;
      }
      chunks.push(chunk);
    }
    return total === expectedBytes ? concatenate(chunks) : undefined;
  } catch {
    return undefined;
  }
}

export async function validatePng(bytes: Uint8Array): Promise<boolean> {
  if (bytes.length < 45 || bytes.length > MAX_COVER_BYTES) return false;
  for (let index = 0; index < PNG_SIGNATURE.length; ++index) {
    if (bytes[index] !== PNG_SIGNATURE[index]) return false;
  }
  if (readU32(bytes, 8) !== 13 || bytes[12] !== 73 || bytes[13] !== 72 ||
      bytes[14] !== 68 || bytes[15] !== 82) {
    return false;
  }
  const width = readU32(bytes, 16);
  const height = readU32(bytes, 20);
  if (width === 0 || height === 0 || width > MAX_COVER_EDGE || height > MAX_COVER_EDGE) {
    return false;
  }
  const bitDepth = bytes[24];
  const colorType = bytes[25];
  if (bitDepth !== 8 || (colorType !== 2 && colorType !== 6) ||
      bytes[26] !== 0 || bytes[27] !== 0 || bytes[28] !== 0) return false;
  let offset = 8;
  let hasIdat = false;
  let hasIend = false;
  let idatEnded = false;
  let chunkIndex = 0;
  const idat: Uint8Array[] = [];
  let compressedBytes = 0;
  while (offset + 12 <= bytes.length) {
    const length = readU32(bytes, offset);
    if (length > MAX_COVER_BYTES || offset + 12 + length > bytes.length) return false;
    let type = "";
    try {
      type = ascii.decode(bytes.subarray(offset + 4, offset + 8));
    } catch {
      return false;
    }
    const expectedCrc = readU32(bytes, offset + 8 + length);
    const actualCrc = crc32(bytes.subarray(offset + 4, offset + 8 + length));
    if (expectedCrc !== actualCrc) return false;
    if (chunkIndex === 0 && type !== "IHDR") return false;
    if (type !== "IHDR" && type !== "IDAT" && type !== "IEND") return false;
    if (type === "IHDR" && (chunkIndex !== 0 || length !== 13)) return false;
    if (type === "IDAT") {
      if (idatEnded) return false;
      hasIdat = true;
      compressedBytes += length;
      if (compressedBytes > MAX_COVER_BYTES) return false;
      idat.push(bytes.slice(offset + 8, offset + 8 + length));
    } else if (hasIdat) {
      idatEnded = true;
    }
    if (type === "IEND") {
      hasIend = length === 0 && offset + 12 === bytes.length;
      break;
    }
    offset += 12 + length;
    ++chunkIndex;
  }
  if (!hasIdat || !hasIend) return false;
  const bytesPerPixel = colorType === 6 ? 4 : 3;
  const rowBytes = width * bytesPerPixel + 1;
  const expectedRasterBytes = rowBytes * height;
  const raster = await inflateBounded(idat, expectedRasterBytes);
  if (!raster) return false;
  for (let row = 0; row < height; ++row) {
    if (raster[row * rowBytes] > 4) return false;
  }
  return true;
}

function crc32(bytes: Uint8Array): number {
  let crc = 0xffffffff;
  for (const value of bytes) {
    crc ^= value;
    for (let bit = 0; bit < 8; ++bit) {
      crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function adler32(bytes: Uint8Array): number {
  let a = 1;
  let b = 0;
  for (const value of bytes) {
    a = (a + value) % 65521;
    b = (b + a) % 65521;
  }
  return ((b << 16) | a) >>> 0;
}

function u32(value: number): Uint8Array {
  return new Uint8Array([
    (value >>> 24) & 0xff, (value >>> 16) & 0xff,
    (value >>> 8) & 0xff, value & 0xff
  ]);
}

function concatenate(parts: Uint8Array[]): Uint8Array {
  const result = new Uint8Array(parts.reduce((total, part) => total + part.length, 0));
  let offset = 0;
  for (const part of parts) {
    result.set(part, offset);
    offset += part.length;
  }
  return result;
}

function pngChunk(type: string, data: Uint8Array): Uint8Array {
  const typeBytes = utf8.encode(type);
  const body = concatenate([typeBytes, data]);
  return concatenate([u32(data.length), body, u32(crc32(body))]);
}

function storedDeflate(bytes: Uint8Array): Uint8Array {
  const parts: Uint8Array[] = [new Uint8Array([0x78, 0x01])];
  let offset = 0;
  while (offset < bytes.length) {
    const length = Math.min(65535, bytes.length - offset);
    const final = offset + length === bytes.length ? 1 : 0;
    const inverse = (~length) & 0xffff;
    parts.push(new Uint8Array([
      final, length & 0xff, (length >>> 8) & 0xff,
      inverse & 0xff, (inverse >>> 8) & 0xff
    ]));
    parts.push(bytes.subarray(offset, offset + length));
    offset += length;
  }
  parts.push(u32(adler32(bytes)));
  return concatenate(parts);
}

export function makeFallbackPng(edge = 128): Uint8Array {
  const raw = new Uint8Array((edge * 4 + 1) * edge);
  for (let y = 0; y < edge; ++y) {
    const row = y * (edge * 4 + 1);
    raw[row] = 0;
    for (let x = 0; x < edge; ++x) {
      const pixel = row + 1 + x * 4;
      const border = x < 5 || y < 5 || x >= edge - 5 || y >= edge - 5;
      const gShape = x > edge * 0.24 && x < edge * 0.74 &&
        y > edge * 0.20 && y < edge * 0.80 &&
        (x < edge * 0.34 || y < edge * 0.30 || y > edge * 0.70 ||
          (x > edge * 0.50 && y > edge * 0.48));
      const ratio = y / Math.max(1, edge - 1);
      const cyan = Math.round(218 * (1 - ratio));
      const pink = Math.round(236 * ratio);
      raw[pixel] = border || gShape ? pink : 12;
      raw[pixel + 1] = border || gShape ? cyan : 18;
      raw[pixel + 2] = border || gShape ? 235 : 20;
      raw[pixel + 3] = 255;
    }
  }
  const ihdr = concatenate([
    u32(edge), u32(edge), new Uint8Array([8, 6, 0, 0, 0])
  ]);
  return concatenate([
    PNG_SIGNATURE,
    pngChunk("IHDR", ihdr),
    pngChunk("IDAT", storedDeflate(raw)),
    pngChunk("IEND", new Uint8Array())
  ]);
}

const FALLBACK_PNG = makeFallbackPng();

function imageResponse(result: ImageResult, exactTtl = 60): Response {
  const ttl = result.exact ? Math.max(0, Math.min(60, exactTtl)) : 5;
  return new Response(result.bytes.slice().buffer, {
    status: 200,
    headers: {
      "content-type": "image/png",
      "content-length": String(result.bytes.length),
      "cache-control": `public, max-age=${ttl}`,
      "cloudflare-cdn-cache-control": `public, max-age=${ttl}`,
      "x-content-type-options": "nosniff",
      "x-goden-cover": result.exact ? "exact" : "fallback"
    }
  });
}

function fallbackResponse(): Response {
  return imageResponse({ bytes: FALLBACK_PNG, exact: false });
}

export class RateLimiter {
  private readonly state: DurableObjectState;

  constructor(state: DurableObjectState) {
    this.state = state;
  }

  async fetch(request: Request): Promise<Response> {
    const url = new URL(request.url);
    if (request.method !== "POST" || url.pathname !== "/allow") {
      return new Response(null, { status: 404 });
    }
    const now = Date.now();
    let rate = await this.state.storage.get<{ window: number; count: number }>("rate");
    if (!rate || now - rate.window >= 60_000) rate = { window: now, count: 0 };
    if (rate.count >= 10) return json({ allowed: false }, 429);
    ++rate.count;
    await this.state.storage.put("rate", rate);
    await this.state.storage.setAlarm(now + 120_000);
    return json({ allowed: true });
  }

  async alarm(): Promise<void> {
    await this.state.storage.deleteAll();
  }
}

export class CoverSession {
  private readonly state: DurableObjectState;
  private record: SessionRecord | undefined;
  private pending = new Map<string, PendingImage>();
  private inflight = new Map<string, Promise<ImageResult>>();
  private requestTimes: number[] = [];

  constructor(state: DurableObjectState) {
    this.state = state;
    this.state.setWebSocketAutoResponse(new WebSocketRequestResponsePair("ping", "pong"));
    this.state.blockConcurrencyWhile(async () => {
      this.record = await this.state.storage.get<SessionRecord>("session");
    });
  }

  async fetch(request: Request): Promise<Response> {
    const url = new URL(request.url);
    if (request.method === "POST" && url.pathname === "/init") {
      const proposed = await request.json<SessionRecord>();
      if (!this.record && /^[0-9a-f]{32}$/u.test(proposed.sessionId) &&
          /^[A-Za-z0-9_-]{40,64}$/u.test(proposed.secret) &&
          proposed.expiresAt > Date.now() && proposed.expiresAt <= Date.now() + SESSION_LIFETIME_MS) {
        this.record = proposed;
        await this.state.storage.put("session", proposed);
        await this.state.storage.setAlarm(proposed.expiresAt);
      }
      return json({ initialized: Boolean(this.record) });
    }
    if (request.method === "GET" && url.pathname === "/socket") {
      return this.acceptSocket(request);
    }
    const match = /^\/cover\/([0-9a-f]{32})\/(\d{10,14})\/([0-9a-f]{32})$/u.exec(url.pathname);
    if (request.method === "GET" && match) {
      return this.serveCover(match[1], Number(match[2]), match[3]);
    }
    return new Response(null, { status: 404 });
  }

  private async acceptSocket(request: Request): Promise<Response> {
    if (!this.record || this.record.expiresAt <= Date.now()) {
      return json({ error: "expired session" }, 410);
    }
    const authorization = request.headers.get("authorization") ?? "";
    if (!constantTimeEqual(authorization, `Bearer ${this.record.secret}`)) {
      return json({ error: "unauthorized" }, 401);
    }
    if (request.headers.get("upgrade")?.toLowerCase() !== "websocket") {
      return json({ error: "websocket required" }, 426);
    }
    if (this.state.getWebSockets("client").some(socket => socket.readyState === 1)) {
      return json({ error: "session already connected" }, 409);
    }
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    this.state.acceptWebSocket(server, ["client"]);
    server.serializeAttachment({ connectedAt: Date.now() });
    return new Response(null, { status: 101, webSocket: client });
  }

  private async serveCover(assetId: string, expiresAt: number, signature: string): Promise<Response> {
    const now = Date.now();
    if (!this.record || this.record.expiresAt <= now ||
        expiresAt <= now || expiresAt > this.record.expiresAt ||
        expiresAt - now > COVER_LIFETIME_MS) {
      return fallbackResponse();
    }
    const expected = await signCoverPath(
      this.record.secret, this.record.sessionId, assetId, expiresAt
    );
    if (!constantTimeEqual(signature, expected)) return fallbackResponse();

    this.requestTimes = this.requestTimes.filter(value => now - value < 60_000);
    if (this.requestTimes.length >= MAX_COVER_REQUESTS_PER_MINUTE) return fallbackResponse();
    let persistedRate = await this.state.storage.get<{
      window: number; count: number;
    }>("cover-rate");
    if (!persistedRate || now - persistedRate.window >= 60_000) {
      persistedRate = { window: now, count: 0 };
    }
    if (persistedRate.count >= MAX_COVER_REQUESTS_PER_MINUTE) return fallbackResponse();
    ++persistedRate.count;
    await this.state.storage.put("cover-rate", persistedRate);
    this.requestTimes.push(now);

    let pending = this.inflight.get(assetId);
    if (!pending) {
      pending = this.requestFromClient(assetId);
      this.inflight.set(assetId, pending);
    }
    try {
      const startedAt = Date.now();
      const result = await pending;
      this.notifyDelivery(assetId, result.exact, Date.now() - startedAt);
      const remainingSeconds = Math.max(0,
        Math.floor((expiresAt - Date.now()) / 1000));
      return imageResponse(result, remainingSeconds);
    } finally {
      if (this.inflight.get(assetId) === pending) this.inflight.delete(assetId);
    }
  }

  private notifyDelivery(assetId: string, exact: boolean, latencyMs: number): void {
    const message = JSON.stringify({
      op: "delivered",
      asset: assetId,
      kind: exact ? "exact" : "fallback",
      latencyMs: Math.max(0, Math.min(10_000, Math.round(latencyMs)))
    });
    for (const socket of this.state.getWebSockets("client")) {
      if (socket.readyState !== 1) continue;
      try {
        socket.send(message);
      } catch {
        // Delivery already completed. Diagnostics are deliberately best effort.
      }
    }
  }

  private requestFromClient(assetId: string): Promise<ImageResult> {
    const socket = this.state.getWebSockets("client").find(value => value.readyState === 1);
    if (!socket) return Promise.resolve({ bytes: FALLBACK_PNG, exact: false });
    const requestId = hexadecimal(randomBytes(16));
    return new Promise<ImageResult>(resolve => {
      const timer = setTimeout(() => {
        this.pending.delete(requestId);
        resolve({ bytes: FALLBACK_PNG, exact: false });
      }, COVER_REQUEST_TIMEOUT_MS);
      this.pending.set(requestId, { resolve, timer });
      try {
        socket.send(JSON.stringify({ op: "fetch", request: requestId, asset: assetId }));
      } catch {
        clearTimeout(timer);
        this.pending.delete(requestId);
        resolve({ bytes: FALLBACK_PNG, exact: false });
      }
    });
  }

  async webSocketMessage(_socket: WebSocket, message: string | ArrayBuffer): Promise<void> {
    if (typeof message === "string") {
      this.resolveAllPending();
      return;
    }
    const bytes = new Uint8Array(message);
    if (bytes.length <= 32 || bytes.length > MAX_COVER_BYTES + 32) {
      this.resolveAllPending();
      return;
    }
    let requestId = "";
    try {
      requestId = ascii.decode(bytes.subarray(0, 32));
    } catch {
      this.resolveAllPending();
      return;
    }
    if (!/^[0-9a-f]{32}$/u.test(requestId)) {
      this.resolveAllPending();
      return;
    }
    const pending = this.pending.get(requestId);
    if (!pending) return;
    const image = bytes.slice(32);
    clearTimeout(pending.timer);
    this.pending.delete(requestId);
    try {
      pending.resolve(await validatePng(image)
        ? { bytes: image, exact: true }
        : { bytes: FALLBACK_PNG, exact: false });
    } catch {
      // Validation must not strand the HTTP request after its deadline was
      // removed. Even an unexpected decoder/runtime failure returns fallback.
      pending.resolve({ bytes: FALLBACK_PNG, exact: false });
    }
  }

  private resolveAllPending(): void {
    for (const [requestId, pending] of this.pending) {
      clearTimeout(pending.timer);
      pending.resolve({ bytes: FALLBACK_PNG, exact: false });
      this.pending.delete(requestId);
    }
  }

  async alarm(): Promise<void> {
    this.resolveAllPending();
    for (const socket of this.state.getWebSockets("client")) {
      try { socket.close(1000, "session expired"); } catch {}
    }
    this.record = undefined;
    this.requestTimes = [];
    await this.state.storage.deleteAll();
  }

  webSocketClose(): void { this.resolveAllPending(); }
  webSocketError(): void { this.resolveAllPending(); }
}

export async function createSession(request: Request, env: Env): Promise<Response> {
  // Authenticate before touching either Durable Object namespace. A missing
  // secret denies admission; it must never silently restore public sessions.
  let verifiers: unknown;
  try { verifiers = JSON.parse(env.AUTHORIZED_CLIENT_SHA256 ?? "null"); }
  catch { return json({ error: "private admission is not configured" }, 503); }
  if (!Array.isArray(verifiers) || !verifiers.length || verifiers.length > 32 ||
      verifiers.some(value => typeof value !== "string" || !/^[0-9a-f]{64}$/u.test(value)))
    return json({ error: "private admission is not configured" }, 503);
  const authorization = request.headers.get("authorization") ?? "";
  if (!/^Bearer [A-Za-z0-9_-]{32,256}$/u.test(authorization))
    return json({ error: "private admission required" }, 401);
  const verifier = await sha256Hex(authorization.slice(7));
  let accepted = false;
  for (const value of verifiers) accepted = constantTimeEqual(value, verifier) || accepted;
  if (!accepted) return json({ error: "private admission required" }, 401);
  const ip = request.headers.get("cf-connecting-ip") ?? "unknown";
  const limiterName = await sha256Hex(`session:${ip}`);
  const limiter = env.RATE_LIMITERS.get(env.RATE_LIMITERS.idFromName(limiterName));
  const allowed = await limiter.fetch("https://internal/allow", { method: "POST" });
  if (!allowed.ok) return json({ error: "session rate limit" }, 429);

  const sessionId = hexadecimal(randomBytes(16));
  const secret = base64Url(randomBytes(32));
  const expiresAt = Date.now() + SESSION_LIFETIME_MS;
  const durable = env.COVER_SESSIONS.get(env.COVER_SESSIONS.idFromName(sessionId));
  const initialized = await durable.fetch("https://internal/init", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ sessionId, secret, expiresAt })
  });
  if (!initialized.ok) return json({ error: "session initialization failed" }, 503);
  const origin = new URL(request.url).origin;
  return json({
    version: 1,
    sessionId,
    secret,
    expiresAt,
    socketUrl: `${origin.replace(/^http/u, "ws")}/v1/sessions/${sessionId}/socket`,
    coverBaseUrl: `${origin}/v1/c/${sessionId}`
  }, 201);
}

async function routeCover(request: Request, env: Env, context: ExecutionContext): Promise<Response> {
  const url = new URL(request.url);
  const match = /^\/v1\/c\/([0-9a-f]{32})\/([0-9a-f]{32})\/(\d{10,14})\/([0-9a-f]{32})\.png$/u.exec(url.pathname);
  if (!match || url.search) return fallbackResponse();
  const cache = (caches as unknown as { default: Cache }).default;
  const cached = await cache.match(request);
  if (cached) return cached;
  const durable = env.COVER_SESSIONS.get(env.COVER_SESSIONS.idFromName(match[1]));
  const response = await durable.fetch(
    `https://internal/cover/${match[2]}/${match[3]}/${match[4]}`
  );
  if (response.ok) context.waitUntil(cache.put(request, response.clone()));
  return response.ok ? response : fallbackResponse();
}

export default {
  async fetch(request: Request, env: Env, context: ExecutionContext): Promise<Response> {
    const url = new URL(request.url);
    if (request.method === "GET" && url.pathname === "/health") {
      return json({ service: "goden-presence-relay", version: 1, healthy: true });
    }
    if (request.method === "POST" && url.pathname === "/v1/sessions") {
      return createSession(request, env);
    }
    const socket = /^\/v1\/sessions\/([0-9a-f]{32})\/socket$/u.exec(url.pathname);
    if (request.method === "GET" && socket) {
      const durable = env.COVER_SESSIONS.get(env.COVER_SESSIONS.idFromName(socket[1]));
      return durable.fetch("https://internal/socket", request);
    }
    if (request.method === "GET" && url.pathname.startsWith("/v1/c/")) {
      return routeCover(request, env, context);
    }
    return json({ error: "not found" }, 404);
  }
};
