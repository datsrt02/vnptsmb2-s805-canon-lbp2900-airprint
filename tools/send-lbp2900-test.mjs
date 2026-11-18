// Minimal dependency-free IPP/URF hardware test for the Linux ARMHF port.
// Usage: node tools/send-lbp2900-test.mjs 192.168.1.33
import { Buffer } from 'node:buffer';

const host = process.argv[2] || '192.168.1.33';
const width = 4760;
const height = 7016;
const dpi = 600;

const font = {
  ' ': ['00000','00000','00000','00000','00000','00000','00000'],
  '-': ['00000','00000','00000','11111','00000','00000','00000'],
  '0': ['01110','10001','10011','10101','11001','10001','01110'],
  '2': ['01110','10001','00001','00010','00100','01000','11111'],
  '5': ['11111','10000','11110','00001','00001','10001','01110'],
  '8': ['01110','10001','10001','01110','10001','10001','01110'],
  '9': ['01110','10001','10001','01111','00001','00010','11100'],
  A: ['01110','10001','10001','11111','10001','10001','10001'],
  B: ['11110','10001','10001','11110','10001','10001','11110'],
  C: ['01111','10000','10000','10000','10000','10000','01111'],
  E: ['11111','10000','10000','11110','10000','10000','11111'],
  I: ['11111','00100','00100','00100','00100','00100','11111'],
  L: ['10000','10000','10000','10000','10000','10000','11111'],
  N: ['10001','11001','10101','10011','10001','10001','10001'],
  O: ['01110','10001','10001','10001','10001','10001','01110'],
  P: ['11110','10001','10001','11110','10000','10000','10000'],
  R: ['11110','10001','10001','11110','10100','10010','10001'],
  S: ['01111','10000','10000','01110','00001','00001','11110'],
  T: ['11111','00100','00100','00100','00100','00100','00100'],
};

const message = 'CANON LBP2900 S805 TEST';
const scale = 16;
const textX = 900;
const textY = 700;

function makeLine(y) {
  const line = Buffer.alloc(width, 255);
  if ((y >= 500 && y < 512) || (y >= 1288 && y < 1300)) {
    line.fill(0, 500, 4260);
  }
  if (y >= 500 && y < 1300) {
    line.fill(0, 500, 512);
    line.fill(0, 4248, 4260);
  }
  if (y >= textY && y < textY + 7 * scale) {
    const row = Math.floor((y - textY) / scale);
    for (let c = 0; c < message.length; c++) {
      const glyph = font[message[c]] || font[' '];
      for (let x = 0; x < 5; x++) {
        if (glyph[row][x] === '1') {
          const left = textX + (c * 6 + x) * scale;
          line.fill(0, left, left + scale);
        }
      }
    }
  }
  if (y >= 1000 && y < 1040) line.fill(0, 900, 1800);
  if (y >= 1080 && y < 1120) line.fill(85, 900, 2400);
  if (y >= 1160 && y < 1200) line.fill(170, 900, 3000);
  return line;
}

function encodePixels(line) {
  const out = [];
  let last = line.length - 1;
  while (last >= 0 && line[last] === 255) last--;
  let i = 0;
  while (i <= last) {
    const value = line[i];
    let j = i + 1;
    while (j <= last && j - i < 128 && line[j] === value) j++;
    if (j - i >= 2) {
      out.push(j - i - 1, value);
      i = j;
      continue;
    }
    j = i + 1;
    while (j <= last && j - i < 128 && !(j < last && line[j] === line[j + 1])) j++;
    const n = j - i;
    out.push(n === 1 ? 0 : 257 - n);
    for (let k = i; k < j; k++) out.push(line[k]);
    i = j;
  }
  if (last < line.length - 1) out.push(128);
  return Buffer.from(out);
}

function makeUrf() {
  const chunks = [];
  const fileHeader = Buffer.alloc(12);
  fileHeader.write('UNIRAST\0', 0, 'binary');
  fileHeader.writeUInt32BE(1, 8);
  const pageHeader = Buffer.alloc(32);
  pageHeader[0] = 8;
  pageHeader[1] = 0;
  pageHeader[2] = 1;
  pageHeader[3] = 4;
  pageHeader.writeUInt32BE(width, 12);
  pageHeader.writeUInt32BE(height, 16);
  pageHeader.writeUInt32BE(dpi, 20);
  chunks.push(fileHeader, pageHeader);

  let previous = makeLine(0);
  let repeat = 0;
  const emit = () => chunks.push(Buffer.from([repeat]), encodePixels(previous));
  for (let y = 1; y < height; y++) {
    const line = makeLine(y);
    if (line.equals(previous) && repeat < 255) {
      repeat++;
    } else {
      emit();
      previous = line;
      repeat = 0;
    }
  }
  emit();
  return Buffer.concat(chunks);
}

function ippAttribute(tag, name, value) {
  const n = Buffer.from(name);
  const v = Buffer.from(value);
  const out = Buffer.alloc(1 + 2 + n.length + 2 + v.length);
  let p = 0;
  out[p++] = tag;
  out.writeUInt16BE(n.length, p); p += 2;
  n.copy(out, p); p += n.length;
  out.writeUInt16BE(v.length, p); p += 2;
  v.copy(out, p);
  return out;
}

function makeIpp(urf) {
  const header = Buffer.from([2, 0, 0, 2, 0, 0, 0, 1, 1]);
  return Buffer.concat([
    header,
    ippAttribute(0x47, 'attributes-charset', 'utf-8'),
    ippAttribute(0x48, 'attributes-natural-language', 'en'),
    ippAttribute(0x45, 'printer-uri', `ipp://${host}:631/ipp/print`),
    ippAttribute(0x42, 'requesting-user-name', 'Hardware test'),
    ippAttribute(0x42, 'job-name', 'S805 Canon LBP2900 hardware test'),
    ippAttribute(0x49, 'document-format', 'image/urf'),
    Buffer.from([3]),
    urf,
  ]);
}

const urf = makeUrf();
const body = makeIpp(urf);
console.log(`Sending ${urf.length} byte URF A4 test page to ${host}...`);
const response = await fetch(`http://${host}:631/ipp/print`, {
  method: 'POST',
  headers: { 'Content-Type': 'application/ipp' },
  body,
});
const reply = Buffer.from(await response.arrayBuffer());
if (reply.length < 8) throw new Error(`Short IPP response (${reply.length} bytes)`);
const status = reply.readUInt16BE(2);
const requestId = reply.readUInt32BE(4);
console.log(`HTTP ${response.status}; IPP status=0x${status.toString(16).padStart(4, '0')}; request-id=${requestId}; reply=${reply.length} bytes`);
if (!response.ok || status >= 0x0100) process.exitCode = 1;
