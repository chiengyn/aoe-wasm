// Headless probe host. No renderer/audio/device availability is claimed here.
#include "../platform/port_prefix.h"
#include <map>
#include <string>
#include <vector>
struct Value{DWORD type;std::vector<BYTE>bytes;};
struct Key{std::map<std::string,Value>values;};
static std::map<std::string,Key> registry;
extern "C" {
HCURSOR LoadCursorA(HINSTANCE,LPCSTR name){return (HCURSOR)name;}
LONG RegCreateKeyExA(HKEY root,LPCSTR name,DWORD,LPSTR,DWORD,REGSAM,LPSECURITY_ATTRIBUTES,PHKEY out,LPDWORD disposition){char buf[32];sprintf(buf,"%lu/",(unsigned long)root);std::string key=std::string(buf)+name;bool exists=registry.count(key);*out=&registry[key];if(disposition)*disposition=exists?2:1;return 0;}
LONG RegCloseKey(HKEY){return 0;}
LONG RegSetValueExA(HKEY h,LPCSTR name,DWORD,DWORD type,const BYTE*data,DWORD size){if(!h||!name)return 87;Value&v=((Key*)h)->values[name];v.type=type;v.bytes.assign(data,data+size);return 0;}
LONG RegQueryValueExA(HKEY h,LPCSTR name,LPDWORD,LPDWORD type,LPBYTE data,LPDWORD size){if(!h||!name||!size)return 87;Key*k=(Key*)h;std::map<std::string,Value>::iterator it=k->values.find(name);if(it==k->values.end())return 2;Value&v=it->second;if(type)*type=v.type;DWORD required=v.bytes.size();if(data&&*size<required){*size=required;return 234;}if(data&&required)memcpy(data,&v.bytes[0],required);*size=required;return 0;}
}

extern "C" {char*_pgmptr=(char*)"/core-probe.wasm";COLORREF GetSysColor(int index){return index==5?0xffffff:0;}}

#include <emscripten.h>
EM_JS(int,probe_load_string,(unsigned int id, char* out, int capacity),{
    // Win32 string resource IDs wrap to 16 bits. In particular, the engine
    // requests AI filenames at 0x26930, whose resource ID is 0x6930.
    if(capacity<=0)return 0;
    const value=Module['gameStrings']?.[id & 65535] ?? '';
    const special='€\u0081‚ƒ„…†‡ˆ‰Š‹Œ\u008dŽ\u008f\u0090‘’“”•–—˜™š›œ\u009džŸ';
    let length=0;
    for(const character of value){
        if(length>=capacity-1)break;
        const code=character.codePointAt(0),extended=special.indexOf(character);
        HEAPU8[out+length++]=code<128||(code>=160&&code<=255)?code:extended>=0?extended+128:63;
    }
    HEAPU8[out+length]=0;
    return length;
});
extern "C" int LoadStringA(HINSTANCE,UINT id,LPSTR out,int capacity){return probe_load_string(id,out,capacity);}
// PostMessageA and the message queue live in platform/win32_loop.cpp.
