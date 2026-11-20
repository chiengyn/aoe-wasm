// Port glue linked into every build (probe and real WinMain).
#include "port_prefix.h"
#include "../upstream/include/pnl_lst.h"

// The recovered header declares the const overload but omits its body. MSVC 5 bound string
// literals to the char* overload; Clang selects this one. insert_line copies the source string,
// so forwarding cannot mutate the literal.
int TTextPanel::append_line(const char* text, long id) { return append_line((char*) text, id); }

// com_hand.cpp IsHost() override used by the probe lobby bridge; -1 keeps the original behavior.
int wasm_lobby_role = -1;
