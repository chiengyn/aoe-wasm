// The room lobby (index.html): a name, then create a room or find one and join it. The game loads in the
// background meanwhile (game-host.mjs) and starts straight in the room (native-room.mjs). Rooms are the
// relay's sessions: GET /api/rooms lists them, POST /api/rooms/check checks a password before the game
// joins (server/rooms.go). native.html stays as the debugging page.
import {MediaHost} from './native-media.mjs';
import {createGameHost, unlockMedia} from './game-host.mjs';
import {unsupportedCharacters} from './cp1252.mjs';
import {ROOM_ERRORS} from './native-room.mjs';

const $ = id => document.getElementById(id);
const NAME_KEY = 'aoe.playerName';
const POLL_MS = 3000;
// The game's own limits: player names (scr_mp.cpp) and game names (scr_cj.cpp).
const NAME_MAX = 15, ROOM_MAX = 40, PASSWORD_MAX = 32;

// localStorage can be missing or throw (private windows, blocked site data): the lobby works without it.
const store = {
  get: key => { try { return localStorage.getItem(key); } catch { return null; } },
  set: (key, value) => { try { localStorage.setItem(key, value); } catch {} },
};

// An error message for text the game can't take, or ''.
function problem(text, what, max) {
  if (!text) return `Enter ${what}.`;
  if ([...text].length > max) return `Use at most ${max} characters for ${what}.`;
  const bad = unsupportedCharacters(text);
  if (bad.length) return `The game can't write ${bad.map(c => `“${c}”`).join(' ')}. Use Latin letters, digits and common punctuation.`;
  return '';
}

// ---- the player's name
let player = (store.get(NAME_KEY) || '').trim();
if (problem(player, 'a name', NAME_MAX)) player = 'Player' + String(Math.floor(Math.random() * 900) + 100);
const nameText = $('name-text'), nameForm = $('name-form'), nameInput = $('name-input'), nameError = $('name-error');
const showName = () => { nameText.textContent = player; };
const editName = editing => {
  nameText.hidden = editing; nameForm.hidden = !editing;
  $('name-edit').hidden = editing; $('name-actions').hidden = !editing;
  nameError.textContent = '';
  if (editing) { nameInput.value = player; nameInput.focus(); nameInput.select(); } else $('name-edit').focus();
};
showName();
$('name-edit').addEventListener('click', () => editName(true));
$('name-cancel').addEventListener('click', () => editName(false));
nameInput.addEventListener('keydown', e => { if (e.key === 'Escape') { e.preventDefault(); editName(false); } });
nameForm.addEventListener('submit', e => {
  e.preventDefault();
  const name = nameInput.value.trim();
  const error = problem(name, 'your name', NAME_MAX);
  if (error) { nameError.textContent = error; nameInput.focus(); return; }
  player = name;
  store.set(NAME_KEY, player);
  showName();
  editName(false);
  if (!roomName.dataset.edited) roomName.value = defaultRoomName();
});

// ---- views
const tabs = [$('tab-join'), $('tab-create')], panels = [$('join'), $('create')];
function showView(index, focus = false) {
  tabs.forEach((tab, i) => {
    tab.setAttribute('aria-selected', String(i === index));
    tab.tabIndex = i === index ? 0 : -1;
    panels[i].hidden = i !== index;
  });
  if (focus) tabs[index].focus();
  if (index === 0) poll(); else roomName.focus();
}
tabs.forEach((tab, i) => {
  tab.addEventListener('click', () => showView(i));
  tab.addEventListener('keydown', e => {
    if (e.key === 'ArrowRight' || e.key === 'ArrowLeft') { e.preventDefault(); showView(1 - i, true); }
  });
});
const notice = $('notice');
const say = text => { notice.textContent = text || ''; notice.hidden = !text; };

// ---- create a room
const roomName = $('room-name'), roomPassword = $('room-password'), createError = $('create-error');
const defaultRoomName = () => `${player}'s room`.slice(0, ROOM_MAX);
roomName.value = defaultRoomName();
roomName.addEventListener('input', () => { roomName.dataset.edited = '1'; });
$('password-show').addEventListener('click', e => {
  const show = roomPassword.type === 'password';
  roomPassword.type = show ? 'text' : 'password';
  e.target.textContent = show ? 'Hide' : 'Show';
  e.target.setAttribute('aria-pressed', String(show));
});
$('create-form').addEventListener('submit', e => {
  e.preventDefault();
  const name = roomName.value.trim(), password = roomPassword.value;
  const error = problem(name, 'the room name', ROOM_MAX) || (password && problem(password, 'the password', PASSWORD_MAX));
  if (error) { createError.textContent = error; (error.includes('password') ? roomPassword : roomName).focus(); return; }
  createError.textContent = '';
  enterRoom({create: true, name, password, locked: !!password});
});

// ---- find and join a room
const list = $('rooms'), empty = $('rooms-empty'), search = $('search'), rowTemplate = $('room-row');
const rows = new Map();   // room id -> <li>
let rooms = [], loaded = false, failedPolls = 0, pollTimer = 0;
const wanted = new URLSearchParams(location.search).get('room');   // an invite link: ?room=<id>
let wantedShown = false;

async function poll() {
  clearTimeout(pollTimer);
  if (inRoom || document.hidden || panels[0].hidden) return;
  try {
    const response = await fetch('api/rooms', {cache: 'no-store'});
    if (!response.ok) throw new Error(response.status);
    rooms = (await response.json()).rooms || [];
    loaded = true; failedPolls = 0;
  } catch {
    failedPolls++;
  }
  render();
  pollTimer = setTimeout(poll, POLL_MS);
}
document.addEventListener('visibilitychange', () => { if (!document.hidden) poll(); });
search.addEventListener('input', render);

function render() {
  list.setAttribute('aria-busy', String(!loaded));
  const query = search.value.trim().toLowerCase();
  const shown = rooms
    .filter(r => !query || r.name.toLowerCase().includes(query) || r.host.toLowerCase().includes(query))
    .sort((a, b) => (b.open - a.open) || (b.players - a.players) || a.name.localeCompare(b.name));
  const keep = new Set(shown.map(r => r.id));
  for (const [id, li] of rows) if (!keep.has(id)) { li.remove(); rows.delete(id); }
  shown.forEach((room, i) => {
    let li = rows.get(room.id);
    if (!li) { li = makeRow(room.id); rows.set(room.id, li); }
    fillRow(li, room);
    if (list.children[i] !== li) list.insertBefore(li, list.children[i] || null);
  });
  empty.hidden = shown.length > 0;
  const message = failedPolls > 1 ? 'The server is not answering. Trying again…'
    : !loaded ? 'Looking for rooms…'
    : rooms.length && query ? `No room or host matches “${search.value.trim()}”.`
    : 'No rooms yet. Create one and share its name.';
  // Only when it changes: the list repaints every few seconds, and the button may have the focus.
  if (empty.dataset.message !== message) {
    empty.dataset.message = message;
    empty.textContent = message;
    if (loaded && !rooms.length) {
      const create = document.createElement('button');
      create.className = 'link'; create.type = 'button'; create.textContent = 'Create a room';
      create.addEventListener('click', () => showView(1, true));
      empty.append(' ', create);
    }
  }
  if (wanted && !wantedShown && rows.has(wanted)) {
    wantedShown = true;
    const li = rows.get(wanted);
    li.classList.add('selected');
    li.scrollIntoView({block: 'center'});
    const room = rooms.find(r => r.id === wanted);
    if (room?.locked && room.open) openPassword(li); else li.querySelector('.join').focus();
  } else if (wanted && !wantedShown && loaded) {
    wantedShown = true;
    say('The room in your invite link has closed. Pick another one or create your own.');
  }
}

function makeRow(id) {
  const li = rowTemplate.content.firstElementChild.cloneNode(true);
  li.dataset.id = id;
  const form = li.querySelector('.room-password'), input = form.querySelector('input'), error = form.querySelector('.error');
  li.querySelector('.join').addEventListener('click', () => {
    unlockMedia(media);   // this click is the user gesture that lets the game play sound
    const room = rooms.find(r => r.id === id);
    if (!room) return;
    if (room.locked) openPassword(li);
    else enterRoom({id, name: room.name, locked: false});
  });
  form.querySelector('.cancel').addEventListener('click', () => { form.hidden = true; li.querySelector('.join').hidden = false; li.querySelector('.join').focus(); });
  input.addEventListener('keydown', e => { if (e.key === 'Escape') form.querySelector('.cancel').click(); });
  form.addEventListener('submit', async e => {
    e.preventDefault();
    unlockMedia(media);
    const room = rooms.find(r => r.id === id);
    const password = input.value;
    if (!room) return;
    if (!password) { error.textContent = 'Enter the password.'; input.focus(); return; }
    const submit = form.querySelector('.primary');
    submit.disabled = true; error.textContent = '';
    let result;
    try {
      const response = await fetch('api/rooms/check', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({id, password})});
      result = (await response.json()).result;
    } catch {
      result = 'offline';
    }
    submit.disabled = false;
    if (result === 'ok') {
      // The row is closed again for when the player comes back to the list.
      input.value = ''; form.hidden = true; li.querySelector('.join').hidden = false;
      enterRoom({id, name: room.name, password, locked: true});
      return;
    }
    error.textContent = {
      'wrong-password': 'That password is not right. Check it with the host.',
      'closed': 'This room is full, or its match has started.',
      'gone': 'This room has closed.',
      'slow-down': 'Too many tries. Wait a few seconds, then try again.',
    }[result] || 'The server is not answering. Try again in a moment.';
    input.select();
  });
  return li;
}

function openPassword(li) {
  const form = li.querySelector('.room-password');
  li.classList.add('opening');
  form.hidden = false;
  li.querySelector('.join').hidden = true;
  form.querySelector('input').focus();
}

function fillRow(li, room) {
  li.classList.toggle('locked', room.locked);
  li.querySelector('.room-name span').textContent = room.name;
  li.querySelector('.lock').setAttribute('aria-label', 'Needs a password');
  li.querySelector('.room-host').textContent = room.host ? `Hosted by ${room.host}` : 'Host is arriving';
  li.querySelector('.room-players').textContent = `${room.players} of ${room.maxPlayers}`;
  li.querySelector('.room-players').setAttribute('aria-label', `${room.players} of ${room.maxPlayers} players`);
  const join = li.querySelector('.join'), state = li.querySelector('.room-state'), form = li.querySelector('.room-password');
  const full = room.players >= room.maxPlayers;
  state.textContent = room.open ? '' : full ? 'Full' : 'In game';
  join.setAttribute('aria-label', `Join ${room.name}`);
  if (!room.open) { join.hidden = true; form.hidden = true; }
  else if (form.hidden) join.hidden = false;
}

// ---- the game
// One game host at a time. It starts loading as soon as the lobby opens, so the room starts quickly; after a
// room, the next host loads again (the browser revalidates the cached files).
const media = new MediaHost($('stage'), () => {});
const lobby = $('lobby'), game = $('game'), cover = $('cover'), coverTitle = $('cover-title'), coverStatus = $('cover-status');
const dialogs = {
  box: {box: $('box'), title: $('box-title'), text: $('box-text')},
  help: {dialog: $('help'), frame: $('help-frame'), title: $('help-title'), close: $('help-close')},
};
let host = null, inRoom = null, loadError = '';

function newHost() {
  loadError = '';
  host = createGameHost({
    stage: $('stage'), media, dialogs,
    // The stage's area: the window below the bar, less the stage's padding (lobby.css).
    space: () => ({width: innerWidth - 32, height: innerHeight - $('game').querySelector('.game-bar').offsetHeight - 24}),
    commandLine: 'NODXCHECK NOSTARTUP',
    onStatus: text => { coverStatus.textContent = text; },
    onLog: (text, error) => { if (error) console.warn(text); },
    onRoom,
    onEnd: data => {
      const reason = data.kind === 'exit' ? 'The game closed.' : `The game stopped: ${data.reason}`;
      if (inRoom) leaveRoom(reason);
      else { loadError = reason; say(`${reason} Reload the page to try again.`); }
    },
    onFullscreen: on => { $('fullscreen').textContent = on ? 'Exit full screen' : 'Full screen'; },
  });
}

function enterRoom(room) {
  if (inRoom) return;
  if (loadError) { say(`${loadError} Reload the page to try again.`); return; }
  inRoom = {...room, setup: false};
  clearTimeout(pollTimer);
  say('');
  lobby.hidden = true; game.hidden = false;
  document.title = `${room.name} - Rise of Rome`;
  $('game-room').textContent = room.name;
  $('game-lock').hidden = !room.locked;
  $('invite').hidden = true;
  coverTitle.textContent = room.create ? `Creating ${room.name}` : `Joining ${room.name}`;
  cover.hidden = false;
  host.fit();
  host.go(room.create ? {create: true, player, room: room.name, password: room.password}
                      : {player, id: room.id, password: room.password || ''});
}

async function onRoom(event, code) {
  if (!inRoom) return;
  if (event === 'setup') {
    inRoom.setup = true;
    cover.hidden = true;
    host.screen.focus();
    // The host's room gets its id once the relay lists it: the invite link needs it.
    if (!inRoom.id) {
      try {
        const listed = (await (await fetch('api/rooms', {cache: 'no-store'})).json()).rooms;
        inRoom.id = listed.filter(r => r.name === inRoom.name && r.host === player).pop()?.id;
      } catch {}
    }
    $('invite').hidden = !inRoom?.id;
  } else if (event === 'failed') {
    const reason = ROOM_ERRORS[code];
    leaveRoom({
      'wrong-password': `The password for ${inRoom.name} has changed. Ask the host for it.`,
      'closed': `${inRoom.name} is full, or its match has started.`,
      'gone': `${inRoom.name} has closed.`,
    }[reason] || (inRoom.create ? `${inRoom.name} could not be created. Try again.` : `Could not join ${inRoom.name}. Try again.`));
  } else if (event === 'left') {
    leaveRoom(`You left ${inRoom.name}.`);
  }
}

// Back to the lobby: the game ends (its connection closes, so the room lets the player go) and a new one
// starts loading for the next room.
function leaveRoom(message) {
  if (!inRoom) return;
  inRoom = null;
  host.destroy();
  newHost();
  $('leave-confirm').hidden = true; $('leave').hidden = false;
  game.hidden = true; lobby.hidden = false;
  document.title = 'Rise of Rome rooms';
  say(message);
  if (panels[0].hidden) showView(0); else poll();
}

$('fullscreen').addEventListener('click', () => host.toggleFullscreen());
$('leave').addEventListener('click', () => {
  if (!inRoom?.setup) { leaveRoom(`You left ${inRoom.name}.`); return; }
  $('leave').hidden = true; $('leave-confirm').hidden = false; $('leave-no').focus();
});
$('leave-no').addEventListener('click', () => { $('leave-confirm').hidden = true; $('leave').hidden = false; host.screen.focus(); });
$('leave-yes').addEventListener('click', () => leaveRoom(`You left ${inRoom.name}.`));
$('invite').addEventListener('click', async e => {
  const link = new URL(`?room=${inRoom.id}`, location.href).href;
  try { await navigator.clipboard.writeText(link); e.target.textContent = 'Link copied'; }
  catch { e.target.textContent = 'Copy failed'; e.target.title = link; }
  setTimeout(() => { e.target.textContent = 'Copy invite link'; }, 2500);
});

newHost();
showView(0);
// Diagnostics from the console, as on native.html.
window.aoeDebug = () => host?.debug();
window.aoeMedia = media;
