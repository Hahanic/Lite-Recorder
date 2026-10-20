import {test} from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {checkCapture, spsDimensions} from '../tools/check-capture.mjs';

// Construct a baseline-profile 1280x720 SPS from syntax fields.
function ue(n) { const v = (n+1).toString(2); return '0'.repeat(v.length-1)+v; }
const bits = '01000010'+'00000000'+'00011111'+ue(0)+ue(0)+ue(0)+ue(0)+ue(1)+'0'+ue(79)+ue(44)+'1'+'1'+'0'+'0'+'1';
const sps = Buffer.concat([Buffer.from([0x67]), Buffer.from(bits.padEnd(Math.ceil(bits.length/8)*8,'0').match(/.{8}/g).map(x=>parseInt(x,2)))]);
const video = Buffer.concat([Buffer.from([0,0,0,1]),sps,Buffer.from([0,0,1,0x68,0xce,0x38,0x80,0,0,1,0x65,0x88])]);
function fixture(edit, verify) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(),'lite-probe-test-'));
  try {
    const csv = `stream,timestamp_us,frames,offset,length\nV,1000,1,0,${video.length}\nA,1000,1,0,4\n`;
    const summary = {format:'lite-recorder-probe-1',state:3,fault:0,result:0,video_bytes:video.length,audio_bytes:4,
      index_bytes:Buffer.byteLength(csv),video_packets:1,audio_packets:1,video_span_us:0,nal_mask:416};
    fs.writeFileSync(path.join(dir,'video.h264'),video); fs.writeFileSync(path.join(dir,'audio.pcm'),Buffer.alloc(4));
    fs.writeFileSync(path.join(dir,'packets.csv'),csv); fs.writeFileSync(path.join(dir,'summary.json'),JSON.stringify(summary));
    edit?.(dir,summary); verify(dir);
  } finally {
    assert.equal(path.dirname(path.resolve(dir)),path.resolve(os.tmpdir()));
    assert(path.basename(dir).startsWith('lite-probe-test-'));
    fs.rmSync(dir,{recursive:true,force:true});
  }
}
test('SPS resolves 720p without hardcoded parameters',()=>assert.deepEqual(spsDimensions(sps),{width:1280,height:720,progressive:true}));
test('truncated SPS rejected',()=>assert.throws(()=>spsDimensions(Buffer.from([0x67,0x42]))));
test('intact transport does not claim hardware target passed',()=>fixture(null,d=>{
  const r=checkCapture(d); assert.equal(r.transportIntegrity,'PASS'); assert.equal(r.hardwareTarget,'UNVERIFIED');
}));
test('truncated video rejected',()=>fixture(d=>fs.truncateSync(path.join(d,'video.h264'),2),d=>assert.throws(()=>checkCapture(d),/truncated/)));
test('extra audio data rejected',()=>fixture(d=>fs.appendFileSync(path.join(d,'audio.pcm'),Buffer.alloc(4)),d=>assert.throws(()=>checkCapture(d),/size mismatch/)));
test('negative index fields rejected',()=>fixture(d=>{
  const p=path.join(d,'packets.csv'); fs.writeFileSync(p,fs.readFileSync(p,'utf8').replace('V,1000','V,-100'));
},d=>assert.throws(()=>checkCapture(d),/Malformed/)));
test('overlapping offset rejected',()=>fixture(d=>{
  const p=path.join(d,'packets.csv'); fs.writeFileSync(p,fs.readFileSync(p,'utf8').replace('V,1000,1,0','V,1000,1,1'));
},d=>assert.throws(()=>checkCapture(d),/overlapping/)));
test('backend error cannot pass',()=>fixture((d,s)=>{
  s.state=4;s.fault=2;fs.writeFileSync(path.join(d,'summary.json'),JSON.stringify(s));
},d=>assert.equal(checkCapture(d).transportIntegrity,'FAIL')));
test('missing audio cannot pass',()=>fixture((d,s)=>{
  const p=path.join(d,'packets.csv'), csv=fs.readFileSync(p,'utf8').replace('A,1000,1,0,4\n','');
  fs.writeFileSync(p,csv);fs.writeFileSync(path.join(d,'audio.pcm'),Buffer.alloc(0));
  Object.assign(s,{audio_bytes:0,audio_packets:0,index_bytes:Buffer.byteLength(csv)});
  fs.writeFileSync(path.join(d,'summary.json'),JSON.stringify(s));
},d=>assert(checkCapture(d).errors.includes('No audio packets'))));
test('timestamp regression rejected',()=>fixture((d,s)=>{
  const p=path.join(d,'packets.csv'),csv=fs.readFileSync(p,'utf8')+`V,999,1,${video.length},${video.length}\n`;
  fs.writeFileSync(p,csv);fs.appendFileSync(path.join(d,'video.h264'),video);
  Object.assign(s,{video_bytes:video.length*2,video_packets:2,index_bytes:Buffer.byteLength(csv)});
  fs.writeFileSync(path.join(d,'summary.json'),JSON.stringify(s));
},d=>assert.throws(()=>checkCapture(d),/regressed/)));
