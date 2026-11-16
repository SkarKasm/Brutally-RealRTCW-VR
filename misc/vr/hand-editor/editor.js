import * as THREE from 'three';
import {OrbitControls} from './vendor/OrbitControls.js';
import {TransformControls} from './vendor/TransformControls.js';
// RealRTCW VR Hand Editor. Positions are kept in the game's model units (x forward, y left,
// z up) and shown in centimetres at the size VR draws the gun.
const $=id=>document.getElementById(id), clone=v=>structuredClone(v);
const digits=['thumb','index','middle','ring','little'];
const freshHand=()=>({translation:[0,0,0],rotationDeg:[0,0,0],scale:1,sleeveFollows:true,fingers:Object.fromEntries(digits.map(d=>[d,[0,0,0,0,0]]))});
let token,revision,documentEdits,weaponData,weaponId,side='trigger',dirty=false,busy=false,stopped=false;
let undo=[],redo=[],previewSerial=0,previewTimer,loadingWeapon=0,suppressGizmo=false,cm=1;
const status=(message,error=false)=>{$('status').textContent=message;$('status').classList.toggle('error',error);};
function markDirty(){dirty=true;$('save-state').textContent='Unsaved changes';$('save-state').style.color='#f4bd70';}
function remember(){undo.push(JSON.stringify(documentEdits));if(undo.length>150)undo.shift();redo=[];}
function change(fn){remember();fn();markDirty();refreshControls();schedulePreview();}
function weaponEdit(){return documentEdits.weapons[weaponId]??={weaponScale:1,hands:{}};}
function handEdit(){return weaponEdit().hands[side]??=freshHand();}
async function api(path,data){
 const options=data===undefined?{}:{method:'POST',headers:{'Content-Type':'application/json','X-Editor-Token':token},body:JSON.stringify(data)};
 const response=await fetch('/api/'+path,options);const result=await response.json();
 if(!response.ok)throw new Error(result.error||'Request failed');return result;
}
function fail(e){status(e.message||String(e),true);console.error(e);}
window.addEventListener('error',e=>fail(e.error||new Error(e.message)));
window.addEventListener('unhandledrejection',e=>fail(e.reason));
const viewport=$('viewport'),scene=new THREE.Scene();scene.background=new THREE.Color('#151b23');
const camera=new THREE.PerspectiveCamera(42,1,.05,20000);
const renderer=new THREE.WebGLRenderer({antialias:true,preserveDrawingBuffer:true});
renderer.setPixelRatio(Math.min(devicePixelRatio,2));viewport.appendChild(renderer.domElement);
renderer.outputColorSpace=THREE.SRGBColorSpace;
scene.add(new THREE.HemisphereLight(0xe0edff,0x596475,2));
for(const [pos,power] of [[[30,70,90],2.2],[[-60,30,-70],1.5]]){const light=new THREE.DirectionalLight(0xffffff,power);light.position.set(...pos);scene.add(light);}
const orbit=new OrbitControls(camera,renderer.domElement);orbit.enableDamping=true;orbit.dampingFactor=.12;orbit.minDistance=.5;orbit.maxDistance=2000;
let grid=new THREE.GridHelper(200,20,0x52647d,0x2c394c);scene.add(grid);
const objects=new THREE.Group();scene.add(objects);
const meshes={},sleeves={},skeletons={};let weaponMesh;
const rootHandle=new THREE.Object3D();scene.add(rootHandle);
const gizmo=new TransformControls(camera,renderer.domElement);gizmo.setSize(.85);gizmo.setSpace('world');gizmo.attach(rootHandle);scene.add(gizmo.getHelper());
// game space (x forward, y left, z up) <-> three (y up)
const C=new THREE.Matrix4().set(1,0,0,0, 0,0,1,0, 0,-1,0,0, 0,0,0,1),Ci=C.clone().transpose();
const toThree=p=>[p[0],p[2],-p[1]],fromThree=p=>[p.x,-p.z,p.y];
const positions=points=>new Float32Array(points.flatMap(toThree));
function newMesh(points,triangles,color,opts={}){
 const geometry=new THREE.BufferGeometry();geometry.setAttribute('position',new THREE.BufferAttribute(positions(points),3));geometry.setIndex(triangles.flat());geometry.computeVertexNormals();
 const mesh=new THREE.Mesh(geometry,new THREE.MeshStandardMaterial({color,roughness:.85,metalness:.08,side:THREE.DoubleSide,...opts}));objects.add(mesh);return mesh;
}
function updateMesh(mesh,points){
 if(!mesh)return;const a=positions(points);
 mesh.geometry.attributes.position.array.set(a);mesh.geometry.attributes.position.needsUpdate=true;
 mesh.geometry.computeVertexNormals();mesh.geometry.computeBoundingSphere();mesh.geometry.computeBoundingBox();
}
function disposeObject(o){objects.remove(o);o.geometry?.dispose();o.material?.dispose();}
function skeleton(label,joints){
 if(skeletons[label])disposeObject(skeletons[label]);
 const points=[],colors=[];
 for(const chain of joints){
  const c=new THREE.Color(label===side&&chain.finger===$('finger').value?0xffef6e:0xe4edf8);
  for(let i=1;i<chain.points.length;i++){points.push(...toThree(chain.points[i-1]),...toThree(chain.points[i]));colors.push(c.r,c.g,c.b,c.r,c.g,c.b);}
 }
 const g=new THREE.BufferGeometry();g.setAttribute('position',new THREE.Float32BufferAttribute(points,3));g.setAttribute('color',new THREE.Float32BufferAttribute(colors,3));
 const lines=new THREE.LineSegments(g,new THREE.LineBasicMaterial({vertexColors:true,depthTest:false,transparent:true,opacity:.95}));lines.renderOrder=10;objects.add(lines);skeletons[label]=lines;
}
function viewSettings(){
 if(!weaponMesh)return;
 weaponMesh.material.wireframe=$('wire').checked;weaponMesh.material.transparent=$('transparent').checked;
 weaponMesh.material.opacity=$('transparent').checked?.28:1;weaponMesh.material.depthWrite=!$('transparent').checked;
 weaponMesh.visible=$('show-weapon').checked;
 for(const label of ['trigger','support']){
  const shown=label===side||$('other-hand').checked;
  if(meshes[label]){meshes[label].material.wireframe=$('wire').checked;meshes[label].visible=shown;}
  if(sleeves[label])sleeves[label].visible=shown&&$('show-sleeves').checked;
  if(skeletons[label])skeletons[label].visible=$('skeleton').checked&&shown;
 }
}
function syncGizmo(){
 if(!weaponData||gizmo.dragging||!weaponData.hands[side])return;
 const base=weaponData.hands[side].palm,edit=handEdit();suppressGizmo=true;
 rootHandle.position.set(...toThree(base.map((v,i)=>v+edit.translation[i])));
 const r=new THREE.Matrix4().makeRotationFromEuler(new THREE.Euler(...edit.rotationDeg.map(THREE.MathUtils.degToRad),'XYZ'));
 rootHandle.quaternion.setFromRotationMatrix(C.clone().multiply(r).multiply(Ci));suppressGizmo=false;
}
gizmo.addEventListener('dragging-changed',event=>{orbit.enabled=!event.value;if(event.value)remember();else{markDirty();schedulePreview();}});
gizmo.addEventListener('objectChange',()=>{
 if(suppressGizmo||!gizmo.dragging||!weaponData)return;
 const edit=handEdit(),base=weaponData.hands[side].palm;
 edit.translation=fromThree(rootHandle.position).map((v,i)=>Math.max(-60,Math.min(60,v-base[i])));
 const r=new THREE.Matrix4().makeRotationFromQuaternion(rootHandle.quaternion);const native=Ci.clone().multiply(r).multiply(C);
 const e=new THREE.Euler().setFromRotationMatrix(native,'XYZ');edit.rotationDeg=[e.x,e.y,e.z].map(THREE.MathUtils.radToDeg);
 markDirty();refreshControls();schedulePreview();
});
const controls=[];
function slider(parent,label,min,max,step,get,set){
 const row=document.createElement('div');row.className='control';const name=document.createElement('span');name.textContent=label;
 const number=document.createElement('input');number.type='number';number.min=min;number.max=max;number.step=step;
 const range=document.createElement('input');range.type='range';range.min=min;range.max=max;range.step=step;
 row.append(name,number,range);parent.append(row);
 const update=()=>{const v=get();number.value=Number(v.toFixed(2));range.value=v;};controls.push(update);
 let active=false;
 const edit=event=>{const value=Number(event.target.value);if(!Number.isFinite(value))return;if(!active){remember();active=true;}
  set(Math.min(max,Math.max(min,value)));markDirty();update();schedulePreview();};
 for(const input of [number,range]){input.addEventListener('input',edit);input.addEventListener('change',()=>{active=false;syncGizmo();});}
 update();
}
function createControls(){
 controls.length=0;for(const id of ['weapon-size','root-controls','finger-controls'])$(id).replaceChildren();
 slider($('weapon-size'),'Weapon size · %',25,300,1,()=>weaponEdit().weaponScale*100,v=>weaponEdit().weaponScale=v/100);
 const root=$('root-controls');
 for(const [i,label] of ['Forward / back · cm','Left / right · cm','Up / down · cm'].entries())
  slider(root,label,-40,40,.1,()=>handEdit().translation[i]*cm,v=>handEdit().translation[i]=v/cm);
 for(const [i,label] of ['Roll · degrees','Pitch · degrees','Yaw · degrees'].entries())slider(root,label,-180,180,1,()=>handEdit().rotationDeg[i],v=>handEdit().rotationDeg[i]=v);
 slider(root,'Hand size · %',40,200,1,()=>handEdit().scale*100,v=>handEdit().scale=v/100);
 for(const [i,label] of ['Base joint bend · °','Middle joint bend · °','Tip joint bend · °','Base joint splay · °','Finger roll · °'].entries())
  slider($('finger-controls'),label,-150,150,1,()=>handEdit().fingers[$('finger').value][i],v=>handEdit().fingers[$('finger').value][i]=v);
 refreshControls();
}
function refreshControls(){for(const update of controls)update();$('undo').disabled=!undo.length;$('redo').disabled=!redo.length;
 if(weaponData&&weaponData.hands[side])$('sleeve').checked=handEdit().sleeveFollows!==false;updateDimensions();}
function schedulePreview(){clearTimeout(previewTimer);previewTimer=setTimeout(()=>preview().catch(fail),70);}
async function preview(){
 if(!weaponData)return;const serial=++previewSerial,id=weaponId;
 syncGizmo();
 for(const label of Object.keys(weaponData.hands)){
  const edit=weaponEdit().hands[label]||freshHand();
  const data=await api('preview',{id,side:label,edit});
  if(serial!==previewSerial||id!==weaponId)return;
  updateMesh(meshes[label],data.points);updateMesh(sleeves[label],data.arm);skeleton(label,data.joints);
 }
 viewSettings();
}
function updateDimensions(){
 if(!weaponData)return;
 const points=weaponData.weapon.points,scale=weaponEdit().weaponScale*cm;
 if(!points.length){$('dimensions').textContent='';return;}
 let lo=[Infinity,Infinity,Infinity],hi=[-Infinity,-Infinity,-Infinity];for(const p of points)for(let i=0;i<3;i++){lo[i]=Math.min(lo[i],p[i]);hi[i]=Math.max(hi[i],p[i]);}
 $('dimensions').textContent='In VR, about: length × width × height '+hi.map((v,i)=>((v-lo[i])*scale).toFixed(0)).join(' × ')+' cm';
}
async function selectWeapon(id){
 const request=++loadingWeapon;previewSerial++;weaponId=id;$('loading').hidden=false;
 const data=await api('weapon/'+encodeURIComponent(id));if(request!==loadingWeapon)return;weaponData=data;cm=data.cmPerUnit||1;
 const u=new URL(location.href);u.searchParams.set('weapon',id);history.replaceState(null,'',u);
 for(const child of [...objects.children])disposeObject(child);for(const k of Object.keys(meshes))delete meshes[k];for(const k of Object.keys(sleeves))delete sleeves[k];for(const k of Object.keys(skeletons))delete skeletons[k];
 weaponMesh=newMesh(data.weapon.points,data.weapon.triangles,0xacb7c8);
 for(const [label,hand] of Object.entries(data.hands)){
  meshes[label]=newMesh(hand.points,hand.triangles,label==='trigger'?0xd8943c:0x319dc5);
  if(hand.arm.length)sleeves[label]=newMesh(hand.arm,hand.armTriangles,0x6d7a5c,{transparent:true,opacity:.75});
 }
 if(!data.hands[side])side=data.hands.trigger?'trigger':'support';
 $('trigger').disabled=!data.hands.trigger;$('support').disabled=!data.hands.support;
 for(const label of ['trigger','support'])$(label).classList.toggle('active',label===side);
 $('shared').hidden=!data.shared.length;$('shared').textContent=data.shared.length?`These hands are the same model files as ${data.shared.map(s=>s.replace('.weap','')).join(', ')}: edits here change that weapon too.`:'';
 // grid: 10 cm squares at VR size, under the gun
 scene.remove(grid);const sq=10/cm;grid=new THREE.GridHelper(sq*20,20,0x52647d,0x2c394c);scene.add(grid);
 createControls();await preview();
 const box=boxForAll();if(!box.isEmpty())grid.position.y=box.min.y-1;
 frameAll();$('loading').hidden=true;status(data.name+' · edit the orange trigger hand or the blue support hand');
}
function frameBox(box,direction=[.8,.55,1]){
 const centre=box.getCenter(new THREE.Vector3()),size=box.getSize(new THREE.Vector3());
 const distance=Math.max(size.length(),4)/2/Math.tan(THREE.MathUtils.degToRad(camera.fov/2))*1.25;
 camera.up.set(0,1,0);if(Math.abs(direction[1])>.99)camera.up.set(0,0,-1);
 camera.position.copy(centre).add(new THREE.Vector3(...direction).normalize().multiplyScalar(distance));orbit.target.copy(centre);camera.lookAt(centre);orbit.update();
}
function boxForAll(){const box=new THREE.Box3();for(const child of objects.children)if(child.isMesh&&child.visible)box.expandByObject(child);return box;}
function frameAll(){if(weaponData)frameBox(boxForAll());}
for(const button of document.querySelectorAll('[data-view]'))button.onclick=()=>{
 if(button.dataset.view==='eye'){
  // the flat game's first person camera: at the origin of the hands model, looking forward
  camera.up.set(0,1,0);camera.position.set(-2,1,0);const box=boxForAll();const c=box.getCenter(new THREE.Vector3());orbit.target.copy(c);camera.lookAt(c);orbit.update();return;
 }
 const directions={angle:[.8,.55,1],left:[0,0,-1],right:[0,0,1],front:[1,0,0],back:[-1,0,0],top:[0,1,0],bottom:[0,-1,0]};frameBox(boxForAll(),directions[button.dataset.view]);
};
$('frame-all').onclick=frameAll;$('frame-hand').onclick=()=>meshes[side]&&frameBox(new THREE.Box3().setFromObject(meshes[side]));
$('move').onclick=()=>{gizmo.enabled=true;gizmo.getHelper().visible=true;gizmo.setMode('translate');gizmo.setSpace('world');};
$('rotate').onclick=()=>{gizmo.enabled=true;gizmo.getHelper().visible=true;gizmo.setMode('rotate');gizmo.setSpace('local');};
$('hide-gizmo').onclick=()=>{gizmo.enabled=false;gizmo.getHelper().visible=false;};
for(const label of ['trigger','support'])$(label).onclick=()=>{if(!weaponData?.hands[label])return;side=label;for(const s of ['trigger','support'])$(s).classList.toggle('active',s===side);refreshControls();syncGizmo();schedulePreview();};
$('weapon').onchange=e=>selectWeapon(e.target.value).catch(fail);
$('finger').onchange=()=>{refreshControls();schedulePreview();};
$('sleeve').onchange=()=>change(()=>handEdit().sleeveFollows=$('sleeve').checked);
for(const id of ['wire','transparent','skeleton','other-hand','show-weapon','show-sleeves'])$(id).onchange=viewSettings;
$('reset-hand').onclick=()=>change(()=>weaponEdit().hands[side]=freshHand());
$('reset-finger').onclick=()=>change(()=>handEdit().fingers[$('finger').value]=[0,0,0,0,0]);
$('reset-weapon').onclick=()=>change(()=>documentEdits.weapons[weaponId]={weaponScale:1,hands:{}});
function restoreHistory(source,target){if(!source.length)return;target.push(JSON.stringify(documentEdits));documentEdits=JSON.parse(source.pop());markDirty();refreshControls();schedulePreview();}
$('undo').onclick=()=>restoreHistory(undo,redo);$('redo').onclick=()=>restoreHistory(redo,undo);
async function save(){const result=await api('save',{edits:documentEdits,revision});revision=result.revision;dirty=false;$('save-state').textContent='Saved';$('save-state').style.color='#93d7bb';status(result.message);}
$('save').onclick=()=>save().catch(fail);
$('build').onclick=async()=>{try{await save();await api('build',{});busy=true;status('Building saved poses…');$('log').hidden=false;}catch(e){fail(e);}};
$('install').onclick=async()=>{try{if(dirty)throw new Error('Save and build your latest changes before installing');await api('install',{});busy=true;status('Installing…');$('log').hidden=false;}catch(e){fail(e);}};
$('uninstall').onclick=async()=>{try{if(!confirm('Take the installed hand edits out of the game? Your saved poses stay here.'))return;await api('uninstall',{});busy=true;status('Removing…');$('log').hidden=false;}catch(e){fail(e);}};
$('export').onclick=()=>download(new Blob([JSON.stringify(documentEdits,null,2)],{type:'application/json'}),'realrtcw-vr-hand-poses.json');
$('import').onclick=()=>$('import-file').click();
$('import-file').onchange=async e=>{try{const file=e.target.files[0];if(!file)return;if(file.size>1024*1024)throw new Error('Preset is too large');
 const validated=await api('validate',JSON.parse(await file.text()));
 remember();documentEdits=validated;markDirty();refreshControls();await preview();status('Preset imported. Save when ready.');
 }catch(error){fail(error);}finally{e.target.value='';}};
function download(blob,name){const url=URL.createObjectURL(blob),a=document.createElement('a');a.href=url;a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);}
$('screenshot').onclick=()=>{renderer.render(scene,camera);renderer.domElement.toBlob(blob=>download(blob,`realrtcw-${weaponId}-${side}.png`));};
$('toggle-log').onclick=()=>$('log').hidden=!$('log').hidden;
$('stop-editor').onclick=async()=>{try{if(dirty)throw new Error('Save your poses before stopping the editor');await api('shutdown',{});stopped=true;status('Editor stopped. You can close this tab.');}catch(e){fail(e);}};
window.addEventListener('keydown',e=>{
 if((e.ctrlKey||e.metaKey)&&e.key.toLowerCase()==='s'){e.preventDefault();save().catch(fail);return;}
 if((e.ctrlKey||e.metaKey)&&e.key.toLowerCase()==='z'){e.preventDefault();(e.shiftKey?$('redo'):$('undo')).click();return;}
 if(['INPUT','SELECT','TEXTAREA'].includes(document.activeElement.tagName))return;
 const key=e.key.toLowerCase();if(key==='g')$('move').click();if(key==='r')$('rotate').click();if(key==='f')frameAll();
});
window.addEventListener('beforeunload',e=>{if(dirty){e.preventDefault();e.returnValue='';}});
new ResizeObserver(()=>{const w=viewport.clientWidth,h=viewport.clientHeight;renderer.setSize(w,h);camera.aspect=w/h;camera.updateProjectionMatrix();}).observe(viewport);
renderer.setAnimationLoop(()=>{orbit.update();renderer.render(scene,camera);});
async function poll(){if(stopped)return;try{const job=await api('status');$('log').textContent=job.log;
 if(job.running||busy){status(job.message,!job.running&&job.log.includes('Traceback'));}
 busy=job.running;for(const id of ['build','install','uninstall','save'])$(id).disabled=busy;
 }catch(e){status('Editor stopped. Reopen "Start Hand Editor.cmd".',true);}setTimeout(poll,1500);}
try{
 const state=await api('state');token=state.token;revision=state.revision;documentEdits=state.edits;
 for(const w of state.catalog){const option=document.createElement('option');option.value=w.id;option.textContent=w.name+(w.hands.length>1?'':' (one hand)');$('weapon').append(option);}
 const requested=new URLSearchParams(location.search).get('weapon');
 const initial=state.catalog.find(w=>w.id===requested)||state.catalog.find(w=>w.id==='mp40.weap')||state.catalog[0];$('weapon').value=initial.id;
 await selectWeapon(initial.id);poll();
}catch(e){$('loading').textContent='Cannot load the editor: '+e.message;fail(e);}
