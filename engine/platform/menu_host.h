#ifndef AOE_WASM_MENU_HOST_H
#define AOE_WASM_MENU_HOST_H
class TShape;
class TDrawArea;
void* wasm_menu_dc();
extern "C" void wasm_menu_begin_frame();
void wasm_menu_present(TDrawArea*, void*);
#endif
