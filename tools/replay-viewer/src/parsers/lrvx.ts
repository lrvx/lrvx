// Parser for .lrvx segment files (uncompressed only in MVP).
//
// Format: SegmentHeader(64) + stream of (FrameHeader(12) + payload).
// Frame types: 1 = Trade, 2 = BookSnapshot, 3 = BookDelta, 6 = BookTop.
// A top is the touch alone (at most one level per side, an empty side
// means empty) and has the same payload shape as a depth frame.
// See docs/spec/lrvx.md for the full layout.

const MAGIC_SEGMENT = 0x584f4c46; // "FLOX"
const MAGIC_BLOCK = 0x4b4c4246; // "FBLK"
const PRICE_SCALE = 1e8;
const QTY_SCALE = 1e8;

// LZ4 block-format decompressor. Spec:
// https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md
// `out` must be sized to original_size; we write into it directly.
function lz4DecompressBlock(src: Uint8Array, dst: Uint8Array): void {
  let sp = 0;
  let dp = 0;
  const sEnd = src.length;
  while (sp < sEnd) {
    const token = src[sp++];
    let litLen = token >>> 4;
    if (litLen === 15) {
      let b: number;
      do { b = src[sp++]; litLen += b; } while (b === 255);
    }
    for (let i = 0; i < litLen; i++) dst[dp++] = src[sp++];
    if (sp >= sEnd) break;
    const offset = src[sp++] | (src[sp++] << 8);
    if (offset === 0) throw new Error('LZ4 invalid offset 0');
    let matchLen = token & 0x0f;
    if (matchLen === 15) {
      let b: number;
      do { b = src[sp++]; matchLen += b; } while (b === 255);
    }
    matchLen += 4;
    let mp = dp - offset;
    for (let i = 0; i < matchLen; i++) dst[dp++] = dst[mp++];
  }
}

export interface Trade {
  type: 'trade';
  ts_ns: bigint;
  recv_ts_ns: bigint;
  price: number;
  qty: number;
  side: 'buy' | 'sell';
  symbol_id: number;
  trade_id: bigint;
}

export interface BookEvent {
  type: 'book_snapshot' | 'book_delta' | 'book_top';
  ts_ns: bigint;
  recv_ts_ns: bigint;
  symbol_id: number;
  bids: Array<{ price: number; qty: number }>;
  asks: Array<{ price: number; qty: number }>;
}

export type LrvxEvent = Trade | BookEvent;

function readU32(view: DataView, off: number): number {
  return view.getUint32(off, true);
}
function readU16(view: DataView, off: number): number {
  return view.getUint16(off, true);
}
function readI64(view: DataView, off: number): bigint {
  return view.getBigInt64(off, true);
}
function readU64(view: DataView, off: number): bigint {
  return view.getBigUint64(off, true);
}

export function parseSegment(buf: ArrayBuffer): LrvxEvent[] {
  const view = new DataView(buf);
  if (buf.byteLength < 64) {
    throw new Error('segment shorter than header');
  }
  if (readU32(view, 0) !== MAGIC_SEGMENT) {
    throw new Error('bad lrvx magic');
  }
  const flags = view.getUint8(6);
  const compression = view.getUint8(48);
  const compressed = (flags & 0x02) !== 0 || compression === 1;
  const indexOff = readU64(view, 40);
  // For uncompressed segments, stop the frame loop at the index trailer
  // so its bytes do not get misread as another FrameHeader.
  const frameStreamEnd = !compressed && indexOff > 0n
    ? Number(indexOff)
    : buf.byteLength;

  // If compressed, walk CompressedBlocks and concatenate the decompressed
  // frame stream into one buffer; then fall through to the regular
  // frame loop on the resulting bytes.
  let frameBuf: Uint8Array;
  if (compressed) {
    const chunks: Uint8Array[] = [];
    let bp = 64;
    const stopAt = indexOff > 0n ? Number(indexOff) : buf.byteLength;
    while (bp + 16 <= stopAt) {
      if (readU32(view, bp) !== MAGIC_BLOCK) {
        throw new Error('bad LZ4 block magic at offset ' + bp);
      }
      const csize = readU32(view, bp + 4);
      const osize = readU32(view, bp + 8);
      bp += 16;
      const src = new Uint8Array(buf, bp, csize);
      const dst = new Uint8Array(osize);
      lz4DecompressBlock(src, dst);
      chunks.push(dst);
      bp += csize;
    }
    let total = 0;
    for (const c of chunks) total += c.length;
    frameBuf = new Uint8Array(total);
    let off = 0;
    for (const c of chunks) { frameBuf.set(c, off); off += c.length; }
  } else {
    frameBuf = new Uint8Array(buf, 64, frameStreamEnd - 64);
  }

  return parseFrameStream(frameBuf);
}

function parseFrameStream(frameBuf: Uint8Array): LrvxEvent[] {
  // Wrap the frame buffer in a fresh DataView so all offsets below are
  // relative to the frame stream rather than the original segment file.
  const view = new DataView(frameBuf.buffer, frameBuf.byteOffset, frameBuf.byteLength);
  const events: LrvxEvent[] = [];
  let cursor = 0;
  while (cursor + 12 <= frameBuf.byteLength) {
    const size = readU32(view, cursor);
    const type = view.getUint8(cursor + 8);
    const rec_version = view.getUint8(cursor + 9);
    cursor += 12;
    if (rec_version !== 1) {
      throw new Error(`unknown rec_version ${rec_version}`);
    }
    if (cursor + size > frameBuf.byteLength) {
      break; // truncated frame
    }
    const payloadOff = cursor;
    cursor += size;
    if (type === 1 && size >= 48) {
      events.push({
        type: 'trade',
        ts_ns: readI64(view, payloadOff + 0),
        recv_ts_ns: readI64(view, payloadOff + 8),
        price: Number(readI64(view, payloadOff + 16)) / PRICE_SCALE,
        qty: Number(readI64(view, payloadOff + 24)) / QTY_SCALE,
        trade_id: readU64(view, payloadOff + 32),
        symbol_id: readU32(view, payloadOff + 40),
        side: view.getUint8(payloadOff + 44) === 0 ? 'buy' : 'sell',
      });
    } else if ((type === 2 || type === 3 || type === 6) && size >= 40) {
      const bid_count = readU16(view, payloadOff + 28);
      const ask_count = readU16(view, payloadOff + 30);
      const ev: BookEvent = {
        type: type === 2 ? 'book_snapshot' : type === 3 ? 'book_delta' : 'book_top',
        ts_ns: readI64(view, payloadOff + 0),
        recv_ts_ns: readI64(view, payloadOff + 8),
        symbol_id: readU32(view, payloadOff + 24),
        bids: [],
        asks: [],
      };
      let lvlOff = payloadOff + 40;
      for (let i = 0; i < bid_count; i++, lvlOff += 16) {
        ev.bids.push({
          price: Number(readI64(view, lvlOff)) / PRICE_SCALE,
          qty: Number(readI64(view, lvlOff + 8)) / QTY_SCALE,
        });
      }
      for (let i = 0; i < ask_count; i++, lvlOff += 16) {
        ev.asks.push({
          price: Number(readI64(view, lvlOff)) / PRICE_SCALE,
          qty: Number(readI64(view, lvlOff + 8)) / QTY_SCALE,
        });
      }
      events.push(ev);
    }
    // unknown type: skip silently per spec
  }
  return events;
}

export interface LrvxManifest {
  segments: Array<{ name: string; type: string }>;
}

// Symbol metadata table extracted from `metadata.json` if present.
// Lets the viewer label charts / trade rows with "exchange/name"
// instead of bare numeric IDs — the difference is most visible for
// merged multi-tape views (`lrvx tape view t1 t2`), where each global
// symbol id covers a different venue+instrument.
export type SymbolTable = Map<number, { exchange: string; name: string }>;

export interface LrvxParseResult {
  events: LrvxEvent[];
  symbols: SymbolTable;
}

interface MetadataJson {
  exchange?: string;
  symbols?: Array<{
    symbol_id: number;
    name?: string;
    base_asset?: string;
    quote_asset?: string;
  }>;
}

async function parseMetadataJson(file: File): Promise<SymbolTable> {
  const out: SymbolTable = new Map();
  try {
    const m = JSON.parse(await file.text()) as MetadataJson;
    if (Array.isArray(m.symbols)) {
      for (const s of m.symbols) {
        if (typeof s.symbol_id !== 'number') continue;
        const label = s.name ?? '';
        // Per `BinaryLogRecorderHook`, `metadata.json::exchange` is the
        // venue string. For merged tapes written by `lrvx tape view`'s
        // materialise path, the symbol name already carries
        // "exchange/symbol" because that's how the merger labels
        // global ids — leave it untouched in that case.
        const exchange = label.includes('/') ? '' : (m.exchange ?? '');
        out.set(s.symbol_id, { exchange, name: label });
      }
    }
  } catch {
    // Bad JSON / absent file. Caller treats empty symbols map as
    // "fall back to numeric ids".
  }
  return out;
}

export async function parseLrvxDirectory(
  files: File[],
): Promise<LrvxParseResult> {
  // Three on-disk shapes are accepted:
  //   1. Manifest form (publish spec):
  //        name.lrvx/manifest.json + segment .bin files listed inside.
  //   2. Bare segments + `metadata.json` (`lrvx tape record` output):
  //        name.lrvx/<timestamp>.lrvx + metadata.json
  //   3. Bare segments alone (legacy / partial captures): same as 2 but
  //      without metadata.json. Each candidate file is sniffed for the
  //      segment sentinel (it spells FLOX, for compatibility with existing
  //      tapes) before being treated as a segment.
  const manifestFile = files.find((f) => f.name === 'manifest.json');
  const metadataFile = files.find((f) => f.name === 'metadata.json');
  const symbols = metadataFile
    ? await parseMetadataJson(metadataFile)
    : new Map();
  const candidates: File[] = manifestFile
    ? []
    : files.filter((f) =>
        /\.(bin|lrvx|floxlog)$/i.test(f.name) || !f.name.includes('.'),
      );
  const all: LrvxEvent[] = [];
  if (manifestFile) {
    const manifest = JSON.parse(await manifestFile.text()) as LrvxManifest;
    for (const segMeta of manifest.segments) {
      const segFile = files.find((f) => f.name === segMeta.name);
      if (!segFile) continue;
      const buf = await segFile.arrayBuffer();
      all.push(...parseSegment(buf));
    }
  } else {
    for (const f of candidates) {
      const buf = await f.arrayBuffer();
      const view = new DataView(buf);
      if (buf.byteLength < 4) continue;
      if (view.getUint32(0, true) !== MAGIC_SEGMENT) continue;
      all.push(...parseSegment(buf));
    }
    if (all.length === 0) {
      throw new Error('no usable segment found — drop the .lrvx directory');
    }
  }
  all.sort((a, b) => Number(a.ts_ns - b.ts_ns));
  return { events: all, symbols };
}
