// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only validator. Does not encode, repair, concatenate or upload recordings.
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';

function need(condition, message) { if (!condition) throw new Error(message); }
function integer(x, name) {
  need(Number.isSafeInteger(x) && x >= 0, `Invalid nonnegative integer: ${name}`);
  return x;
}
export function nals(buffer) {
  const headers = [];
  for (let i = 0; i + 3 < buffer.length; i++) {
    if (buffer[i] || buffer[i + 1]) continue;
    let h = i + 2;
    if (buffer[h] === 0) h++;
    if (h + 1 < buffer.length && buffer[h] === 1) {
      headers.push({start: i, h: h + 1}); i = h;
    }
  }
  return headers.map((v, i) => buffer.subarray(v.h, headers[i + 1]?.start ?? buffer.length));
}
// Decode SPS dimensions only; this is not a video decoder or a quality test.
export function spsDimensions(nal) {
  need((nal[0] & 31) === 7, 'Not an SPS');
  const bytes = [];
  for (let i = 1; i < nal.length; i++) {
    if (i >= 3 && nal[i] === 3 && nal[i - 1] === 0 && nal[i - 2] === 0) continue;
    bytes.push(nal[i]);
  }
  let bit = 0;
  function bits(n) {
    need(n <= 32 && bit + n <= bytes.length * 8, 'Truncated SPS');
    let v = 0;
    while (n--) { v = v * 2 + ((bytes[bit >> 3] >> (7 - (bit & 7))) & 1); bit++; }
    return v;
  }
  function ue() {
    let zeros = 0;
    while (bits(1) === 0) need(++zeros < 31, 'SPS Exp-Golomb overflow');
    return 2 ** zeros - 1 + bits(zeros);
  }
  function se() { const v = ue(); return v & 1 ? (v + 1) / 2 : -v / 2; }
  const profile = bits(8); bits(16); ue();
  let chroma = 1, separate = 0;
  if ([100,110,122,244,44,83,86,118,128,138,139,134,135].includes(profile)) {
    chroma = ue(); need(chroma <= 3, 'Invalid SPS chroma');
    if (chroma === 3) separate = bits(1);
    ue(); ue(); bits(1);
    if (bits(1)) for (let i = 0; i < (chroma === 3 ? 12 : 8); i++) {
      if (!bits(1)) continue;
      let last = 8, next = 8;
      for (let j = 0; j < (i < 6 ? 16 : 64); j++) {
        if (next !== 0) next = (last + se() + 256) % 256;
        last = next === 0 ? last : next;
      }
    }
  }
  ue();
  const order = ue();
  if (order === 0) ue();
  else if (order === 1) {
    bits(1); se(); se();
    const cycle = ue(); need(cycle < 256, 'Invalid SPS cycle');
    for (let i = 0; i < cycle; i++) se();
  } else need(order === 2, 'Invalid SPS picture order');
  ue(); bits(1);
  const w = ue() + 1, h = ue() + 1, frame = bits(1);
  if (!frame) bits(1);
  bits(1);
  let left = 0, right = 0, top = 0, bottom = 0;
  if (bits(1)) { left = ue(); right = ue(); top = ue(); bottom = ue(); }
  const arrayType = separate ? 0 : chroma;
  const cropX = arrayType === 0 || arrayType === 3 ? 1 : 2;
  const cropY = (arrayType === 1 ? 2 : 1) * (2 - frame);
  const width = 16 * w - cropX * (left + right);
  const height = 16 * h * (2 - frame) - cropY * (top + bottom);
  need(width > 0 && height > 0 && width <= 16384 && height <= 16384, 'Invalid SPS dimensions');
  return {width, height, progressive: !!frame};
}
export function checkCapture(directory) {
  const errors = [], warnings = [];
  const file = name => path.join(directory, name);
  const summary = JSON.parse(fs.readFileSync(file('summary.json'), 'utf8'));
  need(summary.format === 'lite-recorder-probe-1', 'Unknown recording format');
  for (const key of ['state','fault','result','video_bytes','audio_bytes','index_bytes',
    'video_packets','audio_packets','video_span_us','nal_mask']) integer(summary[key], key);
  const csv = fs.readFileSync(file('packets.csv'));
  need(csv.length === summary.index_bytes, 'Index file size differs from summary');
  need(csv.toString().endsWith('\n'), 'Truncated index line');
  const rows = csv.toString('utf8').trimEnd().split('\n');
  need(rows.shift() === 'stream,timestamp_us,frames,offset,length', 'Unknown index columns');
  const stats = {
    V: {bytes: 0, packets: 0, first: null, last: null},
    A: {bytes: 0, packets: 0, first: null, last: null}
  };
  const types = new Set(), dimensions = new Map();
  let mask = 0n;
  function inspect(data, inVideo) {
    const units = nals(data);
    if (inVideo) need(units.length > 0, 'Video packet has no Annex-B start code');
    for (const nal of units) {
      need(!(nal[0] & 128), 'H.264 forbidden_zero_bit is set');
      const type = nal[0] & 31;
      types.add(type);
      if (inVideo) mask |= 1n << BigInt(type);
      if (type === 7) {
        try { const d = spsDimensions(nal); dimensions.set(`${d.width}x${d.height}`, d); }
        catch (e) { errors.push(`SPS: ${e.message}`); }
      }
    }
  }
  const fd = fs.openSync(file('video.h264'), 'r');
  try {
    for (const [index, row] of rows.entries()) {
      need(/^[VA],\d+,\d+,\d+,\d+$/.test(row), `Malformed index row ${index + 2}`);
      const [stream, ...values] = row.split(',');
      const [timestamp, frames, offset, length] = values.map((v, i) => integer(Number(v), `row ${index + 2} column ${i + 2}`));
      const st = stats[stream];
      need(offset === st.bytes, `${stream} missing, duplicate or overlapping packet at ${offset}`);
      need(length > 0 && length <= (stream === 'V' ? 0x80000 : 0x1000), 'Invalid packet length');
      need(st.last === null || timestamp >= st.last, `${stream} timestamps regressed`);
      if (stream === 'A') need(length % 4 === 0, 'PCM packet is not sample-aligned');
      else {
        const data = Buffer.alloc(length);
        need(fs.readSync(fd, data, 0, length, offset) === length, 'Video file is truncated');
        inspect(data, true);
      }
      st.first ??= timestamp; st.last = timestamp; st.bytes += length; st.packets++;
      void frames; // GRC-reported quantity is retained; never assume it equals decoded FPS.
    }
  } finally { fs.closeSync(fd); }
  for (const [stream, prefix, name] of [['V','video','video.h264'],['A','audio','audio.pcm']]) {
    const st = stats[stream], actual = fs.statSync(file(name)).size;
    need(actual === st.bytes && actual === summary[`${prefix}_bytes`], `${prefix} file/index/summary size mismatch`);
    need(st.packets === summary[`${prefix}_packets`], `${prefix} packet count mismatch`);
    if (!st.packets) errors.push(`No ${prefix} packets`);
  }
  need(mask === BigInt(summary.nal_mask), 'Video NAL mask differs from summary');
  need(summary.video_span_us === (stats.V.last ?? 0) - (stats.V.first ?? 0), 'Video duration differs from timestamps');
  if (fs.existsSync(file('codec.h264'))) {
    need(fs.statSync(file('codec.h264')).size <= 8192, 'Codec sidecar is too large');
    inspect(fs.readFileSync(file('codec.h264')), false);
  }
  if (!types.has(7) || !types.has(8)) errors.push('Missing captured SPS/PPS; restart console and test before launching the game');
  if (!types.has(5)) errors.push('No IDR keyframe captured');
  if (summary.state !== 3 || summary.fault !== 0 || summary.result !== 0) errors.push(`Probe ended with state=${summary.state}, fault=${summary.fault}, result=0x${summary.result.toString(16)}`);
  if (dimensions.size !== 1) errors.push('Expected one consistent SPS resolution');
  const resolution = [...dimensions.values()][0] ?? null;
  if (resolution && (resolution.width !== 1280 || resolution.height !== 720)) warnings.push('Captured resolution is not 1280x720');
  const span = summary.video_span_us / 1e6;
  const audioSeconds = stats.A.bytes / 192000;
  if (span < 100) warnings.push('Video span is shorter than 100 seconds; sustained capture is not established');
  if (Math.abs(audioSeconds - span) > 2) warnings.push('Audio sample duration differs from video span by more than 2 seconds');
  warnings.push('Packet rate is not decoded frame rate. 30 fps, visual quality and A/V sync still require decoding/device validation.');
  warnings.push('codec.h264 holds the latest observed parameter sets; test only one game per boot.');
  return {transportIntegrity: errors.length ? 'FAIL' : 'PASS',
    hardwareTarget: 'UNVERIFIED', errors, warnings, resolution,
    stopReason: summary.stop_reason ?? 'not_recorded_by_this_version',
    elapsedAfterFirstVideoSeconds: summary.elapsed_after_first_video_us === undefined ? null : summary.elapsed_after_first_video_us / 1e6,
    videoSecondsBetweenFirstAndLastPacket: span, audioSeconds,
    videoPacketsPerSecond: span ? (stats.V.packets - 1) / span : null,
    videoBytes: stats.V.bytes, audioBytes: stats.A.bytes, nalTypes: [...types].sort((a,b)=>a-b)};
}
if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  try {
    need(process.argv.length === 3, 'Usage: node check-capture.mjs <session-directory>');
    const report = checkCapture(process.argv[2]);
    console.log(JSON.stringify(report, null, 2));
    if (report.errors.length) process.exitCode = 1;
  } catch (e) { console.error(`Capture validation failed: ${e.message}`); process.exitCode = 1; }
}
