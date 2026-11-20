// Browser and offline tests share this bridge to the recovered C++ simulation.
// This layer reads state and sends commands; it does not simulate units or AI.
export class EngineSession {
  constructor(engine,dat){this.m=engine;this.dat=dat;this.time=0;this.started=false;this.player=1;this.playerCount=2;this.civs={1:1,2:2};}
  startMultiplayer(room,me){
    if(this.started)throw new Error('Each match needs a new engine instance.');
    this.player=me;this.playerCount=room.players.length;
    // Compact room slots into simulation player numbers; room IDs remain stable.
    this.actorMap=Object.fromEntries(room.players.map((p,i)=>[p.slot,i+1]));
    this.player=this.actorMap[me];
    room.settings.forEach((v,i)=>{if(!this.m._engine_mp_option(i,v))throw new Error('Invalid match settings.');});
    room.players.forEach((p,i)=>{this.civs[i+1]=p.civ;if(!this.m.ccall('engine_mp_player','number',['number','string','number','number','number'],[i+1,p.name,p.civ,p.color,p.team]))throw new Error('Invalid player.');});
    this.m._engine_probe_clock(1);
    if(this.m._engine_mp_start(room.seed,room.players.length,this.player)!==1)throw new Error('Could not create the multiplayer match.');
    this.started=true;this.width=this.m._engine_probe_map_info(0);this.height=this.m._engine_probe_map_info(1);return this.map();
  }
  start(seed=12345,computer=true,mapType=4,difficulty=2){
    if(this.started)throw new Error('Each match needs a new engine instance.');
    if(!this.m._engine_probe_options(mapType,difficulty))throw new Error('Invalid match settings.');
    if(this.m._engine_probe_new_game(seed>>>0,computer?1:0)!==1)throw new Error('Could not create the map.');
    this.started=true;this.width=this.m._engine_probe_map_info(0);this.height=this.m._engine_probe_map_info(1);
    return this.map();
  }
  map(){return {width:this.width,height:this.height,halfWidth:this.m._engine_probe_map_info(2),halfHeight:this.m._engine_probe_map_info(3),elevation:this.m._engine_probe_map_info(4),
    tiles:Array.from({length:this.width*this.height},(_,i)=>{const x=i%this.width,y=Math.floor(i/this.width);return {x,y,slp:this.m._engine_probe_ground(x,y,0),frame:this.m._engine_probe_ground(x,y,1),sx:this.m._engine_probe_ground(x,y,2),sy:this.m._engine_probe_ground(x,y,3),terrain:this.m._engine_probe_ground(x,y,4),height:this.m._engine_probe_tile(x,y,1)};})};}
  tick(count=1){if(!Number.isInteger(count)||count<0||count>10000)throw new Error('Invalid time step.');this.time=this.m._engine_probe_tick(count);return this.snapshot();}
  snapshot(){
    const m=this.m,objects=[];
    for(let player=0;player<=this.playerCount;player++)for(let i=0,n=m._engine_probe_objects(player);i<n;i++){
      const id=m._engine_probe_object_id(player,i),v=f=>m._engine_probe_object_value(id,f);
      objects.push({id,master:v(0),x:v(1),y:v(2),hp:v(3),state:v(4),player,group:v(6),held:v(7),resource:v(8),graphic:v(9),frame:v(10),facet:v(11),z:v(12),maxHp:v(13),type:v(14),sx:v(15),sy:v(16),inside:v(17)});
    }
    const player=this.player,visibility=Uint8Array.from({length:this.width*this.height},(_,i)=>m._engine_mp_visible(player,i%this.width,Math.floor(i/this.width)));
    const available=this.dat.units.filter(u=>u.civ===this.civs[player]&&m._engine_probe_player_unit(player,u.slot)).map(u=>u.slot);
    return {time:this.time,objects,visibility,available,resources:[0,1,2,3,4,11,32].map(r=>m._engine_probe_resource(player,r)),players:Array.from({length:this.playerCount},(_,p)=>m._engine_probe_player_state(p+1)),ageResearch:[101,102,103].map(id=>[id,m._engine_mp_research(player,id)])};
  }
  command(command,player=this.player){
    if(!command||!Array.isArray(command.ids)||command.ids.length>200)return {accepted:0,message:'Invalid command.'};
    const m=this.m,ids=command.ids,owned=[...new Set(ids)].filter(id=>Number.isInteger(id)&&m._engine_probe_object_value(id,5)===player&&m._engine_probe_object_value(id,4)<3);
    if(!owned.length)return {accepted:0,message:'Select one of your own units or buildings.'};
    let accepted=0;
    const point=()=>Number.isFinite(command.x)&&Number.isFinite(command.y)&&command.x>=0&&command.y>=0&&command.x<this.width&&command.y<this.height;
    switch(command.kind){
      case 'move': if(point())for(const id of owned)accepted+=m._engine_probe_move(id,command.x,command.y);break;
      case 'work': {
        const target=command.target,x=Math.floor(m._engine_probe_object_value(target,1)),y=Math.floor(m._engine_probe_object_value(target,2));
        if(Number.isInteger(target)&&x>=0&&y>=0&&x<this.width&&y<this.height&&m._engine_mp_visible(player,x,y)===15)
          for(const id of owned)accepted+=m._engine_probe_work(id,target);
        break;
      }
      case 'stop':for(const id of owned)accepted+=m._engine_probe_stop(id);break;
      case 'train': {
        const unit=this.dat.units.find(u=>u.civ===this.civs[player]&&u.slot===command.unit);
        if(unit&&m._engine_probe_player_unit(player,unit.slot)&&m._engine_probe_object_value(owned[0],0)===unit.trainAt)accepted=m._engine_probe_train(owned[0],unit.slot);
        break;
      }
      case 'build': {
        const unit=this.dat.units.find(u=>u.civ===this.civs[player]&&u.slot===command.unit);
        if(unit?.type===80&&m._engine_probe_player_unit(player,unit.slot)&&point()&&m._engine_probe_placement(player,unit.slot,command.x,command.y)===0){
          // One native group packet would be needed for multi-builder formations.
          accepted=m._engine_probe_build(owned[0],unit.slot,command.x,command.y);
        }break;
      }
      case 'research':if([101,102,103].includes(command.tech)&&m._engine_probe_object_value(owned[0],0)===109&&m._engine_mp_research(player,command.tech)===1)accepted=m._engine_probe_research(owned[0],command.tech);break;
      default:return {accepted:0,message:'Command not supported.'};
    }
    return {accepted,message:accepted?'Command issued.':'Cannot do that here, or the requirements are not met.'};
  }
}
export function project(x,y,halfWidth=32,halfHeight=16){return {x:(x+y)*halfWidth,y:(y-x)*halfHeight};}
export function unproject(x,y,halfWidth=32,halfHeight=16){return {x:(x/halfWidth-y/halfHeight)/2,y:(x/halfWidth+y/halfHeight)/2};}
export function visibleObject(object,visibility,width,height){const x=Math.floor(object.x),y=Math.floor(object.y);return object.inside<0&&object.state<7&&x>=0&&y>=0&&x<width&&y<height&&visibility[y*width+x]===15;}
