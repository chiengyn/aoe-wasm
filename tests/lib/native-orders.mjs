// Packet layouts come from Command.h / tcommand.h. Validate before the native
// executor: the Windows implementation trusts peers and several packet fields.
export function executeNativeOrder(session,bytes,actor){
  if(!Array.isArray(bytes)||bytes.length<1||bytes.length>1024||bytes.some(x=>!Number.isInteger(x)||x<0||x>255))return false;
  const data=Uint8Array.from(bytes),v=new DataView(data.buffer),m=session.m;
  const u8=n=>v.getUint8(n),i16=n=>v.getInt16(n,true),i32=n=>v.getInt32(n,true),f32=n=>v.getFloat32(n,true);
  const owns=id=>Number.isInteger(id)&&id>=0&&m._engine_probe_object_value(id,5)===actor;
  const unit=id=>session.dat.units.find(u=>u.civ===session.civs[actor]&&u.slot===id);
  const point=(x,y)=>Number.isFinite(x)&&Number.isFinite(y)&&x>=0&&y>=0&&x<session.width&&y<session.height;
  const group=(offset,count)=>count>0&&count<=25&&data.length===offset+count*4&&Array.from({length:count},(_,i)=>i32(offset+i*4)).every(owns);
  const seen=id=>{const x=Math.floor(m._engine_probe_object_value(id,1)),y=Math.floor(m._engine_probe_object_value(id,2));return m._engine_mp_visible(actor,x,y)===15;};
  let valid=false;
  try{switch(u8(0)){
    case 0:case 2:case 3:valid=group(20,u8(8))&&point(f32(12),f32(16))&&(i32(4)===-1||seen(i32(4)));break;
    case 1:case 13:valid=group(2,u8(1));break;
    case 6:valid=i32(8)>=0&&i32(8)<=5&&group(12,i32(4));break;
    case 100:case 119:{const queue=u8(0)===119,id=i32(4),type=i16(queue?8:10),u=unit(type);valid=data.length===(queue?12:16)&&owns(id)&&u?.trainAt===m._engine_probe_object_value(id,0)&&!!m._engine_probe_player_unit(actor,type)&&(queue?i16(10)>=-1&&i16(10)<=5:u8(8)===actor&&i32(12)===-1);break;}
    case 101:valid=data.length===16&&owns(i32(4))&&u8(8)===actor&&i32(12)===-1&&m._engine_mp_research(actor,i16(10))===1&&m._engine_mp_research_location(i16(10))===m._engine_probe_object_value(i32(4),0);break;
    case 102:{const type=i16(12);valid=group(20,u8(1))&&u8(2)===actor&&i32(16)===-1&&unit(type)?.type===80&&!!m._engine_probe_player_unit(actor,type)&&point(f32(4),f32(8))&&m._engine_probe_placement(actor,type,f32(4),f32(8))===0;break;}
    case 106:valid=data.length===8&&owns(i32(4));break;
    case 107:case 111:valid=group(12,u8(1))&&point(f32(4),f32(8));break;
    case 110:valid=group(8,u8(1))&&seen(i32(4));break;
    case 11:valid=data.length===4&&u8(1)===actor&&u8(3)===0;break;
  }}catch{return false;}
  if(!valid)return false;
  return !!m.ccall('engine_mp_packet','number',['array','number'],[data,data.length]);
}
