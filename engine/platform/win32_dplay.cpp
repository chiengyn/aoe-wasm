// DirectPlay for the real-WinMain build: one service provider, "Browser", that carries the game's own
// DirectPlay traffic through a WebSocket relay (server/relay). The game's networking code
// (com_hand.cpp: sessions, players, guaranteed messages, sync, host migration) runs unmodified.
//
// Calls that wait for the relay (EnumSessions, Open, CreatePlayer) block like DirectPlay did, by yielding
// to the browser through Asyncify. System messages are laid out exactly as DirectPlay delivered them:
// a DPMSG_* structure followed by its strings and data, with pointers into the caller's buffer.
#include "port_prefix.h"
#include "win32_game.h"
#include "dplay.h"
#include <emscripten.h>
#include <deque>
#include <map>
#include <string>
#include <vector>

#define DP_OK 0
#define E_NOTIMPL_ ((HRESULT) 0x80004001)

// The service provider GUID (guids.cpp).
extern "C" const GUID DPSPGUID_BROWSER;
#define BROWSER_PROVIDER DPSPGUID_BROWSER
// The web lobby's room (room_launch.cpp): its password goes with Open, and Open's result explains a failure.
const char* wasm_room_password();
void wasm_room_open_result(HRESULT result);
static const char* const BROWSER_PROVIDER_NAME = "Browser Connection For DirectPlay";

// ---------------------------------------------------------------- wire format (see server/relay/protocol.go)
enum Op {
    OP_ENUM = 1, OP_CREATE = 2, OP_JOIN = 3, OP_CREATE_PLAYER = 4, OP_DESTROY_PLAYER = 5, OP_SEND = 6, OP_SET_DATA = 7,
    OP_SET_NAME = 8, OP_SET_DESC = 9, OP_ENABLE_NEW = 10, OP_CLOSE = 11,
    OP_ENUM_REPLY = 101, OP_OPEN_REPLY = 102, OP_PLAYER_REPLY = 104, OP_DESC = 110, OP_SYS_CREATE = 120,
    OP_SYS_DESTROY = 121, OP_SYS_SETDATA = 122, OP_SYS_SETNAME = 123, OP_SYS_HOST = 124, OP_SYS_SESSIONLOST = 125,
    OP_RECEIVE = 130
};

struct Writer {
    std::vector<BYTE> b;
    Writer(int op) { b.push_back((BYTE) op); }
    void u32(DWORD v) { for (int i = 0; i < 4; i++) b.push_back((BYTE) (v >> (8 * i))); }
    void bytes(const void* p, size_t n) { b.insert(b.end(), (const BYTE*) p, (const BYTE*) p + n); }
    void str(const char* s) { size_t n = s ? strlen(s) : 0; if (n > 0xffff) n = 0xffff; b.push_back((BYTE) n); b.push_back((BYTE) (n >> 8)); bytes(s, n); }
    void blob(const void* p, size_t n) { u32((DWORD) n); bytes(p, n); }
};

struct Reader {
    const BYTE* p;
    size_t n, at;
    bool bad;
    Reader(const BYTE* data, size_t size) : p(data), n(size), at(0), bad(false) {}
    bool need(size_t k) { if (at + k > n) bad = true; return !bad; }
    DWORD u32() { if (!need(4)) return 0; DWORD v = p[at] | p[at + 1] << 8 | p[at + 2] << 16 | (DWORD) p[at + 3] << 24; at += 4; return v; }
    void bytes(void* out, size_t k) { if (need(k)) { memcpy(out, p + at, k); at += k; } }
    std::string str() { if (!need(2)) return ""; size_t k = p[at] | p[at + 1] << 8; at += 2; if (!need(k)) return ""; std::string s((const char*) p + at, k); at += k; return s; }
    std::vector<BYTE> blob() { DWORD k = u32(); std::vector<BYTE> v; if (need(k)) { v.assign(p + at, p + at + k); at += k; } return v; }
};

struct SessionInfo {
    GUID instance, application;
    DWORD flags, max_players, current_players, user[4];
    std::string name, password;
};

static void write_desc(Writer& w, const SessionInfo& s) {
    w.bytes(&s.instance, 16);
    w.bytes(&s.application, 16);
    w.u32(s.flags); w.u32(s.max_players); w.u32(s.current_players);
    for (int i = 0; i < 4; i++) w.u32(s.user[i]);
    w.str(s.name.c_str());
    w.str(s.password.c_str());
}

static SessionInfo read_desc(Reader& r) {
    SessionInfo s;
    r.bytes(&s.instance, 16);
    r.bytes(&s.application, 16);
    s.flags = r.u32(); s.max_players = r.u32(); s.current_players = r.u32();
    for (int i = 0; i < 4; i++) s.user[i] = r.u32();
    s.name = r.str();
    s.password = r.str();
    return s;
}

static SessionInfo from_desc(const DPSESSIONDESC2* d) {
    SessionInfo s;
    memset(&s.instance, 0, sizeof(GUID));
    s.instance = d->guidInstance;
    s.application = d->guidApplication;
    s.flags = d->dwFlags;
    s.max_players = d->dwMaxPlayers;
    s.current_players = d->dwCurrentPlayers;
    s.user[0] = d->dwUser1; s.user[1] = d->dwUser2; s.user[2] = d->dwUser3; s.user[3] = d->dwUser4;
    s.name = d->lpszSessionNameA ? d->lpszSessionNameA : "";
    s.password = d->lpszPasswordA ? d->lpszPasswordA : "";
    return s;
}

// ---------------------------------------------------------------- shared connection state
struct Player {
    DWORD dpid, flags;
    std::string short_name, long_name;
    std::vector<BYTE> remote_data, local_data;
    bool local;
};

// A queued message: DPMSG_* bytes (or an application payload) plus offsets of pointer fields that must be
// rebased onto the caller's buffer on Receive.
struct Message {
    DWORD from, to;
    std::vector<BYTE> bytes;
    std::vector<size_t> pointer_fields;
};

static struct {
    bool in_session, is_host, connection_lost;
    SessionInfo session;
    std::map<DWORD, Player> players;
    std::deque<Message> inbox;
    std::map<DWORD, std::vector<BYTE> > replies;
    DWORD next_request;
} dp;

EM_JS(void, aoe_dp_send, (const BYTE* data, int size), {
    if (!Module['aoeDirectPlay']) throw new Error('DirectPlay transport (aoeDirectPlay) missing');
    Module['aoeDirectPlay'].send(HEAPU8.slice(data, data + size));
});
EM_JS(int, aoe_dp_available, (), { return Module['aoeDirectPlay'] ? 1 : 0; });

static void send(const Writer& w) { aoe_dp_send(&w.b[0], (int) w.b.size()); }

// Sends a request and yields to the browser until its reply arrives (DirectPlay calls were synchronous).
static bool request(Writer& w, DWORD id, std::vector<BYTE>* reply, int timeout_ms = 10000) {
    dp.connection_lost = false;
    send(w);
    double end = emscripten_get_now() + timeout_ms;
    while (!dp.replies.count(id)) {
        if (dp.connection_lost || emscripten_get_now() > end) return false;
        emscripten_sleep(5);
    }
    *reply = dp.replies[id];
    dp.replies.erase(id);
    return true;
}

static DWORD new_request(Writer& w) { DWORD id = ++dp.next_request; w.u32(id); return id; }

// ---------------------------------------------------------------- system message construction
struct Builder {
    Message m;
    size_t reserve(size_t n) { size_t at = m.bytes.size(); m.bytes.resize(at + n); return at; }
    void set32(size_t at, DWORD v) { memcpy(&m.bytes[at], &v, 4); }
    // Appends data and stores its offset in the pointer field at `field` (0 stays NULL when absent).
    void attach(size_t field, const void* data, size_t n, bool terminate) {
        if (!data && !terminate) { set32(field, 0); return; }
        size_t at = m.bytes.size();
        if (n) m.bytes.insert(m.bytes.end(), (const BYTE*) data, (const BYTE*) data + n);
        if (terminate) m.bytes.push_back(0);
        set32(field, (DWORD) at);
        m.pointer_fields.push_back(field);
    }
    void name(size_t dpname_at, const std::string& s, const std::string& l) {
        set32(dpname_at, sizeof(DPNAME));
        attach(dpname_at + offsetof(DPNAME, lpszShortNameA), s.c_str(), s.size(), true);
        attach(dpname_at + offsetof(DPNAME, lpszLongNameA), l.c_str(), l.size(), true);
    }
};

static void deliver(Builder& b, DWORD to = 0) {
    b.m.from = DPID_SYSMSG;
    b.m.to = to;
    dp.inbox.push_back(b.m);
}

static void sys_create(const Player& p) {
    Builder b;
    b.reserve(sizeof(DPMSG_CREATEPLAYERORGROUP));
    b.set32(offsetof(DPMSG_CREATEPLAYERORGROUP, dwType), DPSYS_CREATEPLAYERORGROUP);
    b.set32(offsetof(DPMSG_CREATEPLAYERORGROUP, dwPlayerType), DPPLAYERTYPE_PLAYER);
    b.set32(offsetof(DPMSG_CREATEPLAYERORGROUP, dpId), p.dpid);
    b.set32(offsetof(DPMSG_CREATEPLAYERORGROUP, dwCurrentPlayers), (DWORD) dp.players.size());
    b.set32(offsetof(DPMSG_CREATEPLAYERORGROUP, dwDataSize), (DWORD) p.remote_data.size());
    b.set32(offsetof(DPMSG_CREATEPLAYERORGROUP, dwFlags), p.flags);
    b.attach(offsetof(DPMSG_CREATEPLAYERORGROUP, lpData), p.remote_data.empty() ? 0 : &p.remote_data[0], p.remote_data.size(), false);
    b.name(offsetof(DPMSG_CREATEPLAYERORGROUP, dpnName), p.short_name, p.long_name);
    deliver(b);
}

static void sys_destroy(const Player& p) {
    Builder b;
    b.reserve(sizeof(DPMSG_DESTROYPLAYERORGROUP));
    b.set32(offsetof(DPMSG_DESTROYPLAYERORGROUP, dwType), DPSYS_DESTROYPLAYERORGROUP);
    b.set32(offsetof(DPMSG_DESTROYPLAYERORGROUP, dwPlayerType), DPPLAYERTYPE_PLAYER);
    b.set32(offsetof(DPMSG_DESTROYPLAYERORGROUP, dpId), p.dpid);
    b.set32(offsetof(DPMSG_DESTROYPLAYERORGROUP, dwLocalDataSize), (DWORD) p.local_data.size());
    b.set32(offsetof(DPMSG_DESTROYPLAYERORGROUP, dwRemoteDataSize), (DWORD) p.remote_data.size());
    b.set32(offsetof(DPMSG_DESTROYPLAYERORGROUP, dwFlags), p.flags);
    b.attach(offsetof(DPMSG_DESTROYPLAYERORGROUP, lpLocalData), p.local_data.empty() ? 0 : &p.local_data[0], p.local_data.size(), false);
    b.attach(offsetof(DPMSG_DESTROYPLAYERORGROUP, lpRemoteData), p.remote_data.empty() ? 0 : &p.remote_data[0], p.remote_data.size(), false);
    b.name(offsetof(DPMSG_DESTROYPLAYERORGROUP, dpnName), p.short_name, p.long_name);
    deliver(b);
}

static void sys_set_data(const Player& p) {
    Builder b;
    b.reserve(sizeof(DPMSG_SETPLAYERORGROUPDATA));
    b.set32(offsetof(DPMSG_SETPLAYERORGROUPDATA, dwType), DPSYS_SETPLAYERORGROUPDATA);
    b.set32(offsetof(DPMSG_SETPLAYERORGROUPDATA, dwPlayerType), DPPLAYERTYPE_PLAYER);
    b.set32(offsetof(DPMSG_SETPLAYERORGROUPDATA, dpId), p.dpid);
    b.set32(offsetof(DPMSG_SETPLAYERORGROUPDATA, dwDataSize), (DWORD) p.remote_data.size());
    b.attach(offsetof(DPMSG_SETPLAYERORGROUPDATA, lpData), p.remote_data.empty() ? 0 : &p.remote_data[0], p.remote_data.size(), false);
    deliver(b);
}

static void sys_set_name(const Player& p) {
    Builder b;
    b.reserve(sizeof(DPMSG_SETPLAYERORGROUPNAME));
    b.set32(offsetof(DPMSG_SETPLAYERORGROUPNAME, dwType), DPSYS_SETPLAYERORGROUPNAME);
    b.set32(offsetof(DPMSG_SETPLAYERORGROUPNAME, dwPlayerType), DPPLAYERTYPE_PLAYER);
    b.set32(offsetof(DPMSG_SETPLAYERORGROUPNAME, dpId), p.dpid);
    b.name(offsetof(DPMSG_SETPLAYERORGROUPNAME, dpnName), p.short_name, p.long_name);
    deliver(b);
}

static void sys_generic(DWORD type) {
    Builder b;
    b.reserve(sizeof(DPMSG_GENERIC));
    b.set32(0, type);
    deliver(b);
}

static void sys_session_desc() {
    Builder b;
    b.reserve(sizeof(DPMSG_SETSESSIONDESC));
    size_t d = offsetof(DPMSG_SETSESSIONDESC, dpDesc);
    b.set32(0, DPSYS_SETSESSIONDESC);
    DPSESSIONDESC2 desc;
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = dp.session.flags;
    desc.guidInstance = dp.session.instance;
    desc.guidApplication = dp.session.application;
    desc.dwMaxPlayers = dp.session.max_players;
    desc.dwCurrentPlayers = dp.session.current_players;
    desc.dwUser1 = dp.session.user[0]; desc.dwUser2 = dp.session.user[1]; desc.dwUser3 = dp.session.user[2]; desc.dwUser4 = dp.session.user[3];
    memcpy(&b.m.bytes[d], &desc, sizeof(desc));
    b.attach(d + offsetof(DPSESSIONDESC2, lpszSessionNameA), dp.session.name.c_str(), dp.session.name.size(), true);
    b.attach(d + offsetof(DPSESSIONDESC2, lpszPasswordA), dp.session.password.c_str(), dp.session.password.size(), true);
    deliver(b);
}

static Player read_player(Reader& r) {
    Player p;
    p.dpid = r.u32();
    p.flags = r.u32();
    p.short_name = r.str();
    p.long_name = r.str();
    p.remote_data = r.blob();
    p.local = false;
    return p;
}

static void leave_session() {
    dp.in_session = false;
    dp.is_host = false;
    dp.players.clear();
    dp.inbox.clear();
}

// ---------------------------------------------------------------- relay events
extern "C" EMSCRIPTEN_KEEPALIVE void aoe_dp_incoming(const BYTE* data, int size) {
    if (size < 1) return;
    Reader r(data + 1, size - 1);
    switch (data[0]) {
        case OP_ENUM_REPLY: case OP_OPEN_REPLY: case OP_PLAYER_REPLY: {
            DWORD id = r.u32();
            dp.replies[id] = std::vector<BYTE>(data, data + size);
            return;
        }
        case OP_DESC: {
            dp.session = read_desc(r);
            BYTE notify = 0;
            r.bytes(&notify, 1);
            if (notify && dp.in_session) sys_session_desc();
            return;
        }
        case OP_SYS_CREATE: {
            Player p = read_player(r);
            if (r.bad || !dp.in_session) return;
            dp.players[p.dpid] = p;
            sys_create(p);
            return;
        }
        case OP_SYS_DESTROY: {
            DWORD id = r.u32();
            std::map<DWORD, Player>::iterator it = dp.players.find(id);
            if (it == dp.players.end()) return;
            Player p = it->second;
            dp.players.erase(it);
            sys_destroy(p);
            return;
        }
        case OP_SYS_SETDATA: {
            DWORD id = r.u32();
            std::vector<BYTE> blob = r.blob();
            if (r.bad || !dp.players.count(id)) return;
            dp.players[id].remote_data = blob;
            sys_set_data(dp.players[id]);
            return;
        }
        case OP_SYS_SETNAME: {
            DWORD id = r.u32();
            std::string s = r.str(), l = r.str();
            if (r.bad || !dp.players.count(id)) return;
            dp.players[id].short_name = s;
            dp.players[id].long_name = l;
            sys_set_name(dp.players[id]);
            return;
        }
        case OP_SYS_HOST:
            dp.is_host = true;
            sys_generic(DPSYS_HOST);
            return;
        case OP_SYS_SESSIONLOST:
            if (dp.in_session) sys_generic(DPSYS_SESSIONLOST);
            return;
        case OP_RECEIVE: {
            Message m;
            m.from = r.u32();
            m.to = r.u32();
            m.bytes = r.blob();
            if (!r.bad && dp.in_session) dp.inbox.push_back(m);
            return;
        }
    }
}

// The WebSocket closed: pending calls fail and an open session is lost, as when DirectPlay lost its link.
extern "C" EMSCRIPTEN_KEEPALIVE void aoe_dp_disconnected(void) {
    dp.connection_lost = true;
    if (dp.in_session) sys_generic(DPSYS_SESSIONLOST);
}

// ---------------------------------------------------------------- DirectPlay interfaces
// dplay.h declares these IDirectPlay3 members without "= 0"; defining them here emits the interface's
// vtable and type information. Every implementation below overrides them.
HRESULT IDirectPlay3::SendChatMessage(DPID, DPID, DWORD, LPDPCHAT) { return E_NOTIMPL_; }
HRESULT IDirectPlay3::StartSession(DWORD, DPID) { return E_NOTIMPL_; }
HRESULT IDirectPlay3::GetGroupFlags(DPID, LPDWORD) { return E_NOTIMPL_; }
HRESULT IDirectPlay3::GetGroupParent(DPID, LPDPID) { return E_NOTIMPL_; }
HRESULT IDirectPlay3::GetPlayerFlags(DPID, LPDWORD) { return E_NOTIMPL_; }

struct DpObject;

struct DpV3 : IDirectPlay3 {
    DpObject* owner;
    HRESULT QueryInterface(REFIID riid, LPVOID* out);
    ULONG AddRef();
    ULONG Release();
    HRESULT AddPlayerToGroup(DPID, DPID) { return E_NOTIMPL_; }
    HRESULT Close();
    HRESULT CreateGroup(LPDPID, LPDPNAME, LPVOID, DWORD, DWORD) { return E_NOTIMPL_; }
    HRESULT CreatePlayer(LPDPID id, LPDPNAME name, HANDLE, LPVOID data, DWORD size, DWORD flags);
    HRESULT DeletePlayerFromGroup(DPID, DPID) { return E_NOTIMPL_; }
    HRESULT DestroyGroup(DPID) { return E_NOTIMPL_; }
    HRESULT DestroyPlayer(DPID id);
    HRESULT EnumGroupPlayers(DPID, LPGUID, LPDPENUMPLAYERSCALLBACK2, LPVOID, DWORD) { return E_NOTIMPL_; }
    HRESULT EnumGroups(LPGUID, LPDPENUMPLAYERSCALLBACK2, LPVOID, DWORD) { return DP_OK; }
    HRESULT EnumPlayers(LPGUID, LPDPENUMPLAYERSCALLBACK2 callback, LPVOID context, DWORD flags);
    HRESULT EnumSessions(LPDPSESSIONDESC2 desc, DWORD timeout, LPDPENUMSESSIONSCALLBACK2 callback, LPVOID context, DWORD flags);
    HRESULT GetCaps(LPDPCAPS caps, DWORD flags);
    HRESULT GetGroupData(DPID, LPVOID, LPDWORD, DWORD) { return E_NOTIMPL_; }
    HRESULT GetGroupName(DPID, LPVOID, LPDWORD) { return E_NOTIMPL_; }
    HRESULT GetMessageCount(DPID id, LPDWORD count);
    HRESULT GetPlayerAddress(DPID, LPVOID, LPDWORD) { return E_NOTIMPL_; }
    HRESULT GetPlayerCaps(DPID, LPDPCAPS caps, DWORD flags) { return GetCaps(caps, flags); }
    HRESULT GetPlayerData(DPID id, LPVOID data, LPDWORD size, DWORD flags);
    HRESULT GetPlayerName(DPID id, LPVOID data, LPDWORD size);
    HRESULT GetSessionDesc(LPVOID data, LPDWORD size);
    HRESULT Initialize(LPGUID) { return DPERR_ALREADYINITIALIZED; }
    HRESULT Open(LPDPSESSIONDESC2 desc, DWORD flags);
    HRESULT Receive(LPDPID from, LPDPID to, DWORD flags, LPVOID data, LPDWORD size);
    HRESULT Send(DPID from, DPID to, DWORD flags, LPVOID data, DWORD size);
    HRESULT SetGroupData(DPID, LPVOID, DWORD, DWORD) { return E_NOTIMPL_; }
    HRESULT SetGroupName(DPID, LPDPNAME, DWORD) { return E_NOTIMPL_; }
    HRESULT SetPlayerData(DPID id, LPVOID data, DWORD size, DWORD flags);
    HRESULT SetPlayerName(DPID id, LPDPNAME name, DWORD flags);
    HRESULT SetSessionDesc(LPDPSESSIONDESC2 desc, DWORD flags);
    HRESULT AddGroupToGroup(DPID, DPID) { return E_NOTIMPL_; }
    HRESULT CreateGroupInGroup(DPID, LPDPID, LPDPNAME, LPVOID, DWORD, DWORD) { return E_NOTIMPL_; }
    HRESULT DeleteGroupFromGroup(DPID, DPID) { return E_NOTIMPL_; }
    HRESULT EnumConnections(LPCGUID, LPDPENUMCONNECTIONSCALLBACK, LPVOID, DWORD) { return E_NOTIMPL_; }
    HRESULT EnumGroupsInGroup(DPID, LPGUID, LPDPENUMPLAYERSCALLBACK2, LPVOID, DWORD) { return E_NOTIMPL_; }
    HRESULT GetGroupConnectionSettings(DWORD, DPID, LPVOID, LPDWORD) { return E_NOTIMPL_; }
    HRESULT InitializeConnection(LPVOID, DWORD) { return DPERR_ALREADYINITIALIZED; }
    HRESULT SecureOpen(LPCDPSESSIONDESC2, DWORD, LPCDPSECURITYDESC, LPCDPCREDENTIALS) { return E_NOTIMPL_; }
    HRESULT SendChatMessage(DPID, DPID, DWORD, LPDPCHAT) { return E_NOTIMPL_; }
    HRESULT SetGroupConnectionSettings(DWORD, DPID, LPDPLCONNECTION) { return E_NOTIMPL_; }
    HRESULT StartSession(DWORD, DPID) { return E_NOTIMPL_; }
    HRESULT GetGroupFlags(DPID, LPDWORD) { return E_NOTIMPL_; }
    HRESULT GetGroupParent(DPID, LPDPID) { return E_NOTIMPL_; }
    HRESULT GetPlayerAccount(DPID, DWORD, LPVOID, LPDWORD) { return E_NOTIMPL_; }
    HRESULT GetPlayerFlags(DPID, LPDWORD) { return E_NOTIMPL_; }
};

// IDirectPlay (version 1): DirectPlayCreate returns it; the game queries IDirectPlay3A from it and uses it
// directly only for EnableNewPlayers.
struct DpV1 : IDirectPlay {
    DpObject* owner;
    HRESULT QueryInterface(REFIID riid, LPVOID* out);
    ULONG AddRef();
    ULONG Release();
    HRESULT AddPlayerToGroup(DPID, DPID) { return E_NOTIMPL_; }
    HRESULT Close();
    HRESULT CreatePlayer(LPDPID, LPSTR, LPSTR, LPHANDLE) { return E_NOTIMPL_; }
    HRESULT CreateGroup(LPDPID, LPSTR, LPSTR) { return E_NOTIMPL_; }
    HRESULT DeletePlayerFromGroup(DPID, DPID) { return E_NOTIMPL_; }
    HRESULT DestroyPlayer(DPID id);
    HRESULT DestroyGroup(DPID) { return E_NOTIMPL_; }
    HRESULT EnableNewPlayers(BOOL enable);
    HRESULT EnumGroupPlayers(DPID, LPDPENUMPLAYERSCALLBACK, LPVOID, DWORD) { return E_NOTIMPL_; }
    HRESULT EnumGroups(DWORD, LPDPENUMPLAYERSCALLBACK, LPVOID, DWORD) { return E_NOTIMPL_; }
    HRESULT EnumPlayers(DWORD, LPDPENUMPLAYERSCALLBACK, LPVOID, DWORD) { return E_NOTIMPL_; }
    HRESULT EnumSessions(LPDPSESSIONDESC, DWORD, LPDPENUMSESSIONSCALLBACK, LPVOID, DWORD) { return E_NOTIMPL_; }
    HRESULT GetCaps(LPDPCAPS) { return E_NOTIMPL_; }
    HRESULT GetMessageCount(DPID id, LPDWORD count);
    HRESULT GetPlayerCaps(DPID, LPDPCAPS) { return E_NOTIMPL_; }
    HRESULT GetPlayerName(DPID, LPSTR, LPDWORD, LPSTR, LPDWORD) { return E_NOTIMPL_; }
    HRESULT Initialize(LPGUID) { return DPERR_ALREADYINITIALIZED; }
    HRESULT Open(LPDPSESSIONDESC) { return E_NOTIMPL_; }
    HRESULT Receive(LPDPID from, LPDPID to, DWORD flags, LPVOID data, LPDWORD size);
    HRESULT SaveSession(LPSTR) { return E_NOTIMPL_; }
    HRESULT Send(DPID from, DPID to, DWORD flags, LPVOID data, DWORD size);
    HRESULT SetPlayerName(DPID, LPSTR, LPSTR) { return E_NOTIMPL_; }
};

struct DpObject {
    ULONG refs;
    bool opened; // this object created or joined the current session
    DpV1 v1;
    DpV3 v3;
    DpObject() : refs(1), opened(false) { v1.owner = this; v3.owner = this; }
    HRESULT query(REFIID riid, LPVOID* out) {
        if (!out) return DPERR_INVALIDPARAMS;
        if (IsEqualGUID(riid, IID_IDirectPlay)) *out = &v1;
        else if (IsEqualGUID(riid, IID_IDirectPlay3A) || IsEqualGUID(riid, IID_IDirectPlay3)) *out = &v3;
        else { *out = 0; return E_NOINTERFACE; }
        refs++;
        return DP_OK;
    }
    ULONG release() {
        ULONG r = --refs;
        if (!r) { if (opened) v3.Close(); delete this; }
        return r;
    }
};

HRESULT DpV3::QueryInterface(REFIID riid, LPVOID* out) { return owner->query(riid, out); }
ULONG DpV3::AddRef() { return ++owner->refs; }
ULONG DpV3::Release() { return owner->release(); }
HRESULT DpV1::QueryInterface(REFIID riid, LPVOID* out) { return owner->query(riid, out); }
ULONG DpV1::AddRef() { return ++owner->refs; }
ULONG DpV1::Release() { return owner->release(); }

HRESULT DpV3::Close() {
    if (!dp.in_session) return DP_OK;
    Writer w(OP_CLOSE);
    send(w);
    owner->opened = false;
    leave_session();
    return DP_OK;
}
HRESULT DpV1::Close() { return owner->v3.Close(); }

HRESULT DpV3::EnumSessions(LPDPSESSIONDESC2 desc, DWORD, LPDPENUMSESSIONSCALLBACK2 callback, LPVOID context, DWORD flags) {
    if (!desc || !callback) return DPERR_INVALIDPARAMS;
    Writer w(OP_ENUM);
    DWORD id = new_request(w);
    w.bytes(&desc->guidApplication, 16);
    w.u32(flags);
    std::vector<BYTE> reply;
    if (!request(w, id, &reply)) return DPERR_NOCONNECTION;
    Reader r(&reply[0] + 5, reply.size() - 5);
    DWORD count = r.u32();
    for (DWORD i = 0; i < count && !r.bad; i++) {
        SessionInfo s = read_desc(r);
        if (r.bad) break;
        DPSESSIONDESC2 d;
        memset(&d, 0, sizeof(d));
        d.dwSize = sizeof(d);
        d.dwFlags = s.flags;
        d.guidInstance = s.instance;
        d.guidApplication = s.application;
        d.dwMaxPlayers = s.max_players;
        d.dwCurrentPlayers = s.current_players;
        d.lpszSessionNameA = (LPSTR) s.name.c_str();
        d.lpszPasswordA = s.password.empty() ? 0 : (LPSTR) s.password.c_str();
        d.dwUser1 = s.user[0]; d.dwUser2 = s.user[1]; d.dwUser3 = s.user[2]; d.dwUser4 = s.user[3];
        DWORD timeout = 0;
        if (!callback(&d, &timeout, 0, context)) return DP_OK;
    }
    DWORD timeout = 0;
    callback(0, &timeout, DPESC_TIMEDOUT, context); // end of enumeration
    return DP_OK;
}

HRESULT DpV3::Open(LPDPSESSIONDESC2 desc, DWORD flags) {
    if (!desc) return DPERR_INVALIDPARAMS;
    if (dp.in_session) return DPERR_ALREADYINITIALIZED;
    Writer w((flags & DPOPEN_CREATE) ? OP_CREATE : OP_JOIN);
    DWORD id = new_request(w);
    // A room from the web lobby brings its password; the game's own dialogs never set one.
    const char* password = desc->lpszPasswordA && *desc->lpszPasswordA ? desc->lpszPasswordA : wasm_room_password();
    if (flags & DPOPEN_CREATE) { SessionInfo s = from_desc(desc); s.password = password ? password : ""; write_desc(w, s); }
    else { w.bytes(&desc->guidInstance, 16); w.str(password); }
    std::vector<BYTE> reply;
    if (!request(w, id, &reply)) { wasm_room_open_result(DPERR_NOCONNECTION); return DPERR_NOCONNECTION; }
    Reader r(&reply[0] + 5, reply.size() - 5);
    HRESULT result = (HRESULT) r.u32();
    wasm_room_open_result(result);
    if (result != DP_OK) return result;
    dp.session = read_desc(r);
    dp.players.clear();
    dp.inbox.clear();
    DWORD count = r.u32();
    for (DWORD i = 0; i < count && !r.bad; i++) { Player p = read_player(r); dp.players[p.dpid] = p; }
    dp.in_session = true;
    dp.is_host = (flags & DPOPEN_CREATE) != 0;
    owner->opened = true;
    return DP_OK;
}

HRESULT DpV3::CreatePlayer(LPDPID out, LPDPNAME name, HANDLE, LPVOID data, DWORD size, DWORD flags) {
    if (!out) return DPERR_INVALIDPARAMS;
    if (!dp.in_session) return DPERR_INVALIDPARAMS;
    Player p;
    p.short_name = name && name->lpszShortNameA ? name->lpszShortNameA : "";
    p.long_name = name && name->lpszLongNameA ? name->lpszLongNameA : "";
    if (data && size) p.remote_data.assign((BYTE*) data, (BYTE*) data + size);
    p.flags = flags;
    p.local = true;
    Writer w(OP_CREATE_PLAYER);
    DWORD id = new_request(w);
    w.u32(flags);
    w.str(p.short_name.c_str());
    w.str(p.long_name.c_str());
    w.blob(p.remote_data.empty() ? 0 : &p.remote_data[0], p.remote_data.size());
    std::vector<BYTE> reply;
    if (!request(w, id, &reply)) return DPERR_NOCONNECTION;
    Reader r(&reply[0] + 5, reply.size() - 5);
    HRESULT result = (HRESULT) r.u32();
    if (result != DP_OK) return result;
    p.dpid = r.u32();
    dp.players[p.dpid] = p;
    *out = p.dpid;
    return DP_OK;
}

HRESULT DpV3::DestroyPlayer(DPID id) {
    std::map<DWORD, Player>::iterator it = dp.players.find(id);
    if (it == dp.players.end() || !it->second.local) return DPERR_INVALIDPLAYER;
    Writer w(OP_DESTROY_PLAYER);
    w.u32(id);
    send(w);
    dp.players.erase(it);
    return DP_OK;
}
HRESULT DpV1::DestroyPlayer(DPID id) { return owner->v3.DestroyPlayer(id); }

HRESULT DpV3::EnumPlayers(LPGUID, LPDPENUMPLAYERSCALLBACK2 callback, LPVOID context, DWORD) {
    if (!callback) return DPERR_INVALIDPARAMS;
    for (std::map<DWORD, Player>::iterator it = dp.players.begin(); it != dp.players.end(); ++it) {
        DPNAME n;
        memset(&n, 0, sizeof(n));
        n.dwSize = sizeof(n);
        n.lpszShortNameA = (LPSTR) it->second.short_name.c_str();
        n.lpszLongNameA = (LPSTR) it->second.long_name.c_str();
        if (!callback(it->first, DPPLAYERTYPE_PLAYER, &n, it->second.flags, context)) break;
    }
    return DP_OK;
}

HRESULT DpV3::GetCaps(LPDPCAPS caps, DWORD) {
    if (!caps) return DPERR_INVALIDPARAMS;
    DWORD size = caps->dwSize ? caps->dwSize : sizeof(DPCAPS);
    memset(caps, 0, size < sizeof(DPCAPS) ? size : sizeof(DPCAPS));
    caps->dwSize = size;
    // WebSocket delivery is reliable and ordered.
    caps->dwFlags = DPCAPS_GUARANTEEDOPTIMIZED | DPCAPS_GUARANTEEDSUPPORTED | (dp.is_host ? DPCAPS_ISHOST : 0);
    caps->dwMaxBufferSize = 65000;
    caps->dwMaxPlayers = 9;
    caps->dwHundredBaud = 100000;
    caps->dwLatency = 50;
    caps->dwMaxLocalPlayers = 1;
    caps->dwHeaderLength = 13;
    caps->dwTimeout = 5000;
    return DP_OK;
}

HRESULT DpV3::GetMessageCount(DPID id, LPDWORD count) {
    if (!count) return DPERR_INVALIDPARAMS;
    DWORD n = 0;
    for (size_t i = 0; i < dp.inbox.size(); i++) if (!id || dp.inbox[i].to == id || dp.inbox[i].from == DPID_SYSMSG) n++;
    *count = n;
    return DP_OK;
}
HRESULT DpV1::GetMessageCount(DPID id, LPDWORD count) { return owner->v3.GetMessageCount(id, count); }

HRESULT DpV3::Receive(LPDPID from, LPDPID to, DWORD flags, LPVOID data, LPDWORD size) {
    if (!size) return DPERR_INVALIDPARAMS;
    if (dp.inbox.empty()) return DPERR_NOMESSAGES;
    Message& m = dp.inbox.front();
    if (*size < m.bytes.size() || !data) { *size = (DWORD) m.bytes.size(); return DPERR_BUFFERTOOSMALL; }
    if (!m.bytes.empty()) memcpy(data, &m.bytes[0], m.bytes.size());
    for (size_t i = 0; i < m.pointer_fields.size(); i++) {
        DWORD offset;
        memcpy(&offset, (BYTE*) data + m.pointer_fields[i], 4);
        DWORD pointer = (DWORD) (uintptr_t) ((BYTE*) data + offset);
        memcpy((BYTE*) data + m.pointer_fields[i], &pointer, 4);
    }
    *size = (DWORD) m.bytes.size();
    if (from) *from = m.from;
    if (to) *to = m.to;
    if (!(flags & DPRECEIVE_PEEK)) dp.inbox.pop_front();
    return DP_OK;
}
HRESULT DpV1::Receive(LPDPID from, LPDPID to, DWORD flags, LPVOID data, LPDWORD size) { return owner->v3.Receive(from, to, flags, data, size); }

HRESULT DpV3::Send(DPID from, DPID to, DWORD flags, LPVOID data, DWORD size) {
    if (!dp.in_session) return DPERR_NOCONNECTION;
    if (!dp.players.count(from) || !dp.players[from].local) return DPERR_INVALIDPLAYER;
    if (to != DPID_ALLPLAYERS && !dp.players.count(to)) return DPERR_INVALIDPLAYER;
    Writer w(OP_SEND);
    w.u32(from);
    w.u32(to);
    w.u32(flags);
    w.blob(data, size);
    send(w);
    return DP_OK;
}
HRESULT DpV1::Send(DPID from, DPID to, DWORD flags, LPVOID data, DWORD size) { return owner->v3.Send(from, to, flags, data, size); }

HRESULT DpV3::GetPlayerData(DPID id, LPVOID data, LPDWORD size, DWORD flags) {
    if (!size || !dp.players.count(id)) return DPERR_INVALIDPLAYER;
    std::vector<BYTE>& v = (flags & DPGET_LOCAL) ? dp.players[id].local_data : dp.players[id].remote_data;
    if (!data || *size < v.size()) { *size = (DWORD) v.size(); return DPERR_BUFFERTOOSMALL; }
    if (!v.empty()) memcpy(data, &v[0], v.size());
    *size = (DWORD) v.size();
    return DP_OK;
}

HRESULT DpV3::SetPlayerData(DPID id, LPVOID data, DWORD size, DWORD flags) {
    if (!dp.players.count(id)) return DPERR_INVALIDPLAYER;
    Player& p = dp.players[id];
    std::vector<BYTE> v;
    if (data && size) v.assign((BYTE*) data, (BYTE*) data + size);
    if (flags & DPSET_LOCAL) { p.local_data = v; return DP_OK; }
    if (!p.local) return DPERR_ACCESSDENIED;
    p.remote_data = v;
    // Remote data reaches the other players as DPSYS_SETPLAYERORGROUPDATA (the host's lobby options).
    Writer w(OP_SET_DATA);
    w.u32(id);
    w.u32(flags);
    w.blob(v.empty() ? 0 : &v[0], v.size());
    send(w);
    return DP_OK;
}

HRESULT DpV3::GetPlayerName(DPID id, LPVOID data, LPDWORD size) {
    if (!size || !dp.players.count(id)) return DPERR_INVALIDPLAYER;
    Player& p = dp.players[id];
    DWORD need = sizeof(DPNAME) + p.short_name.size() + 1 + p.long_name.size() + 1;
    if (!data || *size < need) { *size = need; return DPERR_BUFFERTOOSMALL; }
    DPNAME* n = (DPNAME*) data;
    memset(n, 0, sizeof(*n));
    n->dwSize = sizeof(DPNAME);
    char* s = (char*) data + sizeof(DPNAME);
    strcpy(s, p.short_name.c_str());
    char* l = s + p.short_name.size() + 1;
    strcpy(l, p.long_name.c_str());
    n->lpszShortNameA = s;
    n->lpszLongNameA = l;
    *size = need;
    return DP_OK;
}

HRESULT DpV3::SetPlayerName(DPID id, LPDPNAME name, DWORD) {
    if (!dp.players.count(id) || !dp.players[id].local) return DPERR_INVALIDPLAYER;
    Player& p = dp.players[id];
    p.short_name = name && name->lpszShortNameA ? name->lpszShortNameA : "";
    p.long_name = name && name->lpszLongNameA ? name->lpszLongNameA : "";
    Writer w(OP_SET_NAME);
    w.u32(id);
    w.str(p.short_name.c_str());
    w.str(p.long_name.c_str());
    send(w);
    return DP_OK;
}

HRESULT DpV3::GetSessionDesc(LPVOID data, LPDWORD size) {
    if (!size) return DPERR_INVALIDPARAMS;
    if (!dp.in_session) return DPERR_NOSESSIONS;
    DWORD need = sizeof(DPSESSIONDESC2) + dp.session.name.size() + 1 + dp.session.password.size() + 1;
    if (!data || *size < need) { *size = need; return DPERR_BUFFERTOOSMALL; }
    DPSESSIONDESC2* d = (DPSESSIONDESC2*) data;
    memset(d, 0, sizeof(*d));
    d->dwSize = sizeof(*d);
    d->dwFlags = dp.session.flags;
    d->guidInstance = dp.session.instance;
    d->guidApplication = dp.session.application;
    d->dwMaxPlayers = dp.session.max_players;
    d->dwCurrentPlayers = (DWORD) dp.players.size();
    d->dwUser1 = dp.session.user[0]; d->dwUser2 = dp.session.user[1]; d->dwUser3 = dp.session.user[2]; d->dwUser4 = dp.session.user[3];
    char* name = (char*) data + sizeof(*d);
    strcpy(name, dp.session.name.c_str());
    char* password = name + dp.session.name.size() + 1;
    strcpy(password, dp.session.password.c_str());
    d->lpszSessionNameA = name;
    d->lpszPasswordA = password;
    *size = need;
    return DP_OK;
}

HRESULT DpV3::SetSessionDesc(LPDPSESSIONDESC2 desc, DWORD) {
    if (!desc) return DPERR_INVALIDPARAMS;
    if (!dp.in_session || !dp.is_host) return DPERR_ACCESSDENIED;
    SessionInfo s = from_desc(desc);
    s.instance = dp.session.instance;
    dp.session = s;
    Writer w(OP_SET_DESC);
    write_desc(w, s);
    send(w);
    return DP_OK;
}

HRESULT DpV1::EnableNewPlayers(BOOL enable) {
    if (!dp.in_session) return DPERR_NOSESSIONS;
    if (enable) dp.session.flags &= ~DPSESSION_NEWPLAYERSDISABLED;
    else dp.session.flags |= DPSESSION_NEWPLAYERSDISABLED;
    Writer w(OP_ENABLE_NEW);
    w.u32(enable ? 1 : 0);
    send(w);
    return DP_OK;
}

// ---------------------------------------------------------------- entry points
extern "C" {

// The lobby (DirectPlayLobby) launched games from Microsoft Gaming Zone; there is no lobby here.
HRESULT DirectPlayLobbyCreateA(LPGUID, void** out, IUnknown*, LPVOID, DWORD) { if (out) *out = 0; return DPERR_UNAVAILABLE; }

HRESULT DirectPlayCreate(LPGUID provider, LPDIRECTPLAY* out, IUnknown*) {
    if (!out) return DPERR_INVALIDPARAMS;
    *out = 0;
    if (!provider || !IsEqualGUID(*provider, BROWSER_PROVIDER) || !aoe_dp_available()) return DPERR_UNAVAILABLE;
    DpObject* o = new DpObject;
    *out = &o->v1;
    return DP_OK;
}

HRESULT DirectPlayEnumerateA(LPDPENUMDPCALLBACKA callback, LPVOID context) {
    if (!callback) return DPERR_INVALIDPARAMS;
    if (aoe_dp_available()) callback((LPGUID) &BROWSER_PROVIDER, (LPSTR) BROWSER_PROVIDER_NAME, 6, 0, context);
    return DP_OK;
}

}
