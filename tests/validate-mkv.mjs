// Optional host verification of output produced by recorder-replay.cpp.
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
const root=path.resolve(process.argv[2]);
function run(exe,args) {
  const result=spawnSync(exe,args,{encoding:'utf8',maxBuffer:32*1024*1024});
  assert.equal(result.status,0,`${exe}: ${result.error??result.stderr}`);
  assert.equal(result.stderr.trim(),'');
  return result.stdout;
}
const verified=[];
for(const mode of ['natural','pause','quick-pause','stop-paused','size','time','long','missing','timestamp']) {
  const directory=path.join(root,mode),report=JSON.parse(fs.readFileSync(path.join(directory,'replay.json'),'utf8'));
  const files=fs.readdirSync(directory).filter(n=>n.endsWith('.mkv')).sort();
  assert.equal(files.length,report.parts);
  let frames=0,duration=0,bytes=0;
  for(const name of files) {
    const file=path.join(directory,name);
    const probe=JSON.parse(run('ffprobe',['-v','error','-count_frames','-show_streams','-show_format','-of','json',file]));
    const v=probe.streams.find(s=>s.codec_type==='video'),a=probe.streams.find(s=>s.codec_type==='audio');
    assert.equal(v.codec_name,'h264');assert.equal(v.width,1280);assert.equal(v.height,720);
    assert.equal(a.codec_name,'pcm_s16le');assert.equal(a.sample_rate,'48000');assert.equal(a.channels,2);
    assert.equal(v.has_b_frames,0);
    const packets=JSON.parse(run('ffprobe',['-v','error','-show_packets','-show_entries',
      'packet=stream_index,pts_time,flags','-of','json',file])).packets;
    const first=packets.find(p=>p.stream_index===v.index);
    assert(first.flags.includes('K'));assert.equal(Number(first.pts_time),0);
    if(files.length===1) for(const join of report.joins_us??[]) {
      assert(packets.some(p=>p.stream_index===v.index&&p.flags.includes('K')&&
        Math.abs(Number(p.pts_time)-join/1e6)<=.000051),'Missing keyframe at continuous join');
    }
    for(const index of [v.index,a.index]) {
      let last=-1;
      for(const p of packets.filter(p=>p.stream_index===index)) {
        const pts=Number(p.pts_time);assert(Number.isFinite(pts)&&pts>=0&&pts>=last);last=pts;
      }
    }
    run('ffmpeg',['-hide_banner','-v','error','-xerror','-err_detect','explode','-i',file,
      '-map','0:v:0','-map','0:a:0','-fps_mode','passthrough','-enc_time_base:v','1:1000000','-f','null','-']);
    // Exercise the written seek index, not just sequential playback.
    const seconds=Number(probe.format.duration);
    if(seconds>2) run('ffmpeg',['-hide_banner','-v','error','-xerror','-ss',String(seconds/2),'-i',file,
      '-map','0:v:0','-frames:v','1','-fps_mode','passthrough','-enc_time_base:v','1:1000000','-f','null','-']);
    frames+=Number(v.nb_read_frames);duration+=seconds;bytes+=fs.statSync(file).size;
  }
  assert.equal(frames,report.video_packets);assert.equal(bytes,report.bytes);
  assert(Math.abs(duration-report.duration_us/1e6)<.002);
  verified.push({mode,files:files.length,frames,duration,bytes});
  console.log(`${mode}: verified ${files.length} files, ${frames} decoded frames, ${duration.toFixed(3)} s`);
}
// Save failure preserves the unfinished file; never advertises it as .mkv.
const failed=path.join(root,'rename-failure');
assert(fs.readdirSync(failed).some(n=>n.endsWith('.mkv.part')));
fs.writeFileSync(path.join(root,'validation.json'),JSON.stringify(verified,null,2));
