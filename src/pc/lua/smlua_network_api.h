#ifndef SMLUA_NETWORK_API_H
#define SMLUA_NETWORK_API_H

#include <lua.h>

void smlua_network_api_bind(lua_State* L);
void smlua_network_api_settings_init(void);
void smlua_network_api_update(lua_State* L);
void smlua_network_api_discard_mod_requests(const char* relativePath);
void smlua_network_api_shutdown(lua_State* L);

#endif
