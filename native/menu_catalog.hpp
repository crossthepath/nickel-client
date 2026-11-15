#pragma once
// Settings that the menu adds to the module catalogue (no game access: tests/menu_catalog_checks.cpp).
#include "generated_catalog.hpp"

namespace overlay {

// Adds the hidden text setting `id` to `item`: a copy of its first setting with every field cleared. Nothing is added
// when the module already has it: InitializeMenu runs again each time the renderer is recreated, and a second copy
// (empty) would sit next to the one holding the user's value.
inline void AddHiddenSetting(Module& item,const char* id,const char* name){
 if(item.settings.empty())return;
 for(const auto& existing:item.settings)if(existing.id==id)return;
 Setting st=item.settings.front();st.id=id;st.name=name;st.description="";st.kind="hidden";st.text="";st.options.clear();st.dependsOn=-1;st.unit="";
 item.settings.push_back(st);}

// Hidden settings written by the HUD layout editor and the Block ESP list:
// - Keystrokes "custom_keys": "forward,left,back,right,jump,attack,use" (0 = the game's binding);
// - Block ESP "block_styles": per-block styles, "name,mode,#colour1,#colour2" entries separated by ";" (mode g / r / s).
inline void AddEditorSettings(std::vector<Module>& list){
 for(auto& item:list)if(item.id=="keystrokes")AddHiddenSetting(item,"custom_keys","Custom Keys");
 for(auto& item:list)if(item.id=="block_esp")AddHiddenSetting(item,"block_styles","Block Styles");}

}  // namespace overlay
