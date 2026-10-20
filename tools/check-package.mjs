import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
const root = path.resolve(process.argv[2] ?? '.');
const moduleId = 0x00FF4C5250524231n;
const dir = path.join(root,'stage/atmosphere/contents/00FF4C5250524231');
const nsp = fs.readFileSync(path.join(dir,'exefs.nsp'));
assert.equal(nsp.toString('ascii',0,4),'PFS0');
const count = nsp.readUInt32LE(4), stringSize = nsp.readUInt32LE(8);
assert.equal(count,2);
const strings = 16 + 24 * count, data = strings + stringSize;
const entries = new Map();
for (let i=0;i<count;i++) {
  const p=16+24*i, off=Number(nsp.readBigUInt64LE(p)), size=Number(nsp.readBigUInt64LE(p+8));
  const start=strings+nsp.readUInt32LE(p+16), end=nsp.indexOf(0,start);
  assert(end>=start && end<data);
  const name=nsp.toString('utf8',start,end);
  assert(data+off+size<=nsp.length); assert(!entries.has(name));
  entries.set(name,nsp.subarray(data+off,data+off+size));
}
assert.equal(entries.get('main').toString('ascii',0,4),'NSO0');
const npdm=entries.get('main.npdm');
assert.equal(npdm.toString('ascii',0,4),'META');
assert.equal(npdm[0xC]&1,1); // AArch64
assert.equal(npdm[0xE],44); assert.equal(npdm[0xF],3);
assert.equal(npdm.readUInt32LE(0x1C),0x20000);
const aci=npdm.readUInt32LE(0x70), acid=npdm.readUInt32LE(0x78);
assert.equal(npdm.toString('ascii',aci,aci+4),'ACI0');
assert.equal(npdm.readBigUInt64LE(aci+0x10),moduleId);
assert.equal(npdm.toString('ascii',acid+0x200,acid+0x204),'ACID');
assert.equal(npdm.readBigUInt64LE(acid+0x210),moduleId);
assert.equal(npdm.readBigUInt64LE(acid+0x218),moduleId);
function services(start,size) {
  const found=[];
  for(let p=start;p<start+size;) {
    const control=npdm[p++], length=(control&7)+1;
    assert.equal(control&0x80,0,'Unexpected hosted service');
    found.push(npdm.toString('ascii',p,p+length));p+=length;
  }
  return found.sort();
}
const expected=['fsp-srv','set:sys','time:u','grc:d','pm:dmnt'].sort();
assert.deepEqual(services(aci+npdm.readUInt32LE(aci+0x28),npdm.readUInt32LE(aci+0x2c)),expected);
assert.deepEqual(services(acid+npdm.readUInt32LE(acid+0x228),npdm.readUInt32LE(acid+0x22c)),expected);
assert.equal(fs.statSync(path.join(dir,'flags/boot2.flag')).size,0);
const nro=fs.readFileSync(path.join(root,'stage/switch/.overlays/ovl-lite-probe.ovl'));
assert.equal(nro.toString('ascii',0x10,0x14),'NRO0');
assert(nro.readUInt32LE(0x18)<=nro.length);
console.log('Package checks passed: PFS0, NSO, NRO, program ID, ACI0/ACID, service allowlist, boot startup flag.');
