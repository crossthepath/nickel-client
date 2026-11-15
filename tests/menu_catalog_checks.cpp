// Settings the menu adds to the module catalogue (native/menu_catalog.hpp). InitializeMenu runs again each time the
// renderer is recreated, so applying the additions twice must leave the catalogue as after the first time.
#include <cstdio>
#include <string>
#include "menu_catalog.hpp"

namespace {
int failures=0;
void Check(bool ok,const char* what){if(!ok){++failures;std::printf("FAILED: %s\n",what);}}
int Count(const overlay::Module& m,const char* id){int n=0;for(auto& s:m.settings)if(s.id==id)++n;return n;}
const overlay::Module* Find(const std::vector<overlay::Module>& list,const char* id){for(auto& m:list)if(m.id==id)return &m;return nullptr;}
overlay::Setting* Get(std::vector<overlay::Module>& list,const char* module,const char* id){
 for(auto& m:list)if(m.id==module)for(auto& s:m.settings)if(s.id==id)return &s;return nullptr;}
}  // namespace

int main(){
 std::vector<overlay::Module> list=overlay::modules;   // the real catalogue
 const auto* keys=Find(list,"keystrokes");const auto* esp=Find(list,"block_esp");
 Check(keys&&esp,"the catalogue has Keystrokes and Block ESP");
 if(!keys||!esp)return 1;
 const size_t keysBefore=keys->settings.size(),espBefore=esp->settings.size();
 overlay::AddEditorSettings(list);
 Check(Count(*Find(list,"keystrokes"),"custom_keys")==1&&Count(*Find(list,"block_esp"),"block_styles")==1,"the first run adds each hidden setting once");
 const auto* added=Get(list,"keystrokes","custom_keys");
 Check(added&&added->kind=="hidden"&&added->text.empty()&&added->options.empty()&&added->dependsOn==-1,"the added setting is a hidden, empty text setting");
 // the user's values are read into the settings, then the renderer is recreated and InitializeMenu runs again
 Get(list,"keystrokes","custom_keys")->text="87,65,83,68,32,1,2";Get(list,"block_esp","block_styles")->text="diamond_ore,g,#00ffff,#0000ff";
 overlay::AddEditorSettings(list);
 Check(Count(*Find(list,"keystrokes"),"custom_keys")==1,"a second run does not add Custom Keys again");
 Check(Count(*Find(list,"block_esp"),"block_styles")==1,"a second run does not add Block Styles again");
 Check(Find(list,"keystrokes")->settings.size()==keysBefore+1&&Find(list,"block_esp")->settings.size()==espBefore+1,"the setting lists keep their size");
 Check(Get(list,"keystrokes","custom_keys")->text=="87,65,83,68,32,1,2"&&Get(list,"block_esp","block_styles")->text=="diamond_ore,g,#00ffff,#0000ff",
       "the stored values are kept");
 if(failures)return 1;
 std::printf("menu catalogue checks passed\n");return 0;}
